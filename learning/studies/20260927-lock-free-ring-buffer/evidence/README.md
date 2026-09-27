---
doc_id: recallfs-evidence-lock-free-ring-buffer-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260927-lock-free-ring-buffer
depends_on:
  - recallfs-study-lock-free-ring-buffer-v1
supersedes: []
verified_by:
  - SPSC-RA-1
  - SPSC-RA-2
  - SPSC-RA-3
  - SPSC-RA-4
  - SPSC-RA-5
---

# Evidence Ledger

## Environment

| Field | Value |
| --- | --- |
| Host | Apple M1 Pro, 8 performance + 2 efficiency cores |
| OS | macOS 26.5.2, Darwin 25.5.0, ARM64 |
| Hardware cache line | 128 bytes from `sysctl hw.cachelinesize` |
| Scheduling | Both threads request `QOS_CLASS_USER_INTERACTIVE`; not CPU-pinned |
| Native frontend | Zig 0.16.0, Clang 21.1.0 |
| Native flags | `-O3 -march=native -mtune=native -DNDEBUG -g` |
| LTO | Not used; Zig's Mach-O link rejected `-flto=thin` |
| FIL-C | 0.684, Clang 20.1.8, Linux ARM64 guest |
| PMU | Unavailable; `powermetrics` requires superuser |

The host has no supported hard CPU-affinity or NUMA binding path in this
harness. P/E-core placement and migration remain uncontrolled. Therefore these
measurements are local wall-time evidence, not a portable performance claim.

## Correctness

[`verification.txt`](verification.txt) records:

| Gate | Result |
| --- | --- |
| FIL-C test | 14/14 suites passed |
| FIL-C benchmark path | 4/4 variants passed self-check |
| Zig O0/O2/O3 | 14/14 suites passed in every profile |
| ASan/UBSan | 14/14 suites passed; instrumentation symbols and runtime present |
| ThreadSanitizer | 14/14 suites passed; instrumentation symbol and runtime present |

The tests cover invalid capacities and overflow, one reserved slot, full and
empty returns, repeated wrap-around, capacities 2 and 17, every implementation,
one million or more concurrent transfers per implementation, FIFO values, and
checksums. FIL-C covers exercised memory-safety and UB paths; TSan adds dynamic
race evidence but neither is a proof for every schedule.

## Article-Shaped Benchmark

Raw data: [`article-shape-benchmark.csv`](article-shape-benchmark.csv).

- 100,000 physical slots, 99,999 usable;
- 100,000,000 increasing `uint64_t` values per sample;
- five samples with rotated variant order;
- allocation and thread creation outside the timer;
- consumer ready handshake before the timer;
- every value validated and checksum retained under `-DNDEBUG`.

| Variant | Median transfers/s | Successful API calls/s | Delta from previous |
| --- | ---: | ---: | ---: |
| mutex | 29.913M | 59.825M | baseline |
| seq_cst | 15.061M | 30.123M | -49.65% |
| acquire/release | 14.576M | 29.152M | -3.22% |
| cached | 21.690M | 43.379M | +48.81% |

The local ranking contradicts the article: the mutex baseline is 37.9% faster
than the cached atomic variant. This does not establish that mutexes are
generally faster. The unpinned heterogeneous scheduler, Darwin mutex behavior,
busy-poll imbalance, `uint64_t` payload, C ABI calls, and different ISA/runtime
all differ from the author's pinned Intel/C++ setup.

Retry counts explain why this run is not a clean cache-coherence experiment.
Across five samples, acquire/release performed about 5.14 billion failed
consumer polls and cached performed about 1.82 billion. The mutex version had
about 19.3 million failed consumer polls but roughly 4.02 million involuntary
context switches. The benchmark measures the complete busy-wait workload, not
isolated queue-operation latency.

## Cache-Line A/B

Raw data: [`alignment-ab.csv`](alignment-ab.csv).

Nine paired rounds alternated which binary ran first. Each point transfers
10,000,000 values. Both binaries are identical except for
`SPSC_CACHE_LINE_SIZE`.

| Variant | 128-byte median | 64-byte median | 128 vs 64 |
| --- | ---: | ---: | ---: |
| mutex | 31.601M | 31.540M | +0.19% |
| seq_cst | 16.009M | 18.813M | -14.90% |
| acquire/release | 14.250M | 14.762M | -3.47% |
| cached | 24.110M | 19.648M | +22.71% |

For every 64-byte sample, runtime address inspection proved that `head` and
`tail` occupied the same 128-byte hardware cache line. Every 128-byte sample
placed them on different lines. Correct separation materially helped the
cached variant, but did not improve every access pattern. Without PMU cache
events, the table is evidence of layout plus wall-time correlation, not a
measured cache-to-cache transfer count.

## Code Generation

[`codegen.txt`](codegen.txt) preserves selected disassembly from the exact
article-shaped benchmark binary.

| Variant | Owner index | Peer index | Publication |
| --- | --- | --- | --- |
| seq_cst | `ldar` | `ldar` | `stlr` |
| acquire/release | `ldr` | `ldapr`/`ldapur` | `stlr` |
| cached | `ldr` | conditional `ldapr`/`ldapur` | `stlr` |

On this ARM64 target, seq_cst and release stores compile to the same `stlr`.
Weakening the memory order only changes owner loads and does not reproduce the
large x86 result reported by the article. Cached peer-index access changes the
operation count and coherence access frequency more substantially.

## Allocation And Working Set

| Dimension | Recorded workload |
| --- | --- |
| Value | Increasing `uint64_t`, validated exactly |
| Size | 800,000-byte slot allocation plus 384-byte aligned control object |
| Lifetime | One allocation pair per sample, entirely outside timed region |
| Access | Sequential circular slot access |
| Concurrency | One producer, one consumer, busy retry |
| Topology | Unpinned macOS threads with performance QoS |

The reproduction removes hot-path allocation. It does not measure allocator
latency, RSS, page placement, huge pages, or NUMA because those are not the
tested mechanism.

## Digests

```text
bdddaf7fd8ac9661f2c4daa342366b2f7d39ca731fee5fe2d3b4f79cca5ca201  demo/include/spsc_ring.h
3599b3cfc2bf5b0166962584dfe29114fdf9524864116d358f09b0b1fdb26b33  demo/src/spsc_ring.c
9f8f51382d1ba20341c663d150f0ec4ecaead223a84b823ece8952d645f3f610  demo/src/benchmark.c
8d0a436b6e31de57aabbe737e8a902c3c16567df2b9b905f54c6520876986ed8  demo/tests/spsc_ring_test.c
627e4af4779947961e70fc8516f3a4dd29e328c60dbbd5d92cf00879ff8acdab  article-shape benchmark binary
dc3f0106b7b48b68cef9652dcdf745c16ace01e1e624dbc311245ab784549be5  article-shape benchmark CSV
```

The source digests cover the article-shaped run. The `verify.sh` script was
added afterward and does not affect the benchmark binary.
