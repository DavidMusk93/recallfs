# Exploration Log

## 1. Research Questions

The article's architecture is useful, but its public description leaves several
semantic assumptions implicit. This study tests the smallest mechanisms that
can be evaluated without Datadog's private implementation.

| Question | Evidence method |
| --- | --- |
| Is create/ack state independent of duplicate delivery and event order? | Exhaustive transition tests |
| When does multiplication of sequential segment ratios equal an end-to-end ratio? | Conservation check plus constructive counterexample |
| What does a weighted branch percentage omit? | Delivery-mass and required-branch comparison |
| Can create and ack be sampled independently? | Stable-versus-independent sampling counterexample |
| What must accompany a completeness percentage? | Invalid measurement and stale-result tests |

## 2. Initial Model

The experimental model separates three layers:

```text
evidence events -> segment state -> topology composition -> decision signal
```

The evidence identity is:

```text
(root_bucket, payload_id, segment_id)
```

`root_bucket` must be inherited from intake. Re-bucketing an acknowledgment by
its local arrival time would split the evidence pair and manufacture loss.

For a sequential chain, the multiplication rule is accepted only when adjacent
counts conserve the same cohort:

```text
created[i + 1] == acknowledged[i]
```

For branches, the experiment reports two different quantities:

- delivery-mass completeness: weighted acknowledged obligations divided by
  expected obligations;
- required-branch floor: the least complete required branch.

Neither is universally "the" completeness of a DAG. The topology contract must
select semantics appropriate to the consumer.

## 3. Planned Negative Cases

1. Acknowledgment arrives before create, followed by duplicate events.
2. The same payload identifier appears in two root buckets.
3. Adjacent sequential segments contain different cohorts.
4. A low-volume required branch fails while the volume-weighted result remains
   high.
5. Create and acknowledgment use different sampling decisions.
6. A percentage is present while the measurement system marks the bucket
   invalid or stale.

## 4. Scope Boundary

This is a correctness study, not a performance reproduction. The article does
not publish enough implementation detail to reproduce its throughput, memory,
or cost claims. The demo therefore uses portable C++ and CMake to test state
and algebraic properties; it does not benchmark the local machine or extrapolate
to Datadog scale.

## 5. Test-First Execution

The public interface and tests were written before the model implementation.
The first build compiled the test translation unit and failed at link time on
every missing behavior:

```text
Undefined symbols:
  completeness::SegmentTracker::observe_create(...)
  completeness::SegmentTracker::observe_acknowledgment(...)
  completeness::compose_sequential(...)
  completeness::summarize_branches(...)
  completeness::stable_sample(...)
  completeness::safe_for_automation(...)
```

The failure is retained in `evidence/red-test.txt`. Implementation then added
only the behavior required by the tests.

## 6. State Model

One identifier has two independent evidence bits: create seen and
acknowledgment seen. The compact three-state representation excludes the empty
state because absent identifiers are not stored:

```text
                      ack
     +------------------------------------+
     |                                    v
+----+----+        create          +------+------+
| created | ---------------------> |  complete   |
+----+----+                         +------+------+
     ^                                    ^
     |                                    |
     +------------------------------------+
                    create
             +------+------+
             | ack-before  |
             +------+------+
                    ^
                    |
                   ack
```

Duplicate create or acknowledgment events do not change state. The exhaustive
short-sequence test enumerates every create/ack sequence of length one through
eight and confirms that final state depends only on which evidence bits have
appeared, not their order or multiplicity.

## 7. Composition Results

For segment `i`, define:

```text
E_i = unique creates for the root cohort
A_i = unique matched acknowledgments for the same cohort
c_i = A_i / E_i
```

For a sequential path, multiplication is valid only if every adjacent boundary
conserves the cohort:

```text
E_(i+1) = A_i
```

Under that condition:

```text
product(c_i)
  = (A_1 / E_1) * (A_2 / E_2) * ... * (A_n / E_n)
  = A_n / E_1
```

The valid test chain `100 -> 90 -> 81 -> 72` produces `0.72`. The mismatched
chain `(100, 90), (100, 90)` is rejected. Multiplying its two marginal ratios
would produce `0.81`, but the second denominator does not identify the 90 items
acknowledged by the first segment, so the scalar has no end-to-end meaning.

For branches, the demo distinguishes:

```text
delivery mass = sum(acknowledged obligations) / sum(expected obligations)
required floor = min(required branch ratios)
```

The counterexample has 990 successful optional obligations and ten failed
required obligations. Delivery mass is `0.99`; required floor is `0.0`.

## 8. Sampling Results

The sampling experiment uses a deterministic hash of payload ID and propagated
seed. With the same seed at create and acknowledgment:

```text
sampled creates = 2087
completeness    = 1.000000
```

With independent seeds at the two observation points, while the underlying
pipeline still acknowledges every payload:

```text
sampled creates                  = 1938
matched completeness             = 0.128483
acknowledgments without creates  = 1861
```

This is a constructive false-loss example. A sampling ratio alone is
insufficient; the decision identity or randomness and the effective inclusion
probability must propagate with the payload. OpenTelemetry's consistent
probability sampling uses the same general contract: common randomness plus a
propagated threshold, with adjusted count equal to the reciprocal probability.

The experiment does not validate Datadog's unpublished weighting method.
Dynamic or unequal probabilities additionally require a defined estimator,
variance or confidence interval, and end-of-bucket handling for accumulated
weight that has no later sampled event.

## 9. Toolchain Investigation

The initial Apple Clang test binary could not start:

```text
Library not loaded: @rpath/libc++.1.dylib
Reason: no LC_RPATH's found
```

`otool -L` showed that `/usr/local/lib/libc++.1.dylib` had shadowed the macOS
SDK stub during linking. Two paths were checked:

- Homebrew GCC 15 built and passed all tests without sanitizers.
- Apple Clang with the active SDK `usr/lib` placed first built and passed under
  AddressSanitizer and UndefinedBehaviorSanitizer.

The exact verified command is in `evidence/local-environment.txt`.

## 10. Evidence Boundary

- The state, composition, branch, sampling, and automation results are
  reproduced for this reference model.
- They are not a test of Datadog's private code.
- The article's scale, memory, cost, detection-latency, and incident claims
  remain reported claims because no raw evidence or implementation is public.
- The tests establish necessary contracts and counterexamples. They do not
  prove that these contracts are sufficient for every data transformation.

## 11. Review Correction

The final review found that the original `require_near` helper accepted `NaN`:
`abs(NaN - expected) > tolerance` is false. A dedicated test reproduced the
silent pass before the helper was changed to reject non-finite operands and a
negative tolerance. The same test unit now directly exercises completeness
threshold, `ObserverDegraded`, non-finite value, out-of-range value, and exact
policy-boundary behavior. The sanitized suite increased from 13 to 14 tests.
