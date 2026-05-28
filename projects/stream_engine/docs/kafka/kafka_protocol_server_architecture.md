# Kafka Protocol Server Architecture

本文梳理 `connector.type = kafka_protocol` 的整体架构。它不是连接外部 Kafka broker 的 source，而是在 Tide worker 内部实现了一套 Kafka server 协议，让 Kafka client 可以把数据 produce 到 Tide 任务。

核心角色分成两层：

- Kafka scheduler process：独立进程，通常作为 Kafka client 的 `bootstrap.servers`，负责返回 metadata 和调度后的 broker 地址。
- Tide worker process：运行 `KafkaProtocolSource`，内嵌 `kafka::server`，负责接收真正的 `ProduceRequest` 并把数据送入 Tide DAG。

## 一句话模型

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | MetadataRequest
        v
+-------------------------+
| Kafka scheduler process |
+-------------------------+
        |
        | MetadataResponse: advertised worker/VIP broker
        v
+----------------+
| Kafka client   |
+----------------+
        |
        | ProduceRequest
        v
+-----------------------------------------+
| Tide worker 内嵌 Kafka protocol server |
+-----------------------------------------+
        |
        | decode / WAL / queue
        v
+------------------------------+
| KafkaProtocolSource operator |
+------------------------------+
        |
        | arrow::Table 或 BinaryArray
        v
+----------------+
| 下游 Tide DAG |
+----------------+
```

## 角色关系

```text
+-------------------------------+       MetadataRequest        +------------------------------+
| Kafka client                  |----------------------------->| Kafka scheduler process      |
| bootstrap.servers = scheduler |                              | independent process          |
+-------------------------------+                              +------------------------------+
        ^                                                              |
        |                                                              | MetadataResponse:
        |                                                              | advertised worker/VIP broker
        |                                                              v
        |                                                +------------------------------+
        |                                                | Kafka client                 |
        |                                                | update broker connection     |
        |                                                +------------------------------+
        |
        | ProduceRequest to advertised broker
        v
+---------------------------------------------------------------+
| Tide worker process                                           |
|                                                               |
|  +-------------------+       register/drain      +----------+ |
|  | kafka::server     |<------------------------->| Source   | |
|  | listen + protocol |                           | operator | |
|  +-------------------+                           +----------+ |
|            |                                                  |
|            | route ntp(topic, partition)                      |
|            v                                                  |
|  +-------------------+      lookup/status       +-----------+ |
|  | partition_manager |------------------------->| TideBridge| |
|  +-------------------+                          +-----------+ |
|            |                                           |      |
|            | partition object                          | owns |
|            v                                           v      |
|  +-------------------+                         +------------+ |
|  | DataPartition     |<------------------------| TopicInfo  | |
|  | decoder/WAL/ring  |        partition list   | status     | |
|  +-------------------+                         +------------+ |
|                                                               |
+---------------------------------------------------------------+
```

边界说明：

- `Kafka scheduler process` 是另一个独立进程，不在 Tide worker 内部。
- 正常写入链路中，Kafka client 的 `bootstrap.servers` 指向 scheduler，先从 scheduler 拿 metadata。
- Tide worker 内部的 `kafka::server` 只是在配置了 `kafka.schedule.host/port` 且 client 连接到 worker 时，把 Kafka `MetadataRequest` 通过 TCP 转发给 scheduler。
- 真正的 `ProduceRequest` 不发给 scheduler，而是进入 metadata 返回的 broker 地址，可能是某个 worker，也可能是 VIP / L4 地址。

## 启动与注册流程

`KafkaProtocolSource::Init()` 负责创建或复用内嵌 Kafka server，并注册 topic / partition。

```text
+-----------+
| Task init |
+-----------+
      |
      v
+--------------------------------------------+
| KafkaProtocolSource::Create(options,schema) |
| read topic/port/thread/format               |
| create decoder by format.type               |
+--------------------------------------------+
      |
      v
+-----------------------------+
| KafkaProtocolSource::Init() |
+-----------------------------+
      |
      v
+---------------------------------------------+
| KafkaServer::NewOrGet(jobId, config, props) |
| same port: reuse server                     |
| new port : create kafka::server + listen    |
+---------------------------------------------+
      |
      v
+------------------------------------------------+
| KafkaServer::RegisterTopic(topic,decoder,para) |
+------------------------------------------------+
      |
      v
+---------------------------------------------+
| TideBridge::RegisterTopic(topic, TopicInfo) |
+---------------------------------------------+
      |
      v
+---------------------------------------+
| KafkaServer::RegisterPartition(topic) |
+---------------------------------------+
      |
      v
+--------------------------------+
| TopicInfo::RegisterPartition() |
+--------------------------------+
      |
      v
+---------------------------------------------+
| DataPartition::Create()                     |
| clone decoders / create ringbuffer / WAL    |
+---------------------------------------------+
      |
      v
+-----------------------+
| DataPartition::init() |
+-----------------------+
```

关键点：

- `KafkaServer` 按 `connector.port` 复用。
- `TopicInfo` 保存 topic 的 decoder、partition 列表和状态。
- `DataPartition` 是 Kafka produce 到 Tide source 之间的核心缓冲层。
- `format.type` 决定 produce payload 如何解析成 Arrow table。

## Metadata Flow

Kafka client 生产前会先问 metadata。metadata 的返回决定 client 后续 produce 连接哪个 broker 地址，以及 topic 有哪些 partition。

正常链路中，client 直接把 `MetadataRequest` 发给 Kafka scheduler，因为 sink 侧 `kafka.bootstrap.servers` 配的是 scheduler 地址。

```text
+-------------------------------+
| Kafka client                  |
| bootstrap.servers = scheduler |
+-------------------------------+
        |
        | MetadataRequest(topic)
        v
+--------------------------+
| Kafka scheduler process |
| independent process     |
+--------------------------+
        |
        | MetadataResponse:
        | broker = selected worker/VIP
        | topic partitions and leaders
        v
+-------------------------------+
| Kafka client                  |
| refresh metadata cache        |
+-------------------------------+
        |
        | ProduceRequest to advertised broker
        v
+-------------------------------+
| Tide worker kafka::server    |
| data plane                   |
+-------------------------------+
```

worker 侧代码里还有一条 metadata 转发路径：如果 client 直接连到了 worker，且 worker 配了 `kafka.schedule.host/port`，worker 会把 metadata 请求转发给 scheduler。

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | MetadataRequest(topic)
        v
+----------------+
| kafka::server  |
+----------------+
        |
        v
+----------------------------+
| metadata_handler::handle() |
+----------------------------+
        |
        +-----------------------------+
        |                             |
        | kafka.schedule.host is set  | no kafka.schedule.host
        v                             v
+----------------------------+  +------------------------------+
| Tide worker forward client |  | Local metadata path          |
| MetadataRequest over TCP   |  | check kafka.metadata.enable  |
+----------------------------+  +------------------------------+
        |                                      |
        | kafka.schedule.host:port             | false
        v                                      v
+----------------------------+  +------------------------------+
| Kafka scheduler process    |  | ErrorResponse                 |
| build broker metadata      |  | unknown_server_error         |
+----------------------------+  +------------------------------+
        |                                      ^
        | MetadataResponse                     |
        v                                      | true
+-----------------------------+  +------------------------------+
| Tide worker relay response |  | Choose advertised broker     |
+-----------------------------+  | ttgw hostport or local IP    |
        |                        +------------------------------+
        |                                      |
        |                                      v
        |                        +------------------------------+
        |                        | TideBridge topic partitions  |
        |                        +------------------------------+
        |                                      |
        +------------------------+-------------+
                                 |
                                 v
                   +-----------------------------------------+
                   | MetadataResponse(brokers/topics/parts)  |
                   +-----------------------------------------+
```

地址选择可以理解为 Kafka 的 advertised listener：

```text
client bootstrap 地址 != metadata 返回给 client 的 broker 地址
```

如果 client 通过 VIP / 四层 / 网关访问，必须让 metadata 返回 client 可达的地址，例如：

```sql
'kafka.metadata.enable' = 'true',
'kafka.ttgw.hostport' = 'frontier-vip.example.com:9951'
```

## Worker 与 Scheduler 交互

这里需要同时看两个仓库：

- Tide worker 仓库：`src/source/mq/kafka/server/handlers/metadata.cpp`
- Kafka scheduler 仓库：`biz/service/kafkaservice/conn.go` 和 `biz/service/kafkaservice/service.go`

### 1. Scheduler 作为 Producer Bootstrap 入口

你贴的 sink 配置属于这个路径：producer 的 `kafka.bootstrap.servers` 是 scheduler 地址。

```text
+-------------------------------+
| Kafka producer                |
| bootstrap.servers=scheduler   |
+-------------------------------+
        |
        | ApiVersions / MetadataRequest
        v
+-------------------------------+
| Kafka scheduler process       |
| KafkaService.ListenPort       |
+-------------------------------+
        |
        | net.Listen + proxyproto
        v
+-------------------------------+
| kafkaservice.handleConn()     |
| per TCP connection goroutine  |
+-------------------------------+
        |
        | protocol.ReadRequest()
        v
+-------------------------------+
| Conn.RoundTrip()              |
| ApiVersions or Metadata only  |
+-------------------------------+
        |
        | Metadata -> handleMetadata()
        v
+-------------------------------+
| Resolve clientHost + topics   |
+-------------------------------+
        |
        | schedule topic -> backend addr
        v
+-------------------------------+
| Build MetadataResponse        |
| brokers + partitions + leader |
+-------------------------------+
        |
        | protocol.WriteResponse()
        v
+-------------------------------+
| Kafka producer metadata cache |
+-------------------------------+
        |
        | ProduceRequest to leader broker
        v
+-------------------------------+
| Tide worker kafka::server     |
+-------------------------------+
```

Scheduler 这条链路只负责 Kafka metadata，不处理 produce 数据。

### 2. Worker 转发 Metadata 到 Scheduler

worker 侧还支持一条兼容路径：如果 producer bootstrap 到 worker，并且 worker 配了 `kafka.schedule.host/port`，worker 会把 metadata request 转发给 scheduler。

```text
+-------------------------------+
| Tide worker kafka::server     |
| metadata_handler::handle()    |
+-------------------------------+
        |
        | if kafka.schedule.host is set
        v
+-------------------------------+
| Decode client MetadataRequest |
+-------------------------------+
        |
        | append internal topic:
        | tide_fip_<real_client_host>
        v
+-------------------------------+
| Open TCP connection           |
| kafka.schedule.host:port      |
+-------------------------------+
        |
        | Send original Kafka request header
        | + rewritten MetadataRequest body
        v
+-------------------------------+
| Kafka scheduler process       |
| build MetadataResponse        |
+-------------------------------+
        |
        | Response bytes
        v
+-------------------------------+
| connection_forward            |
| relay response to client      |
+-------------------------------+
```

代码级行为：

- `metadata_handler::handle()` 发现 `kafka.schedule.host` 非空，就走 `forward_tcp()`。
- `rewrite_request()` 会在 metadata request 的 topics 里追加一个内部 topic：`tide_fip_<real_client_host>`。
- `forward_tcp()` 使用 `kafka.schedule.host` / `kafka.schedule.port` 建 TCP 连接到 scheduler。
- `connection_forward` 读取 scheduler 的 response，并把 response 原样转回原 client 连接。
- 如果 forward 失败且配置了 `kafka.schedule.node0.hostport` / `kafka.schedule.node1.hostport`，worker 会构造一个 fallback metadata response。

注意：

- 这条链路只处理 `MetadataRequest`。
- `ProduceRequest` 不会被 worker 转发给 scheduler。
- Scheduler 在 metadata 里返回的 broker 地址，才决定 client 后续把 produce 发到哪里。

### 3. Scheduler 如何识别 Client 和 Topic

```text
+-------------------------------+
| Conn.handleMetadata()         |
| input: metadata.Request       |
+-------------------------------+
        |
        v
+-------------------------------+
| Iterate request.TopicNames    |
+-------------------------------+
        |
        +-------------------------------------------+
        |                                           |
        | Has topic "tide_fip_<real_client_host>"   | Normal topic name
        v                                           v
+-------------------------------+       +-------------------------------+
| clientHost =                  |       | Lookup in c.topics            |
| strings.TrimPrefix(tide_fip_) |       | scheduler known topic table   |
+-------------------------------+       +-------------------------------+
        |                                           |
        |                                           +----------------------+
        |                                           |                      |
        |                                           | found                | not found
        |                                           v                      v
        |                               +-------------------------------+ +-------------------------------+
        |                               | append to requested topics    | | ignore unknown topic name     |
        |                               +-------------------------------+ +-------------------------------+
        |                                           |
        +---------------------------+---------------+
                                    |
                                    v
                    +-------------------------------+
                    | clientHost still empty?       |
                    +-------------------------------+
                                    |
                    +---------------+---------------+
                    |                               |
                    | yes                           | no
                    v                               v
        +-------------------------------+   +-------------------------------+
        | clientHost = getOriginalIP(   |   | keep tide_fip_ clientHost    |
        | conn.RemoteAddr())            |   | from worker forward path      |
        +-------------------------------+   +-------------------------------+
                    |                               |
                    +---------------+---------------+
                                    |
                                    v
                    +-------------------------------+
                    | requested topics empty?       |
                    +-------------------------------+
                                    |
                    +---------------+---------------+
                    |                               |
                    | yes                           | no
                    v                               v
        +-------------------------------+   +-------------------------------+
        | topics = c.topics             |   | topics = requested topics     |
        | list all known topics         |   | after DeduplicationTopic()    |
        +-------------------------------+   +-------------------------------+
                    |                               |
                    +---------------+---------------+
                                    |
                                    v
                    +-------------------------------+
                    | Output to scheduler route     |
                    | clientHost + topics           |
                    +-------------------------------+
```

关键点：

- worker 转发 metadata 时追加 `tide_fip_<real_client_host>`，让 scheduler 用真实 client IP 做调度。
- producer 直接连 scheduler 时，scheduler 通过 `conn.RemoteAddr()` 或 proxy protocol 后的 remote addr 获取 client IP。
- scheduler 只保留已在 `c.topics` 中存在的业务 topic；未知 topic 最终会拿不到 backend addr。

### 4. Scheduler 如何得到 Topic 和 Worker 地址

Scheduler 后台会从 DAG / job mapping 中构建 topic 表和本地调度组。

```text
+-------------------------------+
| KafkaService.Run()            |
| rebuild timer every 500ms     |
+-------------------------------+
        |
        | if DAG/network changed
        v
+-------------------------------+
| KafkaService.buildSourceGroup |
+-------------------------------+
        |
        v
+-------------------------------+
| JobMapping.GetAll()           |
| all job groups                |
+-------------------------------+
        |
        | FindJobsOf(source.kafka_protocol,
        | connector.port)
        v
+-------------------------------+
| Iterate matched job groups    |
| Copy subtasks + task nodes    |
+-------------------------------+
        |
        v
+-------------------------------+
| Find source subtask           |
| OperatorUniqueName =          |
| source.kafka_protocol         |
+-------------------------------+
        |
        v
+-------------------------------+
| Read source options           |
| connector.topic = topic       |
| connector.port  = kafka port  |
+-------------------------------+
        |
        v
+-------------------------------+
| Calculate PartitionNum        |
| getPartitionNumForTM(nodes)   |
+-------------------------------+
        |
        v
+-------------------------------+
| Append scheduler topic table  |
| Topic{Name, PartitionNum}     |
+-------------------------------+
        |
        v
+-------------------------------+
| For each source task node     |
+-------------------------------+
        |
        v
+-------------------------------+
| Clone node                    |
| UpdateNodeAddrs(":"+port)     |
| SlotAddr -> host:connector.port|
+-------------------------------+
        |
        v
+-------------------------------+
| Dedup by rewritten SlotAddr   |
| one backend per worker addr   |
+-------------------------------+
        |
        v
+-------------------------------+
| Merge nodes by topic          |
| topicGroups[topic].nodesMerge |
+-------------------------------+
        |
        +------------------------------------------+
        |                                          |
        | ForwardToRemote=false                    | ForwardToRemote=true
        v                                          v
+-------------------------------+      +-------------------------------+
| Build accesschedule.Group     |      | Skip local group build        |
| per topic                     |      | remote service will route     |
+-------------------------------+      +-------------------------------+
        |                                          |
        +----------------------+-------------------+
                               |
                               v
             +--------------------------------+
             | Store scheduler runtime state  |
             | k.topics = topic table         |
             | k.groups = local route groups  |
             +--------------------------------+
```

关键点：

- topic 来源是 worker DAG 中的 `source.kafka_protocol` operator。
- broker 端口来自 source option 的 `connector.port`。
- scheduler 会把 worker task node 的 `SlotAddr` 改写成 `host:connector.port`。
- `PartitionNum` 由同一 slot addr 上的 source node 数量推导，不是向 worker 查询 Kafka partition。

### 5. Scheduler 如何选择 Backend Broker

```text
+-------------------------------+
| handleMetadata(client,topics) |
+-------------------------------+
        |
        +-----------------------------+
        |                             |
        | ForwardToRemote=true        | ForwardToRemote=false
        v                             v
+-------------------------------+  +-------------------------------+
| HTTP GET ForwardToRemoteUrl   |  | useLocal(clientHost, topic)  |
| ?topics=a,b&request_ip=client |  | accesschedule route          |
+-------------------------------+  +-------------------------------+
        |                             |
        | response.addrs              | SlotAddr
        v                             v
+-------------------------------+  +-------------------------------+
| topic -> backend host:port    |  | topic -> backend host:port    |
+-------------------------------+  +-------------------------------+
```

远端模式返回 JSON 后，scheduler 会读取：

```text
addrs: topic -> backend address
requested_cloudtype: edge / center
```

本地模式则根据 `clientHost`、topic 对应的 `accesschedule.Group`、网络路由/调度策略，选择一个最优 worker `SlotAddr`。

### 6. Scheduler MetadataResponse 结构

```text
+-------------------------------+
| MetadataResponse.Brokers      |
+-------------------------------+
        |
        +-- NodeID 0
        |   |
        |   +-- edge client:
        |   |   host:port = KafkaService.TTGW
        |   |
        |   +-- center client:
        |       host:port = scheduler self IP:ListenPort
        |
        +-- NodeID 1..N
            |
            +-- one backend broker per topic
                host:port = scheduled worker/VIP addr

+-------------------------------+
| MetadataResponse.Topics       |
+-------------------------------+
        |
        +-- each requested topic
            |
            +-- partitions: 0..PartitionNum-1
                |
                +-- LeaderID = topic backend NodeID
                +-- if no backend: LeaderID=-1,
                    error=UnknownTopicOrPartition
```

这解释了 producer 行为：

- producer 首先连 scheduler。
- producer 从 metadata 里拿到 topic partition 的 `LeaderID`。
- producer 再根据 `LeaderID` 找到 brokers 列表里的 `host:port`。
- 后续 `ProduceRequest` 直接连这个 worker/VIP broker。

### 7. 端到端交互时序

```text
+----------------+        +-------------------------+        +--------------------------+
| Kafka producer |        | Kafka scheduler process |        | Tide worker kafka server |
+----------------+        +-------------------------+        +--------------------------+
        |                              |                                |
        | ApiVersions                  |                                |
        |----------------------------->|                                |
        | ApiVersionsResponse          |                                |
        |<-----------------------------|                                |
        |                              |                                |
        | MetadataRequest(topic)       |                                |
        |----------------------------->|                                |
        |                              | build topic -> backend broker  |
        |                              | from DAG/local/remote route    |
        | MetadataResponse             |                                |
        | broker=worker/VIP:port       |                                |
        |<-----------------------------|                                |
        |                              |                                |
        | ProduceRequest(topic,part)   |                                |
        |-------------------------------------------------------------->|
        |                              |                                | decode/WAL/ringbuffer
        | ProduceResponse              |                                |
        |<--------------------------------------------------------------|
```

## Produce Flow

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | ProduceRequest(topic, partition, record batch)
        v
+--------------------------+
| produce_handler::handle()|
| check tx/idempotent/acks |
+--------------------------+
        |
        v
+------------------------------+
| TideBridge::IsRunning(topic) |
+------------------------------+
        |
        +-----------------------------+
        | false                       | true
        v                             v
+------------------------------+  +--------------------------------+
| ProduceResponse              |  | TideBridge::GetMaxPartition()  |
| not_leader_for_partition     |  +--------------------------------+
+------------------------------+                  |
                                                  |
                        +-------------------------+
                        | <= 0                    | > 0
                        v                         v
          +------------------------------+  +--------------------------------+
          | ProduceResponse              |  | Normalize partition            |
          | not_leader_for_partition     |  | real = request % max_partition |
          +------------------------------+  +--------------------------------+
                                                  |
                                                  v
                                      +-----------------------------+
                                      | partition_manager::get(ntp) |
                                      +-----------------------------+
                                                  |
                        +-------------------------+
                        | null                    | found
                        v                         v
          +------------------------------+  +--------------------------------+
          | ProduceResponse              |  | DataPartition::do_produce()    |
          | unknown_topic_or_partition   |  | decode / WAL / push ringbuffer |
          +------------------------------+  +--------------------------------+
                                                  |
                                                  v
                                      +-----------------------------+
                                      | ProduceResponse             |
                                      | error_code = none           |
                                      +-----------------------------+
```

## Source Consumption Flow

Produce 写入 `DataPartition` 后，并不会直接进入下游 operator，而是等待 `KafkaProtocolSource::Run()` 拉取。

```text
+--------------------------+
| DataPartition ringbuffer |
+--------------------------+
        |
        | Pop KafkaContext
        v
+----------------------------+
| KafkaProtocolSource::Run() |
| replay WAL if needed       |
| mark partition ready       |
+----------------------------+
        |
        | output.Collect(...)
        v
+----------------------+
| Downstream operators |
+----------------------+
```

topic 可写状态也在 source run 阶段完成：

```text
+----------------------------+
| KafkaProtocolSource::Run() |
+----------------------------+
        |
        v
+------------------------------------------------+
| KafkaServer::MarkTopicPartitionReady(topic, p) |
+------------------------------------------------+
        |
        v
+------------------------------------------------+
| TideBridge::MarkTopicPartitionReady(topic, p)  |
+------------------------------------------------+
        |
        v
+---------------------------------+
| TopicInfo::MarkPartitionReady() |
+---------------------------------+
        |
        | all partitions ready
        v
+----------------------------+
| TopicInfo status = RUNNING |
+----------------------------+
```

因此 produce 过早可能失败：

```text
+--------------------+
| Task Init finished |
+--------------------+
        |
        | topic exists, partition exists
        v
+------------------------------------+
| Task Run not called enough times   |
+------------------------------------+
        |
        | TopicInfo not RUNNING
        v
+------------------------------------------+
| ProduceRequest -> not_leader_for_partition |
+------------------------------------------+
```

## DataPartition 内部

```text
+-------------------------------+
| DataPartition                 |
+-------------------------------+
        |
        +------------------------------+
        |                              |
        v                              v
+---------------------------+  +----------------------+
| data_decoders_[thread]    |  | data_tickler_        |
| json/csv/pb/text decoder  |  | WAL for data replay  |
+---------------------------+  +----------------------+
        |
        +------------------------------+
        |                              |
        v                              v
+---------------------------+  +--------------------------+
| resource_decoders_[thread]|  | resource_tickler_        |
| tx/idempotent records     |  | WAL for resource replay  |
+---------------------------+  +--------------------------+
        |
        v
+-------------------------------+
| table_arrays_[thread]         |
| ringbuffer<KafkaContext>      |
+-------------------------------+
```

Produce 写入的是 `table_arrays_`，Source Run 读取的也是 `table_arrays_`。

## 三种典型部署模式

### 1. Client 通过 scheduler bootstrap

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | bootstrap.servers = kafka scheduler
        v
+--------------------------+
| Kafka scheduler process |
+--------------------------+
        |
        | MetadataResponse:
        | broker = selected worker/VIP
        v
+----------------+
| Kafka client   |
+----------------+
        |
        | ProduceRequest
        v
+--------------------------+
| Tide worker kafka::server |
+--------------------------+
```

适合：

- 生产端只知道 scheduler 地址。
- scheduler 根据 topic、client、调度策略返回 worker/VIP broker。
- 你贴的 `kafka.bootstrap.servers = [fdbd:...]:9996,...` 属于这种模式。

### 2. Client 直连 worker，worker 转发 metadata 给 scheduler

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | MetadataRequest
        v
+----------------------------+
| Tide worker kafka::server |
+----------------------------+
        |
        | forward to kafka.schedule.host:kafka.schedule.port
        v
+--------------------------+
| Kafka scheduler process |
| independent process     |
+--------------------------+
        |
        | MetadataResponse
        v
+----------------------------+
| Tide worker kafka::server |
+----------------------------+
        |
        | response back to client
        v
+----------------+
| Kafka client   |
+----------------+
        |
        | ProduceRequest to advertised broker
        v
+--------------------------+
| Chosen Tide worker / VIP |
+--------------------------+
```

适合：

- client 的 bootstrap 地址是 worker/VIP。
- worker 配了 `kafka.schedule.host` 和 `kafka.schedule.port`。
- worker 不自己决策 metadata，而是把 metadata request 转发给独立 scheduler 进程。

### 3. Worker 本地返回 metadata

```text
+----------------+
| Kafka client   |
+----------------+
        |
        | bootstrap.servers = worker-ip:connector.port
        v
+--------------------------+
| Tide worker kafka::server |
+--------------------------+
        |
        | MetadataResponse:
        | broker = kafka.ttgw.hostport
        |       or MY_HOST_IP:connector.port
        v
+----------------+
| Kafka client   |
+----------------+
        |
        | ProduceRequest
        v
+--------------------------+
| same worker kafka::server |
+--------------------------+
```

适合：

- 不经过独立 scheduler。
- client 能直接访问 worker。
- 或者通过 `kafka.ttgw.hostport` 显式返回 VIP / L4 地址。

## 关键配置与含义

Source 侧 `kafka_protocol` 配置：

```sql
'connector.type' = 'kafka_protocol',
'connector.mode' = 'source',
'connector.topic' = '<topic>',
'connector.port' = '<listen-port>',
'kafka.server.thread' = '<server-io-thread-num>',
'format.type' = 'json',

-- 本地 metadata 模式
'kafka.metadata.enable' = 'true',
'kafka.ttgw.hostport' = '<client-reachable-host>:<port>',

-- metadata 转发模式
'kafka.schedule.host' = '<scheduler-host>',
'kafka.schedule.port' = '<scheduler-port>'
```

Sink / producer 侧配置：

```sql
'connector.type' = 'kafka',
'connector.mode' = 'sink',
'kafka.bootstrap.servers' = '<scheduler-host-1>:<port>,<scheduler-host-2>:<port>',
'kafka.topic' = '<topic>'
```

这里的 `kafka.bootstrap.servers` 可以是 scheduler 地址。Kafka client 先连 scheduler 拉 metadata，再根据 metadata 里的 broker 地址连接 worker/VIP 发 produce。

## 常见误解

### 误解 1：这是一个 Kafka source client

不是。`kafka_protocol` 是 Tide worker 内嵌 Kafka server，接收外部 Kafka client produce。

### 误解 2：producer 的 bootstrap.servers 一定是 worker 地址

不是。你贴的 sink 配置里，`kafka.bootstrap.servers` 是 Kafka scheduler 地址。scheduler 返回 metadata 后，client 才会连到 metadata 中的 worker/VIP broker。

### 误解 3：worker 一定自己返回 metadata

不是。worker 有两种 metadata 行为：

- 配了 `kafka.schedule.host/port`：worker 把 `MetadataRequest` 转发给独立 scheduler。
- 没配 `kafka.schedule.host/port`：worker 才走本地 metadata，按 `kafka.ttgw.hostport` 或本机 IP 返回 broker。

### 误解 4：Init 成功后 topic 就一定能 produce

不完全是。Produce handler 还要求 `TopicInfo` 状态为 `RUNNING`。这个状态在 source `Run()` 阶段按 partition 标 ready 后才设置。

## 排障路径

```text
+--------------------------------------+
| client 找不到 topic / metadata 不可用 |
+--------------------------------------+
        |
        v
+--------------------------------------+
| 先确认 client bootstrap 连到哪里      |
| scheduler？worker？VIP？              |
+--------------------------------------+
        |
        v
+--------------------------------------+
| 如果 bootstrap 是 scheduler           |
| 查 scheduler 是否返回 worker/VIP broker|
+--------------------------------------+
        |
        v
+--------------------------------------+
| 如果 bootstrap 是 worker/VIP          |
| 查 worker 是否配置 schedule host      |
| 未配置时才看 kafka.metadata.enable    |
+--------------------------------------+
        |
        v
+--------------------------------------+
| 检查 MetadataResponse broker 地址     |
| 内网 IP 不通 -> 查 scheduler/ttgw 配置 |
| VIP 不通 -> 查 VIP/L4 到 worker 转发  |
+--------------------------------------+
        |
        v
+--------------------------------------+
| 检查 topic 是否注册                   |
| KafkaProtocolSource::Init 是否成功    |
| RegisterTopic/RegisterPartition 日志  |
+--------------------------------------+
        |
        v
+--------------------------------------+
| 检查 topic 是否 RUNNING               |
| 是否看到所有 partition ready 日志     |
| 未 RUNNING 会 not_leader_for_partition|
+--------------------------------------+
```

## 代码索引

| 模块 | 文件 | 作用 |
|---|---|---|
| Source 初始化 | `src/source/mq/kafka_protocol_source.cpp` | 解析 source options、创建 decoder、注册 topic/partition |
| Server 生命周期 | `src/source/mq/kafka_server.cpp` | 按端口创建或复用 KafkaServer |
| TCP Kafka server | `src/source/mq/kafka/server/server.cpp` | bind/listen、accept client connection |
| Metadata handler | `src/source/mq/kafka/server/handlers/metadata.cpp` | 处理 MetadataRequest、返回 broker/topic/partition |
| Produce handler | `src/source/mq/kafka/server/handlers/produce.cpp` | 处理 ProduceRequest、写入 DataPartition |
| Topic registry | `src/source/mq/utils/tide_bridge.cpp` | topic 注册、状态查询、partition 遍历 |
| Topic state | `src/source/mq/utils/topic_info.cpp` | partition 列表、RUNNING 状态 |
| Buffer/WAL | `src/source/mq/utils/data_partition.cpp` | decode、WAL、ringbuffer |
| Address config | `src/source/mq/utils/config.h` | `kafka.ttgw.hostport` 等地址解析 |
| Scheduler service | `/data00/home/sunmingqiang/Documents/scheduler/biz/service/kafkaservice/service.go` | Kafka scheduler 监听端口、接入连接、构建 topic/group |
| Scheduler metadata | `/data00/home/sunmingqiang/Documents/scheduler/biz/service/kafkaservice/conn.go` | 处理 ApiVersions/Metadata、识别 `tide_fip_`、调度 backend broker、构造 MetadataResponse |
| Scheduler metadata schema | `/data00/home/sunmingqiang/Documents/scheduler/biz/service/kafkaservice/protocol/metadata/metadata.go` | Kafka Metadata Request/Response 结构 |
| Scheduler config | `/data00/home/sunmingqiang/Documents/scheduler/biz/config/kafkaservice/config.go` | `ListenPort`、`TTGW`、`ForwardToRemote`、`ForwardToRemoteUrl` 配置 |
