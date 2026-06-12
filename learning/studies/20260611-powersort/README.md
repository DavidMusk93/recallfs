# PowerSort Study

## 1. Conclusion

- Summary: PowerSort replaces Timsort's subtle run-stack invariants with a merge policy derived from a virtual complete binary merge tree.
- What was verified: A C++20 demo sorts plain integer sequences, explains adjacent-run power, compares PowerSort against sequential run merging, and matches `std::stable_sort`.
- What remains uncertain: Production performance depends on minrun, galloping, memory trimming, comparator cost, and low-level runtime integration.

## 2. Source

| Field | Value |
| --- | --- |
| URL | `https://blog.codingnow.com/2026/06/powersort.html` |
| Access Date | `2026-06-11` |
| Archive | `learning/sources/20260611-powersort.md` |
| Study Directory | `learning/studies/20260611-powersort/` |

## 3. Core Idea

- Claim: For partially ordered data, sorting can exploit natural runs instead of treating the input as random.
- Mechanism: Detect runs, compute a `power` for each adjacent run pair from virtual merge-tree midpoints, and merge stack runs when powers indicate that previous local runs should be merged first.
- Constraints: The demo focuses on merge ordering and stability, not full CPython/Python-list production behavior.
- Expected result: PowerSort-style output should match a stable-sort baseline.

## 4. Architecture

```text
+-----------------------------+
| input sequence<int>         |
+--------------+--------------+
               |
               v
+-----------------------------+
| natural run detection       |
| ascending or descending     |
+--------------+--------------+
               |
               v
+-----------------------------+
| virtual tree power          |
| adjacent run midpoints      |
+--------------+--------------+
               |
               v
+-----------------------------+
| stack merge policy          |
| merge when power decreases  |
+--------------+--------------+
               |
               v
+-----------------------------+
| stable-sort validation      |
+-----------------------------+
```

## 5. Demo

| Item | Value |
| --- | --- |
| Path | `learning/studies/20260611-powersort/demo/` |
| Language | C++20 |
| Project Layout | `CMakeLists.txt`, `.clang-format`, `README.md`, `src/` |
| Build Command | `cmake -S learning/studies/20260611-powersort/demo -B learning/studies/20260611-powersort/demo/build -DCMAKE_CXX_COMPILER=/usr/bin/clang++ && cmake --build learning/studies/20260611-powersort/demo/build` |
| Run Command | `learning/studies/20260611-powersort/demo/build/powersort_demo` |

## 6. Comparison

| Case | Practical Meaning | PowerSort Result | Baseline Result | Lesson |
| --- | --- | --- | --- | --- |
| nearly sorted list with a late small batch | A mostly sorted list receives a sorted late batch | Same merge work as baseline | Same merge work | PowerSort is not automatically better |
| time-window batches from multiple producers | Several sorted producer windows are appended out of global order | Same merge work as baseline | Same merge work | Simple merging is already adequate |
| alternating service pages | Multiple sorted pages arrive in alternating key ranges | Lower merge work | Higher merge work | Power-guided merges avoid repeatedly growing a large prefix |
| reverse imported chunk inside sorted data | A reverse chunk is imported into mostly sorted data | Lower merge work | Higher merge work | Run detection plus power-guided merging helps |

## 7. Exploration Log

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Archived source article | Core article claims preserved | `learning/sources/20260611-powersort.md` |
| 2 | Read article and CPython notes | Identified PowerSort as run-stack merge policy | `source.md` |
| 3 | Built C++20 demo | Implemented sequence input, run detection, power calculation, stack merges, and comparison metrics | `demo/src/powersort_demo.cpp` |
| 4 | Validated output | Demo matches `std::stable_sort` and compares against sequential run merge | `evidence/run.log` |

## 8. Lessons

- Reusable insight: PowerSort makes the run-stack bound easier to reason about by connecting merge decisions to a virtual complete binary tree.
- Failure pattern: A demo that only sorts final output is insufficient; it must explain `power`, use natural sequence input, print traces, and compare against a simpler baseline.
- Practical lesson: PowerSort is useful when run layout would make naive sequential merging repeatedly rewrite large prefixes; it is not universally better on every partially sorted input.
- Follow-up: Add minrun and galloping experiments, then benchmark against `std::stable_sort` on partially ordered datasets.
