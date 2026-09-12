---
doc_id: recallfs-study-bplus-tree-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-agent-ready-docs-v1
  - learning/studies/20260911-bplus-tree/source.md
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

# Typed Record B+ Tree Study

## 1. Conclusion

This study delivers `rbt` 0.2.0, a reusable C11 typed-record B+ tree library.
Its public handle is the opaque `struct rbt`; lifecycle functions are
`rbt_create`, `rbt_open`, `rbt_destroy`, and `rbt_get_schema`, and data
operations are `rbt_get`, `rbt_put`, `rbt_delete`, `rbt_scan`, and
`rbt_validate`. Public symbols use the direct `rbt_*` namespace without an
extra tree-name segment.

This is a clean-break development format. `RBT_FORMAT_VERSION=1` is the sole
current development format, and its schema encoding magic and unified layout
are `RBTR`. Earlier `RBTS` schema files and files from the predecessor
implementation are rejected: there is no compatibility reader, migration
layer, API alias, or alternate on-disk format.

The implementation persists one ordered typed schema, derives canonical
composite keys from columns marked `RBT_COLUMN_KEY`, stores variable-length
rows in slotted pages, gives each large non-key variable column its own
overflow chain, and updates only the search path and required siblings. A
non-structural mutation without changed overflow data commits exactly metadata
plus one leaf, while balancing remains bounded by the path and siblings rather
than tree size.

At source commit `ad8e6ee95d766d424249df6894a00bcf354878ce`, all eight suites
passed under Zig Debug, Zig Release, ASan/UBSan, and FIL-C 0.684. The
`ccc-analyzer` run reported no bugs, package installation and the external
typed consumer passed, and review run `20260912-110259-0424385b` validated one
finding: this study still named the pre-unified-row package version. This
documentation update fixes that finding. No actionable findings remain.

The evidence does not establish real power-loss behavior, network filesystem
ordering, multi-thread or multi-process shared-tree access, online backup, or
performance results.

## 2. Public Contract

Row is the only public data model; key and payload handling are projections of
that model rather than separate public object types.

All status-returning public operations return `0` on success or a negative errno value.
Documented failures include `-ENOENT`, `-EINVAL`, `-ENOMEM`, `-EIO`,
`-EBADMSG`, `-ENOTSUP`, `-EBUSY`, and `-EOWNERDEAD`. Required outputs are
cleared before validation can fail.

`rbt_create` accepts an empty provider and a caller-supplied schema,
copies and persists that schema, and acquires one storage lease. `rbt_open`
accepts only storage, loads and validates the persisted schema, and therefore
supports schema-free open. `rbt_get_schema` returns a tree-owned borrowed view.
`rbt_destroy` releases the lease and rejects destruction during a scan
callback.

`struct rbt_record` has one `values` array and `value_count`; it borrows that
array and byte payloads only for the call. `rbt_put` requires and copies a
complete row in physical schema order. `rbt_get`, `rbt_delete`, and non-null
scan bounds require a compact tuple containing only `RBT_COLUMN_KEY` values in
filtered schema order. This is an operation-specific projection of the same
record representation, not a public split key/value object.

`rbt_get` returns an owned `struct rbt_row` containing one complete logical row
in physical schema order; the caller releases it with `rbt_row_destroy`.
`rbt_row_get(row, column_ordinal)` is the sole value accessor, and its result is
borrowed from the row. Scan callbacks receive the same complete logical row,
but the row and all views exist only during the callback and cannot be
destroyed by it. `RBT_SCAN_STOP` ends the scan successfully. Mutating or
destroying the same tree from a scan callback is rejected with `-EBUSY`.

Scans use `[begin, end)` bounds; either bound may be `NULL`. Equal or reversed
bounds produce an empty successful scan. Replacing an existing row reports
`inserted=false`; deleting an absent key reports `-ENOENT` and
`deleted=false`.

## 3. Persisted Schema And Keys

Each schema has a 64-bit schema ID, one ordered `columns` array, and
`column_count`. There are no public split key/payload arrays. Every column
persists:

| Field | Contract |
| --- | --- |
| `id` | unique `uint32_t` across all columns |
| `type` | `BOOL`, `I64`, `U64`, `BYTES`, or validated `UTF8` |
| `flags` | `RBT_COLUMN_NULLABLE`, `RBT_COLUMN_KEY`, and `RBT_COLUMN_DESCENDING`; `DESCENDING` requires `KEY` |
| `max_size` | zero for scalar types; `1..1 MiB` for `BYTES`/`UTF8` |

A schema must have at least one column marked `RBT_COLUMN_KEY` and at most 64
total columns. Key order is physical schema order after filtering for that
flag. Maximum declared field size is 1 MiB and maximum declared row size is
4 MiB. `RBT_COLUMN_DESCENDING` without `RBT_COLUMN_KEY` is invalid. Null values
require `RBT_COLUMN_NULLABLE`.

Composite keys are encoded column by column into one canonical byte string.
Each component carries null ordering; signed integers are sign-normalized,
unsigned integers use order-preserving bytes, and variable bytes use an
unambiguous escaped terminator. Descending columns complement their encoded
component. Ordinary byte comparison therefore implements the complete schema
order without a persisted callback identity.

## 4. Page Format And Mutation

Page, schema, and WAL metadata integers are little-endian; C structs, pointers,
and `size_t` are never persisted. Canonical-key integer components use a
separate order-preserving big-endian transform. Every logical page begins with
the same 64-byte header containing magic, format version, page type, flags,
logical page ID, item count, level, free-space bounds, two type-specific links,
CRC32C, and an auxiliary field.

| Page type | Role |
| --- | --- |
| metadata | root, freelist, item count, high-water mark, height, schema chain |
| leaf | canonical key plus descriptors/payload for non-key columns |
| internal | canonical separator plus child reference |
| schema | `RBTR` unified-column schema bytes |
| overflow | one variable non-key column's payload chain |
| free | reusable page linked from the freelist |

Leaf and internal pages use 8-byte slots: a `u32` cell offset and `u32` cell
length. Slots grow upward from the header and variable cells pack downward from
the page end. Slot bounds, non-overlap, checksums, page IDs, links, types, and
ownership are validated before use.

The leaf stores its canonical key once and descriptors only for non-key
columns. Reads decode the key fields and merge them with payload descriptors to
reconstruct the complete row in physical schema order. Large non-key `BYTES`
or `UTF8` columns spill independently. Overflow-page `AUX` identifies the
physical schema column ordinal, so interleaved key and payload columns retain
their identity through reopen, update, and delete. Changing one large payload
does not rewrite another unchanged column's chain. Keys remain inline and
schema sizing rejects a key whose maximum canonical representation cannot
support conservative page capacity.

Occupancy is deliberately conservative: create rejects schemas that cannot
provide at least three leaf and internal cells at the selected page size, and
non-root pages maintain the implementation's half-full bound. Splits, sibling
borrow, merges, root growth/collapse, overflow release, and freelist reuse are
page-local. A transaction copies only touched pages and atomically submits the
exact page set.

## 5. Storage Separation

`struct rbt_storage` is a page-provider boundary, not a buffer-pool API. It
contains `acquire`, `release`, `read_page`, `page_count`, and `commit_pages`.
The tree copies the callback table, borrows its context, owns operation scratch
buffers, and never retains provider-owned page pointers.

`acquire` reserves exactly one live tree lease per storage context; a second
tree gets `-EBUSY`. `release` returns that lease and cannot fail. Backends also
reject close/destroy while leased.

`commit_pages` receives unique logical page IDs and callback-lifetime page
buffers. Success means the entire page set is synchronously durable and
visible. A definite no-publication failure returns a normal negative errno. If
publication may be partial or unknown, the provider returns `-EOWNERDEAD`; the
tree permanently poisons that handle, and recovery requires destroy/close then
backend reopen and `rbt_open`.

The locality oracle observes exactly two page images, metadata and leaf, for
ordinary existing-row replacement, non-splitting insert, and non-rebalancing
delete. Split, borrow, merge, and root collapse write bounded path/sibling sets;
their write count is independent of total live pages.

## 6. File And WAL Contract

The file backend creates regular database and sidecar files with mode `0600`
and holds a lifetime exclusive nonblocking lock. It retains a parent
directory descriptor and opens the data basename, `.wal.tmp`, and `.wal`
relative to it with `CLOEXEC` and `O_NOFOLLOW`; final-component symlinks and
non-regular files are rejected.

Each mutation uses full-page redo:

1. Validate the exact page set and contiguous extension.
2. Write and sync the complete unpublished `.wal.tmp`.
3. Rename it atomically to `.wal` and sync the directory.
4. Install every page image and sync the database.
5. Unlink `.wal` and sync the directory.

Regular-file durability uses `F_FULLFSYNC` on macOS and `fsync` on other POSIX
platforms. Any backend callback that returns `-EOWNERDEAD` poisons the tree;
commit-unknown also poisons the file backend. Close/reopen removes an abandoned
empty temporary WAL, validates and replays a complete active WAL idempotently,
syncs the data file, and removes the WAL. A malformed nonempty WAL is preserved
for diagnosis and rejected.

## 7. Reconciliation Anchors

| Anchor | Exact contract | Independent verification |
| --- | --- | --- |
| `RBT-RA-1` | unified `columns[]` schema and `values[]` records, KEY-filtered order, complete row reconstruction/access, all types/flags, schema-free open, `RBTR` encoding and `RBTS` rejection | `rbt_schema_test`, `rbt_corruption_test` |
| `RBT-RA-2` | 4 MiB row, 1 MiB field, 64-column limits; physical-ordinal overflow across interleaved columns, reopen/update/delete, lifecycle and reuse | `rbt_overflow_test` |
| `RBT-RA-3` | ordinary mutation is metadata+leaf; split/borrow/merge/root collapse remain path/sibling bounded | `rbt_locality_test` |
| `RBT-RA-4` | conservative occupancy, structural mutation, root collapse, freelist reuse | `rbt_structure_test` |
| `RBT-RA-5` | 30,000 typed mixed operations equal an independent ordered model | `rbt_model_test` |
| `RBT-RA-6` | memory/file page-size matrix, leases, `0600`, lock, namespace, poison and reopen behavior | `rbt_backend_test` |
| `RBT-RA-7` | `RBTS`, schema/slot/physical-ordinal overflow corruption, and 4,096-record WAL/crash-prefix handling are rejected or recovered exactly | `rbt_corruption_test`, `rbt_crash_test` |
| `RBT-RA-8` | 8/8 suites pass in Debug, Release, ASan/UBSan, and FIL-C; `rbt` 0.2 package and analyzer gates pass; current review finding is closed | `evidence/README.md`, `evidence/review.md` |

## 8. Worked Examples

**Typed create and schema-free reopen.** An application creates a schema with
ascending `U64` tenant ID, descending nullable `I64` timestamp, and nullable
`UTF8` payload. `rbt_create` copies and persists all column descriptors. After
destroying the tree and reopening the backend, `rbt_open(&storage, &tree)`
recovers the schema without caller input; `rbt_get_schema` returns its borrowed
tree-owned representation.

**Interleaved row and overflow update.** A schema places a large `BYTES`
payload before two key columns, another payload between them, and a third
payload after them. Put supplies all values in that physical order; get and
delete supply only the two key values in filtered schema order. Reopen returns
the complete row through `rbt_row_get` at physical ordinals. Each payload owns
a separate overflow chain whose `AUX` is that physical ordinal. Updating the
first payload preserves the other chains, and deleting the row releases all
three.

**Crash after WAL publication.** A mutation publishes a valid full-page WAL,
writes one data page, and exits at a test hook. Reopen replays the complete WAL,
syncs the file, removes the WAL, and exposes only the committed row set.

## 9. Evidence Boundary

The raw ledger is bound to source commit
`ad8e6ee95d766d424249df6894a00bcf354878ce`; exact log SHA-256 values are in
[`evidence/README.md`](evidence/README.md). The evidence covers deterministic
process-crash prefixes and executed-path safety. It does not support claims
about physical power loss, controller caches, network filesystems, shared-tree
concurrency across threads or processes, online backup, or benchmark
performance.
