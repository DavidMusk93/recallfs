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
  - BPT-RA-1
  - BPT-RA-2
  - BPT-RA-3
  - BPT-RA-4
  - BPT-RA-5
  - BPT-RA-6
---

# Page-oriented B+ Tree Study

## Contract

### Decision

实现一个边界明确、可恢复的 C11 B+ tree library。v1 提供唯一
`uint64_t -> uint64_t` 映射、point get、upsert、delete 和半开区间 scan；
tree core 只通过 page-oriented `bpt_storage` 接口读页和原子提交页集合，不拥有
file descriptor、WAL、buffer frame、pin count、eviction 或 writeback policy。
内存 backend 提供进程生命周期内的原子提交，文件 backend 使用 full-page
redo WAL 和 `fsync` 提供 crash recovery。

“Production ready”在本文中不是无限承诺，而是以下可审计边界：稳定 API、
版本化 little-endian 格式、CRC32C、单写者锁、明确 commit-unknown 语义、
全量结构校验、故障注入、deterministic differential test、FIL-C 和 sanitizer
门禁。超出该边界的能力列入 Non-goals，不以测试缺失冒充已支持。

### Scope

- `learning/studies/20260911-bplus-tree/lib/include/bptree.h` 公共 tree API；
- `bptree_storage` page provider 合同；
- 内存和 POSIX 文件 backend；
- 512 到 65,536 bytes 的 2 次幂 page size；
- manual little-endian encoding，不持久化 C struct、pointer 或 `size_t`；
- leaf split/redistribution/merge、internal cascading split/merge、root collapse；
- page freelist reuse；
- 每页 CRC32C、format/version/page-ID/range validation；
- 文件 backend 的 process-exclusive lock 和 sidecar redo WAL；
- clean reopen、crash recovery、corruption、I/O failure 和 model differential tests。

### Non-goals

- duplicate keys、variable-length keys/values、overflow pages 或 custom comparator；
- MVCC、snapshot isolation、multi-operation transaction 或 concurrent tree handle；
- 同进程/跨进程并发 reader 与 writer；
- tree 内置 LRU/LFU、prefetch、dirty eviction 或 background writeback；
- online backup、replication、compression、encryption 或 direct I/O；
- 32-bit target、big-endian target 或 network filesystem durability guarantee；
- latent media corruption repair。Checksum 只检测损坏，不重建已清理 WAL 的数据。

### Inputs And Outputs

| Boundary | Input | Output |
| --- | --- | --- |
| Tree API | `uint64_t` key/value、range bounds | exact value、ordered pairs 或 typed status |
| Tree core | logical page ID、caller-owned page buffer、atomic write set | validated pages and metadata |
| Memory backend | atomic page write set | new in-memory page image or unchanged old image |
| File backend | atomic page write set | WAL-backed durable page image or recovery-required status |
| Validator | root metadata and every allocated page | statistics or bounded corruption report |

The range contract is `[begin_key, end_key)`. `begin_key >= end_key` produces
an empty successful scan. Upserting an existing key replaces its value without
changing item count. Deleting an absent key succeeds with `removed=false`.

### Interfaces And Ownership

The core receives this logical shape; exact declarations live in
`include/bptree.h`:

```c
typedef struct {
    void *context;
    uint32_t page_size;
    bpt_status (*read_page)(void *, uint64_t, void *);
    bpt_status (*page_count)(void *, uint64_t *);
    bpt_status (*commit_pages)(void *, const bpt_page_update *, size_t);
} bpt_storage;
```

`read_page` copies one immutable snapshot into caller-owned memory.
`commit_pages` atomically publishes the complete set or returns a status that
does not permit the core to assume partial pages are usable. This permits an
external buffer pool to implement the interface, but the tree never observes
frames or retains pointers after a callback returns.

`bpt_tree` owns operation scratch pages and is not thread-safe. A storage
instance has one attached tree. The file backend owns its descriptors, lock,
WAL path, recovery, and durability barriers. `bpt_tree_close` does not close
the backend.

```text
application
    |
    v
B+ tree core
    |
    v
bpt_storage page contract
    |
    +----------+-------------------+------------------+
    |                              |                  |
    v                              v                  v
memory backend              file backend       buffer pool adapter
                            + redo WAL          supplied by caller
```

### Invariants

1. Keys are strictly increasing and unique in every leaf and ordered globally.
2. An internal separator is greater than every key in its left child and less
   than or equal to every key in its right child.
3. Every leaf is at `height - 1`; only the root may be below minimum occupancy.
4. Leaf `next`/`previous` links form one acyclic, bidirectionally consistent
   chain in tree order.
5. Every page below `next_page_id` is exactly one of metadata, reachable tree
   page, or freelist page.
6. A page reference is in range, matches its encoded page ID and type, and has
   a valid CRC32C before any payload field is trusted.
7. A successful file commit has durable WAL-before-data ordering. A failed
   commit never reports success.
8. After uncertain file I/O, the tree handle is poisoned; only close/reopen and
   WAL recovery may establish the visible state.
9. The core never calls POSIX file APIs and never caches a backend-owned page
   pointer.

### Failure Semantics

| Failure | Result |
| --- | --- |
| Invalid argument or unsupported page size | `BPT_INVALID_ARGUMENT` |
| Missing key | `BPT_NOT_FOUND` for get; successful `removed=false` for delete |
| Allocation failure before commit | `BPT_OUT_OF_MEMORY`; old tree remains visible |
| Invalid magic/version/checksum/range/cycle | `BPT_CORRUPT` with bounded traversal |
| Unsupported format feature | `BPT_UNSUPPORTED` |
| Data I/O before WAL commit point | `BPT_IO`; no data page has been installed |
| I/O at or after WAL durability is attempted | `BPT_RECOVERY_REQUIRED`; handle poisoned |
| Reopen with valid committed WAL | replay all page images, sync data, remove WAL |
| Reopen with empty abandoned WAL | remove it; data remains unchanged |
| Reopen with malformed non-empty WAL | `BPT_CORRUPT`; preserve WAL for diagnosis |

The file backend commit point is a complete, checksummed WAL followed by a
successful WAL `fsync`. It then writes data pages, `fsync`s the data file, and
only then unlinks the WAL and `fsync`s the parent directory. Recovery replay is
idempotent because records are full page images addressed by logical page ID.

### Worked Examples

**BPT-WE-1: cascading split.**

With a 512-byte page, a leaf has a finite encoded capacity. Insert ascending
keys through one more than that capacity. The old leaf keeps the lower half,
a new leaf receives the upper half, both sibling links are updated, and the
new separator is inserted into the parent. If the parent is also full, the
split propagates until a new root is created. Every get returns its exact
value and a full scan returns each key once in ascending order.

**BPT-WE-2: delete and root collapse.**

Create a height-three tree, then delete all but one key. Underfull siblings
first redistribute when one can spare an entry; otherwise they merge and the
removed page joins the freelist. Unary internal roots collapse. The final tree
is one root leaf containing the survivor, and later insertions reuse free pages
before extending `next_page_id`.

**BPT-WE-3: crash after one data page write.**

An upsert produces metadata and leaf page images. The file backend writes and
syncs the complete WAL, writes the first data page, and the process exits at an
injected crash point. Reopen validates the WAL checksum and database identity,
replays every image, syncs the data file, and removes the WAL. The reopened
tree contains the new value and passes full validation; a mixed generation is
never exposed.

### Reconciliation Anchors

| Anchor | Input or condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `BPT-RA-1` | ascending, descending and shuffled inserts across multiple levels | every get and full scan equals the independent sorted model | `bptree_model_test` |
| `BPT-RA-2` | delete through redistribution, merge, empty root and reuse | validator passes after every boundary; high-water page count stops growing when free pages exist | `bptree_structure_test` |
| `BPT-RA-3` | file close/reopen after mutations | exact item count and ordered contents survive every reopen | `bptree_persistence_test` |
| `BPT-RA-4` | crash at WAL sync, first data page and data sync | reopen yields the committed post-state and an empty/absent WAL | `bptree_crash_test` |
| `BPT-RA-5` | bad header, page checksum, child ID, sibling cycle and malformed WAL | operation returns `BPT_CORRUPT` without loop, OOB access or silent repair | `bptree_corruption_test` |
| `BPT-RA-6` | all deterministic suites under FIL-C and native sanitizers | zero test failures and zero reported memory/UB defects | evidence commands in `evidence/README.md` |

### Evidence And Unknowns

Observed evidence is recorded in [`evidence/README.md`](evidence/README.md).
Before the gates run, the implementation is a design target rather than a
validated production artifact. Unknowns that remain outside v1 include
multi-writer scheduling, buffer-pool eviction behavior, hardware power-loss
testing, network filesystem semantics, and workload-specific performance.

## Article To Implementation

The article explains why broad fixed-size nodes reduce height and why linked
leaves make ranges efficient. The library turns those shapes into explicit
contracts:

- page size controls exact leaf and internal fanout;
- no value bytes are stored in internal pages;
- all traversal reads whole pages through `bpt_storage`;
- sequential scans use leaf links;
- storage caching is replaceable and intentionally outside the tree;
- disk mode adds durability machinery absent from the article.

The article's UUID versus sequential-key comparison is not benchmarked here
because the v1 key type is numeric and the requested deliverable is a
correctness-first reusable library. Any future performance claim requires a
target-machine workload and hardware-counter evidence.
