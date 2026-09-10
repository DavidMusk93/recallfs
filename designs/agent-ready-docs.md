---
doc_id: recallfs-agent-ready-docs-v1
kind: standard
status: active
authority: design
applies_to:
  - repository-wide complex changes
depends_on:
  - AGENTS.md
verified_by:
  - required-section review
  - reconciliation-anchor execution
---

# Agent-ready Docs Contract

## 1. Decision

RecallFS treats documentation as durable Agent context. A complex design
document must be a versioned semantic interface: it states intent, boundaries,
dependencies, invariants, examples, and verification anchors precisely enough
that an Agent can implement or audit the scoped change without reconstructing
the design from the whole repository.

This contract does not make all code disposable. Only a subtree with an
explicit generation contract may treat its generated outputs as build
products.

## 2. Authority Is Typed

“Source of truth” is incomplete unless it says **truth about what**.

| Authority | Answers | Examples |
| --- | --- | --- |
| `policy` | What is allowed or required? | nearest `AGENTS.md`, root `AGENTS.md` |
| `design` | What behavior and invariants are intended? | accepted architecture or feature contract |
| `implementation` | What behavior is currently executable? | source, config, schema, migration, tests |
| `evidence` | What was actually observed? | benchmark, production trace, incident record |
| `generation` | What inputs regenerate declared outputs? | doc-native generated subtree |

The first four categories are not a total ordering. They answer different
questions. A disagreement is drift to investigate:

- policy conflict: stop and surface it;
- design versus implementation: report the mismatch and update the affected
  contract, implementation, and tests coherently;
- implementation versus runtime evidence: verify revision, environment,
  configuration, and execution path;
- historical evidence versus current behavior: preserve the historical record
  and add superseding context;
- generation input versus generated output: regenerate from a clean location
  instead of patching the output.

## 3. Scope

This contract applies when an Agent creates or materially changes:

- architecture and feature designs;
- implementation plans for complex changes;
- operational runbooks;
- performance or correctness studies that will guide later work;
- docs that declare generated outputs.

It does not require a noisy migration of every historical Markdown file.
Existing docs adopt the contract when next materially revised.

## 4. Non-goals

- Replacing nmem as the cross-session memory system.
- Declaring all Markdown normative or all source code generated.
- Rewriting historical studies, incidents, and evidence as current design.
- Generating tests and implementation from one prompt and calling agreement
  independent correctness.
- Requiring every worker to load the entire documentation graph.

## 5. Required Metadata

Use YAML front matter for a new or materially revised complex doc:

```yaml
---
doc_id: project-topic-v1
kind: design
status: draft
authority: design
applies_to:
  - path/to/module
depends_on:
  - other-doc-id
supersedes: []
verified_by:
  - command or named manual gate
---
```

Field semantics:

| Field | Requirement |
| --- | --- |
| `doc_id` | Stable repository-unique ID. Change the version only for a breaking semantic rewrite. |
| `kind` | `design`, `plan`, `runbook`, `study`, `incident`, or `reference`. |
| `status` | `draft`, `active`, `superseded`, or `archived`. |
| `authority` | Usually `design` or `evidence`; use `generation` only with Section 9. |
| `applies_to` | Paths or named components governed by the doc. |
| `depends_on` | Explicit doc IDs or repository paths required to interpret this doc. |
| `supersedes` | Older doc IDs replaced by this one. Never silently rewrite history. |
| `verified_by` | Tests, commands, probes, or named manual gates that reconcile claims. |

An Agent may infer and suggest a missing dependency, but inferred edges are
diagnostics. Only explicit `depends_on` edges control execution order.

## 6. Required Body

The body must answer these questions in this order:

1. **Decision:** What is the chosen behavior in one paragraph?
2. **Scope:** What artifacts and users are covered?
3. **Non-goals:** What plausible adjacent work is excluded?
4. **Inputs and outputs:** What enters and leaves the boundary?
5. **Interfaces and ownership:** Which component owns each state transition?
6. **Invariants:** What must remain true across success, failure, and retry?
7. **Failure semantics:** What fails, retries, rolls back, or remains partial?
8. **Worked examples:** What happens step by step on concrete inputs?
9. **Reconciliation anchors:** What exact observations prove conformance?
10. **Evidence and unknowns:** Which claims are observed, inferred, proposed,
    or still unresolved?

Do not use prose such as “handle errors correctly” or “keep performance good.”
Name the error, state transition, threshold, environment, and expected result.

## 7. Worked Examples

A worked example is an executable-in-your-head trace, not a happy-path story.
It contains:

- concrete input;
- initial state and relevant configuration;
- each semantically important intermediate state;
- exact output or error;
- the invariant demonstrated;
- the reconciliation anchor that checks it.

For numerical behavior, include units, ranges, rounding, overflow behavior,
tolerance, and exact expected values where exactness is part of the contract.

For stateful behavior, include identity, ordering, retry, duplicate delivery,
partial failure, and cleanup where applicable.

For APIs, include request, response, status, side effects, and idempotency.

Negative examples are required when a superficially plausible interpretation
would violate the design.

## 8. Reconciliation Anchors

Each behavior-bearing doc must end its examples with stable anchor IDs:

| Anchor | Input or condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `RA-1` | A concrete happy path | Exact output and state | Test or command |
| `RA-2` | A boundary or failure | Exact error and cleanup | Test or probe |
| `RA-3` | A cross-module invariant | Exact observable relationship | Integration or differential check |

Anchors should become executable tests when practical. Generated tests are not
independent evidence if the same Agent and same prose generate both code and
oracle. Keep at least one independent oracle for consequential behavior:

- hand-audited fixture;
- differential implementation;
- property or metamorphic test;
- protocol conformance suite;
- real end-to-end evidence;
- security or performance gate on the target environment.

## 9. Document Dependency DAG

The context graph has two edge types:

```text
doc A --depends_on--> doc B
doc A --consumes----> interface exported by B
```

The orchestrator must:

1. reject missing dependencies and cycles before implementation;
2. read dependencies in topological order;
3. pass one task doc plus only the needed exported contracts to a worker;
4. integrate and verify one dependency layer before starting dependents;
5. log ambiguous prose and upstream defects against the responsible `doc_id`.

Do not make every document self-contained by copying all upstream text. A doc
is self-contained at its task boundary when it names dependencies and restates
only the contracts it consumes.

## 10. Generation Contract

`authority: generation` is valid only when the doc also declares:

```yaml
generated_outputs:
  - path/to/generated/subtree
regenerate:
  command: exact clean-generation command
  clean_root: isolated output directory
reconcile:
  - independent verification command
promotion:
  command: exact validated replacement command
manual_edits: forbidden
```

Before generated output may be called disposable, all of these must hold:

- authoritative inputs and output ownership are complete;
- generation starts from a clean isolated directory;
- dependencies and tool/model versions are recorded;
- independent reconciliation passes;
- API, schema, state, migration, security, and performance compatibility gates
  required by the module pass;
- the old implementation remains recoverable until validated promotion;
- direct edits to generated paths are rejected or overwritten.

Without this block, tracked code is maintained source even when a design doc
exists.

## 11. Agent Prompt

Use a thin prompt that points to the governing doc:

```text
Implement from <entry-doc>.

Treat only active docs with authority=design or authority=generation as
normative. Resolve explicit depends_on edges and read them in topological
order. For each task, load the entry doc plus only the exported contracts of
its dependencies.

Preserve stated invariants and execute every reconciliation anchor. Convert
anchors into tests when practical. If the docs, code, tests, or runtime
evidence disagree, report the drift; do not silently choose one.

Code is disposable only for paths covered by an explicit generation contract.
Return docs_read, anchor_results, ambiguities, evidence, and successor_id.
```

Do not paste the whole design into the prompt. Duplicated requirements drift,
waste context, and create an unversioned second specification.

## 12. Worked Governance Examples

### Example A: Maintained implementation

Input:

- `feature-x-v1` has `status: active` and `authority: design`;
- source and tests currently implement an older behavior;
- no generation contract exists.

Steps:

1. Agent reads `feature-x-v1` and its explicit dependencies.
2. Agent records the design-versus-code drift.
3. Agent changes maintained source and tests.
4. Agent runs all anchors.
5. Agent updates the governing doc only if implementation discovery invalidates
   or completes the design contract.

Expected result: source remains maintained; the Agent must not delete and
regenerate the module merely because a design doc exists.

### Example B: Generated island

Input:

- `schema-client-v2` has `authority: generation`;
- it names generated output, clean command, independent protocol fixture, and
  promotion command;
- a request field changes.

Steps:

1. Agent edits the governing doc and fixture.
2. Agent generates into the declared clean root.
3. Agent runs protocol and compatibility reconciliation.
4. Agent promotes only the validated output.
5. Agent records doc digest, tool/model identity, duration, cost, retries, and
   anchor results.

Expected result: no direct patch to the old generated client survives.

### Example C: Missing dependency

Input:

- `service-api-v3` consumes `auth-policy-v2`;
- `auth-policy-v2` is absent from `depends_on`;
- an Agent infers the relation from prose.

Expected result: implementation does not start. The inferred edge is reported
and made explicit before topological execution.

## 13. Contract Anchors

| Anchor | Condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `DOC-RA-1` | Active design doc conflicts with maintained source | Agent reports drift and updates source/tests coherently; it does not classify source as generated | Review changed-file and evidence ledger |
| `DOC-RA-2` | `authority: generation` lacks one Section 10 field | Generated output is not disposable and promotion is blocked | Metadata review |
| `DOC-RA-3` | Explicit `depends_on` graph contains a cycle | Implementation stops before mutation and reports the cycle | DAG validation |
| `DOC-RA-4` | Worked example contains numeric behavior | Units, boundary, expected value or tolerance, and executable verification are present | Required-section review |
| `DOC-RA-5` | Prompt duplicates the governing design | Replace copied requirements with entry path and execution protocol | Prompt review |

## 14. Run Evidence

An Agent handoff for doc-governed work should include:

```text
docs_read:
anchor_results:
ambiguities:
changed_artifacts:
verification_evidence:
successor_id:
```

The run log is execution evidence, not a second design document. Repeated
ambiguity must be repaired in the governing doc. Durable cross-session lessons
and decisions go to nmem; repository docs remain the versioned project
artifacts that code, tests, and users can reference.
