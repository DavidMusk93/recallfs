# consumer_v2 残余 Lag 根因分析与改进计划

> pause 频繁翻转修复上线后，lag 仍在缓慢持续增长。
> 本文通过 ebpf + Unix Domain Socket metrics 采样 + perf + 静态代码 review 定位瓶颈，给出量化数据与分阶段改进方案。

---

## 0. 现象与口径

| 指标 | 值 | 说明 |
|---|---|---|
| PID | 1401214 | tide_worker, port=6511 |
| UDS | /var/run/tide/worker_6510.sock | metrics 入口 |
| Kafka topic | dwd_tlb_observe_otel_tlb_span_fix_sample_detail_hi | 125 partitions |
| Consumer group | tlb_mirror_large | cluster=bmq_data_sys |
| Workers | 48 | slot 线程 |
| Poll handles | 2 | librdkafka consumer 实例 |
| Ring capacity | 262144 | MsgSlotRing |

### Lag 定义对齐

```
brokerLag      = highWatermark - brokerCommittedOffset   (Kafka 侧可见)
ackedLag       = highWatermark - ackedOffset             (消费侧已确认但未 commit)
commitGap      = ackedOffset   - brokerCommittedOffset   (acked 到 commit 之间的距离)
brokerLag      = ackedLag + commitGap
```

---

## 1. 60s 窗口 Metrics 分析

### 1.1 吞吐量对比

```
Kafka produce rate (estimate): ~754,000 rec/s
Consumer poll rate:            ~203,000 rec/s
Consumer ack rate:             ~203,000 rec/s
Deficit:                       ~551,000 rec/s (73%)
```

**核心矛盾：Kafka 生产端写入速率是消费端的 3.7 倍，消费能力严重不足。**

### 1.2 Lag 趋势 (7 次采样, 10s 间隔)

```
totalBrokerLag 增长:   +34,729,679 (550,854/s)
totalAckedLag  增长:   +18,014,502 (285,731/s)
totalCommitGap 增长:   +16,715,177 (265,123/s)
```

- 125 个 partition 中，116 个 brokerLag 在增长，仅 3 个在下降。
- brokerLag 分布：p50=10.4M, p90=10.5M, max=10.6M —— 几乎均匀增长，无明显倾斜。

### 1.3 资源利用率

```
Ring utilization:       8.5% (22,400/262,144)
Paused partitions:      5/125 (4%)
Active leases:          4/48 workers
Ready partitions:       0
Worker queue depth:     3
polledToProductionRatio: 0.43
```

**Ring 和 Worker 都大量空闲**，瓶颈不在下游处理能力，而在上游 poll 链路。

### 1.4 Commit 频率

```
commitSync calls:      0.38/s (约 2.6s 间隔)
commitSync latency:    8-32ms per call (bpftrace)
commitSync per call:   ~500K offsets (125 partitions)
```

Commit 频率正常，延迟可接受，不是主要瓶颈。

---

## 2. eBPF / perf 分析

### 2.1 函数调用频次 (5s 窗口)

```
updatePauseState:    404,940  (80,988/s)  — per-record 判断
scheduleDispatch:      5,802  ( 1,160/s)
tryDispatch:           1,799  (   360/s)
ackBatch:                483  (    97/s)
dsAck:                   498  (   100/s)
syncPauseLocked:         156  (    31/s)
commitOffsetsLocked:       0  (5s 内无 commit)
pollLoop:                  0  (长驻循环不返回)
```

### 2.2 关键函数延迟直方图

**ackBatch (us)**
```
[64, 128)      79    ━━━━━━
[128, 256)    476    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[256, 512)    661    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← peak
[512, 1K)     252    ━━━━━━━━━━━━━━━━━━━
[1K, 2K)      190    ━━━━━━━━━━━━━━
[2K, 4K)      125    ━━━━━━━━━
[4K, 8K)       69    ━━━━━
[8K, 16K)      19    ━
[16K,32K)       4
[32K,64K)       6
```

**syncPartitionPauseLocked (us)**
```
[16, 32)      221    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[32, 64)       88    ━━━━━━━━━━━━━━━
[1K, 2K)      303    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← 双峰
```
syncPause 呈双峰分布：快路径 16-64us（无 Kafka 调用），慢路径 1-2ms（实际调 Kafka pause/resume）。

**tryDispatchForWorkerLocked (us)**
```
[2, 8)       3219    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← 快路径
[32, 128)    1290    ━━━━━━━━━━━━━━━━━━━━━━━━━                              ← 有 dispatch
[128, 512)    282    ━━━━━
```

### 2.3 Futex 等待直方图 (进程级, 10s)

```
[0]           71071    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[1]           33989    ━━━━━━━━━━━━━━━━━━━━━━━━
[4K, 16K)     26328    ━━━━━━━━━━━━━━━━━━                ← 长等待（worker 空等）
[16K, 64K)    12561    ━━━━━━━━                           ← 更长等待
[512K, 1M)     4019    ━━━                                ← 极长休眠
```
大量 worker 在 futex 上长时间休眠（4-64ms），说明 dispatch 频率太低，worker 得不到数据。

### 2.4 perf CPU 分布

```
45.8%   rdk:broker (ZSTD 解压 + 网络)
22.5%   slot (worker JSON 解析)
18.3%   writer (LZ4 压缩 + 写出)
 3.2%   jemalloc
 2.3%   kcv2 (poll 线程)
 0.4%   rdk:main
```

**poll 线程只占 2.3% CPU**，严重低利用，说明 poll 频率受限而非 CPU bound。

---

## 2.5 commitGap 周期性清零与快速回升

在 60s 采样窗口中，commitGap 呈现明显的 **锯齿波** 模式：

```
snap  totalCommitGap   totalBrokerLag   totalAckedLag   commitCalls
  1      498,047,066    1,170,742,345     672,695,279       924
  2      504,208,418    1,180,126,106     675,917,688       928
  3      505,399,555    1,182,964,088     677,564,533       932
  4           22,656      680,763,385     680,740,729       936  ← 清零!
  5          113,280      684,206,395     684,093,115       940
  6      508,860,122    1,196,096,423     687,236,301       944  ← 回升!
  7      514,762,243    1,205,472,024     690,709,781       948
```

### 这不是 bug，是 `committed()` 查询延迟导致的观测假象

**完整因果链：**

```
  commitGap = ackedOffset - brokerCommittedOffset

  brokerCommittedOffset 来自 ─→ entry.first->committed(partitions, 100ms)
                                 │
                                 └── 向 Kafka broker 发 OffsetFetch 请求
                                     broker 返回 __consumer_offsets 中记录的值
                                     超时 100ms 或 broker 繁忙时返回 ERR
```

关键代码路径 (`unified_consumer.cpp:1420-1467`):

```cpp
// 1. committed() 向 broker 查询已提交 offset (timeout=100ms)
auto commitError = entry.first->committed(partitions, kCommittedOffsetTimeoutMs);

// 2. 只有查询成功 && partition 无错误时才采信 broker offset
if (commitError == RdKafka::ERR_NO_ERROR &&
    item.partition->err() == RdKafka::ERR_NO_ERROR &&
    item.partition->offset() != RdKafka::Topic::OFFSET_INVALID) {
    lagInfo.brokerCommittedOffset = item.partition->offset();
}
// 否则 brokerCommittedOffset 保持默认值 -1

// 3. offsetGap 在 brokerCommittedOffset=-1 时返回 -1（被跳过不累加）
```

**周期规律解释：**

| 阶段 | committed() 返回 | brokerCommittedOffset | commitGap | 表现 |
|---|---|---|---|---|
| commitSync 刚完成 | broker 已更新 | 接近 ackedOffset | ≈ 0 | **清零** |
| commit 后 5-10s | broker 缓存过期或超时 | -1 (查询失败) 或旧值 | 大值 | **回升** |
| 下次 commitSync | broker 更新 | 最新值 | ≈ 0 | **再次清零** |

**验证证据：**
- `commitSync` 每 ~2.6s 调用一次（0.38/s），latency 8-32ms
- 但 `committed()` 查询有独立的 broker 往返，timeout 仅 100ms
- 当 2 个 handle 同时查 125 个 partition 时，部分 partition 查询超时
- 超时时 `commitError != NO_ERROR`，导致 brokerCommittedOffset 留为 -1
- `offsetGap(-1, ackedOffset)` 返回 -1，该 partition 的 commitGap 不参与求和
- 如果恰好大部分 partition 查询成功且刚好在 commitSync 之后，commitGap ≈ 0
- 如果大部分 partition 查询返回旧的 brokerCommittedOffset（上次 commit 前的值），commitGap 很大

**不是 bug 但确实有优化空间：**
1. `kCommittedOffsetTimeoutMs=100` 太短 — 125 partition × 2 handle 的 OffsetFetch 在高负载下容易超时
2. 查询失败时 commitGap 被静默跳过 — 导致观测值跳变，不利于告警
3. 可以缓存上次成功的 brokerCommittedOffset，查询失败时复用缓存值，避免锯齿

---

## 3. 根因分析

### 核心瓶颈链

```
                     ┌──────────────────────────────┐
                     │   Kafka produce: ~754K rec/s  │
                     └──────────┬───────────────────┘
                                │
                   ┌────────────▼────────────────┐
                   │  2 poll handles (librdkafka) │
                   │  consume → handleConsumed    │
                   │        poll rate: ~203K/s    │ ← BOTTLENECK
                   └────────────┬────────────────┘
                                │ mutex_ lock
              ┌─────────────────▼─────────────────┐
              │  handleConsumedMessages (locked)   │
              │  - pushRecord × N                  │
              │  - consumePendingPauseFlip         │
              │  - syncPartitionPauseLocked (1ms)  │
              │  - scheduleDispatchLocked           │
              │  - applyAutoScaleDecision           │
              └─────────────────┬─────────────────┘
                                │ workerCv_.notify
              ┌─────────────────▼─────────────────┐
              │  48 workers (仅 4 active leases)   │
              │  readBatch → process → ackBatch    │
              │  ring 8.5%, worker idle >90%        │
              └─────────────────┬─────────────────┘
                                │ mutex_ lock (ack)
              ┌─────────────────▼─────────────────┐
              │  ackBatch (locked)                  │
              │  - dispatchState_.ack              │
              │  - tryDispatchForWorkerLocked       │
              │  - syncPartitionPauseLocked         │
              │  - enqueueDispatchableWorker        │
              └────────────────────────────────────┘
```

### 根因总结

| # | 根因 | 影响度 | 证据 |
|---|---|---|---|
| **R1** | **Poll 线程消费速率不足** | 极高 | poll rate 203K vs produce 754K，deficit 73% |
| **R2** | **handleConsumedMessages 持锁批量操作** | 高 | 80K/s updatePauseState + scheduleDispatch 全在 mutex_ 下 |
| **R3** | **syncPartitionPauseLocked 在锁内调用 Kafka** | 中 | 双峰延迟，慢路径 1-2ms 阻塞全局锁 |
| **R4** | **Worker dispatch 效率低** | 中 | 48 workers 仅 4 active leases，ready=0，dispatch 仅 360/s |
| **R5** | **Poll handles 不足** | 中 | 仅 2 handles，auto_scale 已触发但被上限约束 |
| **R6** | **Commit 频率正常但 commitGap 持续增长** | 低 | commit 0.4/s，latency 8-32ms，commitGap 增长是 R1 的结果 |

---

## 3.1 Phase3 上线后 handler 吞吐下降复盘

> 采样对象：port=6511 对应 `PID=1872288`，UDS 为 `/var/run/tide/worker_6510.sock`。
> 结论：Phase3 当前实现不是最终无锁并行形态，而是把 poll ingress / ack 串到单 dispatcher owner，同时 `readBatch` slow path 仍允许 worker 抢全局调度锁；两者叠加导致 handler 被喂不满。

### 3.1.1 Metrics 现象

30s 窗口采样：

| 指标 | T0 | T+30s | delta/s | 说明 |
|---|---:|---:|---:|---|
| `totalPolledRecords` | 202,066,862 | 209,568,430 | 250,052/s | 实际 poll 速率 |
| `totalAckedRecords` | 201,955,295 | 209,468,330 | 250,434/s | ack 与 poll 基本一致 |
| `totalDispatchedBatches` | 236,674 | 245,228 | 285/s | batch dispatch 很低 |
| `totalDispatchedRecords` | 201,975,930 | 209,489,146 | 250,440/s | 约等于 ack，说明 worker 处理不是主要丢速点 |
| `totalPauseCalls` | 29,941 | 31,071 | 37.7/s | pause 已不是频繁翻转主因 |
| `totalResumeCalls` | 29,920 | 31,053 | 37.8/s | 与 pause 对称 |
| `totalCommitCalls` | 447 | 467 | 0.67/s | commit 正常 |
| `totalBrokerLag` | 2,373,838,573 | 2,466,171,593 | +3,077,767/s | lag 快速增长 |

运行中快照：

```text
handleCount:                 2
workerCount:                 48
assignedPartitionCount:      500
pausedPartitionCount:        17-21
activeLeaseCount:            7-9
readyPartitionCount:         0
workerQueueDepth:            13-18
totalBufferedRecordCount:    71K-91K
ringLiveCount:               90K-111K / 262K
currentThroughputMsgsPerSec: ~179K/s
productionRecordsPerSec:     0.9M-1.55M/s
ackedOffsetRecordsPerSec:    90K-133K/s
polledToProductionRatio:     0.05-0.19
```

关键含义：

- `activeLeaseCount=7-9/48`：48 个 worker 大部分空闲，handler 没被喂满。
- `readyPartitionCount=0`：调度器没有积压 ready partition，不是 ready queue 太长。
- `totalBufferedRecordCount=71K-91K` 分散到 `500` partitions，平均每 partition 约 `140-180` 条，远低于 `dispatchBatchSize=1024`。
- `dispatchedBatches≈285/s`，即使每批接近 1024 条，也只能支撑约 `292K/s`，与实际 `250K/s` 对齐。

### 3.1.2 perf 证据

20s `perf record -F 99 -g -p 1872288` 显示 CPU top 不集中在 dispatcher：

```text
5.07%  JSONStructuredDecoder::HandleParseObj
3.01%  LZ4_compress_fast_extState
2.92%  native_queued_spin_lock_slowpath
2.57%  __handle_mm_fault
2.33%  StringWriter::Write
1.10%  arrow::BaseBinaryBuilder<BinaryType>::Append
```

线程状态中，部分 `slot-*` worker、`kcv2p*` poll 线程、`rdk:broker*` 线程有 CPU；但 worker 活跃 lease 只有 7-9 个。说明问题不是 worker CPU 彻底算不动，而是调度层没有持续把 48 个 worker 填满。

### 3.1.3 eBPF 证据

5s bpftrace 采样：

```text
handleConsumedMessages: 17,082 calls / 5s
  latency: mostly 128-512us，少量 1-64ms

ackBatch: 2,550 calls / 5s
  latency: mostly 128-512us，少量 1-32ms

scheduleDispatchLocked: 23,580 calls / 5s
  latency: mostly 1-16us，本身不慢

tryDispatchForWorkerLocked: 5,229 calls / 5s
  latency: mostly 2-256us，本身不慢

readBatch: 5,755 calls / 5s
  latency: 3,138 calls in 64-128ms bucket
```

最关键的是 `readBatch`：

```cpp
// Fast path mailbox empty 后进入 slow path
std::unique_lock<std::mutex> lock(mutex_);
scheduleDispatchLocked(false);
...
workerCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasBatch);
...
tryDispatchForWorkerLocked(workerIndex, true);
scheduleDispatchLocked(true);
```

这意味着 Phase3 虽然把 poll ingress / ack command 移到了 dispatcher queue，但 **worker read 空转路径仍会直接抢 `mutex_` 并执行调度**。bpftrace 的 `@sched_by_tid` 也确认大量 `scheduleDispatchLocked` 来自 `slot-*` worker 线程，而不仅是 `kcv2-dispatch`。

### 3.1.4 Phase3 回归根因

```
                  ┌──────────────────────────────┐
                  │ poll threads                  │
                  │ enqueue ingress command       │
                  └──────────────┬───────────────┘
                                 │ lock-free queue
                  ┌──────────────▼───────────────┐
                  │ single dispatcher owner       │
                  │ applyIngress / applyAck       │
                  │ scheduleDispatchLocked        │
                  └──────────────┬───────────────┘
                                 │ per-worker mailbox
      ┌──────────────────────────▼──────────────────────────┐
      │ 48 workers                                           │
      │ mailbox empty -> readBatch slow path -> mutex_        │
      │ wait_for 64-128ms -> partial dispatch fallback        │
      └──────────────────────────────────────────────────────┘
```

| 问题 | 机制 | 结果 |
|---|---|---|
| dispatcher owner 单线程化 | poll ingress 和 ack 都串到一个 owner | 消除了部分锁竞争，但引入新的串行瓶颈 |
| `readBatch` slow path 未 Phase3 化 | worker mailbox 空时仍抢 `mutex_` 做 schedule/tryDispatch | 48 workers 空转时反复争抢调度锁 |
| 500 partitions 稀释 buffer | 平均每 partition 140-180 条，低于 `dispatchBatchSize=1024` | 大量 partition 只能靠 fairness/partial batch 出队 |
| partial dispatch 由 worker timeout 触发 | `readBatch` 等 64-128ms 后才 `allowPartialBatch=true` | handler 被动等待，active lease 只有 7-9 |
| dispatcher queue 无 backlog metrics | 无法区分 queue 堵塞、owner 忙、worker 空等 | 需要补 metrics 才能闭环 |

因此，Phase3 当前实现的主要问题不是 lock-free queue 本身，而是 **只做了 poll/ack ingress，未把 worker demand / partial dispatch / mailbox fill 统一交给 dispatcher owner 主动调度**。

### 3.1.5 修复方向

优先级从高到低：

1. **移除 `readBatch` slow path 的全局调度职责**：worker mailbox 空时只注册 demand / waiter，不直接调用 `scheduleDispatchLocked` 和 `tryDispatchForWorkerLocked`。
2. **dispatcher owner 主动填 mailbox**：dispatcher 在 ingress、ack、worker-demand 三类事件后，批量扫描空闲 worker 并派发 partial batch。
3. **增加 demand command**：`readBatch` fast path 失败后 enqueue `WorkerDemandCommand(workerIndex)`，由 dispatcher 处理；worker 只等待自己的 mailbox。
4. **partial batch 主动化**：当 `totalBufferedRecordCount` 高、active lease 低、worker demand 高时，dispatcher 不等 50ms fairness，直接用 `allowPartialBatch=true`。
5. **补 Phase3 metrics**：
   - `dispatcherQueueDepth`
   - `dispatcherIngressCommandsPerSec`
   - `dispatcherAckCommandsPerSec`
   - `dispatcherDemandCommandsPerSec`
   - `dispatcherLoopLatencyUs`
   - `workerReadTimeoutsPerSec`
   - `partialDispatchBatchesPerSec`
   - `mailboxEmptyReadPerSec`
6. **重新评估 P3.2 partition affinity**：当前 `assignedPartitionCount=500`，需要确认是否由 2 handle × metadata assign / restore 重复统计导致；若真实分配扩大到 500 partitions，会进一步稀释 per-partition buffer。

---

## 4. 改进方案

### Phase 1: 小步优化（已不作为主路径）

#### P1.1 增加 poll handle 数量
- 当前 `handleCount=2`, `maxHandleCount=2`
- 调整 `auto_scale_max_poll_thread_count` 到 4-6
- 每个 handle 独立 poll 线程，线性扩容 poll 带宽
- **结论**: 已实测无效，放弃该方案；瓶颈不是 handle 数量，而是 handle 之后的 dispatch 串行化
- **风险**: librdkafka 线程数增加 ~50 threads/handle

#### P1.2 减少 handleConsumedMessages 锁内工作量
- 将 `scheduleDispatchLocked` 拆分：锁内仅标记 dispatch 请求，锁外执行实际 dispatch
- `updatePauseState` 已经是 per-record O(1)，但 80K/s 在锁内的聚合开销不可忽略
- 考虑 batch coalesce：每 N 条记录才触发一次 schedule，而非 per-batch
- **预期收益**: 锁持有时间减少 30-50%

#### P1.3 syncPartitionPauseLocked 异步化
- 当前在 mutex_ 下直接调用 Kafka pause/resume API（慢路径 1-2ms）
- 改为：锁内仅置标志位，锁外由 poll 线程在下次 consume 前批量执行 pause/resume
- **预期收益**: 消除全局锁内的 Kafka I/O 阻塞

#### P1.4 committed() 查询稳定化（消除 commitGap 锯齿）
- `kCommittedOffsetTimeoutMs` 从 100ms 提升到 1000-5000ms（lag 查询本身 5s 一次，不敏感）
- 缓存上次成功的 `brokerCommittedOffset`，查询失败时复用缓存值
- lag snapshot 中记录 `lagQueryError` 计数，暴露在 metrics 中便于观测
- **预期收益**: commitGap/brokerLag 观测值平稳，不再锯齿跳变，便于告警

### Phase 2: Dispatch 效率提升 (预期 +50%)

#### P2.1 Batch dispatch 优化
- 当前 `dispatchBatchSize=1024`，但 active leases 仅 4/48
- 原因：`readyPartitionCount=0`，说明 partition 来不及填满到阈值就被 pause
- 降低 `scaleOutBacklogThreshold` 或采用 partial batch dispatch，让 worker 更早拿到数据
- **预期收益**: worker 利用率从 8% 提升到 30-50%

#### P2.2 Worker 唤醒策略优化
- 当前 `workerCv_.notify_one` 逐个唤醒，worker 大量 futex 等待 4-64ms
- 改为 batch notify：一次 dispatch 后 `notify_all` 或按 dispatchable worker 数量 notify
- **预期收益**: 减少 worker 空闲等待时间

### Phase 3: 架构级优化 (预期 +3-5x)

#### P3.1 Lock-free dispatch path
- 当前 poll/ack/dispatch 三条路径共用 `mutex_`
- 重构为：
  - poll → lock-free ingress command queue → dispatcher owner
  - dispatcher owner → per-worker mailbox
  - worker ack → lock-free ack command queue → dispatcher owner / commit queue
- **当前落地**: dispatcher command queue 已改为 `boost::lockfree::queue<DispatcherCommand*>`，poll ingress 与 worker ack 均不再通过 mutex/deque 入队
- **预期收益**: 消除 poll 线程与 worker ack/dispatch 在 `mutex_` 上的直接竞争

#### P3.2 Per-handle partition affinity
- 将 125 partitions 均匀分配到多个 handle
- 每个 handle 独立 poll + dispatch，减少跨 handle 竞争
- **当前落地**: runtime poll handle 启动时通过 Kafka metadata 枚举 partition，并按 `hash(topic, partition) % handleCount` 手动 `assign()` 自己负责的子集；metadata/assign 失败时才回退旧 `subscribe()`
- **预期收益**: 消除 handleConsumedMessages 的锁争用

---

## 5. 实施优先级与排期

```
┌──────────────┬───────────────┬────────────────────┬──────────────┐
│    Phase     │    改进项      │    预期提升          │   复杂度      │
├──────────────┼───────────────┼────────────────────┼──────────────┤
│ Phase 1.1    │ 增加 handles  │ 已实测无效/放弃    │ -            │
│ Phase 1.2    │ 锁内工作减少  │ 锁时间 -30-50%     │ 中           │
│ Phase 1.3    │ pause 异步化  │ 消除 1ms 阻塞      │ 中           │
│ Phase 1.4    │ committed()   │ 消除 commitGap     │ 低           │
│              │ 查询稳定化    │ 锯齿观测假象       │              │
│ Phase 2.1    │ batch dispatch│ worker +50%        │ 中           │
│ Phase 2.2    │ 批量唤醒      │ 减少 idle 等待      │ 低           │
│ Phase 3.1    │ lock-free     │ 3-5x               │ 高           │
│ Phase 3.2    │ 分区亲和      │ 消除锁争用          │ 高           │
└──────────────┴───────────────┴────────────────────┴──────────────┘
```

**推荐路径**: 直接进入 Phase 3，P1.1 放弃，P1/P2 仅作为 Phase3 后续补强项。

- P1.1 已实测无效，不再投入
- Phase 3 是最终方案：把 poll ingress、dispatch owner、ack/commit queue 解耦
- 当前提交完成 lock-free dispatcher command queue、poll ingress queue、worker ack queue、per-handle partition affinity

---

## 6. 验证 KPI

| 指标 | 当前值 | Phase 1 目标 | Phase 2 目标 |
|---|---|---|---|
| poll rate | 203K/s | 500K+/s | 600K+/s |
| polledToProductionRatio | 0.43 | > 0.8 | > 0.95 |
| brokerLag 趋势 | 持续增长 551K/s | 增长 < 100K/s | 稳定或下降 |
| worker 利用率 | 8.5% ring, 4/48 active | > 30% | > 50% |
| pausedPartitionCount | 5-16 | < 5 | ≈ 0 |
| commitGap p50 | 4.5M | < 500K | < 100K |

---

## 7. 风险与回滚

| 风险 | 缓解措施 |
|---|---|
| 增加 handle 数导致线程爆炸 | maxRdkThreadCount 硬限、监控线程数 |
| pause 异步化导致 OOM | 保留 ring capacity 硬限、紧急 pause 路径 |
| lock-free 重构引入 data race | 充分单测 + TSan + 压测 |
| 配置变更触发 rebalance | 灰度发布、观察 rebalance 回调计数 |

---

## 附录 A: 调试命令参考

```bash
# 确认进程
PID=$(lsof -i:6511 -sTCP:LISTEN -t)
cd /proc/$PID/cwd
BIN=$(readlink /proc/$PID/exe)
SOCK=/var/run/tide/worker_6510.sock

# Metrics 快照
curl --unix-socket $SOCK http://localhost/json | python3 -m json.tool

# bpftrace 函数计数 (5s)
bpftrace -e 'uprobe:'"$BIN"':_ZN4tide5kafka11consumer_v219SharedConsumerState8pollLoopEi /pid=='"$PID"'/ { @n=count(); } interval:s:5 { exit(); }'

# bpftrace commitSync 延迟
bpftrace -e '
uprobe:'"$BIN"':_ZN7RdKafka17KafkaConsumerImpl10commitSyncERSt6vectorIPNS_14TopicPartitionESaIS3_EE /pid=='"$PID"'/ { @t[tid]=nsecs; }
uretprobe:'"$BIN"':_ZN7RdKafka17KafkaConsumerImpl10commitSyncERSt6vectorIPNS_14TopicPartitionESaIS3_EE /pid=='"$PID"'/ { @us=hist((nsecs-@t[tid])/1000); delete(@t[tid]); }
interval:s:20 { exit(); }'

# perf CPU 采样
perf record -p $PID -g -F 99 -- sleep 5
perf report --stdio --max-stack=8 -n --no-children
```

---

## 附录 B: 原始采样数据

- Metrics 快照: `/tmp/lag_diag/60s/snap_{1..7}.json`
- perf 数据: `/proc/$PID/cwd/perf.data`
- 采样时间: 2026-05-15 15:10-15:12 UTC+8
