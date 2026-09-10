---
doc_id: recallfs-evidence-stackful-coroutine-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-stackful-coroutine/demo
depends_on:
  - recallfs-study-stackful-coroutine-v1
  - recallfs-runbook-rco-demo-v1
supersedes: []
verified_by:
  - CORO-RA-1
  - CORO-RA-2
  - CORO-RA-3
  - CORO-RA-4
  - CORO-RA-5
  - CORO-RA-6
  - CORO-RA-7
---

# Evidence Ledger

## Contract

### Decision

Only observations produced on `ssh d2` from the source identity recorded below
are evidence for this study. FIL-C, sanitizer, native correctness, wall-time
benchmark, and source-complexity evidence answer different questions and are
not substitutes for one another.

### Scope

This ledger covers the runtime, both L4 forwarders, the A/B harness, compiler
cross-checks, binary inspection, and the five-run loopback benchmark.

### Non-goals

It does not claim real-NIC line rate, tail latency, long-duration RSS,
production overload behavior, or a universal coroutine/event-loop ranking.

### Inputs And Outputs

| Input | Output |
| --- | --- |
| Commit `463126c`, pinned tools, d2 topology | correctness logs, hashes, and benchmark samples |
| 5 interleaved runs x 3 modes x 4 streams | 15 aggregates and 60 nonzero per-stream records |

### Interfaces And Ownership

The validation driver reads a synchronized copy of the committed source and
writes generated files under `/root/recallfs/.tmp/rco-ab/final-evidence`; only
a complete, validated result is promoted to
`learning/studies/20260910-stackful-coroutine/evidence/raw/ab`.

### Invariants

- source and optimized binary hashes match the recorded build;
- every expected run and stream exists and has positive byte/rate counters;
- medians are derived from raw CSV without discarding runs;
- unsupported PMU/frequency controls remain explicit limitations.

### Failure Semantics

Any compile, test, sanitizer, stream-progress, or parser failure aborts
promotion. A missing PMU counter does not abort, but is recorded as
`<not supported>` and cannot support a hardware-counter conclusion.

### Worked Example

For paired run 3, coroutine throughput is `24.904 Gbit/s` and epoll throughput
is `25.007 Gbit/s`; their ratio is `0.995896`. All five ratios are retained,
whose median is `0.995896` and mean is `1.002570`. Opposite signs around 1.0
support parity, not a winner.

### Reconciliation Anchors

The stable `CORO-RA-1` through `CORO-RA-7` definitions live in
[`../README.md`](../README.md); each raw path cited below is its observed
evidence.

### Evidence And Unknowns

Observed values are revision- and environment-specific. Real NIC throughput,
p99 latency, long-run memory, and connection-count scaling remain unknown.

## 1. Provenance

| Field | Value |
| --- | --- |
| Target | `ssh d2` / host `n37-125-152` |
| Code/benchmark commit | `463126cb7ab3e40df6f214803695dd200a4f0ef4` |
| Kernel | Linux `5.15.198.bsk.1-amd64` |
| CPU | Intel Xeon Platinum 8457C, 2 sockets, 64 cores, no SMT |
| Native compiler | GCC 8.3.0 |
| LLVM frontend | Zig 0.16.0 `zig cc` / Clang 21.1.0 |
| Zig archive SHA-256 | `70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00` |
| FIL-C | 0.684, x86-64 |
| FIL-C archive SHA-256 | `eefb594bcbc1261a18dfa8b50041674635f53df2b5fe067915b5652adaed4e3f` |
| FIL-C container | `debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171` |

The d2 host glibc 2.28 cannot run the FIL-C compiler directly. FIL-C therefore
ran in the pinned Linux container on d2. Native tests and all performance
measurements ran directly on the d2 host.

Full A/B environment:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/l4/environment.txt`](raw/ab/l4/environment.txt).
Review receipt and closure: [`review.md`](review.md).

## 2. Source And Binary Identity

| Artifact | SHA-256 |
| --- | --- |
| `learning/studies/20260910-stackful-coroutine/demo/src/rco.c` | `f496cac01206d510a1c2ae25a1d4d0ea262808a33e6da41d623a360d557fbf51` |
| `learning/studies/20260910-stackful-coroutine/demo/src/rco_context_x86_64.S` | `42b906658f44801887a1903288766fae96d19f0e5db8137a703b2e590bc955e5` |
| `learning/studies/20260910-stackful-coroutine/demo/bench/rco_bench.c` | `9de36e4eec8f819d3bbfe2cc7d70bf7a9c1365e93bc94f384f41564bd2c0dcbc` |
| coroutine forwarder | `c5e2aabbe0b5a2bf46b116442045a1bd543b5708af97caa2cb253b8459ef0e37` |
| epoll forwarder | `28fad64cd3ac0e8acad97842ae86109d48efa9209996f594e3185eacaefa6ae6` |
| A/B harness | `e05f7d4470bb963a1c04162910a851f400325b2e84b5535b5f02f4c8179b25e1` |
| process resource parser | `0cab7e84c4d7376b11f6a0ccb15ff3db8c60de359dabe435e2e7606594bef6ea` |
| optimized context benchmark | `a7d8c0c651f90a68144ae1e1c268f31c982746a0ed0036c1419192ebf1892cde` |
| optimized coroutine forwarder | `61f526568e0f8234af40a0ca0acf212802dffcd24793c89f9e6f04df880bb8e7` |
| optimized epoll forwarder | `a6226d362004b22b777418f2d8a95fa5b8a8d8d3fd0fc5463fecfe2f2d728c90` |

Build flags:

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto
-fno-ipa-icf -Wall -Wextra -Werror -fno-omit-frame-pointer
-fcf-protection=branch
```

Raw record:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/build-info.txt`](raw/ab/build-info.txt).

## 3. Correctness Gates

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Proof-first native link | Expected undefined `rco_*` symbols | [`../exploration.md`](../exploration.md) |
| FIL-C lifecycle | 3 suites passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/filc.txt`](raw/ab/filc.txt) |
| FIL-C L4 source | Both L4 paths plus epoll state-machine test passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/filc.txt`](raw/ab/filc.txt) |
| Native CTest | 7/7 targets passed; same E2E for both forwarders | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Process parser | `comm` containing spaces and `)` preserves fields 14/15 | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Zig LLVM frontend | Runtime, coroutine, and epoll O0/O2/O3 passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/zig-validation.txt`](raw/ab/zig-validation.txt) |
| Zig ASan + UBSan | Both concurrent L4 and forced drain runs passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/sanitizers-coroutine.txt`](raw/ab/sanitizers-coroutine.txt), [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/sanitizers-epoll.txt`](raw/ab/sanitizers-epoll.txt) |
| ABI disassembly | Expected save/restore sequence present | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/context-switch-disassembly.txt`](raw/ab/context-switch-disassembly.txt) |
| Executable stack | Both binaries have `GNU_STACK` as `RW` | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/gnu-stack-coroutine.txt`](raw/ab/gnu-stack-coroutine.txt), [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/gnu-stack-epoll.txt`](raw/ab/gnu-stack-epoll.txt) |
| Evidence integrity | expected run/stream/resource counts and source hashes passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/validation.txt`](raw/ab/validation.txt) |

FIL-C uses
`learning/studies/20260910-stackful-coroutine/demo/tests/rco_context_filc.c`,
whose switch function aborts if called.
It validates executed C allocation and lifecycle paths but does not claim to
validate assembly, stack alignment, native epoll timing, or performance.

## 4. Context Benchmark

Five independent runs, each taking the median of 11 samples with 1,000,000
iterations, pinned to CPU 0 and NUMA node 0:

| Operation | Median |
| --- | ---: |
| `rco_yield` | `35.141 ns` |
| noinline function call | `1.639 ns` |
| Linux `sched_yield` | `227.847 ns` |

`rco_yield` includes scheduler bookkeeping and two assembly transfers. Every
sample retained a nonzero observable sink and exact task/switch-count checks.
Raw samples:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/context-switch.csv`](raw/ab/context-switch.csv).

## 5. L4 Benchmark

iperf3 configuration:

```text
proxy:  CPU 0, NUMA 0
server: CPU 1, NUMA 0
client: CPU 2, NUMA 0
flows:  4
time:   3 seconds measured after 1 second omit
runs:   5
```

| Path | Median | Range |
| --- | ---: | ---: |
| direct loopback | `74.093 Gbit/s` | `73.710-75.391` |
| coroutine forwarder | `24.904 Gbit/s` | `24.471-25.058` |
| epoll state machine | `25.007 Gbit/s` | `23.946-25.167` |

The coroutine/epoll ratio-of-medians is `0.996`. The median of five paired
ratios is `0.996`, while their mean is `1.003`; the direction changes across
runs. This is parity, not a demonstrated throughput advantage.

The benchmark rejects a sample unless all four sender and receiver streams
make progress. Full per-stream CSV, process resources, iperf client/server
JSON, forwarder counters, and environment are under
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/l4/`](raw/ab/l4/).

| Process metric, median | Coroutine | Epoll |
| --- | ---: | ---: |
| `VmPeak` | 4,720 KiB | 2,948 KiB |
| `VmHWM` | 2,016 KiB | 1,760 KiB |
| user ticks | 3 | 3 |
| system ticks | 398 | 397 |

The stackful version uses about 1,772 KiB more peak virtual space and 256 KiB
more peak resident memory in this five-connection iperf process. CPU tick
resolution is too coarse to rank the implementations.

The benchmark is loopback, not a real-NIC result. It demonstrates sustained
TCP data movement and a reproducible comparison baseline, not line-rate or
tail-latency readiness.

## 6. PMU Limitation

The required hardware counters were requested with:

```text
perf stat -e cycles,instructions,branches,branch-misses,cache-misses
```

d2's KVM returned `<not supported>` for every event. The fallback evidence is
recorded CPU/NUMA topology, fixed affinity, source/binary digests, complete
flags, repeated wall-time samples, and observable sinks. See
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/perf-stat.txt`](raw/ab/perf-stat.txt).

The KVM exposes no cpufreq policy directories, `intel_pstate/no_turbo`, or
generic cpufreq boost control. Both context and L4 environment records state
that frequency policy and turbo state are unavailable instead of silently
omitting them.

## 7. Document DAG Validation

Before the A/B documentation mutation, the proposed metadata graph was checked
against every tracked `doc_id`:

```text
coroutine-evidence --depends_on--> rco-demo-runbook
coroutine-evidence --depends_on--> stackful-coroutine-study
rco-demo-runbook --depends_on--> stackful-coroutine-study
exploration-history --depends_on--> stackful-coroutine-study
exploration-history --depends_on--> source.md
stackful-coroutine-study --depends_on--> agent-ready-docs
stackful-coroutine-study --depends_on--> source.md
```

| Check | Result |
| --- | --- |
| Proposed doc IDs | Four unique IDs; no tracked duplicate |
| Exact path dependency | `source.md` exists |
| ID dependency | `recallfs-agent-ready-docs-v1` resolves to `designs/agent-ready-docs.md` |
| Cycle check | No cycle |
| Local Markdown links | All resolve |

This is the manual evidence for `CORO-RA-5` until a repository linter exists.
