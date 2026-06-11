# Source

| Field | Value |
| --- | --- |
| URL | `https://blog.codingnow.com/2026/06/powersort.html` |
| Title | 对基本有序的序列排序算法 |
| Author | 云风 |
| Access Date | `2026-06-11` |
| Archive | `learning/sources/20260611-powersort.md` |
| Topic | PowerSort, Timsort, stable adaptive merge sort |

## 1. References

| Reference | Usage |
| --- | --- |
| Blog article | Main narrative and learning target |
| CPython `Objects/listsort.txt` | Background for natural runs and Timsort behavior |
| CPython `Objects/listobject.c` | Reference point for production implementation boundaries |

## 2. Scope Note

The demo is a learning reproduction, not a copy of CPython list sorting. It keeps the PowerSort merge-policy idea visible while avoiding CPython object model, reference counting, key-function handling, galloping mode, minrun tuning, and memory-management details.

## 3. Extracted Claims

| Claim | Demo Check |
| --- | --- |
| Existing ordered fragments should be detected as runs | Print detected runs before merges |
| Adjacent run pairs can be mapped to a virtual merge tree by `power` | Print computed powers |
| Power-guided stack merges approximate balanced merge sort | Print merge trace and final sorted output |
| Stability must be preserved | Compare against `std::stable_sort` and stable ids |
