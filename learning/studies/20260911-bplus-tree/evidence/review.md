---
doc_id: recallfs-review-bplus-tree-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-study-bplus-tree-v1
  - recallfs-evidence-bplus-tree-v1
supersedes: []
verified_by:
  - review run 20260911-183036-ed93668c
  - post-review full regression
---

# Code Review Closure

## Scope

- Base: `0a7dc4d`
- Generic implementation: `25f09fe`
- Public option cleanup: `d114cc2`
- Simplification: `3f72391`
- Package-consumer test: `b2c4bd8`
- Generic review fixes: `07914cc`, `f2c2949`, `5c936d1`
- Review run: `20260911-183036-ed93668c`
- Reviewed scope: `learning/studies/20260911-bplus-tree/**`
- Excluded: interleaved coroutine commits and `projects/clipvault`

## Review Coverage

The review used independent specialist contexts for correctness, project
standards, testing, maintainability, performance, public API contract,
reliability, security, and adversarial behavior. The cross-model adversarial
route did not run because the host serving family could not be attested; the
local adversarial reviewer covered that lens without claiming cross-model
corroboration.

The merge produced nine candidates after combining two reports of the same
odd-capacity split defect. A fresh validator accepted four current findings,
rejected three as pre-existing or outside the documented error precedence, and
routed two non-blocking architecture/performance concerns to residual risk.

## Applied Findings

| Finding | Resolution |
| --- | --- |
| Odd-capacity right-edge split underfilled the new leaf | Remove the position-dependent split adjustment and add a 13-byte/21-byte ascending split, validate, and reopen regression |
| Comparator callback could commit a nested mutation | Track comparator activity, return `BTREE_BUSY` from same-tree status APIs, and test that no nested key is installed |
| Native evidence used an undeclared system compiler | Run Debug, Release, sanitizer, install, consumer, and C++ checks through pinned Zig 0.16.0 and record its archive SHA-256 plus full compile commands |
| `scan-build` could report success without instrumented compilation | Configure from a clean tree through `scan-build`, assert `CMAKE_C_COMPILER` is `ccc-analyzer`, and use `--status-bugs` |

The security reviewer also found a pre-existing namespace hazard: the database
path followed a final-component symlink while WAL files were opened relative to
the lexical parent. The final implementation opens the data basename and both
sidecars through one retained directory descriptor with `O_NOFOLLOW`, rejects
non-regular files, and covers symlink rejection plus relative reopen.

## Disposition

- Callback-time close behavior predates the generic revision. The public header
  now explicitly forbids destroying the active tree from scan or comparator
  callbacks; status-returning reentry is rejected.
- Invalid option layouts return `BTREE_INVALID_ARGUMENT`. Only otherwise valid
  options that differ from persisted widths or comparator identity return
  `BTREE_SCHEMA_MISMATCH`; the runbook now states this precedence.
- Reusing page scratch and splitting the 2,108-line core remain non-blocking
  optimization/maintenance candidates. Neither has a hotspot benchmark or
  demonstrated correctness defect, so no speculative refactor was applied.
- Physical power-loss, controller-cache, network-filesystem, concurrent-access,
  and exhaustive syscall-failure qualification remain outside the stated
  production boundary.

## Verification

After all fixes:

- Zig 0.16.0 Debug CTest: 8/8 passed.
- Zig 0.16.0 Release CTest: 8/8 passed.
- Zig 0.16.0 ASan/UBSan CTest: 8/8 passed.
- FIL-C 0.684: 8/8 standalone suites passed.
- Clang static analyzer: `ccc-analyzer` binding confirmed; no bugs found.
- `clang-format --dry-run --Werror`: passed.
- Installed `find_package` C11 consumer and Zig C++17 header compile: passed.
- Every raw evidence digest and every source digest verifies.

Actionable findings remaining: none.
