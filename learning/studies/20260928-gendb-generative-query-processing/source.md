---
doc_id: recallfs-source-gendb-generative-query-processing-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing
depends_on:
  - learning/sources/20260928-gendb-generative-query-processing-v1.pdf
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/evidence/source-digests.txt
---

# Sources

Access date: 2026-09-28.

## Primary Paper

| Field | Value |
| --- | --- |
| Title | GenDB: The Next Generation of Query Processing -- Synthesized, Not Engineered |
| Authors | Jiale Lao, Immanuel Trummer |
| Version | arXiv `2603.02081v1`, submitted 2026-03-02 17:03:43 UTC |
| Subjects | Databases, Artificial Intelligence, Computation and Language, Machine Learning, Multiagent Systems |
| Abstract | <https://arxiv.org/abs/2603.02081> |
| PDF | <https://arxiv.org/pdf/2603.02081> |
| HTML | <https://arxiv.org/html/2603.02081v1> |
| DOI | <https://doi.org/10.48550/arXiv.2603.02081> |
| Local PDF | [`learning/sources/20260928-gendb-generative-query-processing-v1.pdf`](../../sources/20260928-gendb-generative-query-processing-v1.pdf) |
| Pages | 8 |
| Size | 4,877,487 bytes |
| SHA-256 | `a9d98e03a92e806f1098fb105bdb88d4289d717fd35cbacfe9547443971df9cb` |
| License | CC BY 4.0 |

The PDF was downloaded directly from arXiv. `pdfinfo` identified an
unencrypted PDF 1.7 file with no JavaScript. `pdftotext -layout` extracted all
eight pages. Browser automation independently confirmed the abstract page,
title, authors, submission date, abstract, PDF link, HTML link, and license.

## Author Implementation

| Field | Paper snapshot | Current snapshot |
| --- | --- | --- |
| Repository | <https://github.com/SolidLao/GenDB> | <https://github.com/SolidLao/GenDB> |
| Ref | `arxiv-03-02-2026` | `main` |
| Commit | `b7418071fc51aacd219964e4a68e593234db9237` | `0ccf053ade163137775cd2e9c6e1288ea89d524b` |
| Tree | `7dd25a156c2f4ac9396b3b2bee2ce484f196c52d` | `efbdadc2b480f8f8706ef58cc677407855007afe` |
| Commit date | 2026-03-06 | 2026-06-08 |
| License | Apache-2.0 | Apache-2.0 |
| Local inspection copy | `.tmp/research/GenDB` | `.tmp/research/GenDB` |

The stable branch postdates the arXiv submission by four days and is explicitly
identified by the repository as the paper version. The current branch contains
post-paper changes, notably:

- SQL template extraction and parameterized generated executables;
- persistent storage and versioned query artifacts;
- hardware fingerprinting and rebenchmarking;
- multiple LLM providers;
- an experimental six-layer memory system.

Those changes are implementation evidence, not evidence for the paper's
reported experiments.

## Product Context

The application analysis uses the following RecallFS material as local product
context:

- `projects/stream_engine/docs/jit/json_encoder_llvm_jit_plan.md`;
- `projects/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md`;
- `projects/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md`;
- `projects/stream_engine/docs/fringedb-dict-ingest-plan.md`;
- nmem domain record `tide-dev-agent-domain-stream-engine`.

These sources establish that Tide/stream_engine already has:

- a Catalyst-to-DAG-to-Velox query path;
- a `schema + table -> plan -> compiled kernel` direction;
- LLVM JIT experience, cache and code-registry requirements;
- FringeDB physical encodings and dictionary identities;
- measured evidence that preserving execution shape matters more than merely
  generating code.

## Evidence Classification

| Evidence | Status | Supports | Does not support |
| --- | --- | --- | --- |
| arXiv paper | Verified source artifact | Authors' design, setup, limitations, reported results | Independent reproduction or production readiness |
| Paper branch | Statically inspected | Experiment-era orchestration and validator behavior | Runtime success on another machine |
| Current `main` | Statically inspected | Post-paper template, persistence, memory, and provider changes | Paper claims or backward compatibility |
| Repository benchmark JSON | Inspected | Recorded samples behind published figures | Independent measurement, noise control, or fair end-to-end accounting |
| Tide/stream_engine notes | Existing local evidence | Product-specific JIT and execution-shape constraints | A completed GenDB integration |

## Reproduction Boundary

This study does not rerun GenDB end to end. Full reproduction requires the
excluded 12 GB TPC-H and 3.3 GB SEC-EDGAR generated storage artifacts or a full
rebuild, a Linux host with the documented CPU and memory class, supported LLM
credentials, database baselines, and roughly 1.5 to 2.3 hours of generation per
workload in the recorded runs. The local review performed source, metadata,
digest, and syntax checks only.
