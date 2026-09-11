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
  - BPT-RA-1
  - BPT-RA-2
  - BPT-RA-3
  - BPT-RA-4
  - BPT-RA-5
  - BPT-RA-6
  - BPT-RA-7
---

# B+ Tree C Library

## Supported Contract

This C11 library implements a unique fixed-width byte-key to byte-value B+ tree
with:

- point lookup, upsert, delete, and `[begin, end)` ordered scan;
- caller-selected key/value widths and a persisted comparator identity;
- built-in unsigned-byte lexicographic order or a custom comparator;
- page-local split, redistribution, merge, root growth, and root collapse;
- configurable 512 to 65,536 byte power-of-two pages;
- a page-provider API independent of file I/O and buffer-pool policy;
- volatile memory and durable POSIX file backends;
- versioned little-endian pages, CRC32C, full validation, and freelist reuse;
- full-page redo WAL recovery for every file mutation.

One tree handle and its storage instance are single-threaded and single-writer.
The file backend holds an exclusive nonblocking `flock` for its lifetime.
The package version is 1.0.0 and `BTREE_FORMAT_VERSION` is 2. Databases created
by the earlier specialized format are not compatible with this generic schema.

## Build And Test

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

When tests are enabled on Unix, CMake builds a separate test-only file backend
with named `_exit(86)` crash points. The installed production backend never
contains those hooks.

Install static libraries and headers:

```bash
cmake --install build --prefix /chosen/prefix
```

Consume the installed package:

```cmake
find_package(btree 1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE btree::btree btree::memory)
# Or replace btree::memory with btree::file.
```

The repository keeps a standalone consumer at `tests/package_consumer`; release
verification builds it only against the installed package.

## Minimal Memory Example

```c
#include <btree.h>
#include <btree_backends.h>

#include <string.h>

int main(void)
{
    btree_mem *backend = NULL;
    btree *tree = NULL;
    btree_storage storage;
    static const unsigned char key[4] = {0, 1, 2, 3};
    static const unsigned char value[3] = {4, 5, 6};
    unsigned char actual[3] = {0, 0, 0};
    btree_options options;
    bool inserted = false;

    if (btree_mem_create(4096, &backend) != BTREE_OK) {
        return 1;
    }
    btree_options_init(&options, sizeof(key), sizeof(value));
    storage = btree_mem_storage(backend);
    if (btree_create(&storage, &options, &tree) != BTREE_OK ||
        btree_put(tree, key, value, &inserted) != BTREE_OK ||
        btree_get(tree, key, actual) != BTREE_OK ||
        memcmp(actual, value, sizeof(value)) != 0) {
        return 2;
    }

    btree_close(tree);
    btree_mem_destroy(backend);
    return 0;
}
```

For a new file, call `btree_file_create()` followed by
`btree_create()`. For an existing file, call `btree_file_open()`,
`btree_read_schema()`, and then `btree_open()` with matching options. Close the
tree before closing its backend.

## Generic Key/Value Contract

The library copies exactly `key_size` and `value_size` bytes on every put.
Pointers passed to get/put/delete remain caller-owned. Keys and values may
contain zero bytes and need no C string terminator.

`btree_options_init()` selects `BTREE_COMPARATOR_LEXICOGRAPHIC`, which compares
keys as unsigned byte sequences. Portable integer order therefore uses a
canonical big-endian encoding. Applications that use another ordering provide
a deterministic strict-total-order callback and a stable
`comparator_id >= BTREE_COMPARATOR_USER_MIN`.

The metadata page persists key width, value width, and comparator ID. Reopen
requires all three to match and returns `BTREE_SCHEMA_MISMATCH` otherwise.
Comparator code and context are not serialized; the application owns them for
the full tree lifetime. A comparator ID identifies ordering semantics, not a
function address. Reusing an ID for changed semantics can invalidate ordering
and is an application error.

## Buffer Pool Integration

The tree contains no cache. A caller-owned buffer pool implements three
callbacks:

| Callback | Required behavior |
| --- | --- |
| `read_page` | Copy one immutable page snapshot into the supplied buffer |
| `page_count` | Return the current logical high-water page count |
| `commit_pages` | Atomically publish the complete write set or return a typed failure |

The tree never retains a provider-owned pointer and never asks to pin, unpin,
evict, flush, or allocate a frame. A buffer-pool adapter owns those operations.
It must serialize one tree operation at a time and keep reads consistent with
the last successful `commit_pages`.

A custom provider must obey this failure rule:

- a failure that provably publishes no page may return `BTREE_IO` or
  `BTREE_OUT_OF_MEMORY`;
- a failure that may have published any part must return
  `BTREE_RECOVERY_REQUIRED`;
- after `BTREE_RECOVERY_REQUIRED`, all calls on the tree return that status and
  the caller must close and reopen through a recovering provider.

## Page Format

All integers are encoded little-endian. Raw C structs are never persisted.

| Logical page | Role |
| --- | --- |
| `0` | tree metadata: root, freelist head, item count, page high-water, height |
| `1..N` | leaf, internal, or freelist pages |

Every logical page has a 64-byte header containing magic, format version, page
type, logical page ID, cell count, level, sibling/freelist links, and CRC32C.
Metadata schema fields follow that header. Leaf cells use
`key_size + value_size` bytes. Internal payloads use one initial child ID plus
repeated `key_size + child-ID` records. A separator is the minimum key accepted
by its right child.

The file backend reserves physical page zero for a separate database header.
Logical page `N` is stored at physical offset `(N + 1) * page_size`. The header
binds the file-format version, tree-format version, page size, database UUID,
feature flags, and CRC32C.

Unknown versions or feature flags return `BTREE_UNSUPPORTED`. Invalid checksums,
IDs, ranges, topology, file length, UUID binding, or WAL shape return
`BTREE_CORRUPT`.

## WAL Protocol

Each public mutation is one transaction containing full page images.

```text
build write set
      |
      v
write and sync <db>.wal.tmp
      |
      v
rename to <db>.wal + sync directory
      |
      v
install data pages + fsync data file
      |
      v
unlink WAL + fsync directory
```

The WAL records up to 256 page images plus database UUID, both format versions,
page size, original and final page counts, unique logical page IDs, exact total
length, and an aggregate CRC32C. Open removes the fixed unpublished temporary
sidecar, then replays a complete valid active WAL idempotently before exposing
the provider. A malformed nonempty active WAL is preserved and open fails with
`BTREE_CORRUPT`.

Before the temporary WAL is published, a write or sync failure returns
`BTREE_IO` and leaves data pages untouched. After atomic rename publishes the
active WAL, any I/O failure returns `BTREE_RECOVERY_REQUIRED` because user space
cannot prove whether installation completed. An application must treat that
operation result as unknown until reopen and lookup.

## Validation

`btree_open()` performs full structural validation, not only metadata
parsing. `btree_validate()` repeats it on demand and checks:

- strict key order and separator ranges;
- exact height and non-root occupancy;
- child-reference bounds and absence of duplicate/cyclic ownership;
- bidirectional leaf-chain consistency;
- item count;
- freelist type, bounds, cycles, and disjointness;
- ownership of every allocated logical page.

Validation is linear in allocated pages and should be scheduled deliberately
for very large trees. Point operations validate every page they read but do
not rescan the whole tree.

## Complexity

Let `h` be tree height, `F` page fanout, and `k` returned scan items.

| Operation | Tree work | Maximum normal write scope |
| --- | --- | --- |
| get | `O(h log F)` comparator calls | none |
| same-value put | `O(h log F)` comparator calls | none |
| value replace without split | `O(h log F)` comparator calls | metadata plus one leaf |
| insert/delete without rebalance | `O(h log F)` comparator calls | metadata, leaf, and any changed ancestor separator |
| split/merge | `O(hF)` worst case | changed path, siblings, metadata, new/free pages |
| range scan | `O(h log F + k)` | none |
| full validate | `O(allocated pages)` | none |

These are algorithmic bounds, not benchmark results.

## Operational Rules

1. Keep the database and `.wal` sidecar in the same trusted local directory.
2. Do not copy a live database file without coordinating with the exclusive
   owner.
3. Do not delete or edit `.wal` or `.wal.tmp` after an error; let open apply
   the recovery rules.
4. Treat `BTREE_CORRUPT` as a hard stop and preserve both files for diagnosis.
5. Treat `BTREE_RECOVERY_REQUIRED` as commit-unknown; close and reopen.
6. Keep one backup generation outside the database/WAL pair. Checksums detect
   latent corruption but cannot repair it after WAL cleanup.
7. Run the complete native, FIL-C, sanitizer, and crash suites for every source
   change affecting page encoding, balancing, storage, or recovery.

## Unsupported Production Scenarios

Do not use this 1.0 API for variable-width records, duplicate keys, MVCC,
concurrent readers and writers, network filesystems, online backup, or
platforms whose directory `fsync`, `flock`, atomic rename/unlink, and
regular-file durability semantics do not match the documented POSIX
assumptions. On macOS, regular files use `F_FULLFSYNC`. No real power-cut or
storage-controller fault campaign has been completed; subprocess crash
injection verifies ordering and replay logic, not hardware behavior.
