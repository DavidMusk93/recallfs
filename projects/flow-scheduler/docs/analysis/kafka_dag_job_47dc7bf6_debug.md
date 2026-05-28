# Kafka Protocol DAG 解析排查：job `47dc7bf6-481b-487b-bd89-41420c6c58d7`

本文目标：

- 判断 job `47dc7bf6-481b-487b-bd89-41420c6c58d7` 的 DAG 是否被 scheduler 正确拉取、过滤和解析
- 判断它是否按 `kafka_protocol` 建成 topic 调度组
- 判断它是否进入状态管理和 Redis 运行态闭环

当前代码基于 commit：

- `6b5dd5b2ab310e4be5bec5227694983040ce05ac`

## 1. 总体排查图

```text
  logs / runtime
        |
        v
  +------------------+
  | DAG pull         |
  | Job Manager      |
  +--------+---------+
           |
           v
  +------------------+
  | DAG diff/cache   |
  | add/update task  |
  +--------+---------+
           |
           v
  +------------------+
  | DAG filter       |
  | kafka_protocol   |
  +--------+---------+
           |
           v
  +------------------+
  | Schedgroup       |
  | topic -> job     |
  +--------+---------+
           |
           v
  +------------------+
  | StateManager     |
  | job -> tasks     |
  +--------+---------+
           |
           v
  +------------------+
  | Redis runtime    |
  | heartbeat/stats  |
  +------------------+
```

## 2. 先确认环境和配置

线上截图里如果是：

```bash
ENV=tce.online
TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly
```

当前代码会加载：

```text
config/tide_scheduler.tce.online.yaml
```

关键配置结果：

```text
jobmgr.access.addr = tidejobmgr.byted.org:80
jobmgr.cluster     = fringedb-newly
redis psm          = toutiao.redis.tidescheduler
```

先在机器上确认：

```bash
echo "$ENV"
echo "$TIDESCHED_JOBMANAGER_CLUSTER"
env | grep -E '^(ALLOW_OPERATOR_NAMES|ALLOW_TOPICS|ALLOW_RESGROUPS|FILTER_RESGROUPS|FILTER_ALL|DAG_POLL_INTERVAL_MS)='
env | grep -E '^(BYTEREDIS_PSM|SEC_TOKEN_STRING|SEC_TOKEN_PATH)='
```

重点判断：

- `FILTER_ALL=true` 会导致 DAGReader 不启动
- `ALLOW_OPERATOR_NAMES` 如果不包含 `source.kafka_protocol` / `source.kafka_protocol_binary`，Job Manager 请求阶段就可能拿不到 Kafka DAG
- `ALLOW_TOPICS` / `ALLOW_RESGROUPS` / `FILTER_RESGROUPS` 会让 job 被本地 filter 掉
- `BYTEREDIS_PSM` 如果存在，会覆盖 YAML 中的 Redis PSM

## 3. 日志文件范围

常见日志位置取决于部署方式和 `redirect` feature。

建议先找这些文件：

```bash
ls -lh logs 2>/dev/null
find . -maxdepth 3 -type f \( -name '*.log' -o -name 'stdout*' -o -name 'stderr*' \) | sort
```

如果是线上容器日志，也可以把下面命令里的 `LOG_FILE` 换成平台导出的日志文件。

```bash
export JOBID='47dc7bf6-481b-487b-bd89-41420c6c58d7'
export LOG_FILE='logs/stdout.log'
```

如果不确定日志文件：

```bash
grep -R "$JOBID" -n logs . 2>/dev/null | head -50
```

## 4. 第一步：确认 DAGReader 正常拉 DAG

先看是否持续从 Job Manager 拉 DAG：

```bash
grep -nE 'prepare pull DAG|successfully pull|failed to pull DAG|get executor DAG' "$LOG_FILE" | tail -100
```

正向信号：

```text
prepare pull DAG from addr http://tidejobmgr.byted.org:80 cluster fringedb-newly
successfully pull ... DAGs
```

反向信号：

```text
failed to pull DAG because ...
get executor DAG resp code ... message ...
connect to jobmgr addr ... cluster ...
```

如果这里失败，优先排查：

- `ENV` 是否加载了正确 YAML
- `TIDESCHED_JOBMANAGER_CLUSTER` 是否正确
- `jobmgr.access.addr` 是否可访问
- `ALLOW_OPERATOR_NAMES` 是否把 Kafka operator 排除了

对应代码：

- `scheduler/src/dag/mod.rs::DAGReader::pull_impl`
- `scheduler/src/dag/mod.rs::DAGReader::start`

## 5. 第二步：确认目标 job 出现在 DAG diff 中

直接 grep jobid：

```bash
grep -n "$JOBID" "$LOG_FILE" | tail -200
```

新增 job 时可能出现：

```text
detect that N new jobs (47dc7bf6-481b-487b-bd89-41420c6c58d7) have been added, and notify M listeners
```

任务变化时可能出现：

```text
detected that N jobs (47dc7bf6-481b-487b-bd89-41420c6c58d7) had their tasks added
detect that N job (47dc7bf6-481b-487b-bd89-41420c6c58d7) tasks have been updated
detect that N job tasks (47dc7bf6-481b-487b-bd89-41420c6c58d7) have been deleted
```

注意：

- DAG diff 日志只打印集合里的一个 jobid
- 如果一次变更多个 job，目标 job 可能不在这一行出现
- 所以必须继续看 `/statistics` 或更细日志，而不是只靠这行判断不存在

对应代码：

- `scheduler/src/dag/mod.rs::DAGReader::pull_impl`

## 6. 第三步：确认没有被 DAG filter 掉

Kafka protocol DAG 进入 filter 的条件：

```text
SubTask.operator_unique_name =
  source.kafka_protocol
  or source.kafka_protocol_binary
```

检查日志：

```bash
grep -nE "job $JOBID passes the filter|job $JOBID does not meet any conditions|connector.resgroup of job $JOBID|topic .* of job $JOBID|allow topics are set|allow resgroups are set|filter resgroups are set" "$LOG_FILE" | tail -100
```

正向信号：

```text
job 47dc7bf6-481b-487b-bd89-41420c6c58d7 passes the filter
```

反向信号：

```text
job 47dc7bf6-481b-487b-bd89-41420c6c58d7 does not meet any conditions and needs to be filtered out
```

常见过滤原因：

```text
allow topics are set, but ... topic ... is not within the allowed range
allow resgroups are set, but connector.resgroup ... is not within the allowed range
filter resgroups are set, and the resgroup ... is within the filter range
allow topics are set, but ... connector.topic ... is not set
```

Kafka DAG 要重点确认：

- operator 是 `source.kafka_protocol` 或 `source.kafka_protocol_binary`
- option 里有 `connector.topic`
- 如果启用了 resgroup allow/filter，option 里有正确的 `connector.resgroup`
- 不是只配置了 `connector.others.resgroup`

对应代码：

- `scheduler/src/dag/filter.rs::Filter::filter_job`
- `scheduler/src/dag/filter.rs::Filter::kafkaprotocol_filter`
- `scheduler/src/dag/filter.rs::Filter::filter_impl`

## 7. 第四步：确认 Schedgroup 正确解析 Kafka options

通过 filter 后，SchedgroupManager 会监听 job add 事件。

检查日志：

```bash
grep -nE "schedgroup manager listens to job $JOBID|scheduling group manager detects that job $JOBID|merge job $JOBID|use job $JOBID" "$LOG_FILE" | tail -100
```

正向信号：

```text
the schedgroup manager listens to job 47dc7bf6-481b-487b-bd89-41420c6c58d7 ... groups additions ...
the scheduling group manager detects that job 47dc7bf6-481b-487b-bd89-41420c6c58d7 is added, and builds ... topic groups
use job 47dc7bf6-481b-487b-bd89-41420c6c58d7 to create a new dispatch group for topic ...
```

或者：

```text
merge job 47dc7bf6-481b-487b-bd89-41420c6c58d7 into the dispatch group of topic ...
```

如果只看到：

```text
the schedgroup manager listens to job ...
```

但没有：

```text
builds ... topic groups
```

说明 job 通过了前面某些阶段，但 `SchedgroupManager` 没从 subtask 中识别到可建 topic group 的 operator。

对 Kafka 来说，重点怀疑：

- `operator_unique_name` 不是 `source.kafka_protocol` / `source.kafka_protocol_binary`
- `connector.topic` 不是合法 UTF-8
- `connector.topic` 缺失时会落到 `__default__`，可能不是你查询的 topic
- `connector.port` 缺失时后续可能依赖 task props 里的 `source.listen.port`

对应代码：

- `scheduler/src/schedgroup/mod.rs::DAGEvent::on_add_job`
- `scheduler/src/schedgroup/mod.rs::DAGEvent::find_kafkaprotocol`

## 8. 第五步：确认 StateManager 建了 job/task 状态

Schedgroup 和 StateManager 都是 DAG listener。正确解析后，StateManager 应该看到这个 job。

检查日志：

```bash
grep -nE "state manager listens to job $JOBID|in the job $JOBID new event|resource task group" "$LOG_FILE" | tail -200
```

正向信号：

```text
the state manager listens to job 47dc7bf6-481b-487b-bd89-41420c6c58d7 ... tasks additions and builds the infrastructure for it
in the job 47dc7bf6-481b-487b-bd89-41420c6c58d7 new event, after iterating to task ...
```

如果 Schedgroup 有日志但 StateManager 没日志，说明 listener 通知链路或 StateManager 构建异常，需要看：

```bash
grep -nE "failed to notify add job $JOBID|failed to notify|state manager listens" "$LOG_FILE" | tail -100
```

对应代码：

- `scheduler/src/statemgr/mod.rs::DAGEvent::on_add_job`

## 9. 第六步：用 HTTP `/statistics` 验证内存态

如果服务可访问，直接查：

```bash
curl -s 'http://127.0.0.1:6789/statistics' > /tmp/tidesched-statistics.json
grep -o "$JOBID" /tmp/tidesched-statistics.json | head
```

推荐用 `jq`：

```bash
curl -s 'http://127.0.0.1:6789/statistics' \
  | jq --arg job "$JOBID" '{
      in_job_caches: (.job_caches[$job] != null),
      in_job_states: (.job_states[$job] != null),
      related_topics: (.topics
        | to_entries
        | map(select((.value | tostring) | contains($job)))
        | map(.key))
    }'
```

判断：

- `in_job_caches=true` 表示 DAGReader cache 中有这个 job
- `in_job_states=true` 表示 StateManager 已为这个 job 建状态
- `related_topics` 有值表示 Schedgroup topic 中包含这个 job

如果：

```text
in_job_caches=true
in_job_states=false
```

说明 DAG 被拉到，但没有成功通知或构建状态。

如果：

```text
in_job_caches=true
related_topics=[]
```

说明 DAG 被拉到，但没有建成可调度 topic group，重点查 Kafka operator/options。

接口代码：

- `scheduler/src/server/statistics.rs`
- `scheduler/src/server/mod.rs`

## 10. 第七步：用 `/topicstats` 验证 topic -> job

如果你知道 Kafka topic，例如：

```bash
export TOPIC='dwd_frontier_flow_log_access_log_hi'
```

查询：

```bash
curl -s "http://127.0.0.1:6789/stable/topicstats?topic=$TOPIC" \
  | jq --arg job "$JOBID" '.job_res_tg_stats[$job]'
```

判断：

- 返回对象：该 topic 的 SchedGroup 中包含这个 job
- 返回 `null`：该 topic 下没有这个 job

如果返回对象但计数全 0：

- DAG 解析和 topic group 可能已经成功
- runtime heartbeat/statsreport 或 Redis 状态可能没有正常进入

接口代码：

- `scheduler/src/server/topicstats.rs`
- `scheduler/src/schedgroup/mod.rs::SchedgroupManager::topic_stats`

## 11. 第八步：确认 Redis 运行态

Redis key 格式：

```text
{cluster}:{jobid}:{taskid}
```

本例 cluster 来自：

```text
TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly
```

所以 key 形如：

```text
fringedb-newly:47dc7bf6-481b-487b-bd89-41420c6c58d7:{taskid}
```

先从 `/statistics` 里拿 taskid：

```bash
curl -s 'http://127.0.0.1:6789/statistics' \
  | jq -r --arg job "$JOBID" '.job_caches[$job].tasks | keys[]'
```

如果 `statistics` 结构和预期不一致，可以先直接看 job 片段：

```bash
curl -s 'http://127.0.0.1:6789/statistics' \
  | jq --arg job "$JOBID" '.job_caches[$job], .job_states[$job]'
```

然后用调试工具或 Redis 客户端查 key。

如果使用项目 gRPC 调试工具，路径在：

- `tests/client/toolset`
- gRPC 方法：`PullKeyFromRedis`

排查目标：

- key 是否存在
- hash 里是否有 `alive`
- hash 里是否有 `runtime`
- `alive` 是否过期或持续为 false
- `runtime` 是否能解析出队列长度和 rows/sec

相关写入日志：

```bash
grep -nE "store job $JOBID task .* alive|failed to dispatch heartbeat|failed to dispatch statsreport|failed to get job $JOBID task .* key|parse job $JOBID task .* state" "$LOG_FILE" | tail -200
```

如果看到：

```text
failed to dispatch heartbeat because [byted-redis] no available address
failed to dispatch statsreport because [byted-redis] no available address
```

说明 DAG 解析不一定有问题，运行态写 Redis 失败才是主因。

## 12. 结论判断表

| 现象 | 判断 | 下一步 |
| --- | --- | --- |
| 没有 `prepare pull DAG` | DAGReader 没启动或日志级别不够 | 查 `FILTER_ALL`、启动日志、配置 |
| 有 `failed to pull DAG` | Job Manager 访问或请求失败 | 查 Job Manager 地址、cluster、网络 |
| grep 不到 jobid | DAG 可能没返回或日志只打印集合首个 job | 用 `/statistics` 查 `job_caches` |
| `job ... does not meet any conditions` | 被 filter 掉 | 查 operator、topic、resgroup |
| `job ... passes the filter` 但无 Schedgroup 日志 | listener 或构建失败 | 查 `failed to notify add job` |
| 有 Schedgroup listens 但无 topic groups | Kafka subtask 未识别或 topic 异常 | 查 `operator_unique_name`、`connector.topic` |
| 有 topic group 但 `/topicstats` 无 job | topic 查错或 group 后续被更新删除 | 查 topic 名、remove/update 日志 |
| `/topicstats` 有 job 但全 0 | DAG 解析 OK，runtime 状态异常 | 查 heartbeat/statsreport/Redis |
| Redis 报 `no available address` | Redis client 地址列表或服务发现问题 | 查 PSM、`sd lookup`、byted-redis |

## 13. 最短命令路径

```bash
export JOBID='47dc7bf6-481b-487b-bd89-41420c6c58d7'
export LOG_FILE='logs/stdout.log'

# 1. DAGReader 是否工作
grep -nE 'prepare pull DAG|successfully pull|failed to pull DAG|get executor DAG' "$LOG_FILE" | tail -100

# 2. 目标 job 的所有日志
grep -n "$JOBID" "$LOG_FILE" | tail -200

# 3. 是否通过 filter
grep -nE "job $JOBID passes the filter|job $JOBID does not meet any conditions|connector.resgroup of job $JOBID|topic .* of job $JOBID" "$LOG_FILE" | tail -100

# 4. 是否建成调度组
grep -nE "schedgroup manager listens to job $JOBID|scheduling group manager detects that job $JOBID|merge job $JOBID|use job $JOBID" "$LOG_FILE" | tail -100

# 5. 是否进入 StateManager
grep -nE "state manager listens to job $JOBID|in the job $JOBID new event" "$LOG_FILE" | tail -100

# 6. 内存态验证
curl -s 'http://127.0.0.1:6789/statistics' \
  | jq --arg job "$JOBID" '{
      in_job_caches: (.job_caches[$job] != null),
      in_job_states: (.job_states[$job] != null),
      related_topics: (.topics
        | to_entries
        | map(select((.value | tostring) | contains($job)))
        | map(.key))
    }'
```

## 14. 如果需要更强证据

当前代码对 `connector.topic`、`connector.port`、`connector.resgroup` 的具体值没有逐项打印。

如果日志无法判断 Kafka option，建议临时加诊断日志：

```text
jobid
groupid
operator_unique_name
connector.topic
connector.port
connector.resgroup
source.listen.port
```

最佳加点位置：

- `scheduler/src/schedgroup/mod.rs::DAGEvent::find_kafkaprotocol`
- `scheduler/src/dag/filter.rs::Filter::filter_impl`

这样可以直接证明：

- Job Manager 返回的 operator 是否是 Kafka
- Kafka topic 是否是预期 topic
- port 是否来自 `connector.port`
- resgroup 是否命中 allow/filter
