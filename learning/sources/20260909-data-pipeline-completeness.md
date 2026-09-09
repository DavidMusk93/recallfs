# How Datadog Measures Data Completeness at Scale

> Structured source archive, not a verbatim copy.

## Metadata

| Field | Value |
| --- | --- |
| Original URL | `https://www.datadoghq.com/blog/engineering/data-pipeline-completeness/` |
| Title | How we measure data completeness at scale |
| Authors | Valentin Touffet and Alexandre Olivier |
| Publisher | Datadog Engineering |
| Published | 2026-07-01 |
| Accessed | 2026-09-09 |
| Topic | Real-time, per-customer completeness measurement for distributed ingestion pipelines |

## Problem Definition

Datadog defines completeness as every payload entering its ingestion system
becoming available to the customer. A payload may be a metric point, log, span,
or another telemetry item.

The system must answer two questions:

1. Is a customer's data complete now?
2. If it is not, where in the ingestion topology did completeness degrade?

The article says this must work across hundreds of services and paths, including
customer-specific routing, partitioning, loops, replay, and branches.

## Architecture Described by the Article

### Segment model

Pipelines are decomposed into segments both inside and between services. An
example path is:

```text
intake-in -> intake-out -> processing-in -> processing-out -> router
```

Each segment is measured locally. The local measurements are then composed into
an end-to-end result.

### Create and acknowledgment events

For a payload identifier and segment:

- a create event records entry into the segment;
- an acknowledgment records exit from the segment;
- duplicate events are ignored;
- acknowledgment-before-create is retained as a valid intermediate state.

State is scoped to a time bucket derived from a Datadog-controlled timestamp
assigned when the payload first enters the system. The article says this makes
counting idempotent without cross-payload coordination.

For one segment and bucket, the presented ratio is:

```text
segment completeness = acknowledgments / creates
```

### Sequential and parallel composition

The article multiplies ratios along sequential paths:

```text
pipeline completeness = product(segment completeness)
```

Parallel branches are combined with a volume-weighted average. The example has
a 60 percent branch whose sequential completeness is approximately 94 percent
and a 40 percent branch at 100 percent, producing approximately 96 percent
overall completeness.

### Adaptive sampling and load shedding

The system does not retain every payload identifier. A client library samples
payloads per customer and segment according to a centrally distributed
load-shedding bulletin.

For unsampled payloads, the client accumulates a weight and attaches it to the
next sampled create event. Bulletin calculation has two inputs:

1. storage nodes compute stable partial recommendations over a longer window;
2. intake incorporates current in-flight traffic so sudden surges can be
   handled before they reach storage.

### Storage

The article describes a custom in-memory store with:

- 60-second buckets;
- short retention measured in hours;
- worker-per-shard ownership;
- single-threaded event loops per worker;
- compact per-identifier idempotency state;
- no general-purpose external database.

The article reports hundreds of millions of identifiers per second and dozens
of terabytes of short-lived state. It does not publish a benchmark, schema,
memory-accounting breakdown, or implementation.

### Failure independence

The completeness system avoids Kafka and other ingestion-path dependencies so
that it can remain available during failures of the system it observes.

Deployments are split by product into independent "tracks". Each track runs in
two availability zones. The zones use different sharding schemes so that a
single hot customer is less likely to overload equivalent neighbors in both
copies.

The intake also has a partition-aware last-resort rate limiter. Operators can
invalidate time ranges when the measurement system, rather than the data
pipeline, is known to be unhealthy.

### Dynamic topology and consumers

A separate, time-bucketed graph records services as nodes and segments as
edges. It supports path discovery, ownership lookup, and completeness queries
against the topology that existed during the requested interval.

Completeness is consumed by humans and automated systems. The article gives
Kubernetes Autoscaling as an example: a query carries completeness information
so the consumer can avoid acting on stale or incomplete metrics.

## Claims Worth Testing

| Claim | Required validation |
| --- | --- |
| Duplicate and reordered create/ack events are idempotent | Exhaustive state-transition tests |
| Sequential ratios compose by multiplication | Cohort-conservation proof and mismatch counterexample |
| Weighted branch aggregation represents completeness | Explicit branch semantics and critical-branch counterexample |
| Dynamic sampling preserves accuracy | Stable sampling identity, inclusion probabilities, variance, and tail handling |
| Time buckets handle arbitrary delay | Retention, late correction, and bucket-finalization contract |
| Measurement is trustworthy during incidents | Independent health and invalidation signal |

## Information Not Published

The article does not provide:

- event schemas or identifier construction;
- the exact bucket-finalization and late-arrival policy;
- the estimator used when sampling ratios change;
- confidence intervals or minimum sample-size rules;
- how a sampling decision remains consistent across create and acknowledgment;
- semantics for filter, split, join, or aggregation stages;
- how a final acknowledgment proves customer queryability;
- source code, benchmark methodology, or raw incident evidence.

Those omissions are treated as unknowns, not inferred implementation defects.

