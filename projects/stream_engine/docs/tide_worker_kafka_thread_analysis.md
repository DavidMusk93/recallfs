# tide_worker Kafka 线程爆炸：根因确认与 UnifiedConsumer 解耦方案

> 目标：先聚焦 **KafkaSource → ClusterConsumerObj** 这一条真实线上路径，把单个 `tide_worker` 中 Kafka 相关线程从 1w+ 控制到 **≤ 128 rdk 线程**；验证闭环后，再复用同一套解耦层扩展到其他 Kafka Source 场景。

---

## 0. 结论摘要

### 0.1 根因

6511/7511 端口所在 `tide_worker` 的 1w+ 线程不是 Tide 自身 worker 线程膨胀，而是 **librdkafka consumer handle 数 × broker 数** 放大导致：

```text
现场路径: KafkaSource -> ClusterConsumerObj
创建点:   src/source/kafka/consumer_obj.cpp:25

同一 cluster: bmq_data_sys
consumer handle: 28
broker:   360

rdk 线程模型:
  每个 KafkaConsumer handle = 1 个 rdk:main + N 个 rdk:broker

线程放大:
  28 × (1 + 360) = 10,108 个 rdk 线程
```

### 0.2 治理方向

把 **per-subtask consumer handle** 改为 **per-node/per-cluster/per-group UnifiedConsumer**：

```text
旧模型: 28 个 subtask -> 28 个 KafkaConsumer handle -> 28 × 360 broker threads
新模型: 28 个 subtask -> 1~4 个 UnifiedConsumer handle -> subscribe + dispatch -> per-subtask queue
```

关键原则：

1. **节点间分布式安全**：默认使用 `subscribe()` + Kafka group protocol，避免手动 `assign()` 在 100 个节点下出现重叠/遗漏。
2. **节点内线程收敛**：一个节点内同 cluster/group/topics 共享 1 个 consumer handle；高吞吐时最多扩到 2~4 个 handle。
3. **对象生命周期同线程**：用 `MsgSlotRing` 保证 Kafka message 的创建与销毁都在 poll thread，subtask 只借用 slot。
4. **背压闭环**：bounded queue + per-partition pause/resume，慢 subtask 不拖垮整个进程。
5. **可观测优先**：通过 Unix Domain Socket 导出 HTML/JSON/Prometheus，按 worker 端口隔离。

### 0.3 目标线程预算

```text
默认模式:
  handle_count = 1
  rdk:main     = 1
  rdk:broker   = active partition leaders，目标约 30~80
  rdk total    ≈ 31~81

高吞吐模式:
  handle_count = 2~4
  rdk total    ≤ 128 作为硬约束
```

---

## 1. 现场确认：日志追溯 + GDB

本次分析遵循“**测试驱动 + 日志追溯驱动**”，不是静态代码驱动。代码只作为最终定位的锚点；真实路径由运行时日志和 GDB 线程栈确认。

### 1.1 进程概况

| 端口 | PID | 总线程数 | `rdk:*` 线程 | `rdk:main` | 备注 |
|---|---:|---:|---:|---:|---|
| 6511 | 3016658 | 13381 | 11386 | 30 | 首次 GDB attach 后进程异常退出 |
| 7511 | 3017975 | 13381 | 11386 | 18~30 | 同类 worker，用于继续分析 |

### 1.2 日志追溯确认 consumer 创建点

日志检索方式：

```bash
strings $logdir/tide_worker.log | grep "[Created] cluster"
```

确认事实：

```text
创建位置: src/source/kafka/consumer_obj.cpp:25
cluster:  全部为 bmq_data_sys
handle:   28 个 consumer handle
broker:   360 个 broker
```

按 `topic (= group.id)` 聚合：

| topic (= group.id) | handle 数 | 解释 |
|---|---:|---|
| tide2.bytecdn_data_access_out_01 | 8 | 8 个 subtask 各建 1 个 consumer |
| tide2.bytecdn_data_kfcaccess_out_tob_01 | 4 | 4 并行度 |
| tide2.bytecdn_data_kfcaccess_out_01 | 4 | 4 并行度 |
| tide2.bytecdn_data_access_out_tob_01 | 4 | 4 并行度 |
| tide2.bytecdn_data_kfcbs_out_tob_01 | 2 | 2 并行度 |
| tide2.bytecdn_data_kfcbs_out_01 | 2 | 2 并行度 |
| tide2.ttcp_monitor | 1 | 单并行度 |
| tide2.dwm_bytecdn_qlty_client_req_error_stats_hi_1min | 1 | 单并行度 |
| tide2.bytecdn_data_kfcaccess_out_tob_4xx_5xx_01 | 1 | 单并行度 |
| tide2.bytecdn_data_access_out_tob_4xx_5xx_data_01 | 1 | 单并行度 |
| **合计** | **28** | 同一 cluster 下 28 个 handle |

### 1.3 GDB 确认 librdkafka 线程函数

GDB attach 注意事项：进程使用相对路径加载 `./lib/` 下的 so，attach 前必须进入进程工作目录，否则符号不完整。

```bash
cd /proc/$pid/cwd
gdb -p $pid
```

确认到的典型线程栈：

```text
rdk:main
  -> rd_kafka_thread_main()
  -> rd_kafka_q_serve()
     inf/librdkafka/src/rdkafka.c:2217

rdk:broker*
  -> rd_kafka_broker_thread_main()
  -> rd_kafka_broker_consumer_serve()
     inf/librdkafka/src/rdkafka_broker.c:5799
```

### 1.4 根因公式

```text
rdk_threads = handle_count × (1 + broker_count) + transient_threads
            = 28 × (1 + 360) + transient_threads
            = 10,108 + transient_threads
            ≈ 现场 11,386 个 rdk:* 线程
```

核心问题不是 Kafka topic 数，而是：**同一个 worker 进程内，同一 cluster 的每个 subtask 都创建了独立 consumer handle；每个 handle 又各自连接 360 个 broker。**

---

## 2. 设计目标与边界

### 2.1 目标

| 项目 | 当前 | 目标 |
|---|---:|---:|
| 同 cluster consumer handle | 28 | 默认 1，高吞吐 2~4 |
| rdk 线程 | 10k+ | ≤ 128 |
| 分布式节点数 | 100 节点级别 | 不重叠、不遗漏 |
| 队列内存 | 跨线程创建/销毁 | poll thread 同线程管理 |
| 观测方式 | 依赖外部排查 | UDS 导出 HTML/JSON |

### 2.2 首期边界

首期只治理一条路径：

```text
KafkaSource -> ClusterConsumerObj -> RdKafka::KafkaConsumer
```

不在首期扩散到：

```text
MultiClusterKafkaSource
AsyncClusterConsumerObj
其他非 Kafka Source
```

首期完成后，同一套 `UnifiedConsumer` 抽象再扩展到其他路径。

### 2.3 非目标

- 不重写 Tide 调度器。
- 不依赖 Tide scheduler 做跨节点 partition 手动分配。
- 不以手动 `assign()` 作为分布式默认方案。
- 不引入外部 Web UI 服务或中心化 metrics aggregator。

---

## 3. 旧模型：per-subtask consumer handle

### 3.1 当前线程放大路径

```text
+--------------------------------------------------------------------------------+
|                         当前模型：per-subtask KafkaConsumer                       |
+--------------------------------------------------------------------------------+

  tide_worker(port=6511/7511)
  |
  |  same cluster = bmq_data_sys, brokers = 360
  |
  +-- Job-A / topic access_out / subtask-0 --> consumer#00 --> 360 rdk:broker
  +-- Job-A / topic access_out / subtask-1 --> consumer#01 --> 360 rdk:broker
  +-- Job-A / topic access_out / subtask-2 --> consumer#02 --> 360 rdk:broker
  +-- ...
  +-- Job-A / topic access_out / subtask-7 --> consumer#07 --> 360 rdk:broker
  |
  +-- Job-B / topic kfcaccess / subtask-0  --> consumer#08 --> 360 rdk:broker
  +-- Job-B / topic kfcaccess / subtask-1  --> consumer#09 --> 360 rdk:broker
  +-- ...
  |
  +-- other topics/subtasks                 --> consumer#27 --> 360 rdk:broker

  每个 consumer handle:
      1 rdk:main + 360 rdk:broker

  总计:
      28 × 361 = 10,108 rdk threads
```

### 3.2 为什么 broker 数会被乘上 handle 数

librdkafka 的连接与线程属于 consumer handle 内部资源。多个 `KafkaConsumer` handle 即使连接同一 cluster，也不会共享 broker thread：

```text
consumer#0  -> broker-0 thread, broker-1 thread, ..., broker-359 thread
consumer#1  -> broker-0 thread, broker-1 thread, ..., broker-359 thread
...
consumer#27 -> broker-0 thread, broker-1 thread, ..., broker-359 thread
```

所以治理的第一性原理是：**减少 handle 数，而不是只调大线程栈或系统线程上限。**

---

## 4. 新模型：UnifiedConsumer

### 4.1 总体架构

```text
+--------------------------------------------------------------------------------+
|                         新模型：UnifiedConsumer per node/group/cluster            |
+--------------------------------------------------------------------------------+

  tide_worker(port=6511/7511)
  |
  |  key = cluster + group.id + topic-set + security-config
  |
  +------------------------------+
  | UnifiedConsumer              |
  | - cluster: bmq_data_sys      |
  | - subscribe(topics)          |
  | - group.id: original group   |
  | - handle_count: 1 default    |
  +--------------+---------------+
                 |
                 | librdkafka
                 v
      +---------------------+
      | Kafka group protocol|
      | broker rebalance    |
      +----------+----------+
                 |
                 | assigned partitions on this node
                 v
      +---------------------+
      | poll-thread-0       |
      | consume + route     |
      +----------+----------+
                 |
                 v
       +---------+---------+---------+----------------+
       |         |         |         |                |
       v         v         v         v                v
   +-------+ +-------+ +-------+ +-------+        +-------+
   |queue-0| |queue-1| |queue-2| | ...   |        |queue-N|
   +---+---+ +---+---+ +---+---+ +---+---+        +---+---+
       |         |         |         |                |
       v         v         v         v                v
  subtask-0 subtask-1 subtask-2 subtask-3       subtask-N

  offset/checkpoint:
      MsgSlotRing done prefix -> committable offset -> Tide checkpoint
```

### 4.2 UnifiedConsumer key

共享维度不能只按 cluster，否则不同 group/topic/security config 会错误混用。建议 key：

```text
unifiedConsumerKey = {
  cluster,
  groupId,
  normalized_topic_set,
  security_protocol,
  sasl_user_or_identity,
  extra_rdkafka_conf_fingerprint
}
```

首期线上场景中，多个 subtask 的 key 相同，能合并到同一个 UnifiedConsumer。

### 4.3 关键设计决策

| 决策点 | 选择 | 原因 |
|---|---|---|
| 跨节点分区协调 | 默认 `subscribe()` | 100 节点下由 Kafka group protocol 保证无重叠/遗漏 |
| 节点内路由 | poll thread dispatch 到 subtask queue | 收敛 Kafka handle，同时保持 subtask 隔离 |
| handle 数 | 默认 1，最大 4 | 线程数硬控 ≤ 128；高吞吐可扩 |
| Offset | Tide checkpoint 为准，可选同步 Kafka commit | 与 barrier 对齐，保证恢复语义 |
| 背压 | bounded queue + pause/resume | 慢 subtask 只暂停相关 partition |
| 消息生命周期 | MsgSlotRing | 创建/销毁都在 poll thread |
| Metrics | UDS + HTML/JSON | 不新增对外端口，不依赖外部服务 |

### 4.4 接口草案

```cpp
class UnifiedConsumer {
public:
    struct Config {
        std::string cluster;
        std::string groupId;
        std::vector<std::string> topics;

        // 默认 1；高吞吐可配置 2~4；线程数预算强约束 <= 128。
        int pollThreadCount = 1;

        // per-subtask queue / ring 参数。
        int queueCapacity = 1024;
        int highWatermark = 800;
        int lowWatermark = 400;
        int ringCapacity = 4096;

        std::unordered_map<std::string, std::string> rdkafkaConf;
    };

    static std::shared_ptr<MessageQueue> registerQueue(
        const Config& config,
        const std::string& subtaskId);

    static void unregisterQueue(std::shared_ptr<MessageQueue> queue);

    static Metrics getMetrics(
        const std::string& cluster,
        const std::string& groupId);
};

class MessageQueue {
public:
    Status readMsg(MsgSlot** slot, int timeoutMs);
    void ack(MsgSlot* slot);
    std::map<TopicPartition, int64_t> committableOffsets();
    size_t depth() const;
};
```

---

## 5. 分布式安全：默认 subscribe，不默认 assign

### 5.1 为什么不能把 manual assign 作为默认方案

当前问题发生在单机线程爆炸，但真实部署是分布式集群：可能有 100 个消费节点。若每个节点手动 `assign()`，正确性依赖 Tide scheduler 全局分区视图：

```text
风险 1: 两个节点 assign 同一 partition -> 重复消费
风险 2: 某个 partition 未被 assign        -> 消息堆积/丢失处理
风险 3: 节点异常退出后未及时重分配       -> 可用性下降
风险 4: topic partition 扩容后路由滞后    -> 新 partition 长期无人消费
```

因此首期默认：**用 Kafka 自己的 group protocol 做节点间协调。**

### 5.2 100 节点模型

```text
+--------------------------------------------------------------------------------+
|                           分布式模型：subscribe + group protocol                  |
+--------------------------------------------------------------------------------+

   Node-0                       Node-1                         Node-99
   +------------------+         +------------------+           +------------------+
   | UnifiedConsumer  |         | UnifiedConsumer  |           | UnifiedConsumer  |
   | subscribe(topics)|         | subscribe(topics)|           | subscribe(topics)|
   | group.id = G     |         | group.id = G     |           | group.id = G     |
   +--------+---------+         +--------+---------+           +--------+---------+
            |                            |                              |
            +----------------------------+------------------------------+
                                         |
                                         v
                         Kafka Group Coordinator / Rebalance
                                         |
            +----------------------------+------------------------------+
            |                            |                              |
            v                            v                              v
     partitions subset A          partitions subset B            partitions subset Z
            |                            |                              |
            v                            v                              v
     local dispatch               local dispatch                 local dispatch
     to subtask queues            to subtask queues              to subtask queues
```

### 5.3 Rebalance 回调

```text
on_partitions_assigned(partitions):
  1. 根据 Tide checkpoint 查询每个 partition 起始 offset
  2. seek(partition, checkpoint_offset)
  3. 建立 partition -> local subtask queue 路由
  4. 初始化该 partition 的 MsgSlotRing offset 状态
  5. 标记 partition 为 consuming

on_partitions_revoked(partitions):
  1. pause(revoked partitions)
  2. 停止向这些 partition 的 queue dispatch 新消息
  3. 等待 in-flight slot 完成或到达 revoke drain timeout
  4. 计算 DONE prefix 对应的 committable offset
  5. 写入 Tide checkpoint / 可选 commitSync
  6. 删除 partition -> queue 路由
```

### 5.4 assign 的保留用途

`assign()` 只作为以下场景的显式可选模式：

| 场景 | 是否允许 assign | 条件 |
|---|---|---|
| 单机单测 | 允许 | 便于 deterministic 测试 |
| 压测工具 | 允许 | 明确指定 partition 范围 |
| Tide scheduler 已有强一致分区分配 | 谨慎允许 | 必须有重叠/遗漏校验与故障恢复 |
| 默认生产路径 | 不允许 | 使用 `subscribe()` |

---

## 6. 节点内 dispatch 与背压

### 6.1 Partition 亲和路由

poll thread 从 Kafka 拉到消息后，按 `topic + partition` 路由到本节点 subtask queue：

```text
route_key = topic + partition
target    = RouteTable[route_key]

保证:
  同一个 partition 的消息总是进入同一个 subtask queue
  同一个 partition 内顺序不被打乱
```

### 6.2 背压状态机

```text
queue capacity = 1024
high watermark = 800
low watermark  = 400

poll thread                         queue state              action
--------------------------------------------------------------------------------
consume msg                         depth < 800              push slot
consume msg                         depth >= 800             pause(partition)
partition paused                    depth > 400              keep paused
partition paused                    depth <= 400             resume(partition)
ring free slots exhausted           ring full                pause(affected partition)
```

### 6.3 慢 subtask 隔离

```text
+--------------------------------------------------------------------------------+
|                                慢消费者隔离                                      |
+--------------------------------------------------------------------------------+

  partition p0 -> queue-0 -> subtask-0  OK
  partition p1 -> queue-1 -> subtask-1  SLOW, depth >= high watermark
  partition p2 -> queue-2 -> subtask-2  OK

  动作:
    pause(p1)
    continue consume p0/p2

  结果:
    慢 subtask 只影响自己的 partition，不阻塞整个 poll thread。
```

---

## 7. MsgSlotRing：同线程消息生命周期管理

### 7.1 要解决的问题

简单 queue 解耦会产生一个隐患：`RdKafka::Message` 在 poll thread 创建，却在 subtask thread 销毁。

```text
Poll Thread                         Subtask Thread
--------------------------------------------------------------------------------
msg = consumer->consume()   ----->   process(msg)
                                      delete msg

问题:
  1. 跨线程 delete 触发 allocator remote free
  2. cache line bouncing
  3. 高频 new/delete 造成碎片
  4. subtask 线程承担本不该承担的内存管理成本
```

### 7.2 设计原则

**poll thread 是唯一内存管理者**：

```text
创建 RdKafka::Message: poll thread
销毁 RdKafka::Message: poll thread
subtask thread:        只读 payload + atomic 标记 DONE
```

### 7.3 Ring 结构

```text
+--------------------------------------------------------------------------------+
|                                  MsgSlotRing                                     |
+--------------------------------------------------------------------------------+

  fixed-size ring, poll thread owns allocation/reclaim

  +------+------+------+------+------+------+------+------+------+------+
  | s0   | s1   | s2   | s3   | s4   | s5   | s6   | s7   | ...  | sN   |
  +------+------+------+------+------+------+------+------+------+------+
    ^                    ^                         ^
    |                    |                         |
 reclaim_pos          in-flight                   tail

  slot state:

      FREE -> FILLED -> DISPATCHED -> DONE -> FREE
        ^       |           |          |       ^
        |       |           |          |       |
        |   consume()   push queue  subtask  poll thread
        |   fill slot              ack done  batch reclaim
        +--------------------------------------+
```

### 7.4 生命周期

```text
Poll Thread:

  loop:
    1. reclaim:
       while slots[reclaimPos].state == done:
           delete slots[reclaimPos].msg
           slots[reclaimPos].msg = nullptr
           slots[reclaimPos].state = free
           reclaimPos++

    2. acquire:
       if slots[tail].state != free:
           pause(partition)
           continue

    3. consume:
       msg = consumer->consume(timeout)
       if msg is invalid:
           continue

    4. fill slot:
       slot.msg = msg
       slot.payloadPtr = msg->payload()
       slot.payloadLen = msg->len()
       slot.topic = msg->topic_name()
       slot.partition = msg->partition()
       slot.offset = msg->offset()
       slot.state = filled

    5. dispatch:
       queue = route(slot.topic, slot.partition)
       slot.state = dispatched
       queue.push(&slot)
       tail++


Subtask Thread:

  loop:
    slot = queue.pop(timeout)
    process(slot->payloadPtr, slot->payloadLen)
    slot->state.store(done, release)
```

### 7.5 Offset tracking 与 ring 的天然对齐

Kafka offset commit 需要“连续已处理”的最大 offset。Ring 的连续 `done prefix` 正好表达这个语义：

```text
slots:   [done] [done] [done] [dispatched] [filled]
offset:    100    101    102       103        104

committable offset = 103
含义: offset < 103 的消息都已完成，下一次可从 103 恢复。
```

注意：实际实现需要按 `topic-partition` 维护 ring/sequence，不能把不同 partition 的 offset 混在一个全局连续序列中。

### 7.6 接口草案

```cpp
enum class SlotState {
    free,
    filled,
    dispatched,
    done,
};

struct MsgSlot {
    std::atomic<SlotState> state{SlotState::free};
    RdKafka::Message* msg = nullptr;

    std::string topic;
    int32_t partition = -1;
    int64_t offset = -1;

    const void* payloadPtr = nullptr;
    size_t payloadLen = 0;
};

class MsgSlotRing {
public:
    explicit MsgSlotRing(size_t capacity);

    // poll thread only
    MsgSlot* acquire();
    size_t reclaim();
    void fill(MsgSlot* slot, RdKafka::Message* msg);

    // checkpoint / metrics
    int64_t committableOffset(const TopicPartition& tp) const;
    size_t inFlight() const;
    size_t freeSlots() const;

private:
    std::vector<MsgSlot> slots_;
    size_t tail_ = 0;
    size_t reclaimPos_ = 0;
};
```

### 7.7 性能预期

| 指标 | 普通 queue 方案 | MsgSlotRing |
|---|---|---|
| subtask 侧释放成本 | `delete msg`，可能 remote free | `atomic store` |
| poll thread 内存访问 | 分散 new/delete | 顺序 ring reclaim |
| 内存碎片 | 高 | ring 预分配，低碎片 |
| 背压信号 | queue depth + 额外状态 | ring full 即背压 |
| offset tracking | 额外 ack map/bitmap | `done prefix` 天然表达 |

---

## 8. 高吞吐扩展：多 poll thread / 多 handle，但硬控 ≤ 128

### 8.1 默认 1 handle

```text
+--------------------------------------------------------------------------------+
|                              默认模式：单 handle                                  |
+--------------------------------------------------------------------------------+

  UnifiedConsumer
      |
      +-- KafkaConsumer handle-0
              |
              +-- rdk:main
              +-- rdk:broker for active leaders
              +-- poll-thread-0 -> dispatch all assigned partitions

  优点:
    线程最少，适合绝大多数场景。
```

### 8.2 高吞吐 2~4 handle

当单 poll thread CPU 接近瓶颈时，可以在同一个 process 内创建多个 consumer handle，并加入同一个 group。Kafka group protocol 会把 partition 子集分给不同 handle。

```text
+--------------------------------------------------------------------------------+
|                         高吞吐模式：partition-affine handles                     |
+--------------------------------------------------------------------------------+

  UnifiedConsumer(pool_size = 4, same group.id)

      +-- handle-0 -> poll-thread-0 -> partitions subset A -> queues
      +-- handle-1 -> poll-thread-1 -> partitions subset B -> queues
      +-- handle-2 -> poll-thread-2 -> partitions subset C -> queues
      +-- handle-3 -> poll-thread-3 -> partitions subset D -> queues

  特性:
    1. 每个 handle 只消费自己被分配的 partitions
    2. partition 内顺序仍由单 poll thread 保证
    3. 扩容/缩容通过 Kafka rebalance 完成
```

### 8.3 扩容触发条件

| 信号 | 阈值 | 动作 |
|---|---:|---|
| poll_thread_cpu_percent | > 80% 持续 30s | pool_size + 1 |
| poll_idle_percent | < 10% 持续 30s | pool_size + 1 |
| consumer_lag 增长 | 连续 5min 增长 | pool_size + 1 或排查 downstream |
| queue_depth 高但 poll idle 高 | queue > 90% 且 idle > 50% | 不扩 poll，瓶颈在 subtask |
| poll_thread_cpu_percent | < 20% 持续 5min | pool_size - 1 |

### 8.4 线程预算表

| 模式 | handle 数 | 预估 active broker threads | rdk 线程预算 | 说明 |
|---|---:|---:|---:|---|
| 低/中流量 | 1 | 30~80 | 31~81 | 默认 |
| 高流量 | 2 | 40~60/handle | 82~122 | 需 metrics 证明 |
| 极高流量 | 3~4 | 动态限制 | ≤ 128 | 超预算禁止继续扩 |

扩容不是无条件增加 handle，而是受全局预算约束：

```text
if estimated_rdk_threads_after_scale > 128:
    reject scale-out
    emit alert: rdk_thread_budget_exceeded
```

---

## 9. Metrics 与本地 HTML 观测

### 9.1 五层指标

```text
+--------------------------------------------------------------------------------+
|                                Metrics 分层                                      |
+--------------------------------------------------------------------------------+

Layer 1: Kafka / librdkafka
  - consumer_lag_per_partition
  - broker_rtt_ms
  - fetch_bytes_per_sec
  - broker_connection_count
  - rebalance_count

Layer 2: Poll thread
  - poll_msgs_per_sec
  - poll_bytes_per_sec
  - poll_batch_size_avg
  - poll_thread_cpu_percent
  - poll_idle_percent
  - dispatch_latency_us

Layer 3: Queue / MsgSlotRing
  - queue_depth
  - queueCapacity
  - queue_push_per_sec
  - queue_pop_per_sec
  - queue_full_pause_count
  - queue_resume_count
  - ring_inflight
  - ring_free_slots
  - ring_reclaim_per_sec

Layer 4: Subtask
  - process_msgs_per_sec
  - process_latency_us
  - ack_pending_count
  - checkpoint_offset_committed

Layer 5: Process/global
  - total_rdk_threads
  - consumer_handle_count
  - end_to_end_latency_ms
  - consumer_throughput_ratio
```

### 9.2 判断瓶颈的位置

| 现象 | 判断 | 动作 |
|---|---|---|
| lag 增长，poll CPU 高，queue 不深 | poll 瓶颈 | 增加 `pollThreadCount` |
| lag 增长，queue 深，poll idle 高 | subtask 慢 | 排查业务处理，不能盲目扩 poll |
| broker RTT 高，fetch 低 | Kafka/网络瓶颈 | 排查 broker/network |
| ring free slots 低 | in-flight 太多 | 增大 ring 或定位慢 partition |
| total_rdk_threads > 128 | handle 泄漏或扩容失控 | 阻断扩容并报警 |

### 9.3 Unix Domain Socket 导出

不新开 TCP 端口；每个 worker 进程按监听端口创建独立 socket：

```text
/var/run/tide/
├── discovery.json
├── worker_6511.sock
├── worker_7511.sock
├── worker_8511.sock
└── ...
```

接口：

```text
GET /             -> 自包含 HTML Dashboard，1s 自动刷新
GET /json         -> 原始 JSON，便于脚本消费
GET /prometheus   -> Prometheus text format，可选
GET /cluster      -> 同机多 worker 聚合视图
```

使用方式：

```bash
# 单 worker JSON
curl --unix-socket /var/run/tide/worker_6511.sock http://localhost/json | jq .

# 单 worker HTML，通过本地临时 TCP 转发给浏览器
socat TCP-LISTEN:9900,fork UNIX-CONNECT:/var/run/tide/worker_6511.sock
# 浏览器打开 http://localhost:9900/

# 批量查看同机 worker
for sock in /var/run/tide/worker_*.sock; do
  echo "=== $sock ==="
  curl -s --unix-socket "$sock" http://localhost/json | jq '.summary'
done
```

### 9.4 HTML Dashboard 草图

```text
+--------------------------------------------------------------------------------+
| Tide Kafka Consumer Dashboard                         worker:6511 refresh:1s     |
+--------------------------------------------------------------------------------+
| Summary                                                                        |
|   Handles: 1/4       rdk Threads: 56/128       Rebalances: 2                   |
|   Throughput: 42.3k msg/s                  Lag: 1,204                          |
|                                                                                |
| Poll Threads                                                                   |
|   poll-0  CPU 34%  idle 62%  msgs/s 42.3k  batch 128                           |
|                                                                                |
| Queues                                                                         |
|   subtask      depth/cap    push/s   pop/s   paused   state                    |
|   task-0       120/1024     5.2k     5.1k    0        OK                       |
|   task-1       890/1024     5.3k     2.1k    3        SLOW                     |
|                                                                                |
| MsgSlotRing                                                                    |
|   capacity 4096   in-flight 1055   free 3041   reclaim/s 41.8k                 |
|   [████████████░░░░░░░░░░░░░░░░░░░░] 25% used                                  |
|                                                                                |
| Partitions                                                                     |
|   topic:partition       offset       lag       leader      state                |
|   access_out:0          883412       23        broker-5    consuming            |
|   access_out:1          771209       102       broker-12   paused               |
+--------------------------------------------------------------------------------+
```

### 9.5 多进程隔离与 discovery

```text
进程启动:
  1. mkdir -p /var/run/tide
  2. bind /var/run/tide/worker_<listen_port>.sock
  3. chmod 0660 worker_<listen_port>.sock
  4. flock discovery.json.lock
  5. write discovery.json.tmp
  6. rename discovery.json.tmp -> discovery.json

进程退出:
  1. unlink worker_<listen_port>.sock
  2. 原子更新 discovery.json 移除自己

异常退出:
  1. 新进程启动时 connect 旧 socket
  2. connect 失败则判定 stale socket
  3. unlink 后重新 bind
```

`discovery.json` 示例：

```json
{
  "workers": [
    {"pid": 3016658, "port": 6511, "sock": "worker_6511.sock", "started": "2026-05-09T10:00:00Z"},
    {"pid": 3017975, "port": 7511, "sock": "worker_7511.sock", "started": "2026-05-09T10:03:00Z"}
  ]
}
```

选择 `worker_<port>.sock` 而不是 `worker_<pid>.sock`：

- 排障入口通常是端口：`6511 端口的进程线程爆炸`。
- 端口在服务部署中更稳定，PID 重启后会变化。
- 与本次 GDB/日志追踪流程一致。

---

## 10. Day-0 配置止血

在代码改造前，可先通过 librdkafka 配置减少非活跃连接。该方案只能止血，不能根治 handle 数放大。

```text
connections.max.idle.ms = 60000
topic.metadata.refresh.sparse = true
metadata.request.timeout.ms = 30000
```

预期：

```text
改前:
  28 handles × 360 brokers ≈ 10,080 broker threads

配置止血后:
  handle 数仍是 28
  每 handle 活跃 broker 可能下降到几十个
  线程数可能降到 1000~2000 级别

根治:
  必须减少 handle 数，即 UnifiedConsumer。
```

---

## 11. 落地计划：TDD + 日志追溯，先单场景闭环

### 11.1 Phase 0：观测基线

```text
+--------------------------------------------------------------------------------+
| Phase 0: baseline                                                               |
+--------------------------------------------------------------------------------+
| 1. 增加/确认 consumer 创建日志：cluster/group/topic/subtask/handle_id            |
| 2. 增加 rdk thread count 采样脚本                                                |
| 3. GDB attach 前 cd /proc/$pid/cwd，确认符号完整                                  |
| 4. 记录改造前 handle_count、rdk:main、rdk:broker、lag、throughput                 |
+--------------------------------------------------------------------------------+
```

验收：能稳定复现 28 handle × 360 broker 的线程模型。

### 11.2 Phase 1：UnifiedConsumer 骨架

```text
+--------------------------------------------------------------------------------+
| Phase 1: shared handle                                                          |
+--------------------------------------------------------------------------------+
| 1. 定义 `unifiedConsumerKey`                                                     |
| 2. 实现 `registerQueue()` / `unregisterQueue()`                                  |
| 3. 同 key 多次 `registerQueue()` 只创建 1 个 KafkaConsumer handle                |
| 4. 使用 subscribe(topics)，接入 rebalance callback                                |
+--------------------------------------------------------------------------------+
```

TDD：

| 测试 | 验收 |
|---|---|
| 同 key 注册 28 次 | handle_count = 1 |
| 不同 group 注册 | handle_count 分离 |
| 不同 security config 注册 | handle_count 分离 |

### 11.3 Phase 2：Dispatch + bounded queue

```text
+--------------------------------------------------------------------------------+
| Phase 2: dispatch                                                               |
+--------------------------------------------------------------------------------+
| poll thread consume -> route(topic, partition) -> subtask queue                  |
| queue 满 -> pause(partition)                                                     |
| queue 降到 low watermark -> resume(partition)                                    |
+--------------------------------------------------------------------------------+
```

TDD：

| 测试 | 验收 |
|---|---|
| 多 partition dispatch | 无错误路由 |
| 同 partition 顺序 | offset 单调 |
| queue high watermark | 触发 pause |
| queue low watermark | 触发 resume |

### 11.4 Phase 3：MsgSlotRing

```text
+--------------------------------------------------------------------------------+
| Phase 3: same-thread lifecycle                                                  |
+--------------------------------------------------------------------------------+
| 1. poll thread `acquire()` / `fill()` / `reclaim()`                              |
| 2. subtask 只拿 MsgSlot*                                                         |
| 3. `ack()` 只做 state `done`                                                     |
| 4. `reclaim()` 时在 poll thread delete `RdKafka::Message`                        |
+--------------------------------------------------------------------------------+
```

TDD：

| 测试 | 验收 |
|---|---|
| slot 状态流转 | `free -> filled -> dispatched -> done -> free` |
| subtask ack | 不 delete message |
| reclaim | delete 发生在 poll thread |
| committable offset | 返回连续 `done prefix` 的下一个 offset |

### 11.5 Phase 4：KafkaSource 适配

```text
+--------------------------------------------------------------------------------+
| Phase 4: KafkaSource integration                                                |
+--------------------------------------------------------------------------------+
| KafkaSource::Open()    -> UnifiedConsumer::registerQueue()                       |
| KafkaSource::ReadMsg() -> MessageQueue::readMsg()                                |
| KafkaSource::Ack()     -> MessageQueue::ack()                                    |
| KafkaSource::Close()   -> UnifiedConsumer::unregisterQueue()                     |
+--------------------------------------------------------------------------------+
```

验收：现有 KafkaSource 作业端到端跑通，且日志显示同 key 只创建 1 个 handle。

### 11.6 Phase 5：Metrics UDS

```text
+--------------------------------------------------------------------------------+
| Phase 5: observability                                                          |
+--------------------------------------------------------------------------------+
| 1. Metrics snapshot                                                              |
| 2. /var/run/tide/worker_<port>.sock                                              |
| 3. GET /json                                                                     |
| 4. GET / HTML dashboard                                                          |
| 5. discovery.json + /cluster                                                     |
+--------------------------------------------------------------------------------+
```

验收：

```bash
curl --unix-socket /var/run/tide/worker_6511.sock http://localhost/json | jq .summary
```

能看到：

```text
handle_count
rdk_thread_count
broker_connection_count
poll_msgs_per_sec
queue_depth
ring_inflight
consumer_lag
```

---

## 12. 最终验收标准

### 12.1 线程数

```text
验收场景:
  cluster = bmq_data_sys
  broker = 360
  topics = 当前 10 个 topic
  local subtask = 28

改造前:
  rdk:main   ≈ 28
  rdk:broker ≈ 28 × 360 = 10,080
  rdk total  ≈ 10,108+

改造后默认:
  rdk:main   = 1
  rdk:broker = active leaders only
  rdk total  ≤ 128
```

### 12.2 正确性

| 维度 | 验收 |
|---|---|
| 分布式分区 | 100 节点下无 partition 重叠/遗漏 |
| 顺序性 | 同 partition 内 offset 单调处理 |
| checkpoint | barrier 后恢复不丢消息，重复在可接受语义内 |
| rebalance | revoke/assign 后 offset 正确衔接 |
| 背压 | 慢 subtask 不导致 OOM，不阻塞无关 partition |

### 12.3 性能

| 指标 | 验收 |
|---|---|
| 吞吐 | 不低于旧模型，或通过 2~4 handle 扩展补齐 |
| 延迟 | queue/dispatch 延迟可观测，异常可定位 |
| CPU | poll thread CPU 有扩容阈值保护 |
| 内存 | ring/queue bounded，无无限增长 |

---

## 13. 调试备忘

```bash
# 找端口对应进程
ss -tlnp | grep ':6511'

# 线程名分布
cd /proc/$pid/task
for tid in $(ls); do cat $tid/comm; done | sort | uniq -c | sort -rn

# GDB attach：必须先进入进程 cwd，保证 ./lib/ 符号可加载
cd /proc/$pid/cwd
gdb -batch -nx \
  -ex "set pagination off" \
  -ex "attach $pid" \
  -ex "info threads" \
  -ex "thread <gdb_thread_no>" \
  -ex "bt 20" \
  -ex "detach"

# 注意：thread find 只负责查找，不会自动切换线程；必须显式 thread <gdb_thread_no> 后再 bt。

# 日志追溯 consumer 创建
strings $logdir/tide_worker.log | grep "[Created] cluster"

# 统计 rdk broker 线程
cd /proc/$pid/task
for tid in $(ls); do cat $tid/comm; done | grep 'rdk:broker' | wc -l
```

---

## 14. 参考锚点

- Consumer 创建点：`src/source/kafka/consumer_obj.cpp:25`
- Producer 侧已有 cluster 级复用参考：`src/sink/producer_manager/producer_manager.cpp:82`
- librdkafka 后台线程/关闭语义：`inf/librdkafka/INTRODUCTION.md:875`
- librdkafka 配置：`inf/librdkafka/CONFIGURATION.md`
