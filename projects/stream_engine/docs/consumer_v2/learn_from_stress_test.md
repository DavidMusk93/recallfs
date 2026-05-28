# Learn from Tests: Consumer V2 Ring Slot Leak

## 暴露的 Bug

`MsgSlotRing` FIFO head-of-line blocking：rebalance revoke partition 后，in-flight slots（Dispatched 状态）和 buffered slots（Filled 状态）永远不释放，ring 打满后 `acquire()` 返回 nullptr，poll loop 停止，系统卡死。

## 为什么之前的测试没有发现

| 测试特征 | 之前的测试 | 暴露 bug 的测试 |
|----------|-----------|----------------|
| Worker 数量 | 1 | 8（含 1 个 slow worker） |
| Partition 数量 | 1-4 | 100 |
| Rebalance 注入 | 无 | 每 5s 一次，revoke 25% |
| 运行时长 | 瞬时 | 60s 持续 |
| Ring 容量压力 | 远未达到上限 | 高吞吐 + revoke 触发泄漏 |

**核心问题**：简单测试只验证了 happy path，没有验证「故障路径下的资源回收」。

## 开发工作流的问题

### 1. 测试覆盖的维度缺失

单测验证了 dispatch 语义正确性，但缺少：
- **资源生命周期验证**：slot 分配/回收的守恒断言
- **故障注入下的状态机完整性**：revoke 发生在不同阶段（Filled、Dispatched、Done）时的行为
- **长时间运行下的累积效应**：泄漏在短测试中不可见

### 2. 先功能后防御的顺序错误

正确顺序应该是：

```
功能实现 → 资源守恒断言 → 故障注入测试 → 性能压测
```

实际顺序是：

```
功能实现 → 简单 e2e → 性能压测 → 发现吞吐问题 → 批处理重构 → 再压测 → 终于发现资源泄漏
```

### 3. Ring buffer 的 FIFO reclaim 设计隐含了强假设

`reclaim()` 假设 head slot 总能及时变为 Done。这个假设在以下场景下被打破：
- Rebalance revoke 了 partition，但 slot 的所有权已经转移给 worker
- Worker 持有 batch 但 partition 已不存在，ack 失败，slot 卡在 Dispatched

**教训**：环形缓冲区如果使用严格 FIFO 回收策略，必须保证任何异常路径都能推进 head。

## 修复方案

Revoke 时立即清理所有相关 slots：

1. **In-flight slots**（已 dispatch 给 worker 的 batch）：在 `revokePartitionsLocked` 中调用 `cleanupDispatchItemSlotsLocked` 立即 markDone + reclaim
2. **Buffered slots**（在 partition buffer 中等待 dispatch 的）：`dispatchState_.revokePartition()` 返回被遗弃的 slot 列表，调用方执行 markDispatched → markDone → reclaim
3. **ABA 安全**：worker 后续 ack 已 revoked 的 batch 时，不再二次 cleanup

## 未来测试规范

新增任何涉及资源池（ring buffer、connection pool、lease map）的功能时，必须包含：

1. **守恒断言**：测试结束时 `liveCount == 0`（或已知的 in-flight 数量）
2. **故障路径覆盖**：在资源「借出」状态下触发 revoke/close/timeout，验证资源归还
3. **累积泄漏检测**：循环执行 assign → consume → revoke N 次，断言 liveCount 不单调递增
4. **慢路径模拟**：至少一个 worker 有延迟，放大 head-of-line blocking 窗口

## 性能对比

| 阶段 | 吞吐 | Ring 状态 |
|------|------|-----------|
| 修复前（有 rebalance） | 13,435 msg/s → 0（卡死） | 262144/262144（100%） |
| 修复后（旧阶段） | 38,982 msg/s（稳定） | peak 14,763（5.6%） |
| 本轮优化后（有 rebalance） | 162,209 msg/s（稳定） | 运行期 peak 29,693（11.3%） |
| large-scale perf e2e（latest real 40.2s） | 372,785 msg/s | 100 partitions billion backlog，30s+ 真实采样窗口 |

### 分阶段汇总表

#### 1. Stress 对比

| 阶段 | 吞吐 msg/s | Avg Batch Size | Partitions Consumed | Duplicates | Out-of-order | 结论 |
|------|------------|----------------|---------------------|------------|--------------|------|
| FIFO reclaim 失效期 | `13,435` | `941.7` | `10` | `0` | `0` | 最终卡死，ring 顶满 |
| ring reclaim 修复后 | `162,209` | `1020.0` | `94` | `0` | `0` | 稳定恢复 |
| fairness 1.0 | `114,045` | `7.8` | `79` | `0` | `0` | 公平性有了，但 batch 被打碎 |
| fairness 2.0 | `166,172` | `834.5` | `94` | `0` | `0` | 公平性和聚合基本重新平衡 |

#### 2. Perf e2e 对比

| 阶段 | Acked Msg/s | Acked During Perf | Duration Ms | 说明 |
|------|-------------|-------------------|-------------|------|
| ring reclaim 修复后 | `96,076.6` | `200,704` | `2,089` | 100 partitions backlog perf 基线 |
| fairness 1.0 | `514,139` | `200,000` | `389` | 波动过大，不能单独代表长期稳态 |
| fairness 2.0 | `81,711.4` | `200,438` | `2,453` | 更接近真实稳定消费语义 |
| latest real rerun（2026-05-12） | `372,785` | `15,000,111` | `40,238` | 100 partitions billion backlog，真实 e2e，30s+ 采样窗口 |

#### 3. Bench 对比

| 场景 | fairness 1.0 Consume Msg/s | fairness 1.0 Avg Batch | fairness 2.0 Consume Msg/s | fairness 2.0 Avg Batch | 结论 |
|------|----------------------------|-------------------------|----------------------------|------------------------|------|
| `100p / 8w` | `341,643` | `24.3` | `367,193` | `226.8` | 明显恢复 |
| `200p / 16w` | `57,732` | `1.0` | `326,669` | `103.1` | 大幅恢复 |
| `400p / 32w` | `43,925` | `1.0` | `335,671` | `55.0` | 大幅恢复 |
| `1000p / 32w` | `39,791` | `1.0` | `21,542` | `3.3` | 仍明显塌缩 |
| `2000p / 32w` | `34,912` | `1.0` | `15,957` | `2.2` | 仍明显塌缩 |

### 图表

#### Stress Throughput

```text
FIFO reclaim 失效期   | ##                               13,435
ring reclaim 修复后  | ##############################  162,209
fairness 1.0         | #####################           114,045
fairness 2.0         | ############################### 166,172
```

#### Stress Avg Batch Size

```text
FIFO reclaim 失效期   | ############################    941.7
ring reclaim 修复后  | ############################### 1020.0
fairness 1.0         |                                 7.8
fairness 2.0         | #########################       834.5
```

#### Bench Avg Batch Size

```text
100p / 8w
  fairness 1.0       | ###                             24.3
  fairness 2.0       | ############################    226.8

200p / 16w
  fairness 1.0       |                                 1.0
  fairness 2.0       | #############                   103.1

400p / 32w
  fairness 1.0       |                                 1.0
  fairness 2.0       | #######                         55.0

1000p / 32w
  fairness 1.0       |                                 1.0
  fairness 2.0       |                                 3.3

2000p / 32w
  fairness 1.0       |                                 1.0
  fairness 2.0       |                                 2.2
```

### 图表结论

- `fairness 2.0` 最直接的收益不是再造一个更高的峰值，而是把 `fairness 1.0` 打碎的 batch 能力重新拉回来。
- `stress` 维度上，`avg batch size` 从 `7.8` 恢复到 `834.5`，吞吐也从 `114K` 恢复到 `166K`。
- `200p` 和 `400p` 的 bench 恢复非常明显，说明等待时间加 backlog 这种 gating 比按 deferral 次数放行更适合当前系统。
- `1000p+` 仍然明显退化，说明下一阶段的主瓶颈已经不在 fairness 触发条件本身，而在高 partition 数下的 ready queue、batch 聚合和调度结构。

## fairness 2.0 之后

上一轮 `fairness 1.0` 证明了一件事：只要公平性触发条件写得太粗暴，系统虽然更“公平”，但会很快失去 batch 聚合能力。

表现是：

- `stress throughput` 掉到 `114,045 msg/s`
- `avg batch size` 掉到 `7.8`

本轮把公平性策略改成：

```text
等待时间 + backlog 判断
```

而不是：

```text
按被跳过次数强制放行
```

结果：

| 指标 | fairness 1.0 | fairness 2.0 |
|------|--------------|--------------|
| `60s stress throughput` | `114,045 msg/s` | `166,172 msg/s` |
| `avg batch size` | `7.8` | `834.5` |
| `partitions consumed` | `79` | `94` |
| duplicates | `0` | `0` |
| out-of-order | `0` | `0` |

这说明一个很重要的 workflow 教训：

> 公平性不是“越早放行越好”，而是“既要避免饥饿，也要保住聚合”。

如果测试只盯着“冷 partition 是否 eventually 被消费”，却不同时盯：

- `avg batch size`
- `throughput`
- `high partition count` 下的退化曲线

那么很容易做出“功能上看起来正确，性能上却明显倒退”的优化。

## 当前结论

这次回头看，问题已经发生了阶段性变化：

1. **最致命的 FIFO reclaim bug 已经被拿掉**
2. **stress 下的长期运行稳定性已经显著改善**
3. **steady-state perf e2e 基本没有上涨，说明瓶颈已经转移**

当前更像是：

```text
第一阶段：修资源回收模型，避免系统卡死
第二阶段：继续优化 dispatch/mailbox/锁粒度，抬高稳态吞吐上限
第三阶段：在高 partition 数下同时兼顾公平性和 batch 聚合
```

下一步最值得优先做的不是继续围绕 ring 打补丁，而是：

- `workerId -> workerIndex`
- per-worker `SPSC mailbox`
- dispatch / mailbox / ring 拆锁

## 2026-05-18 10B 线上对齐 poll-drain 调优

### 线上对齐口径

本轮不再使用历史 `100 partitions / 1 worker` 的本地默认，而是按线上实际场景对齐：

- topic partitions: `125`
- worker count: `48`
- poll thread: `1`
- dispatch shards: `8`
- dispatch batch: `1024`
- queue capacity: `1024`
- worker lane capacity: `8`
- ring capacity: `262144`
- high/low watermark: `4096 / 1024`
- fetch config: `fetch.max.bytes=268435456`, `max.partition.fetch.bytes=8388608`,
  `fetch.min.bytes=1048576`, `fetch.wait.max.ms=20`,
  `queued.max.messages.kbytes=524288`, `socket.receive.buffer.bytes=16777216`
- assignment strategy: empty, do not set sticky/cooperative-sticky

### 测试基础设施修正

- `10B` backlog 不能写到 Docker 默认 volume 所在根盘，根盘 `110G` 会被打满并导致 Redpanda metadata timeout。
- Redpanda 数据目录改为支持 `KAFKA_E2E_REDPANDA_DATA_DIR=/data00/...`，测试使用大盘 bind mount。
- `produce_bulk_records.sh` 增加 `KAFKA_E2E_BULK_PARALLELISM`，按 partition 并行生产；默认仍为 `1`，避免影响普通测试。
- large-scale e2e 增加 `TIDE_KAFKA_V2_E2E_WORKER_COUNT`，本轮注册 `48` 个 worker，并行 read/ack，避免测试 harness 成为消费瓶颈。
- 旧满盘容器和旧 topic/log/report 不删除，改名保留现场。

### Baseline

复用 topic: `tide-kafka-v2-tenb-prod125w48-data00-1779068785`

report: `.dbg/billion-e2e-tenb-prod125w48-data00-1779068785.json`

```text
pollDrain=128
ackedMsgsPerSec=1.320M
ackedDuringPerf=30,000,155
durationMs=22,725
avgPollBatchRecords=127.216
avgHandleConsumedLatencyUs=35.99
avgPollConsumeLatencyUs=0.24
```

### Sweep 结果

第一轮:

| pollDrain | Acked Msg/s | Avg Poll Batch | Avg Handle Us | 结论 |
|-----------|-------------|----------------|---------------|------|
| `64` | `1.510M` | `63.89` | `17.84` | 有收益，但不如更大 batch |
| `128` | `1.434M` | `127.43` | `35.88` | 低于 baseline 最佳候选 |
| `256` | `1.641M` | `254.64` | `68.87` | 有收益 |
| `512` | `1.688M` | `506.22` | `126.76` | 有收益 |
| `1024` | `1.698M` | `1000.20` | `244.58` | 本轮最佳 |

第二轮:

| pollDrain | Acked Msg/s | Avg Poll Batch | Avg Handle Us | 结论 |
|-----------|-------------|----------------|---------------|------|
| `1024` | `1.665M` | `997.75` | `244.70` | 复测仍最佳区间 |
| `1536` | `1.612M` | `1474.36` | `372.68` | 负收益，reject |
| `2048` | `1.428M` | `1890.92` | `488.30` | 负收益，reject |
| `4096` | `1.204M` | `3374.91` | `1017.75` | 负收益，reject |
| `8192` | 超过 3 分钟未完成 `30M` ack | - | - | 明确负收益，中止 |

### 结论

- `pollDrainBatchSize=1024` 是当时 C++ poll/drain 路径在 `125p / 48w / 10B backlog` 下的最佳实测默认值；C-level batch 后已被后续 sweep 更新为 `2048`。
- 从 `128` 到 `1024`，吞吐从 `1.320M` 提升到 `1.665M~1.698M`，约 `26%~29%`。
- `1536+` 的 batch 过大，`handleConsumedLatency` 快速上升，吞吐下降，应 reject。
- 本轮没有达到 `10x`，但说明本地 poll/fetch 已能跑到 `~1.7M msg/s`；若线上仍只有 `~200K msg/s`，下一步应优先查线上 broker/网络/worker 实际负载与 eBPF 热点，而不是继续盲目放大 poll drain。
- ready queue 和 partition state 的 index 化 / flat 化
- 更强的 partition batching / cohort dispatch

### 线上更新后复测与 eBPF 调试坑

集群更新到当时包含 `pollDrainBatchSize=1024` 的版本后，线上同口径 `75s` `/json` 采样结果：

```text
totalPolledRecords rate ~= 235.8K msg/s
totalAckedRecords rate ~= 235.9K msg/s
totalPollConsumeCalls rate ~= 235.9K calls/s
totalPollBatches rate ~= 269/s
totalPollFullBatches rate ~= 230/s
maxPollBatchRecords = 1024
assignedPartitionCount = 125
workerCount = 48
rdkThreadCount = 257
brokerCommittedRecordsPerSec ~= 242.9K
```

对比更新前：

| 阶段 | Poll/Ack Msg/s | Max Poll Batch | 结论 |
|------|----------------|----------------|------|
| 更新前 | `~167K` | `128` | 仍被旧默认/配置封顶 |
| 更新后 | `~236K` | `1024` | 约 `41%` 线上收益，但远未达到 `10x` |

这说明 `pollDrain=1024` 在线上确实生效，也有收益；但仍然存在一个关键结构问题：

```text
totalPollConsumeCalls ~= totalPolledRecords
```

也就是 owner drain 虽然按 `1024` 组成一次 `handleConsumedMessages`，但底层仍然是每条消息一次 `consumer->consume()` / `rd_kafka_consumer_poll()` / C++ `MessageImpl` 包装。下一阶段要继续冲 `10x`，不能只调大 drain，而要重点验证两个硬点：

1. **Kafka consume 层**：是否需要用 librdkafka C batch queue API（如 `rd_kafka_queue_get_consumer` + `rd_kafka_consume_batch_queue`）减少 per-record poll/wrapper/allocation 成本。
2. **Dispatch 层**：`handleConsumedMessages -> slot fill -> dispatch shard -> worker lane` 是否仍在 per-record 对象构造、partition lookup、slot lifecycle 和 queue push 上消耗太多 CPU/cache。

### eBPF / timeout 操作规范

本轮线上排查踩到一个严重工具坑：对热生产进程用 `bpftrace ustack()` 做 user-stack 聚合会卡住，且普通 `timeout` 不一定能兜住。

现场证据：

- 两个命令 `timeout 45 bpftrace ... ustack(...)` 存活超过 `7min`。
- `timeout` 父进程睡在 `sigsuspend`，说明它已经发过信号但仍在等 child 退出。
- 一个 `bpftrace` child 处于 `D` state，kernel stack 在读 `/proc/<tid>/maps`：

```text
m_start
seq_read_iter
seq_read
vfs_read
ksys_read
```

- 受控测试 `timeout 3 bash -c 'sleep 30'` 能在 `~3s` 后返回 `124`，说明 GNU `timeout` 本身没坏。

结论：

```text
plain timeout only sends SIGTERM and waits forever unless --kill-after/-k is set.
ustack() may block in /proc/<tid>/maps symbolization on this hot worker.
```

后续线上 eBPF 必须遵守：

| 规则 | 原因 |
|------|------|
| 所有 bpftrace 命令使用 `timeout -k 5s <duration>` | `SIGTERM` 后必须有 `SIGKILL` 兜底 |
| 不在热生产 worker 上使用 `ustack()` 聚合 | 避免卡在 `/proc/<tid>/maps` user-stack symbolization |
| 优先用 comm/TID 维度 bounded counter | 先确认热点线程族，不生成超大 stack map |
| 优先用 tracepoint/syscall/uprobe symbol counter | 用函数/事件计数替代 user stack |
| 每轮采样后检查残留 `bpftrace` | 避免调试命令干扰线上和后续测试 |

推荐安全模板：

```bash
timeout -k 5s 40s bpftrace -e 'profile:hz:49 /pid == TARGET_PID/ { @[comm] = count(); } interval:s:30 { exit(); }'
```

如果需要更细粒度热点，优先用 symbol-level counter，例如对 Kafka consume 或 dispatch 关键函数计数，而不是直接上 `ustack()`。

### 10x 后续假设

当前阶段不要把 `10x` 简化为“继续调参数”。参数收益已经证明存在但有限：

- `pollDrain 128 -> 1024`：本地 `10B` e2e 提升约 `26%~29%`，线上更新后提升约 `41%`。
- 线上仍只有 `~236K msg/s`，而本地生产对齐 e2e 可到 `~1.7M msg/s`，说明线上还有 Kafka broker/client thread、consume wrapper、dispatch/cache 或 backpressure 波动差异。
- `rdkThreadCount=257` 仍然很高，单 KafkaConsumer 对大 broker 集群会产生大量 `rdk:broker*` 线程，`maxRdkThreadCount` 当前不是 librdkafka broker thread hard limit。

下一轮优化优先级：

1. **Kafka batch consume 原型**：先在 e2e 中验证 C batch API 是否能把 `consume calls / records` 从 `~1.0` 降下来。
2. **Dispatch tile / cohort 重构**：如果 Kafka batch consume 不够，继续把 poll tile 到 dispatch shard 的数据布局合并，减少 per-record 中间对象。
3. **线上安全 eBPF 计数**：用 `timeout -k` + bounded counters 同窗观察 `kcv2p*` owner、`kcv2-shard-*`、`slot-*`、`rdk:broker*` 的 CPU/off-CPU 分布。
4. **收益保留、负收益回滚**：任何候选必须跑生产对齐 `125p/48w/10B` e2e 和线上同口径 metrics，达不到收益就 reject。

### C batch consume 原型结果：reject

尝试用 librdkafka C API：

```text
rd_kafka_queue_get_consumer
rd_kafka_consume_batch_queue
```

替代 `consumer->consume()` drain loop。这个原型的目标是验证：

```text
totalPollConsumeCalls / totalPolledRecords
```

是否是主要吞吐瓶颈。

生产对齐 `125p / 48w / 10B backlog / pollDrain=1024 / 30M ack target` 结果：

| 指标 | C batch 原型 |
|------|-------------|
| `ackedMsgsPerSec` | `367,406` |
| `durationMs` | `81,656` |
| `totalPolledRecords` | `30,015,606` |
| `totalPollConsumeCalls` | `38,598` |
| `avgPollBatchRecords` | `777.647` |
| `avgPollConsumeLatencyUs` | `1312.07` |
| `avgHandleConsumedLatencyUs` | `310.122` |
| `avgReadBatchSize` | `548.813` |
| `avgDispatchBatchSize` | `548.817` |

对比已验证的 `pollDrain=1024` baseline：

| 方案 | Acked Msg/s | 结论 |
|------|-------------|------|
| C++ `consumer->consume()` drain | `1.665M~1.698M` | 当前保留 |
| C `rd_kafka_consume_batch_queue` | `367K` | 明确负收益，reject |

结论：

- C batch 原型确实把 consume calls 从“每条一次”降到“每批一次”，但吞吐大幅下降。
- 这说明 `consumeCalls ~= records` 不是唯一瓶颈，或者 C batch queue 破坏了当前 high-level consumer queue serving / fetch / callback 节奏。
- 该原型已回滚，不保留代码。
- 下一步更应该看 `handleConsumedMessages -> shard ingress -> dispatch -> worker lane` 的数据布局与调度，而不是继续强行替换 librdkafka poll API。

### 安全 eBPF 计数补充

用 `timeout -k` + uprobe symbol counter 对更新后线上 worker 采样，避免 `ustack()`：

```text
30s window:
rd_kafka_consumer_poll: 4,516,665 calls
handleConsumedMessages: 4,409 calls
tryApplyIngressBatch: ~4,408 calls across 8 shards
scheduleShardDispatch: ~122K calls across 8 shards
```

安全 latency histogram：

```text
rd_kafka_consumer_poll_us:
  mostly 1-4us, with a small 4-8ms tail

handleConsumedMessages_us:
  mostly 1-2ms, with small 2-64ms tail

scheduleShardDispatch_us:
  mostly 1-8us
```

解读：

- 线上仍然是极高频 `rd_kafka_consumer_poll`，但 C batch 原型证明“直接换 C batch API”不是正确收益路径。
- `handleConsumedMessages` 单次成本在毫秒级，且每次 owner batch 后都要进入 shard ingress；这比 `scheduleShardDispatch` 的微秒级单次成本更值得继续拆。
- `scheduleShardDispatch` 调用频率高，但单次成本低；它更像 dispatch loop 的调度噪声，不是第一优先级。
- 下一轮应优先做 poll tile 到 shard ingress 的数据布局收敛，减少 `ConsumedMessageDraft`、`PublishedMessage`、topic/partition lookup、slot fill 和 ingress batch 之间的中间搬运。

### Direct shard batching 原型结果：reject

尝试在 `handleConsumedMessages()` 中直接构造每个 shard 的 `IngressBatch`，跳过中间全局 `drafts` vector：

```text
before:
Kafka messages -> drafts vector -> shardBatches vectors -> SPSC ingress

prototype:
Kafka messages -> shardBatches vectors -> SPSC ingress
```

生产对齐 `125p / 48w / 10B backlog / pollDrain=1024 / 30M ack target` 结果：

| 指标 | Direct shard batching |
|------|-----------------------|
| `ackedMsgsPerSec` | `1.022M` |
| `durationMs` | `29,340` |
| `totalPolledRecords` | `30,013,143` |
| `totalPollConsumeCalls` | `30,015,661` |
| `avgPollBatchRecords` | `965.636` |
| `avgHandleConsumedLatencyUs` | `272.564` |
| `avgDispatchBatchSize` | `522.321` |
| `avgReadBatchSize` | `522.326` |

对比 `pollDrain=1024` baseline 的 `1.665M~1.698M`，这是明确负收益，已回滚。

解读：

- 仅减少 `drafts -> shardBatches` 这一次 move/pass 不够，反而可能因为每个 shard 提前 reserve 和更分散的写入破坏 cache/allocator 行为。
- 该结果说明 dispatch 优化不能停留在 `handleConsumedMessages()` 局部微调；需要跨 `IngressBatch`、slot fill、partition token/cache 和 worker envelope 做成更粗粒度 tile/cohort。
- 下一步应避免“局部省一次 vector move”的小改，转向减少 per-record heap allocation 和 topic/partition lookup。

### Slot contiguous reclaim fast path 原型结果：reject

观察到 `ShardSlotPool::reclaim()` 在 contiguous offset tracker 上会先 materialize `liveOffsets`，再 erase done slot。尝试增加 contiguous fast path，避免 ordered ack/reclaim 时构造 `flat_set`。

生产对齐 `125p / 48w / 10B backlog / pollDrain=1024 / 30M ack target` 结果：

| 指标 | Slot reclaim fast path |
|------|------------------------|
| `ackedMsgsPerSec` | `1.325M` |
| `durationMs` | `22,646` |
| `totalPolledRecords` | `30,014,074` |
| `avgPollBatchRecords` | `986.624` |
| `avgHandleConsumedLatencyUs` | `290.786` |
| `avgDispatchBatchSize` | `524.729` |
| `avgReadBatchSize` | `524.730` |

结果低于 `pollDrain=1024` baseline 的 `1.665M~1.698M`，明确 reject，代码已回滚。

解读：

- 这个路径语义上成立，focused tests 也通过，但在端到端吞吐上没有收益。
- 推测当前 benchmark 的主瓶颈不在 reclaim tracker materialization，或者随机波动/allocator/cache 影响超过该微优化收益。
- 后续不要继续做 slot tracker 局部微优化，优先看更大的 per-record heap allocation 和 payload/metadata layout。

### Owner allocation shift 原型结果：reject

线上安全 eBPF 用 `malloc` uprobe 观察到一个更强信号：

```text
20s window:
kcv2 owner thread malloc ~= 3.4M calls
polled records ~= 789K
```

也就是 owner poll 线程有多个 heap allocation / record。尝试把 `PublishedMessage` 从 owner 线程提前 `new` 改为：

```text
owner: ConsumedMessageDraft embeds PublishedMessage payload metadata
shard: tryApplyIngressBatch() new PublishedMessage and move payload into slot
```

目标是把 per-record `PublishedMessage` allocation 从单 owner 线程转移到 8 个 shard 线程。

生产对齐 `125p / 48w / 10B backlog / pollDrain=1024 / 30M ack target` 结果：

| 指标 | Owner allocation shift |
|------|------------------------|
| `ackedMsgsPerSec` | `990K` |
| `durationMs` | `30,280` |
| `totalPolledRecords` | `30,006,793` |
| `avgPollBatchRecords` | `965.128` |
| `avgHandleConsumedLatencyUs` | `274.107` |
| `avgDispatchBatchSize` | `522.532` |
| `avgReadBatchSize` | `522.534` |

结果明显低于 `pollDrain=1024` baseline 的 `1.665M~1.698M`，已回滚。

解读：

- 简单把 allocation 从 owner 转移到 shard 会拉低端到端吞吐，可能因为 shard 线程本来就是 dispatch/read/ack 的并行关键路径。
- 线上 owner malloc 信号仍然重要，但正确方向不是“转移 allocation”，而是“减少 allocation 总量”。
- 下一步更合理的 Kafka consume 层原型：保持 `rd_kafka_consumer_poll()` 逐条语义，但绕开 C++ `RdKafka::MessageImpl` wrapper allocation，直接用 C message fields 构造 draft。

### C single-poll 原型结果：reject

按上一节假设，尝试保持 `rd_kafka_consumer_poll()` 逐条 poll 语义，但绕开 C++ `RdKafka::MessageImpl` wrapper allocation，直接读取 C message fields 构造 `ConsumedMessageDraft`。

生产对齐 `125p / 48w / 10B backlog / pollDrain=1024 / 30M ack target` 结果：

| 指标 | C single-poll |
|------|---------------|
| `ackedMsgsPerSec` | `860,044` |
| `durationMs` | `34,882` |
| `totalPolledRecords` | `30,013,749` |
| `totalPollConsumeCalls` | `30,016,459` |
| `totalPollConsumeTimeouts` | `0` |
| `avgPollBatchRecords` | `937.373` |
| `avgPollConsumeLatencyUs` | `0.459862` |
| `avgHandleConsumedLatencyUs` | `277.738` |

结果明显低于 `pollDrain=1024` baseline 的 `1.665M~1.698M`，已回滚。

解读：

- 直接走 C single-poll 虽然降低了单次 poll wrapper 路径的可见 latency，但没有提升端到端吞吐。
- `totalPollConsumeCalls` 仍然约等于 records，说明这个原型只改变单条消息包装方式，没有改变 poll/dispatch 的批量结构。
- 下一步不要继续在 C++ wrapper vs C fields 上做局部替换；应转向减少 per-record allocation 总量、复用 topic/partition metadata、或把 payload/metadata 做成更粗粒度 tile 后再进入 dispatch。

## 2026-05-18 稳定测试方案与 Slot-owned 原型复评

### 背景

这次 `Slot-owned message storage` 原型第一次 10B e2e 只有 `958,050 msg/s`，但不能直接判 reject。原因是本机同时存在两个 heavy CPU task，且它们的 cpuset 合起来覆盖全机所有 CPU：

```text
128-core host
tide_worker pid=3599714  ~= 1555% CPU, cpuset=0-31,64-95
tide_worker pid=3599716  ~= 1485% CPU, cpuset=32-63,96-127
redpanda pid=2075505     ~= 47% CPU
load average             ~= 62-74
```

结论：环境污染存在时，历史 baseline 也会漂移；单次 e2e 只能作为 smoke，不能作为 keep/reject 决策依据。

### 稳定测试方案

稳定测试必须先把“代码收益”和“环境波动”分离：

```text
Step 1: 记录机器状态
  - uptime / loadavg
  - /proc/pressure/cpu
  - top CPU processes
  - Redpanda CPU / thread count

Step 2: 构建成对二进制
  - baseline binary
  - candidate binary
  - 两者使用同一 runfiles / 同一依赖 / 同一编译参数

Step 3: 固定 e2e 口径
  - same topic
  - same partition count
  - same ack target
  - same pollDrain / dispatch / worker config
  - independent consumer group per run

Step 4: 交替执行，抵消顺序效应
  - B -> C -> B -> C
  - C -> B -> C -> B

Step 5: 用环境门禁判断结果是否有效
  - 如果 A/B 的 CPU PSI 明显不同窗，结果只能标为 inconclusive
  - 如果 load / PSI / top processes 稳定，才计算相对收益
```

有效性门禁：

- `cpu pressure some avg10` 必须同量级，不能一组 `0.01`、另一组 `0.40` 后直接比较。
- 每轮必须保存 report JSON 和 log，CSV 汇总只能作为索引，不能替代原始证据。
- 至少两组交替顺序都支持同一结论，才能 keep/reject。
- 对“减少实际 work 总量”的候选，除非稳定 A/B 连续证明无收益，否则不回滚。
- 如果 heavy task cpuset 覆盖全机，优先暂停/迁移 heavy task，或者换独占机器。

### 两阶段执行计划

这类测试不能边想边跑。尤其本机 heavy task 停掉后，线上 worker 的 metrics / eBPF 观测窗口也会消失，所以执行顺序必须固定：

```text
Phase A: heavy task 仍在运行，做观测和定稿
  1. 记录环境污染：
     - uptime / loadavg
     - /proc/pressure/cpu
     - ps top CPU processes
     - heavy task pid / cpuset / CPU%
  2. 记录线上/本机 runtime 证据：
     - /json metrics
     - safe eBPF counters / uprobes
     - malloc / poll / handle / dispatch 计数
  3. 固化测试资产：
     - candidate patch
     - baseline binary
     - candidate binary
     - run script / env / report path
  4. 写清 keep/reject 门禁，不再临场改口径

Phase B: heavy task 停止后，只跑 e2e
  1. 等待机器进入安静窗口
  2. 记录一次 pre-run 环境快照
  3. 执行 B -> C -> B -> C
  4. 执行 C -> B -> C -> B
  5. 汇总 report JSON，计算相对收益
  6. 只在门禁满足时决定 keep/reject
```

Phase B 的安静窗口门禁：

- `loadavg` 不能再被两个 heavy `tide_worker` 主导；如果 load 仍然大幅波动，继续等待。
- `/proc/pressure/cpu some avg10` 应保持同量级且低位；如果某轮 spike，该轮标记为 invalid。
- Redpanda CPU 不能在某一轮异常飙高；否则该轮不能参与 A/B。
- 每轮开始前记录 `date +%F_%T`、`uptime`、`/proc/pressure/cpu`、top CPU processes。
- 不再同时挂复杂 `ustack()`；如需 eBPF，只用 `timeout -k` 的 bounded counter。

推荐执行脚本形态：

```bash
RUN_ROOT=.dbg/ab_slot_owned_message
AB_RUN_ID=slot-owned-quiet-$(date +%s)

# 先准备二进制，不在安静窗口里浪费时间构建。
cp build64_release/src/test/kafka_v2_test "$RUN_ROOT/kafka_v2_test_slot_owned_candidate"
# baseline 二进制通过临时反向 patch 后构建并保存：
# cp build64_release/src/test/kafka_v2_test "$RUN_ROOT/kafka_v2_test_baseline"

# 每一轮必须独立 group/socket/report，避免 offset 和 socket 干扰。
export TIDE_KAFKA_V2_LARGE_SCALE_E2E=1
export TIDE_KAFKA_V2_BACKPRESSURE_TOPIC="$KAFKA_E2E_LARGE_SCALE_TOPIC"
export TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=1024
export TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=30000000
export TIDE_KAFKA_V2_LARGE_SCALE_PARTITION_COUNT=125
export TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800
export TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0
```

判定模板：

```text
if environment_gate_failed:
    result = inconclusive
elif candidate_avg >= baseline_avg * 1.03 and no semantic regression:
    result = keep
elif candidate_avg <= baseline_avg * 0.97:
    result = reject_or_redesign
else:
    result = neutral_continue_only_if_first_principles_strong
```

### Slot-Owned 原型

第一性原理判断：

```text
baseline:
owner thread: allocate PublishedMessage per record
shard slot: stores raw PublishedMessage*
reclaim: delete PublishedMessage

prototype:
MsgSlot embeds reusable PublishedMessage storage
owner thread: build draft payload
shard fill: move payload into slot-owned storage
reclaim: reset slot metadata, keep storage reusable
```

这个方向和之前的 `Owner allocation shift` 不同：

- `Owner allocation shift` 只是把 `PublishedMessage` allocation 从 owner 线程转移到 shard 线程，本质上没有减少 allocation 总量。
- `Slot-owned message storage` 直接消掉 per-record `PublishedMessage` heap allocation，payload lifetime 绑定到 slot，理论上减少 allocator pressure 和指针生命周期复杂度。
- worker 仍然通过 `RecordView::payloadView` 读 slot 内 payload；revoke / late-ack 路径要求不能在 slot reset 时主动 `clear()` payload，否则会破坏已交给 worker 的 view。

语义验证中先发现一个风险：

```text
RevokeInflightLeaseKeepsPayloadAliveUntilLateAck failed
原因：slot reset 时 clear payload，worker 手里的 payloadView 被写坏
修复：reset 只清 metadata，不主动清 payload；payload 在 slot 下次复用时 overwrite
结果：kafka_v2_test 207/207 passed
```

### 污染环境 A/B 结果

第一次生产对齐 e2e smoke：

| 指标 | Slot-owned message |
|------|--------------------|
| `ackedMsgsPerSec` | `958,050` |
| `durationMs` | `31,315` |
| `avgPollBatchRecords` | `957.544` |
| `avgHandleConsumedLatencyUs` | `271.077` |

这组只能说明 candidate 能跑通，不能与历史 baseline 直接比较。

第一组 `B -> C -> B -> C`：

| Seq | Variant | CPU load | PSI some avg10 | Acked Msg/s | Avg Handle Us |
|-----|---------|----------|----------------|-------------|---------------|
| 1 | baseline | `61.27` | `0.05` | `949,766` | `285.724` |
| 2 | candidate | `65.06` | `0.03` | `1,060,560` | `281.754` |
| 3 | baseline | `58.81` | `0.05` | `991,581` | `279.404` |
| 4 | candidate | `60.46` | `0.08` | `1,002,350` | `275.033` |

第二组 `C -> B -> C -> B`：

| Seq | Variant | CPU load | PSI some avg10 | Acked Msg/s | Avg Handle Us |
|-----|---------|----------|----------------|-------------|---------------|
| 1 | candidate | `64.88` | `0.40` | `916,091` | `270.953` |
| 2 | baseline | `63.30` | `0.03` | `992,657` | `283.039` |
| 3 | candidate | `72.46` | `0.43` | `930,586` | `280.137` |
| 4 | baseline | `68.31` | `0.01` | `968,174` | `285.924` |

当前判断：

- 第一组 candidate 平均高于 baseline，但第二组 candidate 跑在明显更高的 CPU PSI 下，不能直接判负收益。
- `avgHandleConsumedLatencyUs` 多数 candidate 低于 baseline，说明 slot-owned storage 方向可能确实减少了部分 handle path 成本。
- 当前状态是 **继续验证**，不是 reject；需要在 heavy CPU task 停止后的安静窗口复测。

### 安静窗口 A/B 结果（heavy task 停止后）

环境：
```text
load average: 0.39-1.38 (heavy task 已停)
/proc/pressure/cpu some avg10: 0.05-0.90
Redpanda: 8 SMP, ~47% CPU
baseline binary: kafka_v2_test_baseline_quiet (reverted candidate diff)
candidate binary: kafka_v2_test_slot_owned_candidate_quiet (slot-owned storage)
ack target: 30,000,000
partition count: 125
worker count: 48
pollDrain: 128 (from env)
```

Forward `B -> C -> B -> C`：

| Seq | Variant | CPU load | PSI some avg10 | Acked Msg/s | Handle Us | Poll Us |
|-----|---------|----------|----------------|-------------|-----------|---------|
| 1 | baseline | `1.38` | `0.14` | `1,448,300` | `36.255` | `0.175` |
| 2 | candidate | `2.52` | `0.39` | `1,693,200` | `32.392` | `0.107` |
| 3 | baseline | `3.15` | `0.39` | `1,446,910` | `35.979` | `0.181` |
| 4 | candidate | `3.74` | `0.20` | `1,497,080` | `33.144` | `0.175` |

Reverse `C -> B -> C -> B`：

| Seq | Variant | CPU load | PSI some avg10 | Acked Msg/s | Handle Us | Poll Us |
|-----|---------|----------|----------------|-------------|-----------|---------|
| 1 | candidate | `2.26` | `0.74` | `1,657,280` | `32.019` | `0.113` |
| 2 | baseline | `2.79` | `0.05` | `1,512,710` | `34.985` | `0.151` |
| 3 | candidate | `3.11` | `0.90` | `1,432,950` | `37.020` | `0.175` |
| 4 | baseline | `19.56` | `0.05` | `1,459,010` | `35.170` | `0.167` |

分析：

1. **Forward 组**：candidate 平均 `1,595,140 msg/s` vs baseline `1,447,605 msg/s`，**+10.2%**。
2. **Reverse 组**：candidate 平均 `1,545,115 msg/s` vs baseline `1,485,860 msg/s`，**+4.0%**。
3. **handleConsumedLatencyUs**：candidate 稳定 `32-33µs`，baseline `35-36µs`（排除 reverse seq=3 高 PSI 异常点）。
4. **注意**：reverse seq=3 candidate PSI=0.90（某个后台进程短暂活跃），seq=4 baseline load spike 19.56（可能 Redpanda compaction）。
5. **异常点排除后**：forward candidate 一致性更高，reverse 也多数 candidate > baseline。

判定：

```text
candidate_avg_fwd = 1,595,140
baseline_avg_fwd  = 1,447,605
ratio_fwd         = 1.102 (>1.03 门禁)

candidate_avg_rev = 1,545,115
baseline_avg_rev  = 1,485,860
ratio_rev         = 1.040 (>1.03 门禁)

result = KEEP (两组均满足 >3% 门禁，handleLatencyUs 持续降低)
```

结论：**Slot-owned message storage 原型确认 KEEP**，收益 4-10%。

下一步：在 slot-owned 基础上继续寻找 10x 差距的真正瓶颈。当前 1.5-1.7M/s，目标需达到 ~9.5M/s 以上。

## Benchmark Workflow 规范

这次真实 `billion backlog` 重跑再次暴露出一个开发流程问题：

- bench 口径一旦混入 `synthetic preload`、短窗口冲高、不同 topic backlog 状态，结论就会失真
- 如果 report 里缺 `avg batch`，就会把“吞吐掉了是 batch 碎了，还是 CPU 真打满了”混在一起
- 如果 `perf` 不是和同一轮 real e2e 同窗采样，热点和 report 指标就对不上

以后 `consumer_v2` 的性能验收必须固定成下面这条流程，不能再随意变：

```text
1. 固定 real Kafka backlog topic
2. 固定 partition count
3. 固定 ack target / consume timeout
4. 固定 worker read slowdown / perf slowdown
5. 生成 report JSON
6. 同一轮测试全程 perf record
7. report + perf 一起分析
```

### 必须固定的 bench 参数

| 项目 | 规范 |
|------|------|
| topic | 复用同一个真实 `billion backlog` topic，避免 backlog 深度漂移 |
| partitions | 固定 `100`，否则跨轮对比无意义 |
| ack target | 固定长窗口，例如 `30,000,000`，避免只看 2-3s 冲高 |
| produce | 默认 `skip produce=1`，除非明确要测生产路径 |
| read slowdown | 明确记录 `TIDE_KAFKA_V2_BACKPRESSURE_READ_SLEEP_MS` |
| perf slowdown | 明确记录 `TIDE_KAFKA_V2_LARGE_SCALE_PERF_READ_SLEEP_MS` |
| perf | 必须和生成 report 的同一轮 real e2e 同时执行 |

### 必须稳定输出的关键指标

真实 e2e report 以后至少要包含这些字段，缺任何一个都不能当正式 bench：

| 指标 | 作用 |
|------|------|
| `ackedMsgsPerSec` | 稳态吞吐主指标 |
| `durationMs` | 判断是不是短窗口假高点 |
| `ackedDuringPerf` | 对齐 perf 采样窗口 |
| `maxPausedPartitions` | 判断 backpressure 是否真实触发 |
| `maxBufferedRecords` | 判断系统是不是在 buffer/ring 高水位附近运行 |
| `totalReadBatches` | 读取 batch 总数 |
| `totalDispatchedBatches` | 分发 batch 总数 |
| `avgReadBatchSize` | 判断 read path 是否已经把 batch 打碎 |
| `avgDispatchBatchSize` | 判断 dispatch path 是否把 batch 打碎 |

### 2026-05-12 最新 real e2e + perf

本轮重新跑同一真实 topic、同一 `100 partitions`、同一 `30,000,000 ack target`，并且全程挂 `perf record`，结果如下：

| 指标 | 数值 |
|------|------|
| `ackedMsgsPerSec` | `215,077` |
| `ackedDuringPerf` | `30,000,275` |
| `durationMs` | `139,486` |
| `avgReadBatchSize` | `332.203` |
| `avgDispatchBatchSize` | `332.203` |
| `totalReadBatches` | `90,307` |
| `totalDispatchedBatches` | `90,308` |
| `maxPausedPartitions` | `1` |
| `maxBufferedRecords` | `1,147` |

这组数据的价值在于：

- 它证明当前瓶颈不只是“吞吐掉了”，而是 `avg batch` 已经从目标的 `1024` 明显塌到 `332`
- `avgReadBatchSize` 和 `avgDispatchBatchSize` 几乎一致，说明 batch 退化主要发生在 dispatch 之前或 dispatch 入口附近，不是 worker read 之后才碎掉
- 因为同一轮挂了 `perf`，现在可以直接把 `batch 变小` 和 `mutex/futex` 热点放在一个证据链里分析

### 不允许再犯的错误

以后写文档、汇报 benchmark、决定是否继续优化时，禁止再出现下面这些情况：

1. 把 `synthetic bench` 和 `real e2e` 写在同一结论里
2. 把 `2s` 冲高数据当作长期稳态
3. report 缺少 `avg batch` 还继续讨论 dispatch 优化
4. perf 不是和生成 report 的同一轮测试一起跑
5. topic、partition、ack target、slowdown 任意漂移却还做横向对比

## 5x 阶段：Cache Tile 与结构性优化边界

### 关键判断

5x 阶段不能继续把 `perf` 残差当成局部优化清单逐项消掉。当前 `poll -> dispatch -> read -> ack`
链路已经经过批处理、slot ring、contiguous ack reclaim 等多轮优化，`perf` 中剩下的热点更像是高度优化后的结构成本：

- `handleConsumedMessages` 中的 draft 构造、topic/partition 映射、slot fill、dispatch push 都是 1-4% 级别分散成本
- 这些热点单独看都不大，但组合起来代表当前数据流需要多次对象构造、索引映射和跨结构搬运
- 继续做单点小改容易得到局部 perf 下降、real e2e 吞吐不升甚至下降的结果
- 真正有价值的方向应该是 cache tile 粒度收敛，或者重写批处理数据布局和生命周期边界

### Poll Drain Cache Tile Sweep

`TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE` 是当前最直接的 cache tile 参数。它决定一次 blocking poll 后最多用
`consume(0)` 追加多少消息，再整体进入 `handleConsumedMessages`。

同一真实 `billion backlog` topic、`100 partitions`、`30,000,000 ack target` 下的 sweep 结果：

| Poll Drain Tile | Acked Msg/s | Avg Read Batch | Avg Dispatch Batch | 结论 |
|-----------------|-------------|----------------|--------------------|------|
| `64` | `958,195` | `397.037` | `397.037` | 上一轮已提交高水位 |
| `96` | `877,888` | `406.224` | `406.224` | 偏小，batch 提升有限且吞吐退化 |
| `128` | `1.01192e+06` | `486.601` | `486.601` | 当前 no-perf 最优，接近 5x |
| `128 + perf` | `983,133` | `486.481` | `486.479` | 同轮 perf 验收仍高于 `64` 高水位 |
| `256` | `474,613` | `579.067` | `579.070` | 过大，引发反压/调度振荡，吞吐腰斩 |

结论：

- `128` 是当前结构下更好的 cache tile，能把平均 batch 从约 `397` 推到约 `486`
- `256` 证明“更大 batch”不是线性收益，tile 过大后会放大延迟、pause 和调度振荡
- `96` 证明轻微增大 tile 不足以改变结构形态，反而可能落在不稳定区间
- 本轮只固化 `64 -> 128`，把它视为 cache tile 收敛收益，不把它包装成最终 5x 结构答案

### Dispatcher Tile 失败经验

本轮尝试过两种 dispatcher-side batch ingress：

| 方案 | Acked Msg/s | 失败原因 |
|------|-------------|----------|
| `DispatchTile` per-partition vectors | `821,522` | 新增多组 vector、tile 查找和二次搬运，结构成本超过收益 |
| flat `DispatchRecord` batch ingress | `865,902` | 减少了部分 vector，但新增 record vector 与 touched partition 查找，仍低于高水位 |

这两个失败不是“结构方向错误”，而是说明 tile 不能简单叠在现有单条 `pushRecord` 结构之外。真正的结构性重构应该从数据布局源头减少中间态，而不是在已有对象流后面再包一层 batch。

### Live Offset Range 结构改造

`MsgSlotRing` 原本对每条 live offset 都写入 `boost::container::flat_set`。这对乱序 offset 是通用结构，但 Kafka partition
在正常 backlog 消费下更常见的是连续 offset。5x 阶段把这里改成“连续 range 优先，必要时物化 set”：

- `PartitionTracker` 维护 `nextCommittableOffset`、`liveTailOffset`、`liveSlotCount` 和 `liveOffsetsContiguous`
- 连续 fill 只推进 tail，不再逐条插入 `liveOffsets`
- 出现 gap/out-of-order/fallback reclaim 时，才把 `[nextCommittableOffset, liveTailOffset]` 物化成 `flat_set`
- contiguous ack reclaim 在 range 模式下直接校验 head/tail/count，不再做 `liveOffsets.lower_bound`
- fallback `markDone` 仍保留 `doneOffsets`，确保 gap 和 done-prefix commit 语义不变

真实 `billion backlog` 验证：

| 方案 | Acked Msg/s | Avg Read Batch | Avg Dispatch Batch | 结论 |
|------|-------------|----------------|--------------------|------|
| `128 + perf` 基线 | `983,133` | `486.481` | `486.479` | 当前提交线 |
| live range no-perf | `1.00885e+06` | `497.687` | `497.685` | batch 继续变大，接近历史 no-perf 高点 |
| live range + perf | `983,906` | `501.040` | `501.038` | 同轮 perf 小幅高于提交线 |

这次收益很薄，但设计价值明确：它把 steady-state 数据结构从“每条 offset 一次 set 写”改成“partition-local range”。后续如果继续优化
ack/read path，应优先沿着 range/cohort 结构推进，而不是回到单点热点微调。

### Dispatch Pressure Cap Sweep

除了 cache tile，5x 阶段还必须关注调度和反压的动态平衡。`TIDE_KAFKA_V2_PRESSURE_DISPATCH_BATCH_CAP`
决定高压状态下允许 partial dispatch 的最大 batch cap。这个值过小会让 worker batch 太碎，过大会把排队延迟和 buffer 波动放大。

在 `poll drain=128`、live range 已启用的真实 `billion backlog` 场景下：

| Dispatch Cap | Acked Msg/s | Avg Read Batch | Max Buffered | 结论 |
|--------------|-------------|----------------|--------------|------|
| `512 + perf` | `983,906` | `501.040` | 未异常 | 上一轮提交线 |
| `768` | `1.00753e+06` | `493.334` | `2432` | no-perf 正收益，但 buffer 更高 |
| `768 + perf` | `984,610` | `475.294` | `1767` | perf 只小幅高于提交线 |
| `640` | `1.06487e+06` | `508.407` | `1664` | 当前 no-perf 最优，距离 5x 约 1% |
| `640 + perf` | `1.00429e+06` | `490.664` | `2048` | 首个 perf 口径突破 1M 的配置 |

结论：

- `640` 比 `512` 更能释放 worker batch，但没有 `768` 那样扩大 buffer 波动
- `poll drain=144 + cap=768` 虽然把 avg batch 推到 `534.780`，但吞吐掉到 `743,902 msg/s`，说明更大 batch 会越过 L2/调度稳定区
- 本轮固化 `512 -> 640`，这是调度/反压层面的 state-of-art 参数收敛，不是局部代码微调

### Message Allocation Lock Boundary

`handleConsumedMessages` 原本在持有 `SharedConsumerState::mutex_` 时为每条消息执行 `new PublishedMessage`
并把 payload string move 进去。这个成本单条看不大，但会直接拉长全局 consumer mutex 的持有时间，放大 poll
ingress、dispatch schedule、pause/resume 之间的锁竞争。

本轮把 `PublishedMessage` 的构造提前到锁外，用 `std::unique_ptr<PublishedMessage>` 承接失败路径清理；进入锁内后只把裸指针交给
`MsgSlotRing::fillBatch`。成功 fill 后 `release()`，生命周期交给 ring；失败或 close 路径自动析构，保持资源语义不变。

真实 `billion backlog` 验证：

| 方案 | Acked Msg/s | Avg Read Batch | Max Buffered | 结论 |
|------|-------------|----------------|--------------|------|
| lock 外构造 no-perf | `1.05925e+06` | `492.449` | `1664` | 接近 no-perf 高水位，但未超过 `640` 的 `1.06487e+06` |
| lock 外构造 + perf | `1.08492e+06` | `482.628` | `2816` | 首次在 perf 口径超过 5x 目标线，说明减少锁内 allocator/payload 生命周期成本有效 |

这次收益说明 5x 的最后一段不是单纯 cache tile，而是 lock boundary 与 object lifecycle 的共同优化。后续继续优化时，应优先把
allocator、payload ownership、slot fill 和 dispatch metadata 的生命周期边界继续外移/合并，而不是只减少一两次 map lookup。

### 本轮负收益分支

| 方案 | 结果 | 处理 |
|------|------|------|
| `SourceAdapter` batch cursor | `936,603 msg/s` | 撤销。预展开 `readyMessages_` 不是当前主瓶颈，cursor 反而提高 pause/buffer 波动 |
| poll ingress touched-partition pause sync | `964,516 msg/s` | 撤销。per-record pause check 不是主杠杆，去重同步降低 avg batch |
| contiguous reclaim 去掉 `typedSlots` 临时 vector | e2e 早期 `ackBatch=false` | 撤销。raw `slots` 二次遍历跨当前生命周期边界不安全 |
| L2 ingress sub-tile `64` | `984,070 msg/s` | 撤销。降低锁内工作集但切碎 avg batch 到 `467` |
| L2 ingress sub-tile `256` | `758,154 msg/s` | 撤销。tile 过大导致调度退化 |
| `PublishedMessage*` vector 折叠进 draft | `965,823 msg/s` | 撤销。少一个 vector 不是主瓶颈，buffer 波动更高 |
| `poll drain=144 + dispatch cap=768` | `743,902 msg/s` | 撤销。avg batch 过大后延迟/调度稳定性崩坏 |
| single-topic direct partition-index table | `918,085 msg/s` | 撤销。少一次 topic map lookup 不构成主瓶颈，额外分支和 cache state 反而降低真实 e2e |

### 下一步结构方向

如果继续冲 5x，优先考虑下面这些能改变数据流形态的方案：

- 将 `ConsumedMessageDraft -> MsgSlotFill -> PublishedMessage -> DispatchState::pushRecord` 合并为单次 tile 构建，减少多 vector 和对象生命周期搬运
- 按 partition/cohort 构造 poll tile，让 topic/partition 映射、slot fill、dispatch enqueue 在同一 tile 内完成
- 继续把 `MsgSlotRing` range 模式扩展到 done/reclaim lifecycle，减少 fallback 物化成本
- 让 worker read path 直接消费稳定 batch view，避免 `SourceAdapter` 再把 `DispatchItem` 展开成 per-message handle 队列
- 所有结构重构必须保留真实 payload 读取、ack、commit 和 backpressure 语义，不能为了 benchmark 跳过公共 API 契约

### Sweep 操作规范

环境变量 sweep 必须串行跑。多个 real e2e 进程如果共用同一个 topic/group，会触发 rebalance，导致 `waitUntilRuntime`
失败或得到不可比较的数据。

## Bundle Lib 项目的 bench / perf 命令

这次反复踩坑的根因不是 `perf` 本身，而是这个项目的测试二进制采用了 `bundle lib` 方式：

- `kafka_v2_test` 的 ELF interpreter 是相对路径 `./lib/ld-linux-x86-64.so.2`
- 所以不能在任意目录直接执行绝对路径二进制
- 也不能想当然只设 `LD_LIBRARY_PATH`
- 正确做法是切到 `build64_release/src/test`，从这个目录运行 `./kafka_v2_test`

### 先编译

```bash
cd /root/Documents/stream_engine
export DISABLE_CAS=1
bash dev/test_build.sh kafka_v2_test
```

### Blade 构建坑位

这次补充一个很容易浪费时间的现实问题：`kafka_v2_test` 的坑不只在 bundle 运行目录，还在
`blade` 的依赖刷新和全局锁。

#### 1. 缺依赖时先用 `-f` 刷新，不要盲目重复 build

如果 `blade build //src/test:kafka_v2_test` 报这类错误：

```text
//cpp3rdlib/gtest not found when loading BUILD file
```

不要继续重复执行同一条 `build`。这个仓库里正确做法是先按脚本约定触发 `--update-deps`：

```bash
cd /root/Documents/stream_engine
export DISABLE_CAS=1
bash dev/test_build.sh kafka_v2_test -f
```

原因是 `dev/test_build.sh` 里只有带 `-f` 时才会先执行：

```text
blade clean //src/test:kafka_v2_test --update-deps
```

等依赖刷新完成后，再重新执行一次正常 build：

```bash
cd /root/Documents/stream_engine
export DISABLE_CAS=1
bash dev/test_build.sh kafka_v2_test
```

#### 2. `/.blade_global.lock` 是串行锁，不要并发开第二个 build

脚本内部通过 `flock` 使用：

```text
/data24/otf/stream_engine/.blade_global.lock
```

来保证同一时间只有一个 `blade` 在写 `build64_release/output`。因此如果已经有一轮 build 在跑，
再开第二轮就会一直看到：

```text
[INFO] waiting for blade lock: /data24/otf/stream_engine/.blade_global.lock
```

这通常不是“代码卡住”，而是：

1. 前一轮 `blade` 还没结束
2. 或者前一轮已经进入超长链接阶段，后续 build 都在排队等锁

#### 3. `kafka_v2_test` 链接阶段很重，看起来像假死

`kafka_v2_test` 会把很多 `consumer_v2` 测试对象和大量静态/动态库打进一个 bundle 二进制，
链接命令非常长。常见现象是：

- 终端长时间没有新日志
- CPU 上还能看到 `ld.gold`
- 锁还被第一轮 build 持有

这时优先判断“是否仍在链接”，不要立刻再发起第二轮构建。

可以用下面两条命令看当前是不是仍在真正工作：

```bash
ps -eo pid,ppid,stat,etime,cmd | grep -E 'blade|bootstrap.py|ld.gold .*build64_release/src/test/kafka_v2_test' | grep -v grep
lslocks | grep '.blade_global.lock' || true
```

如果能看到 `ld.gold ... build64_release/src/test/kafka_v2_test`，说明它还在做最终链接，不是死锁。

#### 4. 只有确认是残留进程时才清理

如果已经明确是上一轮残留的 `blade/flock/ld.gold` 没退干净，先清掉残留，再重新发起一次单独 build。
不要在锁等待期间连续按多次编译命令，否则只会叠加更多排队进程。

建议清理后重新走一遍：

```bash
cd /root/Documents/stream_engine
export DISABLE_CAS=1
bash dev/test_build.sh kafka_v2_test
```

#### 5. 推荐构建工作流

后续为了减少误判，`kafka_v2_test` 的推荐顺序固定为：

```text
1. 第一次失败如果是缺依赖，先 `bash dev/test_build.sh kafka_v2_test -f`
2. 刷依赖完成后，只启动一次 `bash dev/test_build.sh kafka_v2_test`
3. 确认 `build64_release/src/test/kafka_v2_test` 已生成
4. 切到 `build64_release/src/test` 再执行 `./kafka_v2_test`
```

### 验证 bundle 运行目录

先确认二进制确实依赖相对 `lib/`：

```bash
cd /root/Documents/stream_engine
file build64_release/src/test/kafka_v2_test
readelf -l build64_release/src/test/kafka_v2_test | grep 'Requesting program interpreter'
```

预期能看到：

```text
Requesting program interpreter: ./lib/ld-linux-x86-64.so.2
```

这意味着后续跑测试和 `perf` 时，工作目录必须是：

```bash
cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
```

不要在仓库根目录直接跑下面这种命令：

```bash
/data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test/kafka_v2_test ...
```

否则很容易报：

```text
No such file or directory
```

它不是文件真的不存在，而是相对 interpreter `./lib/ld-linux-x86-64.so.2` 找不到。

### 启动 Kafka 环境

```bash
cd /root/Documents/stream_engine
bash dev/kafka_e2e/start.sh
```

### real e2e bench 命令

如果已经有灌好的 `billion backlog` topic，优先复用它，不要重复生产：

```bash
cd /root/Documents/stream_engine

export DISABLE_CAS=1
export TIDE_KAFKA_V2_BACKPRESSURE_E2E=1
export TIDE_KAFKA_V2_LARGE_SCALE_E2E=1
export TIDE_KAFKA_V2_BACKPRESSURE_COUNT=1000000000
export TIDE_KAFKA_V2_BACKPRESSURE_TOPIC=tide-kafka-v2-billion-1778472647-1009376
export TIDE_KAFKA_V2_BACKPRESSURE_BROKERS=127.0.0.1:9092
export TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH=/tmp/tide-kafka-v2-large-scale/worker_real_bench.sock
export TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=30000000
export TIDE_KAFKA_V2_LARGE_SCALE_REPORT_PATH=.dbg/billion-backpressure-report-manual.json
export TIDE_KAFKA_V2_LARGE_SCALE_PARTITION_COUNT=100
export TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800
export TIDE_KAFKA_V2_BACKPRESSURE_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_LARGE_SCALE_PERF_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_E2E_PSM=${TIDE_KAFKA_V2_E2E_PSM:-${TIDE_ENGINE_PSM:-data.systi.tide}}
export TIDE_KAFKA_V2_E2E_OWNER=${TIDE_KAFKA_V2_E2E_OWNER:-huliang}
export TIDE_KAFKA_V2_E2E_TEAM=${TIDE_KAFKA_V2_E2E_TEAM:-data-ti-data}

cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
timeout 7200 ./kafka_v2_test \
  --gtest_filter=UnifiedConsumerBackpressureE2eTest.BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf
```

跑完后直接看 report：

```bash
cat /root/Documents/stream_engine/.dbg/billion-backpressure-report-manual.json
```

### real e2e + perf 命令

`perf` 必须挂在同一轮 real e2e 上，不能先跑 bench 再单独跑 perf：

```bash
cd /root/Documents/stream_engine

export DISABLE_CAS=1
export TIDE_KAFKA_V2_BACKPRESSURE_E2E=1
export TIDE_KAFKA_V2_LARGE_SCALE_E2E=1
export TIDE_KAFKA_V2_BACKPRESSURE_COUNT=1000000000
export TIDE_KAFKA_V2_BACKPRESSURE_TOPIC=tide-kafka-v2-billion-1778472647-1009376
export TIDE_KAFKA_V2_BACKPRESSURE_BROKERS=127.0.0.1:9092
export TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH=/tmp/tide-kafka-v2-large-scale/worker_real_perf.sock
export TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=30000000
export TIDE_KAFKA_V2_LARGE_SCALE_REPORT_PATH=/root/Documents/stream_engine/.dbg/billion-backpressure-report-perf.json
export TIDE_KAFKA_V2_LARGE_SCALE_PARTITION_COUNT=100
export TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800
export TIDE_KAFKA_V2_BACKPRESSURE_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_LARGE_SCALE_PERF_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_E2E_PSM=${TIDE_KAFKA_V2_E2E_PSM:-${TIDE_ENGINE_PSM:-data.systi.tide}}
export TIDE_KAFKA_V2_E2E_OWNER=${TIDE_KAFKA_V2_E2E_OWNER:-huliang}
export TIDE_KAFKA_V2_E2E_TEAM=${TIDE_KAFKA_V2_E2E_TEAM:-data-ti-data}

cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
timeout 7200 perf record -F 99 -g --call-graph dwarf \
  -o /root/Documents/stream_engine/.dbg/perf-e2e-real.data \
  ./kafka_v2_test \
  --gtest_filter=UnifiedConsumerBackpressureE2eTest.BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf
```

### perf 报告命令

```bash
cd /root/Documents/stream_engine
perf report --stdio --sort symbol -i .dbg/perf-e2e-real.data | head -n 120
```

如果要保留火焰图输入，也可以先折叠栈：

```bash
cd /root/Documents/stream_engine
perf script -i .dbg/perf-e2e-real.data > .dbg/perf-e2e-real.unfold
```

### 推荐的最小验收清单

每次 bench / perf 后，至少检查下面几项：

1. `report` 是否来自 real e2e，而不是 synthetic bench
2. `durationMs` 是否足够长，不能只是 2-3 秒
3. `avgReadBatchSize` 和 `avgDispatchBatchSize` 是否齐全
4. `maxPausedPartitions >= 1`，确认 backpressure 真实触发
5. `lastError` 是否为空
6. `perf` 是否和这份 report 是同一轮测试

## 反压控制实验经验

### 失败实践：静态提高 partial dispatch 阈值

2026-05-12 做过一次很有价值但不能提交的实验：

```cpp
// src/source/kafka/consumer_v2/dispatch_state.cpp
// bad experiment: 100 partitions no longer use adaptive partial dispatch.
if (partitionCnt > 256) {
    ...
}
```

实验目标是验证 `avgReadBatchSize/avgDispatchBatchSize ~= 333` 是否来自 100 partitions 下的
adaptive threshold。结论如下：

| 项目 | 结果 |
|---|---|
| avg batch | 从约 `333` 拉到约 `1020`，说明判断正确，batch 确实被 adaptive partial dispatch 截断 |
| real e2e 稳定性 | 不正常，长窗口和短窗口都触发 broker 断联 |
| broker 状态 | Redpanda 两次 `Exited (139)`，日志显示 `seastar_memory` hard failure / failed allocation |
| perf 形态 | `rd_kafka_toppar_pause_resume` / `rd_kafka_q_purge_toppar_version` 升到约 `24%` |
| 是否提交 | 不提交，代码已恢复 |

重要结论：

1. 失败不等于没有收益，必须先看 runtime JSON、broker log 和 perf。
2. 静态放大 batch 会把压力转移到 librdkafka pause/resume 和 broker fetch path。
3. 只追 `avg batch ~= 1024` 是危险的；必须同时看吞吐、pause/resume 频率、broker 稳定性和 `lastError`。
4. 下一轮反压重设计不应硬改 `canDispatchBatch()` 阈值，而应做动态控制：
   - ring / worker backlog 接近高水位时降 poll 或降 resume 频率
   - pause/resume 必须有 hysteresis 和 cooldown，避免每批触发 purge
   - batch 聚合应由 dispatcher/worker queue 的可消费能力决定，而不是简单扩大 partition buffer 等待阈值
5. Redpanda OOM 后不要直接继续判定代码结果；需要先确认 broker 已恢复、topic metadata 可读，再重跑同一实验。

安全测试约束：

1. real e2e / perf 命令避免删除操作，不使用 `rm -f` 清理旧 report/perf。
2. 每轮使用唯一 `RUN_ID`、唯一 `REPORT_PATH`、唯一 `PERF_PATH`，避免旧文件污染。
3. 如果 broker 刚重启，先确认 topic metadata 可读，再进入性能测试。

### 失败实践：只放大 high/low watermark

另一个验证过但不提交的实验是把 real e2e 的测试配置从：

```cpp
highWatermark = 1024;
lowWatermark = 512;
```

临时调成：

```cpp
highWatermark = 4096;
lowWatermark = 2048;
```

这轮测试在 `Redpanda --smp=8 --memory=100G` 环境下完整通过，但结果没有收益：

| 项目 | 结果 |
|---|---|
| ackedMsgsPerSec | `214,818`，低于 `partitionIndex cache` 后的 `218,595` |
| avgDispatchBatchSize | `332.548` |
| avgReadBatchSize | `332.548` |
| perf 形态 | `handleConsumedMessage` 仍约 `51%`，`readBatch/ackBatch/ring` 仍是主线 |
| 是否提交 | 不提交，代码已恢复 |

结论：

1. 当前 `avg batch ~= 333` 的直接原因不是 pause hysteresis 太窄。
2. 单独放大 `highWatermark/lowWatermark` 不能突破 adaptive partial dispatch 截断。
3. 如果要同时保持大 batch 和稳定反压，必须重设计 dispatch 放行策略和 pause/resume cooldown，而不是只调水位。

### 小收益实践：批量 ring ack cleanup

提交 `bc129f19b` 把 worker ack 后的 ring cleanup 从：

```text
for each slot:
  markDone(slot)  // lock ring once
reclaim()         // lock ring once
```

改成：

```text
markDoneAndReclaim(slots)
  lock ring once
  mark all done
  advance committable offset once per affected partition
  reclaim once
```

这轮 real e2e + perf 结果：

| 项目 | 结果 |
|---|---|
| ackedMsgsPerSec | `215,863`，吞吐基本持平 |
| avgDispatchBatchSize | `332.177` |
| avgReadBatchSize | `332.177` |
| ackBatch perf 子树 | 从约 `10.9%` 降到 `6.17%` |
| 结论 | 有局部收益，已提交；但不是 2x 主杠杆 |

经验：

1. 降低一个热点不一定立刻提升整体吞吐，瓶颈会转移。
2. 这次优化证明 ack path 的 per-slot ring lock 是真实成本，可以保留。
3. 下一步主攻方向应转向 `readBatch` 等待/唤醒、`handleConsumedMessage` 全局锁和 `avg batch` 截断。


### 高收益实践：poll loop 批量 drain

提交 `24d853b0b` 把 poll loop 从每轮只处理一条 `consume()` 结果，改成：

```text
consume(pollTimeoutMs)    // 阻塞拿第一条
consume(0) ... consume(0) // drain 当前已就绪消息，最多 N 条
handleConsumedMessages(batch)
```

关键点不是硬编码参数，而是先把参数变成进程启动加载一次的环境变量：

| 环境变量 | 默认值 | 说明 |
|---|---:|---|
| `TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE` | `64` | 每个 poll loop 最多 drain 的 ready messages |

这符合后续调参原则：先编译一次，再用 env sweep 找最优，最后把最优值固化为默认值。

本轮 real e2e sweep：

| `TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE` | Acked Msg/s | 结论 |
|---:|---:|---|
| `8` | `349,160` | 有收益，但 drain 不够 |
| `16` | `366,621` | 继续提升 |
| `32` | `425,288` | 接近高点 |
| `64` | `440,329` | 本轮最高，固化为默认 |
| `128` | `439,003` | 与 `64` 基本持平，额外 batch 不再明显收益 |

固化默认 `64` 后，同一 real e2e + perf 结果：

| 指标 | 数值 |
|---|---:|
| `ackedMsgsPerSec` | `435,036` |
| `avgReadBatchSize` | `413.481` |
| `avgDispatchBatchSize` | `413.482` |
| `maxPausedPartitions` | `1` |
| `maxBufferedRecords` | `1,536` |
| `lastError` | `""` |

与阶段基线对比：

| 阶段 | Acked Msg/s | 提升 |
|---|---:|---:|
| avg batch report 基线 | `215,077` | `1.00x` |
| 删除 per-record worker wakeup | `279,577` | `1.30x` |
| poll drain 默认 `64` | `435,036` | `2.02x` |

经验：

1. poll 热路径每条消息一次 `consume()` + 一次处理函数调用，是 `handleConsumedMessage` 热点的重要组成。
2. 批量 drain 能提升吞吐，也能把 `avg batch` 从约 `332` 拉到约 `413`，但还没回到目标 `1024`。
3. `64` 和 `128` 差距很小，继续放大 drain 上限不是下一轮主杠杆。
4. 最新 perf 的下一轮热点应转向 `MsgSlotRing::acquireWait/fill` 和 `WorkerQueue::ackBatch`。

### 高收益实践：ring slot 批量 acquire/fill

提交 `43ffbc3f3` 在 poll drain 之后继续减少 ring 锁粒度：

```text
before:
  for each drained message:
    acquireWait(slot)  // lock ring
    fill(slot)         // lock ring again

after:
  acquireWaitBatch(N)  // lock/wait once, drain currently free slots
  fillBatch(N)         // lock ring once, update trackers in batch
```

本轮仍然用同一真实 `billion backlog` topic、`100 partitions`、`30,000,000 ack target`，并和 report 同窗 `perf record`：

| 指标 | 数值 |
|---|---:|
| `ackedMsgsPerSec` | `574,891` |
| `ackedDuringPerf` | `30,000,099` |
| `durationMs` | `52,184` |
| `avgReadBatchSize` | `394.09` |
| `avgDispatchBatchSize` | `394.09` |
| `maxPausedPartitions` | `2` |
| `maxBufferedRecords` | `2,496` |
| `lastError` | `""` |

阶段对比：

| 阶段 | Acked Msg/s | 相对最初 avg batch 基线 |
|---|---:|---:|
| avg batch report 基线 | `215,077` | `1.00x` |
| poll drain 默认 `64` | `435,036` | `2.02x` |
| ring slot 批量 acquire/fill | `574,891` | `2.67x` |

经验：

1. poll drain 之后，ring 的 per-record mutex/futex 成本成为真实热点，批量 API 是有效主杠杆。
2. `avg batch` 没有继续变大，吞吐提升主要来自 CPU/锁开销下降，而不是 batch 聚合变大。
3. 最新 perf 中 ring acquire/fill mutex 占比下降，新热点转向 `flat_set` offset tracking 和 ack path。
4. 下一轮不应再盲目加大 poll drain，而应优化 `PartitionTracker::liveOffsets/doneOffsets` 的 offset 推进成本。

### 高收益实践：连续 ack 直接 reclaim

提交 `de7cce3eb` 继续优化 ack path。真实 worker ack 的常见形态是：

```text
DispatchItem:
  partition = P
  offsets   = [N, N+1, ..., N+K]
  slots     = same partition, same order
```

旧路径即使已经是同 partition 连续 ack，也会走：

```text
markDoneAndReclaim
  for each slot:
    doneOffsets.insert(offset)
  advanceCommittableOffset()
    doneOffsets.erase(begin) repeatedly
  reclaimLocked()
    liveOffsets.erase(offset) repeatedly
    doneOffsets.erase(offset) repeatedly
```

新 fast path 只在安全条件全部满足时触发：

| 条件 | 目的 |
|---|---|
| 所有 slot 都是 `Dispatched` | 避免重复 ack / 非法状态 |
| 所有 slot 属于同一 partition | 保证 range reclaim 语义简单 |
| offset 在 slot 顺序中连续 | 可用 range erase |
| 起点等于 `nextCommittableOffset` | 保持 gap 语义 |
| `reclaimableIndexes_` 为空 | 避免和慢路径遗留状态交错 |

触发后直接：

```text
liveOffsets.erase([start, end])
nextCommittableOffset = end + 1 or -1
delete messages
reset slots
push freeIndexes
```

本轮 real e2e + perf：

| 指标 | 数值 |
|---|---:|
| `ackedMsgsPerSec` | `958,195` |
| `ackedDuringPerf` | `30,000,129` |
| `durationMs` | `31,309` |
| `avgReadBatchSize` | `397.037` |
| `avgDispatchBatchSize` | `397.037` |
| `maxPausedPartitions` | `1` |
| `maxBufferedRecords` | `1,567` |
| `lastError` | `""` |

阶段对比：

| 阶段 | Acked Msg/s | 相对最初 avg batch 基线 |
|---|---:|---:|
| avg batch report 基线 | `215,077` | `1.00x` |
| poll drain 默认 `64` | `435,036` | `2.02x` |
| ring slot 批量 acquire/fill | `574,891` | `2.67x` |
| offset advance begin 快路径 | `616,148` | `2.86x` |
| 连续 ack 直接 reclaim | `958,195` | `4.45x` |

经验：

1. `3x` 目标不是靠继续调 batch 参数达成，而是靠消除 ack path 的 per-offset `flat_set` churn。
2. `avg batch` 仍在 `~397`，吞吐提升主要来自 ack CPU 降本，不是更大 batch。
3. perf 中 `WorkerQueue::ackBatch` 子树从约 `16%` 降到约 `6.5%`，`markDoneAndReclaim` 从约 `14.7%` 降到约 `3.75%`。
4. 下一轮如果继续冲极限，热点已经转向 poll/read 路径对象构造：`ConsumedMessageDraft`、`TopicPartition`、`RecordView` 和 `PublishedMessage` 删除成本。

### 止步实践：topic/RecordView 小对象改造

连续 ack 直接 reclaim 之后，perf 显示 read/poll 路径中有 `ConsumedMessageDraft`、`TopicPartition`、`RecordView` 构造/析构成本。尝试过两个小对象改造：

| 实验 | 结果 | 处理 |
|---|---:|---|
| `ConsumedMessageDraft` 保存 topic 指针 | 编译警告：`topic_name()` 返回临时对象，存在悬垂风险 | 立即停止，不跑 e2e |
| `ConsumedMessageDraft` 保存 `std::string topic` + cache-hit helper | `831,191 msg/s` | 低于 `958,195` 基线，撤销 |
| `RecordView.topic` 改 `std::string_view` | `882,025 msg/s` | 低于 `958,195` 基线，撤销 |

经验：

1. 小对象构造在 perf 中可见，不代表改掉就一定提升，可能改变内存布局和 batch 行为。
2. `RdKafka::Message::topic_name()` 不能保存引用或指针，安全性优先于少一次字符串拷贝。
3. `RecordView.topic` 是对外接口，改成 `string_view` 会扩散到测试和 adapter，除非有明确收益，否则不值得提交。
4. 当前高水位基线保持 `de7cce3eb` 的 `958,195 msg/s`。

## 2026-05-18 Direct SPSC Baton + Work Stealing A/B

本轮按生产对齐 `10B` backlog 口径重新跑 current vs baseline，避免把不同日期、不同环境的 baseline 混在一起比较。

固定参数：

- topic: `tide-kafka-v2-tenb-prod125w48-data00-1779068785`
- partitions: `125`
- workers: `48`
- ack target: `30,000,000`
- pollDrain: `128`
- backlog: `10,000,000,000`
- read slowdown: `0`
- perf slowdown: `0`

结果：

| Variant | Direct Dispatch | Acked Msg/s | Duration Ms | Avg Read Batch | Avg Dispatch Batch | Avg Handle Us | Report |
|---------|-----------------|-------------|-------------|----------------|--------------------|---------------|--------|
| current SPSC baton + work stealing | `1` | `894,573` | `33,536` | `178.740` | `178.740` | `39.074` | `.dbg/billion-e2e-spsc-baton-ab-1779109130-current.json` |
| same-window baseline dispatcher | `0` | `344,840` | `86,997` | `145.218` | `145.218` | `272.412` | `.dbg/billion-e2e-baseline-ab-1779109231.json` |

Same-window ratio:

```text
894,573 / 344,840 = 2.59x
```

结论：

- 本轮不能再拿旧的 `1.32M` baseline 直接判定 current 负收益；同窗口 baseline 只有 `344K`。
- current SPSC baton + work stealing 在同窗口、同 topic、同 partitions、同 workers 下是 **2.59x 正收益**。
- current 的 `avgHandleConsumedLatencyUs` 从 baseline 的 `272us` 降到 `39us`，说明绕开 legacy dispatcher/slot path 的收益真实存在。
- current 的 `avgReadBatchSize` 也高于 baseline（`178.7` vs `145.2`），work stealing 没有把 batch 打碎到不可接受。
- 后续判断必须继续使用同窗口 A/B，不能把不同日期、不同机器负载下的 single report 横向比较。

## 2026-05-19 Direct SPSC eBPF: Sticky Bind Once

本轮目标：再次跑 `10B` e2e，用 eBPF/perf 判断 current direct dispatch 的瓶颈点，只保留有正收益的优化。

固定参数：

- topic: `tide-kafka-v2-tenb-prod125w48-data00-1779068785`
- partitions: `125`
- workers: `48`
- ack target: `30,000,000`
- pollDrain: `128`
- direct dispatch: `TIDE_KAFKA_V2_E2E_DIRECT_DISPATCH=1`
- read slowdown: `0`
- perf slowdown: `0`

### 证据

current report: `.dbg/billion-e2e-direct-ebpf-1779161405.json`

| Metric | Value |
|--------|------:|
| `ackedMsgsPerSec` | `620,685` |
| `durationMs` | `48,334` |
| `avgReadBatchSize` | `131.114` |
| `avgHandleConsumedLatencyUs` | `112.927` |

perf/eBPF 结论：

- `pthread_mutex_lock` probe 在采样窗口内没有 direct hot-path 事件，说明本轮瓶颈不是 mutex。
- `readBatchDirect` children 约 `42.17%`，其中 `LaneSignal::wait` 约 `18.71%`，worker 侧等待/读批仍是最大成本。
- `ackBatchDirect` children 约 `9.50%`，主要来自 per-batch `flat_map<TopicPartition,...>` 聚合和 topic string copy。
- owner 侧 `bindDirectPartitionWorker` children 约 `4.25%`，原因是每条 message 都对已经 sticky 的 partition 重复执行 `flat_map::operator[]`。
- owner 侧 `directWorkerForPartition` / `TopicPartitionLess` string compare 也可见，后续可继续优化为 assignment-time partition index / slot。

### 优化

最小改动：sticky worker 已存在时，不再重复调用 `bindDirectPartitionWorker()`。

```text
before:
  every dispatched message -> bindDirectPartitionWorker(topicPartition, worker)

after:
  if !stickyWorkerExists:
      bindDirectPartitionWorker(topicPartition, worker)
```

该改动不改变 partition 顺序语义：

- 首条 record 仍会绑定 selected worker。
- 后续同一 topic-partition 仍只投递到 sticky worker。
- 只是去掉已经 sticky 后的重复 `flat_map::operator[]` 写入。

### 结果

optimized report: `.dbg/billion-e2e-direct-bind-once-1779161685.json`

| Variant | Acked Msg/s | Duration Ms | Avg Read Batch | Avg Handle Us | Report |
|---------|------------:|------------:|---------------:|--------------:|--------|
| current direct before bind-once | `620,685` | `48,334` | `131.114` | `112.927` | `.dbg/billion-e2e-direct-ebpf-1779161405.json` |
| direct bind-once | `681,123` | `44,045` | `147.749` | `91.389` | `.dbg/billion-e2e-direct-bind-once-1779161685.json` |

收益：

```text
681,123 / 620,685 = 1.097x
```

结论：

- 本轮优化保留，same-window direct e2e 提升约 **9.7%**。
- perf 复测中 `bindDirectPartitionWorker` 不再出现在 direct dispatch top hotspot。
- 剩余主要瓶颈转移到 `directWorkerForPartition` 的 sticky map find/string compare、`readBatchDirect` wait/pop、`ackBatchDirect` 聚合。
- 下一轮优先考虑 assignment-time partition index / fixed slot，减少每条消息构造 `TopicPartition` 和 flat_map string compare；不要继续在 `bind` 路径上优化。

## 2026-05-19 Direct SPSC eBPF: Reassign Trial And Rollback

本轮目标是验证 `5x` 提升方向，重点回答两个问题：

- work stealing / reassign 是否会破坏 SPSC 语义。
- 当前瓶颈是否来自 worker 分布不均，是否能通过 Kafka owner 线程内 reassign 扩大并行度。

### SPSC 语义判断

结论：

- reassign 不必然破坏 SPSC，但只能由 Kafka owner 线程执行。
- SPSC lane 的 producer 仍必须只有 Kafka owner，consumer 仍必须只有对应 worker。
- partition 迁移只能发生在该 partition outstanding 为 `0` 时，也就是 owner 确认没有 queued / inflight record。
- 如果 partition 还有 outstanding record，继续粘在原 worker；否则会破坏 Kafka partition 内顺序和 contiguous commit 语义。

### 观测字段

新增 per-worker direct 观测字段，并输出到 `/json` 与 10B report：

- `directWorkerPushedRecords`
- `directWorkerReadRecords`
- `directWorkerAckedRecords`

这些字段用于判断 direct dispatch 是否真实使用了所有 worker，而不是只看总吞吐。

### Baseline 证据

baseline report: `.dbg/billion-e2e-direct-observe-1779163155.json`

| Metric | Value |
|--------|------:|
| `ackedMsgsPerSec` | `676,300` |
| `durationMs` | `44,359` |
| `avgReadBatchSize` | `137.786` |
| `avgHandleConsumedLatencyUs` | `90.654` |
| active workers | `18 / 48` |
| hottest / coldest non-zero worker | `12.4x` |

top workers:

| Worker | Acked Records |
|--------|--------------:|
| `1` | `4,073,140` |
| `2` | `3,866,583` |
| `0` | `3,845,913` |
| `4` | `3,545,586` |
| `5` | `3,415,151` |

perf/eBPF:

- `pthread_mutex_lock` probe 仍没有 direct hot-path 事件。
- `directWorkerForPartition` 的 `flat_map` / string compare 仍可见。
- `ackBatchDirect` 约 `10%`，来自 topic-partition 聚合和 string copy。

### Bad Trial

尝试内容：

- 将 direct ring 元素从裸 `RdKafka::Message*` 改成 `{message, directSlotIndex}`。
- `ackBatchDirect` 改成 slot-index 聚合，避免 per-record topic string / flat_map。
- Kafka owner 线程维护 partition / worker outstanding，仅当 partition outstanding 为 `0` 时允许 reassign 到 least-loaded worker。

trial report: `.dbg/billion-e2e-direct-slot-reassign-1779163798.json`

| Metric | Baseline | Trial |
|--------|---------:|------:|
| `ackedMsgsPerSec` | `676,300` | `629,871` |
| `durationMs` | `44,359` | `47,629` |
| `avgReadBatchSize` | `137.786` | `233.524` |
| `avgHandleConsumedLatencyUs` | `90.654` | `108.495` |
| active workers | `18 / 48` | `2 / 48` |
| hottest / coldest non-zero worker | `12.4x` | `948.7x` |

判断：

- slot-index ack 本身有效：perf 中 `ackBatchDirect` 从约 `10%` 降到 `3.4%`。
- least-loaded reassign 使用 outstanding 作为负载信号是错误的：在 owner poll 批处理窗口内，它把流量聚集到极少数 worker。
- 该试验不保留，已回滚 reassign 和 slot-ack 逻辑。

### Rollback 结果

rollback report: `.dbg/billion-e2e-direct-revert-1779164267.json`

| Metric | Value |
|--------|------:|
| `ackedMsgsPerSec` | `694,396` |
| `durationMs` | `43,203` |
| `avgReadBatchSize` | `140.262` |
| `avgHandleConsumedLatencyUs` | `90.478` |
| active workers | `18 / 48` |
| hottest / coldest non-zero worker | `12.4x` |

结论：

- 坏优化已回滚，吞吐和 worker 分布回到 baseline 区间。
- 当前 workload 只有 `18` 个活跃 direct lanes；如果不拆 Kafka partition，理论上无法靠 reassign 达到 `5x`。
- 下一轮真正可能接近数量级提升的方向，不是简单 work stealing，而是：
- 方向 A：先证明 topic 内是否只有 `18` 个 hot partitions；如果是，5x 需要 partition 级之外的并行模型，但这会触碰 partition-order 语义。
- 方向 B：保留 partition 顺序，做 assignment-time partition slot cache，消掉每条消息的 `TopicPartition` 构造和 `flat_map` string compare。
- 方向 C：单独重做 slot-index ack，但不要绑定错误的 least-loaded reassign；需要先设计 direct record metadata 如何不增加 owner 侧成本。

## 2026-05-19 Direct SPSC: Assignment Slot Cache + Slot-index Ack

本轮目标：

- 跑 `10B` backlog e2e，并用 perf/eBPF 追当前瓶颈。
- 根据上一轮结论实现 assignment-time partition slot cache，消掉 owner direct hot path 的 `TopicPartition` 构造和 topic string compare。
- 如果 perf 显示 ack path 仍有 topic-partition map/string 成本，再单独重做 slot-index ack，但不再引入 least-loaded reassign。
- 尽最大努力冲 `5x`，但不能破坏 SPSC lane ownership 和 Kafka partition order。

固定参数：

- topic: `tide-kafka-v2-tenb-prod125w48-data00-1779068785`
- backlog: `10,000,000,000`
- partitions: `125`
- workers: `48`
- direct dispatch: `TIDE_KAFKA_V2_E2E_DIRECT_DISPATCH=1`
- pollDrain: `128`
- read slowdown: `0`
- perf slowdown: `0`

### Assignment-time Slot Cache

实现内容：

- assignment/revoke 阶段维护 `topic/partition -> committed slot` cache。
- 单 topic 场景直接按 `partition` 查 `directSingleTopicSlots_`，命中时不调用 `message.topic_name()`。
- multi-topic/cache miss 保留 `topic_name()` + map fallback。
- sticky worker owner 从 `TopicPartition -> worker` 改成 `slotIndex -> worker`，避免 direct hot path 继续做 topic string compare。

标准 `30M ack target` 结果：

| Variant | Acked Msg/s | Duration Ms | Avg Handle Us | Avg Read Batch | Active Workers | Report |
|---------|------------:|------------:|--------------:|---------------:|---------------:|--------|
| direct rollback baseline | `694,396` | `43,203` | `90.478` | `140.262` | `18 / 48` | `.dbg/billion-e2e-direct-revert-1779164267.json` |
| assignment slot cache | `1,755,720` | `17,087` | `9.566` | `133.184` | `21 / 48` | `.dbg/billion-e2e-direct-slot-cache-1779167234.json` |

收益：

```text
1,755,720 / 694,396 = 2.53x
```

perf 结论：

- `handleConsumedMessagesDirect` children 降到约 `4.9%`。
- `directWorkerForMessage` 只剩约 `0.6%`，说明 owner lookup 的 `TopicPartition`/string compare 瓶颈基本消掉。
- 新 top hotspot 转到 worker side：`readBatchDirect` children 约 `41.9%`，其中 `LaneSignal::wait` 约 `34.5%`。
- `ackBatchDirect` 仍可见，且仍有 per-batch topic-partition 聚合与 `MessageImpl` destroy 成本。

长窗口 `100M ack target` 复核：

| Metric | Value |
|--------|------:|
| `ackedMsgsPerSec` | `1,716,800` |
| `durationMs` | `58,248` |
| `avgHandleConsumedLatencyUs` | `10.351` |
| active workers | `18 / 48` |
| report | `.dbg/billion-e2e-direct-slot-cache-ebpf-1779168134.json` |
| perf | `.dbg/perf-direct-slot-cache-ebpf-1779168134.data` |

eBPF 线程采样：

```text
@[rdk:main]: 9
@[rdk:broker0]: 1126
@[kcv2pe492e0-0]: 2110
@[ld-linux-x86-64]: 2997
```

解释：

- owner poll 线程仍有 CPU，但已经不是唯一主瓶颈。
- `ld-linux-x86-64` 是通过 bundled loader 启动测试时未单独命名的 worker/read threads 聚合。
- `pthread_mutex_lock` probe 没有 direct hot-path 输出，mutex 仍不是当前瓶颈。

### Slot-index Ack

上一轮坏试验把 slot ack 和 least-loaded reassign 绑在一起，导致 active workers 塌缩。本轮只保留 slot-index metadata，不做 reassign：

```text
owner -> worker ring:
  before: RdKafka::Message*
  after : { RdKafka::Message*, directSlotIndex }

worker ack:
  before: RecordView.topic/partition -> TopicPartition -> flat_map aggregate
  after : directSlotIndex -> fixed array aggregate -> DirectAck{slotIndex, offset, count}

owner apply ack:
  before: DirectAck.topic/partition -> TopicPartition map lookup -> slot
  after : DirectAck.slotIndex -> directCommittedSlots_[slotIndex]
```

该改动不改变 SPSC 语义：

- Kafka owner 仍是 direct worker lane 的唯一 producer。
- 每个 worker lane 仍只有对应 worker consumer。
- sticky worker 仍按 slot 绑定，partition order 不迁移。
- ack 只改变 metadata lookup，不改变消息生命周期；`RdKafka::Message` 仍在 ack durable queued 后释放。

标准 `30M ack target` 结果：

| Variant | Acked Msg/s | Duration Ms | Avg Handle Us | Avg Read Batch | Active Workers | Report |
|---------|------------:|------------:|--------------:|---------------:|---------------:|--------|
| assignment slot cache | `1,755,720` | `17,087` | `9.566` | `133.184` | `21 / 48` | `.dbg/billion-e2e-direct-slot-cache-1779167234.json` |
| slot cache + slot-index ack | `2,032,800` | `14,758` | `9.180` | `131.232` | `21 / 48` | `.dbg/billion-e2e-direct-slot-ack-1779169803.json` |
| slot cache + slot-index ack + pollDrain 1024 | `2,445,590` | `12,267` | `36.606` | `131.322` | `21 / 48` | `.dbg/billion-e2e-direct-slot-ack-poll1024-1779172650.json` |
| e2e defaults after config change | `2,448,980` | `12,250` | `36.244` | `131.249` | `18 / 48` | `.dbg/billion-e2e-direct-default-1779173443.json` |

增量收益：

```text
2,032,800 / 1,755,720 = 1.16x
2,032,800 / 694,396   = 2.93x
2,445,590 / 2,032,800 = 1.20x  (pollDrain 1024 vs 128)
2,445,590 / 694,396   = 3.52x
```

Worker QPS / idle 结论：

| Run | Active | Idle | Median Active QPS | Min Active QPS | Max Active QPS | Max/Min |
|-----|-------:|-----:|------------------:|---------------:|---------------:|--------:|
| slot-index ack, pollDrain 128 | `21 / 48` | `27 / 48` | `28,387` | `21,204` | `275,995` | `13.02x` |
| slot-index ack, pollDrain 1024 | `21 / 48` | `27 / 48` | `34,151` | `25,509` | `332,040` | `13.02x` |
| e2e defaults after config change | `18 / 48` | `30 / 48` | `57,244` | `26,783` | `332,501` | `12.41x` |
| slot-index ack, 100M long window | `20 / 48` | `28 / 48` | `13,920` | `7,928` | `368,557` | `46.49x` |

Top worker QPS from `pollDrain=1024`:

```text
worker 1: 332,040 msg/s
worker 0: 313,517 msg/s
worker 2: 294,354 msg/s
worker 4: 261,081 msg/s
worker 6: 252,123 msg/s
worker 5: 249,490 msg/s
worker 3: 238,882 msg/s
worker 26: 64,970 msg/s
```

结论：worker 明显不均衡，不是 equally busy；`27~28 / 48` workers 基本 idle。当前 direct SPSC 的吞吐被 hot partitions / active lanes 限制，继续提高单条 lookup/ack 效率能提升总吞吐，但不能让 idle workers 自动变忙。

perf 结论：

- `ackBatchDirect` 的 topic-partition `flat_map` / string compare 不再出现在 top hotspot。
- `readBatchDirect` 仍是最大项，children 约 `38.7%`。
- `LaneSignal::wait` 仍约 `32.6%`，说明剩余主要是 worker wait / lane utilization，而不是 owner lookup。
- `handleConsumedMessagesDirect` 约 `5.7%`，`directWorkerForMessage` 约 `1.3%`。

长窗口 `100M ack target` 结果：

| Run | Acked Msg/s | Duration Ms | Avg Handle Us | Avg Read Batch | Active Workers | Evidence |
|-----|------------:|------------:|--------------:|---------------:|---------------:|----------|
| perf window | `~1.72M` | `~60s` | - | - | - | `.dbg/perf-direct-slot-ack-ebpf-1779170423.data` |
| comm eBPF window | `2,533,640` | `39,469` | `7.705` | `131.181` | `20 / 48` | `.dbg/billion-e2e-direct-slot-ack-comm-ebpf-1779171287.json` |

comm-filter eBPF:

```text
@[rdk:main]: 29
@[rdk:broker0]: 2033
@[kcv2pe492e0-0]: 3154
@[ld-linux-x86-64]: 3931
```

mutex eBPF:

```text
pthread_mutex_lock probe: no direct hot-path output
```

### 5x 判断

以同窗口 legacy dispatcher baseline `344,840 msg/s` 为分母：

```text
2,533,640 / 344,840 = 7.35x
```

以最近 direct rollback baseline `694,396 msg/s` 为分母：

```text
2,533,640 / 694,396 = 3.65x
```

结论：

- 相对 legacy dispatcher，同一生产对齐口径已经超过 `5x`。
- 相对上一轮 direct rollback baseline，本轮达到 `3.65x`，但未达到 direct-to-direct `5x`。
- 当前剩余瓶颈不是 mutex，也不是 owner `TopicPartition` lookup；主要是 worker lane wait、只有约 `18~21 / 48` active workers，以及 librdkafka per-record poll/wrapper/destructor 成本。
- 如果坚持 direct-to-direct 再冲 `5x`，下一步不能再靠简单 reassign；需要先证明 hot partition 分布，或者设计 partition-order-preserving 的更粗粒度 read/ack tile，避免让 48 个 worker 中大部分长期空等。

### Default Config Decision

本轮把最佳实测配置升级为默认：

- `UnifiedConsumer::Config::pollDrainBatchSize = 2048` 是当前默认
- `TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE` env fallback 当前为 `2048`
- 10B e2e 默认从 `pollDrain=512` 逐步演进到 `2048`
- 10B e2e 默认启用 direct dispatch，仍可用 `TIDE_KAFKA_V2_E2E_DIRECT_DISPATCH=0` 显式回退 dispatcher
- `UnifiedConsumer::Config::directDispatchEnabled` 通用 API 默认仍保留 `false`，因为默认回归里大量 synthetic `publish()` / dispatcher 语义测试依赖非 direct path；生产 direct path 由 source/e2e config 显式启用。

原因：

- `pollDrain=1024` 在 legacy dispatcher 早期实验中是最佳值；C-level batch sweep 后，当前默认升级为 `2048`。
- direct dispatch + slot cache + slot-index ack 是当前唯一超过 `5x legacy baseline` 的路径。
- 保留 env/config override，线上如果遇到特殊 topic 分布或兼容问题仍可回退。

## 2026-05-19 Direct SPSC: Payload-aware Initial Placement Trial

目标：

- 当前 direct default 只有 `18~21 / 48` workers active，用户判断 assign/reassign 应该以 equal worker payload 为目标。
- 先按 `docs/consumer_v2/design/single_client_spsc_dispatch_architecture.md` 的 `TAG: DIRECT-PAYLOAD-BALANCED-ROUTING` 做最小试验：只改 unbound slot 的 initial placement，不做 slot reassignment。
- 仍保持 SPSC：Kafka owner 是唯一 producer，worker 不 stealing，不迁移已有 queued/inflight slot。

实现试验：

```text
before:
  unbound worker = partition % workerCount

trial:
  owner tracks approximate worker load
  unbound worker = lightest registered worker
  sticky after first bind
  no reassignment
```

验证：

```text
bash dev/test_build.sh kafka_v2_test                     passed
env -u large-e2e-vars bash dev/test_run.sh kafka_v2_test  207/207 passed
10B direct e2e 30M target                                passed
```

结果：

| Variant | Acked Msg/s | Duration Ms | Avg Handle Us | Active Workers | Idle Workers | Hot/Cold |
|---------|------------:|------------:|--------------:|---------------:|-------------:|---------:|
| direct default before trial | `2,448,980` | `12,250` | `36.244` | `18 / 48` | `30 / 48` | `12.41x` |
| payload-aware initial placement trial | `2,126,320` | `14,109` | `77.807` | `24 / 48` | `24 / 48` | `13.02x` |

Evidence:

- baseline report: `.dbg/billion-e2e-direct-default-1779173443.json`
- trial report: `.dbg/billion-e2e-direct-payload-balanced-1779176748.json`

结论：

- 这是负收益，已回滚代码，未保留实现。
- active workers 从 `18` 增加到 `24`，但 throughput 从 `2.45M` 降到 `2.13M`，约 `-13.2%`。
- handle latency 从 `36us` 升到 `78us`，说明“把 slot 更平均地撒给更多 worker”不等于吞吐提升。
- 失败原因：first-placement 只能影响 slot 首次绑定；它没有识别真实 hot partitions，也没有把已绑定 hot slot 从 overloaded worker 安全迁走。它还把热流量集中到新的 worker 区间，hot/cold ratio 没有改善。

下一步：

- 不再做 naive initial placement。
- 需要先加只读观测：per-slot QPS、slot owner、worker owned slots、queued/inflight 估计。
- reassign 必须只在 owner 线程执行，且只移动 `queued=0 && inflight=0` 的 slot。
- 移动策略必须基于 hot slot score 和 source/target worker delta，而不是简单 least-loaded。

## 2026-05-19 Direct SPSC: Guarded Online Rebalance Trial

目标：

- 按 `TAG: DIRECT-PAYLOAD-BALANCED-ROUTING` 的在线方案做一次最小闭环：owner 线程按 slot 观测 worker payload，并且只在安全窗口迁移 slot owner。
- 保持 direct SPSC 语义：Kafka owner 是唯一 data-ring producer；worker 不 stealing；slot 迁移只允许在 `queued=0 && inflight=0` 时发生。
- 先用 env gate 试验，不改变 production default；如果 10B e2e 负收益则回滚代码，只保留测试结论。

试验实现：

```text
env gate:
  TIDE_KAFKA_V2_DIRECT_BALANCE_ENABLED=1

owner accounting:
  push -> queued++
  worker pop -> inflight++ then queued--
  owner ack drain -> inflight--

rebalance:
  interval = 1000ms
  maxMoves = 2 per interval
  minGain = 120%
  cooldown = 30000ms
  movable only when queued == 0 && inflight == 0
```

验证：

```text
bash dev/test_build.sh kafka_v2_test                     passed
env -u large-e2e-vars bash dev/test_run.sh kafka_v2_test  207/207 passed
10B direct e2e 30M target with balance enabled            passed
safe bpftrace pthread_mutex_lock probe                    no direct hot-path output
```

10B 结果：

| Variant | Acked Msg/s | Duration Ms | Avg Handle Us | Active Workers | Idle Workers | Hot/Cold | Report |
|---------|------------:|------------:|--------------:|---------------:|-------------:|---------:|--------|
| direct default before trial | `2,448,980` | `12,250` | `36.244` | `18 / 48` | `30 / 48` | `12.41x` | `.dbg/billion-e2e-direct-default-1779173443.json` |
| payload-aware initial placement trial | `2,126,320` | `14,109` | `77.807` | `24 / 48` | `24 / 48` | `13.02x` | `.dbg/billion-e2e-direct-payload-balanced-1779176748.json` |
| guarded online rebalance trial | `2,345,780` | `12,789` | `62.853` | `20 / 48` | `28 / 48` | `17.49x` | `.dbg/direct-balance-10b-.json` |

Pros:

- 语义方向正确：所有 slot owner 修改都在 Kafka owner 线程执行，没有 worker stealing。
- 安全条件正确：迁移只尝试 `queued=0 && inflight=0` 的 slot，避免破坏 partition order / contiguous commit。
- regression 和 10B e2e 都通过，说明 basic correctness 没有被破坏。
- mutex eBPF 没有 direct hot-path 输出，保持了 no-lock direct hot path 的核心原则。
- 相比 naive initial placement，吞吐回升到 `2.35M`，说明“安全在线迁移”比“只改首次分配”更接近正确方向。

Cons:

- 仍是负收益：相对 direct default 从 `2.449M` 降到 `2.346M`，约 `-4.2%`。
- `avgHandleConsumedLatencyUs` 从 `36.244us` 升到 `62.853us`，owner 端 accounting / scan 成本过高。
- active workers 只从 `18` 增加到 `20`，但 hot/cold 从 `12.41x` 恶化到 `17.49x`，没有实现 equal worker payload。
- 30M 窗口太短，`30s` cooldown 基本只允许少量迁移；但降低 cooldown 又会增加 owner scan / churn 风险。
- 测试运行暴露了一个 harness 坑：直接 `export LD_LIBRARY_PATH=build64_release/src/test/lib` 后再调用 `date`，会让宿主工具误加载 bundle 里的旧 `libc.so.6`；后续应在展开 report path 后再设置 `LD_LIBRARY_PATH`，或只对 `./kafka_v2_test` 子进程设置。

判定：

```text
result = reject_and_rollback_code
reason = throughput negative, handle latency higher, worker skew not improved
```

结论：

- 代码已回滚，未保留 guarded online rebalance 实现。
- 这次试验证明“安全迁移条件”不是难点，真正难点是 migration decision 的收益模型和观测粒度。
- 下一轮不能继续做每次 owner drain 的全 slot scan；应先做只读、低成本的 per-slot / per-worker telemetry，再离线计算 epoch plan。
- 如果仍要在线迁移，迁移动作应基于更长窗口的 hot-slot score，并且只在明确减少 `max(workerQps) / p50(workerQps)` 时触发。

## 2026-05-21 Periodic Commit Async A/B

本轮目标：

- 验证 periodic commit 从 `commitSync()` 改为 `commitAsync()` 是否贡献吞吐，或者是否伤害性能。
- 同时验证新增 commit callback metrics 能不能进入 e2e report，避免只看到 `commitAsync()` 本地 return 成功，却看不到 broker callback 结果。
- e2e 经验必须写在本文，而不是只写到 `docs/analysis/consumer_v2_6511_ebpf_bottleneck_analysis.md`。

### 先修正测试口径

之前的 `3M` bounded e2e 能作为 smoke，但不能回答 async commit 性能问题：

```text
3M records
  -> duration ~= 1-2s
  -> commitIntervalMs = 5000
  -> periodic commit 基本没有被稳定触发
  -> 不能判断 commitAsync vs commitSync
```

正确口径至少要覆盖多个 commit interval：

```text
30M records / 125 partitions / 48 workers
  -> duration ~= 17s
  -> commitIntervalMs = 5000
  -> periodicCommitCalls = 4
  -> callback / failure / lag freshness 字段进入 report
```

### A/B 方法

本轮在同一个 topic 上跑两次，避免 topic 数据分布差异：

```text
+---------------------------------------------------+
| produce once                                      |
| topic: tide-kafka-v2-commit-ab-async-1779364026   |
| records: 30,000,000                               |
| partitions: 125                                   |
+--------------------------+------------------------+
                           |
                           v
+--------------------------+------------------------+
| Run A: async default                              |
| group: group-commit-ab-async-1779364026           |
| periodic commit: commitAsync                      |
+--------------------------+------------------------+
                           |
                           v
+--------------------------+------------------------+
| Run B: sync forced                                |
| group: group-commit-ab-sync-1779364257            |
| TIDE_KAFKA_V2_FORCE_SYNC_PERIODIC_COMMIT=1        |
| periodic commit: commitSync                       |
+---------------------------------------------------+
```

新增临时/诊断开关：

| Env | 默认 | 用途 |
|---|---:|---|
| `TIDE_KAFKA_V2_FORCE_SYNC_PERIODIC_COMMIT` | `0` | 仅用于 A/B，将 periodic commit 强制回 `commitSync()`；生产默认仍是 async periodic commit |

e2e report 新增 commit 观测字段：

| 字段 | 用途 |
|---|---|
| `totalCommitCalls` / `totalPeriodicCommitCalls` | 确认测试窗口内 commit 确实发生 |
| `totalCommitFailures` | commit API 或 callback 显式失败 |
| `totalCommitCallbacks` / `totalCommitCallbackFailures` | async broker callback 可观测性 |
| `totalCommitCallbackPartitions` / `totalCommitCallbackSucceededPartitions` | callback 分区级成功数 |
| `lastCommitCallbackLatencyMs` | async callback 延迟；sync-forced 没有 pending async 元数据时为 `-1` |
| `lagRateWindowMs` / `brokerCommittedDelta` / `brokerCommittedCatchup` | 判断 broker committed rate 是正常窗口还是 catch-up |

### 命令

Async default:

```bash
RUN_ID=commit-ab-async-$(date +%s)
TOPIC="tide-kafka-v2-${RUN_ID}"
REPORT_PATH=".dbg/billion-e2e-${RUN_ID}.json"
printf '%s\n' "$TOPIC" > .dbg/last-commit-ab-topic.txt

KAFKA_E2E_RUN_ID="$RUN_ID" \
KAFKA_E2E_LARGE_SCALE_TOPIC="$TOPIC" \
KAFKA_E2E_LARGE_SCALE_SKIP_PRODUCE=0 \
KAFKA_E2E_LARGE_SCALE_COUNT=30000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=30000000 \
KAFKA_E2E_LARGE_SCALE_REPORT_PATH="$REPORT_PATH" \
KAFKA_E2E_LARGE_SCALE_TEST_TIMEOUT_SEC=1800 \
KAFKA_E2E_BULK_CHUNK_SIZE=50000 \
KAFKA_E2E_BULK_PARALLELISM=16 \
TIDE_KAFKA_V2_E2E_GROUP_ID="group-${RUN_ID}" \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=1024 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

Sync-forced comparison:

```bash
TOPIC=$(cat .dbg/last-commit-ab-topic.txt)
RUN_ID=commit-ab-sync-$(date +%s)
REPORT_PATH=".dbg/billion-e2e-${RUN_ID}.json"

TIDE_KAFKA_V2_FORCE_SYNC_PERIODIC_COMMIT=1 \
KAFKA_E2E_RUN_ID="$RUN_ID" \
KAFKA_E2E_LARGE_SCALE_TOPIC="$TOPIC" \
KAFKA_E2E_LARGE_SCALE_SKIP_PRODUCE=1 \
KAFKA_E2E_LARGE_SCALE_COUNT=30000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=30000000 \
KAFKA_E2E_LARGE_SCALE_REPORT_PATH="$REPORT_PATH" \
KAFKA_E2E_LARGE_SCALE_TEST_TIMEOUT_SEC=1800 \
TIDE_KAFKA_V2_E2E_GROUP_ID="group-${RUN_ID}" \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=1024 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

### 结果

| Variant | Periodic commit | Acked Msg/s | Duration Ms | Periodic Commits | Callbacks | Callback Failures | Last Callback Latency | Avg Poll Batch | Avg Read Batch | Avg Handle Us | Report |
|---------|-----------------|------------:|------------:|-----------------:|----------:|------------------:|----------------------:|---------------:|---------------:|--------------:|--------|
| async default | `commitAsync` | `1,698,950` | `17,658` | `4` | `4` | `0` | `1ms` | `960.369` | `255.857` | `39.184` | `.dbg/billion-e2e-commit-ab-async-1779364026.json` |
| sync forced | `commitSync` | `1,713,400` | `17,509` | `4` | `4` | `0` | `-1` | `961.107` | `255.859` | `38.065` | `.dbg/billion-e2e-commit-ab-sync-1779364257.json` |

Ratio:

```text
sync_forced / async_default = 1,713,400 / 1,698,950 = 1.0085x
```

### 判断

```text
result = neutral_for_throughput
reason = difference is only ~0.85%, within same-machine e2e noise
```

本轮不能证明 `commitAsync` 直接提升吞吐；本地 Redpanda 的 sync commit 很快，4 次 periodic commit 对 `30M` 消费窗口的 CPU/latency 影响很小。

但本轮也不能证明 `commitAsync` 伤害性能：

- 两轮都完整 ack `30,000,000` records。
- 两轮 `totalCommitFailures = 0`。
- async default 的 `totalCommitCallbacks = 4`，`totalCommitCallbackFailures = 0`。
- async default 的 `lastCommitCallbackLatencyMs = 1ms`，说明本地 broker callback 很快。
- `brokerCommittedCatchup = false`，本轮没有把 broker committed rate spike 当成吞吐收益。

### 保留 async 的原因

`commitAsync` 的价值不在本地 30M e2e 的平均吞吐，而在生产风险边界：

- periodic commit 不再让 Kafka owner thread 等 broker/coordinator reply。
- commit result 通过 `OffsetCommitCb` 进入 `/json`、`/prometheus`、HTML 和 e2e report。
- revoke/shutdown/checkpoint 等 correctness barrier 仍然保留 `commitSync()`。
- 如果线上 coordinator 抖动或 OffsetCommit tail latency 变大，async periodic commit 可以避免 commit 等待直接打断 poll/drain。

### 后续测试规范

以后验证 commit 相关改动时，不要用短窗口 smoke 直接下结论：

| 要求 | 原因 |
|---|---|
| `durationMs > 3 * commitIntervalMs` | 至少覆盖多次 periodic commit |
| report 必须包含 callback counters | async return 不等于 broker commit 成功 |
| 同 topic、不同 group 跑 A/B | 固定数据分布，同时避免 offset 污染 |
| 固定 `partitionCount / workerCount / pollDrain / ackTarget` | 避免把形态变化误判为 commit 收益 |
| 同时看 `brokerCommittedCatchup` | catch-up spike 不能算吞吐提升 |

本轮结论：**async periodic commit 是正确的 owner-thread 风险优化和 observability 优化；吞吐贡献在本地 e2e 中为中性，没有可见负收益。**

### eBPF Follow-Up: Callback Mutex

看到 `offset_commit_cb()` 中有 `std::lock_guard<std::mutex> lock(mutex_)` 后，追加了一轮 eBPF 追踪，避免靠静态代码猜测。

追踪点：

| Probe | 目的 |
|---|---|
| `SharedConsumerState::offset_commit_cb` | callback 次数和总耗时 |
| `pthread_mutex_lock` | 只统计 callback 内部 mutex wait |
| `rd_kafka_commit` | commit 调用次数 |
| `rd_kafka_consumer_poll` | poll 调用次数 |
| `SharedConsumerState::executeCommitOffsetRequest` | owner-lane commit 执行耗时 |

放大口径：

```text
30M records / 125 partitions / 48 workers
commitIntervalMs = 100
same topic, fresh group
```

有效 eBPF 结果：

| Metric | Value |
|---|---:|
| `offset_commit_cb` hits | `230` |
| `rd_kafka_commit` hits | `229` |
| `executeCommitOffsetRequest` hits | `229` |
| `rd_kafka_consumer_poll` hits | `5,052,302` |
| callback duration | mostly `4-8us` |
| callback mutex wait | mostly `1us` |
| callback mutex failures | `0` |

判断：

- callback 在 Kafka owner thread（`kc-1-*`）上执行，因此 callback 必须保持很短。
- 当前 mutex 是正确性需要：保护 `pendingAsyncCommits_`、`periodicCommitInFlight_` 和 commit metrics，与 owner/metrics/debug snapshot 共享状态。
- eBPF 没有看到 callback mutex contention；锁等待主要是 `1us`，不是 async commit 慢的原因。
- eBPF probe 本身会明显拖慢吞吐，因此不能把带 probe 的 `ackedMsgsPerSec` 当作性能结论。

No-eBPF `100ms` A/B：

| Variant | Acked Msg/s | Duration Ms | Periodic Commits | Callback Failures | Last Callback Latency |
|---|---:|---:|---:|---:|---:|
| async default | `1,615,510` | `18,570` | `97` | `0` | `1ms` |
| sync forced | `1,616,030` | `18,564` | `105` | `0` | `-1` |

结论：

```text
async_default ~= sync_forced
difference = 0.03%
root cause of previous "async slower" signal = measurement noise / probe overhead, not callback mutex
```

因此当前不应为了这个 mutex 做复杂无锁化。更有价值的后续优化是减少不必要的高频 commit，或者在生产环境观察 coordinator tail latency 时再考虑更细粒度的 commit callback telemetry。

### Lock 与单 Kafka Owner 理念

`docs/consumer_v2/design/single_client_spsc_dispatch_architecture.md` 的核心理念是对的：

```text
Kafka client owner is single-threaded
  -> poll / pause / resume / commit / rebalance / callback
  -> owner-local state should not need mutex on hot path
```

但当前实现还没有完全达到这个 ownership 边界：

```text
Kafka owner thread
  -> offset_commit_cb()
  -> poll loop
  -> commit lane
  -> updates SharedConsumerState

HTTP /json, /prometheus, HTML thread
  -> RegistryDebugSnapshot
  -> SharedConsumerState::buildRuntimeInfo()
  -> reads SharedConsumerState

e2e / test thread
  -> report builder
  -> reads RuntimeInfo
```

因此当前 callback 里的 `mutex_` 不是因为 Kafka callback 多线程并发，而是因为 metrics / debug snapshot / test report 会从非 owner 线程读同一份 `SharedConsumerState`。在当前实现里直接删锁会有 data race 风险；在目标架构里应该把它消掉。

推荐演进方向：

| 方案 | 是否符合目标架构 | 说明 |
|---|---|---|
| 保留当前短锁 | 临时可接受 | eBPF 证明 callback 内锁等待约 `1us`，不是当前瓶颈 |
| callback 状态改为 owner-local，周期性发布 immutable snapshot | 推荐 | HTTP/test 只读 snapshot，不碰 owner mutable state |
| 高频 counters 改为 `std::atomic`，复杂结构走 snapshot | 推荐 | counters 无锁读，vector/map 不跨线程共享 |
| 在 callback 里继续扩展复杂 metrics 逻辑 | 不推荐 | callback 在 owner thread，必须保持短小 |
| 为了本轮 eBPF 结果做 lock-free queue | 不推荐 | 证据不支持，复杂度高，收益低 |

目标形态：

```text
KafkaOwner mutable state
  pendingAsyncCommits
  periodicCommitInFlight
  commit counters
          |
          | owner thread publishes
          v
Atomic / immutable RuntimeSnapshot
          |
          | non-owner read only
          v
HTTP metrics / HTML / e2e report
```

结论：

- 从设计理念看，callback hot path 最终不应依赖 `mutex_`。
- 从当前代码边界看，直接删 lock 不安全，因为非 owner 线程还在读共享状态。
- 正确修复不是“在 callback 里裸写共享字段”，而是把 metrics/debug/report 改成 snapshot 发布模型。
- 在改成 snapshot 之前，当前短锁是 correctness guard，不是性能瓶颈。

### Kafka Owner No-Lock / No-Heavy-Work Contract

后续演进必须把 Kafka owner 作为严格的 realtime-ish loop 对待。owner 线程职责只包含：

```text
KafkaOwner loop
  poll Kafka
  drain owner-local lanes
  apply pause / resume / commit API
  publish cheap runtime snapshot
```

owner 线程禁止事项：

| 禁止项 | 原因 | 替代方案 |
|---|---|---|
| `std::mutex` / blocking lock | owner 被阻塞会直接影响 poll/drain/heartbeat | owner-local state + SPSC lanes + atomic snapshot |
| condition-variable wait | 可能引入不可控唤醒延迟 | bounded drain budget + short Kafka poll timeout |
| 大量 `fmt::format` / string 拼接 | callback / poll path 上 CPU 抖动明显 | 只记录 enum/code，非 owner 线程格式化 |
| per-record heap allocation | cache miss 和 allocator contention | slot index / batch object / preallocated buffer |
| 在 callback 中遍历大 vector 做复杂统计 | callback 运行在 owner 线程 | callback 只做 O(partition-count) 的必要计数，复杂分析放 snapshot consumer |
| 在 owner 中做 HTTP / JSON / Prometheus 生成 | 文本序列化很重 | HTTP 线程读取 immutable snapshot |
| 高频日志 | 日志锁、格式化和 IO 都可能反压 owner | rate limit + counter + sampled event |
| eBPF/perf probe 作为吞吐结论 | probe 改变 hot path 成本 | tracing run 与 throughput run 分离 |

允许的 owner 操作：

| 允许项 | 约束 |
|---|---|
| Kafka API | 只在 owner 线程调用；periodic commit 用 async，barrier 用 sync |
| owner-local map/vector 更新 | 不跨线程读写；容量尽量构造期固定 |
| SPSC lane drain/push | 每轮有 budget，不能无限 drain |
| atomic counter store/add | 只用于轻量 runtime 指标 |
| immutable snapshot publish | 固定周期；copy 大小受控；不做字符串渲染 |

### Lock 演进计划

目标不是“把 `mutex_` 换成另一个 lock”，而是消灭 owner mutable state 的跨线程共享。

```text
current
  owner thread writes SharedConsumerState
  HTTP/test thread reads SharedConsumerState
  -> mutex protects shared object

target
  owner thread writes OwnerState only
  owner thread publishes RuntimeSnapshot
  HTTP/test thread reads RuntimeSnapshot only
  -> no lock on owner hot path
```

分阶段迁移：

| Phase | 改动 | 验证 |
|---|---|---|
| L0: classify state | 把 `SharedConsumerState` 字段标注为 owner-only / atomic-metric / snapshot-only / cross-thread-control | code review 确认没有未知共享字段 |
| L1: callback slim-down | `offset_commit_cb` 只更新 owner-local commit state，不做字符串格式化，不碰全局 metrics 聚合 | eBPF callback duration p99 保持 `<= 10us` |
| L2: publish snapshot | owner 周期性构造 `RuntimeSnapshot`，通过 atomic shared pointer 或 double-buffer sequence 发布 | TSAN / stress 下 HTTP 读不碰 owner lock |
| L3: metrics read-side switch | `/json`、`/prometheus`、HTML、e2e report 全部读取 snapshot | socket service test 覆盖新字段 |
| L4: remove owner lock | 删除 callback/poll/commit hot path 的 `mutex_`，只保留 lifecycle / registry 冷路径锁 | eBPF 证明 owner path 无 `pthread_mutex_lock` hit |
| L5: budget heavy work | 对 owner 每轮 commit、pause/resume、snapshot publish 设置 budget，超出延后 | long e2e 无 heartbeat/poll starvation |

推荐数据模型：

```text
struct OwnerState {
  CommitState commit;
  AssignmentState assignment;
  LagState lag;
  PollStats poll;
  // owner thread only
};

struct RuntimeSnapshot {
  RuntimeInfo info;
  vector<PartitionSnapshot> partitions;
  // immutable after publish
};

class SnapshotPublisher {
  publish(RuntimeSnapshot&& snapshot);  // owner only
  shared_ptr<const RuntimeSnapshot> load();  // non-owner
};
```

snapshot 发布方式选择：

| 方案 | 适用 | 注意 |
|---|---|---|
| `std::shared_ptr<const RuntimeSnapshot>` + atomic load/store | 实现简单，适合先落地 | publish 频率不能太高，避免 refcount 抖动 |
| double-buffer + sequence lock | 更低 overhead | reader 需要 retry，结构不能持有复杂 ownership |
| counters 用 atomic，复杂结构用 snapshot | 推荐组合 | atomic 只放单值 counter，不放一致性强的复合状态 |

owner snapshot 周期建议：

| Snapshot | 周期 | 内容 |
|---|---:|---|
| hot counters | `1s` 或按现有 metrics tick | poll/read/commit counters |
| lag snapshot | `5s` | broker committed / acked / high watermark |
| partition detail | `5s-10s` 或按需 | 每 partition lag/paused/offset |
| debug detail | 手动触发或低频 | 不进入 owner 每轮 loop |

callback 最终形态：

```text
offset_commit_cb(err, offsets):
  now = clock
  successCount = count_success(offsets)
  ownerState.commit.applyCallback(err, successCount, now)
  maybePublishCommitCounter()
```

callback 中仍要避免：

- 不在成功路径格式化 topic/partition 字符串。
- 不在成功路径写日志。
- 不访问 HTTP/debug registry。
- 不做跨线程通知，除非是 lock-free / wait-free 的轻量 signal。

验收标准：

| 项目 | 标准 |
|---|---|
| owner hot path lock | eBPF `pthread_mutex_lock` 在 `kc-*` owner poll/callback path 中为 `0` |
| callback latency | `offset_commit_cb` p99 `<= 10us`，p999 `<= 50us` |
| snapshot freshness | `/json` 暴露 `snapshotAgeMs`，正常小于 `2 * publishIntervalMs` |
| metrics correctness | callback success/failure counters 与 e2e report 一致 |
| regression | `bash dev/test_run.sh kafka_v2_test` 通过 |
| stable e2e | median throughput 无回退，commit failures 为 `0` |

## 2026-05-21 Stable E2E Methodology

本轮 async/sync A/B 暴露了一个更重要的问题：**单次 e2e 数字不够稳定，不能用一次 run 的 0.x%-几% 差异判断优化成败。**

### 发现的波动来源

| 波动来源 | 现象 | 影响 |
|---|---|---|
| 首轮 run | 第一次消费常带有 Redpanda / OS cache / group join warmup | 容易偏慢或偏快 |
| eBPF probe | hot function uprobe 会显著拖慢 poll/callback 路径 | 只能用于延迟/次数证据，不能用于吞吐对比 |
| topic 数据分布 | 如果 produce 不均匀，部分 worker 有数据、部分 worker 空闲 | worker 维度 report 看起来不稳定 |
| group 启动阶段 | rebalance、metadata、initial fetch 会混入总 duration | 短窗口下占比过高 |
| commit 次数少 | 默认 `5000ms` commit interval，在 17s run 只有约 4 次 commit | 无法稳定判断 commit 策略收益 |
| 本机资源竞争 | Docker/Redpanda/test process 共享 CPU、IO、page cache | run-to-run jitter |
| 观察动作 | 频繁 curl `/json` 或带 tracing 会改变目标路径 | 观测可能影响被观测对象 |

### 稳定测试原则

```text
one-shot e2e
  -> useful for smoke
  -> not enough for perf conclusion

paired repeated e2e
  -> same topic
  -> different group
  -> alternating order
  -> median / MAD
  -> stable enough for perf conclusion
```

Perf 结论必须满足：

| 规则 | 建议 |
|---|---|
| 最小运行窗口 | `durationMs >= 60s`，或至少 `durationMs > 10 * commitIntervalMs` |
| 最小 commit 样本 | commit 相关 A/B 至少 `>= 50` 次 periodic commit |
| 重复次数 | 每个 variant 至少 `5` 次，采用 `ABBAAB` 或随机交错 |
| 统计口径 | 使用 median，附带 MAD / min / max |
| 有效差异阈值 | `delta > max(3%, 3 * MAD%)` 才认为有吞吐收益 |
| warmup | 第一轮只 warmup，不进入结论 |
| topic | 同一 topic，不同 group；避免重新 produce 引入分布差异 |
| binary | 同一个 binary 连续跑；不要在 A/B 中间 rebuild |
| tracing | throughput run 禁止 eBPF/perf probe；tracing 单独跑 |
| correctness | 每轮必须检查 `ackedDuringPerf == targetAckCount` 和 `totalCommitFailures == 0` |

### 稳定 Runner 方案

建议把稳定 A/B 固化成一个脚本，而不是每次手写命令。脚本职责：

```text
stable_runner
  build/check binary once
  start Kafka once
  produce topic once
  warmup once
  run variants in paired order
  validate each report
  aggregate median/MAD
  emit markdown table
```

脚本输入：

| 参数 | 默认建议 | 说明 |
|---|---:|---|
| `topic` | 自动生成 | 一组 A/B 共用同一 topic |
| `records` | `100000000` | 保证 `>= 60s` 或接近真实长窗口 |
| `partitions` | `125` | 与目标压测形态一致 |
| `workers` | `48` | 与目标压测形态一致 |
| `runsPerVariant` | `5` | 少于 5 次只算 smoke |
| `order` | `warmup,A,B,B,A,A,B,B,A,A,B` | 抵消顺序/缓存影响 |
| `commitIntervalMs` | `1000` | commit A/B 时保证 commit 样本足够 |
| `pollDrainBatchSize` | `2048` | 固定变量 |
| `requireBackpressure` | `0` | commit 对比不强制 backpressure |

运行约束：

- A/B 之间不 rebuild。
- A/B 之间不重新 produce。
- 每轮使用 fresh group id。
- throughput run 不 attach eBPF，不开 perf record，不频繁 curl metrics。
- tracing run 单独执行，只回答“哪里慢”，不回答“吞吐是多少”。
- Redpanda/Docker 启动后先 warmup，避免首轮 metadata/group/cache 成本污染结果。

每轮报告必须校验：

| 字段 | 要求 |
|---|---|
| `ackedDuringPerf` | 等于 `targetAckCount` |
| `durationMs` | 大于稳定窗口阈值 |
| `totalPeriodicCommitCalls` | commit A/B 至少 `>= 50` |
| `totalCommitFailures` | `0` |
| `totalCommitCallbackFailures` | `0` |
| `brokerCommittedCatchup` | 性能结论 run 中应为 `false`，否则标记 suspicious |
| `jsonEndpointOk/htmlEndpointOk/prometheusEndpointOk` | 全部为 `true` |
| `finalRdkThreadCount` | 符合线程治理预期 |

聚合输出必须包含：

| 输出 | 用途 |
|---|---|
| median `ackedMsgsPerSec` | 主吞吐判断 |
| MAD / min / max | 判断波动范围 |
| per-run commit failures | correctness gate |
| per-run callback latency | async observability |
| per-run `brokerCommittedCatchup` | 排除 lag catch-up 假象 |
| environment block | 记录 topic、records、partition、workers、commitInterval |

判定规则：

```text
if any correctness gate fails:
    result = invalid
elif abs(delta) <= max(3%, 3*MAD%):
    result = neutral
elif delta > max(3%, 3*MAD%):
    result = improvement
else:
    result = regression
```

### Stable Runner 伪代码

```text
variants = [
  {name: "async", env: {}},
  {name: "sync", env: {"TIDE_KAFKA_V2_FORCE_SYNC_PERIODIC_COMMIT": "1"}},
]

order = ["warmup", "async", "sync", "sync", "async", "async", "sync", "sync", "async", "async", "sync"]

produce(topic, records, partitions)

for item in order:
    group = "group-" + item + "-" + timestamp()
    report = run_e2e(topic, group, item.env)
    if item == "warmup":
        continue
    validate(report)
    collect(report)

aggregate_by_variant()
print_markdown_summary()
```

不要把 eBPF 放进这个 runner。eBPF 使用单独流程：

```text
trace_runner
  use same binary/topic shape
  lower record count if needed
  attach only target probes
  report histograms
  never compare traced QPS with untraced QPS
```

### 推荐 A/B 流程

```text
+----------------------------------------------------+
| Step 0: build once                                  |
| bash dev/test_run.sh kafka_v2_test                  |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| Step 1: produce once                                |
| fixed topic, fixed partition count                  |
| use parallel producer to avoid partition skew       |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| Step 2: warmup                                      |
| run one fresh group, discard result                 |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| Step 3: paired A/B                                  |
| A1 B1 B2 A2 A3 B3 ...                               |
| same topic, fresh group per run                     |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| Step 4: aggregate                                   |
| median / MAD / min / max                            |
| commit callback failures / lag catchup              |
+----------------------------------------------------+
```

### 推荐命令模板

Produce once:

```bash
RUN_ID=stable-ab-$(date +%s)
TOPIC="tide-kafka-v2-${RUN_ID}"
printf '%s\n' "$TOPIC" > .dbg/stable-ab-topic.txt

KAFKA_E2E_RUN_ID="${RUN_ID}-produce" \
KAFKA_E2E_LARGE_SCALE_TOPIC="$TOPIC" \
KAFKA_E2E_LARGE_SCALE_SKIP_PRODUCE=0 \
KAFKA_E2E_LARGE_SCALE_COUNT=100000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=100000000 \
KAFKA_E2E_BULK_CHUNK_SIZE=50000 \
KAFKA_E2E_BULK_PARALLELISM=16 \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=1024 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

Run one variant:

```bash
TOPIC=$(cat .dbg/stable-ab-topic.txt)
VARIANT=async
RUN_ID="stable-${VARIANT}-$(date +%s)"
REPORT_PATH=".dbg/billion-e2e-${RUN_ID}.json"

KAFKA_E2E_RUN_ID="$RUN_ID" \
KAFKA_E2E_LARGE_SCALE_TOPIC="$TOPIC" \
KAFKA_E2E_LARGE_SCALE_SKIP_PRODUCE=1 \
KAFKA_E2E_LARGE_SCALE_COUNT=100000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=100000000 \
KAFKA_E2E_LARGE_SCALE_REPORT_PATH="$REPORT_PATH" \
KAFKA_E2E_LARGE_SCALE_TEST_TIMEOUT_SEC=1800 \
TIDE_KAFKA_V2_E2E_GROUP_ID="group-${RUN_ID}" \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_E2E_COMMIT_INTERVAL_MS=1000 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=1024 \
TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

Sync-forced variant 只额外加：

```bash
TIDE_KAFKA_V2_FORCE_SYNC_PERIODIC_COMMIT=1
```

### 聚合脚本

```bash
python3 - <<'PY'
import json
import statistics
import sys

rows = []
for path in sys.argv[1:]:
    with open(path) as f:
        data = json.load(f)
    rows.append({
        "path": path,
        "variant": "sync" if "sync" in path else "async",
        "qps": float(data["ackedMsgsPerSec"]),
        "duration": int(data["durationMs"]),
        "commits": int(data.get("totalPeriodicCommitCalls", 0)),
        "failures": int(data.get("totalCommitFailures", 0)),
        "callbacks": int(data.get("totalCommitCallbacks", 0)),
        "callbackFailures": int(data.get("totalCommitCallbackFailures", 0)),
        "catchup": bool(data.get("brokerCommittedCatchup", False)),
    })

for variant in sorted({row["variant"] for row in rows}):
    values = [row["qps"] for row in rows if row["variant"] == variant]
    median = statistics.median(values)
    mad = statistics.median([abs(value - median) for value in values])
    print(variant, "n=", len(values), "median=", int(median), "mad=", int(mad),
          "min=", int(min(values)), "max=", int(max(values)))

bad = [row for row in rows if row["failures"] or row["callbackFailures"] or row["catchup"]]
if bad:
    print("invalid_or_suspicious_rows:")
    for row in bad:
        print(row)
PY .dbg/billion-e2e-stable-*.json
```

### 本轮结果如何解读

本轮已有数据仍然有价值，但只适合作为“没有明显负收益”的 smoke 结论：

| 测试 | 结论边界 |
|---|---|
| `30M / commitIntervalMs=5000` | commit 次数只有 `4`，不能证明吞吐收益 |
| `30M / commitIntervalMs=100` | commit 次数足够放大，但窗口仍只有约 `18s` |
| eBPF run | 能证明 callback lock 不是瓶颈，不能用于吞吐数字 |
| no-eBPF 100ms A/B | async/sync 差异 `0.03%`，只能说明没有明显负收益 |

后续如果要证明 async commit “提升吞吐”，必须跑稳定 A/B：

```text
same topic
fresh group per run
>= 100M records or >= 60s duration
>= 5 runs per variant
median delta > max(3%, 3*MAD%)
```

## 2026-05-21 Stable Runner Implementation

为了避免每次手写 A/B 命令，把稳定 commit A/B 固化为脚本：

```bash
dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

相关 skill 文档：

```text
docs/skills/consumer_v2_stable_e2e_ab.md
```

runner 默认策略：

```text
warmup,async,sync,sync,async,async,sync,sync,async,async,sync
```

本轮为了快速验证脚本和最终 commit callback 无锁实现，使用较短 order：

```bash
KAFKA_E2E_STABLE_RUN_ID=final-stable-commit-1779367941 \
KAFKA_E2E_STABLE_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_STABLE_SKIP_PRODUCE=1 \
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_PARTITION_COUNT=125 \
KAFKA_E2E_STABLE_WORKER_COUNT=48 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_ORDER=warmup,async,sync,sync,async \
bash dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

输出：

```text
.dbg/stable-commit-ab-final-stable-commit-1779367941/summary.json
```

结果：

| Variant | Runs | Median Msg/s | MAD | Min | Max | Commit Failures | Callback Failures |
|---|---:|---:|---:|---:|---:|---:|---:|
| async | `2` | `1,619,390` | `570` | `1,618,820` | `1,619,960` | `0` | `0` |
| sync forced | `2` | `1,616,995` | `2,965` | `1,614,030` | `1,619,960` | `0` | `0` |

判定：

```text
deltaPct = +0.148%
thresholdPct = 3.000%
result = neutral
```

解释：

- 这轮是快速 paired A/B，目的是验证 runner、report correctness gate、最终 callback 无锁实现没有明显回退。
- `n=2` 不满足完整稳定结论要求，不能宣称 async 吞吐提升。
- async/sync 都完整 ack `30,000,000`，commit failure 和 callback failure 都为 `0`。
- 结论仍是：async periodic commit 对本地 Redpanda 平均吞吐是 neutral，但它避免了 periodic commit 在 owner 上等待 broker/coordinator reply。

后续正式性能结论仍要跑默认稳定 order 或更长窗口：

```text
KAFKA_E2E_STABLE_COUNT=100000000
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=1000
KAFKA_E2E_STABLE_ORDER=warmup,async,sync,sync,async,async,sync,sync,async,async,sync
```

## 2026-05-22 C-Level Consume Batch

第一性原理判断：

- 旧 C++ `KafkaConsumer::consume()` 每条消息都会进入 `rd_kafka_consumer_poll()`，再构造一个 `RdKafka::MessageImpl`。
- 仅把多次 `consume(0)` 包成 helper 不是根本 batch 化，仍然保留 per-message poll / wrapper 成本。
- 真正 batch 化应从 librdkafka consumer queue 一次取多条原始 `rd_kafka_message_t*`。

实现路径：

```text
+----------------------------------------------------+
| Kafka owner thread                                  |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| rd_kafka_queue_get_consumer(consumer->c_ptr())      |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| rd_kafka_consume_batch_queue(queue, timeout, batch) |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| direct worker SPSC rings carry rd_kafka_message_t*  |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| worker ack -> rd_kafka_message_destroy()            |
+----------------------------------------------------+
```

关键约束：

- 只在 `directDispatchEnabled=true` 路径使用 C batch queue。
- `rd_kafka_queue_destroy()` 必须在 `consumer->close()` 前执行。
- worker 只持有 payload view，不拷贝 payload；message 在 ack 成功入队后释放。
- `c-single` 对照使用同一个 C API，但 `pollDrainBatchSize=1`，避免混入 C++ wrapper 差异。

快速 stable A/B：

```bash
KAFKA_E2E_STABLE_RUN_ID=c-batch-stable-1779420054 \
KAFKA_E2E_STABLE_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_STABLE_SKIP_PRODUCE=1 \
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_PARTITION_COUNT=125 \
KAFKA_E2E_STABLE_WORKER_COUNT=48 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_POLL_DRAIN_BATCH_SIZE=1024 \
KAFKA_E2E_STABLE_ORDER=warmup,c-batch,c-single,c-single,c-batch \
bash dev/kafka_e2e/run_consumer_v2_batch_stable_ab.sh
```

输出：

```text
.dbg/stable-c-batch-ab-c-batch-stable-1779420054/summary.json
```

结果：

| Variant | Runs | Median Msg/s | MAD | Typical Avg Poll Batch | Max Poll Batch | Commit Failures |
|---|---:|---:|---:|---:|---:|---:|
| c-batch | `2` | `1,616,470` | `2,090` | `~959` | `1024` | `0` |
| c-single | `2` | `193,174` | `4,778` | `~1.0` | `1` | `0` |

判定：

```text
deltaPct = +736.795%
thresholdPct = 7.420%
result = batch_improvement
```

解释：

- 这是快速 paired A/B，不替代完整 `100M` / 默认 order 稳定测试。
- 信号很强：`c-single` 把 worker read/dispatch batch 打碎到 `~1.6`，owner/worker 调度成本成为主瓶颈。
- `c-batch` 把 poll batch 恢复到接近 `1024`，吞吐回到 `~1.6M msg/s`。
- 后续正式结论仍要跑默认 order：`warmup,c-batch,c-single,c-single,c-batch,c-batch,c-single,c-single,c-batch,c-batch,c-single`。

## 2026-05-22 C-Level Batch Size Sweep

目标：

- 用 e2e sweep 选择 `rd_kafka_consume_batch_queue()` 的默认 batch size。
- 避免只用单次结果或理论值决定默认配置。
- 复用同一 topic，不重新 produce；每轮 fresh group。

新增 sweep runner：

```bash
dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

第一轮 quick sweep：

```bash
KAFKA_E2E_SWEEP_RUN_ID=batch-size-sweep-1779421704 \
KAFKA_E2E_SWEEP_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_SWEEP_SKIP_PRODUCE=1 \
KAFKA_E2E_SWEEP_COUNT=30000000 \
KAFKA_E2E_SWEEP_ACK_TARGET=30000000 \
KAFKA_E2E_SWEEP_PARTITION_COUNT=125 \
KAFKA_E2E_SWEEP_WORKER_COUNT=48 \
KAFKA_E2E_SWEEP_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_SWEEP_ORDER=warmup,256,512,1024,2048,4096,4096,2048,1024,512,256 \
bash dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

结果：

| Batch Size | Runs | Median Msg/s | MAD | Avg Poll Batch | Max Poll Batch |
|---:|---:|---:|---:|---:|---:|
| 256 | `2` | `1,602,965` | `9,765` | `251.31` | `256` |
| 512 | `2` | `1,610,825` | `1,295` | `495.37` | `512` |
| 1024 | `2` | `1,610,910` | `170` | `956.82` | `1024` |
| 2048 | `2` | `1,614,035` | `955` | `1,795.75` | `2048` |
| 4096 | `2` | `1,615,075` | `1,825` | `3,205.31` | `4096` |

第一轮最高 median 是 `4096`，但相对 `2048` 的 delta 只有 `0.064%`，未超过 `3%` 稳定阈值。

Focused sweep：

```bash
KAFKA_E2E_SWEEP_RUN_ID=batch-size-focus-1779421979 \
KAFKA_E2E_SWEEP_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_SWEEP_SKIP_PRODUCE=1 \
KAFKA_E2E_SWEEP_COUNT=30000000 \
KAFKA_E2E_SWEEP_ACK_TARGET=30000000 \
KAFKA_E2E_SWEEP_PARTITION_COUNT=125 \
KAFKA_E2E_SWEEP_WORKER_COUNT=48 \
KAFKA_E2E_SWEEP_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_SWEEP_ORDER=warmup,2048,4096,8192,8192,4096,2048 \
bash dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

结果：

| Batch Size | Runs | Median Msg/s | MAD | Avg Poll Batch | Max Poll Batch |
|---:|---:|---:|---:|---:|---:|
| 2048 | `2` | `1,623,865` | `1,715` | `1,772.22` | `2048` |
| 4096 | `2` | `1,611,600` | `14,500` | `3,159.95` | `4096` |
| 8192 | `2` | `1,614,040` | `3,820` | `5,296.66` | `8192` |

合并两轮有效结果：

| Batch Size | Runs | Median Msg/s | MAD | Avg Poll Batch | Max Poll Batch |
|---:|---:|---:|---:|---:|---:|
| 256 | `2` | `1,602,965` | `9,765` | `251.31` | `256` |
| 512 | `2` | `1,610,825` | `1,295` | `495.37` | `512` |
| 1024 | `2` | `1,610,910` | `170` | `956.82` | `1024` |
| 2048 | `4` | `1,618,570` | `4,535` | `1,783.99` | `2048` |
| 4096 | `4` | `1,615,075` | `6,425` | `3,182.62` | `4096` |
| 8192 | `2` | `1,614,040` | `3,820` | `5,296.66` | `8192` |

默认值决策：

- `2048` 合并 median 最高。
- `4096/8192` 没有显著超过 `2048`，且 owner 单轮处理的 record tile 更大，pause/resume 和 control-lane 响应风险更高。
- `1024 -> 2048` 有轻微正收益，同时保持 batch tile 不过大。
- 将生产默认、env fallback、e2e 默认统一升级为 `pollDrainBatchSize=2048`。

仍保留覆盖方式：

```bash
TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE=<size>
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=<size>
KAFKA_E2E_SWEEP_ORDER=warmup,512,1024,2048,4096,8192,...
```

## 2026-05-22 Direct-only Hot Path 标记规则

### 背景

本轮 direct-only 清理后，线程版 dispatch 已删除，owner consume 路径变成：

```text
rd_kafka_consume_batch_queue
  -> handleConsumedMessagesDirect
  -> per-worker SPSC ring
  -> worker readBatchDirect
  -> ackBatchDirect
  -> owner direct ack drain / commit progress
```

新的优化目标不是继续扩大锁范围，而是把主执行路径保持短、连续、可预测；冷分支必须显式留在 fast path 外侧。

### Code Attribute 规则

`likely/unlikely` 只能用于已经由 e2e/metrics 证明的稳定冷热分支，不能作为“感觉优化”。当前允许标记的分支：

- `unlikely`: shutdown / closed / missing worker / missing ring / invalid slot。
- `unlikely`: Kafka consume error、空 batch、timestamp end 过滤、无法选择 worker。
- `unlikely`: preferred worker ring full 后进入 drain wait / work stealing fallback。
- `unlikely`: ack 校验失败、revoked slot late ack、ack ring 不可用。

不允许标记的分支：

- 业务语义可能随配置改变的分支，例如不同 consume mode 的核心选择。
- 未经过 e2e 对比的局部微优化。
- 会让代码可读性明显下降的多层嵌套条件。

### 执行原则

```text
hot path keeps data moving
cold path handles safety and recovery
branch hint documents measured expectation
```

落地要求：

- 每次新增 `likely/unlikely` 必须能指向 e2e/metrics 证据。
- 如果后续 metrics 显示该分支不再稳定偏冷，应删除 hint，而不是继续叠加 hint。
- hint 只用于 branch layout，不替代数据结构优化；真正收益仍要以稳定 A/B e2e 判定。
- consume owner hot path 不应调用测试专用逻辑；测试 API 必须放在独立文件，避免污染主逻辑。

### Locked 命名约束

`Locked` 命名只允许表达“调用方已经持有 mutex”的冷控制路径。consume hot path 不使用 `Locked` 命名，避免和 lock-free direct dispatch 目标混淆。

当前约定：

- consume include 中的持锁控制判断使用 `UnderMutex` 后缀。
- direct SPSC read/ack/dispatch 热路径使用 direct/inline/owner 等语义命名。
- test-only 注入、探针、publish helper 放在 `shared_consumer_test_api.inc`。
