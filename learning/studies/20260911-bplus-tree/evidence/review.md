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
  - review run 20260911-143717-b9a08456
  - post-review full regression
---

# Code Review Closure

## Scope

- Base: `db65f4c`
- Initial implementation commit: `bafd0e0`
- Review fixes: `d388274`
- WAL bound follow-up: `0f6c17b`
- Public header clarification: `fe17463`
- Review run: `20260911-143717-b9a08456`
- Reviewed scope: `learning/studies/20260911-bplus-tree/lib`
- Excluded: unrelated concurrent coroutine changes and untracked study docs

## Review Coverage

The review used independent specialist contexts for correctness, project
standards, testing, maintainability, performance, public API contract,
reliability, security, and adversarial behavior. The cross-model adversarial
route did not run because the host serving family could not be attested; the
local adversarial reviewer covered that lens without claiming cross-model
corroboration.

The mechanical merge produced 22 semantically distinct candidates after
deduplicating repeated crash-test, WAL-publication, and close-on-exec reports.
A fresh validator accepted 12 and rejected 10 as unsupported preferences,
explicit non-goals, or scenarios outside the stated operating contract.

## Applied Findings

| Finding | Resolution |
| --- | --- |
| Default CTest skipped crash recovery | Build a hook-enabled test-only file backend and always register the Unix crash suite |
| Storage ownership was underspecified | Document borrowed provider lifetime and synchronous callback-buffer lifetime in the public header |
| Recovery-required was underspecified | Document commit-unknown output and mandatory close/reopen inspection |
| WAL allocation was unbounded | Validate the fixed header first and cap one transaction at 256 page images |
| Relative WAL paths drifted after `chdir` | Bind WAL operations to the opened parent directory with `openat`/`renameat`/`unlinkat` |
| macOS durability barrier was too weak | Use `F_FULLFSYNC` for regular files and directory `fsync` for namespace changes |
| Crash tests used only an in-place leaf update | Force page-growing root split at every commit and replay crash point |
| Future WAL versions returned corruption | Return `BPT_UNSUPPORTED` after checksum-valid compatibility parsing |
| Descriptors survived `exec` | Use `O_CLOEXEC` plus an `FD_CLOEXEC` fallback and a fork/exec lock regression |
| Scan callback could mutate live topology | Reject nested scan and callback-time put/delete with `BPT_BUSY` |
| Memory storage grew one page at a time | Add checked geometric capacity growth |
| Locality proof used a root leaf only | Assert exact metadata-plus-leaf writes on a multi-level tree |

The WAL publication fix also changed the protocol from direct active-WAL writes
to `<db>.wal.tmp` plus durable atomic rename. Open removes only that known
unpublished temporary sidecar. This closes the partial-WAL recovery dead end
identified during review.

## Rejected Findings

- Splitting the two large C files was a maintainability preference without a
  demonstrated correctness defect.
- Full validation during open is an intentional design contract.
- Multi-operation batching is an explicit v1 non-goal and had no measured
  performance requirement.
- A stale same-UUID WAL and online pathname replacement require external file
  manipulation outside the supported ownership protocol.
- The existing CMake toolchain interface is sufficient for explicit FIL-C
  invocation; the actual FIL-C gate is recorded in the evidence ledger.
- A scan callback may propagate any `bpt_status`; only tree mutation and nested
  scan are forbidden during callback execution.

## Verification

After all fixes:

- Debug CTest: 7/7 passed.
- Release CTest: 7/7 passed.
- ASan/UBSan CTest: 7/7 passed.
- FIL-C 0.684: 7/7 standalone suites passed.
- Clang static analyzer: no bugs found.
- `clang-format --dry-run --Werror`: passed.
- Installed-header C11 consumer and C++17 header compile: passed.

Actionable findings remaining: none.
