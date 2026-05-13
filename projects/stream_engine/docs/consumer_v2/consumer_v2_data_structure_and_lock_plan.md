# Consumer V2 Optimization Roadmap

每一步改动是什么，收益是什么，一目了然。

---

## Optimization Timeline

### Step 0: Batch Dispatch (680a3a1d8)

**改动**: dispatch 从逐条改为批处理，`dispatchBatchSize=1024`。

**收益**: 建立批处理基础，后续所有优化的前置条件。

---

### Step 1: Ring Slot Leak Fix (2aa07db11)

**改动**: 修复 revoke 后 ring slot 泄漏。FIFO reclaim 的 head-of-line blocking 导致 revoked partition 的 slot 永远不释放，ring 打满后系统卡死。

**修复**: revoke 时立即清理所有相关 slots（in-flight + buffered），不再依赖 FIFO head 推进。

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| stress throughput | 13,435 msg/s → 0（卡死） | 38,982 msg/s（稳定） |
| ring 占用 | 262,144/262,144（100%） | peak 14,763（5.6%） |

---

### Step 2: Per-Partition Reclaim + Dispatch 热路径收缩 (8b937b278)

**改动**:
- `MsgSlotRing` 从 FIFO reclaim 改为 `freeIndexes_ + reclaimableIndexes_`，done slot 独立回收
- `dispatchableWorkers_` 避免每轮全量扫描 worker
- worker mailbox 改为轻量投递语义
- 动态 batch limit，高压场景允许 partial flush

| 指标 | Step 1 | Step 2 | 提升 |
|------|--------|--------|------|
| stress throughput | 38,982 msg/s | 162,209 msg/s | **+316%** |
| stress total ack (60s) | — | 9,732,566 | — |
| partitions consumed | — | 94/100 | — |
| avg batch size | — | 1,020.0 | — |

---

### Step 3: Fairness 1.0 — Deferral-Based (d4af35c66)

**改动**: 按 partition 被跳过的次数（deferral count）触发 partial dispatch，避免冷 partition 饿死。

**问题**: 触发条件太激进，batch 被打碎。

| 指标 | Step 2 | Step 3 | 变化 |
|------|--------|--------|------|
| stress throughput | 162,209 msg/s | 114,045 msg/s | **-30%** |
| avg batch size | 1,020.0 | **7.8** | **-99.2%** |
| partitions consumed | 94 | 79 | -16% |

**教训**: 公平性不是"越早放行越好"，粗暴的 deferral 计数摧毁了 batch 聚合能力。

---

### Step 4: Worker Index 化 + Mailbox 内联 (3be8cfdd9, 91c5febd4)

**改动**:
- `workerId` 从 string 改为 `size_t workerIndex`
- worker 相关数据结构从 `unordered_map<string, ...>` 改为 `vector<...>`
- mailbox dispatch 状态内联到 worker state

**收益**: 消灭 worker 热路径上的 string hash/compare/copy。为后续 SPSC mailbox 铺路。

（此步与 fairness 2.0 同批开发，bench 数据合并在 Step 5。）

---

### Step 5: Fairness 2.0 — Wait Time + Backlog (d73d6e8bc)

**改动**: 公平性触发条件从 deferral 计数改为：

```
等待时间 >= fairnessWaitMs  AND  全局 backlog >= fairnessBacklogThreshold
```

同时修复两个 flaky e2e 测试（snapshot / source adapter）。

| 指标 | Fairness 1.0 | Fairness 2.0 | 变化 |
|------|--------------|--------------|------|
| stress throughput | 114,045 msg/s | 166,172 msg/s | **+46%** |
| avg batch size | 7.8 | 834.5 | **+107x** |
| partitions consumed | 79 | 94 | +19% |
| duplicates / out-of-order | 0 / 0 | 0 / 0 | 保持 |

Bench 对比（partition scaling）:

| 场景 | F1.0 msg/s | F1.0 batch | F2.0 msg/s | F2.0 batch | 变化 |
|------|-----------|------------|-----------|------------|------|
| 100p / 8w | 341,643 | 24.3 | 367,193 | 226.8 | +7% / +9x |
| 200p / 16w | 57,732 | 1.0 | 326,669 | 103.1 | **+466%** / +103x |
| 400p / 32w | 43,925 | 1.0 | 335,671 | 55.0 | **+664%** / +55x |
| 1000p / 32w | 39,791 | 1.0 | 21,542 | 3.3 | -46% / +3x |
| 2000p / 32w | 34,912 | 1.0 | 15,957 | 2.2 | -54% / +2x |

**结论**: 200-400p 大幅恢复。1000p+ 仍然塌缩，瓶颈转向 ready queue 扫描和 partition state 查找。

---

### Step 6: Adaptive Dispatch for High Partition Count (a8dcc20e0)

**改动**:
- adaptive batch threshold：高 partition 数下自动降低满批门槛
- bounded ready queue scan：每轮 `tryDispatchNext` 最多扫描 128 个 partition
- adaptive coalesce interval：高 partition 数下更频繁触发 dispatch

**收益**: 消除 1000p+ 性能悬崖。

（bench 数据与 Step 7 合并，因为 Step 6/7 连续开发。）

---

### Step 7: Full Partition + Worker Index 化 (b85cd073e)

**改动**:
- 所有热路径从 `unordered_map<TopicPartition, ...>` 改为 `vector<PartitionState>`，通过 `uint32_t partitionIndex` 索引
- `partitionIndexMap_` 使用 `flat_map<TopicPartition, uint32_t>`，只在首次 assign 时查找
- `readyQueue_` 从 `deque<TopicPartition>` 改为 `deque<uint32_t>`
- `workerLeases_` 从 `unordered_map<string, uint64_t>` 改为 `vector<uint64_t>`

**收益**: 热路径不再有 string hash/compare/copy，全部变为 vector 随机访问。

（bench 数据合并在 Step 8。）

---

### Step 8: Ring Lock Split (ef308a340)

**改动**:
- `MsgSlotRing` 新增内部 `ringMutex_`，与全局 `mutex_` 完全解耦
- `ackBatch` 中 ring 的 `markDone/reclaim` 移出全局 `mutex_` 外
- `handleConsumedMessage` 中 ring 的 `acquireWait` 移出全局 `mutex_` 外
- `closed_` 改为 `std::atomic<bool>`，避免 lock 外读取的数据竞争
- 新增 `release()` 方法修复 publish 错误路径的 slot 泄漏

Partition scaling bench (8 workers, 4 publishers):

| Partitions | Msg/s | Avg Batch |
|-----------|-------|-----------|
| 10 | 293,000 | 647.7 |
| 50 | 357,000 | 392.1 |
| 100 | 411,000 | 253.8 |
| 500 | 408,000 | 53.7 |
| 1,000 | 392,000 | 26.8 |
| 2,000 | 390,000 | 14.5 |

Worker scaling bench (200 partitions, 4 publishers):

| Workers | Msg/s |
|---------|-------|
| 1 | 442,000 |
| 4 | 423,000 |
| 8 | 395,000 |
| 16 | 395,000 |
| 32 | 365,000 |

Production target simulation:

| 场景 | Msg/s | 达标率 (vs 6K/p) |
|------|-------|-------------------|
| 100p / 8w | 388,000 | 64.7% |
| 200p / 16w | 349,000 | 29.1% |
| 1,000p / 32w | 327,000 | 5.5% |
| 2,000p / 32w | 335,000 | 2.8% |

**关键收益**: 10p 到 2000p 吞吐稳定在 293K-411K，**无性能悬崖**。

---

### Step 9: Per-Worker SPSC Mailbox (c5752e7c0)

**改动**:
- 新增 `WorkerMailbox` 结构体，每 worker 独立 mutex + 单槽信箱
- `readBatch` 快速路径：先从 per-worker mailbox pop（只取 mailbox lock），命中则跳过全局锁
- `consumeEnvelopeBrief`：快速路径命中时，短暂全局锁仅注册 `activeItems_`
- `cancelLease`：revoke 扫描 mailbox 时安全取消 worker 的悬挂 lease
- 修复 `source_adapter.cpp` `fillReadyMessages` 遗漏 `partitionIndex` 拷贝（flaky e2e root cause）

Partition scaling bench (8 workers, 4 publishers):

| Partitions | Step 8 Msg/s | Step 9 Msg/s | Step 8 Batch | Step 9 Batch | 变化 |
|-----------|-------------|-------------|-------------|-------------|------|
| 10 | 293,000 | 308,000 | 647.7 | 663.3 | +5% |
| 50 | 357,000 | 365,000 | 392.1 | 397.1 | +2% |
| 100 | 411,000 | 399,000 | 253.8 | 246.9 | -3% |
| 500 | 408,000 | 406,000 | 53.7 | 53.5 | -0.5% |
| 1,000 | 392,000 | 402,000 | 26.8 | 27.8 | **+3%** |
| 2,000 | 390,000 | 389,000 | 14.5 | 14.3 | ≈0% |

Production target simulation:

| 场景 | Step 8 Msg/s | Step 9 Msg/s | Step 9 Batch | 变化 |
|------|-------------|-------------|-------------|------|
| 100p / 8w | 388,000 | 410,000 | 254.9 | **+6%** |
| 200p / 16w | 349,000 | 404,000 | 136.2 | **+16%** |
| 400p / 32w | — | 355,000 | 64.2 | — |
| 1,000p / 32w | 327,000 | 351,000 | 27.4 | **+7%** |
| 2,000p / 32w | 335,000 | 330,000 | 14.0 | -1% |

**分析**: 低 partition 场景变化不大（快速路径命中率高但锁竞争本来不重，收益被 mailbox mutex 开销抵消）。200p/16w 提升最明显（+16%），因为 worker 多、竞争大时 mailbox 解耦价值最高。

---

## Summary: 全程演进

```text
Step   改动                         Stress msg/s    1000p bench    状态
─────  ─────────────────────────────  ──────────────  ─────────────  ──────
  0    batch dispatch 1024           (baseline)      —              done
  1    ring slot leak fix            38,982          —              done
  2    per-partition reclaim         162,209         —              done
  3    fairness 1.0 (deferral)      114,045 (-30%)  39,791         done
  4    worker index + mailbox inline (see Step 5)    —              done
  5    fairness 2.0 (wait+backlog)  166,172 (+46%)  21,542         done
  6    adaptive dispatch             (see Step 8)    —              done
  7    partition+worker index化      (see Step 8)    —              done
  8    ring lock split              —               392,000        done
  9    per-worker SPSC mailbox      —               402,000        done
```

```text
Stress throughput 演进:

13K ──┐ (FIFO bug 期)
      │
39K ──┤ Step 1: ring leak fix
      │
162K ─┤ Step 2: per-partition reclaim          ████████████████
      │
114K ─┤ Step 3: fairness 1.0 (回退)           ███████████
      │
166K ─┤ Step 5: fairness 2.0                   █████████████████
```

```text
1000p bench 演进:

40K ──┐ fairness 1.0 (batch=1.0)
      │
22K ──┤ fairness 2.0 (batch=3.3, 更严格调度)
      │
392K ─┤ Step 6-8: adaptive + index + lock split ████████████████████
      │
402K ─┤ Step 9: per-worker mailbox              █████████████████████
```

```text
1000p/32w production bench 演进:

 35K ──┐ Step 10: bitmap (48 线程争 1 mutex)
       │
196K ──┤ Step 11: lock-free fast path             █████████████████████████
```

```text
200p/16w production bench 演进:

 58K ──┐ fairness 1.0 (batch=1.0)
       │
327K ──┤ fairness 2.0
       │
349K ──┤ Step 8: ring lock split                ████████████████████
       │
404K ──┤ Step 9: per-worker mailbox (+16%)      █████████████████████████
       │
273K ──┤ Step 10+11: bitmap + lock-free         ██████████████████████
```

---

### Latest Real Billion E2E (2026-05-12)

**口径**: 真实 Kafka / 真实 `consumer_v2` runtime / 复用已有 `100 partitions`、`1,000,000,000` backlog topic。
采样窗口拉长到 `40.2s`（`targetAckCount=15,000,000`），避免之前 `2.3s` 短窗口误导。

| Scenario | Acked Msg/s | Acked During Perf | Duration Ms | Peak Reported Msg/s | 说明 |
|----------|-------------|-------------------|-------------|---------------------|------|
| 100p billion backlog real e2e | 372,785 | 15,000,111 | 40,238 | 412,592 | 最新真实 bench，`finalPausedPartitions=0`，`lastError=""` |

**结论**: 当前 latest real bench 口径下，`consumer_v2` 在 `100 partitions` billion backlog 场景可稳定跑到约 `373K msg/s`。

---

### Step 10: Free-Worker Bitmap (602b8c90f)

**改动**: 用 `uint64_t dispatchableWorkerBitmap_` + `__builtin_ctzll` 替换 `deque<size_t> dispatchableWorkers_`。
移除 `WorkerState::dispatchableQueued` 字段。新增 `tryDispatchForWorkerLocked` 为 readBatch 调用者优先 dispatch。
修复 scale in/out bug（`livePollThreadBitmap_` + `respawnPollThreadsLocked`）。

**收益**: 性能持平（与 deque 基线相当），代码更简洁，消除 double-enqueue 问题。

| Scenario | Step 9 (deque) | Step 10 (bitmap) | 变化 |
|----------|---------------|-----------------|------|
| 100p/8w | 242K | 230K | 持平 |
| 500p/8w | 210K | 204K | 持平 |
| 1000p/8w | 208K | 209K | 持平 |

---

### Step 11: Lock-Free Fast Path (df0723941)

**改动**: 从 `consumeEnvelopeBrief` 快速路径移除全局 mutex。

关键技术:
- `DispatchEnvelope` 新增 `topic`/`partition` 字段，dispatch 时预填充，快速路径不需访问 `dispatchState_`
- 全局 `activeItems_`（flat_map）替换为 `perWorkerActiveItems_`（per-worker vector），消除共享写
- `hasPendingRevokes_` 原子标志，仅 revoke 时（罕见）才走加锁路径

**收益**: 高线程场景（32w+16pub=48 线程）性能巨幅提升。

| Scenario | Step 10 | Step 11 | 提升 |
|----------|---------|---------|------|
| 100p/8w | 230K | 256K | +11% |
| 200p/16w | 250K | 273K | +9% |
| 400p/32w | 205K | 237K | **+16%** |
| 1000p/32w | 35K | 196K | **+460%** |
| 2000p/32w | 23K | 32K | +39% |

---

## Next: 待实施优化

### Phase A: ackBatch Lock Reduction

**改动**: 拆分 ackBatch 中的工作：ack + re-schedule 分离，或将 schedule 延迟到下次 readBatch。

**预期收益**: 减少 ackBatch 持锁时间。

### Phase B: Dispatch Shard Mutex

**改动**: `dispatchState_` 按 `partitionIndex % N` 分 shard，每 shard 独立锁。

**预期收益**: 高 partition 数下 dispatch 和 ack 并行度提升。

### Phase C: Ring Batch Allocation

**改动**: ring slot 批量分配，减少 `ringMutex_` 争抢频次。

**预期收益**: publish 路径 lock 频率降低 N 倍。

---

## 当前瓶颈分析

```text
                 +------------------------------+
poll/publish --->|                              |
readBatch   ---->|   SharedConsumerState        |----> dispatchState_  (已 index 化)
ackBatch    ---->|     global mutex_            |----> workerMailboxes_ (已 per-worker 化)
rebalance   ---->|                              |----> perWorkerActiveItems_ (已 per-worker 化)
snapshot    ---->|  (ring 已拆出)               |
                 +------------------------------+

已解决:
  ✓ ring 操作不再持全局锁 (Step 8)
  ✓ 热路径不再 hash string (Step 4, 7)
  ✓ 高 partition 不再性能悬崖 (Step 6)
  ✓ readBatch 快速路径不进全局锁 (Step 9)
  ✓ readBatch 快速路径完全 lock-free (Step 11)

剩余瓶颈:
  → ackBatch 的 dispatch state 更新 + schedule 仍需全局锁
  → handleConsumedMessage 每 N 条 record 需全局锁做 schedule
  → ring slot allocation 每条 record 需 ringMutex_
  → dispatch 扫描所有 worker 而非仅 free worker
```
