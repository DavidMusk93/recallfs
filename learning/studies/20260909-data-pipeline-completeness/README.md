# Distributed Data-Pipeline Completeness

> Source: [How we measure data completeness at scale](https://www.datadoghq.com/blog/engineering/data-pipeline-completeness/)
>
> Studied and reproduced on 2026-09-09. This report separates Datadog's
> published claims from mathematical derivation and local executable evidence.

## 1. Conclusion

The article contains a strong architecture pattern worth retaining:

> Completeness is not a property of a counter. It is the discharge status of
> explicit delivery obligations for one cohort under one topology version.

Datadog's segment model, create/ack evidence, failure-independent observer,
dynamic topology, and decision gating form a useful control plane for large
distributed pipelines. The local model reproduces the core idempotency
mechanism and demonstrates why several omitted contracts are mandatory.

The article should not be copied as a complete design. Its public description
does not define the statistical estimator, confidence bounds, late-bucket
finalization, transformation cardinality, or measurement-health protocol.
Without those, a plausible completeness percentage can be mathematically
invalid or operationally unsafe.

## 2. Evidence Summary

| Finding | Status | Evidence |
| --- | --- | --- |
| Create/ack can converge under duplicates and reordering | Reproduced | All event sequences up to length eight |
| Root ingress bucket must be inherited | Reproduced | Cross-bucket create and ack do not match |
| Sequential ratios require cohort conservation | Derived and reproduced | Valid `100 -> 90 -> 81 -> 72`; mismatched chain rejected |
| Volume weighting can hide a failed required branch | Reproduced | Delivery mass `0.99`, required floor `0.0` |
| Create and ack need one sampling decision | Reproduced | Stable result `1.0`; independent result `0.128483` |
| Automation must inspect measurement health | Reproduced | Invalid, stale, undersampled, and unknown-topology signals fail closed |
| Custom storage is cheaper at Datadog scale | Reported only | No public implementation or benchmark |
| Detection usually occurs in under one minute | Reported only | No incident-level measurements |

## 3. The Useful Architecture

The article turns an unbounded end-to-end question into four bounded layers:

```text
payload context
      |
      v
segment evidence -----> time-bucketed evidence store
      |                            |
      v                            v
topology graph --------> path completeness query
                                   |
                                   v
                         human or automation gate
```

### 3.1 Local evidence

Each segment emits create and acknowledgment evidence for a stable payload ID.
The minimum identity is:

```text
(root_bucket, payload_id, segment_id)
```

The root bucket is assigned by a trusted intake clock and inherited across the
path. Using local event arrival time at each hop would place delayed create and
acknowledgment events in different buckets and manufacture loss.

The compact state machine is:

| Stored state | Create seen | Ack seen | Meaning |
| --- | --- | --- | --- |
| `Created` | yes | no | entered, not yet observed leaving |
| `AckBeforeCreate` | no | yes | reordered evidence awaiting create |
| `Complete` | yes | yes | both facts observed |

Duplicate events are no-ops. Acknowledgment-before-create is not discarded.
This gives order-independent local evidence without distributed coordination.

### 3.2 Topology evidence

The topology must be time-versioned separately from payload evidence. A current
service graph cannot correctly interpret an old bucket after routes, ownership,
or branch policy changed.

This graph is operational evidence, not merely visualization. It provides:

- the paths valid for a time bucket;
- the owner of a degraded segment;
- branch semantics and expected successor counts;
- the context needed to explain a scalar result;
- a route from detection to paging and automated mitigation.

### 3.3 Decision gating

The most transferable product pattern is attaching data quality to a query and
letting the consumer fail closed. An autoscaler should not treat a successful
HTTP response as proof that the returned metrics are fit for a decision.

The completeness result therefore needs to be a typed envelope, not a float:

```text
CompletenessEvidence
  value
  expected_obligations
  acknowledged_obligations
  sample_count
  inclusion_probability
  confidence_interval
  root_bucket
  observed_at
  age
  topology_version
  observer_health
  invalidation_state
```

## 4. Sequential Composition: The Missing Proof Obligation

For segment `i`, let:

```text
E_i = unique creates in one root cohort
A_i = matched acknowledgments in the same cohort
c_i = A_i / E_i
```

The article multiplies sequential ratios. That operation is valid only if
adjacent segments conserve the same cohort:

```text
E_(i+1) = A_i
```

Then the terms cancel:

```text
product(c_i)
  = (A_1 / E_1) * (A_2 / E_2) * ... * (A_n / E_n)
  = A_n / E_1
```

The demo verifies a chain:

```text
100 --90%--> 90 --90%--> 81 --88.89%--> 72
```

Its composed result is `72 / 100 = 0.72`.

A superficially similar chain `(100, 90), (100, 90)` fails conservation.
Multiplying `0.9 * 0.9` would report `0.81`, even though the second 100 may be a
different population. The correct behavior is to reject the composition, not
return a number.

This constraint requires more than matching counts. Production systems should
carry a root cohort or lineage identity so equal counts from different
populations cannot pass validation.

## 5. Generalizing from Payloads to Obligations

An `ack/create` ratio assumes one create implies exactly one expected
acknowledgment. Real pipelines violate that assumption:

| Transformation | Required contract |
| --- | --- |
| Filter | Predicate determines zero or one successor obligation |
| Fan-out | One input creates a known set or count of successor obligations |
| Aggregation | A group of inputs creates one output obligation with group lineage |
| Join | Output obligation depends on both input cohorts and join policy |
| Replay | Replay generation must not collide with the original obligation |
| Optional sink | Policy marks the obligation optional rather than silently weighting it down |

The general conservation unit is therefore a delivery obligation:

```text
root_obligation_id
root_bucket
topology_version
predecessor_id
successor_id
expected_successor_count
segment_id
sampling_randomness
inclusion_probability
```

This is an evidence ledger with explicit state inheritance. It can model
payload-preserving segments as the simple one-to-one case while still handling
split, merge, filter, and replay.

## 6. Branches Need Declared Semantics

The article uses a volume-weighted average across parallel branches. That is a
valid answer to:

> What fraction of expected delivery mass was acknowledged?

It is not necessarily an answer to:

> What fraction of source payloads reached every required sink?

The demo's counterexample contains 990 successful optional obligations and ten
failed required obligations:

```text
delivery mass  = 990 / 1000 = 0.99
required floor = min(0 / 10) = 0.00
```

A single 99 percent number would hide total loss of the required branch.
Useful DAG policies include:

| Policy | Meaning |
| --- | --- |
| Weighted mass | Fraction of all expected deliveries observed |
| Required floor | Worst required branch |
| All-required | Fraction of roots for which every required obligation completed |
| Per-sink vector | Completeness retained separately for each consumer |

The consumer, not the aggregation implementation, determines which policy is
safe. Alerting and autoscaling often need a required-sink rule rather than a
global weighted average.

## 7. Sampling Is Part of Correctness

Sampling create and acknowledgment independently breaks pairing. In the
deterministic local experiment, the underlying pipeline acknowledges every
payload:

| Sampling mode | Creates | Measured completeness | Ack without create |
| --- | ---: | ---: | ---: |
| Same propagated seed | 2,087 | 1.000000 | 0 |
| Independent seeds | 1,938 | 0.128483 | 1,861 |

The independent result is false loss caused solely by the observer.

A valid adaptive sampling contract needs:

1. stable randomness or a propagated decision for the entire obligation;
2. the effective inclusion probability recorded with sampled evidence;
3. inverse-probability weighting when probabilities differ;
4. confidence or error bounds, especially for low-volume customers;
5. explicit handling of probability changes during an in-flight cohort;
6. bucket-end flushing for accumulated unsampled weight;
7. separate accounting for dropped observation traffic.

OpenTelemetry's consistent probability-sampling design is relevant because it
propagates common randomness and an effective threshold. Its adjusted count is
the reciprocal of sampling probability. It is a useful reference contract, not
evidence that Datadog uses the same algorithm.

## 8. Watermarks and Completeness Answer Different Questions

The article says arbitrary delay prevents the watermark approach from providing
its required guarantee. Apache Flink documentation agrees that arbitrarily late
events may violate a watermark and that finite waiting limits determinism.

The useful conclusion is not that watermarks are generally unusable:

| Mechanism | Question answered |
| --- | --- |
| Watermark | How far event-time processing is believed to have progressed |
| Allowed lateness | How long results remain open to correction |
| Create/ack evidence | Which declared delivery obligations have discharged |
| Retention/finalization | When unresolved obligations become loss, expired, or unknown |

Create/ack tracking still needs a finalization rule. Datadog reports a few hours
of retention while allowing arbitrary customer delay. Because the root bucket
is assigned only after intake, pre-intake customer delay is outside that
retention problem. Once accepted, however, an acknowledgment arriving after
eviction cannot repair the old state unless a late-correction path exists.

The result should distinguish:

- pending within the latency objective;
- late but still correctable;
- permanently missing after finalization;
- unknown because evidence expired;
- invalid because the observer was unhealthy.

## 9. The Observer Must Not Share the Failure

Datadog's strongest operational principle is:

> The completeness system must outlive the outage it measures.

The article applies this through direct intake/storage, minimal external
dependencies, product tracks, two availability zones, different partitioning
schemes, partition-aware shedding, and manual invalidation.

This is best understood as fate-sharing analysis:

| Potential common cause | Required control |
| --- | --- |
| Monitored Kafka failure | Observer transport does not depend on that Kafka |
| Hot customer | Per-partition shedding and different replica sharding |
| Product traffic surge | Independent product tracks |
| Deployment defect | Staggered versions across replicas |
| Observer overload | Observer-health signal and scoped invalidation |
| Client-library or schema defect | Version visibility, canaries, and independent validation |
| Regional or control-plane fault | Explicitly accepted boundary or additional isolation |

"No external dependencies" is not sufficient by itself. Shared instrumentation
code, schemas, control bulletins, credentials, and regions can still correlate
failure.

## 10. Application to Existing Stream Work

The existing stream-engine notes already distinguish:

```text
highWatermarkOffset
ackedOffset
brokerCommittedOffset
```

and derive:

```text
brokerLag = highWatermarkOffset - brokerCommittedOffset
ackedLag  = highWatermarkOffset - ackedOffset
commitGap = ackedOffset - brokerCommittedOffset
```

That is a strong linear progress model. It prevents a broker-visible committed
offset from being confused with locally processed data. The Datadog pattern
extends the same discipline across service boundaries and branching topology.

It should be adopted only when the required question becomes end-to-end:

- Which accepted records reached the final serving or storage boundary?
- Which segment owns the missing obligations?
- Is the answer valid for this customer, partition, and topology version?
- May an automated consumer act on the returned data?

Adding per-record evidence to every stream path without those concrete
questions would create cost without a defensible semantic contract.

## 11. Recommended Engineering Contract

A production design should enforce these invariants:

1. **Stable identity:** root obligation ID and bucket survive every hop.
2. **Idempotent evidence:** duplicate and reordered events converge.
3. **Explicit cardinality:** every transformation declares successor
   obligations.
4. **Cohort-safe composition:** invalid topology or count joins return unknown,
   never a plausible scalar.
5. **Consistent sampling:** selection identity and probability are inherited.
6. **Statistical honesty:** estimate, sample size, and uncertainty travel
   together.
7. **Independent health:** measurement failure is distinguishable from data
   failure.
8. **Versioned topology:** historical evidence is evaluated against historical
   routing.
9. **Consumer policy:** weighted mass, required sinks, freshness, and fallback
   behavior are explicit.
10. **Fail-closed automation:** stale, invalid, undersampled, or unexplained
    evidence cannot authorize an automated action.

## 12. Deliverables

- `source.md`: source metadata, references, and evidence limitations.
- `exploration.md`: hypotheses, test-first path, counterexamples, and toolchain
  investigation.
- `demo/`: C++20 reference model, scenario program, and 13-test suite.
- `evidence/`: red test, sanitized test output, scenario output, environment,
  and source digests.
- `learning/sources/20260909-data-pipeline-completeness.md`: structured source
  archive.

Build and run commands are in `demo/README.md`.

