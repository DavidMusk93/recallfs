---
doc_id: recallfs-exploration-lock-free-ring-buffer-v1
kind: runbook
status: active
authority: supporting
applies_to:
  - learning/studies/20260927-lock-free-ring-buffer
depends_on:
  - recallfs-source-lock-free-ring-buffer-study-v1
supersedes: []
verified_by:
  - learning/studies/20260927-lock-free-ring-buffer/evidence/verification.txt
  - learning/studies/20260927-lock-free-ring-buffer/evidence/article-shape-benchmark.csv
  - learning/studies/20260927-lock-free-ring-buffer/evidence/alignment-ab.csv
---

# Exploration Log

## 1. Source Freeze

The article was read in a browser on 2026-09-27. Its HTML digest and modified
date were recorded. The linked source was traced through git history:

```text
74ddbf9  Add RingBuffer implementations with benchmark
    |
    v
ffaefb7  Reorganized codebase for simplicity
```

The 248-line source is byte-identical across the move. The original revision is
the most direct source anchor.

## 2. Reproduction Boundary

The implementation was translated independently to C11 rather than wrapping
the C++ template. Four APIs preserve one changing factor per stage:

```text
pthread mutex
    |
    v
seq_cst atomic indices
    |
    v
relaxed owner + acquire peer + release publish
    |
    v
endpoint-local cached peer index
```

The C API owns a separately allocated `uint64_t` slot array. Allocation occurs
once before timing. The cached indices live in thread-owned endpoints rather
than shared queue fields, making their ownership explicit.

## 3. Correctness First

The first completed implementation passed both FIL-C programs:

- 14/14 API, wrap, capacity-2, capacity-17, and concurrent FIFO suites;
- 4/4 benchmark paths with per-item validation and checksums.

The original success message said `10/10` although the driver invoked 14
suites. The label was corrected and FIL-C rerun. No test was added merely to
make the count match.

Native O0, O2, O3, ASan/UBSan, and TSan then passed. The final script preserves
the ordering so native performance never runs before FIL-C.

## 4. Toolchain Findings

Zig 0.16.0 was downloaded from the official URL and matched the repository's
pinned SHA-256.

Two toolchain failures were retained:

1. Zig's Mach-O link rejected `-flto=thin` with `error: LTO requires using
   LLD`; adding `-fuse-ld=lld` did not change the result. Native results do not
   claim LTO.
2. A directly Zig-linked TSan binary exited 139 before test output. Reusing the
   repository's established boundary, Zig compiled the instrumented C objects
   and Apple Clang linked the platform TSan runtime. The resulting binary
   contained `___tsan_init` and passed 14/14 suites.

Zig-instrumented ASan/UBSan objects similarly use the Apple runtime plus a
version-symbol adapter. The final binary contains both ASan and UBSan symbols.

## 5. Rejected First Benchmark

The initial 10-million-transfer benchmark only synchronized on a start flag.
The producer could begin before the new consumer thread had reached the flag,
which exaggerated the startup full-ring phase. The heterogeneous M1 Pro
scheduler added visible variation.

The benchmark was changed to:

- require a consumer `ready` handshake before starting the timer;
- request the same user-interactive QoS for both threads;
- rotate variant order each sample;
- check the stop flag only every 1,024 failed polls;
- send the article's increasing integer sequence;
- calculate the expected checksum outside the timed loop.

The first measurements were discarded from the final result.

## 6. Article-Shaped Run

The final long run used 100 million transfers, 100,000 physical slots,
128-byte index separation, and five rotated samples. It reproduced one
optimization: peer-index caching improved the acquire/release median by 19.3%.

It falsified two portable interpretations of the article:

- acquire/release was 3.2% slower than seq_cst within this noisy unpinned run;
- cached atomics were 39.6% slower than the mutex baseline.

Disassembly explains why the first result is plausible on ARM64: both variants
publish with `stlr`; the main difference is `ldar` versus `ldr` for the
owner-written index. The second result remains workload/runtime-specific. The
mutex implementation may batch ownership through Darwin's mutex behavior,
while the atomic consumers execute hundreds of millions to billions of failed
polls.

## 7. Cache-Line Falsification

The host reports 128-byte cache lines. A second binary intentionally used the
common 64-byte default. Runtime address checks proved that its head and tail
shared one hardware line in every measured sample.

Nine interleaved A/B rounds showed:

- correct 128-byte separation improved cached median throughput by only 1.35%;
- it reduced seq_cst and acquire/release medians in this run;
- a prior nine-round run had shown a 22.7% cached improvement.

This rejects two simplistic rules: 64 bytes is not portable, and padding alone
is not guaranteed to win. It also demonstrates that unpinned wall time was too
unstable to quantify false sharing. The access pattern determines whether
combining two frequently read lines can offset write invalidation costs.

## 8. Fixed-Point Rejection Gate

The article mentions a power-of-two mask but its actual code uses a
once-per-lap conditional wrap branch, not `%`. No measured divide or modulo
hotspot exists in the reproduced binary. A masked-capacity rewrite would also
change accepted capacities and storage semantics. It was therefore rejected
without inventing a fixed-point optimization.

## 9. Remaining Target Work

- Re-run on a homogeneous Linux x86-64 host with two isolated physical cores.
- Pin both threads and memory, record SMT siblings and frequency policy.
- Capture `perf stat` and `perf c2c` events for cache-to-cache traffic.
- Compare empty-heavy, full-heavy, and sustained mid-occupancy workloads.
- Add batch push/pop variants and measure publication amortization.
- Test larger payloads and ownership/destructor policy before generalizing the
  scalar queue into a reusable typed library.
