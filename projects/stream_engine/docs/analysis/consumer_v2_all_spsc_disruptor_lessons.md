# Consumer V2 All-SPSC Architecture: Lessons From LMAX Disruptor

Date: 2026-05-22

## Scope

This note compares the current `consumer_v2` all-SPSC / direct-dispatch architecture with design
ideas from LMAX Disruptor.

The goal is not to import the Java Disruptor library or replace the current SPSC lanes. The goal is
to absorb useful engineering details:

- sequence-based progress tracking
- gating without blocking the producer on every full queue
- preallocated event slots
- explicit wait strategies
- batch publication and end-of-batch behavior
- cache-line isolation and false-sharing avoidance
- dependency graph thinking for data, ack, commit, and metrics paths

Primary local architecture reference:

- `docs/consumer_v2/design/single_client_spsc_dispatch_architecture.md`

Primary external reference:

- `https://github.com/LMAX-Exchange/disruptor`
- `https://lmax-exchange.github.io/disruptor/user-guide/index.html`
- `https://lmax-exchange.github.io/disruptor/disruptor.html`

## Current All-SPSC Model

The production direct path currently looks like this:

```text
Kafka owner
  -> directWorkerRings_[worker]     owner writes, worker reads
  -> directWorkerSignals_[worker]   owner notifies worker

Worker
  -> directAckRings_[worker]        worker writes, owner reads
  -> directDrainSignals_[worker]    worker notifies owner after popping data

Kafka owner
  -> drain direct ack rings
  -> apply committed slots
  -> commit / lag / metrics through owner-owned Kafka API path
```

Important invariants already match Disruptor-style mechanical sympathy:

| Area | Current invariant |
|---|---|
| Kafka API | single owner thread |
| Data ring | one producer, one consumer |
| Ack ring | one producer, one consumer |
| Payload transfer | `RdKafka::Message*` pointer transfer, no payload copy |
| Backpressure | bounded rings, no drop |
| Ordering | one topic-partition bound to one worker at a time |
| Hot path lock | no new `std::mutex` in direct record/read/ack path |

The main weakness seen in live eBPF/metrics evidence is not that SPSC is the wrong shape. It is that
the owner can still block inside direct dispatch when a sticky worker ring is full:

```text
Kafka owner polls one message
  -> choose sticky worker
  -> worker ring full
  -> owner waits on directDrainSignals_[worker]
```

That path showed up as `LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop` in futex
sampling.

## Disruptor Concepts Worth Absorbing

### 1. Sequence Is The Core Primitive

In Disruptor, the ring buffer is not the real core. The core is `Sequencer` and `Sequence`:

- producer claims sequence ranges
- producer publishes the highest visible sequence
- consumers advance their own sequences
- producer gates against the minimum dependent consumer sequence
- each `Sequence` is padded to avoid false sharing

For `consumer_v2`, the equivalent should be explicit per-lane and per-slot progress, not only queue
depth guesses.

Suggested mapping:

| Disruptor concept | Consumer V2 equivalent |
|---|---|
| `cursor` / published sequence | owner-published data sequence per worker lane |
| consumer sequence | worker-read sequence per worker lane |
| gating sequence | lowest safe sequence that owner must not overwrite |
| dependent sequence | acked / committed sequence for slot or partition |
| `remainingCapacity()` | exact `capacity - (published - consumed)` instead of `sizeGuess()` |

Why this matters:

- `sizeGuess()` is fine for metrics, but weak for decisions.
- A full SPSC ring is a symptom; sequence deltas tell which edge is slow.
- eBPF showed futex waits, but sequence counters would show whether owner waited because worker did
  not pop, worker did not ack, or downstream was blocked after read.

Recommended detail:

```text
DirectWorkerLane[w]
  publishedSeq      owner increments after successful push
  consumedSeq       worker increments after pop/read
  ackPublishedSeq   worker increments after ack enqueue
  ackConsumedSeq    owner increments after ack drain
```

These counters should be cache-line isolated and exposed in metrics:

```text
dataLag[w] = publishedSeq[w] - consumedSeq[w]
ackLag[w]  = ackPublishedSeq[w] - ackConsumedSeq[w]
```

### 2. Gating Should Replace Park-On-Full As The First-Class Model

Disruptor prevents producer wrap by gating against consumer sequences. The producer does not treat a
full ring as an exceptional queue condition; it treats it as a normal capacity decision derived from
sequence arithmetic.

Current direct path behavior:

```text
tryPush()
if full:
  wait directDrainSignal
  retry same worker
```

Better all-SPSC behavior:

```text
required = batchSize or 1
available = lane.capacity - (publishedSeq - consumedSeq)

if available >= required:
  publish
else:
  mark lane blocked
  stop feeding that lane in this owner turn
  drain ack/control/commit/lag work
  retry later or pause affected partitions
```

This keeps bounded backpressure but avoids making the Kafka owner park immediately in the dispatch
hot path.

Important correction:

- The owner still must preserve partition order.
- The owner must not move a sticky partition while old queued or in-flight records exist.
- The owner can avoid waiting by retaining a bounded pending record/batch for that partition, or by
  pausing/reducing polling, but it cannot drop or reorder the message.

### 3. Wait Strategy Must Be Explicit And Measurable

Disruptor has explicit wait strategies:

- busy spin
- yield
- sleeping
- blocking
- phased backoff: spin, then yield, then fallback

Current direct path already has a small phased budget:

```text
spinCount = 16
yieldCount = 4
parkTimeoutUs = 500
```

But the decision is local to the full-ring retry path. It is not expressed as a lane-level policy
with metrics.

Recommended detail:

| Wait site | Preferred strategy | Reason |
|---|---|---|
| worker waits for data | phased spin/yield/park | worker can park when idle |
| owner waits for ack ring drain | short spin/yield, bounded park only on shutdown-safe path | ack ring full should be rare |
| owner sees worker data ring full | do not park first; mark lane blocked and continue owner maintenance | owner must keep polling callbacks, commits, lag refresh, and acks |
| shard/owner idle | phased wait on signal sequence | no work exists |

Add metrics:

```text
directDataWaitSpinCount[w]
directDataWaitYieldCount[w]
directDataWaitParkCount[w]
directDrainWaitParkCount[w]
directDrainWaitParkUs[w]
directLaneBlockedTurns[w]
```

This lets eBPF futex counts be mapped back to runtime-level wait reasons.

### 4. Preallocate Slots, Not Per-Record Objects

Disruptor preallocates event objects in the ring. The publisher mutates an existing slot and then
publishes a sequence. This reduces allocation and improves cache locality.

Current direct path already avoids payload copy by passing `RdKafka::Message*`, but still has
metadata objects and per-message ownership transitions.

The longer-term all-SPSC architecture already points in the right direction:

```text
IngressRecord:
  slotIndex
  partitionIndex
  offset
  messageLen
```

Recommended detail:

- Keep `RdKafka::Message*` or payload pointer in a preallocated slot table.
- Push compact slot indexes through SPSC lanes when possible.
- Use power-of-two ring capacities so slot index masking is cheap.
- Avoid per-record `TopicPartition` string construction in hot path.
- Keep per-slot metadata contiguous for owner-owned updates.
- Keep per-worker sequence/cache-hot fields padded away from adjacent worker fields.

Potential target:

```text
DirectSlot {
  RdKafka::Message* message
  uint32_t partitionSlot
  uint32_t worker
  int64_t offset
  uint32_t len
}

directWorkerRing[w] carries uint32_t slotIndex, not a larger message wrapper.
```

This turns the data lane into a compact index ring and makes sequence/slot accounting cheaper.

### 5. Batch Claim / Batch Publish Should Be The Default

Disruptor supports claiming a range and publishing a range. This reduces sequence updates and wakeup
frequency.

Current direct path pushes message by message, then batch-notifies touched workers at the end:

```text
for message in poll batch:
  tryPushToWorker(...)

for touched worker:
  notify()
```

This is already better than per-record notify, but capacity decisions are still per message.

Recommended detail:

```text
owner poll batch
  -> group by target worker
  -> for each worker bucket:
       check available capacity once
       push contiguous chunk
       publish sequence once
       notify once
```

For sticky partitions, grouping must preserve per-partition order:

- records for the same partition stay in original Kafka order
- a sticky partition still maps to only one worker at a time
- if a worker has partial capacity, publish a prefix only and keep the remaining suffix pending

Expected effect:

- fewer atomic sequence updates
- fewer failed `tryPush` loops
- fewer owner-side full-ring waits
- better cache locality in worker reads

### 6. End-Of-Batch Is A Useful Signal

Disruptor event handlers receive `endOfBatch`. That lets consumers coalesce downstream work and
reduce expensive operations.

Consumer V2 can use the same idea:

```text
DirectMessage:
  slotIndex
  flags.endOfBatchForWorker
  flags.endOfPollBatch
```

Useful applications:

- worker can aggregate ack events until end-of-batch
- sink path can flush at batch boundary rather than per record
- metrics can sample once per batch
- owner can commit/coalesce after observed batch boundaries

This is especially relevant because current eBPF evidence repeatedly shows downstream hot classes:

- `slot-*`
- `writer/ex-*`
- `fringedb-c*`
- `rdk:broker-*`

If downstream write/flush dominates, reducing per-record downstream actions may matter more than
micro-optimizing the SPSC queue itself.

### 7. Consumer Dependency Graph Maps Cleanly To Ack / Commit

Disruptor supports consumer dependency graphs and only gates producers on the leaf sequences that
matter. This is directly applicable to `consumer_v2`.

Current direct pipeline:

```text
owner publish -> worker read -> worker process -> worker ack -> owner drain ack -> commit
```

The owner should not gate data publication on every downstream detail. It needs the right leaf
progress counters:

| Progress | Owner uses it for |
|---|---|
| worker consumed sequence | data ring capacity |
| worker ack published sequence | ack ring pressure |
| owner ack consumed sequence | commit candidate visibility |
| partition contiguous ack offset | safe commit |

This separation prevents one metric from being overloaded. For example:

- high data ring lag means worker is not popping fast enough
- low data ring lag but high ack lag means worker/process/downstream is slow after read
- low local ack lag but high broker lag means commit visibility or Kafka commit path needs checking

### 8. False Sharing Must Be Treated As A Design Invariant

Disruptor pads `Sequence` to prevent false sharing. This is not a Java-only lesson. It matters for
C++ per-worker counters too.

Current direct mode has many per-worker arrays:

```text
directWorkerPushedRecords_[w]
directWorkerReadRecords_[w]
directWorkerAckedRecords_[w]
directWorkerRings_[w]
directAckRings_[w]
directWorkerSignals_[w]
directDrainSignals_[w]
```

If hot counters for adjacent workers share cache lines, 48 workers can create unnecessary cache-line
ping-pong even with SPSC data lanes.

Recommended detail:

```text
struct alignas(64) DirectWorkerCounters {
  std::atomic<uint64_t> pushed;
  std::atomic<uint64_t> read;
  std::atomic<uint64_t> acked;
  std::atomic<uint64_t> dataWaitPark;
  std::atomic<uint64_t> drainWaitPark;
  char pad[...];
};
```

Rules:

- counters written by different workers should not share a cache line
- producer sequence and consumer sequence should not share a cache line if written by different
  threads
- read-mostly config should not share lines with hot counters
- per-worker lane state should be stable-address and aligned

### 9. Single Producer Assertion Is Valuable

Disruptor's `SingleProducerSequencer` includes an assertion that detects accidental access by more
than one producer thread when assertions are enabled.

Consumer V2 should add debug-only ownership checks for SPSC lanes:

```text
SpscLaneDebugOwner:
  producerTid
  consumerTid

tryPush:
  assert currentTid == producerTid or producerTid unset

tryPop:
  assert currentTid == consumerTid or consumerTid unset
```

This is not for production hot path. It is a debug/test/eBPF validation aid.

Benefits:

- catches accidental MPSC regression early
- proves all-SPSC ownership in tests
- makes future refactors safer

### 10. Do Not Confuse Multicast With Our Work Distribution

Disruptor can multicast every event to multiple consumers. Consumer V2 direct dispatch is not a
multicast problem:

```text
one Kafka record -> exactly one worker
```

So we should not copy the Disruptor consumer model literally. The useful part is the dependency
graph and sequence gating, not broadcasting all events to all workers.

## Recommended Improvements

### P0: Sequence Metrics For Direct Lanes

Add explicit sequence counters to data and ack lanes.

```text
dataPublishedSeq[w]
dataConsumedSeq[w]
ackPublishedSeq[w]
ackConsumedSeq[w]
```

Expose derived metrics:

```text
dataLag[w]
ackLag[w]
dataAvailableCapacity[w]
ackAvailableCapacity[w]
```

Why P0:

- lets us distinguish worker-pop bottleneck from worker-process/ack bottleneck
- makes eBPF futex evidence explainable from metrics
- enables gating decisions without relying on approximate depth

### P0: Replace Owner Park-On-Full With Non-Blocking Gating

For sticky worker full:

```text
do not immediately park owner on directDrainSignals_[worker]
```

Instead:

```text
if sticky lane has no capacity:
  keep message or suffix in bounded owner pending state
  mark worker lane blocked
  drain direct acks
  run commit/control/lag refresh budgets
  retry pending before polling more records
  pause affected partitions if pending exceeds high watermark
```

Correctness constraints:

- bounded pending only
- no partition reorder
- no message drop
- no migration until queued and in-flight are both zero
- shutdown drains or destroys pending messages exactly once

### P1: Batch Capacity Check And Batch Publish

Group polled records by target worker before pushing:

```text
poll batch
  -> choose worker per record
  -> build worker buckets
  -> publish bucket prefix based on available capacity
  -> keep blocked suffix pending
```

Expected benefits:

- fewer `tryPush` failures
- fewer sequence/cache updates
- fewer worker notifications
- lower owner futex risk

### P1: Per-Lane Wait Strategy Policy

Create named wait strategies:

| Strategy | Use |
|---|---|
| `busy-low-latency` | controlled benchmark only |
| `phased-default` | worker idle data wait |
| `owner-nonblocking` | owner full-ring path |
| `shutdown-safe-blocking` | close/release only |

Metrics must separate:

- spin retries
- yields
- parks
- timeout wakes
- notify wakes

### P1: Cache-Line-Isolated Worker Lane State

Audit per-worker arrays and consolidate hot fields into aligned structs:

```text
DirectWorkerLaneState[w]
  data ring pointer
  ack ring pointer
  padded sequences
  padded counters
  signal pointers
```

This makes ownership and cache behavior explicit.

### P2: Slot-Index Ring Instead Of Message Wrapper Ring

Move toward:

```text
directWorkerRing[w]: uint32_t slotIndex
slotTable[slotIndex]: message pointer + metadata
```

This is closer to Disruptor preallocated event slots.

Benefits:

- smaller ring elements
- better cache locality
- easier per-slot queued/in-flight accounting
- easier safe reassignment checks

Risks:

- slot lifetime becomes stricter
- revoke/shutdown must release slots correctly
- more state must be owner-owned or shard-owned by construction

### P2: End-Of-Batch Flags

Add batch boundary metadata so workers and sinks can coalesce:

```text
endOfWorkerBatch
endOfPollBatch
```

Validate with eBPF:

- fewer downstream write/flush calls
- lower `writer/ex-*` and `fringedb-c*` syscall pressure
- no worse ack latency

## Proposed Validation

Each improvement should be judged with the same 60s evidence discipline used in the bottleneck
analysis.

### Runtime Metrics

Collect at least 3 consecutive 60s windows:

```text
totalPolledRecords/s
totalReadRecords/s
totalAckedRecords/s
totalAckedLag/s
ringLiveCount/s
workerQueueDepth/s
dataLag[w]
ackLag[w]
directDrainWaitParkCount[w]
directDrainWaitParkUs[w]
```

### eBPF Commands

Futex attribution for owner:

```bash
timeout 60 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == OWNER_TID/ { @[args->op & 127, ustack(12)] = count(); }'
```

Scheduler behavior for owner:

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

### Success Criteria

For non-blocking owner gating:

| Metric | Expected direction |
|---|---|
| owner `LaneSignal::wait` futex stack | down materially |
| owner sleeping sched switches | down or explained by idle |
| `dataLag[w]` | not worse than baseline |
| `ackLag[w]` | not worse than baseline |
| `totalAckedLag/s` | flat or lower |
| CPU spin | no owner hot spin regression |

For batch publish:

| Metric | Expected direction |
|---|---|
| failed push retries | down |
| worker notifications per record | down |
| owner futex waits | down |
| read batch size | same or higher |
| ack throughput | same or higher |

## Final Recommendation

The all-SPSC direction is still correct. The most valuable Disruptor lesson is not a new queue
implementation; it is making sequence/gating/wait strategy first-class.

Near-term work should focus on:

1. Add explicit per-lane data/ack sequences and padded metrics.
2. Replace owner park-on-full with bounded non-blocking pending plus pause/backpressure.
3. Batch capacity checks and batch publish by worker.
4. Make wait strategy metrics visible so eBPF futex counts map to runtime causes.
5. Move gradually toward preallocated slot-index rings when lifetime handling is ready.

This keeps the current correctness model:

```text
single Kafka owner
bounded SPSC on every hot edge
one topic-partition owned by one worker at a time
no hot-path mutex
no record drop
```

And it absorbs the parts of Disruptor that matter most for this system:

```text
sequence-driven progress
gating instead of accidental blocking
preallocated slots
phased wait strategy
cache-line isolation
batch publication
dependency graph clarity
```
