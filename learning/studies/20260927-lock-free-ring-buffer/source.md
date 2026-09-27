---
doc_id: recallfs-source-lock-free-ring-buffer-study-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260927-lock-free-ring-buffer
depends_on:
  - recallfs-source-lock-free-ring-buffer-snapshot-v1
supersedes: []
verified_by:
  - source identity review
---

# Sources

Access date: 2026-09-27.

## Primary Source

| Field | Value |
| --- | --- |
| Article | [Optimizing a Lock-Free Ring Buffer](https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/) |
| Archive | [`../../sources/20260927-lock-free-ring-buffer.md`](../../sources/20260927-lock-free-ring-buffer.md) |
| HTML SHA-256 | `ec74cc76481fc01232d457af51cad4d6f45b59cb2fe99eaf47eed9a2f5823499` |
| Referenced code | [`CppPlayground@74ddbf9`](https://github.com/david-alvarez-rosa/CppPlayground/blob/74ddbf92cd96d9bdb31a554cb5024fde4bb751bc/DataStructures/ring_buffer.cpp) |
| Code SHA-256 | `6093b9b5bddcd8f10ba777fbd35359dad2f0188c85e701d413c44de3db7214f0` |
| License | Article CC BY-NC-SA 4.0; upstream code MIT |

The later repository revision
`ffaefb7adda684d6eb7fb4ad15b0d4d95e05ebd9` only moved the identical
248-line source to `dsa/ring_buffer.cpp`.

## Supporting References

| Reference | Used for |
| --- | --- |
| [C11 atomics, ISO/IEC 9899:2011 section 7.17](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf) | Atomic object and memory-order semantics |
| [C++ release-acquire ordering](https://en.cppreference.com/w/cpp/atomic/memory_order.html) | Explanatory happens-before model matching the article |
| [Rigtorp SPSCQueue@1053918](https://github.com/rigtorp/SPSCQueue/blob/1053918dbd251fbff69b24ef27fa5d51c29ec2af/include/rigtorp/SPSCQueue.h) | Peer-index caching and allocator-aware production implementation |
| [Linux perf c2c](https://man7.org/linux/man-pages/man1/perf-c2c.1.html) | How to validate cache-line contention on a suitable Linux target |

## Reproduction Scope

The maintained C11 implementation reproduces V2 through V5 for `uint64_t`
payloads:

- mutex;
- sequentially consistent atomic indices;
- acquire/release atomic indices;
- acquire/release plus endpoint-owned cached peer indices.

The test oracle checks every transferred sequence value independently. The
benchmark uses the article's 100,000-slot shape, but local measurements run on
an unpinned Apple M1 Pro and are not expected to reproduce Intel throughput.
The article's optional power-of-two masking idea is not implemented because
its actual code already uses a predictable wrap branch and no profile
identified integer remainder as the bottleneck.
