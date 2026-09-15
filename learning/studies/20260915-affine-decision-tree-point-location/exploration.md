---
doc_id: recallfs-study-affine-decision-tree-point-location-exploration-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location
depends_on:
  - recallfs-source-affine-decision-tree-point-location-v1
  - learning/studies/20260915-affine-decision-tree-point-location/demo/README.md
supersedes: []
verified_by:
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/README.md
---

# Exploration Log

## 1. Source Resolution

The supplied WeChat page was loaded in a real browser. Its DOM identified the
article title, author, publication date, benchmark table, upstream repository,
and 2D example links. A direct HTML retrieval produced SHA-256
`28b4ed9487fd4b8ef4fa8db190e1933b1f22ccf085a11343d4f9eb1895afd1ee`.

The upstream `apart` repository was inspected at commit
`470d3a3e76ab8cb28822dc4c87f24768e4a977ad`, the revision named by the
article. The exact README, 2D example, builder, and MIT license were read from
that revision or the matching main head.

## 2. What The Article Left Implicit

The article correctly describes the node expression and headline benchmark,
but four mechanisms needed expansion:

1. Pairwise squared-distance comparison becomes affine because the $x^2$ and
   $y^2$ terms cancel.
2. The evaluator and builder are separate systems; query-time code does not
   process polygons.
3. The builder's objective is a tradeoff between balanced children and copied
   polygon fragments, not merely “pick a middle line”.
4. A logical region may occur at multiple leaves because clipping duplicates
   its label across control-flow paths.

## 3. Artifact Choice

A DuckDB extension reproduction would hide the mechanism behind binder,
vector, and SQL integration code. A complete 1,000-cell builder would make the
geometry machinery dominate the lesson. The selected artifact is therefore:

- a reusable C evaluator with explicit ownership and error semantics;
- a hand-derived three-site tree that contains genuine oblique boundaries;
- a Python brute-force oracle that shares only the domain constants.

The example is intentionally not a one-off expression chain: the evaluator
accepts any validated immutable tree using the published reference encoding.

## 4. Model Derivation

Sites A `(2,2)`, B `(8,3)`, and C `(4,7)` produce:

| Pair | Lower-ID-wins condition |
| --- | --- |
| A/B | `-6*x - y >= -32.5` |
| A/C | `-2*x - 5*y >= -28.5` |
| B/C | `4*x - 4*y >= 4` |

A/B is evaluated first. The surviving site is then compared with C. This
proves nearest-site classification with two interior comparisons and makes C
reachable through two leaves.

Four preceding comparisons define the closed rectangle. Outside points
short-circuit after one to four comparisons; every inside point takes exactly
six.

## 5. Proof-first Sequence

Before implementation:

1. public C types and status codes were declared;
2. `odt_test.c` specified 12 location anchors, tie behavior, non-finite input,
   zero references, unreachable nodes, and cycles;
3. Python specified a 20,000-random-point differential test;
4. CMake was run before source implementation.

The expected red result was observed:

```text
Cannot find source file: src/odt.c
Cannot find source file: src/main.c
No SOURCES given to target: odt
```

After implementation, the native C test passed before the Python environment
or documentation evidence was completed.

## 6. Toolchain Correction

Passing the Zig executable directly as `CMAKE_C_COMPILER` failed because CMake
invoked `zig -arch arm64`; Zig requires the `zig cc` subcommand. The final
fixed-version LLVM checks therefore invoke:

```text
zig 0.16.0 cc <complete flags> <sources>
```

Both Release and ASan/UBSan binaries passed. CMake remains the portable project
entry point and was checked locally, but it is not used as evidence for the
fixed Zig compiler gate.

## 7. Deliberate Differences From apart

| Surface | apart | Local C artifact |
| --- | --- | --- |
| Dimension | arbitrary fixed feature count | exactly 2 |
| Leaf type | any common DuckDB type | `int32_t` |
| Null | null feature returns null | no nullable C input |
| NaN | IEEE false branch | rejected |
| Validation | DuckDB bind time | explicit `odt_tree_validate` |
| Execution | DuckDB vectors | scalar C call |
| Builder | greedy clipped-polygon BSP | hand-derived three-site tree |
| Fixed depth | optional rewrite | not implemented |

These differences keep the mechanism visible and are not presented as
extension compatibility.

## 8. Failed Or Rejected Alternatives

- An axis-aligned four-quadrant example was rejected because it does not teach
  oblique cuts.
- Reimplementing the upstream NumPy builder in C was rejected because polygon
  clipping would obscure evaluator semantics and would require a larger
  independent geometry oracle.
- Reusing the tree traversal in Python was rejected because it would only show
  that two implementations share the same mistake. The oracle instead computes
  nearest sites directly.
- Treating the `zig` driver itself as a CMake C compiler was rejected after the
  observed command-shape failure; direct `zig cc` is explicit and reproducible.

## 9. Remaining Boundary

This study validates the static evaluator and one exact Voronoi construction.
It does not evaluate builder quality, generated tree size, DuckDB vectorization,
branch prediction, cache behavior, or the article's performance numbers.
