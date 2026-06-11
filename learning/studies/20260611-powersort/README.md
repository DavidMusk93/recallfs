# PowerSort Study

## 1. Conclusion

- Summary: PowerSort replaces Timsort's subtle run-stack invariants with a merge policy derived from a virtual complete binary merge tree.
- What was verified: A C++20 demo detects natural runs, computes adjacent-run power, performs stable adjacent merges, and matches `std::stable_sort`.
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
- Mechanism: Detect runs, compute a `power` for each adjacent run pair from virtual merge-tree midpoints, and merge stack runs when powers violate the desired order.
- Constraints: The demo focuses on merge ordering and stability, not full CPython/Python-list production behavior.
- Expected result: PowerSort-style output should match a stable-sort baseline.

## 4. Architecture

```text
+-----------------------------+
| input records               |
| key + original id           |
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

## 6. Exploration Log

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Archived source article | Core article claims preserved | `learning/sources/20260611-powersort.md` |
| 2 | Read article and CPython notes | Identified PowerSort as run-stack merge policy | `source.md` |
| 3 | Built C++20 demo | Implemented run detection, power calculation, stack merges | `demo/src/powersort_demo.cpp` |
| 4 | Validated output | Demo matches `std::stable_sort` baseline | `evidence/run.log` |

## 7. Lessons

- Reusable insight: PowerSort makes the run-stack bound easier to reason about by connecting merge decisions to a virtual complete binary tree.
- Failure pattern: A demo that only sorts final output is insufficient; it must print runs, powers, and merge trace to expose the algorithm.
- Follow-up: Add minrun and galloping experiments, then benchmark against `std::stable_sort` on partially ordered datasets.
