---
name: "allocator-engineering"
description: "Optimizes allocation, initialization, and memory lifetime with evidence. Invoke for malloc/RSS hot paths, arenas, slabs, zeroing, or Rust allocation work."
---

# Allocator Engineering

Use this skill to turn allocator symptoms into a measured change with explicit
lifetime and initialization contracts. Do not begin by replacing the process
allocator.

## 1. Required Outcome

Produce:

1. the current allocation and ownership model;
2. workload evidence across value, size, lifetime, access, concurrency, and
   topology;
3. the smallest allocator/representation change that attacks the measured
   mechanism;
4. correctness invariants for alignment, failure, lifetime, and initialization;
5. before/after allocator, CPU/VM, and production metrics;
6. rollback conditions.

For the full C-first explanation and runnable reference, read:

`../../../learning/studies/20260907-allocator-provenance/README.md`

## 2. Non-Negotiable Rules

- Reduce logical allocation count before tuning allocator internals.
- Treat `fresh`, `dirty reused`, `explicitly zeroed`, `OS reclaimed`, and
  `mixed` as different states.
- Skip initialization only from a documented, range-complete contract.
- Do not equate virtual address reuse, RSS residency, and zero contents.
- Do not infer production RSS from `sizeof`, requested bytes, or a
  microbenchmark.
- Allocation and deallocation must use the same allocator family and layout.
- Unknown platform behavior, partial reclaim, and syscall failure fall back to
  the conservative path.
- C code must compile and run with the repository FIL-C toolchain. Never fall
  back to host Clang.
- Rust `unsafe` must document initialization, layout, provenance, drop, panic,
  and allocator-identity invariants.

## 3. Workflow

### Step 1: Define the Metric Boundary

Record at least:

| Layer | Required metrics |
| --- | --- |
| API | allocation/reallocation count, requested bytes, failure count |
| Allocator | usable/live/active/resident, bin or size class, fragmentation |
| CPU | cycles, instructions, cache/TLB misses, lock contention |
| VM | minor/major faults, anonymous RSS, THP, NUMA placement |
| Product | throughput, p95/p99 latency, steady-state RSS/PSS |

Separate cold start, steady state, peak load, and post-load retention.

### Step 2: Classify the Workload

Build a six-dimensional table:

| Dimension | Evidence to collect |
| --- | --- |
| Value | common values and variants |
| Size | histogram, size classes, long tail |
| Lifetime | owner, creation/destruction batches |
| Access | sequential/random, read/write, hot/cold |
| Concurrency | allocating/freeing thread, remote-free ratio |
| Topology | CPU/NUMA node, arena/shard placement |

If these are unknown, instrument first. Do not guess an allocator from a type
name or average object size.

### Step 3: Optimize in This Order

```text
+---------------------------+
| remove allocations        |
| borrow, inline, merge     |
+-------------+-------------+
              |
              v
+---------------------------+
| reuse capacity            |
| reserve, scratch buffers  |
+-------------+-------------+
              |
              v
+---------------------------+
| match lifetime domain     |
| arena, slab, pool         |
+-------------+-------------+
              |
              v
+---------------------------+
| preserve provenance       |
| zero, dirty, reclaimed    |
+-------------+-------------+
              |
              v
+---------------------------+
| tune allocator backend    |
| bins, arenas, purge       |
+-------------+-------------+
              |
              v
+---------------------------+
| validate in production    |
+---------------------------+
```

Do not advance to a lower box when a higher box explains the dominant cost.

### Step 4: Write the Initialization State Machine

At minimum model:

| State | Zeroed request |
| --- | --- |
| Fresh private anonymous mapping | May elide clear under OS contract |
| Dirty in-process reuse | Must clear |
| Fully reclaimed private anonymous range | May elide after successful eager discard |
| Partially reclaimed or mixed range | Must clear |
| Unknown platform/release mode | Must clear |

The allocation transaction should return or preserve provenance. Avoid a later
metadata rescan if the lower layer already computed the answer.

For Linux, distinguish:

- `MADV_DONTNEED`: private anonymous mappings become zero-fill-on-demand after
  success;
- `MADV_FREE`: contents may remain observable until reclaim, so it is not
  immediate zero evidence;
- `posix_madvise(POSIX_MADV_DONTNEED)`: an advisory API that must not change
  access semantics.

### Step 5: Choose the C Allocator Shape

| Workload | Candidate |
| --- | --- |
| Arbitrary size and lifetime | general allocator |
| Batch lifetime, many small objects | bump arena |
| Same-size, independently reused objects | slab/pool |
| Large page-aligned regions | page allocator |
| Mostly local allocate/free | thread-local cache |

For a custom C allocator, require:

- overflow-safe size/alignment arithmetic;
- explicit zero-size behavior;
- explicit failure return;
- allocator identity paired with every deallocation;
- no pointer use after arena reset/reclaim;
- full-range and no-live-reference proof before page reclaim;
- counters for backing and logical allocations;
- tests for dirty reuse and failure fallback.

Prefer a domain allocator handle over hidden global replacement. Use
`LD_PRELOAD` or link-time replacement only for controlled measurement or when
the whole-process ownership contract is understood.

### Step 6: Map the Decision to Rust

Use stable APIs first:

- `Vec::with_capacity` or `reserve` for known growth;
- `try_reserve` for recoverable capacity/allocation errors;
- `clear` to reuse scratch capacity;
- `Vec::into_boxed_slice` for owned fixed-length data;
- slices and `Cow` to avoid ownership and copies;
- `MaybeUninit` or spare capacity only after initialization cost is measured;
- `GlobalAlloc` only when a binary-level allocator decision is justified.

Before using per-container `Allocator`, `Vec<T, A>`, `Box<T, A>`, or `*_in`,
check the current official docs; these APIs may still require nightly.

For `MaybeUninit`, prove:

1. every element/field becomes a valid `T`;
2. initialized length is advanced only after writes;
3. error/panic cleanup drops initialized resources;
4. zero bytes are a valid representation for `T` before using zeroed memory.

For arena crates, verify whether object `Drop` runs. An arena freeing its chunks
does not imply that nested resources were released.

### Step 7: Verify

Before compiling C, confirm that `.tmp/fil-c/bin/filcc` and
`.tmp/fil-c/bin/filrun` are executable. If either is absent, read
`../../../tools/c/README.md` and provision the repository-local supported Linux
environment. If provisioning cannot complete, stop and report FIL-C as a
blocker. Never substitute the host C compiler.

For C:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  <sources> -o .tmp/<test>
.tmp/fil-c/bin/filrun .tmp/<test>
```

Required test categories:

- fresh zero path;
- dirty reuse path;
- full reclaim path;
- failed reclaim fallback;
- partial reclaim fallback when the implementation accepts partial ranges;
- alignment and overflow;
- reset lifetime contract, plus stale-handle rejection when the API enforces it;
- concurrent ownership when concurrency exists.

FIL-C establishes runtime memory-safety evidence for exercised paths. It does
not prove performance, race freedom, OS portability, or an untested provenance
transition.

For Rust, use release-mode benchmarks and the applicable combination of unit
tests, Miri, sanitizers, allocator profiling, and production telemetry.

### Step 8: Report the Decision

Use this order:

1. conclusion;
2. measured bottleneck;
3. ownership and provenance model;
4. selected allocator/representation;
5. rejected alternatives;
6. correctness evidence;
7. performance evidence;
8. platform limits and rollback gates.

Label source benchmark results separately from locally reproduced results.

## 4. Stop Conditions

Stop and redesign when:

- the lifetime domain cannot be stated;
- a pointer can outlive arena reset or reclaim;
- only part of a range has trusted zero provenance;
- the allocator cannot pair deallocation with the original layout/family;
- the proposed metric is only `sizeof` or a single RSS sample;
- a Rust `unsafe` path cannot describe partial-initialization cleanup;
- a faster microbenchmark regresses page faults, p99 latency, or resident memory.

## 5. Completion Checklist

- [ ] Allocation count reduction was considered before backend replacement.
- [ ] Six workload distributions are recorded.
- [ ] Initialization states and transitions are explicit.
- [ ] Unknown/failed/partial paths conservatively initialize.
- [ ] C code passed FIL-C compile and runtime tests.
- [ ] Rust stable/nightly boundaries are current and cited.
- [ ] Allocator, CPU/VM, and production metrics are separated.
- [ ] Rollout and rollback conditions are stated.
