# DAG Connector 与调度行为手册

本文基于 commit：

- `6b5dd5b2ab310e4be5bec5227694983040ce05ac`

目标：

- 说明 scheduler 遇到 DAG 中不同 connector 类型时如何处理
- 重点说明 `kafka_protocol`
- 说明 SQL option / DAG option 中哪些字段会影响调度
- 说明环境变量如何改变 DAG 拉取、过滤和调度行为
- 给出排查入口

## 1. 总体数据流

```text
  +----------------+
  | Job Manager    |
  | GetExecutorDAG |
  +-------+--------+
          |
          | DAG:
          | job -> task_group -> subtask
          | task_slot -> props
          v
  +----------------+
  | DAGReader      |
  | dag/mod.rs     |
  +-------+--------+
          |
          | filter by:
          | operator_unique_name
          | connector.topic
          | connector.resgroup
          v
  +----------------+
  | DAG Filter     |
  | dag/filter.rs  |
  +-------+--------+
          |
          | DAG events:
          | add/remove/update job/task
          v
  +----------------+
  | SchedgroupMgr  |
  | schedgroup/    |
  +-------+--------+
          |
          | topic -> job/group
          | listen port
          | sched policy
          v
  +----------------+
  | SchedGroup     |
  | policy         |
  +-------+--------+
          |
          | getaddr/topicstats
          v
  +----------------+
  | HTTP response  |
  +----------------+
```

## 2. 重要结论

当前代码中，scheduler **不直接读取** SQL option 里的：

- `connector.type`
- `connector.mode`

它实际使用 Job Manager 返回的：

- `SubTask.operator_unique_name`

来判断 connector 类型。

默认允许拉取的 operator：

- `source.sharedhttpd`
- `source.kafka_protocol`
- `source.kafka_protocol_binary`

代码入口：

- `scheduler/src/config/mod.rs`
  - `DAGFilter::default_allow_operator_names()`
- `scheduler/src/dag/filter.rs`
  - `Filter::filter_job()`
- `scheduler/src/schedgroup/mod.rs`
  - `DAGEvent::on_add_job()`

## 3. Connector 类型处理

### 3.1 分流规则

```text
  SubTask.operator_unique_name
             |
             v
  +-------------------------+
  | source.sharedhttpd      |
  +-----------+-------------+
              |
              v
       find_sharedhttpd()
       topic: httpd.topic
       port : httpd.hostport

  +-------------------------+
  | source.kafka_protocol   |
  | source.kafka_protocol_  |
  | binary                  |
  +-----------+-------------+
              |
              v
       find_kafkaprotocol()
       topic: connector.topic
       port : connector.port

  +-------------------------+
  | other operator name     |
  +-----------+-------------+
              |
              v
       ignored by scheduler
```

### 3.2 `source.sharedhttpd`

调度器读取：

- `httpd.topic`
- `httpd.hostport`

行为：

- `httpd.topic` 缺失时使用 `__default__`
- `httpd.hostport` 作为端口候选
- 如果 task props 中存在 `source.listen.port`，最终端口优先使用 task props

代码入口：

- `scheduler/src/schedgroup/mod.rs`
  - `DAGEvent::find_sharedhttpd()`
- `scheduler/src/dag/filter.rs`
  - `Filter::sharedhttpd_filter()`
- `scheduler/src/dag/mod.rs`
  - `Task::calc_access_port()`

### 3.3 `source.kafka_protocol`

调度器读取：

- `connector.topic`
- `connector.port`
- `connector.resgroup`
- `flowsched.sched.policy`
- `flowsched.accessmap.rule`
- `flowsched.access.map`

行为：

- `connector.topic` 决定调度 topic
- `connector.topic` 缺失时使用 `__default__`
- `connector.port` 作为端口候选
- `connector.resgroup` 只参与 DAG 过滤
- `flowsched.*` 可覆盖调度策略或 access map
- 如果 task props 中存在 `source.listen.port`，最终端口优先使用 task props，而不是 `connector.port`

代码入口：

- `scheduler/src/schedgroup/mod.rs`
  - `DAGEvent::find_kafkaprotocol()`
  - `DAGEvent::on_add_job()`
- `scheduler/src/dag/filter.rs`
  - `Filter::kafkaprotocol_filter()`
  - `Filter::filter_impl()`
- `scheduler/src/dag/mod.rs`
  - `Task::calc_access_port()`
- `scheduler/src/schedgroup/group/mod.rs`
  - `calc_sched_addr()`

### 3.4 `source.kafka_protocol_binary`

处理方式与 `source.kafka_protocol` 相同。

代码里两者走同一分支：

```text
source.kafka_protocol
source.kafka_protocol_binary
        |
        v
find_kafkaprotocol()
```

## 4. `kafka_protocol` 示例 option 解读

输入示例：

```sql
) WITH (
    'name' = 'dwd_frontier_flow_log_access_log_hi__kafka_source',
    'connector.type' = 'kafka_protocol',
    'connector.mode' = 'source',

    'connector.topic' = 'dwd_frontier_flow_log_access_log_hi',
    'connector.port' = '9951',
    'port_range' = '9951-9954',
    'kafka.server.thread' = '6',
    'kafka.metadata.enable' = 'false',

    'format.type' = 'json',

    'connector.others.parallelism' = '6',
    'connector.parallelism' = '6',
    'connector.resgroup' = 'doubao_stream:default',
    'connector.others.resgroup' = 'doubao_stream:default',
    'connector.resgroup.policy' = 'broadcast',
    'connector.others.resgroup.policy' = 'broadcast'
);
```

### 4.1 会影响 scheduler 的字段

| Option | 是否影响 | 作用 |
| --- | --- | --- |
| `connector.topic` | 是 | 作为调度 topic，影响 `getaddr?topic=...` 查找 |
| `connector.port` | 是 | 作为端口候选，task props 没有 `source.listen.port` 时使用 |
| `connector.resgroup` | 是 | 参与 DAG allow/filter 过滤 |
| `flowsched.sched.policy` | 是 | DAG 内覆盖调度策略 |
| `flowsched.accessmap.rule` | 是 | DAG 内覆盖 access map 匹配规则 |
| `flowsched.access.map` | 是 | DAG 内提供 access map |

### 4.2 当前代码未使用的字段

这些字段在当前 commit 中没有被 scheduler 读取：

| Option | 当前 scheduler 行为 |
| --- | --- |
| `connector.type` | 不直接读取，类型由 `operator_unique_name` 决定 |
| `connector.mode` | 不读取 |
| `port_range` | 不读取 |
| `kafka.server.thread` | 不读取 |
| `kafka.metadata.enable` | 不读取 |
| `format.type` | 不读取 |
| `connector.parallelism` | 不读取 |
| `connector.others.parallelism` | 不读取 |
| `connector.others.resgroup` | 不读取 |
| `connector.resgroup.policy` | 不读取 |
| `connector.others.resgroup.policy` | 不读取 |

注意：

- 这些 option 可能被上游 Flink/Job Manager/Kafka connector 使用
- 但在当前 scheduler 代码里，它们不参与调度决策

## 5. `kafka_protocol` 调度路径

```text
  SQL WITH options
          |
          | Job Manager converts to DAG
          v
  +--------------------------+
  | SubTask                  |
  | operator_unique_name     |
  | operator_options         |
  +-----------+--------------+
              |
              | operator_unique_name:
              | source.kafka_protocol
              v
  +--------------------------+
  | Filter::filter_job       |
  +-----------+--------------+
              |
              | checks:
              | connector.topic
              | connector.resgroup
              v
  +--------------------------+
  | find_kafkaprotocol       |
  +-----------+--------------+
              |
              | extracts:
              | topic = connector.topic
              | port  = connector.port
              v
  +--------------------------+
  | SchedGroup(topic)        |
  +-----------+--------------+
              |
              | task access addr:
              | source_host_ip / slot_addr
              | source.listen.port / connector.port
              v
  +--------------------------+
  | getaddr result           |
  | ip:port                  |
  +--------------------------+
```

## 6. 端口选择规则

最终返回地址由：

- access IP
- access port

组成。

### 6.1 IP

`Task::calc_access_ip()` 规则：

- 如果 `source_host_ip` 非空且不是 `localhost`，使用 `source_host_ip`
- 否则从 `slot_addr` 去掉最后一个 `:port` 得到 IP
- IPv6 外层 `[]` 会被去掉，最终拼接时再补

### 6.2 Port

`Task::calc_access_port()` 规则：

```text
  task.props["source.listen.port"]
            |
            | exists
            v
       use this port

  otherwise
            |
            v
       use connector port:
       sharedhttpd: httpd.hostport
       kafka    : connector.port
```

因此，对于示例：

```text
connector.port = 9951
```

只有当 task props 中没有：

```text
source.listen.port
```

时，`9951` 才会成为最终调度地址端口。

`port_range = 9951-9954` 当前不会被 scheduler 解析。

## 7. Topic 与 resgroup 过滤

### 7.1 Topic

Kafka topic key：

- `connector.topic`

HTTPD topic key：

- `httpd.topic`

如果配置了 allow topics：

- Kafka 必须有 `connector.topic`
- HTTPD 必须有 `httpd.topic`
- topic 必须在 allow list 内

否则 job 会被过滤。

### 7.2 Resgroup

resgroup key：

- `connector.resgroup`

如果配置了 allow resgroups：

- subtask 必须有 `connector.resgroup`
- resgroup 必须在 allow list 内

如果配置了 filter resgroups：

- 命中的 `connector.resgroup` 会被过滤

注意：

- `connector.others.resgroup` 当前不参与 scheduler 过滤

## 8. 调度策略来源

调度策略优先级：

```text
  DAG option:
  flowsched.sched.policy
          |
          | absent
          v
  Config / env:
  sched.sched_policy[topic]
          |
          | absent
          v
  default:
  load-only
```

支持的策略类型：

- `load-only`
- `idc-and-load`
- `region-proportion`

注意：

- 当前代码中 `region-proportion` 有未实现逻辑，使用前需要谨慎验证

## 9. 环境变量如何影响 DAG 与调度

配置加载顺序：

```text
  ENV
   |
   | selects config file
   v
  YAML config
   |
   | replace_from_env()
   v
  env override
   |
   | finish()
   v
  final runtime config
```

### 9.1 配置文件选择

| 环境变量 | 影响 |
| --- | --- |
| `ENV` | 选择 `config/tide_scheduler.{ENV}.yaml` |

未设置 `ENV` 时读取：

- `config/tide_scheduler.yaml`

### 9.2 DAG 拉取与过滤

| 环境变量 | 影响 |
| --- | --- |
| `ALLOW_OPERATOR_NAMES` | 覆盖拉 DAG 时允许的 operator name |
| `TIDESCHED_AGENTSCHED_ALLOWED_TOPICS` | 覆盖 allow topics |
| `ALLOW_TOPICS` | 覆盖 allow topics |
| `TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS` | 覆盖 allow resgroups |
| `ALLOW_RESGROUPS` | 覆盖 allow resgroups |
| `FILTER_RESGROUPS` | 覆盖 filter resgroups |
| `FILTER_ALL` | 为 `true` 时不启动 DAGReader |
| `DAG_POLL_INTERVAL_MS` | 覆盖 DAG 轮询间隔 |

`ALLOW_*` / `FILTER_RESGROUPS` 的值是 JSON 字符串数组。

示例：

```bash
export ALLOW_OPERATOR_NAMES='["source.kafka_protocol","source.kafka_protocol_binary"]'
export ALLOW_TOPICS='["dwd_frontier_flow_log_access_log_hi"]'
export ALLOW_RESGROUPS='["doubao_stream:default"]'
export DAG_POLL_INTERVAL_MS=3000
```

### 9.3 调度策略

| 环境变量 | 影响 |
| --- | --- |
| `SCHED_POLICY` | 覆盖全局 topic 调度策略 map |
| `ACCESSMAP_RULE` | 覆盖全局 access map rule |

`SCHED_POLICY` 是 JSON map，key 是 topic。

示例：

```bash
export SCHED_POLICY='{
  "dwd_frontier_flow_log_access_log_hi": {
    "load-only": {}
  }
}'
```

### 9.4 getaddr 行为

| 环境变量 | 影响 |
| --- | --- |
| `GETADDR_POLICY_FIXADDR` | 强制 `Fixaddr` |
| `TIDESCHED_AGENTSCHED_FIX_ADDR` | 强制 `Fixaddr` |
| `GETADDR_POLICY_FIXADDR_AND_NORMAL` | 强制 `FixaddrAndNormal` |
| `GETADDR_DEFAULT_TOPIC` | 使用 default topic policy |
| `FORWARD_ADDRESS` | 覆盖 forward 目标 |
| `TIDESCHED_AGENTSCHED_FORWARD_TARGET` | 覆盖 forward 目标 |
| `FORWARD_PATH` | 覆盖 forward path |
| `TIDESCHED_AGENTSCHED_FORWARD_PATH` | 覆盖 forward path |
| `FORWARD_DEFAULT_TOPIC` | 覆盖无 topic 时的默认 topic |
| `GETADDR_PATH_PREFIX1` | 覆盖 getaddr path prefix |
| `NOTOPIC_POLICY` | topic 缺失时使用 `error` 或 `random` |

重要联动：

- 当 policy 是 `Fixaddr` 或 `Forward` 时，`finish()` 会：
  - 强制 `dag_filter.filter_all()`
  - 关闭 distributed mode

这意味着：

- DAGReader 不会启动
- Redis/分布式状态不会参与普通调度

### 9.5 地址与端口相关

| 环境变量 | 影响 |
| --- | --- |
| `TIDESCHED_LISTEN_PORT` | 覆盖 scheduler HTTP/gRPC 监听端口 |
| `LISTEN_PORT` | 覆盖 scheduler HTTP/gRPC 监听端口 |
| `PORT` | TCE 环境下影响自身对外通告端口，不是 bind 端口 |

注意：

- 当前 commit 未发现 `KAFKA_LISTEN_PORT`
- 当前 commit 未发现 `KAFKA_ADVERTISED_HOST`
- 当前 commit 未发现独立 Kafka server/facade 入口

### 9.6 截图中的进程环境变量

截图里看到的进程环境变量如下：

```bash
TIDESCHED_KAFKASERVICE_ENABLE=false
TRACELOG_ENABLE=true
ENV=tce.online
GCTUNER_MODE=disable
TIDESCHED_DEBUG_LISTEN_ENABLE=true
TIDESCHED_IP_LIBRARY_READER_NAME='[["bytedance-oort", "false"]]'
TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly
SEC_KV_AUTH=1
TIDESCHED_AGENTSCHEDULE_ENABLE=true
TIDESCHED_AGENTSCHED_ENABLE_IDCSELECTOR=false
TIDESCHED_AGENTSCHED_ENABLE_NETROUTE=false
```

当前 commit 中，只有部分变量被本仓库代码直接读取。其余变量可能来自部署平台、依赖库、sidecar、旧版本配置或下游服务，不应直接推断为会影响当前 scheduler 的调度逻辑。

| 环境变量 | 当前代码是否直接读取 | 含义与影响 |
| --- | --- | --- |
| `ENV=tce.online` | 是 | 选择配置文件 `config/tide_scheduler.tce.online.yaml` |
| `TRACELOG_ENABLE=true` | 是 | 覆盖日志配置里的 trace log 开关 |
| `TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly` | 是 | 覆盖 Job Manager 请求中的 cluster，直接影响拉取哪个集群的 DAG |
| `TIDESCHED_KAFKASERVICE_ENABLE=false` | 未发现 | 当前 commit 未发现 Kafka service/facade 入口，也未发现该变量读取点 |
| `GCTUNER_MODE=disable` | 未发现 | 当前仓库未读取，可能是运行时或平台 GC/tuner 相关变量 |
| `TIDESCHED_DEBUG_LISTEN_ENABLE=true` | 未发现 | 当前 commit 有 `DEBUG_LISTEN_PORT`，但未发现该 enable 变量读取点 |
| `TIDESCHED_IP_LIBRARY_READER_NAME` | 未发现 | 当前仓库未读取；IP 元数据当前主要看 `IP_FETCHER`、`IP_MAPPING_RULES`、`IPFETCHER_USE_GALAXY` 等变量 |
| `SEC_KV_AUTH=1` | 未发现 | 当前仓库未读取；可能是安全/凭证系统相关变量 |
| `TIDESCHED_AGENTSCHEDULE_ENABLE=true` | 未发现 | 当前 commit 未读取；不能据此判断 scheduler 一定启用 agent schedule 分支 |
| `TIDESCHED_AGENTSCHED_ENABLE_IDCSELECTOR=false` | 未发现 | 当前 commit 未读取；IDC 选择主要由调度策略、请求头、IP metadata、access map 决定 |
| `TIDESCHED_AGENTSCHED_ENABLE_NETROUTE=false` | 未发现 | 当前 commit 未读取；网络路由行为不能从该变量推断 |

### 9.7 截图变量对 DAG/Kafka 调度的实际影响

对 `kafka_protocol` 调度最关键的是：

```text
ENV
  |
  v
selected YAML
  |
  v
TIDESCHED_JOBMANAGER_CLUSTER
  |
  v
GetExecutorDAG(cluster)
  |
  v
DAG operator/options
  |
  v
source.kafka_protocol
connector.topic
connector.port
connector.resgroup
```

实际影响排序：

| 优先级 | 变量 | 为什么重要 |
| --- | --- | --- |
| 1 | `ENV` | 选错配置文件会导致 Job Manager、DAG filter、Redis、策略全都不同 |
| 2 | `TIDESCHED_JOBMANAGER_CLUSTER` | 直接决定向 Job Manager 请求哪个 cluster 的 DAG |
| 3 | `TRACELOG_ENABLE` | 影响日志观测，不直接改变调度结果 |
| 4 | 其他截图变量 | 当前 commit 未发现直接读取点，先按外部运行环境变量处理 |

如果线上看到 `connector.topic`、`connector.port` 都正确，但调度 topic 不存在或 topicstats 为空，优先检查：

```bash
echo "$ENV"
echo "$TIDESCHED_JOBMANAGER_CLUSTER"
env | grep -E '^(ALLOW_OPERATOR_NAMES|ALLOW_TOPICS|ALLOW_RESGROUPS|FILTER_RESGROUPS|FILTER_ALL|DAG_POLL_INTERVAL_MS)='
```

### 9.8 如何确认某个环境变量是否真的被当前版本使用

在当前 commit 中，可以用下面的方式确认：

```bash
cd /root/Documents/flow-scheduler
grep -R 'TIDESCHED_JOBMANAGER_CLUSTER' -n scheduler/src
grep -R 'TIDESCHED_KAFKASERVICE_ENABLE' -n scheduler/src
grep -R 'TIDESCHED_AGENTSCHED_ENABLE_IDCSELECTOR' -n scheduler/src
```

判断规则：

- 能在 `scheduler/src` 找到读取点，才说明当前 scheduler 代码直接使用
- 找不到读取点，不代表变量无意义，但它可能属于平台、依赖库、sidecar 或旧版本
- 对调度排查来说，先看当前代码直接读取的变量，再看外部系统变量

### 9.9 按 `ENV=tce.online` 解析配置

如果进程环境变量是：

```bash
ENV=tce.online
```

当前代码会加载：

- `scheduler/config/tide_scheduler.tce.online.yaml`

文件内容很短：

```yaml
service:
  listen_port: 6789
  distributed_mode:
    !bytedredis { "psm": "toutiao.redis.tidescheduler" }
jobmgr:
  access:
    !domain { "addr": "tidejobmgr.byted.org:80" }
  cluster: "fringedb-newly"
log:
  level: "Info"
  agent_enable: true
discover:
  !byte_sd { "other_idcs": ["hl","lf","lq","yg"] }
```

注意配置文件选择规则：

```text
ENV=tce.online
        |
        v
config/tide_scheduler.tce.online.yaml

ENV=tce.online.sg
        |
        v
config/tide_scheduler.tce.online.sg.yaml
```

所以，截图里的：

```bash
ENV=tce.online
```

对应的是：

```text
config/tide_scheduler.tce.online.yaml
```

不是：

```text
config/tide_scheduler.tce.online.sg.yaml
```

如果要使用 SG 配置，环境变量需要改成：

```bash
ENV=tce.online.sg
```

### 9.10 `tce.online.yaml` 与截图环境变量叠加后的关键结果

以下解析假设：

- 明确加载 `scheduler/config/tide_scheduler.tce.online.yaml`
- 叠加截图里的环境变量
- 没有设置其他未展示的覆盖变量

| 配置项 | YAML 原值 | 环境变量覆盖 | 最终值 | 影响 |
| --- | --- | --- | --- | --- |
| 配置文件 | `tide_scheduler.tce.online.yaml` | `ENV=tce.online` | `tide_scheduler.tce.online.yaml` | 决定整套在线配置生效 |
| `service.listen_port` | `6789` | 截图无 `TIDESCHED_LISTEN_PORT` / `LISTEN_PORT` | `6789` | HTTP/gRPC 监听端口 |
| `service.distributed_mode` | `bytedredis` | 截图无 `BYTEREDIS_PSM` | `bytedredis` | 使用 ByteRedis 作为分布式状态后端 |
| `service.distributed_mode.psm` | `toutiao.redis.tidescheduler` | 截图无 `BYTEREDIS_PSM` | `toutiao.redis.tidescheduler` | Redis 地址通过该 PSM 服务发现 |
| `jobmgr.access` | `domain` | 截图无 `TIDESCHED_JMACCESSOR_MODE` / `JOBMGR_ACCESSOR_MODE` | `domain` | Job Manager 通过固定域名访问 |
| `jobmgr.access.addr` | `tidejobmgr.byted.org:80` | 截图无 `JOBMGR_ADDRESS` / `TIDESCHED_JOBMANAGER_ADDRESS` | `tidejobmgr.byted.org:80` | DAG 从在线 Job Manager 域名拉取 |
| `jobmgr.cluster` | `fringedb-newly` | `TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly` | `fringedb-newly` | `GetExecutorDAG` 请求的 cluster |
| `log.level` | `Info` | 截图无 `LOG_LEVEL` | `Info` | 日志级别 |
| `log.agent_enable` | `true` | 截图无对应覆盖项 | `true` | 打开 agent 风格日志字段 |
| `log.tracelg_enable` | 默认 `false` | `TRACELOG_ENABLE=true` | `true` | 打开 trace log |
| `discover` | `byte_sd` | 截图无 discover mode 覆盖 | `byte_sd` | 本服务对 peer/partner 使用 byte_sd 发现 |
| `discover.other_idcs` | `["hl","lf","lq","yg"]` | 截图无 `OTHER_IDCS` | `["hl","lf","lq","yg"]` | 额外发现这些 IDC 的 peer/partner |
| `service.dag_filter.allow_operator_names` | 默认 Kafka/HTTPD 三类 | 截图无 `ALLOW_OPERATOR_NAMES` | `source.sharedhttpd`, `source.kafka_protocol`, `source.kafka_protocol_binary` | Job Manager 只返回这些 operator 相关 DAG |
| `service.dag_filter.allow_topics` | 默认 `None` | 截图无 `ALLOW_TOPICS` | `None` | 不按 topic allow-list 过滤 |
| `service.dag_filter.allow_resgroup` | 默认 `None` | 截图无 `ALLOW_RESGROUPS` | `None` | 不按 resgroup allow-list 过滤 |
| `service.dag_filter.filter_resgroup` | 默认 `None` | 截图无 `FILTER_RESGROUPS` | `None` | 不按 resgroup deny-list 过滤 |
| `service.dag_filter.filterall` | 默认 `false` | 截图无 `FILTER_ALL` | `false` | DAGReader 会启动 |
| `service.dag_poll_interval` | 默认 `12000` ms | 截图无 `DAG_POLL_INTERVAL_MS` | `12000` ms | 每 12 秒刷新 DAG |
| `service.getaddr_policy` | 默认 `normal` | 截图无 fixaddr/forward 覆盖 | `normal` | 正常走 DAG + 状态 + 策略调度 |
| `service.ipfetcher` | 默认 `none` | 截图无 `IP_FETCHER` / `IP_MAPPING_RULES` | `none` | 当前 scheduler 不启用配置化 IP metadata fetcher |

### 9.11 `tce.online` 配置对 `kafka_protocol` 的直接影响

```text
config/tide_scheduler.tce.online.yaml
        |
        v
jobmgr:
  domain = tidejobmgr.byted.org:80
  cluster = fringedb-newly
        |
        v
GetExecutorDAG(
  cluster = fringedb-newly,
  allowed_operator_names = [
    source.sharedhttpd,
    source.kafka_protocol,
    source.kafka_protocol_binary
  ],
  allowed_resgroups = []
)
        |
        v
DAG subtask:
  source.kafka_protocol
  connector.topic
  connector.port
  connector.resgroup
        |
        v
SchedGroup(topic)
```

对 Kafka 调度来说，`tce.online.yaml` 最关键的点是：

- `jobmgr.access.addr = tidejobmgr.byted.org:80`
- `jobmgr.cluster = fringedb-newly`
- `service.distributed_mode.psm = toutiao.redis.tidescheduler`
- 默认允许 `source.kafka_protocol` 和 `source.kafka_protocol_binary`

如果 `GetAddr` 查不到 Kafka topic，优先排查：

```bash
echo "$ENV"
echo "$TIDESCHED_JOBMANAGER_CLUSTER"
env | grep -E '^(ALLOW_OPERATOR_NAMES|ALLOW_TOPICS|ALLOW_RESGROUPS|FILTER_RESGROUPS|FILTER_ALL)='
```

如果 `topicstats` 全 0 或 heartbeat/statsreport 写入失败，优先排查：

```bash
env | grep -E '^(BYTEREDIS_PSM|SEC_TOKEN_STRING|SEC_TOKEN_PATH)='
sd lookup toutiao.redis.tidescheduler
```

### 9.12 与 `tce.online.sg.yaml` 的差异

如果要使用：

```bash
ENV=tce.online.sg
```

实际会加载 `tide_scheduler.tce.online.sg.yaml`。它和当前 `tce.online.yaml` 的关键差异是：

| 配置项 | `tce.online.yaml` | `tce.online.sg.yaml` |
| --- | --- | --- |
| Redis PSM | `toutiao.redis.tidescheduler` | `toutiao.redis.tidescheduler.service.sgcompliance` |
| Job Manager addr | `tidejobmgr.byted.org:80` | `tidejobmgr-sg.byted.org:80` |
| Job Manager cluster | `fringedb-newly` | `fringedb-newly` |
| discover other idcs | `["hl","lf","lq","yg"]` | `[]` |

这意味着：

- 截图里的 `ENV=tce.online` 会使用主在线 Redis PSM 和主在线 Job Manager 域名
- 只有 `ENV=tce.online.sg` 才会使用 SG Redis PSM 和 SG Job Manager 域名
- 如果线上机器在 SG，但 `ENV` 仍是 `tce.online`，DAG 和 Redis 访问目标会按主在线配置走

## 10. 排查方法

### 10.1 确认 DAG 是否被拉取

看日志：

```text
prepare pull DAG from addr ...
successfully pull ... DAGs
failed to pull DAG because ...
```

代码：

- `scheduler/src/dag/mod.rs`
  - `DAGReader::pull_impl()`
  - `DAGReader::start()`

### 10.2 确认 operator 是否被允许

检查最终配置：

- `ALLOW_OPERATOR_NAMES`
- YAML 中 `service.dag_filter.allow_operator_names`

Kafka 需要包含：

```text
source.kafka_protocol
source.kafka_protocol_binary
```

否则 Job Manager 请求阶段可能就拿不到相关 DAG。

### 10.3 确认 topic 是否匹配

Kafka 使用：

```text
connector.topic
```

HTTPD 使用：

```text
httpd.topic
```

如果设置了 `ALLOW_TOPICS`，需要确认 topic 在列表内。

### 10.4 确认 resgroup 是否匹配

scheduler 只读取：

```text
connector.resgroup
```

不读取：

```text
connector.others.resgroup
```

如果设置了 `ALLOW_RESGROUPS`，需要确认 `connector.resgroup` 在列表内。

如果设置了 `FILTER_RESGROUPS`，需要确认没有被命中。

### 10.5 确认端口来源

优先级：

```text
task.props["source.listen.port"]
        >
connector.port
```

排查时不要只看 SQL 里的：

```text
connector.port
```

还要看 Job Manager 返回 task slot props 里是否存在：

```text
source.listen.port
```

### 10.6 确认策略来源

优先级：

```text
flowsched.sched.policy
        >
SCHED_POLICY / config.sched.sched_policy
        >
load-only
```

如果策略不符合预期，先查 DAG option，再查环境变量。

## 11. 快速定位清单

```text
  [1] ENV 选中了哪个 YAML?
  [2] FILTER_ALL 是否为 true?
  [3] ALLOW_OPERATOR_NAMES 是否包含 Kafka?
  [4] DAG 中 operator_unique_name 是什么?
  [5] connector.topic 是否存在?
  [6] ALLOW_TOPICS 是否过滤了它?
  [7] connector.resgroup 是否存在?
  [8] ALLOW_RESGROUPS / FILTER_RESGROUPS 是否命中?
  [9] task props 是否覆盖 source.listen.port?
 [10] flowsched.sched.policy 是否覆盖策略?
 [11] GETADDR policy 是否变成 Fixaddr/Forward?
```

## 12. 关键代码索引

| 目的 | 代码 |
| --- | --- |
| DAG 拉取 | `scheduler/src/dag/mod.rs::DAGReader::pull_impl` |
| DAG 轮询 | `scheduler/src/dag/mod.rs::DAGReader::start` |
| DAG 过滤 | `scheduler/src/dag/filter.rs::Filter::filter_job` |
| Kafka topic 过滤 | `scheduler/src/dag/filter.rs::Filter::kafkaprotocol_filter` |
| 默认 operator | `scheduler/src/config/mod.rs::DAGFilter::default_allow_operator_names` |
| Kafka option 解析 | `scheduler/src/schedgroup/mod.rs::DAGEvent::find_kafkaprotocol` |
| HTTPD option 解析 | `scheduler/src/schedgroup/mod.rs::DAGEvent::find_sharedhttpd` |
| 调度组构建 | `scheduler/src/schedgroup/mod.rs::DAGEvent::on_add_job` |
| 端口选择 | `scheduler/src/dag/mod.rs::Task::calc_access_port` |
| 地址拼接 | `scheduler/src/schedgroup/group/mod.rs::calc_sched_addr` |
| 策略配置 | `scheduler/src/config/mod.rs::Sched` |
| env 覆盖 | `scheduler/src/config/mod.rs::replace_from_env` |
