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
  - CORO-RA-8
  - CORO-RA-9
---

# Evidence Ledger

## Contract

### Decision

Only observations produced on `ssh d2` from the source identity recorded below
are evidence for this study. FIL-C, sanitizer, native correctness, wall-time
benchmark, and source-complexity evidence answer different questions and are
not substitutes for one another.

### Scope

This ledger covers the runtime, coroutine and epoll L4 implementations, four
compiled forwarder variants, the A/B harness, three context-switch backends,
the high-concurrency matrix, compiler cross-checks, and binary inspection.

### Non-goals

It does not claim real-NIC line rate, tail latency, long-duration RSS,
production overload behavior, or a universal coroutine/event-loop ranking.

### Inputs And Outputs

| Input | Output |
| --- | --- |
| Commits `ae03646` and `57f7ce3`, pinned tools, d2 topology | correctness logs, hashes, and benchmark samples |
| 5 interleaved runs x 3 modes x 4 streams | 15 aggregates and 60 nonzero per-stream records |
| 5 interleaved runs x 5 modes x 4 streams | 25 aggregates and 100 nonzero per-stream records |

### Interfaces And Ownership

The benchmark harnesses remain general-purpose tools: their output-directory
arguments are caller-selected and are not themselves tracked-evidence
boundaries. Tracked promotion has two non-overlapping root pairs:

| Dataset | `clean_root` | `generated_output` |
| --- | --- | --- |
| L4 A/B | `.tmp/rco-ab/final-evidence` | `learning/studies/20260910-stackful-coroutine/evidence/raw/ab` |
| CACS scale | `.tmp/rco-cacs/final-evidence` | `learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale` |

The CACS driver additionally declares `.tmp/rco-cacs/work` as its work root.
All three CACS roots are repository-relative and pairwise non-overlapping.
The exact no-argument invocation is:

```bash
ssh d2 'cd /root/recallfs && exec bash learning/studies/20260910-stackful-coroutine/demo/bench/cacs_scale_evidence.sh'
```

Its five modes are direct, SysV coroutine, CACS, CACS with
`preserve_none`, and epoll. The latter four are distinct forwarder binaries;
five runs at four streams must produce 25 aggregate rows, 100 stream rows,
20 forwarder-resource rows, and 25 rotation rows.

Before promotion, the promotion driver resolves the repository root and
rejects any configured path if it is absolute, empty, `.`, contains a `..`
segment, resolves to the repository root, traverses a symlink, escapes the
repository after canonicalization, or overlaps another declared root. It also
rejects `clean_root` overlap with any tracked or maintained path and rejects a
`generated_output` other than the exact declared roots above. These checks
apply to promotion only and do not narrow benchmark output-directory CLIs.

After all validation gates pass, the driver creates `SHA256SUMS` in
`clean_root`. The manifest excludes itself and records every other regular
payload as SHA-256 plus a normalized path relative to `generated_output`, in
bytewise path order. The candidate file set must exactly match the manifest,
and every digest must verify in `clean_root`, before this deterministic
promotion:

```bash
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
clean_root=.tmp/rco-cacs/final-evidence
generated_output=learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale
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
- all four forwarders report their expected backend identities and retain
  their validated SHA-256 identities through every five-mode sample;
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

The stable `CORO-RA-1` through `CORO-RA-9` definitions live in
[`../README.md`](../README.md); each raw path cited below is its observed
evidence.

### Evidence And Unknowns

Observed values under
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/`](raw/ab/) and
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/`](raw/cacs-scale/)
are revision- and environment-specific. Real NIC throughput, p99 latency,
long-run memory, and connection-count scaling remain unknown.

## 1. Provenance

| Field | Value |
| --- | --- |
| Target | `ssh d2` / host `n37-125-152` |
| Default L4 code/benchmark commit | `ae0364683fc45e99847872a9f1fe32e5de98c1f4` |
| CACS/high-concurrency evidence source commit | `57f7ce3163adb14ea8023c51b9d0f7272176f8d1` |
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

### L4 A/B (`ae03646`)

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
-O3 -DNDEBUG -flto -march=native -mtune=native
-Wall -Wextra -Werror -fno-omit-frame-pointer -mno-red-zone
-fcf-protection=branch
```

Raw record:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/build-info.txt`](raw/ab/build-info.txt).

### CACS scale (`57f7ce3`)

| Artifact | SHA-256 |
| --- | --- |
| `demo/src/rco.c` | `7afa62fa6a77b654e469f0528bbf8e7a6d9574d9fb7e0e07c898740752f75e0b` |
| `demo/src/rco_internal.h` | `df07019a25d15932b0789d8ac104215196c946b27f33a0ee4143ac8bbb0b09ed` |
| `demo/src/rco_context_cacs_x86_64.S` | `37e56ccad8e5068e3cca79955255ad2381640be5e729d3b8a5c94dd0757b3589` |
| `demo/include/rco.h` | `f3575521f350878ca585f46f5fead862a45a80cff1f88c131b6a251188750137` |
| `demo/bench/rco_high_concurrency_bench.py` | `972ab6ab83c941cba3ffeeaefb68e16650bc4c8f2b2e6b0acb9655e4753de107` |
| `demo/bench/l4_bench.sh` | `0616e610b9dd0b963da0c0c1221cfc13c69b86a43bb7b8fc31cdc13ef0ac6397` |
| `demo/bench/cacs_scale_evidence.sh` | `5246db76bd516a60b6e0a73a164f53808fbae200d147b49f4a39063bb242ec01` |
| high-concurrency SysV binary | `1aa82e4aa0b33dca8dcfeee78275cf420031c101eac95b315930d236935a7e0f` |
| high-concurrency CACS binary | `9b9763c07f3b42052a77e7018de9a611c57e11af7df398496b6dd4dfa19c23af` |
| high-concurrency CACS+PN binary | `f9ff6390d0da0320f088eda34bfceaaaf3f70b97758d8e195bd4e423ce6258db` |
| L4 SysV coroutine binary | `6976eae1061c5ca1fa5d36548df0e0f7fd944bff10a6f1c262700035bba788ab` |
| L4 CACS binary | `ef0fb49d36b4758bbfde4cba76aa4d53e828c957689200acb431ebcfc50a1bf0` |
| L4 CACS+PN binary | `468e78674647a4766be78a4f9cc0815568aacc8bc944a55156a3673d79489737` |
| L4 epoll binary | `3b14ac307ee997f814085db94803707cacc9b7450adfa5e8e2b9c2735413200b` |

Full compiler, source and binary identity:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/environment.txt`](raw/cacs-scale/environment.txt).

## 3. Correctness Gates

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Proof-first native link | Expected undefined `rco_*` symbols | [`../exploration.md`](../exploration.md) |
| FIL-C lifecycle | 3 suites passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/filc.txt`](raw/ab/filc.txt) |
| FIL-C L4 source | Both L4 paths plus epoll state-machine test passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/filc.txt`](raw/ab/filc.txt) |
| Native CTest | 9/9 targets passed; same E2E for both forwarders | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Process parser | `comm` containing spaces and `)` preserves fields 14/15 | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/native-tests.txt`](raw/ab/native-tests.txt) |
| Zig LLVM frontend | Runtime, coroutine, and epoll O0/O2/O3 passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/zig-validation.txt`](raw/ab/zig-validation.txt) |
| CACS matrix | Three backends pass O0/O2/O3, ABI, codegen and guard tests | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/optimization-matrix.txt`](raw/cacs-scale/optimization-matrix.txt) |
| CACS L4 integration | SysV, CACS and CACS+PN pass the same L4 E2E | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/native-tests.txt`](raw/cacs-scale/native-tests.txt) |
| CACS native CTest | 25/25 targets passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/native-tests.txt`](raw/cacs-scale/native-tests.txt) |
| CACS ASan + UBSan | 22/22 targets passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/sanitizer-tests.txt`](raw/cacs-scale/sanitizer-tests.txt) |
| FIL-C scale parser/bounds | Invalid high-concurrency bounds rejected before stack switching | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/filc.txt`](raw/cacs-scale/filc.txt) |
| High concurrency | 150/150 samples pass checksum, switch, sentinel and lifecycle oracles | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/high-concurrency/samples.csv`](raw/cacs-scale/high-concurrency/samples.csv) |
| CACS evidence manifest | 130/130 payload paths and SHA-256 digests passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/SHA256SUMS`](raw/cacs-scale/SHA256SUMS) |
| Parent signal cleanup | 19/19 focused tests; launch-window signal, stopped leader and TERM-ignoring descendant reaped | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/high-concurrency-focused-tests.txt`](raw/cacs-scale/high-concurrency-focused-tests.txt) |
| Complete build commands | O0/O2/O3/ASan compile and link commands captured and target-bound | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/build-commands/`](raw/cacs-scale/build-commands/) |
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

## 6. CACS And High Concurrency

All three variants use the same 64-byte context layout and the same
Zig 0.16.0/Clang 21.1.0 optimized build.

| Backend | Median `rco_yield` | Ratio vs SysV |
| --- | ---: | ---: |
| SysV | `35.657 ns` | `1.000` |
| CACS | `13.322 ns` | `0.374` |
| CACS + explicit `preserve_none` | `13.330 ns` | `0.374` |

Representative scaling results:

| Case | SysV CPU ns/measured-yield | CACS | CACS+PN |
| --- | ---: | ---: | ---: |
| 256 tasks, 4 KiB touched | 74.76 | 23.62 | 24.34 |
| 4,096 tasks, 4 KiB touched | 164.85 | 45.94 | 47.99 |
| 16,384 tasks, 4 KiB touched | 290.18 | 76.92 | 76.00 |
| 1,024 tasks, 128 KiB touched | 664.90 | 425.16 | 420.53 |
| 1,024 tasks, 256 KiB touched | 1,463.69 | 1,230.50 | 1,240.46 |
| 1,024 tasks, 1,024 yields/task | 136.83 | 39.79 | 38.33 |

At 16,384 tasks, median PSS is 200,838/200,838/200,838 KiB for
SysV/CACS/CACS+PN. Identical context size and stack configuration remove
layout as an explanation for the CPU result. Increasing touched stack from
32 KiB to 256 KiB reduces the CACS advantage from about 67% to 16%, showing
that memory work eventually dominates switch cost.

Raw matrix, environment, IR, disassembly and sanitizer evidence:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/`](raw/cacs-scale/).

The same CACS binaries were then compared in the real L4 workload using five
position-rotated runs:

| L4 path | Median |
| --- | ---: |
| Direct loopback | `72.284 Gbit/s` |
| SysV coroutine | `25.404 Gbit/s` |
| CACS | `25.583 Gbit/s` |
| CACS + `preserve_none` | `25.522 Gbit/s` |
| epoll state machine | `25.145 Gbit/s` |

CACS/SysV paired throughput ratio has median `1.002753`; CACS+PN/SysV is
`1.004652`. The scheduler-only CPU reduction therefore does not propagate to
this kernel-TCP-dominated throughput test.

## 7. PMU Limitation

The required hardware counters were requested with:

```text
perf stat -e cycles,instructions,branches,branch-misses,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses
```

d2's KVM returned `<not supported>` for every event. The fallback evidence is
recorded CPU/NUMA topology, fixed affinity, source/binary digests, complete
flags, repeated wall-time samples, and observable sinks. See
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/perf-stat.txt`](raw/cacs-scale/perf-stat.txt).

The KVM exposes no cpufreq policy directories, `intel_pstate/no_turbo`, or
generic cpufreq boost control. Both context and L4 environment records state
that frequency policy and turbo state are unavailable instead of silently
omitting them.

## 8. Document DAG Validation

The final metadata graph was checked against every tracked `doc_id`:

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
