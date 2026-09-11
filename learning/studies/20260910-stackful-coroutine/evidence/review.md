---
doc_id: recallfs-review-stackful-coroutine-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-stackful-coroutine/demo
depends_on:
  - recallfs-evidence-stackful-coroutine-v1
supersedes: []
verified_by:
  - review run 20260910-184406-b37f2090
  - review run 20260911-000014-f8811338
  - review run 20260911-174115-96315ecf
---

# Code Review Closure

| Field | Value |
| --- | --- |
| Review run | Initial runtime review; identity preserved in the raw receipt |
| Reviewed head | Pre-A/B runtime snapshot; no longer reachable from `master` |
| Review status | Complete |
| Initial verdict | Ready with fixes |
| Validated findings | 5 |
| Fix commit | `ecda6780273a57adcde8e6c94754563f61671e64` |
| Raw receipt | [`learning/studies/20260910-stackful-coroutine/evidence/raw/code-review.json`](raw/code-review.json) |

## Applied Findings

| Finding | Resolution | Verification |
| --- | --- | --- |
| Ready tasks can starve I/O | Poll epoll nonblocking after 64 READY dispatches | Deterministic pipe fairness test; all 20 iperf proxy streams progressed |
| Benchmark accepts stalled streams | Require exact stream count and positive sender/receiver progress; write per-stream CSV | Five direct and five proxy runs passed |
| Accept failure leaves live dead proxy | Route unexpected listener wait errors through `app_fail()` | Focused native test and FIL-C compile |
| Combined READ/WRITE misses write-only readiness | Merge ready bits and wake a shared waiter once | EPOLLOUT-only socketpair test |
| Frequency policy missing | Record driver/governor/range/turbo or explicit unavailability | d2 reports KVM sysfs controls unavailable |

Additional test hardening from review:

- every recursive frame now checks its exact stack checksum;
- the E2E backend waits for request EOF before sending its response;
- the context benchmark checks exact checksums, task counts, and switch counts;
- normal readiness uses `EPOLLONESHOT` plus `EPOLL_CTL_MOD` rearm;
- overload rejection sleeps after one over-limit accept;
- the default listener changed from wildcard to loopback.

## Residual Risks

- A deterministic stale-event injection test is still absent. Real connection
  churn exercises generation changes, but cannot force a stale token already
  copied into an `epoll_wait` result batch.
- `CLOCK_MONOTONIC` failure after timers exist is not fault-injected. This is a
  narrow Linux platform failure path.
- Established TCP sessions have no application idle timeout. This preserves
  transparent long-lived TCP semantics; deployments need an external policy or
  a future opt-in idle deadline if idle-slot exhaustion is in their threat
  model.
- `rco.c` remains a large private implementation file. The independent
  validator rejected file length alone as a correctness or project-contract
  defect; splitting remains optional maintenance work.

The cross-model adversarial route did not run because the host serving family
could not be attested. A fresh in-process adversarial reviewer covered the
lens, and an independent validator checked all eight primary candidates.

## L4 A/B Review Closure

| Field | Value |
| --- | --- |
| Review run | L4 A/B review; identity preserved in the raw receipt |
| Reviewed head | `463126cb7ab3e40df6f214803695dd200a4f0ef4` plus staged docs/evidence |
| Review status | Complete |
| Initial verdict | Not ready |
| Reviewers | correctness, standards, testing, maintainability, security, performance, API contract, reliability, adversarial |
| Independent validation | 6 findings validated, 2 dropped |
| Code fix commit | `ae0364683fc45e99847872a9f1fe32e5de98c1f4` |
| Raw receipt | [`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/code-review.json`](raw/ab/code-review.json) |

Applied findings:

| Finding | Resolution | Verification |
| --- | --- | --- |
| Transient accept errors killed the process | Both forwarders classify connection-local errors and back off on resource errors | FIL-C plus injected accept errors |
| Saturated epoll listener queued clients | Bounded accept-and-close rejection now matches coroutine semantics | `max_connections=1` parity E2E |
| Benchmark could hang or accept forced drain | Child deadlines escalate TERM to KILL; normal summary must be unique and non-forced | focused shell harness test |
| Fixed mode order biased sub-percent comparison | Three positions rotate deterministically and are recorded in `run-order.csv` | six-run order fixture and final five-run evidence |
| Epoll VM depended on `RLIMIT_NOFILE` | FD watches use lazy 256-entry chunks; soft/hard limits are recorded | 1,024 vs 1,048,576 startup test, 0 KiB growth |
| Evidence promotion was not bounded | Clean/output roots are explicit and 59 payload paths have SHA-256 entries | `SHA256SUMS` verification |

The validator rejected two structural suggestions as non-defects: file length
alone did not justify a P1, and compiling the production C translation unit
into a white-box focused test did not prove behavioral divergence. The review
also rejected a compatibility requirement for the study-local benchmark
script because no external caller exists and the documented invocation changed
with the new A/B contract.

Remaining test gaps are deterministic connect-deadline expiry and partial-send
`EAGAIN` injection. The existing end-to-end suite exercises both paths under
real sockets but cannot force their exact timing. They are residual coverage
risk, not unresolved review findings.

## CACS And Scale Review Closure

| Field | Value |
| --- | --- |
| Review run | `20260911-000014-f8811338` |
| Reviewed head | `7ef59dbec7b068be354a89be4ce14e04ef786597` plus staged docs/evidence |
| Review status | Complete |
| Initial verdict | Not ready |
| Reviewers | correctness, standards, testing, maintainability, performance, API contract, reliability, adversarial |
| Independent validation | 7 findings validated, 1 dropped |
| Fix commits | `db37e70`, `2ac22ec`, `e4b9b05` through `57f7ce3` |
| Raw receipt | [`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-code-review.json`](raw/cacs-code-review.json) |

Applied findings:

| Finding | Resolution | Verification |
| --- | --- | --- |
| Signal during `Popen` escaped cleanup | Block handled signals through launch and restore them only inside cleanup protection | Deterministic launch-window injection in the 19/19 focused suite |
| Descendant zombies were accepted as reaped | Enable Linux child-subreaping, reap by process group, and require group disappearance | TERM-ignoring descendant and zombie-sensitive tests |
| `/proc` metrics lacked field-level oracles | Extract pure status, smaps, and stat parsers with exact and malformed fixtures | Four parser tests plus real-process integration |
| L4 labels trusted positional binaries | Query compile-time identities, reject duplicates, and execute digest-verified private copies | Swapped, duplicate, source-replacement, and private-copy replacement tests |
| L4 summary discarded run pairing | Emit ratio-of-medians separately and use median paired ratios for comparisons | Divergent-ratio fixture and incomplete/duplicate run rejection |
| Build provenance omitted target flags | Track one bounded evidence driver and capture O0/O2/O3/ASan compile/link commands | `compile_and_link_target_binding=PASS` |
| CACS conclusions lacked a document edge | Add `raw/cacs-scale` to exploration metadata and contract paths | Document DAG and local-link validation |

The validator dropped the proposed broad `-diff` attributes for generated
payloads as review-policy preference. The two CRLF CSV files remain explicitly
`-text`, and the staged Git blobs match the 130-entry SHA-256 manifest.

That CACS review iteration used d2 evidence source
`57f7ce3163adb14ea8023c51b9d0f7272176f8d1`. Its tracked focused log records
19/19 high-concurrency harness tests; native O3 and sanitizer suites report
25/25 and 22/22 targets.

## v0.2 Local-State, Preemption, And Pool Review

| Field | Value |
| --- | --- |
| Review run | `20260911-174115-96315ecf` |
| Reviewed range | `860f59a..0a7dc4d` |
| Review status | Complete with degraded adversarial coverage |
| Completed reviewers | correctness, testing, maintainability, performance, reliability, API contract, project standards |
| Incomplete reviewer | adversarial terminated at the repository 10-minute review limit |
| Independent validation | 8 findings validated, 0 rejected |
| Fix commit | `d3f48b96964d39760ec75b4e5a2ce7227b9b7947` |
| Raw receipt | [`raw/v02-code-review.json`](raw/v02-code-review.json) |

Applied findings:

| Finding | Resolution | Verification |
| --- | --- | --- |
| Zero nested `max_fds` lost the runtime default | Share `RCO_MAX_FDS_DEFAULT` and expand only when the control FD requires it | Required-worker FD wait with descriptor opened after pool start |
| Whole dispatch batch became non-stealable | Claim each job immediately before spawn | `dispatch_batch > 1` regression proves peer worker steals remaining jobs |
| Job entry errors were invisible | Add `failed` and `first_job_error`; keep wait/join scoped to infrastructure | Mixed zero/negative/positive job-result test |
| Failed start lifecycle was undocumented | Specify internal join and destroyability | Invalid nested runtime config, rejected submit, repeat join, destroy |
| Timed join retry was untested | Add blocked multiworker timeout and successful retry | Three backend pool suites |
| TLS/finalizer wording was ambiguous | Limit task TLS access to TLS destructors; finalizer sees root state | finalizer `get/set == -EPERM` assertions |
| Reserved preemption signal was undocumented | Specify `SIG_BLOCK`/`SIG_SETMASK` rejection | local-state and preemption suites |
| Pool evidence could hang or false-pass scaling | Add per-run timeout, worker speedup gates, failure fields, and pool PMU probe | 1,500 stress runs, 90 samples, 139-entry manifest |
| Active design contradicted v0.2 | Update study, runbook, evidence ledger, and exploration | frontmatter DAG and local-link checks |

Residual findings:

- `rco.c`, `rco_pool.c`, and `rco_pool_test.c` remain large. Splitting them now
  would move tightly coupled private invariants across translation units after
  correctness stabilization; defer until a concrete ownership boundary pays
  for that risk.
- pool queue metadata uses one mutex. Corrected long-workload evidence passes
  `1.50x`/`2.50x` minimum gates and observes `1.878-1.891x` at two workers and
  `3.104-3.145x` at four, so this is not a current blocker. Short-job
  contention remains workload-dependent.
- the embedded Python evidence validator remains in the shell driver. It is
  large but keeps one promotion transaction; extracting it is maintenance
  work, not a correctness prerequisite.
