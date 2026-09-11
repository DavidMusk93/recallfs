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
  - BPT-RA-7
---

# Page-oriented B+ Tree Study

## 1. 结论先行

本 study 交付了一个可直接构建和嵌入的通用 C11 B+ tree library：

- core 使用 search-path transaction；普通 value update 只提交 metadata 和
  一个 leaf，不做全树重建；
- key/value 是调用方编码的固定宽度 opaque bytes；默认按 unsigned bytes
  lexicographic 排序，也可绑定带稳定 ID 的自定义 comparator；
- key/value width 与 comparator ID 写入 metadata，reopen 时不匹配会返回
  `BTREE_SCHEMA_MISMATCH`，避免使用错误 schema 解释磁盘数据；
- library API 版本是 1.0.0，磁盘格式版本是 2；旧的专用
  `uint64_t -> uint64_t` 格式不会被静默解释为新 schema；
- insert/delete 完整覆盖 leaf/internal split、borrow、merge、root
  grow/collapse 和 freelist reuse；
- `btree_storage` 只暴露 page copy 与 atomic page-set commit，buffer pool 可由
  调用方独立实现；
- memory backend 使用几何扩容；file backend 使用排他锁、CRC32C、
  full-page redo WAL、dirfd-relative sidecar、`CLOEXEC` 和恢复期重放；
- macOS regular-file barrier 使用 `F_FULLFSYNC`，其他 POSIX 平台使用
  `fsync`；namespace 变更使用 directory `fsync`；
- `btree_open()` 会执行全量结构校验，避免在打开后才暴露损坏拓扑。

最终 8 个 CTest suites 在 Debug、Release、ASan/UBSan 和 FIL-C 0.684 下全部
通过。Crash suite 覆盖 split 导致的 page growth、commit 五个阶段、recovery
两个阶段、有效 WAL 的每个非空截断前缀，以及 checksum/version/size 损坏。
安装后的 CMake package 也由仓内独立 consumer 通过 `find_package` 验收。

这支持在本文声明的单线程、单写者、本地 POSIX 文件系统边界内进入生产集成和
更高层 E2E。它不等价于真实断电、存储控制器、network filesystem 或并发事务
认证；这些能力不得从现有测试外推。

## 2. Contract

### Decision

实现一个边界明确、可恢复的 C11 B+ tree library。1.0 API 提供固定宽度
opaque byte key/value、point get、upsert、delete 和带可空边界的半开区间
scan。tree core 只通过 page-oriented `btree_storage` 接口读页和原子提交页
集合，不拥有 file descriptor、WAL、buffer frame、pin count、eviction 或
writeback policy。内存 backend 提供进程生命周期内的原子提交，文件 backend
使用 full-page redo WAL 提供 crash recovery。

“Production ready”在本文中不是无限承诺，而是以下可审计边界：稳定 API、
版本化 little-endian 格式、CRC32C、单写者锁、明确 commit-unknown 语义、
全量结构校验、故障注入、deterministic differential test、FIL-C 和 sanitizer
门禁。超出该边界的能力列入 Non-goals，不以测试缺失冒充已支持。

### Scope

- `learning/studies/20260911-bplus-tree/lib/include/btree.h` 公共 tree API；
- `btree_storage` page provider 与 borrowed ownership 合同；
- 内存和 POSIX 文件 backend；
- fixed-width binary key/value schema、默认 lexicographic comparator 和
  自定义 comparator；
- metadata 持久化 key/value width 与 comparator identity；
- 512 到 65,536 bytes 的 2 次幂 page size；
- API 1.0.0 与不向后兼容的 disk format v2；
- manual little-endian encoding，不持久化 C struct、pointer 或 `size_t`；
- leaf split/redistribution/merge、internal cascading split/merge、root collapse；
- page freelist reuse；
- 每页 CRC32C、format/version/page-ID/range validation；
- 文件 backend 的 process-exclusive lock 和 sidecar redo WAL；
- clean reopen、crash recovery、corruption、I/O failure 和 model differential tests。

### Non-goals

- duplicate keys、variable-length records 或 overflow pages；
- MVCC、snapshot isolation、multi-operation transaction 或 concurrent tree handle；
- 同进程/跨进程并发 reader 与 writer；
- tree 内置 LRU/LFU、prefetch、dirty eviction 或 background writeback；
- online backup、replication、compression、encryption 或 direct I/O；
- 32-bit target、big-endian target 或 network filesystem durability guarantee；
- latent media corruption repair。Checksum 只检测损坏，不重建已清理 WAL 的数据。

### Inputs And Outputs

| Boundary | Input | Output |
| --- | --- | --- |
| Tree API | fixed-width encoded key/value、nullable range bounds | copied value、ordered pairs 或 typed status |
| Tree core | logical page ID、caller-owned page buffer、atomic write set | validated pages and metadata |
| Memory backend | atomic page write set | new in-memory page image or unchanged old image |
| File backend | atomic page write set | WAL-backed durable page image or recovery-required status |
| Validator | root metadata and every allocated page | statistics or bounded corruption report |

The range contract is `[begin_key, end_key)`. A `NULL` lower or upper bound is
unbounded. Equal or reversed bounds produce an empty successful scan. Upserting
an existing key replaces its value without changing item count. Deleting an
absent key succeeds with `removed=false`.

### Interfaces And Ownership

The core receives this logical shape; exact declarations live in
`include/btree.h`:

```c
typedef struct btree_schema {
    uint32_t key_size;
    uint32_t value_size;
    uint64_t comparator_id;
} btree_schema;

typedef struct btree_options {
    uint32_t key_size;
    uint32_t value_size;
    uint64_t comparator_id;
    btree_compare_fn compare;
    void *compare_context;
} btree_options;

typedef struct btree_storage {
    void *context;
    uint32_t page_size;
    btree_status (*read_page)(void *, uint64_t, void *);
    btree_status (*page_count)(void *, uint64_t *);
    btree_status (*commit_pages)(void *, const btree_page_update *, size_t);
} btree_storage;
```

`btree_options_init()` selects stable unsigned-byte lexicographic ordering.
A custom comparator uses an application-owned ID at or above
`BTREE_COMPARATOR_USER_MIN`; the function and its context are borrowed and must
remain behaviorally identical across reopen. The ID names comparator semantics,
not a function address; the application owns comparator versioning.

`read_page` copies one immutable snapshot into caller-owned memory.
`commit_pages` atomically publishes the complete set or returns a status that
does not permit the core to assume partial pages are usable. This permits an
external buffer pool to implement the interface, but the tree never observes
frames or retains pointers after a callback returns.

`btree` owns operation scratch pages and is not thread-safe. A storage
instance has one attached tree. Comparator state and storage context are
borrowed for the tree lifetime. The file backend owns its descriptors, lock,
WAL path, recovery, and durability barriers. `btree_close` does not close the
backend.

```text
application
    |
    v
B+ tree core
    |
    v
btree_storage page contract
    |
    +----------+-------------------+------------------+
    |                              |                  |
    v                              v                  v
memory backend              file backend       buffer pool adapter
                            + redo WAL          supplied by caller
```

### Invariants

1. Keys are strictly increasing and unique under the configured comparator.
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
| Invalid argument or unsupported page size | `BTREE_INVALID_ARGUMENT` |
| Missing key | `BTREE_NOT_FOUND` for get; successful `removed=false` for delete |
| Allocation failure before commit | `BTREE_OUT_OF_MEMORY`; old tree remains visible |
| Invalid magic/version/checksum/range/cycle | `BTREE_CORRUPT` with bounded traversal |
| Unsupported format feature | `BTREE_UNSUPPORTED` |
| Reopen with different key/value width or comparator ID | `BTREE_SCHEMA_MISMATCH` |
| Data I/O before WAL commit point | `BTREE_IO`; no data page has been installed |
| I/O at or after WAL durability is attempted | `BTREE_RECOVERY_REQUIRED`; handle poisoned |
| Reopen with valid committed WAL | replay all page images, sync data, remove WAL |
| Reopen with empty abandoned WAL | remove it; data remains unchanged |
| Reopen with malformed non-empty WAL | `BTREE_CORRUPT`; preserve WAL for diagnosis |

The file backend first writes and durably syncs `<database>.wal.tmp`, atomically
renames it to `<database>.wal`, and syncs the parent directory. That publication
is the commit point. It then writes data pages, durably syncs the data file, and
only then unlinks the active WAL and syncs the directory. Recovery replay is
idempotent because records are full page images addressed by logical page ID.

### Worked Examples

**BPT-WE-1: generic schema and cascading split.**

Create a tree with 13-byte keys and 21-byte values. Insert embedded-NUL byte
records through one more than leaf capacity. The old leaf keeps the lower half,
a new leaf receives the upper half, both sibling links are updated, and the new
separator is inserted into the parent. Input buffers are overwritten after
each call; later reads still return the copied bytes. Reopen first reads the
persisted schema, then validates the caller-supplied schema before traversal.

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
| `BPT-RA-1` | ascending, descending and shuffled inserts across multiple levels | every get and full scan equals the independent sorted model | `btree_model_test` |
| `BPT-RA-2` | delete through redistribution, merge, empty root and reuse | validator passes after every boundary; high-water page count stops growing when free pages exist | `btree_structure_test` |
| `BPT-RA-3` | file close/reopen after mutations | exact item count and ordered contents survive every reopen | `btree_persistence_test` |
| `BPT-RA-4` | crash before WAL publish, after publish, during data write, during replay, and after data sync | reopen yields the exact old or committed post-state and no active WAL | `btree_crash_test` |
| `BPT-RA-5` | bad header, page checksum, child ID, sibling cycle and malformed WAL | operation returns `BTREE_CORRUPT` without loop, OOB access or silent repair | `btree_corruption_test` |
| `BPT-RA-6` | all deterministic suites under FIL-C and native sanitizers | zero test failures and zero reported memory/UB defects | evidence commands in `evidence/README.md` |
| `BPT-RA-7` | 13-byte/21-byte records plus custom 8-byte comparator schema | byte-exact copy, ordered scan, schema reopen/mismatch, split/delete all pass | `btree_generic_test` |

### Evidence And Unknowns

Observed evidence is recorded in [`evidence/README.md`](evidence/README.md).
The current ledger is bound to source commit
`b2c4bd8f6e922b06ee58aecf764f8278365a9801` plus per-file SHA-256 values.
Unknowns outside 1.0 include multi-writer scheduling, buffer-pool eviction
behavior, physical power-loss testing, network filesystem semantics, and
workload-specific performance.

## 3. Article To Implementation

The article explains why broad fixed-size nodes reduce height and why linked
leaves make ranges efficient. The library turns those shapes into explicit
contracts:

- page size controls exact leaf and internal fanout;
- no value bytes are stored in internal pages;
- all traversal reads whole pages through `btree_storage`;
- sequential scans use leaf links;
- storage caching is replaceable and intentionally outside the tree;
- disk mode adds durability machinery absent from the article.

The article's UUID versus sequential-key comparison is not benchmarked here.
The library can represent UUIDs, integers, strings, composite keys, row IDs,
or application records through fixed-width encodings, but workload performance
still requires target-machine evidence.
