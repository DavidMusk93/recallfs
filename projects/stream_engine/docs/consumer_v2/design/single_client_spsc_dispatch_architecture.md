# Consumer V2 Single Kafka Client + SPSC Dispatch Architecture

## 1. Design Goal

`consumer_v2` 的下一代调度架构必须坚持一个核心目标：

> 一个 shared consumer 尽可能只使用一个 Kafka client；Kafka client 之后的所有解耦边界都使用 bounded SPSC lock-free queue；record / dispatch / ack hot path 不碰 mutex。

这不是"把当前 single dispatcher 换成更多 dispatcher"这么简单，而是重新定义所有权：

- Kafka client 只属于一个 owner。
- partition state 只属于一个 shard。
- worker input lane 只属于一个 producer shard 和一个 consumer worker。
- ack lane 只属于一个 producer worker 和一个 consumer shard。
- commit / pause / resume 只由 Kafka owner 触碰 Kafka API。

高性能的关键不是"更多线程"或"更快 MPSC queue"，而是让 hot path 上的每条边都天然只有一个 producer 和一个 consumer。

***

## 2. Non-Goals

- 不通过增加 Kafka client / KafkaConsumer handle 来扩吞吐。
- 不在 hot path 使用全局 `mutex_`、condition-variable 调度、全局 ready queue 仲裁。
- 不让 worker 线程直接执行调度逻辑。
- 不让 dispatch shard 或 worker 线程调用 Kafka `pause` / `resume` / `commit` API。
- 不把 MPSC queue 当作默认抽象；如果出现 MPSC，说明 ownership 还没拆干净。

***

## 3. Current Problem

Phase3 后的当前形态解决了一部分 data race 和锁竞争，但本质仍是单点串行：

```text
poll threads ── lock-free command queue ── single dispatcher owner ── worker mailbox
worker ack  ── lock-free command queue ──┘
worker demand ───────────────────────────┘
```

它的问题是：

- Kafka client 的单 owner 被错误延伸成 dispatch state 的单 owner。
- poll ingress、ack、worker demand 全部串到同一个 dispatcher loop。
- worker mailbox 空时曾经会反向抢全局调度锁，修复后也只是把 demand 串回单 dispatcher。
- partition buffer、ready queue、lease、acked offset、pause desired state 都集中在一个大对象里。
- queue 虽然 lock-free，但 command queue 是多生产者单消费者，仍需要全局排队和 cache line 竞争。

正确方向是：Kafka client 单 owner，dispatch state 多 owner。

### 3.1 Dispatcher 仍在 mutex\_ 内运行（关键事实）

当前 dispatcher owner 虽然序列化了 command 处理顺序，但每条 command 的 apply 函数仍然持有全局 `mutex_`：

```text
dispatcherLoop():
  pop command from boost::lockfree::queue
  applyIngressBatch()   → std::lock_guard<std::mutex>(mutex_)
  applyAckBatch()       → std::lock_guard<std::mutex>(mutex_)
  applyWorkerDemand()   → std::lock_guard<std::mutex>(mutex_)
```

dispatcher 只消除了 command 入队竞争，没有消除状态修改竞争。readBatch 慢路径也取 `mutex_`，与 dispatcher 形成 hot path 上的双向锁竞争。

### 3.2 MsgSlotRing 双重 mutex

`MsgSlotRing` 内部有独立的 `ringMutex_`，加上外层 `mutex_`，hot path 实际是双重 mutex：

```text
applyIngressBatch():
  lock(mutex_)                    ← 全局锁
    msgSlotRing_.fillBatch()
      lock(ringMutex_)            ← ring 内部锁
```

stress test 证据：ring slot 批量 acquire/fill 优化带来 2.67x 提升，说明 `ringMutex_` 在 hot path 上有显著成本。

### 3.3 对象构造成本

stress test 最终阶段（958K msg/s 后）perf 热点转向：

- `ConsumedMessageDraft`：per-record 构造，包含 `TopicPartition` string copy
- `std::unique_ptr<PublishedMessage>`：per-record heap allocation
- `DispatchEnvelope`：`std::string topic` + `std::vector<void*> slots` + `std::vector<int64_t> offsets`

SPSC queue 再快也吃不掉每条 record 的 heap allocation 和 string copy。

### 3.4 代码膨胀

当前 `unified_consumer.cpp` 4374 行，包含 `SharedConsumerState`（\~2300 行）、`UnifiedConsumerRegistry`（\~300 行）、`ConsumerV2DebugSocketService`（\~900 行）等多个大类全部塞在一个编译单元里。新架构必须通过分层解决代码膨胀问题。

***

## 4. Target Model

### 4.1 High Level

```text
                       ┌────────────────────────────┐
                       │ KafkaClientOwner           │
                       │ one KafkaConsumer          │
                       │ poll / pause / resume      │
                       │ commit / rebalance         │
                       └──────────────┬─────────────┘
                                      │ demux by partition owner
             ┌────────────────────────┼────────────────────────┐
             │                        │                        │
             ▼                        ▼                        ▼
  ┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐
  │ DispatchShard 0     │   │ DispatchShard 1     │   │ DispatchShard N     │
  │ partition buffers   │   │ partition buffers   │   │ partition buffers   │
  │ ready queue         │   │ ready queue         │   │ ready queue         │
  │ leases              │   │ leases              │   │ leases              │
  │ acked offsets       │   │ acked offsets       │   │ acked offsets       │
  │ pause desired state │   │ pause desired state │   │ pause desired state │
  │ shard-local slots   │   │ shard-local slots   │   │ shard-local slots   │
  └──────────┬─────────┘   └──────────┬─────────┘   └──────────┬─────────┘
             │                        │                        │
             ▼                        ▼                        ▼
        worker lanes              worker lanes              worker lanes
             │                        │                        │
             ▼                        ▼                        ▼
          Workers                  Workers                  Workers
```

Kafka owner 后面可以有多个 dispatch shard，但 shard 不碰 Kafka client。

### 4.2 Concrete Data Structures

数据结构目标是复用成熟库，而不是在 consumer\_v2 里重新实现并发 primitive。

```text
KafkaClientOwner
  kafkaConsumer: unique KafkaConsumer owner
  shardIngress: vector<SpscLane<IngressBatch>>
  shardCommit: vector<SpscLane<CommitEvent>>
  shardControl: vector<SpscLane<ControlEvent>>
  ownerSignals: vector<LaneSignal>

DispatchShard[i]
  ingress: SpscLane<IngressBatch>&
  commitOut: SpscLane<CommitEvent>&
  controlOut: SpscLane<ControlEvent>&
  workerOut: vector<SpscLane<DispatchBatch>*>
  ackIn: vector<SpscLane<AckEvent>*>
  partitionTable: vector<PartitionState>
  readyQueue: local intrusive ring / deque<uint32_t>
  laneCredit: vector<uint16_t>
  slotPool: ShardSlotPool
  signal: LaneSignal

Worker[j]
  inputLanes: vector<SpscLane<DispatchBatch>*>
  ackLanes: vector<SpscLane<AckEvent>*>
  token: WorkerToken
  signal: LaneSignal
```

推荐类型映射：

| Logical structure | Concrete choice                         | Notes                                       |
| ----------------- | --------------------------------------- | ------------------------------------------- |
| SPSC data lane    | `folly::ProducerConsumerQueue<T>`       | 通过 `SpscLane<T>` 封装；不自研 queue               |
| Lane wait signal  | `folly::Baton`                          | 通过 `LaneSignal` 封装；只负责轻量级 park/unpark，不承载数据 |
| Partition table   | `std::vector<PartitionState>`           | shard owner 独占修改，不需要并发容器                    |
| Ready queue       | `std::deque<uint32_t>` 或 fixed ring     | shard 本地结构，不跨线程共享                           |
| Lane matrix       | `std::vector<std::vector<SpscLane<T>>>` | 构造期固定，运行期不扩容                                |
| Token registry    | owner-thread local vector/map           | 控制面使用，非 hot path                            |

`folly::MPMCQueue`、`boost::lockfree::queue` 这类多生产者队列不作为新架构默认结构。它们只能用于 legacy adapter 或临时桥接，不能进入目标 hot path。

### 4.3 Ownership

| Object                 | Exclusive owner        | Mutated by                 | Notes                     |
| ---------------------- | ---------------------- | -------------------------- | ------------------------- |
| Kafka client           | `KafkaClientOwner`     | Kafka owner thread         | 唯一线程调用 Kafka API          |
| Partition assignment   | Kafka owner            | Kafka owner thread         | rebalance 后发布 token 变更    |
| Partition buffer       | `DispatchShard`        | shard thread               | 按 partition token 归属      |
| Ready queue            | `DispatchShard`        | shard thread               | 不跨 shard 共享               |
| Lease state            | `DispatchShard`        | shard thread               | lease ack 必须回 home shard  |
| Acked offset           | `DispatchShard`        | shard thread               | shard 产出 commit candidate |
| Shard slot pool        | `DispatchShard`        | shard thread               | slot reclaim 不需全局 mutex   |
| Commit execution       | Kafka owner            | Kafka owner thread         | 聚合 shard candidates 后提交   |
| Pause/resume execution | Kafka owner            | Kafka owner thread         | shard 只上报 desired state   |
| Worker input lane      | one shard + one worker | shard writes, worker reads | SPSC                      |
| Worker ack lane        | one worker + one shard | worker writes, shard reads | SPSC                      |

### 4.5 Metrics / Debug Snapshot Rule

`/` HTML、`/json`、Prometheus 以及 `getRuntimeInfo()` 属于观测通道，它们必须遵守同一条 Kafka API ownership 规则：

```text
observability thread MUST NOT call Kafka APIs
```

解释：

- `KafkaConsumer::committed()`、`get_watermark_offsets()` 等不是纯读，它们会向 librdkafka 发送 op 并等待回复。
- 如果观测线程直接调用这些 API，会与 Kafka owner 的 poll/commit/callback 交错，导致观测值出现非单调（例如 broker committed offset “前进后回退”）。
- 非单调的 broker committed 观测会把 `brokerLag` / `commitGap` 误用为“消费能力不足”，掩盖真实问题。

正确做法：

```text
Kafka owner thread periodically refreshes lag snapshot
  -> publish snapshot to shared state (mutex/atomic)
  -> observability thread reads cached snapshot only
```

观测语义：

- `ackedLag` 是 consumer_v2 本地处理/ack 进度落后于高水位的 lag，优先用于判断消费是否真的落后。
- `brokerLag` / `commitGap` 是 Kafka group committed 观测，必须标注 freshness / regression，不能直接等价为吞吐瓶颈。

### 4.4 Kafka Group Defaults

生产默认必须走 Kafka group `subscribe()`，不能走 manual `assign()`。manual affinity assignment 只能作为显式 debug/emergency 开关，避免绕过 group coordinator 后每个进程都消费全量 partition。

默认 Kafka group 配置：

| Option                          | Default              | Reason                           |
| ------------------------------- | -------------------- | -------------------------------- |
| `partition.assignment.strategy` | `cooperative-sticky` | 扩缩容时降低全量 revoke/assign 抖动        |
| `session.timeout.ms`            | `45000`              | 容忍短暂网络抖动，降低误判离组             |
| `heartbeat.interval.ms`         | `3000`               | 保持默认心跳节奏，满足 session timeout 约束 |
| `group.instance.id`             | disabled by default  | 滚动发布新旧进程同端口重叠时，自动 static member id 会产生 fencing 风险 |

启用 `cooperative-sticky` 后，rebalance callback 必须使用 `incremental_assign()` / `incremental_unassign()`，不能复用 eager assignor 的 `assign()` / `unassign()`。

`group.instance.id` 只能由外部显式配置。consumer_v2 不能自动按 host/port 生成 static member id，因为发布系统会在同一台机器同一端口上短时间保留新旧进程，自动 id 会导致新旧实例共享同一个 static member identity。

***

## 5. All Edges Are SPSC

### 5.0 SPSC Queue Contract

不要自研 SPSC lock-free queue。项目已经有 Boost/Folly 依赖，数据结构选择应优先复用成熟实现，并在 consumer\_v2 内只暴露一个很薄的 lane wrapper。

推荐封装：

```text
template <class T>
class SpscLane {
  // Implementation: folly::ProducerConsumerQueue<T>.

  bool tryPush(T&& item);
  size_t tryPushBatch(span<T> items);

  bool tryPop(T* item);
  template <class Fn> size_t drain(size_t maxItems, Fn&& fn);

  size_t sizeGuess() const;
  size_t capacity() const;

  void notifyConsumer();
  void notifyProducer();
  void waitForConsumerProgress(uint32_t observedSeq, WaitBudget budget);
  void waitForProducerProgress(uint32_t observedSeq, WaitBudget budget);
};
```

类型选择：

| Candidate                         | Use            | Reason                                                         |
| --------------------------------- | -------------- | -------------------------------------------------------------- |
| `folly::ProducerConsumerQueue<T>` | SPSC data lane | Folly 原生 SPSC bounded queue，语义直接匹配 producer/consumer ownership |
| `boost::lockfree::queue<T>`       | 不用于新 hot path  | 这是 MPMC/MPSC 风格 queue，不表达 SPSC ownership，会掩盖错误拓扑               |
| 自研 ring                           | 禁止作为默认方案       | 除非现有依赖无法满足，避免重新踩 memory ordering / padding / ABA / teardown 坑  |

封装原则：

- consumer\_v2 只依赖 `SpscLane<T>`，不在业务代码散落 Folly/Boost API。
- lane 类型在编译期固定，禁止 runtime polymorphism。
- queue bounded，容量构造后不变。
- queue 元素尽量是 move-only small object，payload / slots 通过 span、index 或 pointer ownership 传递。
- `sizeGuess()` 只用于 metrics 和 backpressure hint，不能作为 correctness 条件。
- batch push / drain 是默认 API，单条 push / pop 只服务低流量控制消息。
- teardown 由 token generation 和 owner stop protocol 保证，不能靠清空 queue 猜状态。

SPSC queue 本身只解决数据传递，不负责阻塞等待。等待/唤醒由 `LaneSignal` 处理，使用 `folly::Baton`。

### 5.1 Kafka Owner to Shard Ingress

Kafka owner 是唯一 producer。每个 shard 是自己的 ingress lane 的唯一 consumer。

```text
KafkaClientOwner ── SPSC ingress lane i ──> DispatchShard i
```

Kafka owner poll 后按 partition owner 分桶：

```text
for record in polledBatch:
    shardId = partitionOwner[record.topic, record.partition]
    shardBucket[shardId].push(record)

for shardId in nonEmptyBuckets:
    ingressLane[shardId].push(batch)
```

这里不需要 MPSC，因为所有 Kafka records 都先经过唯一 Kafka owner。

#### 5.1.1 IngressBatch 零拷贝设计

当前 `IngressBatch` 的问题：per-record `unique_ptr<PublishedMessage>` heap allocation + `TopicPartition` string copy。

目标态 `IngressBatch` 应该是 batch-level move-only object：

```text
struct IngressRecord {
    uint32_t slotIndex;             // index into shard-local slot pool, not pointer
    uint32_t partitionIndex;        // integer, not string
    int64_t offset;
    uint32_t messageLen;
};

struct IngressBatch {
    std::vector<IngressRecord> records;   // move-only, no heap alloc per record
    // PublishedMessage ownership attached to slot, not IngressRecord
};
```

原则：

- record payload 通过 slot index 传递，不做 copy
- `TopicPartition` 用 partition index 替代 string，demux 阶段已知 shard/partition 映射
- `PublishedMessage` ownership attach 到 slot，不 per-record `unique_ptr`
- IngressBatch push 到 SPSC lane 时只有 move 语义，零 heap allocation

### 5.2 Shard to Worker Dispatch

每个 worker lane 只绑定一个 producer shard 和一个 consumer worker。

```text
DispatchShard i ── SPSC worker lane j ──> Worker j
```

如果 worker 需要服务多个 shard，不能让多个 shard 同时写同一个 mailbox。必须把 mailbox 拆成 lanes：

```text
DispatchShard 0 ── SPSC lane 0 ─┐
DispatchShard 1 ── SPSC lane 1 ─┼──> Worker local lane set
DispatchShard N ── SPSC lane N ─┘
```

worker 可以轮询自己的 lane set，但每条 lane 仍然是 SPSC。

lane set 不等于全局共享 mailbox。它只是多个 SPSC consumer endpoints 的集合：

```text
Worker j owns:
  inputLane[shard0][workerJ]
  inputLane[shard1][workerJ]
  inputLane[shard2][workerJ]
```

每条 lane 的 producer 仍然唯一：对应 shard。

### 5.3 Worker to Shard Ack

ack 也必须是 SPSC。做法是按 `(worker, shard)` 建 ack lane：

```text
Worker j ── SPSC ack lane j->i ──> DispatchShard i
```

batch 派发时带 `shardId` 和 `leaseToken`。worker 完成后只写回该 batch 的 home shard ack lane：

```text
ackLane[workerId][batch.shardId].push({
    leaseToken,
    partitionToken,
    lastOffset,
    slotRange,
})
```

这样不需要多个 worker 写同一个 shard ack queue，也不需要 MPSC。

ack lane matrix 的规模是 `workerCount * shardCount`，但每条 lane 的元素很小：

```text
AckEvent {
    LeaseToken lease;
    uint32_t recordCount;
    int64_t lastOffset;
    SlotSpan slots;
}
```

在 `48 workers * 8 shards` 场景下是 384 条 ack lanes。每条 lane 可以很浅，例如 64 或 128 个元素，因为 ack 不应长期排队。这个空间成本换来的是：

- ack 不竞争同一个 shard-wide MPSC queue。
- shard drain ack 时可以按 worker lanes 批量轮询。
- 慢 worker 只堵自己的 ack lane，不影响其它 worker ack 回收。
- 可以精确暴露 `ackLaneDepth{worker,shard}` 定位卡点。

### 5.4 Shard to Kafka Owner Commit

commit candidate 也用每 shard 一条 SPSC lane 回 Kafka owner：

```text
DispatchShard i ── SPSC commit lane i ──> KafkaClientOwner
```

shard 只上报"这个 partition 已经安全推进到 offset X"。Kafka owner 聚合后批量 commit。

### 5.5 Shard to Kafka Owner Control

pause / resume desired state 也用每 shard 一条 SPSC control lane：

```text
DispatchShard i ── SPSC control lane i ──> KafkaClientOwner
```

shard 只写 desired transition：

```text
PauseDesired(partitionToken, targetPaused=true)
ResumeDesired(partitionToken, targetPaused=false)
```

Kafka owner 去重、合并、节流后调用 Kafka API。

***

## 6. Token Model

### 6.1 PartitionToken

`PartitionToken` 表示 partition 的内存态归属。

```text
PartitionToken {
    topic
    partition
    generation
    shardId
    localPartitionIndex
}
```

规则：

- 只有 token 指向的 shard 能修改该 partition 的 buffer / lease / acked offset。
- rebalance 或 shard migration 会生成新 generation。
- 旧 generation 的 in-flight lease 必须 drain、cancel 或显式转移后才能启用新 token。

### 6.2 WorkerToken

`WorkerToken` 表示 worker 当前被允许读取哪些 SPSC lanes。

```text
WorkerToken {
    workerId
    readableLanes: [(shardId, laneId)]
    generation
}
```

规则：

- worker 不向 shard 请求调度。
- worker 只从 token 授权的 lanes 读取 batch。
- token 变更是控制面事件，不在 record hot path 上发生。

### 6.3 LeaseToken

`LeaseToken` 表示一个 batch 的唯一所有权。

```text
LeaseToken {
    shardId
    partitionTokenGeneration
    localLeaseId
    workerId
    beginOffset
    endOffset
}
```

规则：

- ack 必须带原始 `LeaseToken`。
- shard 校验 generation / lease id / worker id 后推进 acked offset。
- 旧 generation ack 到达时，按 revoke/cancel 规则处理，不允许污染新 partition owner。

### 6.4 CommitToken

`CommitToken` 是 shard 到 Kafka owner 的提交候选。

```text
CommitToken {
    partitionToken
    committableOffset
    shardGeneration
}
```

规则：

- Kafka owner 只提交当前 assignment generation 下的 token。
- 同一 partition 多个 candidate 只保留最大连续 offset。

***

## 7. Thread Model

### 7.1 KafkaClientOwner

线程命名：`kcv2-owner`

唯一职责：

- `consume` / `poll`（短 timeout 1-5ms，不长阻塞）
- rebalance callback 收敛
- partition token 创建 / 撤销
- drain shard commit lanes
- drain shard control lanes
- 批量 `pause` / `resume`
- 批量 commit

禁止职责：

- 不维护 partition buffer。
- 不执行 ready queue 扫描。
- 不给 worker 直接派 batch。
- 不等待 worker。

Kafka owner consume timeout 必须很短（1-5ms）或用 `rd_kafka_consumer_poll(0)` + 自己的 LaneSignal 做 idle 等待。commit/control drain 必须在每轮 poll 后立即执行，不能被 consume timeout 卡住。

### 7.2 DispatchShard

线程命名：`kcv2-shard-{i}`（Phase B 时 `kcv2-shard-0` 等价于当前 `kcv2-dispatch`）

每个 shard 是独立线程。`S=8` 时 8 shard + 1 owner + 48 workers = 57 线程，线程预算可控。

唯一职责：

- drain ingress lane
- append partition buffer
- maintain ready queue
- drain `(worker, shard)` ack lanes
- release lease and slot（shard-local slot pool）
- fill worker SPSC lanes
- emit commit candidate
- emit pause/resume desired state

禁止职责：

- 不调用 Kafka API。
- 不等待 worker。
- 不持有全局 registry / lifecycle mutex。
- 不直接迁移 partition ownership。

### 7.3 Worker

线程命名：由上层 subtask 决定（`slot-{i}`）

唯一职责：

- 从 worker token 授权的 SPSC lanes 读取 batch。
- 执行业务处理。
- 按 batch home shard 写 SPSC ack lane。

禁止职责：

- 不做调度。
- 不扫描 partition。
- 不修改 shared consumer state。
- 不碰 Kafka API。

***

## 8. Hot Path

### 8.1 Poll to Dispatch

```text
KafkaOwner.pollBatch()
  -> demux by PartitionToken.shardId
  -> ingressLane[shardId].push(batch)

DispatchShard.drainIngress()
  -> append records to local partition buffers
  -> update local ready queue
  -> fill worker lanes
```

### 8.2 Dispatch to Ack

```text
DispatchShard.fillWorkerLane()
  -> create LeaseToken
  -> move MsgSlot refs into DispatchBatch
  -> workerLane[workerId].push(batch)

Worker.process()
  -> ackLane[workerId][shardId].push(LeaseToken, processed offsets)

DispatchShard.drainAckLane()
  -> validate LeaseToken
  -> advance acked offset
  -> release slots (shard-local)
  -> emit CommitToken if needed
```

### 8.3 Commit / Pause Control

```text
DispatchShard
  -> commitLane[shardId].push(CommitToken)
  -> controlLane[shardId].push(PauseDesired / ResumeDesired)

KafkaOwner
  -> drain all shard commit lanes
  -> coalesce offsets
  -> commit via Kafka client
  -> drain all shard control lanes
  -> coalesce pause/resume
  -> call Kafka pause/resume
```

***

## 9. Backpressure

所有 queue 都必须 bounded。满队列不是"yield 等待"，而是明确 backpressure 信号。

| Queue        | Full means                     | Action                                    |
| ------------ | ------------------------------ | ----------------------------------------- |
| ingress lane | shard 吃不下 poll records         | Kafka owner 标记对应 partitions pause desired |
| worker lane  | worker 吃不下 shard dispatch      | shard 停止给该 lane 派发，换其它 lane               |
| ack lane     | shard 没及时回收 worker ack         | worker 进入短暂自旋/让出，超过预算后上报 fatal metric     |
| commit lane  | Kafka owner 没及时提交候选            | shard 合并本地 candidate，不重复写                 |
| control lane | Kafka owner 没及时执行 pause/resume | shard 合并 desired state，只保留最新              |

原则：

- data queue 满要反压 Kafka poll。
- ack queue 满是严重异常，因为会阻塞 slot reclaim。
- control queue 满时可以合并状态，不能无限堆积。

### 9.1 Lane 容量选择与翻转抑制

当前 watermark 4096/1024 + per-record updatePauseState 是 pause/resume 翻转根因（eBPF 证据：updatePauseState 80,988/s，pausedPartitionCount 在 5\~18 之间抖动）。

新架构的 backpressure 信号应该是 shard ingress lane depth 超过阈值时触发 pause desired，而不是 per-record 检查。

ingress lane 容量应至少能容纳 2-3 个 poll batch（每 batch \~1000-4000 records），避免单次 poll 就打满。建议默认容量：

```text
ingress lane: 16K records 或 16 个 batch slots
worker lane: 4-8 batch slots
ack lane: 64-128 events
commit lane: 256 events
control lane: 128 events
```

backpressure 检查频率：shard 在 drain ingress batch 后检查 lane depth，不 per-record 检查。

***

## 10. Wait Strategy Without Mutex

hot path 不使用 mutex，也不使用 condition variable 做调度唤醒。SPSC queue 只负责传递数据；轻量等待/唤醒由独立的 `LaneSignal` 负责。

推荐封装：

```text
class LaneSignal {
  // Backed by folly::Baton.
  // Exact API is hidden here so consumer_v2 does not depend on Baton details.

  void notify();
  bool wait(uint32_t observedSeq, WaitBudget budget);
  uint32_t sequence() const;

private:
  atomic<uint32_t> seq;
};
```

等待策略分三层：

```text
active spin       短暂等待，预算几十到几百 cycles
cpu relax/yield   lane 暂空但系统仍活跃
folly Baton park  idle path，等待 seq 变化后重新 drain lanes
```

原则：

- `tryPush` 成功从空变非空时，producer 调 `LaneSignal::notify()`。
- consumer drain 后发现无事可做，读取 `seq` 并进入 `wait(seq, budget)`。
- `Baton` wait 只等 sequence 变化，不承载 queue correctness。
- spurious wakeup 必须允许；醒来后重新 drain lanes。
- bounded queue 满时优先走 backpressure，不靠等待原语伪装无限容量。
- `folly::Baton` 只藏在 `LaneSignal`，业务代码不直接调用。

### 10.1 Kafka Owner

Kafka owner 的主节奏由短 timeout poll 驱动：

```text
loop:
  records = kafka.consume(timeout=1ms)
  if records:
      demux records to shard ingress lanes
  drain commit lanes with budget
  drain control lanes with budget
  execute coalesced commit / pause / resume
  if no records and no control work:
      ownerSignal.wait(seq, idleBudget)
```

Kafka owner 不等待 shard，也不等待 worker。consume timeout 必须短（1-5ms），确保 commit/control lane 及时 drain。

### 10.2 DispatchShard

DispatchShard 不用条件变量等待 worker demand。它只观察 lanes：

```text
loop:
  drainedIngress = ingressLane.drain(maxIngress)
  drainedAck = drainAckLanes(maxAck)
  dispatched = fillWorkerLanes(maxDispatch)

  if drainedIngress + drainedAck + dispatched == 0:
      observed = shardSignal.sequence()
      shardSignal.wait(observed, waitBudget)
```

能唤醒 shard 的事件包括：Kafka owner 写入 ingress lane、worker 写入 ack lane、control plane 改 worker token。

### 10.3 Worker

worker 不发 demand command。它只消费 token 授权的 input lanes：

```text
loop:
  for lane in workerToken.readableLanes:
      if lane.tryPop(&batch):
          process batch
          ackLane[batch.shardId].tryPush(ack)
          ackSignal[batch.shardId].notify()
          continue loop

  observed = workerSignal.sequence()
  workerSignal.wait(observed, waitBudget)
```

能唤醒 worker 的事件包括：shard 写入 worker input lane、control plane 更新 worker token、shutdown。

因此 worker 空闲不会反向进入 dispatcher，也不会抢任何调度锁。

***

## 11. Lane Topology and Cost

全 SPSC 的代价是 lane 数量增加。这个代价必须显式设计，而不是退回 MPSC。

### 11.1 Required Lanes

| Direction                    |                 Count | Producer    | Consumer    |
| ---------------------------- | --------------------: | ----------- | ----------- |
| Kafka owner -> shard ingress |                   `S` | Kafka owner | shard i     |
| shard -> worker input        | `S * W` active subset | shard i     | worker j    |
| worker -> shard ack          | `W * S` active subset | worker j    | shard i     |
| shard -> Kafka owner commit  |                   `S` | shard i     | Kafka owner |
| shard -> Kafka owner control |                   `S` | shard i     | Kafka owner |

`S=8, W=48` 时，理论 lane 数：

```text
ingress: 8
worker input: 384
ack: 384
commit: 8
control: 8
total: 792 lanes
```

这看起来多，但每条 lane 是固定 ring，且元素很小。相比单 MPSC queue 的 cache line 竞争，lane matrix 更可预测。

### 11.2 Active Lane Subset

不要求所有 `S * W` lanes 都活跃。

简单策略：

```text
worker j primaryShard = j % shardCount
active input lanes: shard primaryShard -> worker j
active ack lanes: worker j -> primaryShard
```

扩展策略：

```text
worker token grants K shards
active input lanes: K lanes
active ack lanes: K lanes
```

默认 `K=1`，只有负载不均时控制面临时提高到 `K=2` 或 `K=3`。

### 11.3 Why Not One Ack Queue per Shard

一个 shard ack queue 看起来更省 lane，但它是 MPSC：

```text
Worker 0 ─┐
Worker 1 ─┼──> shard ack queue
Worker N ─┘
```

这会重新引入：

- 多 worker CAS 竞争。
- queue node/cache line 抢占。
- ack 延迟相互影响。
- 无法定位哪个 worker ack lane 堵塞。

所以 ack 必须保持 `(worker, shard)` SPSC。

***

## 12. Scheduling Policy

调度策略属于 shard 本地状态，不属于 Kafka owner，也不属于 worker。

每个 shard 维护：

```text
partitionBuffer[localPartitionIndex]
readyQueue
inflightLease[localLeaseId]
workerLaneCredit[workerId]
partitionPauseState
committableOffset
```

核心循环：

```text
drain ingress:
  append records
  if partition reaches full batch threshold:
      readyQueue.push(partition)
  if latency budget exceeded:
      readyQueue.push(partition as partial-ready)

drain ack:
  release lease
  reclaim slots (shard-local)
  advance committable offset
  return worker lane credit

dispatch:
  while readyQueue not empty and worker lane has credit:
      choose partition
      choose worker lane
      create lease
      push batch to SPSC worker input lane
```

`worker lane credit` 取代 demand command。worker input lane 有容量就表示 worker 可被投喂；没有容量就跳过该 lane。

***

## 13. Work Stealing

默认不做 work stealing。原因：

- stealing 会破坏 SPSC 的简单 ownership。
- stealing 后 ack 仍必须回 home shard，容易引入跨 shard复杂性。
- 当前主要矛盾是调度解耦，不是 shard 间负载均衡。

如果后续必须做 stealing，只允许 stealing worker token，而不是 stealing partition state：

```text
Worker gets temporary readable lane from another shard
Batch still belongs to original shard
Ack still returns through Worker->OriginalShard SPSC lane
```

stealing 是控制面低频动作，不进入 record hot path。

***

## 14. Rebalance, Token Migration and Shutdown

### 14.1 Rebalance

rebalance 是唯一允许改变 partition ownership 的场景。

```text
KafkaOwner receives revoke
  -> publish FreezePartitionToken to old shard
  -> old shard stops new dispatch for token
  -> old shard drains or cancels in-flight leases
  -> old shard emits final commit candidate
  -> KafkaOwner commits if needed
  -> KafkaOwner revokes Kafka assignment

KafkaOwner receives assign
  -> create new PartitionToken generation
  -> publish token to target shard
  -> target shard starts accepting ingress
```

关键点：

- generation 是防线，防止旧 ack 污染新 owner。
- revoke drain 不允许 worker 或 shard 调 Kafka API。
- Kafka owner 是 assignment truth source。

### 14.2 Graceful Shutdown Protocol

```text
shutdown ordering:
  1. KafkaOwner: stop polling, push Shutdown to each shard ingress lane
  2. Shards: drain remaining ingress, finish in-flight dispatch, drain ack lanes
  3. Shards: emit final commit candidates, push ShardDone to commit lane
  4. Workers: finish current batch, push final ack, detect lane closed → exit
  5. KafkaOwner: drain commit lanes, final commit, close Kafka client
  6. Join all shard threads, then join worker threads
```

关键点：

- shutdown 通过 SPSC lane 传播，不靠全局 flag + mutex
- shard 不主动 kill worker，worker 通过 lane closed 语义自然退出
- 最终 commit 由 Kafka owner 保证

***

## 15. Memory and Slot Ownership

### 15.1 当前问题

当前 `MsgSlotRing` 内部有 `ringMutex_`，加上外层 `mutex_`，hot path 双重 mutex。`PartitionTracker`（含 `flat_set<int64_t> liveOffsets/doneOffsets`）在全局 ring 内，需要完整迁入 shard。

### 15.2 Per-Shard Slot Pool 设计

```text
KafkaOwner poll record for shard i
  -> allocate slot from ShardSlotPool[i]
  -> slot ownership transfers with IngressBatch to shard

DispatchShard i
  -> dispatch: slot ownership transfers with LeaseToken to worker
  -> drain ack: slot returns via ack lane
  -> reclaim slot to ShardSlotPool[i]  (no global mutex)
```

Slot 状态机每一步的线程归属：

| Transition          | Thread       | Notes                       |
| ------------------- | ------------ | --------------------------- |
| Free → Acquired     | Kafka owner  | 从 ShardSlotPool\[i] 取       |
| Acquired → Filled   | Kafka owner  | poll 后填充 payload            |
| Filled → Dispatched | shard thread | dispatch 时标记                |
| Dispatched → Done   | shard thread | drain ack 后标记               |
| Done → Free         | shard thread | reclaim 回 ShardSlotPool\[i] |

`PartitionTracker`（liveOffsets / doneOffsets / committableOffset）完整迁入 shard。全局 ring 只保留 pool 管理（capacity / global free list），不参与 hot path offset tracking。

### 15.3 Slot Pool Allocation Timing

Kafka owner poll 时还没有 partition→shard 映射（librdkafka 先给 records，消费端再 demux）。两种策略：

- **方案 A：demux 后分配**。Kafka owner 先 poll 到临时 buffer，按 partition 分桶后从 ShardSlotPool\[i] 分配。优点是 pool 精确归属；缺点是 demux 前多一轮临时 buffer。
- **方案 B：预分配池**。Kafka owner 持有小的 pre-allocated slot cache，poll 后先用 cache，demux 到哪个 shard 就记账给谁，cache 不足时从全局 free list 补充。优点是 poll 路径不等 shard；缺点是需要 cache 和记账。

推荐 **方案 A**，因为 poll batch 通常很大（数千条），一次 demux 的 amortized 成本低。

***

## 16. Metrics

必须按 shard 暴露指标，否则无法判断设计是否真的解耦。

### 16.1 新增指标

| Metric                            | Level   | Purpose               |
| --------------------------------- | ------- | --------------------- |
| `kafkaOwnerPollRecordsPerSec`     | owner   | 单 Kafka client 实际入口能力 |
| `kafkaOwnerDemuxLatencyUs`        | owner   | poll 后分桶成本            |
| `shardIngressDepth`               | shard   | shard 是否吃不下 poll      |
| `shardReadyPartitions`            | shard   | 本地 ready backlog      |
| `shardBufferedRecords`            | shard   | 本地 buffer 压力          |
| `shardDispatchBatchesPerSec`      | shard   | 派发能力                  |
| `shardAckDrainPerSec`             | shard   | ack 回收能力              |
| `workerLaneDepth`                 | lane    | worker 是否被喂满          |
| `ackLaneDepth`                    | lane    | ack 是否及时回 shard       |
| `commitLaneCoalescedOffsets`      | owner   | commit 合并效果           |
| `controlLaneCoalescedTransitions` | owner   | pause/resume 合并效果     |
| `mutexInHotPathCount`             | process | 必须趋近 0                |

### 16.2 保留现有指标

以下指标在现有 UDS/Prometheus 中已暴露，新架构必须保留兼容性：

- `totalPolledRecords` / `totalAckedRecords` / `totalPolledBytes`
- `currentThroughputMsgsPerSec`
- `totalPauseCalls` / `totalResumeCalls`
- `pausedPartitionCount` / `assignedPartitionCount`
- `totalBufferedRecordCount` / `ringLiveCount` / `ringFreeSlotCount`
- `totalWorkerReadTimeouts` / `totalMailboxEmptyReads` / `totalPartialDispatchBatches`
- `polledToProductionRatio`
- `dispatcherQueueDepth`（Phase B 时改名为 `shardIngressDepth`）
- `totalDispatcherIngressCommands` / `totalDispatcherAckCommands` / `totalDispatcherDemandCommands`
- `totalDispatcherLoopIterations` / `totalDispatcherLoopLatencyUs`

### 16.3 验收标准

- record hot path mutex 调用为 0。
- ack hot path mutex 调用为 0。
- worker read hot path mutex 调用为 0。
- Kafka client count 默认为 1。
- shard ingress / ack / worker lanes 无长期满队列。

***

## 17. eBPF 验证方案

Phase G 需要具体的 probe 清单和验收标准。

### 17.1 Hot Path Mutex 验证（必须为 0）

```bash
bpftrace -p $PID -e '
uprobe:/lib/.../libpthread.so:pthread_mutex_lock /comm == "kcv2-shard*" || comm == "slot-*"/ {
  @mutex_in_hot[comm, ustack(perf,4)] = count();
}
interval:s:5 { print(@mutex_in_hot); exit(); }'
```

### 17.2 SPSC Lane Ownership 验证

确认每条 lane 只有正确的一个 producer 和一个 consumer 线程访问：

- ingress lane push 只在 `kcv2-owner`
- ingress lane pop 只在 `kcv2-shard-{i}`
- worker lane push 只在 `kcv2-shard-{i}`
- worker lane pop 只在 `slot-{j}`
- ack lane push 只在 `slot-{j}`
- ack lane pop 只在 `kcv2-shard-{i}`

### 17.3 Lane 延迟直方图

```bash
# ingress lane: poll → shard drain latency
# worker lane: shard dispatch → worker pop latency
# ack lane: worker ack push → shard drain latency
```

### 17.4 LaneSignal Baton Wait 频率

```bash
# shard signal wait vs active drain ratio
# worker signal wait vs active process ratio
```

***

## 18. Code Module Layering

### 18.1 当前代码结构问题

```text
unified_consumer.cpp   4374 行  ← 包含 SharedConsumerState, Registry, SocketService, 全部类型定义
unified_consumer.h      290 行
dispatch_state.cpp      387 行
dispatch_state.h        143 行
msg_slot_ring.cpp       526 行
msg_slot_ring.h         107 行
source_adapter.cpp      365 行
source_adapter.h        115 行
unified_consumer_html.cpp 730 行
unified_consumer_html.h    86 行
```

`unified_consumer.cpp` 一个文件包含：

- 15+ 个 struct/class 定义（DispatchEnvelope, PublishedMessage, WorkerMailbox, ConsumedMessageDraft, IngressBatch, AckBatchResult, AckBatchCommand, WorkerDemandCommand, DispatcherCommand, DiscoveryFileLock, SharedConsumerState, PerWorkerActiveItem, UnifiedConsumerRegistry, ConsumerV2DebugSocketService...）
- SharedConsumerState \~2300 行巨类，混合 poll / dispatch / ack / commit / pause / metrics / rebalance / lifecycle
- 所有类型定义隐藏在 .cpp 里，无法被其他编译单元引用和测试

### 18.2 目标分层

```text
Layer 0: Primitives (无业务依赖)
  spsc_lane.h            SpscLane<T> wrapper
  lane_signal.h          LaneSignal (`folly::Baton` based)
  lane_types.h           IngressRecord, IngressBatch, DispatchBatch,
                         AckEvent, CommitEvent, ControlEvent
  tokens.h               PartitionToken, WorkerToken, LeaseToken, CommitToken

Layer 1: State Machines (单线程 owned，无 mutex)
  dispatch_state.h/cpp   PartitionState, readyQueue, lease, backpressure
                         (已存在，需迁移 PartitionTracker 进来)
  shard_slot_pool.h/cpp  per-shard slot pool + PartitionTracker
                         (从 MsgSlotRing 拆出)

Layer 2: Threaded Components (各自拥有线程 + SPSC lanes)
  kafka_client_owner.h/cpp   poll / demux / commit / pause / rebalance
  dispatch_shard.h/cpp       shard loop / drain / dispatch / ack
  worker_adapter.h/cpp       readBatch / ackBatch via SPSC lanes

Layer 3: Composition & Lifecycle
  shared_consumer.h/cpp      组装 owner + shards + workers + token registry
                             (替代 SharedConsumerState 的 lifecycle 部分)
  consumer_registry.h/cpp    全局 registry（从 unified_consumer.cpp 拆出）

Layer 4: Monitoring & Debug
  consumer_metrics.h/cpp     JSON / Prometheus / HTML metrics builder
                             (复用 unified_consumer_html.h/cpp)
  debug_socket_service.h/cpp UDS HTTP server
                             (从 unified_consumer.cpp 拆出)

Layer 5: External Interface (不变)
  unified_consumer.h/cpp     WorkerQueue + UnifiedConsumer 公开接口
                             (变成 thin facade)
  source_adapter.h/cpp       Source 框架适配
```

### 18.3 依赖方向

```text
Layer 5  →  Layer 3  →  Layer 2  →  Layer 1  →  Layer 0
Layer 4  →  Layer 3  →  Layer 2  →  Layer 1  →  Layer 0

禁止：
  Layer 0/1/2 → Layer 3/4/5
  Layer 1 → Layer 2
  Layer 2 内部横向依赖（owner/shard/worker 不直接调用对方方法，只通过 SPSC lane）
```

### 18.4 文件布局

```text
src/source/kafka/consumer_v2/
  ├── primitives/
  │   ├── spsc_lane.h
  │   ├── lane_signal.h
  │   ├── lane_types.h
  │   └── tokens.h
  ├── state/
  │   ├── dispatch_state.h
  │   ├── dispatch_state.cpp
  │   ├── shard_slot_pool.h
  │   └── shard_slot_pool.cpp
  ├── components/
  │   ├── kafka_client_owner.h
  │   ├── kafka_client_owner.cpp
  │   ├── dispatch_shard.h
  │   ├── dispatch_shard.cpp
  │   ├── worker_adapter.h
  │   └── worker_adapter.cpp
  ├── shared_consumer.h
  ├── shared_consumer.cpp
  ├── consumer_registry.h
  ├── consumer_registry.cpp
  ├── consumer_metrics.h
  ├── consumer_metrics.cpp
  ├── debug_socket_service.h
  ├── debug_socket_service.cpp
  ├── unified_consumer.h           (thin facade, 保持公开接口不变)
  ├── unified_consumer.cpp          (瘦身到 <500 行)
  └── source_adapter.h/cpp          (不变)
```

### 18.5 单文件行数预算

| 文件                           |    预算 | 说明                                          |
| ---------------------------- | ----: | ------------------------------------------- |
| spsc\_lane.h                 | \~100 | header-only wrapper                         |
| lane\_signal.h               |  \~80 | header-only, wraps `folly::Baton`           |
| lane\_types.h                |  \~80 | POD struct 定义                               |
| tokens.h                     |  \~60 | POD struct 定义                               |
| dispatch\_state.h/cpp        | \~500 | 已有 530 行，略扩展                                |
| shard\_slot\_pool.h/cpp      | \~400 | 从 msg\_slot\_ring 迁出 PartitionTracker       |
| kafka\_client\_owner.h/cpp   | \~600 | poll + demux + commit + control             |
| dispatch\_shard.h/cpp        | \~800 | shard loop + drain + dispatch + ack         |
| worker\_adapter.h/cpp        | \~300 | SPSC lane 版 readBatch/ackBatch              |
| shared\_consumer.h/cpp       | \~500 | 组装 + lifecycle + config                     |
| consumer\_registry.h/cpp     | \~200 | 从 unified\_consumer.cpp 拆出                  |
| consumer\_metrics.h/cpp      | \~800 | 从 unified\_consumer\_html + JSON/Prometheus |
| debug\_socket\_service.h/cpp | \~500 | 从 unified\_consumer.cpp 拆出                  |
| unified\_consumer.h/cpp      | \~300 | thin facade                                 |

总计 \~5200 行（vs 当前 4374 + 730 = 5104 行），代码总量不膨胀，但模块边界清晰。

***

## 19. Implementation Plan

### Phase 0: Extract Non-Hot-Path Modules (代码分层，行为不变)

从 `unified_consumer.cpp` 中拆出不影响 hot path 的模块：

| 拆出模块                           | 来源                                | 目标文件                         |    行数 |
| ------------------------------ | --------------------------------- | ---------------------------- | ----: |
| `ConsumerV2DebugSocketService` | unified\_consumer.cpp L3487-L4170 | `debug_socket_service.h/cpp` | \~700 |
| `UnifiedConsumerRegistry`      | unified\_consumer.cpp L3188-L3485 | `consumer_registry.h/cpp`    | \~300 |
| 类型定义                           | unified\_consumer.cpp L194-L340   | `primitives/lane_types.h`    | \~150 |

验证：全量测试通过，UDS metrics 行为不变。

### Phase A: Introduce SPSC Primitives

- 引入 `SpscLane<T>`，内部封装 `folly::ProducerConsumerQueue<T>`。
- 引入 `LaneSignal`，内部封装 `folly::Baton`。
- 明确 `tryPush` / `tryPop` / `drain` / `sizeGuess` / `capacity` / backpressure 语义。
- 禁止业务代码直接依赖 Folly/Boost queue API，避免后续类型替换污染架构。
- 禁止在 `SpscLane` 和 `LaneSignal` 里使用 `std::mutex` / `std::condition_variable`。
- 单测覆盖 push/pop/drain/backpressure/signal。

### Phase B: Split Ownership with `shardCount=1`

- 拆出 `KafkaClientOwner` 和 `DispatchShard` 到独立文件。
- 先保持一个 shard，行为等价。
- 所有边改成 SPSC lane，即使只有一个 shard。
- `SharedConsumerState` 瘦身为 `SharedConsumer`，只做 lifecycle + config + 组装。
- **性能基线**：stress test bench 必须 >= 当前 958K msg/s 基线。

### Phase C: Move Partition State into Shard

- partition buffer / ready queue / lease / acked offset 迁入 `DispatchShard`。
- `PartitionTracker` 从 `MsgSlotRing` 迁入 shard-local `ShardSlotPool`。
- `SharedConsumer` 只保留 registry / lifecycle / config control plane。
- worker read 不再访问 global dispatch state。

### Phase D: SPSC Ack Return

- 为每个 `(worker, shard)` 建 ack lane。
- worker ack 只写 ack lane。
- shard drain ack lane 后释放 lease、推进 offset、产出 commit candidate。
- 引入 `WorkerAdapter`（SPSC lane 版 readBatch/ackBatch）。

### Phase E: Commit and Pause Control Lanes

- 每 shard 建 commit lane 和 control lane 回 Kafka owner。
- Kafka owner 批量 drain、合并、节流并调用 Kafka API。
- shard 不再执行任何 Kafka API。
- backpressure 改为 shard ingress lane depth 驱动，不 per-record 检查。

### Phase F: Enable `shardCount=N`

- partition token 按 hash 或 assignment plan 分配到 shard。
- Kafka owner demux 到多个 ingress lanes。
- worker token 分配到 shard lanes。

### Phase G: Remove Hot Path Mutex and Verify

- 用 bpftrace probe（Section 17）验证 record / dispatch / read / ack hot path 不进入 pthread mutex。
- 剩余 mutex 只允许出现在 lifecycle、registry、debug snapshot、test hook。
- 验证所有 SPSC lane ownership（Section 17.2）。
- 性能基线 bench。

***

## 20. Design Invariants

必须长期保持以下不变量：

- 一个 shared consumer 默认只有一个 Kafka client。
- Kafka API 只在 Kafka owner 线程调用。
- partition mutable state 只有一个 shard owner。
- 所有 hot path queue 都是 bounded SPSC。
- ack 通过 worker-to-shard SPSC lane 回 home shard。
- commit / pause / resume 通过 shard-to-owner SPSC lane 回 Kafka owner。
- worker 不执行调度。
- shard 不等待 worker。
- Kafka owner 不维护 dispatch state。
- hot path 不碰 mutex。
- 单文件行数 ≤ 800 行，编译单元职责单一。

***

## 21. Summary

正确架构不是"多 Kafka client"，也不是"单 dispatcher owner"。

目标是：

```text
Single Kafka client owner
  + partition-owned dispatch shards
  + SPSC queue on every boundary
  + token-based ownership
  + no mutex in hot path
  + layered code modules (≤ 800 lines per file)
```

这个模型把 Kafka group / commit / pause 的复杂性集中在唯一 Kafka owner，把可并行的 dispatch / ack / worker feeding 拆成严格 ownership 的 shard 数据平面。只要每条边都能证明是 single-producer / single-consumer，就不需要 MPSC，也不需要 mutex。

代码分层确保每个编译单元职责单一、可独立测试、不回退到巨类模式。

***
***

## 2026-05-18: Direct SPSC Production Fast Path

### 背景

当前落地形态不是完整的多 shard 目标态，而是一个可生产验证的过渡架构：

```text
Single Kafka owner
  -> per-worker bounded SPSC data rings
  -> workers zero-copy read RdKafka::Message*
  -> per-worker bounded SPSC ack rings
  -> owner drains ack and commits offsets
```

这个形态优先解决 10B backlog 压测暴露出的真实瓶颈：

- 保持一个 shared consumer 默认只有一个 Kafka client。
- record/read/ack direct hot path 不新增任何 `std::mutex` / `lock_guard`。
- 不再经过 legacy dispatch slot pool、worker mailbox、dispatcher command queue。
- `RdKafka::Message*` 指针直接跨 SPSC lane 传给 worker，payload 不 copy。
- ring 满时不丢消息，而是通过 drain signal 做 bounded backpressure。

### 当前数据面

```text
Kafka owner / poll thread
  consume()
  -> drainAllDirectAcks()
  -> snapshotDirectWorkerLanes()
  -> for each RdKafka::Message*:
       choose sticky/preferred worker
       tryPush directWorkerRings_[worker]
       if full: wait directDrainSignals_[worker] and retry
  -> batch notify directWorkerSignals_[touched workers]

Worker[w]
  readBatchDirect()
    -> wait directWorkerSignals_[w]
    -> pop up to 256 message pointers
    -> build RecordView from msg->payload()
    -> notify directDrainSignals_[w]

  ackBatchDirect()
    -> aggregate max offset by topic-partition
    -> push DirectAck to directAckRings_[w]
    -> delete RdKafka::Message* only after ack is queued

Kafka owner
  drainDirectAckRing(w)
    -> applyDirectAck()
    -> update directCommittedSlots_[partition].nextOffset atomically
    -> notify directAckDrainSignals_[w]

  snapshot / commit path
    -> syncDirectCommittedOffsetsLocked()
    -> commitOffsetsLocked()
```

### Lane 和状态结构

当前 direct mode 使用固定上限和稳定地址，避免运行期扩容带来的引用失效：

| 结构 | 生产者 | 消费者 | 说明 |
|------|--------|--------|------|
| `directWorkerRings_[w]` | Kafka owner | worker `w` | `RdKafka::Message*` zero-copy data lane |
| `directAckRings_[w]` | worker `w` | Kafka owner | `DirectAck` lane，ack 先入队后释放消息 |
| `directWorkerSignals_[w]` | Kafka owner | worker `w` | data available signal |
| `directDrainSignals_[w]` | worker `w` | Kafka owner | worker pop 后通知 owner ring 有空间 |
| `directAckDrainSignals_[w]` | Kafka owner | worker `w` | owner drain ack 后通知 worker ack ring 有空间 |
| `directWorkerRegistered_[w]` | lifecycle/control | owner/worker | atomic registration bit，read/ack 前先检查 |
| `directWorkerLimit_` | lifecycle/control | owner | atomic worker upper bound for snapshots |
| `directCommittedSlots_` | owner | snapshot/metrics sync | fixed slot array，保存 direct ack 推进的 next offset |

固定上限：

```text
kDirectMaxWorkerCount    = 256
kDirectMaxPartitionCount = 4096
direct worker ring       = 65536 message pointers / worker
direct ack ring          = 4096 DirectAck events / worker
readBatchDirect limit    = 256 messages / batch
```

### Sticky Partition Worker

早期 direct 设计允许每次 owner push 时 probe 所有 worker。生产审查后改为 **sticky partition worker**：

```text
first record for topic-partition:
  preferred = partition % workerCount
  if preferred full:
      probe other workers
  bindDirectPartitionWorker(topicPartition, selectedWorker)

later records for same topic-partition:
  only try selectedWorker
  if full:
      wait selectedWorker drain signal
      retry selectedWorker
```

原因：

- Kafka partition 内 offset 必须保持顺序。
- 同一 partition 跨 worker 乱序处理会破坏 contiguous commit 语义。
- work stealing 只允许发生在 partition 首次绑定之前；绑定之后只做 sticky backpressure。

这与长期目标 Section 13 的原则一致：不迁移 partition state，不在 hot path 做会破坏 owner 语义的 stealing。当前 direct mode 的“首次选择 worker”发生在 Kafka owner 内部，owner 仍是所有 data rings 的唯一 producer，因此不破坏 SPSC。

### No-Lock Direct Hot Path

direct mode 的 record/read/ack 热路径不新增锁：

- `readBatchDirect()` 不进入 `mutex_`，只读取 `directWorkerRegistered_` 和当前 worker 的稳定 lane 指针。
- `ackBatchDirect()` 不进入 `mutex_`，只写当前 worker 的 `directAckRings_[w]`。
- `snapshotDirectWorkerLanes()` 不进入 `mutex_`，通过 `directWorkerLimit_` + `directWorkerRegistered_` 构造只读快照。
- `directWorkerForPartition()` / `bindDirectPartitionWorker()` 只在 Kafka owner 线程调用，`directPartitionWorkerByTopicPartition_` 是 owner-owned。
- `applyDirectAck()` 只在 Kafka owner drain ack 时调用，offset 推进写入 `directCommittedSlots_` 的 atomic `nextOffset`。
- commit/snapshot 仍复用 legacy control-plane lock，但先通过 `syncDirectCommittedOffsetsLocked()` 把 direct committed slots 合入 `committedOffsetByTopicPartition_`。

因此 direct hot path 的锁边界是：

```text
owner poll -> direct SPSC push -> worker read -> worker ack SPSC push -> owner ack drain
  no mutex     no mutex            no mutex       no mutex                 no mutex
```

`mutex_` 只保留在 lifecycle、assignment、commit snapshot、metrics snapshot、debug/control path。它不是 direct record/read/ack path 的同步原语。

### Non-Dropping Backpressure

data ring 和 ack ring 都是 bounded queue；满队列不能 drop：

```text
data ring full:
  if partition already sticky:
      wait selected worker drain signal
      retry selected worker
  else:
      probe workers once
      if still full, wait preferred drain signal and retry

ack ring full:
  worker waits directAckDrainSignals_[w]
  retry until queued or closed
```

关闭语义：

- `closed_` 是所有 direct wait loop 的退出条件。
- close/release worker 时 notify data/drain/ack-drain signals，避免 parker 卡死。
- worker release 只有在 `directDispatchEnabled=true` 时触碰 direct vectors，非 direct 模式不访问未初始化 direct lanes。
- release worker 会 drain 未读 direct messages 并 delete，避免 `RdKafka::Message*` 泄漏。

### Commit Offset Flow

direct mode 不再依赖 legacy `MsgSlotRing` / `PartitionTracker` 推进 offset，而是显式走 `DirectAck`：

```text
Worker ackBatchDirect()
  -> group records by topic-partition
  -> DirectAck{topic, partition, maxOffset, recordCount}
  -> push directAckRings_[worker]
  -> delete message pointers

Kafka owner drainAllDirectAcks()
  -> applyDirectAck()
  -> directCommittedSlots_[partition].nextOffset = max(nextOffset, ack.offset + 1)

snapshot / commit
  -> syncDirectCommittedOffsetsLocked()
  -> recordCommittedTopicOffsetLocked()
  -> commitOffsetsLocked()
```

assignment 时为每个 assigned partition 建 direct committed slot；revoke 时标记 slot `assigned=false` 并清理 sticky worker 绑定，防止旧 assignment 污染新 generation。

### 与长期 Shard 目标态的关系

Direct SPSC fast path 是生产压测优先的 Phase-G 验证路径，不替代本文前半部分的最终多 shard 架构。

| 维度 | Direct fast path | 长期目标态 |
|------|------------------|------------|
| Kafka client | single owner | single owner |
| Dispatch owner | Kafka owner 直接选择 worker | `DispatchShard` owner |
| Partition state | sticky worker map + direct committed slot | shard-local `PartitionState` |
| Worker data lane | owner -> worker SPSC | shard -> worker SPSC |
| Ack lane | worker -> owner SPSC | worker -> shard SPSC |
| Commit aggregation | owner drains direct ack slots | shard emits commit token, owner commits |
| Hot path lock | 0 new locks | 0 locks |

后续迁移到多 shard 时，direct fast path 中已经验证的部分应保留：

- `SpscLane<T>` + `LaneSignal` 的 bounded wait/notify 模型。
- `RdKafka::Message*` 或 slot pointer 的 zero-copy transfer。
- 每条 lane 明确 single producer / single consumer。
- registration / lifecycle 与 hot path 分离。
- ack 先 durable queue，再释放 payload。

### 生产约束

| 约束 | 当前保证 |
|------|----------|
| 消息不丢弃 | data ring / ack ring 满时等待 drain signal，仅 close 中断 |
| partition 顺序 | 首次选择 worker 后 sticky 绑定，同一 topic-partition 不跨 worker |
| hot path 不用锁 | direct read/ack/push/drain 不新增 `std::mutex` / `lock_guard` |
| SPSC 不变量 | 每条 data ring 只有 owner 写、worker 读；每条 ack ring 只有 worker 写、owner 读 |
| 零拷贝 | `RdKafka::Message*` 指针传递，`RecordView.payloadView` 指向原 payload |
| ack 后释放 | `DirectAck` 成功入队后才 delete message |
| close 安全 | close/release notify 所有 direct signals，并 drain 未读 message |
| 非 direct 兼容 | `directDispatchEnabled=false` 不访问 direct vectors |

### 验证口径

按 `docs/consumer_v2/learn_from_stress_test.md` 中定义的生产口径验证：

- topic: 复用 `tide-kafka-v2-tenb-prod125w48-data00-*` billion backlog。
- partitions: **125**，与线上对齐。
- workers: **48**。
- ack target: **30,000,000**。
- pollDrain: **128**。
- duration: **30s+** 有效采样窗口。
- direct mode: `TIDE_KAFKA_V2_E2E_DIRECT_DISPATCH=1`。
- regression: `kafka_v2_test` 全量必须通过。
- lock scan: tracked C++ additions 不允许出现新的 `std::mutex` / `std::lock_guard` / `std::unique_lock` / `std::scoped_lock`。

最新验证结果：

```text
kafka_v2_test: 207/207 passed
10B e2e report: .dbg/billion-e2e-direct-nolock-1779113220.json
ackedDuringPerf: 30,000,046
ackedMsgsPerSec: 610,216
durationMs: 49,163
avgReadBatchSize: 132.544
avgDispatchBatchSize: 132.544
lastError: ""
```

***

## TAG: DIRECT-PAYLOAD-BALANCED-ROUTING

Date: 2026-05-19

Status: Design + implementation plan

### Problem Statement

The current direct SPSC fast path is fast but not balanced. It binds a partition slot to a worker using a static first-choice policy:

```text
first owner = partition % workerCount
then sticky forever
```

This preserves Kafka partition order, but it ignores real payload. The latest 10B e2e reports show the issue clearly:

```text
default direct e2e:
  active workers: 18 / 48
  idle workers  : 30 / 48
  hottest worker: ~332K msg/s
  coldest active: ~26K msg/s
  hot/cold ratio: ~12.4x

100M long window:
  active workers: 20 / 48
  idle workers  : 28 / 48
  hot/cold ratio: ~46.5x
```

So the next throughput boost must come from **payload-balanced partition placement**, not from more string/ack micro-optimization. The target is to keep SPSC and partition order, while making the owner assign or reassign partition slots so worker payload becomes much closer to equal.

### Invariants

This design must not compromise the properties that made direct SPSC production-safe:

- Kafka owner remains the only producer of all direct worker data rings.
- Each worker remains the only consumer of its direct worker data ring.
- Each worker remains the only producer of its ack ring.
- Kafka owner remains the only consumer of all direct ack rings.
- A Kafka topic-partition can be owned by only one worker at a time.
- A partition slot can move only when no queued or in-flight records for that slot exist on the old worker.
- Worker threads never steal work and never mutate routing state.
- Direct record/read/ack hot path remains lock-free.

### Core Idea

The owner thread maintains routing metadata per committed slot:

```text
slotOwner[slot]              -> current worker index, -1 if unbound
slotQueuedRecords[slot]      -> pushed to worker ring, not yet read by worker
slotInflightRecords[slot]    -> read by worker, not yet acked
slotPayloadScore[slot]       -> decayed record-rate score
slotLastMovedMs[slot]        -> cooldown to avoid thrash
```

And load metadata per worker:

```text
workerAssignedScore[worker]   -> sum(slotPayloadScore for owned slots)
workerQueuedRecords[worker]   -> queued direct records
workerInflightRecords[worker] -> in-flight direct records
workerAssignedSlots[worker]   -> number of owned slots
```

The routing decision changes from partition hash to measured load:

```text
new slot owner = worker with smallest effective load

effective load =
  workerAssignedScore
  + queuedWeight * workerQueuedRecords
  + inflightWeight * workerInflightRecords
```

### Assignment-time Placement

When a direct slot is created or first receives records, the owner chooses the lightest registered worker:

```text
chooseInitialWorker(slot):
  best = none
  for worker in registeredWorkers:
      load = effectiveWorkerLoad(worker)
      if best is none or load < best.load:
          best = worker
  bind slot -> best
```

This immediately avoids the current `partition % workerCount` skew for newly assigned partitions. It is safe because the slot has no old owner and therefore has no queued/in-flight records.

### Owner-thread Safe Reassignment

Reassignment is allowed, but only the Kafka owner can perform it. It runs at coarse intervals, not per record:

```text
maybeRebalanceDirectSlots(now):
  if now - lastRebalanceMs < rebalanceIntervalMs:
      return

  source = worker with max effective load
  target = worker with min effective load
  if sourceLoad - targetLoad < rebalanceMinGain:
      return

  slot = hottest movable slot owned by source
  if slot exists:
      move slot from source to target
```

A slot is movable only if all safety gates pass:

```text
isMovable(slot, source, target, now):
  slotOwner[slot] == source
  slotQueuedRecords[slot] == 0
  slotInflightRecords[slot] == 0
  now - slotLastMovedMs[slot] >= slotMoveCooldownMs
  target is registered and has a ring
  slotPayloadScore[slot] > 0
```

Move operation:

```text
moveSlot(slot, source, target):
  slotOwner[slot] = target
  workerAssignedScore[source] -= slotPayloadScore[slot]
  workerAssignedScore[target] += slotPayloadScore[slot]
  workerAssignedSlots[source]--
  workerAssignedSlots[target]++
  slotLastMovedMs[slot] = now
```

This preserves order because there are no remaining records for that partition on the old worker when the owner changes the route. Future records go to the new worker.

### Queue / In-flight Accounting

The direct ring already carries `directSlotIndex`. That makes exact accounting possible without `TopicPartition` construction:

```text
owner push success:
  slotQueuedRecords[slot]++
  workerQueuedRecords[worker]++
  slotPayloadScore[slot] = decay(slotPayloadScore[slot]) + 1

worker read batch:
  queued -> inflight for each directSlotIndex

worker ack batch:
  inflight -> committed for each directSlotIndex
```

Implementation detail:

- Owner-side push accounting is direct and cheap.
- Read-side accounting must not add locks. Prefer one of:
  - worker writes compact read/ack metadata to its existing ack lane, then owner applies queued/inflight transitions;
  - or use relaxed atomics for `slotQueuedRecords` / `slotInflightRecords` with strict move gate requiring both observed as zero.
- Reassignment uses conservative gates. If accounting is uncertain, skip the move.

### Why The Previous Reassign Trial Failed

The previous least-loaded reassign trial collapsed traffic to `2 / 48` workers and hurt throughput. The root issue was that it used an unsafe load signal and did not treat per-slot outstanding state as the hard correctness gate.

This design differs in four ways:

- It moves individual partition slots, not general work.
- It moves only when the slot has zero queued and zero in-flight records.
- It balances by decayed payload score, not only instantaneous outstanding count.
- It uses cooldown and minimum gain thresholds to avoid oscillation.

### Online Worker Throughput Balance Plan

Online balancing must target worker QPS, not slot count:

```text
workerQps[w] = acked records by worker w / sample window seconds
activeWorker = workerQps[w] > minActiveQps
clusterAvgQps = totalQps / desiredActiveWorkers

balanced when:
  p95(workerQps) / p50(workerQps) <= 2.0
  max(workerQps) / max(minNonZero(workerQps), 1) <= 4.0
  idleWorkerCount is explained by hotPartitionCount < workerCount
```

The placement objective is:

```text
minimize max(workerQps)
maximize activeWorkerCount
keep every topic-partition single-owner
never move a slot with queued or in-flight records
```

#### Layer 1: Shadow Measurement

Before any movement, the owner records per-slot and per-worker QPS for several windows:

```text
slotQps[slot]       = EWMA(records pushed or acked for this slot)
slotOwner[slot]     = current worker
workerQps[worker]   = sum(slotQps for owned slots)
workerSlots[worker] = owned slot count
```

This read-only phase must answer:

```text
Is skew caused by unlucky partition % worker mapping?
Are there enough hot partitions to fill 48 workers?
Are top hot partitions already one-per-worker?
Is a single elephant partition dominating total QPS?
```

Decision rules:

```text
if hotSlotCount < workerCount:
  perfect 48-worker equality is impossible without splitting a Kafka partition

if top1SlotQps > totalQps / workerCount * 4:
  one partition is an elephant; routing can isolate it but cannot make all workers equal

if multiple hot slots share one worker while idle workers exist:
  safe reassignment should improve throughput
```

#### Layer 2: Epoch Bin-packing

Every balance epoch, the owner computes a desired placement from the latest slot QPS:

```text
balanceEpochMs = 1000-5000

sortedSlots = slots sorted by slotQps desc
desiredLoad[worker] = 0
for slot in sortedSlots:
  if slot is sticky and not movable:
      keep current owner in desired plan
      desiredLoad[currentOwner] += slotQps[slot]
  else:
      target = worker with min desiredLoad
      desiredOwner[slot] = target
      desiredLoad[target] += slotQps[slot]
```

This is LPT (longest processing time first) bin packing. It is simple, stable, and closer to online schedulers used in production stream systems than local least-loaded moves.

Important: the desired plan is only a plan. It does not move anything unless the runtime safety gate passes.

#### Layer 3: Safe Incremental Migration

The owner applies the desired plan slowly:

```text
maxMovesPerEpoch = 1-4
minMoveGainRatio = 1.20
slotMoveCooldownMs = 30000-120000

for candidate in plannedMoves sorted by gain desc:
  source = slotOwner[slot]
  target = desiredOwner[slot]

  if source == target:
      continue
  if sourceLoad - targetLoad < slotQps[slot] * minMoveGainRatio:
      skipNoGain++
      continue
  if slotQueuedRecords[slot] != 0 or slotInflightRecords[slot] != 0:
      skipOutstanding++
      continue
  if now - slotLastMovedMs[slot] < slotMoveCooldownMs:
      skipCooldown++
      continue

  move slot to target
  stop when moves == maxMovesPerEpoch
```

This avoids the two failure modes already observed:

- **collapse to fewer workers**: LPT desired plan is global and capacity-spreading, not local least-loaded chasing.
- **throughput drop from naive spreading**: moves are only applied when they reduce measured source/target QPS imbalance enough to justify the move.

#### Online Safeguards

Production online balancing must have automatic rollback behavior:

```text
if throughput drops > 3% for 2 consecutive epochs:
  freeze balancing
  restore last stable owner plan for movable idle slots

if activeWorkerCount decreases:
  freeze balancing

if p95/p50 or max/min worsens:
  reduce maxMovesPerEpoch to 1

if any commit/order assertion fails:
  disable balancing immediately
```

The rollout mode should be explicit:

```text
mode=observe
  collect slotQps and simulated desired plan, no movement

mode=plan
  emit planned moves and predicted workerQps, no movement

mode=move-one
  allow only one safe move per epoch

mode=auto
  allow bounded moves when 10B e2e and online canary prove stable
```

#### What Equality Can And Cannot Mean

Kafka partition order creates a hard ceiling:

```text
max useful active workers <= number of hot partitions
```

So online equality must be defined as:

- if hot partitions >= workers: worker QPS should converge near equal.
- if hot partitions < workers: every hot partition should be isolated first, and remaining workers may be idle.
- if one elephant partition dominates: direct SPSC cannot make all workers equal without a new intra-partition ordered processing model.

The current direct routing plan therefore aims for **best possible worker payload balance under partition-order constraints**, not artificial equal utilization that would break correctness.

### Expected Effect

The desired outcome is:

```text
before:
  active workers: 18-21 / 48
  idle workers  : 27-30 / 48
  hot/cold ratio: 12x-46x

after:
  active workers: closer to min(hot partitions, workers)
  idle workers  : significantly lower
  hot/cold ratio: target < 4x first, then < 2x
  throughput    : should increase because hot worker wait/backpressure drops
```

This will not split a single Kafka partition across workers. If the topic truly has only a few hot partitions, the maximum active worker count is still bounded by hot partition count. But if the current skew is caused by unlucky `partition % workerCount` placement, payload-balanced routing should materially boost throughput.

### Metrics To Add

Add direct routing metrics to the runtime snapshot and e2e report:

```text
directWorkerAssignedSlots[]
directWorkerAssignedPayloadScore[]
directWorkerQueuedRecords[]
directWorkerInflightRecords[]
directSlotReassignments
directSlotReassignSkippedOutstanding
directSlotReassignSkippedCooldown
directSlotReassignSkippedNoGain
directWorkerActiveCount
directWorkerIdleCount
directWorkerHotColdQpsRatio
```

The e2e report must keep the current per-worker pushed/read/acked arrays and add derived QPS summary so every run answers:

```text
Are workers equally busy or idle?
Which workers are hot?
Did reassign increase active workers?
Did hot/cold QPS ratio decrease?
```

### Implementation Plan

Phase 1: Observability and Accounting

- Add per-slot and per-worker direct load state owned by Kafka owner.
- Add `directSlotIndexes` based queued/inflight accounting.
- Expose assigned slots, queued records, inflight records, and payload score metrics.
- Do not move slots yet.
- Validate regression and one 10B e2e report.

Phase 2: Assignment-time Payload Placement

- Replace unbound `partition % workerCount` selection with lightest registered worker.
- Keep sticky behavior after binding.
- Validate e2e active worker count and hot/cold QPS ratio.
- If throughput regresses, rollback this phase only.

Phase 3: Safe Owner-thread Reassignment

- Add low-frequency `maybeRebalanceDirectSlots()`.
- Move only zero queued / zero in-flight slots.
- Add cooldown and minimum gain thresholds.
- Keep feature guarded by config/env until e2e proves stable.
- Validate with 30M and 100M target runs.

Phase 4: Production Default Decision

- Keep if throughput increases and worker skew improves.
- Reject if active workers collapse, throughput drops, or any ordering/commit issue appears.
- Document final result in `docs/consumer_v2/learn_from_stress_test.md`.

### Validation Plan

Required checks:

```text
bash dev/test_build.sh kafka_v2_test
env -u large-e2e-vars bash dev/test_run.sh kafka_v2_test
10B direct e2e 30M target
10B direct e2e 100M target if 30M is positive
perf report for direct hot path
bpftrace pthread_mutex_lock probe: no direct hot-path mutex
```

Success criteria:

```text
throughput > current direct default e2e (~2.45M msg/s)
active workers > current 18-21 / 48
idle workers < current 27-30 / 48
hot/cold QPS ratio < current 12x on 30M runs
no direct hot-path mutex
default regression 207/207 passed
```

Rollback criteria:

```text
throughput decreases by > 3%
active workers collapse
hot/cold ratio worsens materially
any partition ordering or commit correctness failure
any new direct hot-path lock
```
