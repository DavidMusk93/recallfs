---
doc_id: recallfs-evidence-gendb-implementation-audit-v1
kind: evidence
status: active
authority: evidence
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing
depends_on:
  - recallfs-source-gendb-generative-query-processing-v1
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/evidence/source-digests.txt
---

# GenDB Implementation Audit

This audit distinguishes the paper snapshot
`b7418071fc51aacd219964e4a68e593234db9237` from current `main`
`0ccf053ade163137775cd2e9c6e1288ea89d524b`.

## 1. What Is Implemented

The paper branch implements a deterministic outer orchestrator around five LLM
roles:

1. Workload Analyzer profiles hardware, schema, samples, selectivity, joins,
   and cardinality.
2. Storage/Index Designer emits a binary columnar layout, ingestion code,
   indexes, and per-query guides.
3. Query Planner emits a JSON physical plan.
4. Code Generator writes, compiles, executes, and repairs C++17/OpenMP code.
5. Query Optimizer revises plans from timing and validation feedback.

The outer process serializes candidate execution to reduce benchmark
interference while allowing LLM work for different queries to overlap. It
compiles with unpinned `g++ -O3 -march=native -std=c++17 -fopenmp`.

Current `main` adds parameterized SQL templates, versioned binaries, hardware
fingerprints, multiple model providers, query similarity routing, persistent
storage reuse, and an experimental memory hierarchy. These are useful
directions but are not part of the paper evaluation.

## 2. Verified Strengths

| Mechanism | Evidence | Product value |
| --- | --- | --- |
| Typed stage boundaries | Planner and storage stages write JSON contracts | Limits some free-form cross-agent drift |
| Deterministic outer budget | Iteration, timeout, queue, and stall logic live in code | Agent cannot silently expand the optimization budget |
| Runtime feedback loop | Every candidate is compiled, run, timed, and compared | Optimization is grounded in executable behavior |
| Candidate rollback | Slower or newly incorrect later candidates are rejected | Preserves the best observed candidate within a run |
| Hardware-aware specialization | Cache sizes, core count, SIMD, and memory enter planning | Enables execution shapes unavailable to a generic operator |
| Post-paper template reuse | SQL literals become parameters and binaries are versioned | Moves amortization from one query instance toward a workload family |

## 3. Production Blockers

### GDB-AUDIT-1: invalid first candidate can be promoted

Observed in both the paper branch and current `main`:

```text
iter0Passed = execution_results.run.status == "pass"
bestResult.status = "completed"
...
if result.status == "completed":
    promoteToVersion(...)
```

`run.status` only means the binary exited successfully. It does not mean
`validation.status == "pass"`. Current promotion records
`hardware_results.<fingerprint>.validated = false` but still updates the
`best` symlink to that version.

Required correction: the promotion state machine must accept only a candidate
whose complete gate result is `PASS`; a failed or missing oracle is not a
deployable version.

### GDB-AUDIT-2: current reuse is keyed by query ID, not semantic identity

`inspectWorkloadState()` checks that `queries/<queryId>/template.sql` and
`best` exist, then skips generation when the hardware fingerprint is already
validated. It does not compare the current SQL with the stored template.
Changing SQL while retaining `Q3`, for example, can route to the old binary.

Required correction: use a content-addressed key over normalized logical plan,
parameter schema, result schema, SQL semantic settings, and all dependencies.
Human labels such as `Q3` are metadata, never identity.

### GDB-AUDIT-3: artifact identity omits semantic and data epochs

`meta.json` records storage path strings and hardware results. It does not bind
the artifact to:

- schema/catalog version;
- storage layout and index manifest digests;
- source data snapshot or visibility epoch;
- statistics/profile digest and validity range;
- SQL dialect, collation, timezone, null and overflow semantics;
- compiler version, flags, runtime ABI, model, prompts, or verifier version.

Required correction: sign a complete artifact manifest and fail closed on any
identity mismatch.

### GDB-AUDIT-4: generated code and agents are unsandboxed

The Claude provider sets `permissionMode: "bypassPermissions"` and
`allowDangerouslySkipPermissions: true`. The Codex provider sets
`sandboxMode: "danger-full-access"` and `approvalPolicy: "never"`. Generated
native binaries then execute directly on the host. No seccomp, namespace,
cgroup, filesystem allowlist, network denial, syscall policy, or resource
limit was found.

Required correction: isolate both the build agent and candidate process.
Generated code must receive read-only snapshot handles and a bounded result
sink, not arbitrary host paths or production credentials.

### GDB-AUDIT-5: the oracle is benchmark-specific and incomplete

The validator compares CSV files. TPC-H and financial modes classify tolerance
from output column names. Headers themselves are not compared. Generic mode
rounds numeric values and sorts rows. The current test suite contains no
dedicated test files, and `npm test` starts the full orchestrator.

Required correction: define result schema, ordering, duplicate, null, decimal,
floating-point, collation, timezone, overflow, and error semantics explicitly.
Run differential tests over held-out parameters and data snapshots, plus
metamorphic and property-based tests.

### GDB-AUDIT-6: the benchmark does not establish end-to-end superiority

The reported results cover:

- five TPC-H SF10 queries and six SEC-EDGAR queries;
- a 384 GB host with the complete database cached in memory;
- three measured hot runs;
- one hardware class;
- read-only analytical workloads.

The GenDB benchmark subtracts the generated program's reported `output` phase
from `total`, while database baselines time `execute(...).fetchall()`. This is
not necessarily outcome-changing for small outputs, but it is an asymmetric
measurement contract that must be normalized before product decisions.

No public evidence establishes cold-cache behavior, concurrent tenants,
updates, transactions, crash recovery, schema changes, skew drift, NUMA
placement, tail latency, or long-running soak behavior.

### GDB-AUDIT-7: build and dependency reproduction are weak

The repository has:

- no release tags;
- no JavaScript lockfile;
- caret-ranged SDK dependencies;
- an unpinned system compiler;
- no CI workflow;
- no real automated test command;
- a package license field of `ISC` while the repository license is
  Apache-2.0.

These do not negate the research prototype, but they prevent treating the
repository itself as a production component.

## 4. Benchmark Evidence

Paper-reported aggregate hot execution:

| Workload | GenDB | Best reported baseline | Speedup |
| --- | ---: | ---: | ---: |
| TPC-H SF10, 5 selected queries | 214 ms | Umbra 590 ms | 2.8x |
| SEC-EDGAR, 6 selected queries | 328 ms | DuckDB with GenDB indexes 1,549 ms | 4.7x, reported as 5.0x |

Recorded multi-agent generation telemetry:

| Workload | Wall clock | LLM cost |
| --- | ---: | ---: |
| TPC-H | 90.9 min | $14.15 |
| SEC-EDGAR | 139.7 min | $23.49 |

An approximate execution-only break-even, ignoring ingestion, verification,
deployment, and dollar cost, is:

$$
N_{\text{break-even}}
=
\left\lceil
\frac{T_{\text{generate}}}
{T_{\text{baseline}} - T_{\text{accelerated}}}
\right\rceil.
$$

Using the paper's aggregate numbers gives approximately 14,400 repeated TPC-H
workload cycles or 6,900 SEC-EDGAR cycles. This is why production admission
must be driven by measured recurrence and saved CPU time, not speedup alone.

## 5. Evidence Limits

- Static syntax checks passed for the inspected orchestrator, template
  extractor, SQL parser, result comparator, and benchmark entry point.
- GenDB was not installed or run end to end.
- Benchmark numbers are author-provided artifacts, not independently
  reproduced measurements.
- The paper and repository disagree on one TPC-H ablation label: repository
  samples total about 281 ms for `Single-Agent (High-Level)` and 456 ms for
  `Single-Agent (Guided)`, while the paper text names guided as the 281 ms
  variant. The multi-agent conclusion remains reported, but that label should
  be resolved before using the ablation quantitatively.
