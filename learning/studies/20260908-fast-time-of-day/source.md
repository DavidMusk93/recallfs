# Source

| Field | Value |
| --- | --- |
| URL | `https://www.benjoffe.com/fast-time-of-day` |
| Title | A faster way to convert a timestamp -> Hour, Min, Sec |
| Author | Ben Joffe |
| Published | 2026-09-07 |
| Access Date | 2026-09-08 |
| Archive | `learning/sources/20260908-fast-time-of-day.md` |
| Upstream Code | `benjoffe/fast-world-calendars@25f6d9345f2d681a77315cf6eaeee25b3629af8c` |
| Topic | integer division, fixed-point arithmetic, dependency chains |

## 1. Primary References

| Reference | Usage |
| --- | --- |
| [Article](https://www.benjoffe.com/fast-time-of-day) | Main claims, formulas, validity ranges, and published benchmarks |
| [Reference code](https://github.com/benjoffe/fast-world-calendars/tree/25f6d9345f2d681a77315cf6eaeee25b3629af8c/time) | Cross-check exact algorithms and upstream test methodology |
| [Faster Remainder by Direct Computation](https://arxiv.org/abs/1902.01961) | Background for interpreting multiplication low bits as fractional progress |

## 2. Reproduction Scope

The demo independently implements five scalar C variants:

1. traditional sequential decomposition;
2. V1 with ordinary unsigned division;
3. V1 with restricted-range fixed-point division;
4. V2 with fixed-point high/low products;
5. V3 with the base-64 modulo identity.

Correctness is checked exhaustively over one day for every variant and over
each restricted algorithm's full claimed range. The first value outside each
claimed range is checked as a deliberate mismatch.

Performance measurements distinguish serial dependency latency from independent
scalar throughput. SIMD, sub-second fields, leap seconds, time zones, and
calendar-date conversion are outside this reproduction.

## 3. Source Limitations

- The article was accessed successfully in a browser and through its
  prerendered HTML.
- The local HTML download had SHA-256
  `a9f207b54dea3f16d8970fe341ce5b9d1d2bcbc169ee5088c385074ca72a7c6a`.
- The full HTML is not committed; the archive preserves the technical claims
  needed for this study.
- Upstream benchmark numbers are not treated as portable. This study records
  independent target-machine measurements.
