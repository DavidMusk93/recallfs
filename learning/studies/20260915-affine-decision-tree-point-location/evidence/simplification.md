# U8 Simplification Review

Review date: 2026-09-16.

Three independent reviewer contexts inspected the benchmark and qualification
driver for reuse, code quality, and efficiency.

| Lens | Findings | Applied | Skipped |
| --- | ---: | ---: | ---: |
| Reuse | 3 | 1 | 2 |
| Code quality | 3 | 2 | 1 |
| Efficiency | 3 | 2 | 1 |

Applied changes:

1. Restrict untracked evidence exceptions to the exact promoted U8 records.
2. Build the disabled-sink negative probe through the benchmark's CMake target
   so warning and optimization flags cannot drift.
3. Keep benchmark pass functions private while retaining no-inline symbols for
   disassembly inspection.
4. Validate scalar, restored, and exact results in one pass and retain only the
   batch output array.
5. Build only `odt_benchmark` in benchmark-specific CMake configurations and
   retain only the three relevant disassembly slices.

Skipped changes:

- Shared CSV helpers were not extracted from `odt_example.c`; their validation
  contracts differ, and merging them would broaden a settled evidence path.
- The benchmark report was not wrapped in another aggregate object; this would
  move parameter count without reducing report fields.
- Iteration-bound internal environment variable names remain private to
  `verify.sh`; renaming them would be mechanical churn without changing the
  manifest schema or evidence filenames.
- Target access retains explicit status handling rather than using
  `run_logged`, because expected denial and timeout are evidence rather than
  fatal command failures.

Behavior-preserving checks include benchmark build, exact/checksum self-check,
disabled-sink failure, canonical 3,102-query checksum, shell syntax, formatting,
and ASCII graph validation. The final qualification run is authoritative.
