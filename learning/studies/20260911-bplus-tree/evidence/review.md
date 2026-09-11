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
  - RBT-RA-8
  - review run 20260911-221757-4574fb5e
  - post-review full regression
---

# Code Review Closure

## Scope

| Item | Revision |
| --- | --- |
| Base | `10c0875` |
| Typed slotted-page implementation | `938f510` |
| Restored production regression gates | `0892d30` |
| Large transaction indexing | `97fbe20` |
| Ownership/overflow/backend fixes | `ed68923` |
| Allocation-invariant fixes | `6d82359` |
| Final source | `6d823594103237e372cfe844ac0cac12fb187e3d` |
| Review run | `20260911-221757-4574fb5e` |

The reviewed implementation is the clean-break `rbt` 0.1.0 API and sole
`RBT_FORMAT_VERSION=1` format. Historical `btree` files are intentionally
rejected; compatibility readers, migration layers, aliases, and alternate
formats were outside the reviewed design.

## Review Coverage

The run covered correctness, public API and ownership, persisted schema and key
encoding, slotted-page and overflow invariants, mutation locality, provider
atomicity and leases, file/WAL recovery, corruption handling, package
consumption, tests, maintainability, security, and analyzer/toolchain evidence.

Cross-model review was unavailable because the host serving family was
un-attestable. The run therefore records no cross-model corroboration and does
not relabel same-family review as independent.

## Disposition

Fourteen validator-accepted findings were fixed and closed. The fixes covered
the validated ownership, overflow replacement, page-set validation, allocation
overflow/initialization, backend lease and poison behavior, WAL bounds,
corruption checks, locality assertions, and API failure semantics found during
the run.

The validator rejected two candidates:

| Candidate | Disposition |
| --- | --- |
| Split the core monolith | rejected as a maintainability preference without a demonstrated correctness defect |
| Add another backend-poison finding | rejected because poisoning was already covered by the contract and tests |

Backend poisoning was nevertheless hardened in `ed68923`: unknown publication
returns `-EOWNERDEAD`, both backend and tree remain poisoned, and only
close/reopen recovery re-establishes a usable state.

`6d82359` then made allocation invariants explicit by validating replacement
cell images locally, zero-initializing transaction page arrays, and allocating
initial schema pages as one checked contiguous block. This removed analyzer
ambiguity without changing the format or success semantics.

## Verification

- Zig 0.16.0 Debug CTest: 8/8 passed.
- Zig 0.16.0 Release CTest: 8/8 passed.
- Zig 0.16.0 ASan/UBSan CTest: 8/8 passed.
- FIL-C 0.684: 8/8 standalone suites passed.
- `ccc-analyzer`: binding confirmed; no bugs found.
- `clang-format --dry-run --Werror`: passed.
- Installed typed `find_package(rbt 0.1 CONFIG REQUIRED)` consumer: passed.
- C++17 public-header compile: passed.
- Raw evidence SHA-256 ledger and source digest ledger: recorded at final
  source commit.

This closes `RBT-RA-8`. No actionable findings remain.

## Production Boundary

The review does not certify real power-loss or controller-cache behavior,
network filesystems, multi-thread or multi-process access to one live tree,
online backup, or performance. Process-crash injection and locality page counts
must not be presented as those missing qualifications or as benchmark results.
