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
---

# B+ Tree C Library

## Supported Contract

This C11 library implements a unique `uint64_t -> uint64_t` B+ tree with:

- point lookup, upsert, delete, and `[begin, end)` ordered scan;
- page-local split, redistribution, merge, root growth, and root collapse;
- configurable 512 to 65,536 byte power-of-two pages;
- a page-provider API independent of file I/O and buffer-pool policy;
- volatile memory and durable POSIX file backends;
- versioned little-endian pages, CRC32C, full validation, and freelist reuse;
- full-page redo WAL recovery for every file mutation.

One tree handle and its storage instance are single-threaded and single-writer.
The file backend holds an exclusive nonblocking `flock` for its lifetime.

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

Link the core plus one backend:

```text
-lbptree_storage_memory -lbptree
-lbptree_storage_file -lbptree
```

## Minimal Memory Example

```c
#include <bptree.h>
#include <bptree_backends.h>

int main(void)
{
    bpt_memory_backend *backend = NULL;
    bpt_tree *tree = NULL;
    bpt_storage storage;
    bool inserted = false;
    uint64_t value = 0;

    if (bpt_memory_backend_create(4096, &backend) != BPT_OK) {
        return 1;
    }
    storage = bpt_memory_backend_storage(backend);
    if (bpt_tree_create(&storage, &tree) != BPT_OK ||
        bpt_tree_put(tree, 42, 84, &inserted) != BPT_OK ||
        bpt_tree_get(tree, 42, &value) != BPT_OK || value != 84) {
        return 2;
    }

    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
    return 0;
}
```

For a new file, call `bpt_file_backend_create()` followed by
`bpt_tree_create()`. For an existing file, call `bpt_file_backend_open()`
followed by `bpt_tree_open()`. Close the tree before closing its backend.

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

- a failure that provably publishes no page may return `BPT_IO` or
  `BPT_OUT_OF_MEMORY`;
- a failure that may have published any part must return
  `BPT_RECOVERY_REQUIRED`;
- after `BPT_RECOVERY_REQUIRED`, all calls on the tree return that status and
  the caller must close and reopen through a recovering provider.

## Page Format

All integers are encoded little-endian. Raw C structs are never persisted.

| Logical page | Role |
| --- | --- |
| `0` | tree metadata: root, freelist head, item count, page high-water, height |
| `1..N` | leaf, internal, or freelist pages |

Every logical page has a 64-byte header containing magic, format version, page
type, logical page ID, cell count, level, sibling/freelist links, and CRC32C.
Leaf cells are `(key, value)` pairs. Internal payloads alternate child page IDs
and separator keys. A separator is the minimum key accepted by its right child.

The file backend reserves physical page zero for a separate database header.
Logical page `N` is stored at physical offset `(N + 1) * page_size`. The header
binds the file-format version, tree-format version, page size, database UUID,
feature flags, and CRC32C.

Unknown versions or feature flags return `BPT_UNSUPPORTED`. Invalid checksums,
IDs, ranges, topology, file length, UUID binding, or WAL shape return
`BPT_CORRUPT`.

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
length, and an aggregate CRC32C. Open removes the fixed unpublished temporary sidecar, then
replays a complete valid active WAL idempotently before exposing the provider.
A malformed nonempty active WAL is preserved and open fails with
`BPT_CORRUPT`.

Before the temporary WAL is published, a write or sync failure returns
`BPT_IO` and leaves data pages untouched. After atomic rename publishes the
active WAL, any I/O failure returns `BPT_RECOVERY_REQUIRED` because user space
cannot prove whether installation completed. An application must treat that
operation result as unknown until reopen and lookup.

## Validation

`bpt_tree_open()` performs full structural validation, not only metadata
parsing. `bpt_tree_validate()` repeats it on demand and checks:

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
| get | `O(h log F)` | none |
| same-value put | `O(h log F)` | none |
| value replace without split | `O(h log F)` | metadata plus one leaf |
| insert/delete without rebalance | `O(h log F)` | metadata, leaf, and any changed ancestor separator |
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
4. Treat `BPT_CORRUPT` as a hard stop and preserve both files for diagnosis.
5. Treat `BPT_RECOVERY_REQUIRED` as commit-unknown; close and reopen.
6. Keep one backup generation outside the database/WAL pair. Checksums detect
   latent corruption but cannot repair it after WAL cleanup.
7. Run the complete native, FIL-C, sanitizer, and crash suites for every source
   change affecting page encoding, balancing, storage, or recovery.

## Unsupported Production Scenarios

Do not use this v1 for variable records, duplicate keys, MVCC, concurrent
readers and writers, network filesystems, online backup, or platforms whose
directory `fsync`, `flock`, atomic rename/unlink, and regular-file durability
semantics do not match the documented POSIX assumptions. On macOS, regular
files use `F_FULLFSYNC`. No real power-cut or storage-controller fault campaign
has been completed; subprocess crash injection verifies ordering and replay
logic, not hardware behavior.
