---
doc_id: recallfs-source-archive-affine-decision-tree-point-location-v1
kind: reference
status: archived
authority: evidence
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location
depends_on: []
supersedes: []
verified_by:
  - browser DOM extraction on 2026-09-15
---

# Source Archive: DuckDB apart 2D Point Location

This is a structured archive of the supplied WeChat article, not a verbatim
copy.

| Field | Value |
| --- | --- |
| URL | <https://mp.weixin.qq.com/s/VcHSNSpV0tI0dSku72Lhpg> |
| Title | DuckDB 新扩展 apart：网格点定位快 59 倍 |
| Author | alitrack |
| Published | 2026-09-15 |
| Accessed | 2026-09-15 |
| First retrieved HTML SHA-256 | `28b4ed9487fd4b8ef4fa8db190e1933b1f22ccf085a11343d4f9eb1895afd1ee` |
| Second retrieved HTML SHA-256 | `ea675e610040907839bc4cb0c2d8193c08151fb5f1f5785ae83cdcd628ecf7f6` |

The wrapper HTML is dynamic: two successful retrievals on the same day had
different byte digests while browser-extracted title, author, article body, and
links agreed. The digests prove what was fetched, not a stable source revision.
Stable implementation claims below are therefore pinned to the upstream Git
commit.

## Extracted Claims

1. `apart` evaluates an affine decision tree whose internal node computes
   `weights[i] dot x >= thresholds[i]`.
2. Positive child references select another internal node; negative references
   select a leaf value.
3. Its 2D example compiles a fixed Voronoi polygon grid into a tree. Candidate
   cuts are perpendicular bisectors between generating sites.
4. A cut polygon is clipped into both branches and retains the same cell ID.
   This can duplicate one logical region across several leaves.
5. The reported 1,000-cell tree has 4,090 decision nodes and at most 18
   comparisons per query, including four rectangle checks.
6. The article reports 35.7 ns/row for `apart`, 6,197 ns/row for nested SQL
   `CASE`, and 2,123 ns/row for a polygon join over one million points.
7. `fixed_depth` pads shallow leaves to the maximum depth. It can help only
   when the original tree is already close to fixed-depth and rejects expansion
   beyond 1,000,000 internal nodes.
8. The tree is constant within one query; feature values vary by row. Native
   IEEE comparison sends NaN to the `below` branch.
9. The article's tested extension revision is `470d3a3`.

## Interpretation Boundary

The timing numbers are upstream observations, not reproduced results in this
study. They compare complete DuckDB query implementations, so they do not
isolate decision-tree traversal from SQL expression evaluation, geometry
filtering, data layout, or extension overhead.
