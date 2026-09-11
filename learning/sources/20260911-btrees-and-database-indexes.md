---
doc_id: recallfs-source-planetscale-btrees-database-indexes-20240909
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree
depends_on: []
supersedes: []
verified_by:
  - source URL retrieval
  - archived response SHA-256
---

# B-trees and database indexes

## Source

| Field | Value |
| --- | --- |
| Publisher | PlanetScale |
| Author | Ben Dicken |
| Published | 2024-09-09 |
| URL | <https://planetscale.com/blog/btrees-and-database-indexes> |
| Accessed | 2026-09-11 |
| Retrieved bytes | 134,525 |
| Response SHA-256 | `572aec598464c7e80d932705b98009dd43e55ad4ecb54c8b038eccaa158a2adc` |

The raw response is not tracked because it includes site chrome and generated
application data. The article body was inspected in a real browser and the
response identity above was captured independently with `curl`.

## Technical Claims

1. A B-tree keeps all leaves at one depth and uses sorted keys to choose one
   child per level during a point lookup.
2. Fixed-size nodes map naturally to storage blocks. Larger fanout reduces tree
   height and therefore the number of pages visited.
3. A B+ tree stores values only in leaves. Internal pages contain separators
   and child references, which increases internal fanout.
4. Linked leaves permit ordered range traversal without returning to internal
   pages for every result.
5. InnoDB normally uses 16 KiB pages and caches them in a buffer pool. The
   index and the page cache solve different problems.
6. Random primary keys distribute writes across leaves, while monotonic keys
   tend to revisit the rightmost path.
7. Wider keys reduce fanout. This can increase height and page reads.

## Scope Correction

The article is an educational overview, not an on-disk format or crash-safety
specification. It does not define:

- atomic page installation;
- recovery after torn writes;
- checksums and corruption handling;
- page reuse after deletion;
- concurrency control;
- the API boundary between a tree and a buffer pool.

The companion study supplies those contracts independently and treats the
article only as motivation for the page-oriented B+ tree shape.
