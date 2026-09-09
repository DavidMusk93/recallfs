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

