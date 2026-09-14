---
doc_id: recallfs-exploration-whale-agent-harness-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260914-whale-agent-harness
depends_on:
  - recallfs-source-whale-agent-harness-v1
supersedes: []
verified_by:
  - command and decision review
---

# Exploration Log

Date: 2026-09-14.

## 1. Question

The user supplied arXiv `2609.00196` and asked:

1. download and study the paper;
2. explain how to interact with an Agent efficiently;
3. determine whether the result can become a reusable specification.

The study was classified as a mechanism and workflow question. A toy
coordinate-descent demo would not exercise model training, executable harness
search, verifier behavior, or task-distribution shift, so the chosen durable
outputs were a source archive, evidence-led study, browser explainer, and
repository-wide interaction contract.

## 2. Source Acquisition

Commands:

```bash
curl --fail --location --retry 3 --retry-delay 2 \
  --output learning/sources/20260914-whale-joint-harness-weight-v1.pdf \
  https://arxiv.org/pdf/2609.00196

pdfinfo learning/sources/20260914-whale-joint-harness-weight-v1.pdf
pdftotext -layout \
  learning/sources/20260914-whale-joint-harness-weight-v1.pdf \
  .tmp/data/whale-paper.txt

shasum -a 256 \
  learning/sources/20260914-whale-joint-harness-weight-v1.pdf
```

Observed:

- download succeeded;
- PDF title and authors matched the arXiv abstract page;
- page count was 28;
- file size was 628,308 bytes;
- SHA-256 was
  `f4307846da7686ee75ef070fdc53112c8eb8ca557b9318da0a88838e56b9e602`;
- `pdftotext` emitted a font mismatch warning but extracted readable text.

Browser automation separately verified the abstract and PDF URLs, version,
submission timestamp, authors, and code link.

## 3. Author Code Inspection

Commands:

```bash
git clone --depth 1 https://github.com/krafton-ai/WHALE.git .tmp/data/WHALE
git -C .tmp/data/WHALE rev-parse HEAD
git -C .tmp/data/WHALE log -1 --format='%H%n%aI%n%s'
```

Inspected revision:

```text
fbe125eb7abea7f760c99ab9acc1a6261e708fc6
2026-09-04T11:53:39+09:00
Link the Hugging Face Papers page from the README
```

The main implementation handoff is visible in `run/lib/alternate.sh`:

```text
accepted harness -> weight phase via HARNESS_PATH
new checkpoint   -> harness phase via VLLM_MODEL
accepted harness -> next cycle
```

The driver records cycle progress, reuses completed checkpoints and accepted
harnesses, and supports fixed or patience-based harness phases.

The domain proposer skills add constraints absent from the short paper:

- one candidate should test one mechanism;
- candidates must preserve tool schemas;
- task-specific answer hardcoding is forbidden;
- failed and successful trajectories are read before proposing;
- candidates are syntax-checked and self-critiqued;
- the outer loop, not the proposer session, owns evaluation.

These are important engineering controls, not incidental prompt wording.

## 4. Static Verification

Commands:

```bash
find .tmp/data/WHALE -type f -name '*.py' \
  -not -path '*/verl/*' -print0 |
  xargs -0 -n1 python3 -m py_compile

/bin/bash -n .tmp/data/WHALE/run/lib/alternate.sh
```

Observed:

- 58 non-vendored Python files passed syntax compilation;
- the shared alternation driver passed Bash syntax checking;
- bulk parsing all shell scripts stopped on `[[ -v VAR ]]` because the local
  macOS host provides Bash 3.2. The documented execution target is Linux with
  an H200-specific stack, so this was recorded as a local portability boundary.

No runtime, dataset, GPU, retrieval, sandbox, proposer, or judge execution was
attempted.

## 5. Evidence Extraction Decisions

The final study retained:

- exact equations for the joint objective and alternating update;
- domain-level Table 2 values;
- schedule and rollout values from Table 3;
- intermediate behavior metrics that distinguish harness- and model-dominant
  regimes;
- adaptive stopping parameters;
- explicit reproduction requirements and code-release limitations.

The study did not reproduce every paper trajectory or bibliography entry.
Those do not change the answer to the interaction-protocol question.

## 6. Critical Review

The following limitations changed the engineering conclusion:

1. proposer compute is excluded from rollout-efficiency claims;
2. no multi-seed variance or confidence intervals are reported;
3. candidate harness evaluation uses one rollout per example;
4. best results are selected from test mean@8 trajectories;
5. one fixed verifier participates in training, harness ranking, and testing;
6. the public cleaned launchers were stub-tested but not end-to-end GPU tested;
7. the study covers small Qwen models and three tasks with binary outcomes.

Therefore the reusable contract does not prescribe WHALE's numeric schedule or
claim automatic harness search is universally cost-effective. It preserves the
mechanism-level ideas: joint-system diagnosis, one-mechanism candidates,
separate evaluation roles, bounded alternating updates, rollback, and explicit
stopping.

## 7. Promotion Decision

The reusable result was promoted to:

[`designs/agent-interaction-harness.md`](../../../designs/agent-interaction-harness.md)

It depends on the existing
[`designs/agent-ready-docs.md`](../../../designs/agent-ready-docs.md):

- agent-ready docs define durable context and authority;
- the new contract defines run-time interaction, harness diagnosis, controlled
  optimization, and evidence handoff.

No new Agent skill was created. The contract should first be exercised on
several real tasks. A skill becomes justified only after repeated use reveals
stable automation boundaries and measurable improvement.

## 8. Documentation and Browser Acceptance

The frontmatter dependency graph was resolved manually before delivery:

```text
source.md
    |
    +----> README.md ----> agent-interaction-harness.md
    |
    +----> exploration.md

AGENTS.md -------------> agent-interaction-harness.md
agent-ready-docs.md ---> agent-interaction-harness.md
```

All referenced paths existed, all four new `doc_id` values were unique in the
scanned scope, and no cycle was found.

ASCII graph verification:

```bash
rustc tools/verify_ascii_graphs.rs -o .tmp/verify-ascii-graphs
.tmp/verify-ascii-graphs --ruler \
  learning/studies/20260914-whale-agent-harness/README.md
.tmp/verify-ascii-graphs --ruler \
  learning/studies/20260914-whale-agent-harness/source.md
.tmp/verify-ascii-graphs --ruler \
  learning/studies/20260914-whale-agent-harness/exploration.md
.tmp/verify-ascii-graphs --ruler \
  designs/agent-interaction-harness.md
```

The browser report was served at the final artifact path and checked with
Playwright against installed Google Chrome:

| Check | Desktop | Mobile |
| --- | --- | --- |
| Viewport | 1440 x 900 | 390 x 844 |
| Page overflow | pass, 1440 / 1440 px | pass, 390 / 390 px |
| Table probe | pass, 2 / 2 tables | pass, 2 / 2 tables |
| Code-block probe | pass | pass |
| SVG overlap probe | pass | pass |
| Clipped text | none | none |
| Console/page/network errors | none | none |

The first browser pass found the SVG return path crossing its explanatory
label. The label was moved below the path, and the complete matrix was rerun.
The final HTML SHA-256 is
`e0a6bac8b1645cc348de2aaff38f5bf0e224ef4c3b45089305efd297ed709a8e`.
Temporary screenshots and probe results remain under
`.tmp/reports/whale-agent-harness-report/` and are not committed.
