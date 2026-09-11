---
doc_id: recallfs-runbook-bplus-tree-lib-v1
kind: runbook
status: active
authority: design
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-study-bplus-tree-v1
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

# rbt C Library

## Supported Contract

`rbt` 0.1.0 is a C11 typed-record B+ tree. The public tree type is opaque
`struct rbt`. Use:

- `rbt_create`, `rbt_open`, `rbt_destroy`, `rbt_get_schema`;
- `rbt_get`, `rbt_put`, `rbt_delete`, `rbt_scan`, `rbt_validate`;
- `rbt_row_get_key`, `rbt_row_get_value`, `rbt_row_destroy`;
- memory backend `rbt_mem_*` or file backend `rbt_file_*`.

Public symbols use the direct `rbt_*` namespace without an extra tree-name
segment. Public functions return `0` or negative errno.
`RBT_FORMAT_VERSION=1` is the only on-disk tree format. This is a clean-break
development format: predecessor files are rejected, and the package contains
no compatibility reader, migration layer, alias, or second format.

## Build, Install, And Consume

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /chosen/prefix
```

An installed consumer uses:

```cmake
find_package(rbt 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE rbt::rbt rbt::memory rbt::file)
```

Link `rbt::rbt` plus the provider needed by the application.
`rbt::memory` and `rbt::file` are separate backend targets. The tracked
`tests/package_consumer` project verifies all three exported targets using only
the installed package.

## Typed Record Example

This example matches the current public header and package consumer:

```c
#include <rbt.h>
#include <rbt_backends.h>

#include <string.h>

int main(void) {
    static const char message[] = "typed package record";
    const struct rbt_column key_column = {
        .id = 1u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u,
    };
    const struct rbt_column value_column = {
        .id = 2u, .type = RBT_TYPE_UTF8, .flags = 0u, .max_size = 64u,
    };
    const struct rbt_schema schema = {
        .id = 1u,
        .key_columns = &key_column,
        .key_column_count = 1u,
        .value_columns = &value_column,
        .value_column_count = 1u,
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {.storage = &storage, .schema = &schema};
    struct rbt *tree = NULL;
    struct rbt_value key_value = {
        .type = RBT_TYPE_U64, .is_null = false, .as.u64 = 42u,
    };
    struct rbt_value text_value = {
        .type = RBT_TYPE_UTF8,
        .is_null = false,
        .as.bytes = {.data = message, .size = sizeof(message) - 1u},
    };
    struct rbt_record record = {
        .key = &key_value, .key_count = 1u,
        .value = &text_value, .value_count = 1u,
    };
    struct rbt_record key = {
        .key = &key_value, .key_count = 1u,
        .value = NULL, .value_count = 0u,
    };
    struct rbt_row *row = NULL;
    const struct rbt_value *actual = NULL;
    bool inserted = false;
    int result = 0;

    if (rbt_mem_create(512u, &memory) != 0 ||
        rbt_mem_storage(memory, &storage) != 0 ||
        rbt_create(&config, &tree) != 0 ||
        rbt_put(tree, &record, &inserted) != 0 || !inserted ||
        rbt_get(tree, &key, &row) != 0 ||
        rbt_row_get_value(row, 0u, &actual) != 0 ||
        actual->type != RBT_TYPE_UTF8 || actual->is_null ||
        actual->as.bytes.size != sizeof(message) - 1u ||
        memcmp(actual->as.bytes.data, message, sizeof(message) - 1u) != 0) {
        result = 1;
    }

    if (row != NULL && rbt_row_destroy(row) != 0) result = 2;
    if (tree != NULL && rbt_destroy(tree) != 0) result = 3;
    if (memory != NULL && rbt_mem_destroy(memory) != 0) result = 4;
    return result;
}
```

For a file tree, create/open the backend with `rbt_file_create` or
`rbt_file_open`, obtain `rbt_storage` with `rbt_file_storage`, and then call
`rbt_create` or `rbt_open`. `rbt_open` takes no schema; inspect the persisted,
tree-owned schema with `rbt_get_schema`.

## Schema And Ordering

`struct rbt_column` persists `id`, `type`, `flags`, and `max_size`.

| Type | Value member | Size contract |
| --- | --- | --- |
| `RBT_TYPE_BOOL` | `as.boolean` | `max_size == 0` |
| `RBT_TYPE_I64` | `as.i64` | `max_size == 0` |
| `RBT_TYPE_U64` | `as.u64` | `max_size == 0` |
| `RBT_TYPE_BYTES` | `as.bytes` | `0 < max_size <= 1 MiB` |
| `RBT_TYPE_UTF8` | `as.bytes` | same bound plus valid UTF-8 |

`RBT_COLUMN_NULLABLE` is valid for key and value columns.
`RBT_COLUMN_DESCENDING` is valid only for key columns. IDs are unique across
both arrays; keys contain at least one column; total columns are at most 64;
the maximum declared row is at most 4 MiB.

Composite keys have a canonical order-preserving encoding. Components encode
null state and type value without ambiguity. Signed and unsigned integers are
normalized for byte order, variable data is escaped and terminated, and a
descending component is complemented. Internal separators and leaf keys use
the same encoding.

Create borrows the caller's schema only for the call, then copies and persists
it. Open loads the schema from disk without caller input. The pointer returned
by `rbt_get_schema` is borrowed from the tree and expires at `rbt_destroy`.

## Record And Row Ownership

`struct rbt_record` borrows arrays and byte payloads for one operation. Get,
delete, and scan bounds contain key values only; put must supply all key and
value columns.

`rbt_get` allocates an owned row. Row accessors return views borrowed from that
row; call `rbt_row_destroy` once after use. Scan creates callback-lifetime rows:
the row and all accessor results become invalid when the callback returns, and
`rbt_row_destroy` returns `-EINVAL` for such a row. Returning
`RBT_SCAN_STOP` is a successful early stop. Same-tree scan reentry and destroy
return `-EBUSY`.

## Provider And Lease Integration

Every `rbt_storage` callback is mandatory:

| Callback | Required behavior |
| --- | --- |
| `acquire` | reserve one live tree lease; return `-EBUSY` if already leased |
| `release` | relinquish a successful lease; cannot fail |
| `read_page` | copy exactly `page_size` bytes into caller memory |
| `page_count` | return the number of addressable logical pages |
| `commit_pages` | synchronously and atomically publish the complete unique page set |

The tree copies the table and borrows `context`. Update arrays and buffers are
caller-owned and valid only during `commit_pages`; the provider may not retain
them. The core has no file descriptor, frame, pin, eviction, or writeback
policy.

One storage context supports one live tree. Backends reject destroy/close while
the lease is held. A provider returns an ordinary negative errno only when it
knows no partial publication escaped. Unknown publication must return
`-EOWNERDEAD`; the tree poisons that handle until destroy/close and reopen.

## Page Layout

Page sizes are powers of two from 512 through 65,536 bytes. Page/schema/WAL
metadata integers are little-endian; canonical key integers use a distinct
order-preserving big-endian transform. All pages have a 64-byte header:

| Region | Contents |
| --- | --- |
| fixed header | magic/version/type/ID/count/level/free bounds/links/CRC32C/aux |
| slot array | one `u32` offset plus one `u32` length per leaf/internal cell |
| free space | gap between upward slots and downward cells |
| cells | variable leaf rows or internal separator/child entries |

Logical types are metadata, leaf, internal, schema, overflow, and free pages.
Metadata points to the root, schema chain, and freelist. Large variable value
columns each own an independent overflow chain; keys do not spill.

Capacity uses declared maxima and conservative occupancy. A schema must support
at least three leaf and internal cells for its page size. Non-root pages retain
the half-full invariant. Split, borrow, merge, root collapse, overflow release,
and freelist allocation are local to touched pages.

## Mutation Locality

Each public mutation builds one copied write set and calls `commit_pages` once.
Evidence observes:

| Mutation | Exact/bounded page set |
| --- | --- |
| same-value put | no commit |
| existing value replacement without overflow changes | metadata + leaf, exactly 2 |
| non-splitting insert | metadata + leaf, exactly 2 |
| non-rebalancing delete | metadata + leaf, exactly 2 |
| split/borrow/merge/root collapse | metadata plus bounded path/sibling/new/free pages |
| one large-column replacement | metadata, leaf, and only that column's overflow pages |

These are write-locality results, not throughput or latency benchmarks.

## File And WAL Operations

The file backend:

- creates database and WAL files as regular mode-`0600` files;
- holds an exclusive nonblocking lock;
- retains a directory FD and uses dirfd-relative names;
- uses `O_NOFOLLOW` and `CLOEXEC`;
- writes a full-page redo `.wal.tmp`, syncs it, atomically publishes `.wal`,
  syncs the directory, installs pages, syncs data, then removes and syncs;
- uses `F_FULLFSYNC` for macOS regular files and `fsync` elsewhere;
- returns `-EOWNERDEAD` and poisons the backend after commit-unknown failure;
- recovers only through close/reopen and idempotent WAL replay.

Do not delete or edit `.wal` or `.wal.tmp` after failure. Preserve malformed
nonempty WAL files for diagnosis.

## Validation And Operations

`rbt_open` performs full validation. `rbt_validate` checks page headers and
checksums, schema and slot bounds, strict key order, separator ranges, height
and occupancy, leaf links, overflow chain identity/length, freelist
disjointness, item count, and ownership of every allocated page.

Operational rules:

1. Destroy the tree before destroying/closing its backend.
2. Keep file data and sidecars in one trusted local directory.
3. Treat `-EBADMSG` as corruption and preserve files.
4. Treat `-EOWNERDEAD` as commit-unknown and close/reopen.
5. Run all eight suites plus analyzer and package gates after format, storage,
   balancing, schema, or recovery changes.

## Unsupported Production Scenarios

No evidence supports real power-loss or controller-cache guarantees, network
filesystems, multi-thread or multi-process access to one live tree, online
backup, or benchmark performance. The exclusive file lock and one-tree lease
are safety boundaries, not a concurrent access protocol.
