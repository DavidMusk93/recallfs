# Consumer V2 Disruptor Mechanism Improvement Plan

Date: 2026-05-22

## Goal

Absorb the useful parts of LMAX Disruptor into the current Consumer V2 all-SPSC direct path without
changing the core correctness model:

- one Kafka owner
- bounded SPSC lanes on hot edges
- one topic-partition owned by one worker at a time
- no direct hot-path mutex
- no message drop

The target is not to import Disruptor. The target is to make Disruptor-like sequence, gating, wait
strategy, and preallocated-slot ideas explicit in our C++ data path.

## Evidence Baseline

The current eBPF and metrics evidence showed:

- `kc-owner` futex pressure is real.
- A large part comes from librdkafka queue serving.
- Another important part comes from `LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop`.
- That wait is caused by sticky worker data ring backpressure, not ack event waiting.
- Three 60s windows showed queue depth can shrink and then regrow, so the analysis remains open.

Therefore the first implementation steps must improve observability and owner gating before changing
routing or memory ownership.

## Phase 0: Plan And Baseline Documents

Status: in progress.

Deliverables:

- Commit current analysis documents.
- Add this improvement plan.
- Push every commit immediately after local validation.

Validation:

```bash
git status --short
```

## Phase 1: Direct Lane Sequence Metrics

Purpose:

Expose Disruptor-style progress counters for direct data and ack lanes.

Add per-worker counters:

```text
directDataPublishedSeq[w]   owner increments after successful data push
directDataConsumedSeq[w]    worker increments after direct data pop/read
directAckPublishedSeq[w]    worker increments after successful ack enqueue
directAckConsumedSeq[w]     owner increments after ack drain
```

Derived values in JSON:

```text
directDataLag[w] = directDataPublishedSeq[w] - directDataConsumedSeq[w]
directAckLag[w]  = directAckPublishedSeq[w] - directAckConsumedSeq[w]
```

Constraints:

- No behavior change.
- No new mutex.
- Relaxed atomics are enough because these are observability/progress counters, not correctness
  gates in Phase 1.
- Keep arrays capped by `kDirectWorkerMetricCount`.

Validation:

```bash
bash dev/test_run.sh kafka_v2_test
```

Commit:

```text
consumer_v2: expose direct lane sequence metrics
```

## Phase 2: Direct Wait Metrics

Purpose:

Make owner and worker wait behavior explainable without relying only on eBPF futex stacks.

Add per-worker counters:

```text
directDrainWaitSpinCount[w]
directDrainWaitYieldCount[w]
directDrainWaitParkCount[w]
directDrainWaitTimeoutCount[w]
directDrainWaitParkUs[w]
directDataWaitParkCount[w]
directAckDrainWaitParkCount[w]
```

Constraints:

- No behavior change.
- Count only direct wait sites.
- JSON should expose arrays so 60s deltas can identify hot workers.

Validation:

```bash
bash dev/test_run.sh kafka_v2_test
```

Commit:

```text
consumer_v2: expose direct wait strategy metrics
```

## Phase 3: Owner Non-Blocking Full-Lane Gating

Purpose:

Replace immediate owner park-on-full with bounded non-blocking gating.

Current behavior:

```text
sticky worker ring full
  -> owner waits on directDrainSignals_[worker]
  -> retry same worker
```

Target behavior:

```text
sticky worker ring full
  -> keep current message or suffix in bounded owner pending state
  -> mark worker lane blocked for this owner turn
  -> drain direct acks
  -> run commit/control/lag refresh budgets
  -> retry pending before polling more records
  -> pause affected partitions if pending exceeds high watermark
```

Correctness gates:

- Pending buffer is bounded.
- Pending record keeps original `rd_kafka_message_t*` ownership.
- Same topic-partition order is preserved.
- Sticky worker is not migrated while queued or in-flight records exist.
- Shutdown destroys each pending message exactly once.

Validation:

```bash
bash dev/test_run.sh kafka_v2_test
```

Runtime validation:

```bash
timeout 60 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == OWNER_TID/ { @[args->op & 127, ustack(12)] = count(); }'
```

Success:

- `LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop` futex stack drops materially.
- `directDataLag[]` and `workerQueueDepth` do not grow faster than baseline.
- `totalAckedLag/s` is flat or lower across three 60s windows.

Commit:

```text
consumer_v2: avoid owner park on direct worker full
```

## Phase 4: Batch Capacity Check And Publish

Purpose:

Move from per-record full checks to per-worker bucket publish.

Target:

```text
poll batch
  -> choose worker per record
  -> bucket by worker
  -> check capacity by worker
  -> publish available prefix
  -> retain blocked suffix as pending
  -> notify worker once
```

Constraints:

- Preserve Kafka partition order.
- Do not split a partition suffix across workers.
- Keep direct SPSC ownership unchanged.

Validation:

```bash
bash dev/test_run.sh kafka_v2_test
```

Runtime success:

- failed push retries down
- owner futex waits down
- read batch size same or higher
- ack throughput same or higher

Commit:

```text
consumer_v2: batch direct worker publication
```

## Phase 5: Cache-Line-Isolated Direct Lane State

Purpose:

Apply Disruptor's false-sharing lesson to per-worker hot state.

Target:

```text
struct alignas(64) DirectWorkerCounters
struct alignas(64) DirectLaneSequence
```

Move hot per-worker counters away from adjacent worker counters when safe.

Validation:

```bash
bash dev/test_run.sh kafka_v2_test
```

Runtime success:

- no throughput regression
- no new hot-path mutex
- lower cache-line ping-pong if perf counters are available

Commit:

```text
consumer_v2: isolate direct worker hot counters
```

## Phase 6: E2E Verification

Run after Phases 1-5 or after each risky phase.

Minimum checks:

```bash
bash dev/test_run.sh kafka_v2_test
```

Live/e2e checks:

```text
3 x 60s metrics windows
3 x 60s eBPF owner futex attribution
3 x 60s CPU hot class profile
3 x 60s VFS write pressure if downstream remains hot
```

Success criteria:

- no regression in unit/integration tests
- owner direct-drain futex stack reduced
- no owner spin regression
- `directDataLag[]` and `directAckLag[]` explain queue behavior
- `totalAckedLag/s` does not worsen
- no ordering or commit correctness failure

## Phase 7: Lesson Document

Create a final document under `docs/lesson`.

Content:

- what was changed
- what Disruptor ideas helped
- what did not apply
- before/after metrics
- eBPF commands
- rollback criteria
- future work

Commit:

```text
docs: summarize consumer v2 disruptor lessons
```

## Commit And Push Discipline

For every phase:

```bash
git status --short
git add -A
git commit -m "<phase commit>"
git push origin kafka_consumer_v2
```

No phase should mix unrelated refactors. If validation fails, fix within the same phase before
committing, or explicitly document the failure and stop.
