---
doc_id: recallfs-source-lock-free-ring-buffer-snapshot-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260927-lock-free-ring-buffer
depends_on: []
supersedes: []
verified_by:
  - page SHA-256
  - upstream source revision
---

# Optimizing a Lock-Free Ring Buffer: Source Archive

| Field | Value |
| --- | --- |
| Article | Optimizing a Lock-Free Ring Buffer |
| URL | <https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/> |
| Author | David Alvarez Rosa |
| Published | 2026-03-24 |
| Page modified | 2026-06-26 |
| Accessed | 2026-09-27 |
| HTML SHA-256 | `ec74cc76481fc01232d457af51cad4d6f45b59cb2fe99eaf47eed9a2f5823499` |
| Upstream repository | <https://github.com/david-alvarez-rosa/CppPlayground> |
| Original implementation revision | `74ddbf92cd96d9bdb31a554cb5024fde4bb751bc` |
| Current path revision | `ffaefb7adda684d6eb7fb4ad15b0d4d95e05ebd9` |
| Upstream source SHA-256 | `6093b9b5bddcd8f10ba777fbd35359dad2f0188c85e701d413c44de3db7214f0` |
| Upstream code license | MIT |
| Article license | CC BY-NC-SA 4.0 |

## Archived Claims

The article builds a bounded single-producer/single-consumer FIFO with one
reserved slot and reports this progression on an Intel Core Ultra 5 135U:

| Version | Mechanism | Author-reported throughput |
| --- | --- | ---: |
| V2 | One mutex around each push/pop attempt | 12M ops/s |
| V3 | Cache-line-separated sequentially consistent atomics | 35M ops/s |
| V4 | Relaxed owner load, acquire peer load, release publication | 108M ops/s |
| V5 | V4 plus a cached copy of the peer index | 305M ops/s |

The upstream Google Benchmark calls `SetItemsProcessed(num_iters)`. One item is
one successful producer-to-consumer transfer, even though it requires one
successful push and one successful pop. This archive therefore calls the
reported unit `transfers/s`; calling it `ops/s` is ambiguous by a factor of two.

## Mechanisms

1. The mutex baseline serializes producer and consumer, although only the
   producer writes `head` and only the consumer writes `tail`.
2. V3 replaces the mutex with atomic indices and separates them using
   `std::hardware_destructive_interference_size`.
3. V4 weakens the producer-owned and consumer-owned index loads to relaxed,
   reads the peer index with acquire, and publishes the local index with
   release.
4. V5 reads the peer index only when the cached value says the ring may be full
   or empty. A stale cache can cause a conservative retry, but cannot grant
   access to an occupied or unpublished slot.
5. Wrap-around uses a predictable conditional branch. The article mentions a
   power-of-two mask as a possible alternative but does not implement or
   benchmark it.

## Upstream Benchmark Shape

- `T` is `int`.
- Array length is 100,000; usable capacity is 99,999.
- One producer and one consumer transfer 100,000,000 increasing integers.
- Both sides busy-spin on full/empty.
- Linux affinity pins the threads to logical CPUs 2 and 3.
- Google Benchmark reports one item per completed transfer.
- Source flags named by the article are `-O3 -march=native -ffast-math`;
  `-ffast-math` is irrelevant to this integer-only kernel.

## Archive Boundary

This file preserves metadata, claims, algorithms, and benchmark semantics
rather than copying the complete article. The local C implementation is
independent and adds explicit allocation, ownership, lock-free capability,
layout, correctness, and evidence contracts.
