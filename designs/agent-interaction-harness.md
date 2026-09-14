---
doc_id: recallfs-agent-interaction-harness-v1
kind: design
status: active
authority: design
applies_to:
  - repository-wide agent interactions
  - repeatable agent workflows and harnesses
depends_on:
  - AGENTS.md
  - recallfs-agent-ready-docs-v1
  - recallfs-study-whale-agent-harness-v1
supersedes: []
verified_by:
  - interaction-contract review
  - held-out workflow probes
  - reconciliation anchors AIH-RA-1 through AIH-RA-8
---

# Agent Interaction and Harness Optimization Contract

## 1. Decision

RecallFS treats an Agent run as a versioned system composed of:

```text
model + instructions + context + tools + control flow + verifier
```

Efficient interaction means minimizing correction loops and unverified work
while preserving correctness, auditability, and user control. A reusable
interaction must therefore specify the task contract, harness boundary,
evaluation roles, iteration rule, stop conditions, and evidence handoff.

A prompt is only one harness component. Prompt text alone is not the reusable
unit.

## 2. Scope

This contract applies to:

- non-trivial code changes, investigations, reviews, studies, and runbooks;
- repeated workflows that may be converted into Agent skills;
- evaluation of prompts, tools, context policies, orchestration, or model
  choices;
- single-Agent and orchestrated multi-Agent runs.

It governs both:

1. **one-shot collaboration**, where the user wants a task completed with
   minimal clarification;
2. **harness optimization**, where repeated tasks provide enough evidence to
   improve the workflow.

## 3. Non-goals

- Prescribing one universal system prompt.
- Treating every task as an optimization experiment.
- Making safety, permissions, or policy constraints searchable.
- Replacing repository policy, active design, source, tests, runtime evidence,
  or nmem with an interaction log.
- Claiming a stronger model is unnecessary.
- Copying paper-specific phase budgets into unrelated workflows.
- Using the final acceptance set to select the best candidate.

## 4. Terms

| Term | Meaning |
| --- | --- |
| `model` | The fixed model identity and configuration for one comparison phase. |
| `harness` | Instructions, selected context, tool schemas, execution loop, error feedback, retry and termination behavior. |
| `task contract` | The explicit objective, outputs, scope, authority, permissions, verification, and stop conditions for one run. |
| `trajectory` | User input, Agent messages, tool calls and results, state transitions, and final outcome. |
| `oracle` | An evaluator independent enough to reject a plausible but wrong result. |
| `D_work` | Inputs used to perform the current work. |
| `D_harness` | Representative cases used to compare harness candidates. |
| `D_gate` | Held-out cases used only for final acceptance and regression checks. |
| `successor_id` | The next durable run, task, or document identifier for unresolved work; `none` when complete. |

## 5. Inputs and Outputs

### 5.1 Required task input

A non-trivial run must resolve these fields before mutation:

| Field | Requirement |
| --- | --- |
| `objective` | One observable end state, not an activity such as “investigate more.” |
| `deliverables` | Concrete files, APIs, runtime changes, reports, or decisions. |
| `scope` | Components and environments the Agent may inspect or change. |
| `non_goals` | Plausible adjacent work excluded from this run. |
| `authoritative_context` | Applicable policy, governing design, current implementation, and runtime evidence. |
| `execution_authority` | Allowed reads, edits, tests, network actions, commits, pushes, and destructive actions. |
| `verification` | Exact tests, probes, E2E scenarios, thresholds, and independent oracle. |
| `stop_conditions` | Completion, safety stop, ambiguity stop, and budget stop. |

The Agent may infer low-risk omissions from repository context. It must surface
an ambiguity before acting when different answers could cause an irreversible,
security-sensitive, externally visible, or high-cost outcome.

### 5.2 Required handoff

The final handoff fields are exactly:

```text
docs_read:
anchor_results:
ambiguities:
changed_artifacts:
verification_evidence:
successor_id:
```

Concise conversational prose may introduce these fields, but must not replace
them when the governing task requires machine-readable handoff.

## 6. Interfaces and Ownership

| Owner | Responsibility |
| --- | --- |
| User | Defines desired outcome, business constraints, irreversible approvals, and acceptance authority. |
| Governing docs | Preserve versioned intent, invariants, dependencies, and reconciliation anchors. |
| Agent | Gathers evidence, resolves local context, executes within authority, verifies outcomes, and reports uncertainty. |
| Harness | Selects context, exposes tools, controls retries and termination, and preserves execution state. |
| Oracle | Determines whether observable behavior satisfies the contract. |
| Evidence ledger | Records baseline, revisions, environment, outcomes, failures, and retain/revert decisions. |
| nmem | Preserves durable rationale and historical lessons without overriding active design. |

Policy, credentials, permissions, safety boundaries, and the oracle definition
are owned outside the harness optimizer. They cannot be weakened to improve a
score.

## 7. Invariants

### AIH-I1: Authority remains typed

Policy states what is allowed. Active design states intended behavior. Source,
schema, migration, and tests state current executable behavior. Runtime
evidence states what was observed. Historical studies and nmem do not silently
override active design.

### AIH-I2: One comparison changes one axis

A candidate comparison changes exactly one primary axis:

- model identity or weights;
- instructions;
- context selection;
- tool contract;
- observation shaping;
- error feedback and retry;
- control flow;
- termination policy.

Mechanical support edits required by that axis are allowed and must be listed.
Bundling unrelated mechanisms makes attribution invalid.

### AIH-I3: Counterparts stay fixed

During a harness comparison, model identity, task inputs, oracle, and resource
budget remain fixed. During a model comparison, harness, task inputs, oracle,
and resource budget remain fixed.

### AIH-I4: Evaluation roles are separated

`D_harness` may select candidates. `D_gate` may accept or reject the selected
candidate but must not choose among candidates. A gate failure returns the run
to diagnosis; it does not disclose gate-specific answers into a new candidate.

### AIH-I5: Evidence precedes optimization

Every change is tied to an observed failure class and a falsifiable hypothesis.
“Make the Agent better” is not a valid hypothesis.

### AIH-I6: Safety is not an objective term

Permissions, data boundaries, security checks, destructive-action approvals,
and policy compliance are hard constraints. Higher task score cannot trade
them away.

### AIH-I7: Every accepted revision is recoverable

The prior accepted harness, its identity, and its evidence remain available
until the new revision passes `D_gate`. Failed candidates are rejected without
rewriting their historical outcome.

### AIH-I8: Completion is evidence-backed

The Agent cannot declare completion from self-report. Required tests, anchors,
runtime probes, or explicitly stated unverified boundaries determine status.

## 8. Interaction Protocol

### 8.1 Kickoff

The user should provide a thin prompt that points to durable context:

```text
Objective: <one observable outcome>
Deliverables: <owned paths or concrete outputs>
Scope / non-goals: <explicit boundary>
Authoritative context: <policy and governing-doc paths>
Execution authority: <what may proceed without another confirmation>
Verification: <independent oracle, E2E cases, thresholds>
Stop when: <completion, ambiguity, safety, or cost condition>

Return exactly:
docs_read, anchor_results, ambiguities, changed_artifacts,
verification_evidence, successor_id.
```

Do not duplicate the complete governing design into this prompt. The prompt
selects the context and execution protocol; versioned documents carry durable
semantics.

### 8.2 Agent acknowledgement

Before substantial execution, the Agent communicates only:

1. the interpreted outcome;
2. the evidence it will gather first;
3. any assumption that materially changes scope;
4. the immediate next action.

It does not restate the entire request or demand confirmation for reversible,
low-risk implementation details already inside its authority.

### 8.3 Execution updates

An update is useful only when it communicates at least one of:

- new evidence changed the diagnosis;
- a contract boundary or risk was found;
- a phase completed and the next phase begins;
- a blocker requires user action.

Elapsed-time narration without new state is not part of the protocol.

### 8.4 Completion

Completion requires:

```text
requested artifact exists
        |
        v
contract tests and anchors pass
        |
        v
held-out or independent gate passes
        |
        v
diff and evidence reviewed
        |
        v
required commit and push complete
        |
        v
handoff names residual limits
```

An unavailable gate must be reported as unavailable. It cannot be replaced
silently by a weaker check.

## 9. Failure Classification

Classify failures before changing the harness:

| Class | Evidence | First response |
| --- | --- | --- |
| `objective` | Output solves a different problem | Repair task contract or governing design |
| `context` | Required source absent, stale, or buried | Fix selection, dependency order, or compaction |
| `tool-schema` | Invalid call shape or incompatible response | Repair and validate the tool contract |
| `tool-runtime` | Timeout, unavailable service, permission denial | Diagnose environment; add bounded recovery |
| `observation` | Tool succeeded but decisive evidence was hidden or malformed | Reshape returned evidence without changing truth |
| `reasoning` | Inputs are sufficient but inference is repeatedly wrong | Probe model capability; consider model change |
| `control-flow` | Missing planning, review, checkpoint, or dependency order | Change orchestration |
| `termination` | Premature success or unbounded retry | Add explicit completion and patience conditions |
| `oracle` | Score accepts wrong work or rejects correct work | Stop optimization and repair the oracle independently |

A failure may span classes, but one candidate targets one primary class.

## 10. Harness Optimization Protocol

### 10.1 Entry criteria

Optimize a harness only when:

- the workflow recurs or has material risk/cost;
- success can be observed;
- at least one representative failure trajectory exists;
- a candidate can be reverted;
- policy and oracle are fixed.

For a unique, low-risk task, execute directly and improve documentation only
when a reusable lesson actually appears.

### 10.2 Baseline record

Before the first candidate, record:

```yaml
run_id: <stable id>
task_class: <name>
model_id: <provider/model/config>
harness_id: <revision or digest>
policy_revision: <revision or digest>
oracle_revision: <revision or digest>
dataset_roles:
  work: <identity or digest>
  harness: <identity or digest>
  gate: <identity or digest>
budget:
  wall_time: <limit>
  model_tokens_or_rollouts: <limit>
  tool_calls: <limit>
metrics:
  primary: <task success measure>
  guardrails: [<correctness>, <security>, <cost>, <latency>]
```

### 10.3 Candidate record

Each candidate records:

```yaml
candidate_id: <stable id>
parent_harness_id: <accepted revision>
changed_axis: <exactly one primary axis>
failure_class: <observed class>
hypothesis: <falsifiable expected change>
fixed_components: [model, policy, oracle, data, budget]
changed_artifacts: [<paths or config ids>]
evaluation_result: <metrics and failed cases>
decision: retain | revert | inconclusive
successor_id: <next candidate or none>
```

### 10.4 Selection loop

```text
baseline trajectory
        |
        v
classify one failure mechanism
        |
        v
create one-axis candidate
        |
        v
static and contract checks
        |
        v
evaluate on D_harness
        |
   +----+----------------+
   |                     |
regress              improve
   |                     |
   v                     v
revert             evaluate D_gate
                         |
                    +----+----+
                    |         |
                  fail       pass
                    |         |
                    v         v
                  revert    retain
```

The optimizer may inspect `D_harness` failures after each round. It must not
inspect hidden expected outputs from `D_gate` to propose the next candidate.

### 10.5 Stop and switch rules

Every optimization run defines before execution:

- minimum evidence before accepting or stopping;
- patience count or budget;
- primary improvement threshold;
- guardrail regression threshold;
- hard resource cap;
- model-bottleneck criterion.

Classify a task as provisionally `model-limited` only when:

1. context and tool contracts pass;
2. the oracle is independently reviewed;
3. the failure persists across representative `D_harness` cases;
4. at least one bounded harness candidate targeted the failure;
5. the candidate did not improve the relevant intermediate metric;
6. a stronger model under the same harness improves that metric, or the lack
   of such a comparison is explicitly recorded.

Without item 6, `model-limited` remains a hypothesis, not a conclusion.

## 11. Cost Model

Do not optimize target-model rollouts alone. Record:

$$
C_{\text{total}}
=
C_{\text{target model}}
+ C_{\text{proposer}}
+ C_{\text{tools}}
+ C_{\text{human review}}
+ C_{\text{failed runs}}
+ C_{\text{maintenance}}.
$$

An optimization is efficient only if it improves accepted task outcomes under
the relevant total-cost or latency constraint. Lower token count with more
human repair is not an efficiency gain.

## 12. Failure Semantics

| Failure | Required behavior |
| --- | --- |
| Missing or conflicting authority | Stop before mutation; report exact conflict |
| Tool schema mismatch | Reject candidate before task evaluation |
| Oracle defect suspected | Freeze candidate selection; repair and version oracle |
| `D_harness` regression | Revert candidate and retain evidence |
| `D_gate` regression | Revert; do not tune directly on disclosed gate answer |
| Budget exhausted | Stop with `successor_id`; do not claim completion |
| Model capability unresolved | Mark hypothesis and preserve comparison needed |
| Irreversible action lacks authority | Ask the user before execution |
| Push or external publication fails | Preserve local commit/artifact and report failure |

## 13. Worked Examples

### Example A: Missing repository context

Input:

- objective: fix a schema migration failure;
- Agent edits application code without reading the active migration design;
- unit tests pass, real upgrade path fails.

Steps:

1. Classify as `context`, not immediately as `reasoning`.
2. Keep the model, tools, oracle, and task fixed.
3. Change context selection to resolve and read the active design and migration
   dependency chain before mutation.
4. Rerun the same failing upgrade fixture on `D_harness`.
5. Run a distinct old-version-to-current upgrade on `D_gate`.

Expected result: retain the context policy only if both the original failure
and held-out upgrade pass.

### Example B: Tool contract failure

Input:

- model emits `{"query": "..."}`;
- tool schema requires `{"query_list": ["..."]}`;
- retries repeat the same invalid call.

Steps:

1. Classify as `tool-schema`.
2. Add schema validation before execution and return a compact corrective
   error containing the accepted field and shape.
3. Keep model and verifier fixed.
4. Verify valid calls are unchanged and invalid calls recover within the
   declared retry budget.

Expected result: the task no longer consumes repeated calls on an unexecutable
shape; the harness change is not credited as a model improvement.

### Example C: Suspected model limitation

Input:

- governing docs and required files are present;
- tool calls and intermediate values are correct;
- three parser/control-flow candidates do not change the failing inference;
- a stronger model succeeds under the identical harness and budget.

Steps:

1. Preserve the accepted harness.
2. Record the failed candidates and intermediate metric.
3. Compare model identities with harness, data, oracle, and budget fixed.
4. Select the model using `D_harness`.
5. Run `D_gate` once for acceptance.

Expected result: classify the prior setup as model-limited for this task
distribution, not as a universal property of the model.

### Negative Example D: Test-set prompt tuning

Input:

- a candidate fails one held-out gate case;
- the expected answer is copied into the prompt or parser;
- the gate then passes.

Expected result: reject the candidate. The gate has become training data and
must be replaced with an undisclosed equivalent before evaluation resumes.

## 14. Reconciliation Anchors

| Anchor | Condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `AIH-RA-1` | A task prompt omits a low-risk implementation detail available from repository context | Agent resolves it locally and proceeds; ambiguity is recorded only if material | Review first update and handoff |
| `AIH-RA-2` | A task may perform an irreversible or security-sensitive action without explicit authority | Agent stops before that action and requests approval | Negative execution probe |
| `AIH-RA-3` | A harness candidate changes prompt and tool schema together without necessity | Candidate is rejected as non-attributable | Candidate-record lint |
| `AIH-RA-4` | Candidate improves `D_harness` but regresses a `D_gate` correctness guardrail | Candidate is reverted; prior accepted harness remains recoverable | Held-out regression probe |
| `AIH-RA-5` | A gate answer is used to author the next candidate | Gate is considered contaminated and replaced before acceptance | Evidence-ledger review |
| `AIH-RA-6` | Harness changes fail while a stronger model succeeds under the same harness, data, oracle, and budget | Run may be classified provisionally model-limited with comparison evidence | Controlled model comparison |
| `AIH-RA-7` | Optimization reaches patience or hard budget without required acceptance evidence | Run stops as incomplete with a non-`none` `successor_id` | Handoff review |
| `AIH-RA-8` | Agent reports completion without required E2E or independent oracle | Completion is rejected and the missing gate is named explicitly | Verification review |

## 15. Evidence and Unknowns

This contract is informed by the WHALE study but deliberately does not copy its
training-specific numeric schedule. Evidence supports:

- model and harness can be different bottlenecks;
- executable harness changes exceed prompt-only changes in the studied tasks;
- small alternating updates can outperform one large stage per component;
- training-signal patience can find a useful schedule region.

Still unverified for RecallFS:

- the best minimum sample and patience values for software-engineering tasks;
- whether one-axis candidates are always cost-effective;
- how much proposer and human-review cost the protocol saves;
- which task classes are stable enough to justify automated harness search;
- whether a repository-wide skill should automate the candidate ledger.

Adoption should begin with real repeated workflows and measure correction
rounds, accepted-task rate, wall time, total model/tool cost, escaped defects,
and human repair time. Promotion into a reusable skill requires evidence across
multiple task classes, not one successful session.
