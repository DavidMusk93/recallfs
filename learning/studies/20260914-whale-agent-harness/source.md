---
doc_id: recallfs-source-whale-agent-harness-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260914-whale-agent-harness
depends_on: []
supersedes: []
verified_by:
  - arXiv abstract and HTML metadata review
  - browser access check
  - PDF metadata and SHA-256 verification
  - author repository revision inspection
---

# Sources

Access date: 2026-09-14.

## Primary Paper

| Field | Value |
| --- | --- |
| Title | WHALE: A Simple Recipe for Joint Harness-Weight Optimization |
| Authors | Haechan Kim, Yoonho Lee, Gisang Lee, Chelsea Finn, Kangwook Lee |
| Version | arXiv v1, submitted 2026-08-31 18:12:55 UTC |
| Subjects | Machine Learning (`cs.LG`), Artificial Intelligence (`cs.AI`) |
| Abstract | <https://arxiv.org/abs/2609.00196> |
| PDF | <https://arxiv.org/pdf/2609.00196> |
| HTML | <https://arxiv.org/html/2609.00196v1> |
| DOI | <https://doi.org/10.48550/arXiv.2609.00196> |
| Local PDF | [`learning/sources/20260914-whale-joint-harness-weight-v1.pdf`](../../sources/20260914-whale-joint-harness-weight-v1.pdf) |
| Pages | 28 |
| Size | 628,308 bytes |
| SHA-256 | `f4307846da7686ee75ef070fdc53112c8eb8ca557b9318da0a88838e56b9e602` |
| License | CC BY 4.0 |

The PDF was downloaded directly from arXiv. `pdfinfo` identified the title,
authors, 28 pages, and an unencrypted PDF 1.7 file. Browser automation
independently confirmed that the abstract page and PDF were reachable and that
the PDF response loaded as `application/pdf`.

## Author Code

| Field | Value |
| --- | --- |
| Repository | <https://github.com/krafton-ai/WHALE> |
| Inspected branch | `main` |
| Inspected commit | `fbe125eb7abea7f760c99ab9acc1a6261e708fc6` |
| Commit date | 2026-09-04T11:53:39+09:00 |
| License | Apache-2.0 |
| Local inspection copy | `.tmp/data/WHALE` (not committed) |

The repository contains:

- three domain implementations for SearchQA, mathematical reasoning, and chess
  puzzles;
- weight-only, harness-only, fixed WHALE, and adaptive WHALE launchers;
- the shared alternation driver at `run/lib/alternate.sh`;
- domain-specific base harnesses, harness-search code, and proposer skills;
- dataset preparation and reproduction documentation;
- vendored training-framework copies for each domain.

No result logs, model checkpoints, dataset snapshots, or recorded candidate
harness archives were present in the inspected revision. The public repository
therefore supports code-path and configuration inspection but does not by
itself independently substantiate the reported numeric results.

## Verification Performed

```text
download PDF
    |
    v
verify metadata + digest
    |
    v
extract all 28 pages
    |
    v
cross-check paper tables and algorithms
    |
    v
inspect author code at a fixed revision
    |
    v
run static syntax checks where locally supported
```

Observed checks:

- `pdftotext -layout` extracted the complete paper. It emitted one font
  mismatch warning but produced readable text for all 28 pages.
- All 58 non-vendored Python files compiled with `python3 -m py_compile`.
- The shared alternation driver passed `/bin/bash -n`.
- A bulk shell parse on macOS Bash 3.2 stopped at use of `[[ -v VAR ]]`, a
  Bash feature unavailable in that host version. This is a host portability
  boundary, not evidence of failure under the documented Linux environment.
- The repository's `docs/reproducing.md` states that the published launchers
  are cleaned reimplementations of the scheduler scripts used for the paper.
  Their handoff and resume logic was tested with stubs, but the cleaned
  launchers were not run end to end on GPUs.

## Reproduction Boundary

This study did not reproduce training or benchmark results. Full reproduction
requires, at minimum:

- one machine with eight H200 GPUs according to the author's instructions;
- Qwen3.5-2B and Qwen3.5-4B model artifacts;
- all task datasets and preprocessing;
- a roughly 75 GB SearchQA FAISS/Wikipedia retrieval corpus;
- a code-execution sandbox for mathematical reasoning;
- Anthropic credentials for the harness proposer;
- OpenAI credentials or an equivalent configured key file for the SearchQA
  judge;
- enough rollout budget to execute all compared schedules and multiple
  independent seeds.

The paper's rollout-efficiency comparisons count target-agent rollouts and
explicitly exclude proposer compute. Any total-cost conclusion must include
that omitted cost.

## Evidence Classification

| Evidence | Status | Supports | Does not support |
| --- | --- | --- | --- |
| arXiv PDF and HTML | Verified source artifact | Paper claims, equations, settings, and reported tables | Independent correctness of results |
| Author code at fixed commit | Statically inspected | Published implementation shape and reproduction requirements | Exact identity with experiment-time code |
| Python syntax checks | Locally observed | Non-vendored Python files parse on the local interpreter | Runtime behavior, dependency compatibility, or GPU execution |
| Stub-tested launcher statement | Author-reported | The authors exercised alternation handoffs with stubs | End-to-end reproduction of paper runs |
| Numeric benchmark results | Author-reported | Results under the paper's stated setup | Generalization to other models, tasks, verifiers, or production systems |
