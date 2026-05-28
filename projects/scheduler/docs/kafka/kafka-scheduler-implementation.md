# Kafka 调度实现与配置分析

## 结论

当前提交 `7f09d11952ef889c61bb2aae820f1e040e012fd5` 中存在独立 Kafka 调度实现，核心代码位于：

- `biz/service/kafkaservice`
- `biz/config/kafkaservice`

该模块不是完整 Kafka broker，也不是 Kafka consumer/producer 业务处理器，而是一个 Kafka Metadata 调度代理：

- 监听独立 Kafka TCP 端口。
- 解析 Kafka 二进制协议请求。
- 响应 `ApiVersions` 和 `Metadata`。
- 在 Metadata 阶段为 Kafka client 返回真实 Kafka server/broker 地址。
- 本地模式下由本进程按 DAG、topic、client IP、网络路由、调度策略选择 broker。
- 远端模式下由远端 HTTP 服务返回 topic 对应的 broker 地址，本进程只负责把结果翻译成 Kafka Metadata 响应。

当前未实现真实 Kafka 数据面：

- 不消费 Kafka 消息。
- 不写入 Kafka 消息。
- 不管理 consumer group。
- 不处理 offset commit/fetch。
- 不处理 fetch 数据流。
- `Produce` 协议结构存在，但连接处理逻辑未处理 `Produce` 请求。

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `biz/service/kafkaservice/service.go` | KafkaService 主体，负责启动 TCP listener、构建 topic 调度 group、监听 DAG 和网络变化 |
| `biz/service/kafkaservice/conn.go` | 单连接协议处理，负责 `ApiVersions`、`Metadata` 请求解析与响应 |
| `biz/service/kafkaservice/protocol` | Kafka 二进制协议编码/解码框架 |
| `biz/service/kafkaservice/protocol/apiversions` | `ApiVersions` 请求/响应结构 |
| `biz/service/kafkaservice/protocol/metadata` | `Metadata` 请求/响应结构 |
| `biz/service/kafkaservice/protocol/produce` | `Produce` 请求/响应结构定义，当前未接入 `RoundTrip` |
| `biz/config/kafkaservice/config.go` | KafkaService 配置、默认值和环境变量覆盖 |
| `biz/service/service/service.go` | 主调度服务启动 Kafka protocol listener 的入口 |
| `conf/config.yaml` | 默认主配置，当前未显式开启 KafkaService |

## 交互边界

Kafka server 与 Kafka scheduler 的关系容易误解：真实 Kafka server/broker 不会主动调用这个 Kafka scheduler。交互发生在 Kafka client 获取 Metadata 的阶段。

```text
+------------------+       Metadata        +---------------------------+
|   Kafka Client   | --------------------> |   Kafka Scheduler        |
| producer/admin   |                       |   KafkaService :9999     |
+------------------+                       +-------------+-------------+
                                                        |
                                                        | resolve topic
                                                        v
                                           +---------------------------+
                                           |   Backend Addr Source     |
                                           | local DAG or remote HTTP  |
                                           +-------------+-------------+
                                                        |
                                                        | broker addr
                                                        v
+------------------+    Produce/Fetch/etc. +---------------------------+
|   Kafka Client   | --------------------> |   Real Kafka Server      |
| after metadata   |                       |   broker host:port       |
+------------------+                       +---------------------------+
```

关键点：

- Kafka scheduler 只参与 `ApiVersions` 和 `Metadata` 阶段。
- Kafka scheduler 在 Metadata 响应中告诉 client：某个 topic/partition 的 leader broker 是哪个地址。
- Kafka client 后续会连接 Metadata 响应里的真实 Kafka server/broker。
- Kafka server/broker 本身不需要感知 scheduler，也不需要回调 scheduler。
- scheduler 当前不转发 `Produce`/`Fetch` 数据流，所以它不在数据面链路中。

## 总体架构

```text
                              +----------------------+
                              |     Kafka Client     |
                              | producer/admin/etc.  |
                              +----------+-----------+
                                         |
                                         | Kafka binary protocol
                                         | ApiVersions / Metadata
                                         v
                              +----------------------+
                              |   Kafka Scheduler    |
                              | KafkaService TCP     |
                              | ListenPort :9999     |
                              +----------+-----------+
                                         |
                                         v
                              +----------------------+
                              |   Conn.RoundTrip     |
                              | decode request       |
                              +-----+-----------+----+
                                    |           |
                         ApiVersions|           |Metadata
                                    v           v
                       +----------------+   +----------------------+
                       | Supported APIs |   | Topic Addr Resolve   |
                       | Response       |   | local or remote      |
                       +----------------+   +----------+-----------+
                                                    |
                                                    v
                                      +----------------------------+
                                      | Backend Broker Address     |
                                      | topic -> host:port         |
                                      +-------------+--------------+
                                                    |
                                                    v
                              +-------------------------------+
                              | Metadata Response             |
                              | brokers + partitions + leader |
                              +---------------+---------------+
                                              |
                                              v
                              +-------------------------------+
                              | Real Kafka Server             |
                              | client connects after metadata|
                              +-------------------------------+
```

## 远端模式解释

远端模式由 `KafkaSerivce.ForwardToRemote=true` 开启。它的含义不是把 Kafka 数据转发到远端 broker，也不是 scheduler 代理 Produce/Fetch，而是把“topic 应该路由到哪个 broker”的决策交给另一个 HTTP 服务。

```text
+------------------+     Metadata(topic=A)      +---------------------------+
|   Kafka Client   | -------------------------> | Local Kafka Scheduler     |
| bootstrap server |                            | KafkaService :9996        |
+------------------+                            +-------------+-------------+
                                                              |
                                                              | HTTP GET
                                                              | ?topics=A
                                                              | &request_ip=clientIP
                                                              v
                                                 +---------------------------+
                                                 | Remote Scheduler Service  |
                                                 | ForwardToRemoteUrl        |
                                                 +-------------+-------------+
                                                              |
                                                              | JSON addrs
                                                              | A -> broker:9092
                                                              v
+------------------+     Metadata response       +---------------------------+
|   Kafka Client   | <------------------------- | Local Kafka Scheduler     |
| receives leader  |                            | builds Kafka Metadata     |
+--------+---------+                            +---------------------------+
         |
         | Produce/Fetch/ListOffsets/etc.
         v
+------------------+
| Real Kafka Server|
| broker:9092      |
+------------------+
```

远端模式下各实体职责：

| 实体 | 职责 |
| --- | --- |
| Kafka client | 把 scheduler 当 bootstrap server，请求 Metadata；拿到 Metadata 后连接真实 broker |
| 本地 Kafka scheduler | 解析 Kafka Metadata 请求，调用远端 HTTP 服务，把远端结果编码成 Kafka Metadata 响应 |
| 远端 HTTP 服务 | 按 `topics` 和 `request_ip` 计算 topic 到 broker 的映射 |
| 真实 Kafka server | 承载 Kafka 数据面，处理 client 后续 `Produce`、`Fetch` 等请求 |

远端 HTTP 请求格式：

```text
+-------------------------+
| Local Kafka Scheduler   |
+------------+------------+
             |
             | GET {ForwardToRemoteUrl}?topics=topic1,topic2&request_ip=clientHost
             v
+-------------------------+
| Remote Scheduler Service|
+-------------------------+
```

期望响应：

```json
{
  "code": 0,
  "err_msg": "success",
  "addr": "host:port",
  "addrs": {
    "topic1": "http://broker1:9092",
    "topic2": "http://broker2:9092"
  },
  "requested_cloudtype": "edge"
}
```

处理逻辑：

```text
+-------------------------+
| response.addrs          |
+------------+------------+
             |
             v
+-------------------------+
| strip http/https prefix |
+------------+------------+
             |
             v
+-------------------------+
| map topic -> brokerAddr |
+------------+------------+
             |
             v
+-------------------------+
| Kafka Metadata Response |
+-------------------------+
```

`requested_cloudtype` 会影响 Metadata 中 NodeID `0` 的返回：

- `edge`：返回配置的 `TTGW`。
- 非 `edge`：返回当前 scheduler 实例 IP + `ListenPort`。

注意：NodeID `0` 更像代理/入口节点，NodeID `1..N` 是各 topic 被调度出的真实 broker 地址。

## 本地模式解释

本地模式由 `KafkaSerivce.ForwardToRemote=false` 开启。此时 scheduler 不调用远端 HTTP 服务，而是在本进程中根据 DAG 构建 topic 到 backend nodes 的调度 group。

```text
+-------------------------+
| JobManager DAG          |
| source.kafka_protocol   |
+------------+------------+
             |
             v
+-------------------------+
| KafkaService            |
| buildSourceGroup()      |
+------------+------------+
             |
             v
+-------------------------+
| topicGroups             |
| topic -> backend nodes  |
+------------+------------+
             |
             v
+-------------------------+
| accesschedule.Group     |
| net route + selector    |
+------------+------------+
             |
             v
+-------------------------+
| Metadata broker leader  |
| selected backend addr   |
+-------------------------+
```

DAG source task 需要包含：

| Operator 字段 | 代码常量 | 说明 |
| --- | --- | --- |
| `OperatorUniqueName` | `source.kafka_protocol` | 标识这是 Kafka 协议 source |
| `OperatorOptions["connector.port"]` | `connector.port` | 真实 Kafka 后端节点端口 |
| `OperatorOptions["connector.topic"]` | `connector.topic` | 该 source 对应的 topic |

构建过程：

```text
+-------------------------+
| JobMapping.GetAll()     |
+------------+------------+
             |
             v
+-------------------------+
| FindJobsOf()            |
| source.kafka_protocol   |
+------------+------------+
             |
             v
+-------------------------+
| Iterate Matched Groups  |
| read topic and port     |
+------------+------------+
             |
             v
+-------------------------+
| Rewrite Node SlotAddr   |
| use connector.port      |
+------------+------------+
             |
             v
+-------------------------+
| topicGroups[topic]      |
| merged backend nodes    |
+-------------------------+
```

Metadata 请求进入后：

```text
+-------------------------+
| Metadata Request        |
| topic names             |
+------------+------------+
             |
             v
+-------------------------+
| Resolve Client Host     |
| tide_fip_* or remote IP |
+------------+------------+
             |
             v
+-------------------------+
| useLocal(client, topic) |
+------------+------------+
             |
             v
+-------------------------+
| accesschedule.Group     |
| select optimal backend  |
+------------+------------+
             |
             v
+-------------------------+
| Selected SlotAddr       |
| broker host:port        |
+-------------------------+
```

## 端口说明

| 端口/地址 | 配置字段 | 默认值 | 协议 | 作用 |
| --- | --- | --- | --- | --- |
| `:9999` | `Static.KafkaSerivce.ListenPort` | `9999` | Kafka TCP binary protocol | Kafka Metadata 调度入口。只在 `KafkaSerivce.Enable=true` 时启动 |
| `KafkaSerivce.TTGW` | `Static.KafkaSerivce.TTGW` | 未配置时自动取本机 IP + `ListenPort` | Metadata advertised broker | 返回给边缘客户端的 NodeID `0` 地址，通常作为 TTGW/代理入口 |
| `:6789` | `Static.Service.ListenPort` | `6789` | HTTP/1 + gRPC/HTTP2 | 主调度服务端口，cmux 分流 HTTP 和 gRPC，不承载 Kafka binary protocol |
| `:16789` | `Static.Service.DebugListenPort` | `16789` | HTTP pprof/debug | debug 端口，由主服务配置生成 |
| `:10000` | `Static.Service.LongConnAddress` | `0.0.0.0:10000` | 长连接预留 | 当前主启动链路未看到 workerlistener 启动，主要是历史/测试链路 |

注意：

- Kafka 协议端口与主调度端口是两个 listener。
- KafkaService 不是通过 `cmux` 挂到 `:6789`，而是在启用后单独 `net.Listen("tcp", :ListenPort)`。
- 字段名在代码中拼写为 `KafkaSerivce`，不是 `KafkaService`。YAML 或配置结构中需要按代码字段名写，否则可能无法生效。

## 启动链路

```text
+-------------------------+
| main.go                 |
+------------+------------+
             |
             v
+-------------------------+
| CreateServiceConfig     |
| load conf/config*.yaml  |
+------------+------------+
             |
             v
+-------------------------+
| StaticConfig            |
| env override + Finish   |
+------------+------------+
             |
             v
+-------------------------+
| grpcservice.NewService  |
| Run()                   |
+------------+------------+
             |
             v
+-------------------------+
| SchedService.start()    |
| init scheduler modules  |
+------------+------------+
             |
             v
+-------------------------+
| KafkaSerivce.Enable?    |
+------------+------------+
             |
             v
+-------------------------+
| KafkaService.Run()      |
| listen Kafka TCP port   |
+------------+------------+
             |
             v
+-------------------------+
| Accept Connections      |
| Conn.RoundTrip()        |
+-------------------------+
```

`KafkaService.Run()` 内部动作：

```text
+-------------------------+
| net.Listen(:ListenPort) |
+------------+------------+
             |
             v
+-------------------------+
| proxyproto.Listener     |
+------------+------------+
             |
             v
+-------------------------+
| subscribe DAG events    |
+------------+------------+
             |
             v
+-------------------------+
| rebuild timer 500ms     |
+------------+------------+
             |
             v
+-------------------------+
| accept conn goroutine   |
+-------------------------+
```

## 配置

代码中的配置结构：

```go
// biz/config/kafkaservice/config.go
type Config struct {
    Enable             bool
    ListenPort         int
    TTGW               string
    ForwardToRemote    bool
    ForwardToRemoteUrl string
}
```

默认值：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `Enable` | `false` | 默认关闭 KafkaService |
| `ListenPort` | `9999` | Kafka 协议监听端口 |
| `TTGW` | 空 | `Finish()` 中自动填为本机 IP + `ListenPort` |
| `ForwardToRemote` | `false` | 默认本地调度 |
| `ForwardToRemoteUrl` | 空 | 远端调度查询 URL |

YAML 示例：

```yaml
KafkaSerivce:
  Enable: true
  ListenPort: 9999
  TTGW: "1.2.3.4:9999"
  ForwardToRemote: true
  ForwardToRemoteUrl: "http://scheduler.example.com/kafka/metadata"
```

环境变量：

| 环境变量 | 配置字段 | 说明 |
| --- | --- | --- |
| `TIDESCHED_KAFKASERVICE_ENABLE` | `Enable` | 是否启动 KafkaService |
| `TIDESCHED_KAFKASERVICE_LISTENPORT` | `ListenPort` | Kafka 协议监听端口 |
| `TIDESCHED_KAFKASERVICE_TTGW` | `TTGW` | Metadata 中返回给边缘客户端的入口地址 |
| `TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE` | `ForwardToRemote` | 是否使用远端调度 |
| `TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL` | `ForwardToRemoteUrl` | 远端调度查询 URL |

## Kafka 协议处理

```text
+-------------------------+
| TCP Connection          |
+------------+------------+
             |
             v
+-------------------------+
| Conn.RoundTrip()        |
+------------+------------+
             |
             v
+-------------------------+
| protocol.ReadRequest()  |
| apiKey/apiVersion/body  |
+------------+------------+
             |
             v
+-------------------------+
| switch ApiKey           |
+------+------+-----------+
       |      |
       |      |
       v      v
+------------+  +-------------------------+
| ApiVersions|  | Metadata                |
| Response   |  | handleMetadata()        |
+------------+  +------------+------------+
                            |
                            v
               +-------------------------+
               | protocol.WriteResponse()|
               +-------------------------+
```

`handleApiVersions()` 返回支持列表：

```text
+-------------------------+
| ApiVersions Response    |
+------------+------------+
             |
             +-- ApiVersions
             +-- Metadata
             +-- Produce
```

注意：

- `RoundTrip()` 实际只处理 `ApiVersions` 和 `Metadata`。
- `Produce` 虽在响应中声明，但当前连接处理没有 `case proto.Produce`。
- `produce` 包也未在 `conn.go` 中导入注册，因此真实 Produce 请求不应视为可用能力。

## Metadata 处理

```text
+-------------------------+
| Metadata Request        |
| topic names             |
+------------+------------+
             |
             v
+-------------------------+
| Parse Topic Names       |
| normal / tide_fip_*     |
+------------+------------+
             |
             v
+-------------------------+
| Deduplicate Topics      |
+------------+------------+
             |
             v
+-------------------------+
| Resolve Backend Addr    |
| local or remote         |
+------------+------------+
             |
             v
+-------------------------+
| Build Brokers           |
| NodeID 0, NodeID 1..N   |
+------------+------------+
             |
             v
+-------------------------+
| Build Partitions        |
| leader = backend node   |
+------------+------------+
             |
             v
+-------------------------+
| Metadata Response       |
+-------------------------+
```

Metadata 返回示意：

```text
+-------------------------+
| Kafka Client            |
| asks topic foo          |
+------------+------------+
             |
             v
+-------------------------+
| Kafka Scheduler         |
| selects 10.1.2.3:9092   |
+------------+------------+
             |
             v
+-------------------------+
| Metadata Response       |
| brokers and partitions  |
+------------+------------+
             |
             +-- Broker NodeID 0 = TTGW or scheduler ip:9999
             +-- Broker NodeID 1 = 10.1.2.3:9092
             +-- Topic foo partition 0 leader = NodeID 1
             +-- Topic foo partition 1 leader = NodeID 1
```

## Partition 数量

`Topic.PartitionNum` 来自 `getPartitionNumForTM(nodes)`。

```text
+-------------------------+
| Nodes Of Topic          |
+------------+------------+
             |
             v
+-------------------------+
| Pick First SlotAddr     |
+------------+------------+
             |
             v
+-------------------------+
| Count Same SlotAddr     |
+------------+------------+
             |
             v
+-------------------------+
| PartitionNum            |
+-------------------------+
```

这不是从真实 Kafka broker 查询 partition 数量，而是基于 DAG 节点分布推导。该逻辑需要结合 DAG 中 source task 的建模方式理解。

## 重建机制

KafkaService 会在以下事件发生时标记重建：

- DAG 变化：`DagReaderEventDAGChanged`
- 组织网络变化：`OrganizationNetReaderEventChanged`
- 服务启动初始重建

```text
+-------------------------+
| DAG or Network Event    |
+------------+------------+
             |
             v
+-------------------------+
| KafkaService.Rebuild()  |
| rebuild = true          |
+------------+------------+
             |
             v
+-------------------------+
| Timer Every 500ms       |
+------------+------------+
             |
             v
+-------------------------+
| buildSourceGroup()      |
+------------+------------+
             |
             v
+-------------------------+
| Replace Topics/Groups   |
| atomic and lock guarded |
+-------------------------+
```

连接处理时，每一轮 `RoundTrip()` 前都会刷新 topic 快照：

```text
+-------------------------+
| Connection Loop         |
+------------+------------+
             |
             v
+-------------------------+
| CopyTopic()             |
+------------+------------+
             |
             v
+-------------------------+
| Conn.RoundTrip()        |
+-------------------------+
```

## 与主调度服务的关系

```text
+-------------------------+
| Main Scheduler Process  |
+------------+------------+
             |
     +-------+-------+
     |               |
     v               v
+------------+  +-------------------------+
| :6789      |  | :9999                   |
| HTTP/gRPC  |  | KafkaService            |
+------------+  | Kafka binary protocol   |
                +-------------------------+
```

KafkaService 复用主调度上下文中的能力：

- `JobMapping`
- `DagReader`
- `OrganizationNetReaderManager`
- `GetAddr`
- `AgentSchedule` 配置
- `NetRoute`
- `schedule.Group`

## 典型请求流程

```text
+-------------------------+
| 1. Kafka Client         |
| connect scheduler:9999  |
+------------+------------+
             |
             v
+-------------------------+
| 2. ApiVersions Request  |
+------------+------------+
             |
             v
+-------------------------+
| 3. ApiVersions Response |
+------------+------------+
             |
             v
+-------------------------+
| 4. Metadata Request     |
| topic=foo               |
+------------+------------+
             |
             v
+-------------------------+
| 5. Resolve Backend      |
| local or remote         |
+------------+------------+
             |
             v
+-------------------------+
| 6. Metadata Response    |
| leader broker selected  |
+------------+------------+
             |
             v
+-------------------------+
| 7. Kafka Client         |
| connects real broker    |
+-------------------------+
```

## 限制与风险

| 类型 | 说明 |
| --- | --- |
| 数据面未实现 | 不处理真正的 `Produce`、`Fetch`、offset、consumer group 协议 |
| Produce 声明不一致 | `ApiVersions` 响应中包含 `Produce`，但 `RoundTrip()` 没有处理 `Produce` |
| 配置字段拼写 | 代码字段是 `KafkaSerivce`，存在拼写错误但必须按代码使用 |
| 默认关闭 | `conf/config.yaml` 未开启 KafkaService，默认不会监听 `:9999` |
| topic 来源依赖 DAG | 本地模式下，没有 DAG 中的 `source.kafka_protocol`，Metadata 不会得到有效 topic |
| 远端服务依赖 | 远端模式下，本地 DAG topic group 不是主决策来源，依赖 `ForwardToRemoteUrl` 返回正确 `addrs` |
| partition 数量非 broker 查询 | 本地模式下 partition 数量由 DAG 节点推导，不是查询真实 Kafka 集群 |
| Replica/ISR 固定 | `ReplicaNodes` 和 `IsrNodes` 固定为 `[0, 1]`，偏代理语义 |
| 远端转发依赖 HTTP 契约 | `ForwardToRemoteUrl` 响应字段必须符合代码预期 |

## 运维排查建议

检查 KafkaService 是否启用：

```bash
echo "$TIDESCHED_KAFKASERVICE_ENABLE"
```

检查监听端口：

```bash
echo "$TIDESCHED_KAFKASERVICE_LISTENPORT"
```

检查 TTGW：

```bash
echo "$TIDESCHED_KAFKASERVICE_TTGW"
```

检查远端模式：

```bash
echo "$TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE"
echo "$TIDESCHED_KAFKASERVICE_FORWARD_TO_REMOTE_URL"
```

确认本地模式 DAG 是否包含 Kafka source：

```text
+-------------------------+
| OperatorUniqueName      |
| source.kafka_protocol   |
+-------------------------+
| OperatorOptions         |
| connector.topic         |
| connector.port          |
+-------------------------+
```
