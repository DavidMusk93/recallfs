---
doc_id: recallfs-runbook-gendb-tpch-sf10-reproduction-v1
kind: runbook
status: active
authority: operational
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing/benchmark
depends_on:
  - recallfs-source-gendb-generative-query-processing-v1
  - recallfs-evidence-gendb-tpch-sf10-reproduction-v1
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/benchmark/test_tpch_bench.py
  - learning/studies/20260928-gendb-generative-query-processing/benchmark/test_run_remote.sh
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/preparation-manifest.json
  - learning/studies/20260928-gendb-generative-query-processing/evidence/remote-d2-20260928/run/complete.json
---

# TPC-H SF10 Reproduction Harness

This harness compares the paper-branch GenDB artifacts with DuckDB on one
normalized TPC-H SF10 input.

## Reconciliation Anchors

| ID | Contract | Executable evidence |
| --- | --- | --- |
| `GDB-BENCH-1` | Preparation authenticates source, reference, harness, storage, binaries, runtime, CPU, and FIL-C evidence. | `prepare_remote.sh`, `run_remote.sh` |
| `GDB-BENCH-2` | A failed run attempt cannot leave a prior completion marker visible. | `test_run_remote.sh` |
| `GDB-BENCH-3` | Correctness is retained after warmup and every measured sample; bounded numeric drift never passes strict admission. | `test_tpch_bench.py`, `benchmark-results.json` |
| `GDB-BENCH-4` | Completion is published atomically only after main and ablation outputs have allowed statuses and matching digests. | `run_remote.sh`, `complete.json` |

Worked failure example: Q3 produced the exact canonical result in 8 of 11
formal checks and a one-cent alternative in three checks. The harness retained
the timing but published `complete_with_correctness_failures`, so a consumer
cannot treat timing completion as artifact admission.

## Fixed Inputs

- GenDB: `b7418071fc51aacd219964e4a68e593234db9237`
- `electrum/tpch-dbgen`: `32f1c1b92d1664dba542e927d23d86ffa57aa253`
- `dbgen` build: `CC=gcc MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH`, matching
  the pinned source's original producer configuration explicitly
- Queries: Q1, Q3, Q6, Q9, and Q18 from the GenDB benchmark
- Generated programs: author-selected best iterations from run
  `2026-02-26T06-27-28`
- Warmup/measured samples: 1/10
- Parallelism: 64 physical CPUs, fixed affinity `0-63`, NUMA memory
  interleaved across nodes 0 and 1

`prepare_remote.sh` builds GenDB's base columns, indexes, Q1 compact columns,
Q9 order-year nibble map, query binaries, and DuckDB from the same `.tbl`
files. It also builds every generated iteration used by the ablation and runs a
FIL-C correctness gate for the pinned `dbgen` source.

`prepare_remote.sh` computes a provenance key from source revisions, input
digests, compiler/engine versions, flags, all relevant generated sources,
FIL-C evidence, and CPU identity. Existing database/storage artifacts are
reused only when that key matches and their prior hashes still verify. A
successful preparation atomically publishes `evidence/preparation-key.txt` and
`evidence/preparation-manifest.json`.

The preparation manifest authenticates separate manifests for generated
binaries/storage, source, author references, and harness files, plus the
DuckDB database and FIL-C result. `run_remote.sh` verifies every digest,
revision, clean source checkout, Python interpreter, and DuckDB version before
removing stale result summaries.

`run_remote.sh` validates after warmup and after every measured sample,
alternates system order in AB/BA blocks, and immediately precedes every timed
execution with an untimed run of the same system. It emits raw samples,
position-stratified distributions, and summary statistics. It removes stale
summaries before running and publishes `run/complete.json` only after both the
main comparison and ablation finish.

The preparation schema is version 2, the main result schema is version 3, and
the ablation schema is version 2. The ablation retains correctness for every
warmup-adjacent and measured sample. A completed run may be marked
`complete_with_correctness_failures`; that state is evidence that timing
finished, not permission to promote a failing artifact.

## Timing Contracts

The primary comparison is:

```text
DuckDB: execute + fetchall + CSV serialization
GenDB:  generated program internal total, including CSV serialization
```

GenDB process wall time is retained as a sensitivity bound. DuckDB
`execute + fetchall` is also retained so the paper harness's baseline contract
can be inspected, but it is not mixed with GenDB's output-subtracted metric.

## Correctness Contract

The DuckDB result is the independent runtime oracle and is also checked against
the author's committed SF10 reference CSV. Validation requires:

- exact column names and row counts;
- exact row order;
- exact keys, labels, dates, and counts;
- matching SHA-256 after numeric values are rounded to each generated C++
  program's emitted precision for selected best binaries;
- an absolute one-cent diagnostic tolerance for legacy floating-point
  iterations in the ablation.

The main run uses `--record-correctness-failures`: a selected artifact that
differs only within the diagnostic one-cent bound remains a strict failure, but
its timing samples are retained. Structural, ordering, row-count, or larger
numeric mismatches still abort immediately. In that case `complete.json` is
published as `complete_with_correctness_failures`, never `complete`.

Q1's compact decimal-scaled integer representation has an additional contract
check over the full declared TPC-H value ranges, an overflow proof, source and
binary digests, compiler vectorization output, and final-binary disassembly.

FIL-C `0.684` is downloaded under `${ROOT}/.tmp/fil-c/` from the pinned release
archive and verified
against SHA-256
`eefb594bcbc1261a18dfa8b50041674635f53df2b5fe067915b5652adaed4e3f`.
Because the target host's glibc is too old for the release binary, compilation
and execution run inside pinned image
`debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a`.
The exercised SF0.01 output must match a native GCC build byte-for-byte and
match expected row counts. This is a correctness gate, not a benchmark.

## Remote Invocation

The repository workflow requires complex benchmark reproduction on the target
Linux host rather than on the local macOS workspace.

```text
ROOT=/data00/benchmarks/gendb-tpch-sf10-20260928
PYTHON_BIN=/root/anaconda3/bin/python3

PYTHON_BIN="$PYTHON_BIN" bash "$ROOT/harness/prepare_remote.sh" "$ROOT"
PYTHON_BIN="$PYTHON_BIN" bash "$ROOT/harness/run_remote.sh" "$ROOT"
```

The raw SF10 files, database files, binaries, and generated storage remain
under the remote `ROOT`. Only compact manifests, logs, result CSVs, and JSON
measurements belong in the study evidence directory.

## Local Contract Checks

```text
python3 test_tpch_bench.py
bash test_run_remote.sh
bash -n prepare_remote.sh run_remote.sh verify_dbgen_filc.sh
```
