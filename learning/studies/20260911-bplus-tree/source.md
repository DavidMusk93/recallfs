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

The article was loaded at its canonical URL in a real browser. Its semantic
`article` headings, paragraphs, lists, and code blocks were extracted from the
rendered DOM. A separate `curl -L --compressed` request captured the response
size, headers, and SHA-256.

## Claims Used

| Source claim | Study use |
| --- | --- |
| Internal B+ tree pages omit values | Fixed-width internal pages store only separator keys and child page IDs |
| Values reside in leaves | Leaf pages store configured fixed-width key/value byte records |
| Leaves form an ordered list | Range scans follow checked `next` links |
| Nodes should match storage units | Every node occupies one configurable fixed-size page |
| Buffer pools cache pages | The tree depends on a storage interface and contains no eviction policy |
| Key width affects fanout | The study derives exact capacities from page size and encoded entry width |

## Limitations

The source does not specify a recoverable storage format. WAL ordering,
checksums, freelist ownership, corruption handling, and exact API semantics in
this study are original engineering decisions and are verified separately.
