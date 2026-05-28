# Consumer V2 Disruptor Mechanism Improvements

Date: 2026-05-22

## Background

The live eBPF investigation showed that `kc-owner` is still a single logical owner thread, but it can
generate high futex counts. The important stack for Consumer V2 direct dispatch was:

```text
LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop
```

This was not waiting for ack events. It was the owner waiting for a sticky worker data ring to drain.

LMAX Disruptor was useful as a design reference, not as a library dependency. The useful lessons were:

- make sequence progress explicit
- separate data capacity from ack/commit progress
- make wait strategy visible
- prefer gating/backpressure over producer park-on-full
- keep SPSC ownership and ordering rules simple

## What Changed

### 0. Final-State Boundary

The final implementation intentionally stops before a risky batch-publish rewrite.

Reason:

- `SpscLane::tryPushBatch()` is currently a loop over per-record `tryPush()`, not a true
  Disruptor-style range claim/publish.
- The 30M real e2e report showed `directDrainWaitParkCount[] == 0`, so owner full-lane Baton wait
  was already eliminated by pending gating in that workload.
- A synthetic "bucket then push" layer would add extra vectors and cache work without reducing the
  underlying SPSC write count.

Therefore:

| Phase | Decision |
|---|---|
| Phase 4 batch capacity/publish | Deferred until SPSC supports real range claim/publish |
| Phase 5 cache-line isolation | Implemented |

The final state keeps the direct path simple:

```text
Kafka owner
  -> per-record SPSC write
  -> one notification per touched worker
  -> owner pending queue instead of owner park-on-full
  -> padded per-worker counters/sequences
```

### 1. Direct Lane Sequence Metrics

Commit:

```text
ceb110361 consumer_v2: expose direct lane sequence metrics
```

Added per-worker counters:

| Metric | Writer | Meaning |
|---|---|---|
| `directDataPublishedSeq[w]` | Kafka owner | records successfully pushed to worker data ring |
| `directDataConsumedSeq[w]` | worker | records popped/read from worker data ring |
| `directAckPublishedSeq[w]` | worker | records represented by acks pushed to ack ring |
| `directAckConsumedSeq[w]` | Kafka owner | records represented by acks drained by owner |
| `directDataLag[w]` | snapshot | `published - consumed` |
| `directAckLag[w]` | snapshot | `ackPublished - ackConsumed` |

Why it matters:

- `directDataLag` identifies worker data-pop pressure.
- `directAckLag` identifies ack-ring or owner-drain pressure.
- eBPF futex stacks can now be explained by runtime counters.

### 2. Direct Wait Strategy Metrics

Commit:

```text
e9cb03860 consumer_v2: expose direct wait strategy metrics
```

Added per-worker wait-budget counters for three direct wait sites:

| Wait site | Metrics prefix | Thread direction |
|---|---|---|
| worker waits for data | `directDataWait*` | owner -> worker data ring |
| owner waits for worker drain | `directDrainWait*` | worker -> owner drain signal |
| worker waits for ack drain | `directAckDrainWait*` | owner -> worker ack-drain signal |

Each prefix exposes:

```text
SpinCount
YieldCount
ParkCount
TimeoutCount
ParkUs
```

Interpretation:

- `ParkCount` is an attempted wait-budget park count.
- `TimeoutCount` means `LaneSignal::wait()` returned false.
- These counters are not a replacement for eBPF; they make eBPF actionable by worker and wait site.

### 3. Owner Pending Gating Instead Of Park-On-Full

Commit:

```text
9412df654 consumer_v2: avoid owner park on direct worker full
```

Previous behavior:

```text
owner polls Kafka batch
  -> sticky worker ring full
  -> owner waits on directDrainSignals_[worker]
  -> futex appears in kc-owner
```

New behavior:

```text
owner polls Kafka batch
  -> sticky/preferred worker ring full
  -> owner drains direct acks once and retries
  -> if still full, current message plus remaining poll batch moves to owner pending queue
  -> poll loop retries pending before polling Kafka again
  -> if still pending, owner yields instead of Baton parking
```

Correctness constraints preserved:

- no message drop
- no partition reorder
- sticky partition is not migrated after binding
- pending messages keep original `rd_kafka_message_t*` ownership
- close destroys pending messages after poll thread exits

Added observability:

| Metric | Meaning |
|---|---|
| `directPendingMessageCount` | records retained by owner pending queue for retry |

### 4. Cache-Line-Isolated Direct Counters

Commit:

```text
pending at time of writing: consumer_v2: isolate direct worker hot counters
```

Changed direct per-worker hot counters and sequence metrics from adjacent `std::atomic<size_t>`
arrays to `alignas(64)` padded counter cells.

Covered fields:

```text
directWorkerPushedRecords_
directWorkerReadRecords_
directWorkerAckedRecords_
directWorkerInflightRecords_
directDataPublishedSeq_
directDataConsumedSeq_
directAckPublishedSeq_
directAckConsumedSeq_
directDataWait*
directDrainWait*
directAckDrainWait*
```

Why it matters:

- Different workers update adjacent per-worker counters concurrently.
- Adjacent atomics can share cache lines and cause false sharing.
- Padding follows the same mechanical sympathy lesson as Disruptor `Sequence` padding.

Implementation constraint:

- The wrapper keeps the existing `.load()`, `.store()`, `.fetch_add()`, and `.fetch_sub()` call
  style, so behavior stays unchanged.

## Why This Is Safer Than A Blind Try-Read Loop

Blind retry would risk hot spinning or polling more Kafka records while worker rings remain full.

The chosen design gates the owner:

- pending messages are retried before any new Kafka poll
- the owner still drains acks, commit progress, control lanes, and lag refresh before retry
- pending is bounded by the already-polled batch because the loop does not poll more while pending exists
- `sched_yield()` is used only when pending remains full after a retry turn

This is closer to Disruptor gating:

```text
capacity unavailable -> publish stops at the gate -> dependent sequence catches up -> retry
```

## Validation

### Passed

Command:

```bash
bash dev/test_run.sh kafka_v2_test
```

Result:

```text
162 tests passed
```

This command was run after each phase:

| Phase | Result |
|---|---|
| sequence metrics | passed |
| wait metrics | passed |
| pending gating | passed |

### Cross-Module Build Attempt

Command:

```bash
bash build_dev.sh
```

Result:

The script did not produce a valid full build signal because the workspace/build environment is
missing unrelated dependency targets:

| Missing item | Failing target |
|---|---|
| `//cpp3rdlib/mongocxx` | `//src/runtime/jobmanager:tide_jobmgr` |
| `//tide/librdkafka` | `//src/example:rdkafka++` |
| `//src/plugin/udf_plugins:udf_regex` | UDF plugin build |
| rocksdb branch resolution | example bundle dependency update |

Reading:

- This does not point to a compile error in the changed Consumer V2 files.
- The focused Consumer V2 target compiled and passed tests.

### Real E2E Result

After adding the report fields for direct pending, sequence lag, and wait counters, a real Kafka e2e
run was executed with the same discipline described in
`docs/consumer_v2/learn_from_stress_test.md`.

Command shape:

```bash
KAFKA_E2E_LARGE_SCALE_COUNT=30000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=30000000 \
KAFKA_E2E_BULK_PARALLELISM=16 \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_E2E_COMMIT_INTERVAL_MS=1000 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=2048 \
TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE=2048 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

Report:

```text
.dbg/billion-e2e-disruptor-pending-e2e-1779460335.json
```

Result:

| Metric | Value |
|---|---:|
| `ackedMsgsPerSec` | `1,703,480` |
| `durationMs` | `17,611` |
| `ackedDuringPerf` | `30,000,000` |
| `avgPollBatchRecords` | `1,807.99` |
| `maxPollBatchRecords` | `2,048` |
| `avgReadBatchSize` | `1,012.66` |
| `avgDispatchBatchSize` | `1,012.66` |
| `avgHandleConsumedLatencyUs` | `226.026` |
| `directPendingMessageCount` | `0` |
| `ringLiveCount` | `0` |
| `totalCommitFailures` | `0` |
| `totalCommitCallbackFailures` | `0` |

Direct SPSC evidence:

| Field | Sum | Max | Non-zero Workers |
|---|---:|---:|---:|
| `directDataLag[]` | `0` | `0` | `0` |
| `directAckLag[]` | `0` | `0` | `0` |
| `directDrainWaitParkCount[]` | `0` | `0` | `0` |
| `directDrainWaitTimeoutCount[]` | `0` | `0` | `0` |
| `directDataWaitParkCount[]` | `789,166` | `16,594` | `48` |
| `directDataWaitTimeoutCount[]` | `788,220` | `16,574` | `48` |
| `directAckDrainWaitParkCount[]` | `0` | `0` | `0` |
| `directAckDrainWaitTimeoutCount[]` | `0` | `0` | `0` |

Interpretation:

- Owner-side full-lane drain wait did not occur in this e2e window:
  `directDrainWaitParkCount[] == 0`.
- The pending queue did not accumulate by the end of the run:
  `directPendingMessageCount == 0`.
- Data and ack sequence lags were fully drained:
  `directDataLag[] == 0` and `directAckLag[] == 0`.
- All `48` workers were active in this produced topic:
  `directWorkerAckedRecords[]` had `48` non-zero workers.
- Worker-side data waits are expected after the backlog is drained; they are idle waits, not owner
  backpressure.

Conclusion:

```text
result = e2e_pass
reason = full 30M ack, no commit failure, no pending residue, no owner direct-drain park
```

This is still a single e2e run. It is a valid correctness and smoke performance signal, not a final
stable A/B performance conclusion. If judging throughput improvement, follow the paired stable-runner
method from `docs/consumer_v2/learn_from_stress_test.md`.

### Final Throughput Run

After Phase 5 cache-line-isolated direct counters, a longer final throughput run was executed.

Command shape:

```bash
KAFKA_E2E_LARGE_SCALE_COUNT=100000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=100000000 \
KAFKA_E2E_BULK_PARALLELISM=16 \
TIDE_KAFKA_V2_E2E_WORKER_COUNT=48 \
TIDE_KAFKA_V2_E2E_COMMIT_INTERVAL_MS=1000 \
TIDE_KAFKA_V2_E2E_POLL_DRAIN_BATCH_SIZE=2048 \
TIDE_KAFKA_V2_POLL_DRAIN_BATCH_SIZE=2048 \
TIDE_KAFKA_V2_LARGE_SCALE_REQUIRE_BACKPRESSURE=0 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

Report:

```text
.dbg/billion-e2e-disruptor-final-throughput-1779461079.json
```

Result:

| Metric | Value |
|---|---:|
| `ackedMsgsPerSec` | `2,048,050` |
| `durationMs` | `48,827` |
| `ackedDuringPerf` | `100,000,000` |
| `avgPollBatchRecords` | `1,878.85` |
| `maxPollBatchRecords` | `2,048` |
| `avgReadBatchSize` | `1,019.40` |
| `avgDispatchBatchSize` | `1,019.40` |
| `avgHandleConsumedLatencyUs` | `203.521` |
| `avgPollConsumeLatencyUs` | `736.021` |
| `totalPeriodicCommitCalls` | `46` |
| `totalCommitFailures` | `0` |
| `totalCommitCallbacks` | `46` |
| `totalCommitCallbackFailures` | `0` |
| `directPendingMessageCount` | `0` |
| `ringLiveCount` | `0` |

Direct SPSC evidence:

| Field | Sum | Max | Non-zero Workers |
|---|---:|---:|---:|
| `directDataLag[]` | `0` | `0` | `0` |
| `directAckLag[]` | `0` | `0` | `0` |
| `directDrainWaitParkCount[]` | `0` | `0` | `0` |
| `directDrainWaitTimeoutCount[]` | `0` | `0` | `0` |
| `directDataWaitParkCount[]` | `2,183,394` | `45,871` | `48` |
| `directDataWaitTimeoutCount[]` | `2,181,479` | `45,757` | `48` |
| `directAckDrainWaitParkCount[]` | `0` | `0` | `0` |
| `directAckDrainWaitTimeoutCount[]` | `0` | `0` | `0` |
| `directWorkerAckedRecords[]` | `100,000,000` | `2,400,000` | `48` |

Final reading:

- The final-state direct SPSC path consumed `100M` records at `2.048M msg/s`.
- Owner direct-drain wait stayed eliminated: `directDrainWaitParkCount[] == 0`.
- Pending gating did not leave residue: `directPendingMessageCount == 0`.
- Sequence accounting closed cleanly: `directDataLag[] == 0` and `directAckLag[] == 0`.
- All workers were active for this produced topic shape.
- Worker data waits are idle/wait-for-data events after batches drain; they are not owner
  backpressure.

Final result:

```text
result = final_e2e_pass
throughput = 2.048M msg/s
correctness = pass
owner_full_lane_wait = eliminated in this run
pending_residue = none
```

## Runtime Verification After Deployment

Use at least three consecutive 60s windows.

Metrics fields to compare:

```text
totalPolledRecords/s
totalReadRecords/s
totalAckedRecords/s
totalAckedLag/s
workerQueueDepth
ringLiveCount
directPendingMessageCount
directDataLag[]
directAckLag[]
directDrainWaitParkCount[]
directDrainWaitTimeoutCount[]
```

Expected direction:

| Metric | Expected |
|---|---|
| `directDrainWaitParkCount[]` | materially lower than before |
| `directPendingMessageCount` | usually near zero; short bursts acceptable |
| `directDataLag[]` | explains worker data-ring backlog |
| `directAckLag[]` | explains ack-ring/owner-drain backlog |
| `totalAckedLag/s` | not worse than baseline |
| owner CPU | no hot-spin regression |

eBPF owner futex attribution:

```bash
timeout 60 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == OWNER_TID/ { @[args->op & 127, ustack(12)] = count(); }'
```

Owner scheduler behavior:

```bash
timeout 60 bpftrace -e 'tracepoint:sched:sched_switch /args->prev_pid == OWNER_TID/ { @prev_state[args->prev_state] = count(); } tracepoint:sched:sched_switch /args->next_pid == OWNER_TID/ { @wakeins = count(); }'
```

CPU hot classes:

```bash
timeout 60 bpftrace -e 'profile:hz:99 /pid == PID/ { @[comm] = count(); }'
```

VFS write pressure:

```bash
timeout 60 bpftrace -e 'tracepoint:syscalls:sys_enter_write /pid == PID/ { @bytes[comm] = sum(args->count); @calls[comm] = count(); }'
```

## Rollback Criteria

Rollback the pending-gating commit first if any of these appear:

- `directPendingMessageCount` grows monotonically for multiple 60s windows.
- owner CPU becomes a new dominant hot class.
- `totalAckedLag/s` worsens while downstream hot classes are unchanged.
- ordering or commit correctness tests fail.
- shutdown reports leaked or double-destroyed Kafka messages.

The sequence and wait metrics commits are low-risk observability changes and can usually remain even
if the behavior commit is rolled back.

## Remaining Work

Recommended next steps:

- Phase 4: batch capacity check and publish by worker bucket.
- Phase 5: cache-line isolation for direct worker counters and sequences.
- Phase 6: real Kafka e2e with three 60s windows and eBPF attribution.

Do not continue with Phase 4 until Phase 3 has been validated with live 60s metrics and eBPF.
