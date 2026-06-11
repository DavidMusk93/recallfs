# Exploration Log

## 1. Goal

Build a minimal C++ demo for Radix Top-K using DuckDB-inspired radix key ideas.

## 2. Steps

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Archived the source article | Captured the Radix Top-K algorithm in Markdown | `learning/sources/20260611-radix-top-k.md` |
| 2 | Cloned DuckDB into `.tmp/duckdb` | Local source copy available for inspection | `.tmp/duckdb` |
| 3 | Searched DuckDB for radix and top-n code | Found radix key utilities, ART key usage, radix partitioning, and heap TopN | `source.md` |
| 4 | Read `radix.hpp` | Extracted byte-comparable key encoding idea | `Radix::EncodeData` |
| 5 | Read `art_key.hpp` | Confirmed DuckDB ART keys are built from radix-encoded bytes | `ARTKey::CreateData` |
| 6 | Read `physical_top_n.cpp` | Confirmed DuckDB production TopN uses sort keys plus bounded heap | `TopNHeap` |
| 7 | Implemented standalone demo | Built implicit radix-tree Top-K over bit chunks | `demo/src/radix_top_k.cpp` |
| 8 | Tried direct `c++` build on macOS | Binary failed at runtime with `dyld` / `@rpath` library resolution error | `evidence/run.log` before replacement |
| 9 | Converted demo to CMake project | Standard project layout with explicit C++17 settings | `demo/CMakeLists.txt` |
| 10 | Added Apple runtime search path | `/usr/lib` rpath fixed `@rpath/libc++.1.dylib` runtime lookup | `demo/CMakeLists.txt` |
| 11 | Rebuilt and ran demo | Radix Top-K matched full-sort baseline | `evidence/run.log` |

## 3. Key Observations

- DuckDB's radix utilities are primarily about order-preserving byte encodings.
- DuckDB's ART index stores keys as radix-encoded byte sequences.
- DuckDB's SQL TopN operator is heap-based, not the same as the article's radix bucket pruning.
- The article's algorithm can be modeled as traversal over an implicit radix tree: each bit chunk selects one level of children.
- macOS toolchains can fail after compilation if `c++` points to a custom compiler whose runtime libraries are not discoverable by `dyld`.

## 4. Design Decision

The demo does not copy DuckDB internals. It extracts the useful semantics:

| Source | Extracted Idea | Demo Usage |
| --- | --- | --- |
| DuckDB `Radix::EncodeData` | Convert comparable values into lexicographically sortable keys | Encode `uint32_t` as big-endian bytes |
| DuckDB ART key | Treat encoded bytes as radix-tree path | Traverse fixed-size bit chunks |
| Radix Top-K article | Prune buckets by prefix counts | Keep guaranteed buckets and recurse only into boundary bucket |
| DuckDB TopN | Validate Top-K result semantics | Compare against full-sort baseline |

## 5. macOS Build Pitfall

The first direct compiler command produced a binary, but the binary aborted at runtime:

```text
dyld: Library not loaded: @rpath/libc++.1.dylib
Reason: no LC_RPATH's found
```

The fix was to make the demo a standard CMake project, build it with the system compiler, and add `/usr/lib` as an Apple runtime search path:

```bash
cmake -S learning/studies/20260611-radix-top-k/demo \
  -B learning/studies/20260611-radix-top-k/demo/build \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build learning/studies/20260611-radix-top-k/demo/build
```

## 6. Open Questions

- Extending this to signed integers and floating-point values should reuse DuckDB-style sign flipping and float encoding.
- Production Top-K needs vectorized processing, memory budget control, and tie handling policy.
- A real ART-based implementation would materialize nodes; this demo uses an implicit tree over fixed-width keys.
