# Simplification Review

Review date: 2026-09-15.

Three independent reviewer contexts inspected the C and Python implementation.

| Lens | Findings | Applied |
| --- | ---: | ---: |
| Reuse | 2 | 2 |
| Code quality | 3 | 3 |
| Efficiency | 3 | 3 |

Applied changes:

1. Stream CSV rows through `csv.writer.writerows`.
2. Remove the redundant `subprocess.run(check=False)` default.
3. Remove Python's tree-shaped comparison-count oracle; Python now checks only
   independently derived nearest-site regions.
4. Return `enum odt_status` instead of raw `int` from public status APIs.
5. Describe CLI input as whitespace-separated rather than line-separated.
6. Reuse the validator's node workspace as its traversal stack after indegree
   checks, reducing validation workspace by about nine bytes per node.
7. Stream generated CLI cases instead of materializing user-requested counts.
8. Execute the configured test binary directly and convert `OSError` to a test
   failure instead of using a separate existence check.

No suggested change was skipped. FIL-C, Zig Release, ASan/UBSan, CMake/CTest,
format, and the 20,012-point differential test passed after the changes.
