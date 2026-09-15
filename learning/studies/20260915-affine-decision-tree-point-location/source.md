---
doc_id: recallfs-source-affine-decision-tree-point-location-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location
depends_on:
  - recallfs-source-archive-affine-decision-tree-point-location-v1
supersedes: []
verified_by:
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/raw/source-digests.txt
---

# Sources

Access date: 2026-09-15.

## Primary Article

| Field | Value |
| --- | --- |
| Title | DuckDB 新扩展 apart：网格点定位快 59 倍 |
| Author | alitrack |
| URL | <https://mp.weixin.qq.com/s/VcHSNSpV0tI0dSku72Lhpg> |
| Local archive | [`learning/sources/20260915-affine-decision-tree-point-location.md`](../../sources/20260915-affine-decision-tree-point-location.md) |
| Retrieved HTML SHA-256 | `28b4ed...afd1ee`, then `ea675e...f7f6`; wrapper bytes are dynamic |

The page was read through a real browser and its article DOM was extracted.
The local archive preserves metadata, claims, and evidence boundaries rather
than reproducing the copyrighted article verbatim. Two same-day HTTP fetches
returned the same article content with different wrapper bytes, so the HTML
digest is not treated as a stable source revision.

## Upstream Implementation

| Field | Value |
| --- | --- |
| Repository | <https://github.com/jokasimr/apart> |
| Inspected revision | `470d3a3e76ab8cb28822dc4c87f24768e4a977ad` |
| Revision date | 2026-09-13 |
| License | MIT |
| Evaluator contract | [`README.md`](https://github.com/jokasimr/apart/blob/470d3a3e76ab8cb28822dc4c87f24768e4a977ad/README.md) |
| 2D explanation | [`examples/2 unstructured grid.md`](https://github.com/jokasimr/apart/blob/470d3a3e76ab8cb28822dc4c87f24768e4a977ad/examples/2%20unstructured%20grid.md) |
| Builder | [`point_location.py`](https://github.com/jokasimr/apart/blob/470d3a3e76ab8cb28822dc4c87f24768e4a977ad/examples/unstructured_grid/point_location.py) |

The source review confirms details that the article compresses:

- a candidate split is a bisector between two generating sites;
- split quality minimizes the largest child candidate count plus a penalty for
  fragments copied into both children;
- crossed convex polygons are clipped against both half-planes;
- a defining pair is forced into opposite branches to guarantee recursion
  progress despite floating-point clipping;
- the builder tries deterministic direction/penalty variants and selects
  minimum maximum depth, then minimum node count.

## Local Artifact Relationship

The local C code is an independent explanatory implementation of the published
tree data model and traversal rule. It does not copy the DuckDB extension or
the NumPy/Shapely-scale builder. The three-site tree is derived algebraically
in the study and checked against an independent brute-force Python oracle.
