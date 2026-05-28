# Kafka 调度启动配置与环境变量分析

## 输入标注

本文档基于以下进程启动配置分析。为避免把明文凭据写入仓库，`Resource.Authorization.Password` 已脱敏。

```json
{
  "ID": 2335,
  "ClusterType": "OnlySche",
  "ClusterVersion": "frontier_phy_scheduler_allinone_1.0.0.646_newport",
  "ComponentType": "tidesched",
  "Resource": {
    "Src": "http://luban-source.byted.org/repository/scm/data.tide.scheduler_1.0.0.646.tar.gz",
    "Dest": "scheduler_1.0.0.646",
    "Decompress": true,
    "Authorization": {
      "User": "zhangchaoming.1999",
      "Password": "***REDACTED***"
    }
  },
  "Env": [
    {"Key": "TIDESCHED_AGENTSCHEDULE_ENABLE", "Val": "true"},
    {"Key": "TIDESCHED_MEMORY_OF_MACHINE_FILTER_ENABLE", "Val": "false"},
    {"Key": "TIDESCHED_DEBUG_LISTEN_ENABLE", "Val": "true"},
    {"Key": "TIDESCHED_MEMORY_OF_MACHINE_THRESHOLD", "Val": "0.65"},
    {"Key": "TIDESCHED_IP_LIBRARY_READER_NAME", "Val": "bytedance-cdn-schedule"},
    {"Key": "TIDESCHED_ORGANIZATION_NET_READER_LIST", "Val": "'[[\"bytedance-oort\",\"false\"]]'"},
    {"Key": "TIDESCHED_AGENTSCHED_ENABLE_NETROUTE", "Val": "false"},
    {"Key": "TIDESCHED_KAFKASERVICE_DEFAULT_TOPIC", "Val": "dwd_frontier_flow_log_access_log_hi"},
    {"Key": "TIDESCHED_AGENTSCHEDULE_ENABLE", "Val": "true"},
    {"Key": "TIDESCHED_JOBMANAGER_CLUSTER", "Val": "fringedb-newly"},
    {"Key": "TIDESCHED_KAFKASERVICE_ENABLE", "Val": "true"},
    {"Key": "_TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS", "Val": "'[\"CENTER:default\"]'"},
    {"Key": "TIDESCHED_LISTEN_PORT", "Val": "7889"},
    {"Key": "SERVICE_CLUSTER", "Val": "frontier_grpc_phy"},
    {"Key": "TCE_CLUSTER", "Val": "frontier_grpc_phy"},
    {"Key": "TIDESCHED_KAFKASERVICE_LISTENPORT", "Val": "9996"},
    {"Key": "TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE", "Val": "true"},
    {"Key": "TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL", "Val": "http://tidesched-http.byted.org/tlblog-mq/services"},
    {"Key": "TIDESCHED_KAFKASERVICE_TTGW", "Val": "[fdbd:dc02:fe:2281::1]:8900"},
    {"Key": "ENV", "Val": "tce.cn"},
    {"Key": "TIDESCHED_DEBUG_LISTEN_PORT", "Val": "8794"}
  ],
  "ConsulInfo": {
    "PSM": "_data.systi.tidesched",
    "Env": "cn"
  },
  "Ports": 1,
  "Command": "./bin/data.systi.tidesched",
  "ScheduleStrategy": "BROADCAST"
}
```

## 运行态结论

这份启动配置会把进程启动成一个开启 Kafka Metadata 调度代理的 scheduler。

关键行为如下：

- 主调度服务监听 `:7889`，承载 HTTP/1 和 gRPC/HTTP2。
- Debug/pprof 服务监听 `:8794`。
- KafkaService 被开启，监听 Kafka 二进制协议端口 `:9996`。
- KafkaService 使用远端转发模式，不在本进程内做本地 topic group 调度。
- Kafka Metadata 请求会被转发到 `http://tidesched-http.byted.org/tlblog-mq/services` 获取 topic 对应后端地址。
- 对 edge 类型请求，Metadata 响应中的 NodeID `0` 会返回 `TTGW=[fdbd:dc02:fe:2281::1]:8900`。
- 集群名解析优先使用 `SERVICE_CLUSTER=frontier_grpc_phy`，`TCE_CLUSTER` 同值作为兜底。
- `TIDESCHED_KAFKASERVICE_DEFAULT_TOPIC` 在当前提交源码中没有直接读取，不能确认对 KafkaService 生效。
- `_TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS` 因为变量名前多了 `_`，当前源码不会读取；源码读取的是 `TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS`。

## 端口映射

```text
process ./bin/data.systi.tidesched
  |
  +-- main scheduler port :7889
  |     |
  |     +-- HTTP/1 APIs
  |     +-- gRPC/HTTP2 APIs
  |     +-- /{cluster}/config
  |     +-- /{cluster}/metrics/*
  |     +-- /services when agent schedule enabled
  |
  +-- debug port :8794
  |     |
  |     +-- net/http/pprof
  |
  +-- kafka protocol port :9996
        |
        +-- Kafka ApiVersions
        +-- Kafka Metadata
        +-- remote metadata scheduling
```

## Kafka 调度流程中的变量作用

```text
Kafka client
  |
  | connect scheduler:9996
  v
KafkaService
  |
  | enabled by TIDESCHED_KAFKASERVICE_ENABLE=true
  | listen port from TIDESCHED_KAFKASERVICE_LISTENPORT=9996
  v
Conn.RoundTrip()
  |
  +-- ApiVersions
  |     |
  |     +-- returns supported API versions
  |
  +-- Metadata(topic list)
        |
        +-- determine clientHost
        +-- collect requested topics
        |
        v
   ForwardToRemote=true
        |
        | GET TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL
        |   ?topics=<topics>
        |   &request_ip=<clientHost>
        v
   remote /tlblog-mq/services
        |
        +-- returns topic -> backend broker addr
        +-- returns requested_cloudtype
        |
        v
   build Metadata response
        |
        +-- if requested_cloudtype=edge
        |      NodeID 0 = TIDESCHED_KAFKASERVICE_TTGW
        |
        +-- NodeID 1..N = remote returned backend brokers
        |
        +-- topic partitions leader = selected backend NodeID
        |
        v
Kafka client connects returned backend broker
```

## 环境变量逐项解析

| 变量 | 输入值 | 当前源码是否直接读取 | 作用 |
| --- | --- | --- | --- |
| `TIDESCHED_AGENTSCHEDULE_ENABLE` | `true` | 是 | 启用 HTTP AgentScheduler，注册 `/services` 与 `/{cluster}/services`。该变量重复出现两次，值一致 |
| `TIDESCHED_MEMORY_OF_MACHINE_FILTER_ENABLE` | `false` | 是 | 关闭内存使用率过滤器，调度时不会因为内存使用率超过阈值过滤节点 |
| `TIDESCHED_DEBUG_LISTEN_ENABLE` | `true` | 是 | 启动 debug/pprof HTTP 服务 |
| `TIDESCHED_MEMORY_OF_MACHINE_THRESHOLD` | `0.65` | 是 | 设置内存过滤阈值，但由于 `TIDESCHED_MEMORY_OF_MACHINE_FILTER_ENABLE=false`，该阈值在本次启动中基本不参与过滤 |
| `TIDESCHED_IP_LIBRARY_READER_NAME` | `bytedance-cdn-schedule` | 是 | 指定 IP 库读取器，主要服务于网络匹配/路由能力；本次 Kafka 远端转发模式下不直接做本地 Kafka backend 选择 |
| `TIDESCHED_ORGANIZATION_NET_READER_LIST` | `'[["bytedance-oort","false"]]'` | 是 | 配置组织网络读取器列表；但 `TIDESCHED_AGENTSCHED_ENABLE_NETROUTE=false` 时主流程不会初始化 OrganizationNetReaderManager |
| `TIDESCHED_AGENTSCHED_ENABLE_NETROUTE` | `false` | 是 | 关闭 AgentSchedule 的网络路由；对 Kafka 本地模式会影响 `AgentNetRoute`，但本次 `ForwardToRemote=true`，本地 Kafka 调度被绕过 |
| `TIDESCHED_KAFKASERVICE_DEFAULT_TOPIC` | `dwd_frontier_flow_log_access_log_hi` | 未发现 | 当前提交源码没有读取该变量，不能确认生效；如果运行包有额外补丁才可能使用 |
| `TIDESCHED_JOBMANAGER_CLUSTER` | `fringedb-newly` | 是 | 覆盖 JobManager 请求使用的 cluster，用于拉取对应集群 DAG |
| `TIDESCHED_KAFKASERVICE_ENABLE` | `true` | 是 | 开启 KafkaService 独立 TCP listener |
| `_TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS` | `'["CENTER:default"]'` | 否 | 当前源码读取的是无前导下划线的 `TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS`，因此这个输入不会生效 |
| `TIDESCHED_LISTEN_PORT` | `7889` | 是 | 主调度服务端口，生成 `Service.ListenAddr()`，HTTP/gRPC 监听 `:7889` |
| `SERVICE_CLUSTER` | `frontier_grpc_phy` | 是 | 集群名优先来源，影响 HTTP 路径前缀、日志、默认 job cluster 替换等 |
| `TCE_CLUSTER` | `frontier_grpc_phy` | 是 | 集群名兜底来源；由于 `SERVICE_CLUSTER` 已存在，本次 `GetClusterName()` 使用 `SERVICE_CLUSTER` |
| `TIDESCHED_KAFKASERVICE_LISTENPORT` | `9996` | 是 | KafkaService 监听端口 |
| `TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE` | `true` | 是 | 开启 KafkaService 远端转发模式 |
| `TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL` | `http://tidesched-http.byted.org/tlblog-mq/services` | 是 | Metadata 请求的远端调度查询地址 |
| `TIDESCHED_KAFKASERVICE_TTGW` | `[fdbd:dc02:fe:2281::1]:8900` | 是 | Metadata 响应中返回给 edge client 的 NodeID 0 broker 地址 |
| `ENV` | `tce.cn` | 是 | 选择配置文件 `conf/config.tce.cn.yaml` |
| `TIDESCHED_DEBUG_LISTEN_PORT` | `8794` | 是 | debug/pprof 监听端口 |

## 非 Env 字段对运行的影响

| 字段 | 输入值 | 作用 |
| --- | --- | --- |
| `Command` | `./bin/data.systi.tidesched` | 进程启动命令 |
| `ClusterVersion` | `frontier_phy_scheduler_allinone_1.0.0.646_newport` | 部署包版本标识；需要注意它可能不完全等同当前源码提交 |
| `ConsulInfo.PSM` | `_data.systi.tidesched` | 服务发现注册标识，影响外部访问该 scheduler 的发现路径 |
| `ConsulInfo.Env` | `cn` | 服务发现环境 |
| `ScheduleStrategy` | `BROADCAST` | 调度平台层部署策略，不是 scheduler 内部任务调度策略 |
| `Ports` | `1` | 平台暴露端口数量；但进程内部实际还会监听 debug 和 Kafka 端口，是否对外可达取决于平台网络配置 |

## 对 Kafka Metadata 的实际影响

### 本次配置下的实际请求路径

```text
Kafka client
  |
  v
[scheduler KafkaService] :9996
  |
  +-- TIDESCHED_KAFKASERVICE_ENABLE=true
  +-- TIDESCHED_KAFKASERVICE_LISTENPORT=9996
  |
  v
Metadata request
  |
  +-- requested topics from Kafka client
  +-- clientHost from tide_fip_* topic or remote addr
  |
  v
ForwardToRemote=true
  |
  v
GET http://tidesched-http.byted.org/tlblog-mq/services
    ?topics=<requested_topics>
    &request_ip=<clientHost>
  |
  v
remote service returns:
  |
  +-- addrs: topic -> backend broker addr
  +-- requested_cloudtype: edge / center
  |
  v
Kafka Metadata response
  |
  +-- NodeID 0 = [fdbd:dc02:fe:2281::1]:8900 if edge
  +-- NodeID 1..N = backend broker addr from remote
  +-- partition leader = backend NodeID
```

### 被绕过的本地调度能力

由于 `TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE=true`，以下本地 Kafka 调度能力不会作为主路径执行：

- 从 DAG 的 `source.kafka_protocol` 构建本地 topic group 后做本地选择。
- 使用 `accesschedule.Group.UpstreamAccessToAnOptimalDowmstream()` 选择后端。
- 使用 `TIDESCHED_AGENTSCHED_ENABLE_NETROUTE` 控制的本地 `AgentNetRoute` 做 Kafka backend 路由。
- 使用本地 `TIDESCHED_IP_LIBRARY_READER_NAME` / `TIDESCHED_ORGANIZATION_NET_READER_LIST` 对 Kafka backend 做本地网络匹配。

这些能力仍可能影响 HTTP AgentScheduler 或远端服务自身，但对当前进程的 Kafka Metadata 主链路不是直接路径。

## 配置加载顺序

```text
ENV=tce.cn
  |
  v
load conf/config.tce.cn.yaml
  |
  v
StaticConfig.CheckAndReplace()
  |
  +-- Service.TryReplaceFromEnv()
  |     +-- TIDESCHED_LISTEN_PORT=7889
  |     +-- TIDESCHED_DEBUG_LISTEN_PORT=8794
  |
  +-- AgentSchedule.TryReplaceFromEnv()
  |     +-- TIDESCHED_AGENTSCHED_ENABLE_NETROUTE=false
  |
  +-- Schedule.TryReplaceFromEnv()
  |     +-- TIDESCHED_IP_LIBRARY_READER_NAME=bytedance-cdn-schedule
  |
  +-- PolicyProps.TryReplaceFromEnv()
  |     +-- TIDESCHED_MEMORY_OF_MACHINE_FILTER_ENABLE=false
  |     +-- TIDESCHED_MEMORY_OF_MACHINE_THRESHOLD=0.65
  |
  +-- JobManager.TryReplaceFromEnv()
  |     +-- TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly
  |
  +-- KafkaSerivce.TryReplaceFromEnv()
        +-- TIDESCHED_KAFKASERVICE_ENABLE=true
        +-- TIDESCHED_KAFKASERVICE_LISTENPORT=9996
        +-- TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE=true
        +-- TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL=...
        +-- TIDESCHED_KAFKASERVICE_TTGW=...
```

## 变量依赖图

```text
ENV=tce.cn
  |
  v
conf/config.tce.cn.yaml
  |
  v
runtime env overrides
  |
  +-- TIDESCHED_LISTEN_PORT=7889
  |      |
  |      v
  |   main HTTP/gRPC listener
  |
  +-- TIDESCHED_DEBUG_LISTEN_ENABLE=true
  +-- TIDESCHED_DEBUG_LISTEN_PORT=8794
  |      |
  |      v
  |   pprof/debug listener
  |
  +-- TIDESCHED_JOBMANAGER_CLUSTER=fringedb-newly
  |      |
  |      v
  |   DAG source for scheduler and Kafka topic cache
  |
  +-- TIDESCHED_KAFKASERVICE_ENABLE=true
  +-- TIDESCHED_KAFKASERVICE_LISTENPORT=9996
  |      |
  |      v
  |   Kafka protocol listener
  |
  +-- TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE=true
  +-- TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL=...
  |      |
  |      v
  |   remote metadata address resolution
  |
  +-- TIDESCHED_KAFKASERVICE_TTGW=[fdbd:dc02:fe:2281::1]:8900
         |
         v
      edge Metadata broker NodeID 0
```

## 注意事项

1. `TIDESCHED_KAFKASERVICE_DEFAULT_TOPIC` 在当前提交未被源码读取。如果期望 Kafka client 在未指定 topic 时使用 `dwd_frontier_flow_log_access_log_hi`，需要确认部署包 `1.0.0.646_newport` 是否包含额外补丁，或需要在代码中补充读取逻辑。
2. `_TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS` 不会被当前源码识别。如果要限制 resgroup，应使用 `TIDESCHED_AGENTSCHED_ALLOWED_RESGROUPS`。
3. `TIDESCHED_AGENTSCHED_ENABLE_NETROUTE=false` 与 `TIDESCHED_ORGANIZATION_NET_READER_LIST` 同时出现时，组织网络读取器配置不会进入主流程初始化；本次 Kafka 又是远端转发模式，因此本地网络路由对 Kafka Metadata 主链路无直接影响。
4. `TIDESCHED_MEMORY_OF_MACHINE_THRESHOLD=0.65` 只有在 `TIDESCHED_MEMORY_OF_MACHINE_FILTER_ENABLE=true` 时才会参与节点过滤。
5. `Ports=1` 是部署平台字段，不等价于进程内部只监听一个端口；本配置下至少存在主端口、debug 端口和 Kafka 端口三个逻辑监听。

