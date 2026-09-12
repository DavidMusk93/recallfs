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
  - RBT-RA-1
  - RBT-RA-2
  - RBT-RA-3
  - RBT-RA-4
  - RBT-RA-5
  - RBT-RA-6
  - RBT-RA-7
  - RBT-RA-8
---

# Exploration Log

## 1. Source Extraction

The PlanetScale article was rendered in a browser and its semantic article
body was extracted from the DOM. A separate HTTP retrieval produced a
134,525-byte response with SHA-256
`572aec598464c7e80d932705b98009dd43e55ad4ecb54c8b038eccaa158a2adc`.

The reusable mechanism is the fixed-page B+ tree shape: high-fanout internal
pages, values in linked leaves, and bounded root-to-leaf traversal.

## 2. Local Transaction Choice

A full-tree rebuild after every mutation was rejected because it made writes
and WAL size scale with the whole tree. The accepted transaction model copies
only the search path, changed overflow chains, and required siblings, then
submits the exact page set once.

## 3. Typed Record Model

The final schema persists a schema ID plus one physical `columns[]` array and
`column_count`. Every column carries its ID, type, flags, and maximum size.
`RBT_COLUMN_KEY` marks key columns; their key-tuple order is schema order after
filtering. `RBT_COLUMN_DESCENDING` requires `RBT_COLUMN_KEY`. There are no
public key-column and value-column arrays. Supported types are `BOOL`, `I64`,
`U64`, `BYTES`, and validated `UTF8`. Column IDs are globally unique within
the schema.

The same unified representation applies to operation input. `struct
rbt_record` contains one `values[]` array and `value_count`. Put supplies a
complete physical-schema-order row. Get, delete, and non-null scan bounds
supply the compact KEY tuple in filtered schema order. This is an
operation-specific projection of one record type, not a split key/value
object. Both owned get results and callback-lifetime scan rows are complete
logical rows in physical schema order and use the single
`rbt_row_get(row, column_ordinal)` accessor.

The composite key codec converts typed values into one canonical byte sequence:

- null state participates in ordering;
- signed and unsigned integers become order-preserving byte sequences;
- variable bytes use escaping plus a terminator, so components cannot be
  confused by prefixes or embedded zero bytes;
- descending components complement their encoded bytes.

This removes runtime comparator identity from the persisted contract.
`rbt_open` reads the complete schema from schema pages and accepts no
caller-supplied schema. `rbt_get_schema` exposes the tree-owned copy.

The schema validator bounds the design at 64 total columns, 1 MiB per variable
field, and 4 MiB per declared row. It also rejects a key layout whose maximum
canonical representation leaves fewer than three cells in leaf or internal
pages at the chosen page size.

## 4. Slotted Pages And Overflow

All six logical page types share one 64-byte checked header: metadata, leaf,
internal, schema, overflow, and free. Leaf/internal pages have 8-byte slots
made from `u32 offset` plus `u32 length`; slots grow upward while variable cells
pack downward. `RBT_FORMAT_VERSION=1` is the sole format, and schema pages use
the `RBTR` unified-column layout.

Leaf cells persist the canonical key once plus descriptors only for non-key
columns. Decoding reconstructs the complete row by merging decoded KEY fields
and payload descriptors into physical schema order. Variable non-key columns
inline small payloads. A large payload gets an overflow descriptor and its own
chain tagged in page `AUX` with the physical schema column ordinal and logical
length. Separate payload columns never share a chain. Tests deliberately
interleave payload and KEY columns and cover overflow creation, reopen, update,
delete, and reuse; changing one large payload retains every page in other
unchanged payload chains.

The validator independently checks slot bounds and overlap, page checksums and
IDs, schema chain size/count, overflow cycles/column tags/lengths, tree
ownership, leaf links, freelist disjointness, and the page accounting identity.

## 5. Local Mutation And Reuse

Insertion and deletion retain a search path. The balancing rules are:

- split an overflowing leaf or internal page into conservative legal siblings;
- propagate a separator only through the path;
- borrow from a sibling before merging;
- carry the parent separator during internal merge;
- collapse a unary internal root;
- turn removed tree/overflow pages into checked free pages;
- allocate from the freelist before extending the high-water mark.

A storage spy established the write boundary, not just the algorithmic intent.
Existing-row replacement, non-splitting insertion, and non-rebalancing deletion
each submit one atomic commit containing page `0` metadata and one leaf. A
same-value put submits no commit. Leaf split, internal split, borrow, merge, and
root collapse each write fewer pages than the live tree and stay within a
height-derived path/sibling bound.

The overflow locality regression separately records page IDs. A scalar update
beside two large columns writes only metadata and leaf; changing one large
column excludes the other chain. A failed expanded update leaves the previous
row and page accounting intact. The interleaved-column regression also checks
that physical ordinals survive reopen, update, and delete without key/payload
confusion.

## 6. Provider And Lease Boundary

The core copies an `rbt_storage` callback table and borrows its context.
`read_page` copies into caller memory; `page_count` reports the logical
high-water mark; `commit_pages` publishes one exact, unique page set atomically.
No provider pointer survives a callback.

Mandatory `acquire`/`release` callbacks reserve one live tree per storage
context. A second `rbt_create` or `rbt_open` returns `-EBUSY`, and backend
close/destroy also fails while leased.

A definite provider failure may return a normal negative errno. An unknown
publication state must return `-EOWNERDEAD`. The tree then poisons the handle so
all storage-dependent operations continue returning `-EOWNERDEAD`; only
destroy/close and reopen can recover.

## 7. File Durability

The file backend creates mode-`0600` regular files, obtains an exclusive
nonblocking lock, and keeps a parent directory descriptor. Data and WAL names
are opened relative to that descriptor with `O_NOFOLLOW` and `CLOEXEC`, which
binds the data file to the WAL namespace and rejects final-component symlinks.

One commit writes a complete full-page redo image set to `.wal.tmp`, syncs it,
renames it to `.wal`, syncs the directory, installs all data pages, syncs the
database, then removes the WAL and syncs the directory. macOS regular files use
`F_FULLFSYNC`; other POSIX targets use `fsync`.

Publication makes later errors commit-unknown. The backend returns
`-EOWNERDEAD` and stays poisoned. Reopen validates and replays a complete WAL
idempotently; malformed nonempty WAL remains for diagnosis. Crash tests cover
mutation and recovery hooks, every nonempty truncated prefix, and a synthetic
4,096-record WAL.

## 8. Regression Expansion

The final eight suites are:

| Suite | Main coverage |
| --- | --- |
| `rbt_schema_test` | unified columns/records/rows, all types/flags, canonical order, UTF-8, ownership, schema-free reopen |
| `rbt_overflow_test` | interleaved physical ordinals, reopen/update/delete, independent chains, reuse, 2,048-page replacement |
| `rbt_locality_test` | exact ordinary write set, balancing bounds, poison propagation |
| `rbt_structure_test` | growth, delete, root collapse, freelist reuse |
| `rbt_model_test` | 30,000 typed mixed operations against an independent model |
| `rbt_backend_test` | 512/4096/65536 matrix, locks, modes, leases, namespace and I/O failures |
| `rbt_corruption_test` | schema, slot, topology, overflow, file and WAL corruption |
| `rbt_crash_test` | full-page redo crash/recovery prefixes and 4,096-record WAL |

All eight passed at source commit
`f41e0976977e5012cd4946fa2dca258e85aebd32` under Zig 0.16.0 Debug and
Release, ASan/UBSan, and FIL-C 0.684. The analyzer was bound to
`ccc-analyzer` and reported no bugs. The installed package consumer uses the
public unified-row API through
`find_package(rbt 0.2.0 EXACT CONFIG REQUIRED)`.

## 9. Final Review

Current review run `20260912-110259-0424385b` validated one finding: the
README contained one stale package-version statement. This documentation
update fixes it. The validator rejected a separate public key type because it
conflicts with the row-only direction; per-field allocation because it lacked
benchmark or hotspot evidence; and a cross-product interleaved-overflow gap
because it did not establish a defect and later reopen/update/delete lifecycle
coverage exists. No actionable findings remain.

## 10. Reconciliation

`RBT-RA-1` through `RBT-RA-8` bind the unified schema/record/row model,
physical-ordinal overflow, locality, structure, model, backend,
corruption/crash, and complete qualification evidence respectively. Their
exact definitions are in the study README and evidence ledger.

The remaining boundary is explicit: subprocess crashes are not physical power
loss; network filesystems are unsupported; one tree lease is not shared-tree
thread/process concurrency; no online-backup protocol exists; and no benchmark
result was collected.
