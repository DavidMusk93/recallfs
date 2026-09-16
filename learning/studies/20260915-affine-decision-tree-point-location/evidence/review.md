# U8 Code Review Closure

Review date: 2026-09-16.

Review run: `20260916-180422-cc301747`.

## Coverage

Seven independent local reviewer contexts covered correctness, project
standards, testing, maintainability, performance, reliability, and adversarial
failure scenarios. The cross-model pass did not run because the host serving
family could not be attested, so no code was sent to an external provider.

The review covered the U8 benchmark, CMake registration, qualification driver,
production integration guide, study explanation, and evidence ledger.
`projects/clipvault` was an unrelated pre-existing worktree change and was
excluded.

An independent validator accepted ten merged findings and rejected two
file-length-only findings because length alone did not establish a defect and
the implementation plan explicitly left internal source splitting open.

## Applied Findings

| Finding | Severity | Resolution |
| --- | --- | --- |
| Evidence manifest became stale after review edits | P1 | Final qualification regenerates and independently verifies all promoted digests |
| README example discarded successful outside classification | P1 | Example now returns the complete `odt_query_result` |
| Local result could be read as named-target qualification | P1 | Final status and manifest distinguish local unpinned evidence from degraded target access |
| Evidence ledger was outside source digest inventory | P1 | Ledger, review, and simplification documents are tracked inputs |
| Benchmark-owned C bypassed safety tools | P1 | Added fast exact/checksum self-check under FIL-C and ASan/UBSan |
| Prior review closure was stale | P2 | Replaced it with this U8 review record |
| Local benchmark lacked affinity semantics | P2 | Linux uses recorded CPU/NUMA binding when available; unsupported hosts are explicitly non-target-qualifying |
| Query-only speedup hid construction amortization | P2 | Added source-build/restored-load break-even and canonical total-cost fields |
| Target access could hang | P2 | Added process-group timeout and a sleeping-command negative probe |
| Timed work could diverge after preflight validation | P2 | Every warmup and measured checksum is checked; disassembly is validated per measurement body |

## Simplification Decisions

The benchmark keeps parsing, independent exact arithmetic, measurement, and
reporting in one private executable. A proposed multi-file split was rejected:
the validator found no behavioral defect, and introducing new private
interfaces would increase change surface during evidence stabilization.

The qualification driver also remains one fail-closed entrypoint. Splitting it
would not reduce the number of ordered gates or their shared promotion state.

## Validation Contract

Review closure is valid only when the final
[`manifest.json`](manifest.json) reports every required stage passed and its
source digest for this file matches. The definitive command and observed counts
are recorded in [`raw/u7-commands.txt`](raw/u7-commands.txt) and the manifest.

## Residual Risks

- Named target `fdbd:dc02:e:137::47` was unavailable during this run.
- The local macOS benchmark has no supported CPU-affinity, NUMA, fixed-frequency,
  or PMU control and is marked non-target-qualifying.
- Malformed benchmark CSV and injected allocation/clock/report-write failures
  are not separately fault-injected; canonical parsing, exact arithmetic,
  checksum, sink, semantic corruption, and timeout paths are covered.

## Verdict

Ready to commit only after the final qualification run regenerates the
manifest and reports zero digest mismatches. No actionable review finding may
remain outside that gate.
