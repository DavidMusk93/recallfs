---
doc_id: recallfs-evidence-gendb-tpch-sf10-reproduction-v1
kind: evidence
status: active
authority: evidence
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing
depends_on:
  - recallfs-source-gendb-generative-query-processing-v1
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/run/benchmark-results.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/run/ablation-results.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/run/complete.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/input-sha256.txt
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/preparation-manifest.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/filc-dbgen-smoke.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/q1-numeric-contract.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/q3-stability-probe.txt
  - learning/studies/20260928-gendb-generative-query-processing/benchmark/tpch_bench.py
---

# GenDB vs DuckDB: TPC-H SF10 Reproduction

## 1. Verdict

The reproduction supports a narrower claim than the paper:

> On a second 64-core machine, the author-selected GenDB artifacts beat DuckDB
> on Q9 and Q18, but lose on Q1, Q3, and Q6. The diagnostic five-query timing
> aggregate improves by `1.33x`, not the paper's `2.77x`; Q3 fails strict
> correctness admission because its parallel floating-point reduction is not
> output-deterministic at a half-cent boundary.

Q1, Q6, Q9, and Q18 produced the same canonical result SHA-256 as both DuckDB
and the author's committed SF10 reference in all 11 checks. Q3 matched exactly
in 8 of 11 checks and differed by one cent in 3. A separate 30-run probe
observed the same two outputs 22 and 8 times. The performance measurements are
retained for diagnosis, but the run manifest is
`complete_with_correctness_failures` and Q3 is not an admissible artifact.

The dominant benefit is not generic code generation. It is the ability to
materialize a repeated query into a query-shaped data path:

- replace a large hash aggregation with a sequential grouped-index scan;
- replace generic joins with cache-sized direct lookup structures;
- persist derived attributes that remove a table scan;
- narrow columns to their actual value domain;
- fuse filtering, joining, aggregation, and top-k work.

These choices are hardware- and workload-sensitive. A generated artifact that
wins on the generation host can lose to DuckDB elsewhere. Production use
therefore requires per-query benchmarking and fallback.

## 2. Reproduction Contract

| Dimension | Value |
| --- | --- |
| Host | `d2` / `n37-125-152`, Linux 5.15 KVM |
| CPU | 2 sockets, 32 physical cores/socket, 64 CPUs, no SMT |
| CPU model | Intel Xeon Platinum 8457C |
| Cache | 48 KiB L1d/core, 2 MiB L2/core, 99,840 KiB aggregate L3 |
| Memory | 251 GiB |
| GenDB | paper commit `b7418071fc51aacd219964e4a68e593234db9237` |
| dbgen | `32f1c1b92d1664dba542e927d23d86ffa57aa253` |
| DuckDB | Python/engine `1.2.1` |
| Compiler | GCC `8.3.0` |
| Query flags | `-O3 -march=native -mtune=native -DNDEBUG -flto -std=c++17 -Wall -fopenmp -DGENDB_PROFILE -lstdc++fs` |
| Placement | CPUs `0-63`; memory interleaved over NUMA nodes 0 and 1 |
| Samples | 1 warmup + 10 measured hot runs per query |
| Scheduling | Counterbalanced AB/BA; every timed execution immediately follows an untimed run of the same system |

The source revision, generated input, build flags, binaries, storage files,
reference outputs, harness, Python path, DuckDB version, and FIL-C result are
content-addressed and cross-linked by `preparation-manifest.json` in
[`remote-d2-20260928`](remote-d2-20260928/).

Before SF10 reuse, `dbgen` was also compiled and executed under FIL-C `0.684`
inside pinned Debian image
`debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a`.
FIL-C and native GCC used the same explicit
`MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH` configuration as the pinned
source's SF10 producer. A heap-use-after-free negative control failed under
FIL-C, and its SF0.01 output matched the native build byte-for-byte for all
eight tables and matched expected row counts. This validates the exercised
generator path for memory safety and undefined behavior detection; it is not
a proof of all generator paths and is not performance evidence.

## 3. Timing Contract

The primary metric compares complete result production without external
process startup:

```text
DuckDB = execute + fetchall + CSV serialization
GenDB  = generated program internal "total", including CSV serialization
```

This is more symmetric than the upstream paper harness, which subtracts the
GenDB `output` phase while timing DuckDB through `fetchall()`. GenDB process
wall time is reported separately because invoking one executable per query
adds measurable startup cost.

Both systems use the same 64-CPU affinity and NUMA policy. The complete data
fits in memory. This is a hot-cache, single-client throughput comparison, not a
cold-cache, concurrent, or tail-latency benchmark.

Each measured execution immediately follows an untimed execution of the same
system and query. The system order alternates between DuckDB-then-GenDB and
GenDB-then-DuckDB. This controls the previously observed second-position
advantage better than bare alternation, while the position-stratified medians
below still expose residual host noise.

With ten observations, nearest-rank p95 is the maximum sample. It is retained
as a noise signal, not treated as a stable production-tail estimate.

## 4. Measured Results

All values are milliseconds. Speedup is `DuckDB / GenDB`; values below `1.0x`
mean GenDB is slower.

| Query | DuckDB median | DuckDB p95 | GenDB median | GenDB p95 | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q1 | 39.76 | 41.87 | 47.81 | 53.75 | 0.83x |
| Q3 | 65.13 | 73.70 | 123.74 | 146.03 | 0.53x |
| Q6 | 17.65 | 21.58 | 31.08 | 36.12 | 0.57x |
| Q9 | 290.08 | 310.15 | 147.35 | 162.58 | 1.97x |
| Q18 | 146.64 | 156.88 | 70.49 | 82.84 | 2.08x |
| **Five-query sum** | **559.27** | - | **420.47** | - | **1.33x** |

Including GenDB process launch raises its sum of medians to `492.08 ms`, so the
aggregate advantage falls to `1.14x`.

A guarded dispatcher that uses GenDB only for Q9 and Q18, and DuckDB for the
three regressions, would produce a `340.38 ms` sum of medians, or `1.64x`
against all-DuckDB. Including GenDB process launch gives `381.07 ms`, or
`1.47x`. Q3 is on the DuckDB path for both performance and correctness. This is
measured support for per-artifact admission and fallback.

### 4.1 Residual execution-position sensitivity

The table reports median milliseconds for position 1 / position 2 after
self-preconditioning:

| Query | DuckDB | GenDB internal |
| --- | ---: | ---: |
| Q1 | 39.93 / 39.52 | 43.20 / 48.04 |
| Q3 | 64.93 / 65.82 | 135.95 / 101.93 |
| Q6 | 17.77 / 17.63 | 29.08 / 31.52 |
| Q9 | 288.18 / 291.77 | 153.17 / 136.20 |
| Q18 | 148.64 / 146.20 | 80.49 / 64.94 |

DuckDB is position-stable in this run. GenDB retains material per-position
variation, especially on short Q1/Q6 executions, despite self-preconditioning.
Q9 and Q18 remain GenDB wins in both strata; Q1, Q3, and Q6 remain losses.
Small GenDB differences should not be interpreted as isolated causal effects
without more blocks, frequency controls, and usable PMU counters.

## 5. Correctness

Validation ran after warmup and after every measured sample. It compared
ordered rows and exact headers. Keys, labels, dates, and counts were exact.
Numeric output was canonicalized to the precision emitted by each generated
program, then required an exact SHA-256 match for selected artifacts.

| Query | Rows | Exact checks | Bounded-only checks | Admission |
| --- | ---: | ---: | ---: | --- |
| Q1 | 4 | 11/11 | 0 | Pass |
| Q3 | 10 | 8/11 | 3 | **Fail** |
| Q6 | 1 | 11/11 | 0 | Pass |
| Q9 | 175 | 11/11 | 0 | Pass |
| Q18 | 100 | 11/11 | 0 | Pass |

Q3's second row has exact revenue `439855.3250`. The generated program emits
two decimals and alternates between `439855.33` and `439855.32` as OpenMP
floating-point reduction order changes. The strict canonical hashes are
`43b200...d9366c` and `35515e...3e621`. Three formal checks emitted `.32` and
preserve row 2, column `revenue`, expected `.33`, and actual `.32` directly
in its mismatch evidence. A separate 30-run probe observed the first output
22 times and the second 8 times. This is bounded to one cent and does
not affect row order, but it is a real reproducibility failure, not a passing
correctness result.

The record-only benchmark mode retains timings only when all structural fields
match and numeric differences remain within one cent; the failure remains
visible in both `benchmark-results.json` and `complete.json`. Legacy ablation
iterations use the same one-cent diagnostic tolerance because several use
floating-point reduction.

## 6. Paper Comparison

The paper numbers are three-run averages on a different dual-socket machine.
The reproduction values are ten-run medians under the normalized timing
contract, so the comparison diagnoses portability rather than claiming an
identical experiment.

| Query | Paper DuckDB | Paper GenDB | Paper speedup | `d2` DuckDB | `d2` GenDB | `d2` speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Q1 | 83.83 | 40.12 | 2.09x | 39.76 | 47.81 | 0.83x |
| Q3 | 82.97 | 54.78 | 1.51x | 65.13 | 123.74 | 0.53x |
| Q6 | 19.33 | 13.73 | 1.41x | 17.65 | 31.08 | 0.57x |
| Q9 | 231.37 | 38.02 | 6.08x | 290.08 | 147.35 | 1.97x |
| Q18 | 176.76 | 67.64 | 2.61x | 146.64 | 70.49 | 2.08x |
| **Sum** | **594.27** | **214.28** | **2.77x** | **559.27** | **420.47** | **1.33x** |

DuckDB's aggregate changed by only `-5.9%` relative to the paper result.
GenDB's aggregate increased by `96.2%`. The failure to reproduce `2.77x` is
therefore primarily a portability failure of the specialized artifacts, not a
grossly slower DuckDB baseline.

Likely contributors are the different CPU/cache/NUMA topology, GCC 8 code
generation, standalone process and OpenMP startup, and fixed data-structure
sizes selected for the paper host. The experiment does not isolate these
factors, so they remain hypotheses rather than causal claims.

## 7. Feedback-Loop Ablation

The same host reran every valid generated iteration with the same correctness
oracle and 1+10 protocol.

| Query | Iteration 0 | Selected best | Improvement | Main change |
| --- | ---: | ---: | ---: | --- |
| Q1 | 83.22 | 47.02 | 1.77x | Compact integer columns and integer accumulation |
| Q3 | 160.45 | 126.22 | 1.27x | Random index nested loops to sequential scans plus compact hash state |
| Q6 | 26.50 | 26.50 | 1.00x | No optimization iteration |
| Q9 | 146.61 | 94.82 | 1.55x | Persistent order-year nibble map plus compact measures |
| Q18 | 8682.36 | 69.56 | 124.81x | Per-thread hash aggregation to grouped scan of a sorted index |

Q18 is the clearest result. The first candidate allocated hundreds of
megabytes of per-thread hash state and created millions of heap nodes. Runtime
feedback exposed that failure. The next iteration reused the prebuilt sorted
`lineitem_orderkey` index and turned the group-by into a sequential scan.

Q1 also records a rejected direction: iteration 1 regressed from `83.22 ms` to
`85.52 ms`. Small deltas among intermediate iterations remain vulnerable to
host noise; Q18 is the robust signal that candidates must be benchmarked and
selected rather than assumed to improve monotonically.

The ablation's overall status is `bounded_numeric_failures_recorded`, not
`pass`: Q1 iterations 0-2 and Q3 iteration 1 produced at least one
bounded-only sample. The selected Q1, Q6, Q9, and Q18 variants were exact in
all 11 checks; Q3 remains rejected by the stricter main-run admission policy.

## 8. Where the Benefit Comes From

### 8.1 Persistent query-shaped state

This is the strongest observed mechanism.

- Q18 reuses a 480 MB sorted foreign-key index to eliminate general hash
  aggregation.
- Q9 persists a 30 MB nibble-packed `orderkey -> year` map, eliminating a
  15-million-row orders scan and a 57 MB runtime array initialization.

Both winning queries turn repeated generic work into a one-time storage build.
That shifts cost from query latency to artifact build, storage, invalidation,
and recovery.

### 8.2 Narrow representations

Q1 converts quantity, discount, and tax from 8-byte doubles to 1-byte integers,
and price to 4-byte cents. Q9 reuses three of those columns. The four Q1
versions occupy about 420 MB instead of about 1.92 GB.

This improved Q1 by `1.77x` relative to its first generated iteration, but Q1
still lost to DuckDB on `d2`. Narrow layout is useful; it is not sufficient to
guarantee that a complete specialized query beats a mature vectorized engine.

The compact representation is a decimal-scaled integer transformation, not an
unqualified floating-point shortcut:

- quantity remains integral; price is stored as cents in `int32_t`; discount
  and tax are stored as hundredths in `uint8_t`;
- the builder conversion is `int(binary64_value * scale + 0.5)`, with
  round-to-nearest semantics only for the declared nonnegative TPC-H domain;
- exhaustive verification covered every integer point in the declared domains,
  including all 10,404,860 possible price-cent values;
- products and accumulators widen to signed `int64_t`; the conservative
  `sum_charge` bound is `6,799,146,657,523,920,000`, leaving
  `2,424,225,379,330,855,807` below `INT64_MAX`;
- the upstream builder does not enforce these ranges, so a production ingest
  path must reject or fall back outside the contract.

The source and final binary are content-addressed in
`q1-source-binary-sha256.txt`. GCC's report confirms vectorization of helper
and merge basic blocks, but the final OpenMP worker disassembly shows scalar
integer `imul/add` in the row loop. The measured `iter_2 -> iter_3` change is a
bundled `1.62x` improvement (`76.35 -> 47.02 ms`); this experiment does not
isolate a SIMD or arithmetic-only speedup.

### 8.3 Cache-shaped lookup and fused execution

The generated plans use direct arrays for bounded keys, tiny fixed aggregation
arrays for low-cardinality groups, and sequential scans that combine filter,
join, aggregate, and top-k logic. This removes generic operator boundaries and
intermediate materialization.

Q3 shows the limit. Its paper-host rewrite avoids large random index probes,
but the fixed hash/atomic design was slower than DuckDB on `d2`. Cache-fitting
claims must be tied to a hardware class and rebenchmarked on admission.

### 8.4 The LLM is not the runtime advantage

No model runs in the measured path. The runtime advantage comes from ordinary
native code and precomputed data structures. The LLM's role is to search a
larger implementation space and use runtime feedback to reject poor
candidates.

This distinction determines the product architecture: keep generation
offline, keep execution deterministic, and promote only measured winners.

## 9. Storage and Amortization

| Artifact | Bytes |
| --- | ---: |
| Raw `.tbl` input | 11,232,136,268 |
| DuckDB database | 5,735,460,864 |
| GenDB base columns | 10,352,198,187 |
| GenDB general indexes | 1,061,228,328 |
| GenDB query-derived columns | 449,902,366 |
| GenDB total | 11,863,328,881 |

The GenDB representation is about `2.07x` the DuckDB database size. General
indexes add about `10.3%` over GenDB base columns and the two query-specific
derived layouts add another `4.3%`.

Using the paper's `90.9 min` TPC-H generation time and the reproduced
`138.81 ms` diagnostic aggregate saving requires about `39,300` five-query
cycles to amortize generation wall time alone. Using actual GenDB process wall
raises the threshold to about `81,200` cycles. Storage build, verification, deployment,
and invalidation costs are still excluded.

## 10. Evidence Limits

- This is one virtualized host, one SF10 dataset, five fixed queries, one
  client, and hot page cache.
- The paper did not pin DuckDB or compiler versions. This run used DuckDB
  `1.2.1` and GCC `8.3.0`; version sensitivity is unresolved.
- GenDB's internal timer excludes process startup. The secondary wall metric
  includes it, but DuckDB remains an in-process persistent connection.
- The counterbalanced self-preconditioned protocol removed the known
  systematic second-run advantage, but GenDB's position-stratified medians
  still vary materially on short queries.
- Hardware PMU events were unavailable in the VM. `perf stat` exposed
  `task-clock`, while cycles, instructions, and cache misses were unsupported.
- No cold-cache, concurrency, update, crash, schema-change, or data-drift test
  was run.
- The ablation compares generated iterations that often change several things
  together. It identifies bundled mechanisms, not isolated causal effect sizes.
- Q3 failed strict output determinism. Its timing remains diagnostic evidence,
  but the artifact is not eligible for promotion without deterministic decimal
  accumulation or a product-approved approximate numeric contract.
- The author-generated programs were reused. This reproduces execution and
  validation, not the 90.9-minute LLM generation process.
