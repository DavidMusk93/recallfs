# Source

| Field | Value |
| --- | --- |
| Primary URL | `https://www.datadoghq.com/blog/engineering/data-pipeline-completeness/` |
| Title | How we measure data completeness at scale |
| Authors | Valentin Touffet and Alexandre Olivier |
| Publisher | Datadog Engineering |
| Published | 2026-07-01 |
| Access date | 2026-09-09 |
| Archive | `learning/sources/20260909-data-pipeline-completeness.md` |
| Topic | Distributed data-pipeline completeness |

## Primary Evidence

| Reference | Role |
| --- | --- |
| [Datadog engineering article](https://www.datadoghq.com/blog/engineering/data-pipeline-completeness/) | Architecture, operational goals, scale statements, and aggregation rules |
| [Apache Flink timely stream processing](https://nightlies.apache.org/flink/flink-docs-stable/docs/concepts/time/) | Watermark and late-event semantics |
| [OpenTelemetry consistent probability sampling](https://opentelemetry.io/docs/specs/otel/trace/tracestate-probability-sampling/) | Propagated randomness, thresholds, and adjusted counts |
| [Penn State STAT 506: unequal probability sampling](https://online.stat.psu.edu/stat506/Lesson03) | Inclusion probabilities and inverse-probability estimators |

## Evidence Classification

| Class | Meaning in this study |
| --- | --- |
| Reported | A Datadog architecture or scale statement without public reproduction material |
| Derived | A mathematical consequence with its assumptions stated |
| Reproduced | Behavior exercised by the local demo and automated tests |
| Recommended | A design contract inferred from the evidence and counterexamples |

## Source Limitations

- The primary page was available and inspected in a browser. This repository
  stores a structured archive rather than a verbatim copyrighted copy.
- Datadog does not publish the implementation, schemas, estimator, test suite,
  raw incidents, or benchmark data needed to reproduce its scale and cost
  claims.
- The study therefore tests the public model's semantics, not Datadog's private
  implementation.
- The OpenTelemetry probability-sampling specification is marked Development.
  It is used as a concrete consistency design, not as evidence of Datadog's
  implementation.
- The Flink documentation explicitly permits late events after a watermark.
  Watermarks communicate progress under a lateness policy; they are not proof
  that no older event can ever arrive.

