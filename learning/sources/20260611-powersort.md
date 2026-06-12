# PowerSort Article Archive

| Field | Value |
| --- | --- |
| URL | `https://blog.codingnow.com/2026/06/powersort.html` |
| Title | 对基本有序的序列排序算法 |
| Author | 云风 |
| Published | 2026-06-11 17:46 |
| Access Date | 2026-06-11 |

## 1. Archived Summary

The article explains why stable sorting matters, why merge sort is naturally stable but needs extra space, and why real-world data often contains ordered fragments. Timsort improves merge sort by scanning natural ordered runs and merging adjacent runs through stack rules. The article then explains historical Timsort stack-invariant issues and motivates PowerSort as a clearer merge policy introduced by Python 3.11.

## 2. Core Points

| Topic | Notes |
| --- | --- |
| Stable sort | Equal keys should preserve original relative order. |
| Natural run | Existing ascending or descending ordered fragment in the input. |
| Timsort | Scans runs and merges adjacent runs through stack invariants. |
| Timsort issue | Historical stack-size proof was subtle; crafted run lengths could break assumptions. |
| PowerSort | Uses a virtual complete binary merge tree and assigns each adjacent run pair a `power`. |
| Stack bound | Power is at most around `log2(n)`, so stack depth is easier to reason about. |
| Other optimizations | Minrun, galloping, reduced temp space, descending-run handling are mostly shared with Timsort. |

## 3. PowerSort Mechanism

PowerSort imagines the full input range as a virtual complete binary tree. For two adjacent runs, it compares their midpoints against this virtual tree and computes a level-like value called `power`. A run stack stores these powers. When a new adjacent pair has a power that would violate the monotonic stack order, the algorithm merges previous top runs first. This approximates balanced merge sort while preserving natural runs.

## 4. Reproduction Scope

The study demo focuses on the merge-policy core:

- sort a natural `std::vector<int>` sequence,
- detect natural ascending and strictly descending runs,
- reverse strictly descending runs,
- compute pair power with the CPython-style midpoint loop,
- merge adjacent runs using a PowerSort-style stack rule,
- compare against sequential run merging to show whether the idea helps,
- validate sorted output against `std::stable_sort`.

The demo does not reproduce production Timsort/Python optimizations such as minrun tuning, galloping mode, binary insertion sort for short runs, or reduced merge memory.
