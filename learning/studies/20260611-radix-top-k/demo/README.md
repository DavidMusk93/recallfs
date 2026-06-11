# Radix Top-K Demo

This is a standalone CMake project that reproduces the Radix Top-K idea with a C++20 prototype.

## 1. Layout

```text
demo/
  CMakeLists.txt
  .clang-format
  README.md
  src/radix_top_k.cpp
  build/              # generated locally; do not commit
```

## 2. Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build build
```

## 3. Run

```bash
./build/radix_top_k
```

## 4. Expected Result

The demo prints:

- radix top-k smallest,
- full-sort baseline smallest,
- radix top-k largest,
- full-sort baseline largest,
- `validation ok` when both radix results match the full-sort baseline.

## 5. macOS Notes

- Prefer an out-of-source CMake build in `build/`.
- If a custom `c++` resolves to a non-system toolchain and fails with `dyld` or `@rpath` errors, retry with `/usr/bin/clang++`.
- This demo adds `/usr/lib` as an Apple runtime search path because the local toolchain emitted `@rpath/libc++.1.dylib` without a usable default `LC_RPATH`.
- Do not commit `build/` or generated binaries.

## 6. Notes

- The demo implements an implicit radix tree over fixed-width unsigned integer keys.
- It extracts DuckDB's byte-comparable key idea but does not depend on DuckDB headers.
- It is a learning prototype, not a production replacement for DuckDB's heap-based `PhysicalTopN`.
- Function and variable names follow `rules.md`: little-camel-case outside class and namespace names.
