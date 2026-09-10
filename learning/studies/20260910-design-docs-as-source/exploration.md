# Exploration Log

## 1. Source Resolution

The supplied X photo URL could not be opened by an anonymous Chromium session:

```text
Navigation failed: net::ERR_HTTP_RESPONSE_CODE_FAILURE
```

The public FxTwitter status API returned the post text and its expanded
alphaXiv URL. The paper identity was then independently checked against arXiv:

```text
Title: Design Docs Are All You Need:
       An AI-native Machine-Learning Performance Tool
arXiv: 2609.05364v1
Date:  2026-09-04
```

The PDF was downloaded from arXiv, not from the social post or an image mirror.

## 2. Archive Verification

Commands:

```bash
curl -L --fail --silent --show-error \
  https://arxiv.org/pdf/2609.05364 \
  -o ../../sources/20260910-design-docs-are-all-you-need-v1.pdf

shasum -a 256 ../../sources/20260910-design-docs-are-all-you-need-v1.pdf
pdfinfo ../../sources/20260910-design-docs-are-all-you-need-v1.pdf
pdftotext -layout ../../sources/20260910-design-docs-are-all-you-need-v1.pdf -
```

Observed:

```text
SHA-256: 98010c2ff6a519a021af8f80a2909eaa0b8751b2656231ae32cc8eed9f8e5f9a
Type: PDF 1.7
Pages: 5
Size: 226625 bytes
Encrypted: no
JavaScript: no
```

`pdftotext` emitted one font mismatch warning but extracted all five pages.

## 3. Claim Extraction

The paper's argument was separated into mechanism, enablers, and outcomes:

| Class | Extracted claim |
| --- | --- |
| Problem | Incremental patching carries obsolete structure into the next version |
| Problem | Finite context causes locally plausible but globally weak Agent changes |
| Mechanism | Keep a DAG of self-contained natural-language docs on the main branch |
| Mechanism | Use one coding sub-agent per doc in topological order |
| Feedback | Log ambiguous prose and upstream bugs against the responsible doc |
| Enabler | Use step-by-step worked examples as in-context demonstrations |
| Enabler | End number-bearing docs with exact reconciliation anchors |
| Enabler | Keep the implementation IR minimal, orthogonal, and recursively composable |
| Outcome | Authors report 1.5 to 3 hour rebuilds at about 100 USD |
| Outcome | Authors report round-off parity with hand-audited reference models |

## 4. Evidence Audit

The PDF contains:

- a workflow figure;
- one condensed Flash Attention DSL listing;
- a description of the recursive `Op` IR;
- reported project size, rebuild time, API cost, and numerical parity.

The PDF does not contain:

- a repository or artifact link;
- the 50 design docs;
- generated source or orchestration code;
- prompts, model versions, logs, or test corpus;
- a result table, repeated-run distribution, ablation, or maintenance baseline;
- a definition of distance in the debt equation;
- dependency-inference accuracy;
- human authoring, review, and repair cost.

Searches by title, arXiv ID, SMART, and author names did not locate a public
repository. The final report therefore labels performance and correctness
outcomes as author-reported.

## 5. Adaptation Decisions

### 5.1 Keep typed authority

The first draft interpretation, “docs replace code,” was rejected for the
whole repository. RecallFS includes maintained executables, schemas, migrations,
tests, operational scripts, benchmark evidence, source archives, and incident
records. These artifacts answer different questions and cannot share one
undifferentiated authority.

The adopted model distinguishes policy, intended design, current
implementation, observed evidence, historical record, and explicitly generated
output.

### 5.2 Make dependencies explicit

The paper uses read-only agents to infer doc dependencies. RecallFS instead
makes `depends_on` authoritative and uses Agent inference only to find probable
missing edges.

Reason: a missed dependency can invalidate execution order. A correctness edge
must be reviewable, diffable, and deterministic.

### 5.3 Keep independent oracles

Tests generated from the same prose can share the same mistake as generated
code. The contract therefore requires at least one independent oracle for
consequential behavior, such as a hand-audited fixture, property test,
differential implementation, protocol suite, target-machine benchmark, or
real end-to-end result.

### 5.4 Use thin prompts

Copying design content into an Agent prompt creates an unversioned duplicate.
The adopted prompt names an entry doc and defines how to resolve dependencies,
anchors, conflicts, and evidence.

### 5.5 Require an explicit generation boundary

Tracked code remains maintained source unless a document declares generated
paths, a clean command, independent reconciliation, promotion, provenance, and
no-manual-edit behavior.

## 6. Why No Demo Was Built

A toy Markdown-to-code generator would not test the paper's actual claims:

- 50-doc dependency orchestration;
- context bounding across topological waves;
- repeated 1.5 to 3 hour full regeneration;
- round-off parity with hand-audited ML performance models;
- total cost relative to incremental maintenance.

Without SMART's docs and reference models, such a demo would be theater rather
than reproduction. The practical output of this study is instead the
repository contract in `designs/agent-ready-docs.md`, which can be piloted on a
real low-risk module and evaluated with explicit metrics.

## 7. Independent Review

Two read-only reviews were run:

1. a paper-claims audit, focused on claim/evidence separation and external
   validity;
2. a RecallFS docs-context audit, focused on compatibility with existing
   learning records, project mirrors, runnable demos, and Agent rules.

Both independently rejected repository-wide code disposability and recommended
typed authority, explicit generation boundaries, independent reconciliation,
and incremental adoption rather than mass-editing historical docs.
