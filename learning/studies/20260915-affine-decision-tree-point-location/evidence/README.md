---
doc_id: recallfs-evidence-affine-decision-tree-point-location-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location
depends_on:
  - recallfs-study-affine-decision-tree-point-location-v1
  - recallfs-runbook-oblique-decision-tree-library-v1
  - recallfs-source-affine-decision-tree-point-location-v1
supersedes: []
verified_by:
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/manifest.json
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/review.md
---

# Oblique Decision Tree Evidence Ledger

## Contract

This ledger separates correctness, safety, compatibility, concurrency, and
performance evidence. A result supports only the environment, revision,
commands, inputs, and binary digests recorded in
[`manifest.json`](manifest.json).

The maintained driver is:

```bash
learning/studies/20260915-affine-decision-tree-point-location/verify.sh
```

It removes its dedicated `.tmp/affine-decision-tree/u8-verify` tree, runs
FIL-C before native C, promotes curated raw records only after all required
gates pass, then verifies every promoted digest.

## Result Matrix

| Gate | Required result | Raw evidence |
| --- | --- | --- |
| FIL-C core | 8 library suites plus benchmark-owned exact/checksum self-check pass before native execution | [`u7-filc.txt`](raw/u7-filc.txt) |
| Debug and Release | 10/10 CTest targets pass in each profile | [`u7-native.txt`](raw/u7-native.txt) |
| ASan/UBSan | 10/10 CTest targets plus the benchmark self-check pass | [`u7-native.txt`](raw/u7-native.txt) |
| Static and format | 8 production units plus benchmark pass analyzer; all C/C++ files are formatted | [`u7-static-format.txt`](raw/u7-static-format.txt) |
| Exact protocol | generated data reproduces; C scalar/batch/restored and Python `Fraction` oracle report zero mismatches | [`u7-protocol.txt`](raw/u7-protocol.txt) |
| Fault, corruption, concurrency | focused suites pass; TSan capability is recorded separately | [`u7-safety.txt`](raw/u7-safety.txt), [`u7-limitations.txt`](raw/u7-limitations.txt) |
| Installed package | clean prefix and C11/C++17 consumers pass using exported metadata | [`u7-package.txt`](raw/u7-package.txt) |
| Native benchmark | source, restored, batch, and exact brute-force checksums match; speedup and CI gates pass | [`u8-benchmark.json`](raw/u8-benchmark.json), [`u8-benchmark.txt`](raw/u8-benchmark.txt) |
| Code retention | final binary retains scalar, batch, brute bodies and calls to measured query functions | [`u8-benchmark-disassembly.txt`](raw/u8-benchmark-disassembly.txt) |
| Negative controls | sanitizer, analyzer, formatter, package, disabled sink, semantic corruption, and timeout probes are rejected | [`u7-negative-probes.txt`](raw/u7-negative-probes.txt) |
| Evidence manifest | required stage counts and source/data/binary/raw digests verify | [`manifest.json`](manifest.json) |

## Benchmark Method

The canonical workload has 1,000 sites and 3,102 queries. All timed variants
receive the same query order:

- scalar query against the source generation;
- batch query against the source generation;
- scalar query against the restored generation;
- independent exact brute-force nearest-site lookup.

The exact baseline converts each finite in-domain binary64 coordinate to an
integer on one common power-of-two scale. It compares sums of squared integer
differences with fixed-limb arithmetic and breaks ties by input ordinal. It
does not call the library's exact predicate implementation.

Before timing, all four variants must produce classification checksum
`5c7f9b20aab8aad3`. The driver performs warmup, rotates variant order across
nine measured repetitions, and consumes every output through a volatile
checksum sink. Every warmup and measured pass must also match the
loop-adjusted validated checksum. A separately compiled no-sink binary must
fail its self-check; an injected brute-force classification error must fail
before timing.

The local optimized build uses:

```text
-O3 -march=native -mtune=native -DNDEBUG
```

The machine-readable record reports every raw wall-time sample, median latency
and throughput, paired speedup, and a two-sided 95% interval over log ratios.
The required local gate is median scalar and batch speedup at least `2x`, with
each 95% lower bound above `1.0x`. This is explicitly a steady-state query
gate, not target-machine qualification.
The same record reports source-build and restored-load break-even query counts,
plus total-cost speedup for one 3,102-query corpus; these values prevent query
throughput from hiding construction cost.

Observed values must be read from
[`raw/u8-benchmark.json`](raw/u8-benchmark.json), not copied into a detached
summary. CPU topology, compiler identity, flags, source and binary digests,
frequency limitations, and PMU limitations are in
[`raw/u8-benchmark-environment.txt`](raw/u8-benchmark-environment.txt).

## Named Target

The benchmark workflow first requests access to `fdbd:dc02:e:137::47` with
`orthrus-cli demand`, then attempts batch-mode SSH through jump host `j`.
Observed command output and exit statuses are preserved in
[`raw/u8-target-access.txt`](raw/u8-target-access.txt).

If credentials are denied, local native measurements remain local evidence.
They are not relabeled as target-machine results. Target topology, repository
revision, frequency policy, PMU counters, and throughput remain unknown until
the target can be inspected and the same digest-bound workflow runs there.

## Reconciliation Results

| Anchor | Observation |
| --- | --- |
| `ODT-RA-1` | Validation and fault tests reject bad inputs and preserve null output generations |
| `ODT-RA-2` | Exact tie cases agree across runtime and independent Python oracle |
| `ODT-RA-3` | Canonical build satisfies depth, comparison, fragment, memory, and fallback thresholds |
| `ODT-RA-4` | 3,102 scalar, batch, restored, and oracle results have zero mismatches |
| `ODT-RA-5` | Batch envelope failures are transactional and accepted batches are complete |
| `ODT-RA-6` | Corruption and fault matrices reject every maintained injected failure |
| `ODT-RA-7` | Concurrent reads and installed C/C++ consumers pass their dedicated gates |
| `ODT-RA-8` | Native variants share one checksum; DCE and semantic negative probes fail as designed |

## Raw Record Ownership

`u7-*` names retain the qualification unit that first created those records.
`u8-*` records add benchmark and target-access evidence. The current manifest
binds both groups into one `U7-U8` run and excludes only its own digest to
avoid recursive identity.

`source-digests.txt` identifies the article and inspected upstream revision;
it is historical source evidence, not a benchmark input digest. Current
tracked source, data, binaries, commands, tools, stages, and promoted raw
records are enumerated by `manifest.json`.

## Limitations

- FIL-C covers executed supported paths; it is not a proof or performance
  baseline.
- PMU, fixed-frequency, and NUMA controls may be unavailable and are recorded
  rather than silently substituted.
- Local runs without a supported CPU-affinity mechanism are marked
  `local-unpinned-non-target`; their timing remains bounded local evidence.
- The benchmark measures repeated lookup over one static corpus. It does not
  measure application publication/reclamation, update cadence, crash recovery,
  or a database execution engine.
- No result is a DuckDB benchmark or a reproduction of the article's `59x`
  claim.
- Current review closure is recorded in [`review.md`](review.md).
