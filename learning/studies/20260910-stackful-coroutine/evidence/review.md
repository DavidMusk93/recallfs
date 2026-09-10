# Code Review Closure

| Field | Value |
| --- | --- |
| Run ID | `20260910-155329-04cb4f1c` |
| Reviewed head | `b4bd61477b34d7e61da8c290811a51e3f63879ef` |
| Review status | Complete |
| Initial verdict | Ready with fixes |
| Validated findings | 5 |
| Fix commit | `ecda6780273a57adcde8e6c94754563f61671e64` |
| Raw receipt | [`raw/code-review.json`](raw/code-review.json) |

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
