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

## 7. Evidence Limits

- No LLM credentials were used.
- No 12 GB or 3.3 GB generated storage bundle was downloaded.
- No GenDB end-to-end run or target-machine benchmark was performed.
- No Tide source tree or remote production machine was modified.
- Performance conclusions remain attributed to the paper and repository.
- The product design is a proposal grounded in existing Tide notes, not an
  implemented feature.
