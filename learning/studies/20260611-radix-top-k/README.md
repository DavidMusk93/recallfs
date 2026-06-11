# Radix Top-K Study

## 1. Conclusion

- Summary: Radix Top-K can find the top-k set by traversing key prefixes instead of sorting the full input.
- What was verified: A standalone C++ demo matches a full-sort baseline for both smallest and largest top-k on unsigned integers.
- What remains uncertain: Production performance needs vectorization, memory-budget control, and extended type encoding for signed integers, floats, strings, and null ordering.

## 2. Source

| Field | Value |
| --- | --- |
| URL | `https://veitner.bearblog.dev/radix-top-k/` |
| Access Date | `2026-06-11` |
| Archive | `learning/sources/20260611-radix-top-k.md` |
| Study Directory | `learning/studies/20260611-radix-top-k/` |
| DuckDB Reference | `https://github.com/duckdb/duckdb` |

## 3. Core Idea

- Claim: Top-K does not require full sorting if the key domain can be explored prefix by prefix.
- Mechanism: Count candidates in radix buckets, use prefix counts to identify guaranteed buckets and a single boundary bucket, then recurse only into the boundary bucket.
- Constraints: This demo handles `uint32_t` values; richer SQL ordering needs DuckDB-style sort-key encoding.
- Expected result: The selected top-k values match a full-sort baseline.

## 4. Architecture

```text
+-----------------------------+
| DuckDB radix key idea       |
| byte-comparable key encoding|
+--------------+--------------+
               |
               v
+-----------------------------+
| Implicit radix tree         |
| fixed bit chunks as levels  |
+--------------+--------------+
               |
               v
+-----------------------------+
| Radix Top-K pruning         |
| count -> prefix -> boundary |
+--------------+--------------+
               |
               v
+-----------------------------+
| C++ demo validation         |
| compare with full sort      |
+-----------------------------+
```

## 5. Demo

| Item | Value |
| --- | --- |
| Path | `learning/studies/20260611-radix-top-k/demo/` |
| Language | C++20 |
| Project Layout | `CMakeLists.txt`, `.clang-format`, `README.md`, `src/` |
| Build Command | `cmake -S learning/studies/20260611-radix-top-k/demo -B learning/studies/20260611-radix-top-k/demo/build -DCMAKE_CXX_COMPILER=/usr/bin/clang++ && cmake --build learning/studies/20260611-radix-top-k/demo/build` |
| Run Command | `learning/studies/20260611-radix-top-k/demo/build/radix_top_k` |

## 6. Exploration Log

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Archived source article | Radix Top-K workflow captured | `learning/sources/20260611-radix-top-k.md` |
| 2 | Inspected DuckDB source | Found radix encoding, ART key usage, heap TopN | `source.md` |
| 3 | Built demo | Implemented implicit radix-tree Top-K as a CMake project | `demo/src/radix_top_k.cpp` |
| 4 | Fixed macOS build pitfall | Avoided `dyld` / `@rpath` issue with system `clang++` and Apple runtime search path in CMake | `exploration.md` |
| 5 | Validated result | Demo matches full-sort baseline | `evidence/run.log` |

## 7. Lessons

- Reusable insight: order-preserving key encoding separates comparison semantics from selection mechanics.
- Failure pattern: calling this a DuckDB radix top-k implementation would be misleading; DuckDB's SQL TopN is heap-based.
- macOS pitfall: a direct `c++` command may compile but still produce a binary that fails at runtime if the selected toolchain has unresolved runtime library paths.
- Source boundary: the demo intentionally extracts DuckDB radix-key semantics instead of linking DuckDB, because the inspected DuckDB code does not contain a reusable standalone radix Top-K tree module.
- Follow-up: extend the demo to signed integers and floats with DuckDB-style sign-bit and NaN handling.
