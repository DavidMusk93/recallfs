# PowerSort Demo

This is a C++20 learning demo for the PowerSort merge-policy core.

## 1. Layout

```text
demo/
  CMakeLists.txt
  .clang-format
  README.md
  src/powersort_demo.cpp
  build/              # generated locally; do not commit
```

## 2. Build

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build build
```

## 3. Run

```bash
./build/powersort_demo
```

## 4. Expected Result

The demo prints detected runs, computed powers, merge operations, final sorted records, and `validation ok`.

## 5. Scope

- Implements natural run detection and stable adjacent merge.
- Uses a CPython-style midpoint loop to compute pair power.
- Does not implement minrun, galloping, binary insertion sort, or Python object handling.
