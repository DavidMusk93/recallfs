---
doc_id: recallfs-exploration-gendb-generative-query-processing-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing
depends_on:
  - recallfs-source-gendb-generative-query-processing-v1
  - recallfs-evidence-gendb-implementation-audit-v1
  - recallfs-evidence-gendb-tpch-sf10-reproduction-v1
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/evidence/source-digests.txt
---

# Exploration Log

## 1. Scope Decision

The request was interpreted as two coupled goals:

1. learn the GenDB paper and implementation;
2. determine how to apply the mechanism to the user's database product.

Repository and nmem context identify Tide/stream_engine as the relevant product
domain. The study therefore maps GenDB to the existing Catalyst, Velox,
FringeDB, and LLVM JIT architecture.

No executable demo was created. A toy SQL-to-C++ generator would repeat the
paper's least production-ready layer without testing the real Tide ABI,
snapshots, or workloads. The smallest useful artifact for this request is an
evidence-backed adoption design with explicit production gates.

## 2. Source Acquisition

Commands:

```text
curl -fL --retry 3 --connect-timeout 20 \
  https://arxiv.org/pdf/2603.02081 \
  -o .tmp/data/gendb-v1.pdf

pdfinfo .tmp/data/gendb-v1.pdf
pdftotext -layout .tmp/data/gendb-v1.pdf .tmp/data/gendb-v1.txt

git clone --filter=blob:none \
  https://github.com/SolidLao/GenDB.git \
  .tmp/research/GenDB
```

Browser automation opened the arXiv abstract and GitHub repository. The
rendered pages confirmed the source identity and the repository's statement
that `arxiv-03-02-2026` is the stable paper branch.

## 3. Paper Extraction

The paper was read in five passes:

1. problem statement and relation to compiled query processing;
2. five-agent system flow and structured handoffs;
3. experiment setup, workload, hardware, and baseline policy;
4. reported performance and ablation;
5. limitations and research agenda.

The main extracted facts were:

- one generated executable per concrete query in the paper version;
- binary columnar storage and query-specific indexes;
- runtime comparison against a traditional database result;
- serialized execution measurement and parallel LLM work;
- a 300-second query timeout and 30-minute agent timeout;
- explicit silent failures from hash table non-termination and agent token
  exhaustion;
- no formal correctness guarantee;
- hot, in-memory OLAP evaluation on one server.

## 4. Implementation Inspection

The paper branch and current `main` were compared. Relevant files:

```text
src/gendb/orchestrator.mjs
src/gendb/gendb.config.mjs
src/gendb/shared.mjs
src/gendb/providers/claude.mjs
src/gendb/providers/codex.mjs
src/gendb/tools/compare_results.py
src/gendb/tools/sql-parser.py
src/gendb/tools/template-extractor.mjs
src/gendb/utils/paths.mjs
src/gendb/utils/hardware.mjs
benchmarks/lib/gendb.py
benchmarks/lib/runners.py
```

Static checks:

```text
node --check src/gendb/orchestrator.mjs
node --check src/gendb/shared.mjs
node --check src/gendb/tools/template-extractor.mjs
python3 -m py_compile src/gendb/tools/compare_results.py
python3 -m py_compile src/gendb/tools/sql-parser.py
python3 -m py_compile benchmarks/benchmark.py
```

Result: all passed.

No dedicated tests, CI workflow, dependency lockfile, or release tags were
found. `npm test` invokes the orchestrator rather than a test suite.

## 5. Critical Control-Plane Findings

### 5.1 Promotion can accept a wrong result

Both inspected revisions initialize the first best candidate from:

```text
execution_results.run.status == "pass"
```

instead of:

```text
execution_results.validation.status == "pass"
```

Current `main` subsequently promotes every completed result and updates the
`best` symlink even if `meta.json` records `validated: false`.

### 5.2 Query reuse can ignore changed SQL

Current `inspectWorkloadState()` checks for:

- a query-ID directory;
- an existing `template.sql`;
- a `best` symlink;
- a matching hardware fingerprint.

It does not compare the current SQL/template digest with stored content before
returning `skip`.

### 5.3 Runtime trust is too broad

The Claude provider bypasses permission checks; the Codex provider uses
danger-full-access without approval. Generated native binaries run directly
on the host. No process sandbox or resource containment was found.

### 5.4 Benchmark accounting is asymmetric

GenDB benchmark parsing subtracts generated program output time from total
time. Baseline runners time query execution plus result fetch into the client.
This is recorded as a methodological limitation, not asserted to invalidate
the reported ranking.

## 6. Product Mapping

The following local documents were reviewed:

- `projects/stream_engine/docs/jit/json_encoder_llvm_jit_plan.md`;
- `projects/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md`;
- `projects/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md`;
- `projects/stream_engine/docs/fringedb-dict-ingest-plan.md`.

The main product-specific finding is that Tide already has the correct landing
zone for this idea: a plan-based JIT subsystem with an existing baseline and
fallback. The first GenDB-style experiment should search within a typed
`EncoderPlan`, not generate an entire replacement database executable.

The negative optimization record is especially important. It demonstrates that
code generation can lose to existing hand-tuned code when it changes
column-major execution into row-major execution or leaves host callbacks in the
hot loop. This became a required acceptance principle in the final design.

## 7. Independent TPC-H Reproduction

The paper branch was checked out at
`b7418071fc51aacd219964e4a68e593234db9237` on remote host `d2`. TPC-H SF10
was generated from `electrum/tpch-dbgen`
`32f1c1b92d1664dba542e927d23d86ffa57aa253`. One normalized set of `.tbl`
files fed both systems.

GenDB storage was rebuilt rather than downloaded:

1. base binary columns from the generated ingest program;
2. zone maps, primary-key hashes, composite hashes, and sorted foreign-key
   indexes from the generated index builder;
3. Q1's compact integer measure columns from `build_ext_compact.cpp`;
4. Q9's nibble-packed order-year mapping from
   `build_ext_oky_nibble.cpp`.

The five author-selected best query sources and all valid optimization
iterations were compiled with native optimization and OpenMP. GCC 8 required
explicit `-lstdc++fs` for generated uses of `std::filesystem`.

DuckDB `1.2.1` loaded the same files with the paper harness's schema and
foreign-key stripping behavior. Both systems ran with 64 physical CPUs,
affinity `0-63`, and memory interleaved over both NUMA nodes.

The harness:

- performs one warmup and ten formal runs;
- alternates DuckDB/GenDB order by sample and immediately precedes every timed
  execution with an untimed run of the same system;
- records DuckDB query/fetch, serialization, and end-to-end times separately;
- records GenDB internal phase timings and external process wall time;
- compares exact headers, ordered rows, structural values, decimal values, and
  canonical SHA-256 against both DuckDB and the author's committed SF10 result;
- records bounded one-cent failures without admitting the candidate, so one
  unstable result cannot erase the remaining timing evidence;
- reruns every generated iteration to measure the value of runtime feedback.

Before the SF10 artifacts were reused, the pinned `dbgen` source was compiled
and executed under FIL-C `0.684` in a pinned Debian container. Its SF0.01
outputs matched a native GCC oracle byte-for-byte and matched expected row
counts. Structured preparation metadata authenticates source, reference,
harness, binary, storage, DuckDB, and FIL-C evidence before a run may start.

Q1/Q6/Q9/Q18 passed all 11 strict checks. Q3 passed 8/11 and produced a
one-cent alternative at the remaining checks; each formal failure retains the
exact differing cell, and a separate 30-run probe observed the two outputs 22
and 8 times. The normalized five-query diagnostic sum was `559.27 ms` for
DuckDB and `420.47 ms` for GenDB, a `1.33x` aggregate speedup.
GenDB won Q9 and Q18 but lost Q1, Q3, and Q6. Per-query dispatch to the two
correct and faster implementations would improve the aggregate to `1.64x`
(`1.47x` when GenDB process startup is included).

The most informative ablation was Q18: `8682.36 ms` at iteration 0 to
`69.56 ms` at iteration 1 after replacing per-thread hash aggregation with a
sequential group scan over the sorted order-key index.

## 8. Updated Evidence Limits

- No LLM credentials were used; the generation stage was not rerun.
- No SEC-EDGAR benchmark was run.
- The remote experiment covers one virtualized host, hot page cache, one
  client, and five fixed queries.
- The paper did not pin DuckDB or compiler versions. The reproduction used
  DuckDB `1.2.1` and GCC `8.3.0`.
- PMU cycles, instructions, and cache misses were unavailable in the VM.
- No Tide source tree or production machine was modified.
- The product design remains a proposal grounded in Tide notes, not an
  implemented feature.
