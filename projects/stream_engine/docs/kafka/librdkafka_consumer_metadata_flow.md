# librdkafka Consumer Metadata Flow

本文基于 `inf/librdkafka` 源码梳理 Kafka consumer 的消费流程、metadata 请求时机、为什么日志中会看到多次 metadata 调用，以及 metadata / fetch 请求的 broker 选择规则。

## 一句话模型

```text
+-------------------------------+
| Kafka consumer application    |
| subscribe / assign / poll     |
+-------------------------------+
        |
        | bootstrap.servers
        v
+-------------------------------+
| librdkafka client             |
| broker threads + main thread  |
+-------------------------------+
        |
        | MetadataRequest
        v
+-------------------------------+
| Any usable broker             |
| scheduler or Kafka broker     |
+-------------------------------+
        |
        | MetadataResponse:
        | brokers + topics + leaders
        v
+-------------------------------+
| librdkafka metadata cache     |
| partition -> leader broker    |
+-------------------------------+
        |
        | FetchRequest per leader broker
        v
+-------------------------------+
| Leader broker                 |
| returns records               |
+-------------------------------+
        |
        | local fetch queue
        v
+-------------------------------+
| application poll()            |
+-------------------------------+
```

## Consumer 消费主流程

```text
+-------------------------------+
| Application                   |
| subscribe(topics) / assign()  |
+-------------------------------+
        |
        v
+-------------------------------+
| Need metadata                 |
| topic -> partitions -> leader |
+-------------------------------+
        |
        v
+-------------------------------+
| MetadataRequest               |
| sent to any usable broker     |
+-------------------------------+
        |
        v
+-------------------------------+
| MetadataResponse              |
| update broker/topic cache     |
+-------------------------------+
        |
        v
+-------------------------------+
| Consumer group rebalance      |
| assign partitions             |
+-------------------------------+
        |
        v
+-------------------------------+
| Toppar fetch start            |
| set concrete fetch offset     |
+-------------------------------+
        |
        v
+-------------------------------+
| Delegate partition to leader  |
| broker active toppar list     |
+-------------------------------+
        |
        v
+-------------------------------+
| Broker thread builds Fetch    |
| one in-flight Fetch per broker|
+-------------------------------+
        |
        v
+-------------------------------+
| FetchResponse parsed          |
| messages enter fetch queue    |
+-------------------------------+
        |
        v
+-------------------------------+
| Application poll()            |
+-------------------------------+
```

关键点：

- metadata 是消费的控制面前置条件：没有 topic partition 和 leader 信息，就不知道去哪个 broker 拉取数据。
- fetch 是数据面：partition 被绑定到 leader broker 后，broker thread 才会周期性构造 `FetchRequest`。
- leader 变化、topic 不存在、broker down 等都会重新触发 metadata。

## Metadata 请求统一入口

大部分内部刷新最终会走到：

```text
+-------------------------------+
| rd_kafka_metadata_refresh_*   |
+-------------------------------+
        |
        v
+-------------------------------+
| rd_kafka_metadata_refresh_topics() |
+-------------------------------+
        |
        | choose broker
        v
+-------------------------------+
| rd_kafka_broker_any_usable()  |
+-------------------------------+
        |
        | send request
        v
+-------------------------------+
| rd_kafka_MetadataRequest()    |
+-------------------------------+
        |
        v
+-------------------------------+
| rd_kafka_handle_Metadata()    |
| update metadata cache         |
+-------------------------------+
```

代码路径：

- `rd_kafka_metadata_refresh_topics()`：`inf/librdkafka/src/rdkafka_metadata.c`
- `rd_kafka_broker_any_usable()`：`inf/librdkafka/src/rdkafka_broker.c`
- `rd_kafka_MetadataRequest()`：`inf/librdkafka/src/rdkafka_request.c`

## Metadata 触发时机

### 1. 应用主动请求 metadata

```text
+-------------------------------+
| Application                   |
| rd_kafka_metadata()           |
+-------------------------------+
        |
        | force = 1
        v
+-------------------------------+
| rd_kafka_MetadataRequest()    |
| reason="application requested"|
+-------------------------------+
```

特点：

- 应用主动调用会设置 `force=1`。
- `force=1` 会绕过部分 in-flight 去重，目的是满足应用显式请求。
- 如果业务或调试代码频繁调用 `rd_kafka_metadata()`，日志里会直接看到多次 metadata。

### 2. 周期刷新

```text
+-------------------------------+
| rdkafka main thread timer     |
| rd_kafka_metadata_refresh_cb  |
+-------------------------------+
        |
        +---------------------------------------+
        |                                       |
        | high-level consumer with cgrp         | producer/simple client
        v                                       v
+-------------------------------+   +-------------------------------+
| refresh consumer topics       |   | refresh known topics          |
| known + subscribed topics     |   | force=true                    |
+-------------------------------+   +-------------------------------+
        |
        | if no local topic
        v
+-------------------------------+
| refresh broker list           |
| suppressed to about 10s       |
+-------------------------------+
```

特点：

- high-level consumer 会同时刷新已知 topic 和订阅 topic。
- 这样能发现 partition 数变化、topic 删除、订阅 topic 从不存在变为存在。
- 如果本地还没有 topic，客户端会周期性刷新 broker list，避免连接长期空闲且 broker 列表过期。

### 3. broker 连接建立后刷新

```text
+-------------------------------+
| broker connection becomes UP  |
| rd_kafka_broker_connect_up()  |
+-------------------------------+
        |
        v
+-------------------------------+
| refresh known topics          |
+-------------------------------+
        |
        | if no known topics
        v
+-------------------------------+
| refresh broker list           |
+-------------------------------+
```

特点：

- bootstrap broker 或 metadata 返回的新 broker 连接成功后，会主动刷新 metadata。
- 这是为了尽快确认 broker/topic/leader 最新状态。

### 4. Fetch 错误触发刷新

```text
+-------------------------------+
| FetchResponse error           |
+-------------------------------+
        |
        | UNKNOWN_TOPIC_OR_PART
        | LEADER_NOT_AVAILABLE
        | NOT_LEADER_FOR_PARTITION
        | BROKER_NOT_AVAILABLE
        | REPLICA_NOT_AVAILABLE
        v
+-------------------------------+
| force refresh known topics    |
| reason="FetchRequest failed"  |
+-------------------------------+
```

特点：

- 这些错误通常说明本地 metadata 里的 partition leader 已经过期或 topic 状态变化。
- librdkafka 会强制刷新 known topics。
- 如果 broker/scheduler 返回的 leader 不稳定、topic 状态未 ready、VIP 路由不稳定，就会看到 fetch error 和 metadata refresh 交替出现。

### 5. broker down 触发刷新

```text
+-------------------------------+
| Broker was UP                 |
+-------------------------------+
        |
        | transport fail / disconnect
        v
+-------------------------------+
| broker fail handling          |
+-------------------------------+
        |
        v
+-------------------------------+
| force refresh known topics    |
+-------------------------------+
```

特点：

- 已经 UP 的 broker 掉线后，客户端会刷新 metadata 来寻找新的 broker/leader。
- 如果 metadata response 返回的 broker 地址不可达，会形成连接失败 -> metadata 刷新 -> 再连接的循环。

## 为什么会多次 Metadata 调用

多次 metadata 调用通常是正常行为，常见原因如下：

```text
+-------------------------------+
| Same consumer instance        |
+-------------------------------+
        |
        +-- startup connect -> metadata
        |
        +-- subscribe / cgrp -> metadata
        |
        +-- periodic refresh -> metadata
        |
        +-- fetch error -> forced metadata
        |
        +-- broker down -> forced metadata
        |
        +-- application rd_kafka_metadata() -> forced metadata
```

需要重点区分：

- `periodic topic and broker list refresh`：周期刷新。
- `connected`：broker 连接进入 UP 后刷新。
- `FetchRequest failed: ...`：fetch 错误触发，通常值得重点排查。
- `application requested`：应用主动调用。
- `periodic broker list refresh`：没有本地 topic 时刷新 broker list。

## Metadata 请求去重规则

```text
+-------------------------------+
| rd_kafka_metadata_refresh_topics() |
+-------------------------------+
        |
        +-----------------------------+
        |                             |
        | force=false                 | force=true
        v                             v
+-------------------------------+  +-------------------------------+
| metadata_cache_hint()         |  | bypass topic de-dup          |
| filter in-flight topics       |  | query requested topics       |
+-------------------------------+  +-------------------------------+
        |
        | all topics already requested
        v
+-------------------------------+
| Skip metadata refresh         |
| "already being requested"     |
+-------------------------------+
```

说明：

- 非强制刷新会用 metadata cache hint 过滤正在请求中的 topic，避免重复请求。
- 强制刷新会直接请求，例如应用主动请求、周期 consumer topic refresh、fetch 错误刷新。
- full metadata request 还有额外的 in-flight 控制，避免同时发多个 full broker/full topic 请求。

## Metadata Broker 选择规则

metadata 请求不是固定发给 controller，也不是固定发给第一个 bootstrap broker。默认选择一个“any usable broker”。

```text
+-------------------------------+
| Need MetadataRequest broker   |
+-------------------------------+
        |
        v
+-------------------------------+
| rd_kafka_broker_any_usable()  |
+-------------------------------+
        |
        v
+-------------------------------+
| rd_kafka_broker_weighted()    |
| choose highest weighted broker|
+-------------------------------+
        |
        +-----------------------------+
        | candidate requirements      |
        v
+-------------------------------+
| broker state is UP            |
| broker is in whitelist        |
+-------------------------------+
        |
        +-----------------------------+
        | weight preference           |
        v
+-------------------------------+
| non-bootstrap broker          |
| non-logical broker            |
| non-blocking broker           |
| recently used connection      |
+-------------------------------+
        |
        | same weight
        v
+-------------------------------+
| reservoir sampling            |
| random among equal weight     |
+-------------------------------+
```

结论：

- metadata 优先发给已经 UP、可用、最近活跃的真实 broker。
- bootstrap broker 只是初始入口；拿到 metadata 后，后续请求倾向使用 metadata 中的真实 broker。
- 如果开启 sparse connections 且没有可用 broker，客户端会主动挑一个 broker 发起连接。
- 如果所有 broker 都不可用，本次 metadata refresh 会返回 transport/no usable broker，并等待后续连接状态变化。

## Metadata Response 如何影响 Fetch Broker

```text
+-------------------------------+
| MetadataResponse              |
+-------------------------------+
        |
        v
+-------------------------------+
| brokers[]                     |
| node_id -> host:port          |
+-------------------------------+
        |
        v
+-------------------------------+
| topics[].partitions[]         |
| partition -> leader node_id   |
+-------------------------------+
        |
        v
+-------------------------------+
| librdkafka updates topic      |
| partition leader mapping      |
+-------------------------------+
        |
        v
+-------------------------------+
| partition delegated to leader |
| broker active toppar list     |
+-------------------------------+
        |
        v
+-------------------------------+
| FetchRequest sent to leader   |
+-------------------------------+
```

在 Tide scheduler 场景中：

- producer/consumer 的 `bootstrap.servers` 可以是 scheduler。
- scheduler 在 `MetadataResponse` 中返回 worker/VIP broker。
- librdkafka 后续会根据 partition leader 连接 worker/VIP，而不是继续把 produce/fetch 发给 scheduler。

## Fetch Broker 选择规则

Fetch broker 不是由 `any usable broker` 随机选，而是由 metadata 中的 partition leader 决定。

```text
+-------------------------------+
| Partition assigned            |
+-------------------------------+
        |
        v
+-------------------------------+
| Has leader from metadata      |
+-------------------------------+
        |
        v
+-------------------------------+
| Delegate toppar to leader     |
+-------------------------------+
        |
        v
+-------------------------------+
| Leader broker thread          |
| active_toppars list           |
+-------------------------------+
        |
        v
+-------------------------------+
| rd_kafka_broker_fetch_toppars |
| batch partitions on broker    |
+-------------------------------+
        |
        v
+-------------------------------+
| FetchRequest to leader broker |
+-------------------------------+
```

Fetch 构造规则：

- 一个 broker thread 会把该 broker 负责的多个 topic/partition 聚合到同一个 `FetchRequest`。
- 每个 broker 同时最多一个 in-flight Fetch：`rkb_fetching=1`，收到响应后清零。
- `FetchRequest` 中写入每个 partition 的 `FetchOffset` 和 `MaxBytes`。
- 如果本地 fetch queue 已达到 `queued.min.messages` 或 `queued.max.messages.kbytes`，该 partition 暂时不会继续 fetch。
- 如果 partition paused、没有具体 offset、处于 backoff，也不会进入 active fetch list。

## 多次 Metadata 与 Broker 选取的典型时序

```text
+-------------------------------+
| Consumer starts               |
+-------------------------------+
        |
        | connect bootstrap scheduler/broker
        v
+-------------------------------+
| Broker state UP               |
| metadata reason="connected"   |
+-------------------------------+
        |
        | subscribe topics / cgrp
        v
+-------------------------------+
| consumer topics refresh       |
| known + subscribed topics     |
+-------------------------------+
        |
        | metadata response gives
        | partition leader broker
        v
+-------------------------------+
| Fetch from leader broker      |
+-------------------------------+
        |
        +-----------------------------+
        |                             |
        | success                     | leader/topic/broker error
        v                             v
+-------------------------------+  +-------------------------------+
| messages enter fetch queue    |  | force metadata refresh       |
+-------------------------------+  | reason=FetchRequest failed   |
                                   +-------------------------------+
```

如果日志中多次看到 metadata，需要结合 reason 判断：

- 启动早期连续几次：通常是 bootstrap 连接、订阅、consumer group 初始化叠加。
- 周期性稳定出现：通常是 metadata refresh interval。
- fetch 错误后密集出现：通常是 broker 地址、topic ready、leader mapping 或网络路由问题。
- 每次业务代码调用后出现：检查是否显式调用了 metadata API。

## 对 Tide Kafka Scheduler 的含义

```text
+-------------------------------+
| librdkafka bootstrap.servers  |
| scheduler address             |
+-------------------------------+
        |
        | MetadataRequest
        v
+-------------------------------+
| Kafka scheduler               |
| returns worker/VIP broker     |
+-------------------------------+
        |
        | MetadataResponse
        v
+-------------------------------+
| librdkafka broker cache       |
| NodeID -> worker/VIP hostport |
+-------------------------------+
        |
        | Produce/Fetch
        v
+-------------------------------+
| Tide worker kafka::server     |
+-------------------------------+
```

排查建议：

- 如果 client 日志显示反复 metadata，但没有 Produce/Fetch 到 worker，先检查 scheduler 返回的 broker host/port 是否 client 可达。
- 如果 metadata 正常但 Fetch/Produce 报 `NOT_LEADER_FOR_PARTITION` 或 `UNKNOWN_TOPIC_OR_PARTITION`，检查 worker `TopicInfo` 是否已经 `RUNNING`，以及 scheduler 返回的 partition leader 是否指向正确 worker/VIP。
- 如果 metadata reason 是 `FetchRequest failed`，优先看 worker 返回的错误码和 client 当前连接的 broker 地址。
- 如果 metadata broker 选择落在旧 broker，检查 librdkafka metadata cache 是否仍保留旧 broker，以及 scheduler 是否稳定返回同一组 NodeID/hostport。

## 现象：每次 Metadata 只有一个 Broker 但地址变化

你观察到的现象可以描述为：

```text
Metadata #1:
  brokers = [NodeID 1 -> worker-a:9951]
  topic partition leader = 1

Metadata #2:
  brokers = [NodeID 1 -> worker-b:9951]
  topic partition leader = 1

Metadata #3:
  brokers = [NodeID 1 -> worker-c:9951]
  topic partition leader = 1
```

每次 metadata 都只有一个 broker，且这个 broker 都能 produce 成功，但 broker 地址一直变化。

### 直观结论

```text
+-------------------------------+
| 单次 produce 可以成功          |
+-------------------------------+
        |
        | 但 metadata 持续变化
        v
+-------------------------------+
| librdkafka 持续迁移 leader     |
| broker address keeps changing |
+-------------------------------+
        |
        v
+-------------------------------+
| 连接重建 / 队列迁移 / 重试增加 |
+-------------------------------+
        |
        v
+-------------------------------+
| 生产延迟抖动                   |
| 吞吐下降                       |
| metadata 日志变多              |
| broker thread/socket churn     |
+-------------------------------+
```

这不是“完全不能生产”的问题，而是“控制面不稳定导致数据面持续抖动”的问题。

### librdkafka 会发生什么

```text
+-------------------------------+
| Receive MetadataResponse      |
| brokers = one changing broker |
+-------------------------------+
        |
        v
+-------------------------------+
| Rebuild broker whitelist      |
| only current broker remains   |
+-------------------------------+
        |
        v
+-------------------------------+
| Update broker by NodeID       |
| same NodeID, new host:port    |
+-------------------------------+
        |
        v
+-------------------------------+
| broker nodename changed       |
| nodename_epoch++              |
+-------------------------------+
        |
        v
+-------------------------------+
| schedule disconnect/reconnect |
| to new worker/VIP address     |
+-------------------------------+
        |
        v
+-------------------------------+
| Topic partition leader points |
| to the new broker address     |
+-------------------------------+
```

关键点：

- `MetadataResponse.brokers` 只有一个 broker，意味着客户端每次看到的 broker universe 都是单点。
- 本仓库的 librdkafka 会根据 metadata 重建 `white_list_map`，所以旧 broker 会从可用白名单里消失。
- 如果 broker `NodeID` 不变但 host/port 变化，broker 的 `nodename` 会变化，并触发连接重建。
- 如果 metadata 刷新频率高，连接还没稳定或 batch 还没充分积累，下一次 metadata 又把 leader 切到另一个地址。

### 对 Producer 的影响

```text
+-------------------------------+
| Producer message enters queue |
+-------------------------------+
        |
        v
+-------------------------------+
| Current metadata selects      |
| worker-a as leader broker     |
+-------------------------------+
        |
        +-----------------------------+
        |                             |
        | metadata stable             | metadata changes to worker-b
        v                             v
+-------------------------------+  +-------------------------------+
| Batch accumulates normally    |  | Old connection may be closed  |
| ProduceRequest sent           |  | New connection needed         |
+-------------------------------+  +-------------------------------+
        |                             |
        v                             v
+-------------------------------+  +-------------------------------+
| Low latency / high batching   |  | Retry / re-enqueue / wait     |
+-------------------------------+  | higher latency and jitter     |
                                   +-------------------------------+
```

实际表现：

- **延迟抖动**：leader broker 地址变化会带来连接建立、TCP/TLS/SASL 握手、ApiVersions、metadata 刷新等额外等待。
- **吞吐下降**：batch 正在向一个 broker 聚合时，broker 变了会降低 batch 命中率，`linger.ms` 和 `batch.max.bytes` 的收益变差。
- **重试增多**：旧 broker 连接关闭或收到 `NOT_LEADER_FOR_PARTITION` / transport error 后，消息需要等待新 metadata 或新连接。
- **队列堆积**：短时间 broker 频繁切换时，produce queue 可能增长，表现为发送延迟、`queue.buffering` 相关指标升高。
- **重复风险**：如果是 at-least-once 且没有严格幂等语义，连接断开时可能出现“服务端已写入但客户端未知”的情况，重试后有重复写入风险。
- **线程和 socket 抖动**：新的 broker 地址会带来 broker 对象/连接状态变化，旧连接进入失败或待清理状态，增加 CPU、日志和连接管理开销。

如果每个被选中的 worker 都确实能接收该 topic 的 produce，功能上看起来“都能写”；但从生产稳定性看，这等价于每次 metadata 都在做一次小型 broker failover。

### 对 Consumer 的影响

```text
+-------------------------------+
| Consumer fetch from worker-a  |
+-------------------------------+
        |
        | metadata changes leader to worker-b
        v
+-------------------------------+
| Partition delegated to new    |
| leader broker                 |
+-------------------------------+
        |
        v
+-------------------------------+
| Old Fetch stops / new Fetch   |
| starts from same fetch offset |
+-------------------------------+
        |
        v
+-------------------------------+
| Fetch latency jitter          |
| possible duplicate delivery   |
| depending on offset commit    |
+-------------------------------+
```

实际表现：

- consumer 会把 partition delegate 到新的 leader broker。
- old broker 上的 in-flight Fetch 可能过期或返回错误，新 broker 重新从当前 fetch offset 拉。
- 如果应用处理和 offset commit 不是严格同步，broker 频繁切换会放大重复消费窗口。
- 如果切换时 worker 还未 `RUNNING`，可能触发 `NOT_LEADER_FOR_PARTITION` / `UNKNOWN_TOPIC_OR_PARTITION`，进而再次强制 metadata。

### 对 Scheduler 和 Worker 的影响

```text
+-------------------------------+
| Scheduler returns changing    |
| single broker                 |
+-------------------------------+
        |
        v
+-------------------------------+
| Clients reconnect to different|
| workers over time             |
+-------------------------------+
        |
        v
+-------------------------------+
| Worker receive load shifts    |
| abruptly instead of smoothly  |
+-------------------------------+
        |
        v
+-------------------------------+
| Scheduler metadata QPS grows  |
| due to retry/error refresh    |
+-------------------------------+
```

风险：

- scheduler 调度结果如果没有 sticky，会导致同一个 topic 的 leader broker 在多个 worker/VIP 之间来回漂移。
- worker 端如果每个都能写，短期不报错，但负载会抖动。
- 客户端 metadata 刷新变多后，scheduler 的 metadata QPS 也会上升。
- 如果某些 worker 的 topic 状态尚未 ready，客户端会在“可写 worker”和“不可写 worker”之间抖动，错误会进一步触发 metadata refresh。

### 什么时候可以接受

这种模式只有在下面条件都满足时才相对可接受：

- metadata 切换频率低，例如只在故障转移或扩缩容时变化。
- 每个返回的 broker/VIP 都真正等价，且都能稳定处理该 topic 的所有 partition。
- producer 开启足够可靠的重试和幂等配置，业务能接受短时延迟抖动。
- scheduler 返回结果有明确收敛，不会在健康状态下持续来回切。

如果“每次 metadata 都变化”，就不应该视为正常负载均衡，而应该视为调度结果不稳定。

### 建议的 Scheduler 规则

```text
+-------------------------------+
| Same topic + same client zone |
+-------------------------------+
        |
        v
+-------------------------------+
| Return stable broker/VIP      |
| sticky by topic/client/IDC    |
+-------------------------------+
        |
        +-----------------------------+
        | only change on              |
        v
+-------------------------------+
| worker unhealthy              |
| topology changed              |
| explicit rebalance            |
+-------------------------------+
```

建议：

- 对同一个 `topic + clientHost/clientIDC` 返回稳定 broker/VIP，不要每次 metadata 随机选。
- 如果需要负载均衡，优先在更高层做稳定 hash 或带租约的 sticky，而不是每次 metadata 随机漂移。
- metadata 中的 `NodeID -> host:port` 尽量保持稳定；如果同一个 `NodeID` 频繁换 host，会触发客户端重连。
- 如果必须切换，最好让切换有最小租约时间，避免小于 `metadata.max.age.ms` 或业务刷新周期的抖动。
- 对单 topic 单 broker 返回场景，建议把这个 broker 设计成稳定 VIP，而不是频繁变化的真实 worker。

### 如何判断是否影响生产

优先看这些现象：

- producer 端 `metadata` 日志频繁，且 broker host/port 每次不同。
- producer 端出现 `Broker nodename changed`、`disconnect/reconnect`、`transport error`、`retry` 增多。
- produce p99 / p999 延迟呈锯齿状抖动。
- `queue.buffering.max.messages` / queue size 接近上限。
- scheduler metadata QPS 随 producer 数量或错误数放大。
- worker 端 topic `RUNNING` 正常但负载在多个 worker 间突然迁移。

一句话判断：

```text
都能 produce 说明数据面可达；
一直变化说明控制面不稳定；
控制面不稳定会把正常写入变成持续 failover 写入。
```

## 代码索引

| 模块 | 文件 | 作用 |
|---|---|---|
| 应用 metadata API | `inf/librdkafka/src/rdkafka_metadata.c` | `rd_kafka_metadata()` 主动请求 metadata |
| metadata refresh | `inf/librdkafka/src/rdkafka_metadata.c` | `rd_kafka_metadata_refresh_topics()`、known topics、consumer topics、broker list refresh |
| 周期刷新 | `inf/librdkafka/src/rdkafka.c` | `rd_kafka_metadata_refresh_cb()` |
| broker 选择 | `inf/librdkafka/src/rdkafka_broker.c` | `rd_kafka_broker_any_usable()`、`rd_kafka_broker_weighted()` |
| 连接后刷新 | `inf/librdkafka/src/rdkafka_broker.c` | `rd_kafka_broker_connect_up()` |
| fetch request | `inf/librdkafka/src/rdkafka_broker.c` | `rd_kafka_broker_fetch_toppars()` |
| fetch response | `inf/librdkafka/src/rdkafka_broker.c` | `rd_kafka_broker_fetch_reply()`、`rd_kafka_fetch_reply_handle()` |
| fetch decision | `inf/librdkafka/src/rdkafka_partition.c` | `rd_kafka_toppar_fetch_decide()` |
| topic/leader | `inf/librdkafka/src/rdkafka_topic.c` | topic metadata and leader update path |
