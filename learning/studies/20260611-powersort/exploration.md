# Exploration Log

## 1. Goal

Reproduce the PowerSort merge-policy core from the article with a small C++20 CMake demo.

## 2. Steps

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Fetched `https://blog.codingnow.com/2026/06/powersort.html` | Article content available for archive and summary | `learning/sources/20260611-powersort.md` |
| 2 | Read the article | Identified the main contrast: Timsort stack invariants vs PowerSort virtual-tree power | `source.md` |
| 3 | Checked CPython listsort background | Confirmed natural runs, stable merge sort context, and production features outside demo scope | `source.md` |
| 4 | Designed reproduction scope | Kept run detection, power calculation, stack merges, and stable validation | `demo/src/powersort_demo.cpp` |

## 3. Key Observations

- PowerSort does not replace all of Timsort; it mainly simplifies the run merge policy.
- Stability requires only adjacent runs to be merged and equal keys to preserve original order.
- The virtual-tree `power` value gives a clearer stack-depth argument than historical Timsort invariants.
- A learning demo should expose the trace rather than hide the algorithm behind final sorted output.

## 4. Reproduction Design

```text
+-------------------------+
| scan next natural run   |
+-----------+-------------+
            |
            v
+-------------------------+
| compute adjacent power  |
+-----------+-------------+
            |
            v
+-------------------------+---- violation ---->+-------------------------+
| push run to stack       |                     | merge previous top runs |
+-----------+-------------+                     +-------------------------+
            |
            v
+-------------------------+
| collapse remaining runs |
+-----------+-------------+
            |
            v
+-------------------------+
| compare stable baseline |
+-------------------------+
```

## 5. Scope Boundary

| Included | Excluded |
| --- | --- |
| Natural ascending run detection | Python object model |
| Strict descending run reversal | Equal-element descending-run micro-optimization |
| CPython-style midpoint power loop | Full CPython listsort implementation |
| Stable adjacent merge | Galloping mode |
| Traceable merge decisions | Minrun and binary insertion sort |

## 6. Open Questions

- How much does power-guided merging improve real workloads compared with a simple length-based merge policy?
- Which partially ordered datasets expose the largest difference?
- How should minrun and galloping be added without hiding the PowerSort core?
