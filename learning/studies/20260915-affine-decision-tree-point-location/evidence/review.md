# Code Review

Review date: 2026-09-15.

Review run: `20260915-112533-cb95baf8`.

## Coverage

The review used independent in-process contexts for correctness, project
standards, testing, maintainability, performance, reliability, and adversarial
failure analysis. The cross-model pass did not run because the host serving
family could not be attested; no code was sent to an external reviewer.

`projects/clipvault` was an unrelated pre-existing worktree change and was
excluded.

All six merged findings were independently validated before fixes:

| Finding | Severity | Resolution |
| --- | --- | --- |
| Verification could pass against stale or unbound binaries | P1 | Added one manifest-checked `verify.sh`; it builds the tested CLI in the same run and records commands and binary digests |
| Finite operands could produce a non-finite score | P2 | Added `ODT_NUMERIC_RANGE`, result-preservation semantics, and a `DBL_MAX` regression |
| Near-bisector rounding could disagree with exact nearest site | P2 | Defined binary64 evaluator semantics, added an 8-ULP uncertainty contract and deterministic boundary neighborhoods |
| Evidence omitted the governing study dependency | P2 | Added the study doc ID and reran the DAG check |
| Machine mode hid stream I/O failures | P2 | Added read/write/flush checks and CLI error-path tests |
| Study body violated the required contract order | P3 | Moved inputs, ownership, invariants, and failures before worked examples |

Actionable findings after fixes: none.

## Validation

The post-fix aggregate verification passed:

- source manifest: 15/15;
- FIL-C: 12 location anchors and 8 failure checks;
- Zig 0.16.0 Release: C test and fresh CLI passed;
- Zig ASan/UBSan: passed;
- pyenv CPython 3.13.12 + uv: 3/3 tests, including 20,054 differential cases;
- CMake/CTest: 1/1;
- clang-format: passed;
- document DAG: 38 unique IDs, 5 study documents, no cycles.

## Residual Risk

- Output and flush failure checks are implemented but not fault-injected because
  macOS stdio and FIL-C do not expose one shared deterministic mechanism.
- ULP-band points intentionally have no exact-nearest equality guarantee.
- No local performance benchmark was run, so no speedup claim is made.

## Verdict

Ready to commit. No actionable findings remain.
