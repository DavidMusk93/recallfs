---
doc_id: recallfs-source-bplus-tree-study-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree
depends_on:
  - recallfs-source-planetscale-btrees-database-indexes-20240909
supersedes: []
verified_by:
  - source URL retrieval
  - source digest comparison
---

# Source Record

## Primary Source

- Article: [B-trees and database indexes](https://planetscale.com/blog/btrees-and-database-indexes)
- Publisher: PlanetScale
- Author: Ben Dicken
- Publication date: 2024-09-09
- Access date: 2026-09-11
- Local archive:
  [`learning/sources/20260911-btrees-and-database-indexes.md`](../../sources/20260911-btrees-and-database-indexes.md)
- Retrieved HTML SHA-256:
  `572aec598464c7e80d932705b98009dd43e55ad4ecb54c8b038eccaa158a2adc`

## Extraction Method

The canonical article was loaded in a real browser. Headings, paragraphs,
lists, and code blocks were extracted from the rendered semantic `article`
element. A separate `curl -L --compressed` retrieval recorded the response
size, headers, and digest.

## Claims Used

| Source claim | Final `rbt` use |
| --- | --- |
| Internal B+ tree pages omit row values | internal slotted cells persist canonical separators and child page IDs |
| Values reside in leaves | leaf slotted cells persist canonical keys and descriptors for non-key columns, then reconstruct complete rows |
| Leaves form an ordered list | `[begin, end)` scans traverse checked sibling links |
| Nodes should match storage units | every node occupies one configurable 512..65,536-byte page |
| Buffer pools cache pages | the core depends on a page provider and owns no eviction policy |
| Key width affects fanout | schema maxima bound canonical key size and conservative occupancy |

## Engineering Extension

The article supplies the structural motivation, not the production contract.
The `rbt` 0.2.0 public model is one row model. A schema has one ordered
`columns[]` array and `column_count`; `RBT_COLUMN_KEY` selects key columns, and
their order is schema order after filtering. A record likewise has one
`values[]` array and `value_count`: put projects it as a complete schema-order
row, while get/delete/scan bounds project the same representation as a compact
KEY tuple. Owned and callback rows reconstruct the complete logical
schema-order row and expose it through `rbt_row_get`.

On disk, a leaf stores the canonical key plus descriptors only for non-key
columns. Decoding merges KEY fields with payload descriptors into complete
physical schema order. Overflow-page `AUX` records the physical schema column
ordinal, and interleaved KEY/payload layouts are verified through overflow,
reopen, update, and delete.

Persisted `RBTR` schema encoding, canonical composite-key encoding, nullable
and descending key semantics, slotted pages, non-key descriptors,
physical-column-ordinal overflow chains, freelist ownership, atomic page-set
commit, storage leases, checksums, WAL publication, corruption handling,
negative-errno APIs, and package integration are original engineering
decisions verified by `RBT-RA-1` through `RBT-RA-8`.

The library is a clean break from the historical study implementation.
`RBT_FORMAT_VERSION=1` is the sole current development format, but its schema
encoding magic and unified layout are now `RBTR`. Previous `RBTS` files and
predecessor files are rejected and have no compatibility reader, migration
layer, alias, or alternate format.

## Limitations

The source article does not prove the library's crash behavior, safety,
locality, or compatibility boundary. Those conclusions are limited to the
tracked source at commit `ad8e6ee95d766d424249df6894a00bcf354878ce` and the
raw evidence ledger. Neither the article nor this study establishes real
power-loss behavior, network filesystem guarantees, shared-tree concurrency,
online backup, or benchmark performance.
