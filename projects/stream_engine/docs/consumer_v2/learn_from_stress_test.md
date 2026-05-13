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
- ready queue 和 partition state 的 index 化 / flat 化
- 更强的 partition batching / cohort dispatch

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

### 下一步结构方向

如果继续冲 5x，优先考虑下面这些能改变数据流形态的方案：

- 将 `ConsumedMessageDraft -> MsgSlotFill -> PublishedMessage -> DispatchState::pushRecord` 合并为单次 tile 构建，减少多 vector 和对象生命周期搬运
- 按 partition/cohort 构造 poll tile，让 topic/partition 映射、slot fill、dispatch enqueue 在同一 tile 内完成
- 将 `MsgSlotRing::fillBatch` 的 `liveOffsets` 维护改成面向连续 offset range 的结构，而不是每条消息插入 flat set
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
