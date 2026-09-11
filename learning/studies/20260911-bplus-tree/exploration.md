---
doc_id: recallfs-exploration-bplus-tree-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-study-bplus-tree-v1
  - recallfs-source-bplus-tree-study-v1
supersedes: []
verified_by:
  - BPT-RA-1
  - BPT-RA-2
  - BPT-RA-3
  - BPT-RA-4
  - BPT-RA-5
  - BPT-RA-6
---

# Exploration Log

## 1. Source Extraction

The PlanetScale article was rendered in a browser and its semantic article
body was extracted from the DOM. A separate HTTP retrieval produced a
134,525-byte response with SHA-256
`572aec598464c7e80d932705b98009dd43e55ad4ecb54c8b038eccaa158a2adc`.

The reusable mechanism is not the article's MySQL-specific primary-key advice.
It is the fixed-page B+ tree shape: high-fanout internal pages, values in
leaves, and an ordered leaf chain.

## 2. Boundary Decision

The first design question was where the buffer pool belongs. Exposing frame
pointers, pin counts, or dirty bits would couple the tree to one cache. The
chosen interface instead copies pages through `read_page` and atomically
publishes a page-image write set through `commit_pages`.

This makes three implementations possible without changing tree code:

```text
tree core -> volatile memory provider
tree core -> durable file/WAL provider
tree core -> caller-owned buffer pool adapter
```

The provider, not the tree, owns durability and cache policy.

## 3. Proof-first Baseline

Public headers, CMake targets, and black-box tests were written before source
implementation. The first configure failed as intended:

```text
Cannot find source file: src/bptree.c
Cannot find source file: src/bptree_storage_memory.c
Cannot find source file: src/bptree_storage_file.c
```

All test translation units independently passed strict C11 syntax checking,
which separated the expected missing-implementation failure from mistakes in
the tests.

## 4. Rejected Implementation

An initial core implementation collected every key/value pair and rebuilt
every page for each mutation. It passed the model and structural tests, but
was rejected because:

- put/delete became `O(N)`;
- every mutation rewrote all allocated pages;
- WAL size scaled with the entire tree;
- the behavior contradicted the split/redistribution/merge design contract.

The accepted implementation retains the search path and builds a transaction
write set containing only copied pages that are actually changed. A spy
provider now asserts that an existing-value update submits exactly metadata
plus one leaf. A same-value upsert submits no write set.

## 5. Balancing And Page Reuse

The incremental implementation uses these rules:

- leaf overflow splits `capacity + 1` entries into two legal siblings;
- internal overflow promotes one separator and recursively inserts upward;
- delete borrows from a sibling before merging;
- internal merge carries the parent separator into the merged page;
- a zero-key internal root collapses to its only child;
- merged pages are rewritten as checked freelist pages;
- allocation pops the freelist before extending `next_page_id`.

The validator uses an independent recursive traversal plus a freelist walk. It
rejects duplicate ownership, cycles, out-of-range references, bad occupancy,
wrong levels, leaf-link disagreement, and unowned pages.

## 6. Durability Protocol

The file backend uses one full-page redo transaction at a time:

1. validate a unique, contiguous-extension write set;
2. write and durably sync a fixed unpublished `.wal.tmp`;
3. atomically rename it to `.wal` and sync the parent directory;
4. install all data pages and durably sync the database;
5. unlink the active WAL and sync the directory.

The handle is marked recovery-required as soon as WAL creation succeeds. Any
later error remains commit-unknown. Open replays a valid WAL idempotently.

An early gap allowed creation of a new database beside a stale WAL from an old
database identity. The final create path rejects any pre-existing sidecar and
preserves it for diagnosis.

## 7. Regression Expansion

The final deterministic suites cover:

- 30,000 mixed operations against an independent sorted model;
- ascending and descending 4,096-key trees;
- leaf/internal split, redistribution, merge, root collapse, and free reuse;
- 512, 4,096, and 65,536 byte pages;
- repeated clean file reopen;
- process-exclusive lock and `0600` file mode;
- commit crashes before WAL content, after WAL sync, after one data page, and
  after data sync;
- a second crash during recovery after one replayed page and after data sync;
- every nonempty truncated prefix of a valid WAL plus checksum corruption;
- page checksum, out-of-range child, leaf-cycle, and malformed-WAL rejection;
- provider rollback on definite I/O failure and handle poisoning on uncertain
  commit.

## 8. Quality Findings

Clang static analysis initially reported two possible uninitialized split
entries. The split loops initialize every slot, but using `calloc` made the
safety property explicit and removed the analyzer ambiguity.

The simplification pass removed duplicated lower-bound and internal-entry
operations, a dead node field, a dead crash-test CLI path, and duplicate
page-size predicates. It also made same-value upsert a true no-op. Larger
rebalance refactors were intentionally deferred because they increased change
risk without changing the tested contract.

## 9. Remaining Unknowns

- subprocess exit is not a physical power-cut or controller-cache test;
- no concurrent reader/writer protocol is implemented;
- network filesystem ordering is unsupported;
- allocation failure is handled, but exhaustive fail-every-allocation
  instrumentation is not part of v1;
- no workload benchmark supports a performance claim.
