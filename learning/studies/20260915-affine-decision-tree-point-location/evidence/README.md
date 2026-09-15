---
doc_id: recallfs-evidence-affine-decision-tree-point-location-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location
depends_on:
  - recallfs-study-affine-decision-tree-point-location-v1
  - recallfs-source-affine-decision-tree-point-location-v1
  - learning/studies/20260915-affine-decision-tree-point-location/demo/README.md
supersedes: []
verified_by:
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/raw/verification.txt
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/review.md
---

# Evidence

Validation date: 2026-09-15.

## Result Matrix

| Gate | Toolchain | Result | Raw evidence |
| --- | --- | --- | --- |
| Source identity | browser DOM, HTTP SHA-256, GitHub commit | article metadata/body extracted; dynamic wrapper recorded; apart pinned to `470d3a3` | [`source-digests.txt`](raw/source-digests.txt) |
| Local input identity | tracked SHA-256 manifest | all 15 C/Python/build/verification inputs match before execution | [`verification.txt`](raw/verification.txt) |
| C safety/UB | FIL-C 0.684, Clang 20.1.8 | 12 location anchors and 8 failure checks pass | [`verification.txt`](raw/verification.txt) |
| Fixed LLVM build | Zig 0.16.0, `-O2` | fresh test and CLI binaries built, hashed, and run | [`verification.txt`](raw/verification.txt) |
| Sanitizers | Zig 0.16.0, ASan + UBSan | all C checks pass | [`verification.txt`](raw/verification.txt) |
| Differential oracle and CLI | pyenv CPython 3.13.12, uv 0.10.0 | 20,054 cases checked; 2 CLI failure tests pass against the freshly built Zig CLI | [`verification.txt`](raw/verification.txt) |
| CMake interface | CMake 4.0.3, Ninja 1.13.1 | clean configure/build succeeds; CTest 1/1 | [`verification.txt`](raw/verification.txt) |
| Formatting | clang-format 16.0.6 | no format drift | [`verification.txt`](raw/verification.txt) |
| Document DAG | Ruby YAML parser | 38 unique IDs; 5-document study subgraph resolves with no cycles | [`verification.txt`](raw/verification.txt) |
| Simplification | three independent review contexts | 8/8 findings applied | [`simplification.md`](simplification.md) |
| Code review | seven reviewer contexts + one validator | 6/6 findings fixed; no actionable residuals | [`review.md`](review.md) |

FIL-C validates memory safety and undefined behavior only on executed paths. It
is not a proof of all possible tree sizes or floating-point values.

The aggregate verification output records the exact tool paths and versions,
the Zig executable digest, every command through shell xtrace, and every
generated binary digest. [`source-digests.txt`](raw/source-digests.txt) remains
separate because it identifies the article and upstream implementation rather
than the local C/Python inputs.

## Reconciliation Results

| Anchor | Observation | Status |
| --- | --- | --- |
| `ODT-RA-1` | A/B/C sites return `0/1/2`; every internal point takes 6 comparisons | pass |
| `ODT-RA-2` | three pairwise bisector midpoints return A/A/B under `>=` tie policy | pass |
| `ODT-RA-3` | four outside directions return `outside` after 1/2/3/4 comparisons | pass |
| `ODT-RA-4` | 20,000 seeded random points plus 54 deterministic cases satisfy the exact-oracle uncertainty contract | pass |
| `ODT-RA-5` | bad topology, non-finite weight/input, and non-finite computed score are rejected without changing the result | pass |

## Commands

Run every local gate through the single fail-fast entrypoint:

```bash
learning/studies/20260915-affine-decision-tree-point-location/demo/verify.sh
```

To refresh the tracked aggregate evidence only after a successful run:

```bash
mkdir -p .tmp/affine-decision-tree
log=.tmp/affine-decision-tree/verification.log
learning/studies/20260915-affine-decision-tree-point-location/demo/verify.sh \
  >"$log" 2>&1 &&
cp "$log" \
  learning/studies/20260915-affine-decision-tree-point-location/evidence/raw/verification.txt
```

The entrypoint removes its dedicated build directory before compiling. The
Python process receives only the Zig CLI path built in that invocation, so an
older `.tmp/affine-decision-tree/zig-point-location` cannot satisfy the command.
Set `ZIG`, `FILCC`, or `FILRUN` explicitly to override tool discovery without
changing the recorded command stream.

## Negative Evidence

- The proof-first CMake configure failed before implementation because
  `src/odt.c` and `src/main.c` did not exist.
- Passing the Zig driver directly as `CMAKE_C_COMPILER` failed because CMake
  called `zig -arch arm64`; the required interface is `zig cc`. Final fixed
  LLVM evidence uses direct, fully recorded `zig cc` commands.
- Two same-day WeChat HTML fetches had different SHA-256 digests while the
  browser-extracted article content agreed. Whole-page bytes are therefore not
  treated as an immutable source revision.

## Evidence Boundary

The checks establish the documented behavior of the local seven-node tree,
validator, CLI, and Python oracle. They do not establish:

- the reported DuckDB throughput or speedup;
- equivalence to apart's arbitrary-dimensional vectorized evaluator;
- correctness or quality of the upstream 1,000-cell builder;
- bit-identical boundary behavior across floating-point implementations;
- production behavior under mutable trees, concurrent publication, or hostile
  allocation sizes.
