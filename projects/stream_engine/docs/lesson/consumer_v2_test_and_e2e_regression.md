# Consumer V2 Test And E2E Regression Lesson

Date: 2026-05-23

## Why This Exists

This note is for future agents working on `consumer_v2`.

The project already has a long stress-test journal in
`docs/consumer_v2/learn_from_stress_test.md`. That document is useful for historical results and
benchmark decisions. This lesson is narrower:

- how to run the tests in the right order
- how to avoid common environment mistakes
- how to run a reliable regression loop after code changes
- how to interpret the reports without overfitting to a misleading metric

## Core Rules

Before changing code:

- decide whether the change is a correctness fix, throughput fix, or shutdown fix
- pick the smallest regression loop that can falsify the change
- do not mix backlog production with throughput benchmark runs

When validating:

- correctness uses unit tests first
- shutdown semantics use `resetForTest()` or `close()` based checks
- throughput uses fixed backlog, fresh consumer group, and `skip produce = 1`
- final decisions use repeated e2e runs or sweep, not one lucky sample

## Standard Validation Ladder

### 1. Fast Local Unit Regression

Use this after any logic change in `shared_consumer.cpp`, direct dispatch, revoke, ack, or reset
paths.

First test in a fresh workspace, after branch switch, or after dependency/build rule changes should
refresh dependencies:

```bash
bash dev/test_run.sh kafka_v2_test -f
```

In this repository, `-f` is the wrapper's way to pass `blade --update-deps`.

After the dependency graph is refreshed, normal incremental reruns can use:

```bash
bash dev/test_run.sh kafka_v2_test
```

If iteration speed matters, run a targeted filter first and only then full `kafka_v2_test`.

Useful targeted cases:

- graceful quit cleanup:

```bash
bash dev/test_run.sh kafka_v2_test --gtest_filter=UnifiedConsumerRuntimeStateTest.ResetDrainsUnreadDirectWorkerMessages
```

- runtime config/default visibility:

```bash
bash dev/test_run.sh kafka_v2_test --gtest_filter=UnifiedConsumerRuntimeStateTest.RuntimeInfoReportsConfiguredLimits
```

- large-scale e2e harness logic:

```bash
bash dev/test_run.sh kafka_v2_test --gtest_filter=UnifiedConsumerBackpressureE2eTest.BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf
```

Interpretation:

- `kafka_v2_test` green is required, but not sufficient, for perf-sensitive changes.
- If the change touches direct ring cleanup or worker shutdown, unit coverage is mandatory.

### 2. Bring Up Local Kafka Once

Use the local Redpanda docker environment:

```bash
bash dev/kafka_e2e/start.sh
```

Expected success signal:

- `redpanda is ready: http://127.0.0.1:9644/v1/status/ready`

### 3. Produce Backlog Once

For real perf or sweep, produce backlog first, then stop producing.

Typical billion-scale topic preparation:

```bash
KAFKA_E2E_TOPIC_PARTITIONS=125 bash dev/kafka_e2e/create_topics.sh <topic>
KAFKA_E2E_BULK_CHUNK_SIZE=1000000 \
KAFKA_E2E_PARTITION_COUNT=125 \
bash dev/kafka_e2e/produce_bulk_records.sh <topic> 1000000000 billion
```

Important:

- production can be long-running
- once backlog is large enough for the target perf window, stop producing
- throughput validation must reuse the same topic with `SKIP_PRODUCE=1`

### 4. Run Real Large-Scale E2E

The reliable path is to run the bundled gtest from `build64_release/src/test`, not to re-enter
`blade test` for every perf round.

The project script now supports this directly:

```bash
KAFKA_E2E_LARGE_SCALE_TOPIC=<topic> \
KAFKA_E2E_LARGE_SCALE_SKIP_PRODUCE=1 \
KAFKA_E2E_LARGE_SCALE_ACK_TARGET=30000000 \
KAFKA_E2E_LARGE_SCALE_PARTITION_COUNT=125 \
KAFKA_E2E_BROKERS=127.0.0.1:9092 \
TIDE_KAFKA_V2_LARGE_SCALE_E2E=1 \
bash dev/kafka_e2e/run_billion_perf_test.sh
```

What the script does:

- starts local Kafka if needed
- reuses the existing topic
- runs `./kafka_v2_test` from `build64_release/src/test`
- injects a filtered `LD_LIBRARY_PATH`
- writes a JSON report

### 5. Run Batch Sweep For Baseline Decisions

Use sweep only when choosing or re-checking a default like `pollDrainBatchSize`.

Example:

```bash
KAFKA_E2E_SWEEP_RUN_ID=batch-size-sweep-$(date +%s) \
KAFKA_E2E_SWEEP_TOPIC=<topic> \
KAFKA_E2E_SWEEP_SKIP_PRODUCE=1 \
KAFKA_E2E_SWEEP_COUNT=30000000 \
KAFKA_E2E_SWEEP_ACK_TARGET=30000000 \
KAFKA_E2E_SWEEP_PARTITION_COUNT=125 \
KAFKA_E2E_SWEEP_WORKER_COUNT=48 \
KAFKA_E2E_SWEEP_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_SWEEP_ORDER=warmup,256,512,1024,2048,4096,4096,2048,1024,512,256 \
bash dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

Use sweep for:

- default tuning
- regression confirmation after hot-path changes
- comparing neighboring batch sizes under the same backlog

Do not use sweep for:

- validating one-off correctness bugs
- running while producer is still writing heavily
- making decisions from a single round

## Reliable Regression Recipe

For most `consumer_v2` changes, this is the standard sequence:

```text
1. targeted unit test
2. full kafka_v2_test
3. prepare or reuse fixed backlog
4. one real large-scale e2e
5. if perf/default is involved, run sweep
6. compare against recent baseline, not historical best-ever outlier
```

### A. Correctness / Lifetime / Revoke Fix

Run:

```text
targeted unit -> full kafka_v2_test -> one e2e if runtime path changed
```

Focus on:

- no crash
- no double free
- no stale ack commit
- no unread direct message leak after reset

### B. Graceful Quit / Close / Reset Fix

Run:

```text
targeted unit -> full kafka_v2_test -> billion e2e
```

Required signals:

- `gracefulQuitOk = true`
- `socketClosedAfterReset = true`
- `afterResetInfo.running = false`
- `afterResetInfo.startRequested = false`
- `afterResetInfo.handleCount = 0`
- `afterResetInfo.workerCount = 0`

Important nuance:

- a full ring in the final running snapshot is not by itself a shutdown bug
- shutdown correctness is judged after `resetForTest()`, not while the workload is still hot

### C. Throughput / Default Tuning Fix

Run:

```text
fixed backlog -> real large-scale e2e -> sweep -> compare median/paired results
```

Required discipline:

- same topic backlog
- fresh group id per run
- same worker count / partition count / ack target
- producer stopped during measurement

## Metrics That Matter

### For Throughput

Primary:

- `ackedMsgsPerSec`
- `durationMs`
- `avgPollBatchRecords`
- `avgReadBatchSize`
- `avgDispatchBatchSize`

Sanity:

- `totalCommitFailures = 0`
- `totalCommitCallbackFailures = 0`
- `lastError = ""`

Helpful diagnosis:

- `avgPollConsumeLatencyUs`
- `finalPausedPartitions`
- `directPendingMessageCount`
- `ringLiveCount`
- `finalRdkThreadCount`

### For Graceful Quit

Primary:

- `gracefulQuitOk`
- `socketClosedAfterReset`
- `jsonEndpointOk`
- `htmlEndpointOk`
- `prometheusEndpointOk`

After-reset state matters more than final running snapshot.

## Common Pitfalls

### 1. Running The Bundle Binary From The Wrong Directory

Problem:

- `build64_release/src/test/kafka_v2_test` may rely on relative interpreter and runfiles paths

Correct way:

- `cd build64_release/src/test`
- run `./kafka_v2_test`

### 2. Polluting Host Tools With Bundle `LD_LIBRARY_PATH`

Problem:

- putting bundle `LD_LIBRARY_PATH` in front of `timeout`, `bash`, or other host tools can load the
  wrong `libc.so.6`
- this can produce misleading errors such as missing `GLIBC_2.34`

Correct way:

- only set `LD_LIBRARY_PATH` for the gtest subprocess
- the script `dev/kafka_e2e/run_billion_perf_test.sh` now does this correctly

### 3. Using `dev/test_run.sh` Inside Sweep

Problem:

- `dev/test_run.sh` may re-enter `blade test`
- 11 sweep rounds become dominated by build/package time instead of test time

Correct way:

- use the direct bundled gtest path for sweep

Related note:

- for the first local test in a fresh workspace, `bash dev/test_run.sh kafka_v2_test -f` is still
  correct and recommended
- for repeated sweep rounds, never pay the `--update-deps` or full blade startup cost per round

### 4. Measuring While Producer Is Still Running

Problem:

- producer, broker write path, page cache, and consumer compete for resources
- results are not comparable to a fixed-backlog baseline

Correct way:

- produce once
- stop producer
- use `SKIP_PRODUCE=1`

### 5. Treating Final Running Snapshot As Final Shutdown State

Problem:

- under high throughput, the final runtime snapshot can legitimately show:
  - `ringLiveCount` near capacity
  - `ringFreeSlotCount = 0`
  - non-zero `directPendingMessageCount`

Correct way:

- evaluate shutdown using the state after `UnifiedConsumer::resetForTest()`

### 6. Misreading Sweep `valid=false`

Problem:

- sweep summary can mark rows invalid because `brokerCatchup=false`
- this does not always mean the gtest failed or the throughput sample is useless

Correct way:

- inspect:
  - command exit code
  - commit/callback failures
  - `lastError`
  - the raw per-run report
- treat analyzer validity as one signal, not the only signal

### 7. Forgetting Topic Creation In Local Kafka

Problem:

- local Redpanda may not auto-create topics
- e2e then fails with `Unknown topic or partition`

Correct way:

- create topic explicitly with `dev/kafka_e2e/create_topics.sh`

## Known Good Regression Modes

### Minimal Reliable Mode

Use when the change is mostly correctness-related:

```text
full kafka_v2_test
+ one large-scale e2e on fixed backlog
```

### Strong Reliable Mode

Use when changing hot path, batch size, direct dispatch, or shutdown semantics:

```text
full kafka_v2_test
+ one large-scale e2e on fixed backlog
+ one batch sweep on fixed backlog
```

### Release-Candidate Mode

Use before changing production defaults:

```text
1. fixed-backlog large-scale e2e
2. batch sweep
3. repeat the winning config
4. compare against recent branch baseline
5. only then change the default
```

## What Future Agents Should Prefer

- Prefer `docs/consumer_v2/learn_from_stress_test.md` for historical benchmark context.
- Prefer this lesson for execution order and failure interpretation.
- Prefer direct bundled gtest for repeated e2e loops.
- Prefer fixed backlog over live produce during measurement.
- Prefer shutdown assertions after reset over runtime snapshot assumptions.

## Current Practical Baseline

As of 2026-05-23:

- `dispatchBatchSize` default stays `1024`
- `pollDrainBatchSize` default is set back to `1024`
- graceful quit must prove post-reset cleanup, not merely a non-crashing close
- direct gtest from `build64_release/src/test` is the default e2e execution mode for repeated sweep
