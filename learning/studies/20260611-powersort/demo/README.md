# PowerSort Demo

This is a C++20 learning demo for the PowerSort merge-policy core. The public test input is a plain integer sequence because sorting users usually start from a sequence, not from a synthetic record wrapper.

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

## 4. Power Meaning

`power` describes where the boundary between two adjacent runs belongs in a virtual complete merge tree. In this demo, smaller power means the boundary is closer to the root split, while larger power means the boundary is deeper and more local. PowerSort uses that value to decide whether previous stack runs should be merged before pushing the new run.

## 5. Expected Result

The demo prints practical test cases, detected runs, computed powers, merge operations, comparison metrics, and `validation ok`.

## 6. Comparison Baseline

The demo compares PowerSort against a simple sequential run merge. This keeps the learning result practical: if PowerSort does not reduce merge work or stack pressure for a dataset, the output says so.

## 7. Scope

- Implements natural run detection and stable adjacent merge.
- Uses a CPython-style midpoint loop to compute pair power.
- Sorts `std::vector<int>` inputs.
- Uses practical datasets such as late batches, producer windows, alternating pages, and reverse imported chunks.
- Does not implement minrun, galloping, binary insertion sort, or Python object handling.
