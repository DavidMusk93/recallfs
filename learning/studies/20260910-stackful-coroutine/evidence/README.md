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
| Commit `ae03646`, pinned tools, d2 topology | correctness logs, hashes, and benchmark samples |
| 5 interleaved runs x 3 modes x 4 streams | 15 aggregates and 60 nonzero per-stream records |

### Interfaces And Ownership

The benchmark harness remains a general-purpose tool: its output-directory
argument is caller-selected and is not itself a tracked-evidence boundary.
Tracked promotion has exactly one clean root and one generated output root:

| Role | Normalized repository-relative path |
| --- | --- |
| `clean_root` | `.tmp/rco-ab/final-evidence` |
| `generated_output` | `learning/studies/20260910-stackful-coroutine/evidence/raw/ab` |

Before promotion, the promotion driver resolves the repository root and
rejects either configured path if it is absolute, empty, `.`, contains a `..`
segment, resolves to the repository root, traverses a symlink, escapes the
repository after canonicalization, or overlaps the other root. It also rejects
`clean_root` overlap with any tracked or maintained path and rejects a
`generated_output` other than the exact declared root above. These checks apply
to promotion only and do not narrow `l4_bench.sh`'s output-directory CLI.

After all validation gates pass, the driver creates `SHA256SUMS` in
`clean_root`. The manifest excludes itself and records every other regular
payload as SHA-256 plus a normalized path relative to `generated_output`, in
bytewise path order. The candidate file set must exactly match the manifest,
and every digest must verify in `clean_root`, before this deterministic
promotion:

```bash
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
clean_root=.tmp/rco-ab/final-evidence
generated_output=learning/studies/20260910-stackful-coroutine/evidence/raw/ab
manifest=SHA256SUMS

(
    cd "$clean_root"
    find . -type f ! -path "./$manifest" -print0 |
        LC_ALL=C sort -z |
        while IFS= read -r -d '' path; do
            sha256sum "${path#./}"
        done
) >"$clean_root/$manifest"

rsync --archive --delete --checksum \
    "$clean_root/" "$generated_output/"
```

After `rsync`, exact path coverage and all manifest digests must verify again
under `generated_output`. Promotion may modify only that declared root; any
path-policy, manifest, validation, or `rsync` failure aborts the promotion.

### Invariants

- source and optimized binary hashes match the recorded build;
- every expected run and stream exists and has positive byte/rate counters;
- medians are derived from raw CSV without discarding runs;
- every promoted payload except `SHA256SUMS` has one verified manifest entry;
- unsupported PMU/frequency controls remain explicit limitations.

### Failure Semantics

Any compile, test, sanitizer, stream-progress, or parser failure aborts
promotion. A missing PMU counter does not abort, but is recorded as
`<not supported>` and cannot support a hardware-counter conclusion.

### Worked Example

For paired run 3, coroutine throughput is `24.883 Gbit/s` and epoll throughput
is `25.423 Gbit/s`; their ratio is `0.978739`. All five ratios are retained,
whose median is `0.993377` and mean is `0.986512`. Opposite signs around 1.0
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
| Code/benchmark commit | `ae0364683fc45e99847872a9f1fe32e5de98c1f4` |
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
| coroutine forwarder | `26e995846817c8e70487f62c4502e0ac0f582ccad8f6b5e89abf93913aac4ea9` |
| epoll forwarder | `47e3becdee07cef3a18ec940b46d94456489794072f4d0d5c3300aadcd6a3f64` |
| A/B harness | `010799db2dc078c6b737e4ecbcd9e4f20ff544179cd9e387ba9a7410d3a29a16` |
| process resource parser | `0cab7e84c4d7376b11f6a0ccb15ff3db8c60de359dabe435e2e7606594bef6ea` |
| optimized context benchmark | `a7d8c0c651f90a68144ae1e1c268f31c982746a0ed0036c1419192ebf1892cde` |
| optimized coroutine forwarder | `9b71dc1970fbf89e835e44b1c580127b71e15244d760255606f73a34ca1dd327` |
| optimized epoll forwarder | `5d27c0298576810cda931a9b236715a838eac50013cf57dd2345d044cf9a4726` |

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
| Native CTest | 9/9 targets passed; same E2E for both forwarders | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Process parser | `comm` containing spaces and `)` preserves fields 14/15 | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Zig LLVM frontend | Runtime, coroutine, and epoll O0/O2/O3 passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/zig-validation.txt`](raw/ab/zig-validation.txt) |
| Zig ASan + UBSan | Both concurrent L4 and forced drain runs passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/sanitizers-coroutine.txt`](raw/ab/sanitizers-coroutine.txt), [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/sanitizers-epoll.txt`](raw/ab/sanitizers-epoll.txt) |
| ABI disassembly | Expected save/restore sequence present | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/context-switch-disassembly.txt`](raw/ab/context-switch-disassembly.txt) |
| Executable stack | Both binaries have `GNU_STACK` as `RW` | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/gnu-stack-coroutine.txt`](raw/ab/gnu-stack-coroutine.txt), [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/gnu-stack-epoll.txt`](raw/ab/gnu-stack-epoll.txt) |
| Evidence integrity | expected run/stream/resource counts and source hashes passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/validation.txt`](raw/ab/validation.txt) |
| Promotion manifest | 59/59 payload paths and SHA-256 digests passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/SHA256SUMS`](raw/ab/SHA256SUMS) |

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
| `rco_yield` | `35.158 ns` |
| noinline function call | `1.637 ns` |
| Linux `sched_yield` | `228.287 ns` |

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
runs:   5, with deterministic mode-order rotation
```

| Path | Median | Range |
| --- | ---: | ---: |
| direct loopback | `74.747 Gbit/s` | `74.392-75.113` |
| coroutine forwarder | `24.883 Gbit/s` | `23.755-25.411` |
| epoll state machine | `25.012 Gbit/s` | `24.827-25.423` |

The coroutine/epoll ratio-of-medians is `0.995`. The median of five paired
ratios is `0.993`, while their mean is `0.987`; the direction changes across
runs. This is parity, not a demonstrated throughput advantage.

The benchmark rejects a sample unless all four sender and receiver streams
make progress. Full per-stream CSV, process resources, iperf client/server
JSON, forwarder counters, actual mode order, and environment are under
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/l4/`](raw/ab/l4/).

| Process metric, median | Coroutine | Epoll |
| --- | ---: | ---: |
| `VmPeak` | 4,724 KiB | 2,948 KiB |
| `VmHWM` | 1,940 KiB | 1,828 KiB |
| user ticks | 2 | 2 |
| system ticks | 398 | 397 |

The stackful version uses about 1,776 KiB more peak virtual space and 112 KiB
more peak resident memory in this five-connection iperf process. CPU tick
resolution is too coarse to rank the implementations.

The recorded soft/hard `RLIMIT_NOFILE` values are 1,024/1,048,576. The focused
startup test observed the same 2,288 KiB `VmSize` at both soft limits, so the
epoll watch table no longer scales its committed VM with the descriptor limit.

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
review-closure --depends_on--> coroutine-evidence
rco-demo-runbook --depends_on--> stackful-coroutine-study
exploration-history --depends_on--> stackful-coroutine-study
exploration-history --depends_on--> source.md
stackful-coroutine-study --depends_on--> agent-ready-docs
stackful-coroutine-study --depends_on--> source.md
```

| Check | Result |
| --- | --- |
| Proposed doc IDs | Five unique IDs; no tracked duplicate |
| Exact path dependency | `source.md` exists |
| ID dependency | `recallfs-agent-ready-docs-v1` resolves to `designs/agent-ready-docs.md` |
| Cycle check | No cycle |
| Local Markdown links | All resolve |

This is the manual evidence for `CORO-RA-5` until a repository linter exists.
