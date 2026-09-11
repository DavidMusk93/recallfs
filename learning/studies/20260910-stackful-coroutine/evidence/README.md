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
  - CORO-RA-10
  - CORO-RA-11
  - CORO-RA-12
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
the high-concurrency matrix, coroutine-local state, deferred preemption,
multicore pool/work stealing, compiler cross-checks, and binary inspection.

### Non-goals

It does not claim real-NIC line rate, tail latency, long-duration RSS,
production overload behavior, or a universal coroutine/event-loop ranking.

### Inputs And Outputs

| Input | Output |
| --- | --- |
| Commits `ae03646` and `d3f48b9`, pinned tools, d2 topology | correctness logs, hashes, and benchmark samples |
| 5 interleaved runs x 3 modes x 4 streams | 15 aggregates and 60 nonzero per-stream records |
| 5 interleaved runs x 5 modes x 4 streams | 25 aggregates and 100 nonzero per-stream records |
| 5 runs x 3 backends x 3 worker counts x 2 preemption modes | 90 validated pool samples |

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

The pool matrix pins workers to CPUs 0-3 and rotates backend, worker-count, and
preemption positions. It validates deterministic work, exact-once finalizers,
pre-start steals, all-worker participation, zero started-coroutine migration,
empty queues, zero failed jobs, and minimum 2/4-worker speedup.

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

In the final five-mode run, SysV/CACS/CACS+PN/epoll medians are
`25.215/25.436/25.099/25.324 Gbit/s`. The CACS/SysV paired-ratio median is
`1.015959`; CACS+PN/SysV is `0.995533`. Opposite directions around 1.0 support
parity, not a winner.

### Reconciliation Anchors

The stable `CORO-RA-1` through `CORO-RA-12` definitions live in
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
| v0.2 CACS/pool evidence source commit | `d3f48b96964d39760ec75b4e5a2ce7227b9b7947` |
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
Review receipts:
[`raw/cacs-code-review.json`](raw/cacs-code-review.json) and
[`raw/v02-code-review.json`](raw/v02-code-review.json). Closure:
[`review.md`](review.md).

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

### CACS and pool scale (`d3f48b9`)

| Artifact | SHA-256 |
| --- | --- |
| `demo/src/rco.c` | `dc543861f6d6f84170f0077ede3e283f5c3247e5f04474395ea6d0321036c3ef` |
| `demo/src/rco_pool.c` | `4c768f757480b755e832ba296a16cbc6947a32725c484b2ef29363955848745f` |
| `demo/src/rco_internal.h` | `df07019a25d15932b0789d8ac104215196c946b27f33a0ee4143ac8bbb0b09ed` |
| `demo/src/rco_context_cacs_x86_64.S` | `37e56ccad8e5068e3cca79955255ad2381640be5e729d3b8a5c94dd0757b3589` |
| `demo/include/rco.h` | `2c88266d476555b1c49c2945b8341361ef16a6f62f518944a29263298f46f78a` |
| `demo/include/rco_local.h` | `1e2989be9b11baf361fe4e469f9c8d44ceb5ab40c6623f86759713002d7b049b` |
| `demo/include/rco_pool.h` | `8713e4f68c4c0d82d30957e679b1c66f112c26f55a1de0689f701a5904505645` |
| `demo/bench/rco_high_concurrency_bench.py` | `972ab6ab83c941cba3ffeeaefb68e16650bc4c8f2b2e6b0acb9655e4753de107` |
| `demo/bench/rco_pool_bench.c` | `91908fa6d0ebad05aa90c31d6f5bb54b4d3aa9d546e9d7b7ec2cf358f4c70d5c` |
| `demo/bench/l4_bench.sh` | `0616e610b9dd0b963da0c0c1221cfc13c69b86a43bb7b8fc31cdc13ef0ac6397` |
| `demo/bench/cacs_scale_evidence.sh` | `701faa05bfda63c64777820d72b33b8c0a53b752cc0f4c5f49b9dd279915c53e` |
| high-concurrency SysV binary | `dbdee0f72ce0fcd900b20ce8531ec820c42ffd07ff5da1300a0eadbd4f4a9f88` |
| high-concurrency CACS binary | `fbd4a0d5f1253474de42a46cdaf7dff935cdbe7e8dfc9c5de09752a6f2163c82` |
| high-concurrency CACS+PN binary | `80bc30eed3c708ec9d4af8ab0dd614d60b44e2518fc9caee582e2e77d0e91508` |
| pool SysV binary | `1840d5010c4b0018d083dd99017f79a42bd2ddd6bd4dfebe187f6dc98356657c` |
| pool CACS binary | `6cd71ba6bb0bab0af08f9755374b36d5eda3f5803b2bf1a20a7b6306f78dc750` |
| pool CACS+PN binary | `8762f581122be6b09e3e343cd43f5614810b31584966d1119fb3b16fe6588b4e` |
| L4 SysV coroutine binary | `49cef6f8ba39e3858c6114080a2b02a245018eefeed85ed92fdd5817d93f328a` |
| L4 CACS binary | `2d6e134117beacc7dd8a3d3051ddb113a12afddfe6975a8770de85a21f8aa9be` |
| L4 CACS+PN binary | `edcc9de1c25d73e8890bcc11f3465fc5171dc52ef2ffb82f78ede9140519d34a` |
| L4 epoll binary | `a88e4a727808b40460d7228beba86e9fbdb4aef47d27b4f5d0e564eb060ab63f` |

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
| CACS native CTest | 37/37 targets passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/native-tests.txt`](raw/cacs-scale/native-tests.txt) |
| CACS ASan + UBSan | 34/34 targets passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/sanitizer-tests.txt`](raw/cacs-scale/sanitizer-tests.txt) |
| GCC SysV CTest | 13/13 targets passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/gcc-tests.txt`](raw/cacs-scale/gcc-tests.txt) |
| FIL-C scale parser/bounds | Invalid high-concurrency bounds rejected before stack switching | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/filc.txt`](raw/cacs-scale/filc.txt) |
| FIL-C v0.2 guards | 5 local-state/runtime/pool suites passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/filc.txt`](raw/cacs-scale/filc.txt) |
| Pool contracts | 3 backends x 500 repeats passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/pool-stress.txt`](raw/cacs-scale/pool-stress.txt) |
| Pool scaling | 90 samples pass checksum, exact-once, stealing, worker coverage, failure, migration, and speedup gates | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/pool-bench.jsonl`](raw/cacs-scale/pool-bench.jsonl) |
| High concurrency | 150/150 samples pass checksum, switch, sentinel and lifecycle oracles | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/high-concurrency/samples.csv`](raw/cacs-scale/high-concurrency/samples.csv) |
| CACS evidence manifest | 139/139 payload paths and SHA-256 digests passed | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/SHA256SUMS`](raw/cacs-scale/SHA256SUMS) |
| Parent signal cleanup | 19/19 focused tests; launch-window signal, stopped leader and TERM-ignoring descendant reaped | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/high-concurrency-focused-tests.txt`](raw/cacs-scale/high-concurrency-focused-tests.txt) |
| Complete build commands | O0/O2/O3/ASan/GCC compile and link commands captured and target-bound | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/build-commands/`](raw/cacs-scale/build-commands/) |
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

## 4. Original A/B Context Benchmark

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

## 5. Original Three-Mode L4 Benchmark

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
| SysV | `51.964 ns` | `1.000` |
| CACS | `27.728 ns` | `0.534` |
| CACS + explicit `preserve_none` | `27.562 ns` | `0.530` |

Representative scaling results:

| Case | SysV CPU ns/measured-yield | CACS | CACS+PN |
| --- | ---: | ---: | ---: |
| 256 tasks, 4 KiB touched | 62.98 | 34.26 | 44.83 |
| 4,096 tasks, 4 KiB touched | 175.97 | 89.62 | 119.53 |
| 16,384 tasks, 4 KiB touched | 229.00 | 115.86 | 195.06 |
| 1,024 tasks, 128 KiB touched | 623.76 | 456.06 | 495.79 |
| 1,024 tasks, 256 KiB touched | 1,413.30 | 1,252.80 | 1,295.36 |
| 1,024 tasks, 1,024 yields/task | 136.21 | 64.85 | 97.61 |

At 16,384 tasks, median PSS is 201,238/201,238/201,238 KiB for
SysV/CACS/CACS+PN. Identical context size and stack configuration remove
layout as an explanation for the CPU result. Increasing touched stack from
32 KiB to 256 KiB reduces the CACS advantage from about 45% to 11%, showing
that memory work eventually dominates switch cost.

Raw matrix, environment, IR, disassembly and sanitizer evidence:
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/`](raw/cacs-scale/).

The same CACS binaries were then compared in the real L4 workload using five
position-rotated runs:

| L4 path | Median |
| --- | ---: |
| Direct loopback | `73.716 Gbit/s` |
| SysV coroutine | `25.215 Gbit/s` |
| CACS | `25.436 Gbit/s` |
| CACS + `preserve_none` | `25.099 Gbit/s` |
| epoll state machine | `25.324 Gbit/s` |

CACS/SysV paired throughput ratio has median `1.015959`; CACS+PN/SysV is
`0.995533`. The scheduler-only CPU reduction therefore does not propagate to
this kernel-TCP-dominated throughput test.

## 7. Multicore Pool

The pool matrix contains 90 samples: three backends, 1/2/4 workers,
preemption disabled/enabled, and five rotated runs per cell. Every sample
passed deterministic checksum, exact-once entry/finalizer, worker coverage,
pre-start stealing, zero migration, zero failed jobs, empty queues, and wake
coalescing checks.

| Backend | Preemption | 1 worker jobs/s | 2 worker speedup | 4 worker speedup |
| --- | --- | ---: | ---: | ---: |
| SysV | off | 7,402 | `1.889x` | `3.117x` |
| CACS | off | 7,421 | `1.891x` | `3.135x` |
| CACS+PN | off | 7,421 | `1.885x` | `3.104x` |
| SysV | 1 ms | 7,275 | `1.880x` | `3.110x` |
| CACS | 1 ms | 7,226 | `1.883x` | `3.145x` |
| CACS+PN | 1 ms | 7,271 | `1.878x` | `3.141x` |

The validator requires at least `1.50x` at two workers and `2.50x` at four
workers. Three backend-specific pool suites also passed 500 consecutive runs
each. Raw samples and stress binary identities are in
[`pool-bench.jsonl`](raw/cacs-scale/pool-bench.jsonl) and
[`pool-stress.txt`](raw/cacs-scale/pool-stress.txt).

## 8. PMU Limitation

The required hardware counters were requested with:

```text
perf stat -e cycles,instructions,branches,branch-misses,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses
```

d2's KVM returned `<not supported>` for every event. The fallback evidence is
recorded CPU/NUMA topology, fixed affinity, source/binary digests, complete
flags, repeated wall-time samples, and observable sinks. See
[`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale/perf-stat.txt`](raw/cacs-scale/perf-stat.txt).

The representative 4-worker pool probe returned the same unavailable result;
see [`perf-pool-stat.txt`](raw/cacs-scale/perf-pool-stat.txt). Pool scaling is
therefore topology + wall/process CPU evidence, not a cache/TLB/branch claim.

The KVM exposes no cpufreq policy directories, `intel_pstate/no_turbo`, or
generic cpufreq boost control. Both context and L4 environment records state
that frequency policy and turbo state are unavailable instead of silently
omitting them.

## 9. Document DAG Validation

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
