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
| CMake/Ninja | 4.4.3 / 1.13.2; Zig and FIL-C CTest both 2/2 pass |
| clang-format | 23.1.1; all four C/C header sources pass |
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
| mutex | 30.895M | 61.790M | baseline |
| seq_cst | 16.447M | 32.895M | -46.76% |
| acquire/release | 15.651M | 31.303M | -4.84% |
| cached | 18.670M | 37.340M | +19.29% |

The local ranking contradicts the article: the mutex baseline is 65.5% faster
than the cached atomic variant. This does not establish that mutexes are
generally faster. The unpinned heterogeneous scheduler, Darwin mutex behavior,
busy-poll imbalance, `uint64_t` payload, C ABI calls, and different ISA/runtime
all differ from the author's pinned Intel/C++ setup.

Retry counts explain why this run is not a clean cache-coherence experiment.
Across five samples, acquire/release performed about 6.10 billion failed
consumer polls and cached performed about 3.10 billion. The mutex version had
about 23.8 million failed consumer polls but roughly 6.07 million involuntary
context switches. The benchmark measures the complete busy-wait workload, not
isolated queue-operation latency.

## Cache-Line A/B

Raw data: [`alignment-ab.csv`](alignment-ab.csv).

Nine paired rounds alternated which binary ran first. Each point transfers
10,000,000 values. Both binaries are identical except for
`SPSC_CACHE_LINE_SIZE`.

| Variant | 128-byte median | 64-byte median | 128 vs 64 |
| --- | ---: | ---: | ---: |
| mutex | 30.626M | 30.832M | -0.67% |
| seq_cst | 16.070M | 18.567M | -13.45% |
| acquire/release | 15.582M | 16.488M | -5.50% |
| cached | 19.041M | 18.786M | +1.35% |

For every 64-byte sample, runtime address inspection proved that `head` and
`tail` occupied the same 128-byte hardware cache line. Every 128-byte sample
placed them on different lines. This run's cached delta was only +1.35%; a
previous nine-round run observed +22.7%. The large drift proves that unpinned
wall time cannot quantify the false-sharing cost. The table is evidence of
layout and unstable correlation, not a cache-to-cache transfer count.

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
3294a1293deb35890d19d7c83942e845cc028216f835260493adce97aff4d3e7  demo/include/spsc_ring.h
c619cd1d5c673e4ef215b492ae54b61e4ee70119dac7b168ad627226d075b7cd  demo/src/spsc_ring.c
605145b9e4e705e08247c109bb0fd9c2f92becd3a01f88e74995e13bd5b90236  demo/src/benchmark.c
bcea9bdc2de5c1b2c16394561277439ec4c2f1b6198c63f63d83b67eec958023  demo/tests/spsc_ring_test.c
385230584ce09580d5984d7f75d0abb2450ade1303ae4f2b6bb6eff3630facfc  article-shape benchmark binary
e747be136fb496f64537f6cf1d88699ff76fd012c50c4fae4b9634ab06510a7d  article-shape benchmark CSV
```

The source digests cover the article-shaped run. The `verify.sh` script was
added afterward and does not affect the benchmark binary.
