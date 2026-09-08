# Fast Time-of-Day Article Archive

| Field | Value |
| --- | --- |
| URL | `https://www.benjoffe.com/fast-time-of-day` |
| Title | A faster way to convert a timestamp -> Hour, Min, Sec |
| Author | Ben Joffe |
| Published | 2026-09-07 |
| Access Date | 2026-09-08 |
| Page SHA-256 | `a9f207b54dea3f16d8970fe341ce5b9d1d2bcbc169ee5088c385074ca72a7c6a` |
| Reference Repository | `https://github.com/benjoffe/fast-world-calendars` |
| Reference Revision | `25f6d9345f2d681a77315cf6eaeee25b3629af8c` |

## 1. Archived Summary

The article studies conversion of an already normalized day-second value
`time` in `[0, 86399]` into hour, minute, and second. It does not study clock
acquisition, time zones, calendar dates, or parsing.

Its primary claim is structural: the usual decomposition serializes the work
through `hour -> remainder -> minute -> second`, while the following
rearrangement creates two independent chains:

```text
total_minutes = time / 60
hour          = time / 3600
second        = time - total_minutes * 60
minute        = total_minutes - hour * 60
```

The two divisions can begin together. Once they finish, the two remainder
calculations are independent as well. The article calls this version V1.

V2 replaces division by restricted-range fixed-point multiplication and uses
the high and low halves of products. V3 replaces modulo 60 with a modulo 64
identity so that the remainder path uses shifts, additions, and a mask.

## 2. Algorithms and Claimed Domains

| Algorithm | Main idea | Claimed inclusive input range |
| --- | --- | --- |
| Traditional 1 | Divide by 3600, then decompose the remainder | Full unsigned range |
| V1 division | Compute total minutes and hours independently | Full unsigned range |
| V1 fixed-point | Replace both constant divisions with multiply-high | `[0, 2257198]` |
| V2 hi/low bits | Recover quotient and fractional progress from product halves | `[0, 2255818]` |
| V3 base-64 | Use `(x + 4 * floor(x / 60)) mod 64` for modulo 60 | `[0, 2257198]` |

All restricted variants cover the complete day-second domain. Their constants
are:

```text
floor(2^32 / 60)   + 1 = 71582789
floor(2^32 / 3600) + 1 = 1193047
```

The `+1` introduces an approximation error outside the stated domain. These
functions are therefore not drop-in full-range replacements unless callers
enforce the input contract.

## 3. Latency Versus Throughput

The article separates two performance questions:

- Latency: one conversion feeds the next, exposing the longest dependency
  chain.
- Throughput: many independent conversions let an out-of-order CPU overlap
  work.

V3 has the shortest modeled scalar dependency chain, but performs more total
operations than V2. The article therefore recommends benchmarking both rather
than treating one as universally faster.

Representative article results, normalized to Cassio Neri's implementation:

| Platform and compiler | Mode | V1 division | V2 32-bit | V3 32-bit |
| --- | --- | ---: | ---: | ---: |
| Apple M4 Pro, Apple Clang 17 | Latency | `0.65x` | `0.74x` | `0.70x` |
| Apple M4 Pro, Apple Clang 17 | Throughput | `0.71x` | `0.65x` | `0.97x` |
| AMD Ryzen 9 9950X3D, Clang 18 | Latency | `0.61x` | `0.62x` | `0.69x` |
| AMD Ryzen 9 9950X3D, Clang 18 | Throughput | `0.89x` | `0.72x` | `0.96x` |

These are source claims, not results reproduced by this archive.

## 4. Important Boundaries

- The input is elapsed seconds within a day, not a Unix timestamp.
- Unix time does not encode leap seconds. A civil-time value `23:59:60`
  requires an explicit special case or a separate contract.
- Fixed-point constants and the best instruction sequence depend on input
  width, compiler, target ISA, and optimization mode.
- Scalar latency, scalar throughput, and SIMD throughput can select different
  winners.
- The article's cycle tables are a dependency model, not hardware-counter
  measurements.

## 5. Archive Boundary

This archive preserves metadata, formulas, claims, validity ranges, and
representative measurements rather than copying the complete article. The
study contains an independent C reproduction and records its own correctness,
assembly, and benchmark evidence.
