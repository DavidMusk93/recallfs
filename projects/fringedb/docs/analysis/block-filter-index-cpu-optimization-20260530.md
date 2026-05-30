# Block Filter Index CPU 优化分析：cache 与写入模式视角

## 结论

在 PID `2098966` 上补充了更长周期 eBPF 采样：`profile + uprobe` 连续 `90s`，`BlockFilterIndex::Read/MergePrefixFrom` 连续 `60s`。结论与上一轮一致，但从 cache / 写入模式视角可以进一步定位到：当前 `BFI3` 为读侧按 bucket on-demand 访问优化了 cache 与 IO，但写侧 flush 仍然按完整 bucket-major 文件重写，并在每次 `BlockFilterIndex::Write` 时重新做 rowgroup-major 到 bucket-major 的转置。该模式导致写侧 CPU 被 full rewrite + full transpose 放大。

最值得优先做的 CPU 优化不是单纯减少 `write(2)`，而是改变写入模式：**避免每次 flush 对所有 bucket × 所有 row group 做全量转置**。推荐路径是先做低风险的稀疏写出/跳过空 prefix，再演进到分段增量 sidecar 或写侧 bucket-major cache。

关键源码位置：

- `src/fringedb/detail/block_filter_index.cc:62`：`WriteHashBucketMajor`，当前写侧 CPU 热点。
- `src/fringedb/detail/block_filter_index.cc:73`：外层遍历每个 bucket。
- `src/fringedb/detail/block_filter_index.cc:75`：内层遍历所有 row group。
- `src/fringedb/detail/block_filter_index.cc:77`：eBPF 热点落点，按 bucket 读取每个 row group 的 bitset。
- `src/fringedb/detail/block_filter_index.cc:321`：写入路径维护 file-level bitset cache。
- `src/fringedb/detail/block_filter_index.cc:324`：file-level bitset 按 rowgroup bitset 做 OR，成本为 `O(bitset_bytes)`。
- `src/fringedb/detail/block_filter_index.cc:340`：`BFI3` 读路径。
- `src/fringedb/detail/block_filter_index.cc:342`：`<=256KiB` 才 full read 到 `sidecar_buffer_`。
- `src/fringedb/detail/block_filter_index.cc:438`：大文件只记录 file bitset offset。
- `src/fringedb/detail/block_filter_index.cc:445`：大文件只记录 bucket-major offset。
- `src/fringedb/detail/block_filter_index.cc:459`：大文件跳过 bucket-major 数据，不在内存中 materialize。
- `src/fringedb/detail/block_filter_index.cc:791`：`MergePrefixFrom` 合并已有 index。
- `src/fringedb/detail/block_filter_index.cc:825`：合并 hash rowgroup bitsets。
- `src/fringedb/detail/block_filter_index.cc:831`：仅当已有 rowgroup bitset 已经 materialize 时才能复制 prefix。
- `src/fringedb/detail/block.cc:3041`：`FlushBlockFilterIndex`。
- `src/fringedb/detail/block.cc:3047`：每次 flush 前读取已有 `block_filter_summary`。
- `src/fringedb/detail/block.cc:3050`：将已有 index merge 到当前内存 index。
- `src/fringedb/detail/block.cc:3053`：重新写 `block_filter_summary.swp`。
- `src/fringedb/options.h:359`：默认 `row_group_flush_every = 10`。
- `src/fringedb/options.h:360`：默认 `row_group_flush_interval = 30s`。

## 新索引格式与结构关系

### BFI3 sidecar 文件布局

`block_filter_summary` 的当前新格式是 `BFI3`。代码入口在 `BlockFilterIndex::Write/Read`，魔数定义为 `BlockFilterIndex::kMagic = "BFI3"`。文件整体是一个单文件 sidecar，先写 header，再写 structured summaries，最后写 hash summaries。hash 的 rowgroup 级数据使用 bucket-major 布局，读侧按 bucket on-demand 读取。

```text
+============================================================================+
| block_filter_summary: BFI3 sidecar                                          |
| no padding between fields; all fields are written sequentially              |
+============================================================================+
| magic: "BFI3"                                                               |
| size = 4 bytes                                                              |
+----------------------------------------------------------------------------+
                                      |
                                      v
+============================================================================+
| Header                                                                      |
| size = 12 + 4 * num_all_columns bytes                                       |
+----------------------------------------------------------------------------+
| valid_start_row_group_index: int32_t, 4 bytes                               |
| num_row_groups:              int32_t, 4 bytes                               |
| num_all_columns:             int32_t, 4 bytes                               |
| column_indexes[N]:           int32_t[N], 4 * N bytes                        |
+----------------------------------------------------------------------------+
                                      |
                                      v
+============================================================================+
| StructuredSection                                                           |
+----------------------------------------------------------------------------+
| structured_count: int32_t, 4 bytes                                          |
| repeated StructuredColumnEntry                                              |
+============================================================================+
                                      |
                                      v
        +--------------------------------------------------------------------+
        | StructuredColumnEntry                                              |
        | size = 4 + 4 + 18 + rowgroup_count * 18 bytes                      |
        +--------------------------------------------------------------------+
        | storage_key:       int32_t, 4 bytes                                |
        | rowgroup_count:    int32_t, 4 bytes                                |
        | file_summary:      18 bytes                                        |
        |   valid:           uint8_t, 1 byte                                 |
        |   kind:            uint8_t, 1 byte                                 |
        |   min_primary:     uint64_t, 8 bytes, may be unaligned             |
        |   max_primary:     uint64_t, 8 bytes, may be unaligned             |
        | rowgroup_summary:  18 bytes * rowgroup_count                       |
        |   each rg summary = valid(1) + kind(1) + min(8) + max(8)           |
        +--------------------------------------------------------------------+
                                      |
                                      v
+============================================================================+
| HashSection                                                                 |
+----------------------------------------------------------------------------+
| hash_count: int32_t, 4 bytes                                                |
| repeated HashColumnEntry                                                    |
+============================================================================+
                                      |
                                      v
        +--------------------------------------------------------------------+
        | HashColumnEntry                                                     |
        | fixed size = 16 bytes                                               |
        | total size = 16 + bitset_bytes + bucket_major_bytes                 |
        +--------------------------------------------------------------------+
        | column:         int32_t,  4 bytes                                   |
        | rowgroup_count: int32_t,  4 bytes                                   |
        | bitset_bytes:   int32_t,  4 bytes                                   |
        | num_hashes:     uint32_t, 4 bytes                                   |
        +--------------------------------------------------------------------+
                                      |
                   +------------------+------------------+
                   |                                     |
                   v                                     v
        +----------------------------+       +--------------------------------+
        | file_bitset                |       | bucket_major_bitmaps           |
        | size = bitset_bytes        |       | size = bucket_major_bytes      |
        | one bit per hash bucket    |       | bucket0: rowgroup bitmap       |
        | OR(all rowgroup bitsets)   |       | bucket1: rowgroup bitmap       |
        +----------------------------+       | ...                            |
                                             | bucketN: rowgroup bitmap       |
                                             +--------------------------------+

Formulas:
  bucket_count = bitset_bytes * 8
  rowgroup_bitmap_bytes = ceil(rowgroup_count / 8)
  bucket_major_bytes = bucket_count * rowgroup_bitmap_bytes
```

运行时默认配置来自 `HashPrefixSummaryConfig` 与 `SetBlockFilterIndexColumns`：

```text
+============================================================================+
| Hash-prefix runtime defaults                                                |
+----------------------------------------------------------------------------+
| If hash_prefix_summary_config is absent:                                    |
|   no hash-prefix BlockFilterIndex is built                                  |
|                                                                            |
| If hash_prefix_summary_config.keys is set and bucket_count == 0:            |
|   bucket_count = AutoBucketCount(row_group_size)                            |
|                = next_power_of_two(row_group_size * 10), at least 8         |
|                                                                            |
| If hash_prefix_summary_config.keys is set and num_hashes == 0:              |
|   num_hashes = AutoNumHashes(row_group_size, bucket_count)                  |
|              = clamp(round((bucket_count / row_group_size) * 0.693), 1, 16) |
+----------------------------------------------------------------------------+
| With default row_group_size = 1,048,576:                                    |
|   bucket_count = 16,777,216 = 1 << 24                                       |
|   bitset_bytes = bucket_count / 8 = 2,097,152 = 2 MiB                       |
|   num_hashes = 11                                                           |
+============================================================================+
```

`row_group_max_rows` 在当前代码里对应 `WriteOptions::row_group_size`，默认配置出处是 `src/fringedb/options.h`：

```cpp
int64_t row_group_size = 1024 * 1024;
```

运行时行为可以按下面理解：

```text
+============================================================================+
| row_group_size / row_group_max_rows behavior                                |
+----------------------------------------------------------------------------+
| Default source:                                                             |
|   WriteOptions::row_group_size = 1024 * 1024 rows                           |
|                                                                            |
| Write path usage:                                                           |
|   Parquet WriteTable(..., options_->row_group_size)                         |
|   GroupAccumulator(..., options->row_group_size)                            |
|                                                                            |
| Does it auto-adjust by actual rows in one block/file?                       |
|   No. The threshold is fixed from options at writer construction time.       |
|                                                                            |
| Can it be adjusted by design/configured rows?                               |
|   Yes. If the caller sets options.row_group_size, both row group splitting   |
|   and hash-prefix auto sizing use that configured value.                    |
|                                                                            |
| What if a real row group has fewer rows?                                    |
|   Timeout/FlushAll/last partial data can create smaller row groups, but      |
|   bucket_count and num_hashes are still derived from configured             |
|   row_group_size, not from each actual row group's row count.                |
+============================================================================+
```

这意味着默认 hash-prefix rowgroup bitset 的逻辑容量是每列每 row group 约 `2MiB`。`BFI3` 磁盘格式不直接保存 `rowgroup_count` 份 rowgroup-major bitset，而是保存转置后的 dense bucket-major 矩阵；它的大小由 `bucket_count * ceil(rowgroup_count / 8)` 决定。

默认配置下，以单个 hash-prefix 列、`rowgroup_count = 128` 为例：

```text
+============================================================================+
| Hash-prefix size calculation example                                        |
+----------------------------------------------------------------------------+
| Configured row_group_size:            1,048,576 rows                        |
| Auto bucket_count:                   16,777,216 buckets                     |
| Auto num_hashes:                             11 hash positions per value    |
| bitset_bytes per logical row group:   2,097,152 bytes = 2 MiB               |
| rowgroup_count:                             128 row groups                  |
| rowgroup_bitmap_bytes per bucket:            16 bytes = ceil(128 / 8)       |
+----------------------------------------------------------------------------+
| BFI3 bucket_major_bitmaps bytes:                                            |
|   bucket_count * rowgroup_bitmap_bytes                                      |
| = 16,777,216 * 16                                                           |
| = 268,435,456 bytes                                                         |
| = 256 MiB                                                                   |
+----------------------------------------------------------------------------+
| BFI3 file_bitset bytes:                                                     |
|   bitset_bytes = bucket_count / 8                                           |
| = 16,777,216 / 8                                                            |
| = 2,097,152 bytes                                                           |
| = 2 MiB                                                                     |
+----------------------------------------------------------------------------+
| BFI3 HashColumnEntry payload bytes, ignoring 16-byte fixed header:          |
|   file_bitset + bucket_major_bitmaps                                        |
| = 2 MiB + 256 MiB                                                           |
| = 258 MiB                                                                   |
+============================================================================+
```

容易混淆的是 `row_group_size * num_hashes * rowgroup_count / 8`：

```text
1,048,576 * 11 * 128 / 8
= 184,549,376 bytes
= 176 MiB
```

这个 `176MiB` 不是 `BFI3` 的磁盘存储大小，而是把 `rows * num_hashes` 当成“置位尝试次数”后换算出来的 bit 数。真实的 Bloom-style bitmap 是定长 dense bitset：不管实际置位多少，单个 row group 都保留 `bucket_count` 个 bucket 位；落盘的 bucket-major 矩阵也保留 `bucket_count * ceil(rowgroup_count / 8)` 个 bit slot，并且会有 rowgroup byte padding。因此存储大小应使用 `bucket_count`，不是 `row_group_size * num_hashes`。

### 磁盘字段大小与对齐

`BFI3` 的磁盘格式不是 C++ struct dump，而是按 `BlockFilterIndex::Write` 中的 `WritePod(output, value)` 顺序逐字段写入。`WritePod` 直接写 `sizeof(T)` 字节，字段之间没有显式 padding，也没有按 4/8 字节边界补齐。当前实现也没有做字节序转换，磁盘内容使用本机 POD 表示；在当前 x86_64 环境下可理解为 little-endian。

```text
+--------------------------------------------------------------------------+
| Alignment rule                                                           |
+--------------------------------------------------------------------------+
| no struct packing on disk                                                |
| no padding bytes between fields                                          |
| next field offset = previous field offset + previous field size          |
| uint64_t fields can appear at unaligned offsets because previous fields   |
| may be uint8_t                                                           |
+--------------------------------------------------------------------------+
```

Header 固定部分：

```text
+-----------------------------+-------------+------------------------------+
| Field                       | Size        | Notes                        |
+-----------------------------+-------------+------------------------------+
| magic                       | 4 bytes     | "BFI3"                       |
| valid_start_row_group_index | 4 bytes     | int32_t                      |
| num_row_groups              | 4 bytes     | int32_t                      |
| num_all_columns             | 4 bytes     | int32_t                      |
| column_indexes[i]           | 4 bytes * N | int32_t, N = num_all_columns |
+-----------------------------+-------------+------------------------------+

header_bytes = 4 + 12 + 4 * num_all_columns
```

Structured section：

```text
+-----------------------------+-------------+------------------------------+
| Field                       | Size        | Notes                        |
+-----------------------------+-------------+------------------------------+
| structured_count            | 4 bytes     | int32_t                      |
+-----------------------------+-------------+------------------------------+

Repeated StructuredColumnEntry:

+-----------------------------+-------------+------------------------------+
| Field                       | Size        | Notes                        |
+-----------------------------+-------------+------------------------------+
| storage_key                 | 4 bytes     | int32_t                      |
| rowgroup_count              | 4 bytes     | int32_t                      |
| file_valid                  | 1 byte      | uint8_t, 0/1                 |
| file_kind                   | 1 byte      | uint8_t                      |
| file_min_primary            | 8 bytes     | uint64_t, unaligned possible |
| file_max_primary            | 8 bytes     | uint64_t, unaligned possible |
| rowgroup_valid              | 1 byte      | uint8_t, repeated per rg     |
| rowgroup_kind               | 1 byte      | uint8_t, repeated per rg     |
| rowgroup_min_primary        | 8 bytes     | uint64_t, repeated per rg    |
| rowgroup_max_primary        | 8 bytes     | uint64_t, repeated per rg    |
+-----------------------------+-------------+------------------------------+

structured_file_summary_bytes = 1 + 1 + 8 + 8 = 18
structured_rowgroup_summary_bytes = 1 + 1 + 8 + 8 = 18
structured_entry_bytes = 4 + 4 + 18 + rowgroup_count * 18
```

Structured entry 内部偏移示例：

```text
+-----------------------------+----------------+---------------------------+
| Field                       | Entry offset   | Aligned?                  |
+-----------------------------+----------------+---------------------------+
| storage_key                 | 0              | entry-local 4-byte aligned|
| rowgroup_count              | 4              | 4-byte aligned            |
| file_valid                  | 8              | byte field                |
| file_kind                   | 9              | byte field                |
| file_min_primary            | 10             | not 8-byte aligned        |
| file_max_primary            | 18             | not 8-byte aligned        |
| rowgroup0_valid             | 26             | byte field                |
| rowgroup0_kind              | 27             | byte field                |
| rowgroup0_min_primary       | 28             | 4-byte aligned, not 8     |
| rowgroup0_max_primary       | 36             | 4-byte aligned, not 8     |
+-----------------------------+----------------+---------------------------+
```

Hash section：

```text
+-----------------------------+-------------+------------------------------+
| Field                       | Size        | Notes                        |
+-----------------------------+-------------+------------------------------+
| hash_count                  | 4 bytes     | int32_t                      |
+-----------------------------+-------------+------------------------------+

Repeated HashColumnEntry:

+-----------------------------+-------------+------------------------------+
| Field                       | Size        | Notes                        |
+-----------------------------+-------------+------------------------------+
| column                      | 4 bytes     | int32_t                      |
| rowgroup_count              | 4 bytes     | int32_t                      |
| bitset_bytes                | 4 bytes     | int32_t                      |
| num_hashes                  | 4 bytes     | uint32_t                     |
| file_bitset                 | bitset_bytes| one bit per hash bucket      |
| bucket_major_bitmaps        | variable    | bucket_count bitmaps         |
+-----------------------------+-------------+------------------------------+

hash_entry_fixed_bytes = 4 + 4 + 4 + 4 = 16
bucket_count = bitset_bytes * 8
rowgroup_bitmap_bytes = ceil(rowgroup_count / 8)
bucket_major_bytes = bucket_count * rowgroup_bitmap_bytes
hash_entry_bytes = 16 + bitset_bytes + bucket_major_bytes
```

Hash entry 的 offset 关系：

```text
+-----------------------------+--------------------------------------------+
| Region                      | Offset inside HashColumnEntry              |
+-----------------------------+--------------------------------------------+
| fixed header                | 0                                          |
| file_bitset                 | 16                                         |
| bucket_major_bitmaps        | 16 + bitset_bytes                          |
| bucket K bitmap             | 16 + bitset_bytes                          |
|                             |   + K * rowgroup_bitmap_bytes              |
+-----------------------------+--------------------------------------------+
```

磁盘总大小可按 section 累加估算：

```text
total_bytes =
  4
  + 12
  + 4 * num_all_columns
  + 4
  + sum(structured_entry_bytes)
  + 4
  + sum(hash_entry_bytes)
```

### C struct 紧凑布局示意

下面的 C struct 只是为了表达磁盘布局，方便按 offset 解析；当前代码并没有直接把这些 struct `write()` 到磁盘，而是逐字段 `WritePod`。因此这里统一使用 `__attribute__((packed))` 避免编译器 padding，并用 `char[]` 表达变长区域。

```c
#include <stdint.h>

#define BFI3_MAGIC_SIZE 4

typedef struct __attribute__((packed)) {
  char magic[BFI3_MAGIC_SIZE];  // "BFI3", 4 bytes

  // Followed by BFI3HeaderPayload.
  // Total fixed prefix before column_indexes is 4 + 12 bytes.
} BFI3FilePrefix;

typedef struct __attribute__((packed)) {
  int32_t valid_start_row_group_index;  // 4 bytes
  int32_t num_row_groups;               // 4 bytes
  int32_t num_all_columns;              // 4 bytes

  // Followed by:
  //   int32_t column_indexes[num_all_columns];
  // Size = 12 + 4 * num_all_columns bytes.
} BFI3HeaderPayload;
```

Structured section 的紧凑表达：

```c
typedef struct __attribute__((packed)) {
  uint8_t valid;        // 1 byte
  uint8_t kind;         // 1 byte
  uint64_t min_primary; // 8 bytes, may be unaligned on disk
  uint64_t max_primary; // 8 bytes, may be unaligned on disk
} BFI3StructuredSummaryDisk; // 18 bytes

typedef struct __attribute__((packed)) {
  int32_t storage_key;     // 4 bytes
  int32_t rowgroup_count;  // 4 bytes

  BFI3StructuredSummaryDisk file_summary; // 18 bytes

  // Followed by:
  //   BFI3StructuredSummaryDisk rowgroup_summaries[rowgroup_count];
  // Size = 4 + 4 + 18 + 18 * rowgroup_count bytes.
} BFI3StructuredColumnEntryDisk;

typedef struct __attribute__((packed)) {
  int32_t structured_count; // 4 bytes

  // Followed by repeated variable-size BFI3StructuredColumnEntryDisk:
  //   char structured_entries[];
  // Each entry must be parsed using its rowgroup_count.
} BFI3StructuredSectionDisk;
```

Hash section 的紧凑表达：

```c
typedef struct __attribute__((packed)) {
  int32_t column;         // 4 bytes
  int32_t rowgroup_count; // 4 bytes
  int32_t bitset_bytes;   // 4 bytes
  uint32_t num_hashes;    // 4 bytes

  // Followed by:
  //   char file_bitset[bitset_bytes];
  //   char bucket_major_bitmaps[bucket_major_bytes];
  //
  // where:
  //   bucket_count = bitset_bytes * 8
  //   rowgroup_bitmap_bytes = (rowgroup_count + 7) / 8
  //   bucket_major_bytes = bucket_count * rowgroup_bitmap_bytes
  //
  // Size = 16 + bitset_bytes + bucket_major_bytes bytes.
} BFI3HashColumnEntryDisk;

typedef struct __attribute__((packed)) {
  int32_t hash_count; // 4 bytes

  // Followed by repeated variable-size BFI3HashColumnEntryDisk:
  //   char hash_entries[];
  // Each entry must be parsed using bitset_bytes and rowgroup_count.
} BFI3HashSectionDisk;
```

整体文件可以按下面的伪 struct 顺序理解：

```c
typedef struct __attribute__((packed)) {
  BFI3FilePrefix prefix;        // magic[4]
  BFI3HeaderPayload header;     // 12 bytes
  char column_indexes[];        // int32_t[num_all_columns]

  // Then:
  //   BFI3StructuredSectionDisk structured_section;
  //   BFI3HashSectionDisk hash_section;
  //
  // Because column_indexes, structured entries, and hash entries are all
  // variable-size, this top-level type is descriptive rather than directly
  // instantiable as one C object.
} BFI3DiskLayoutSketch;
```

读侧大文件不会把整个 hash section materialize 到 `hash_rowgroup_bitsets`。`BlockFilterIndex::Read` 对 `BFI3` 的大文件只记录：

```text
+--------------------------------------------------------------------------+
| hash_bucket_major_indexes_[column]                                       |
+--------------------------------------------------------------------------+
| rowgroup_count                                                           |
| bucket_count                                                             |
| num_hashes                                                               |
| rowgroup_bitmap_bytes                                                    |
| file_bitset_bytes                                                        |
| file_bitset_offset                                                       |
| bucket_major_offset                                                      |
+--------------------------------------------------------------------------+
```

因此读侧查询时可以按需 seek，不需要把大 sidecar 全部读入内存：

```text
+-------------------+      +----------------------------+
| query literal     | ---> | HashBucketsForLiteral(...) |
+-------------------+      +----------------------------+
                                      |
                                      v
              +------------------------------------------------+
              | file-level bucket read                         |
              | offset = file_bitset_offset + bucket / 8       |
              +------------------------------------------------+
                                      |
                                      v
              +------------------------------------------------+
              | rowgroup-level bucket read                     |
              | offset = bucket_major_offset                   |
              |        + bucket * rowgroup_bitmap_bytes        |
              | read rowgroup_bitmap_bytes                     |
              +------------------------------------------------+
```

### 内存索引结构

`BlockFilterIndex::Data` 是写侧和小文件读侧的主要内存结构，hash rowgroup summary 在内存里是 rowgroup-major，写出时再转成 bucket-major。

```text
+--------------------------------------------------------------------------+
| BlockFilterIndex                                                         |
+--------------------------------------------------------------------------+
| data_: Data                                                              |
| sidecar_buffer_: string                                                  |
| hash_bucket_major_indexes_: map<column, BucketMajorHashIndex>            |
+--------------------------------------------------------------------------+
                                      |
                                      v
+--------------------------------------------------------------------------+
| Data                                                                     |
+--------------------------------------------------------------------------+
| header: Header                                                           |
| structured_file_summaries: map<storage_key, StructuredSummary>           |
| structured_rowgroup_summaries: map<storage_key, vector<StructuredSummary>>|
| hash_file_summaries: map<column, HashSummary>                            |
| hash_rowgroup_bitsets: map<column, vector<string>>                       |
+--------------------------------------------------------------------------+
                                      |
                                      v
        +------------------------------------------------------------------+
        | hash_rowgroup_bitsets[column]                                    |
        | vector index = rowgroup id                                       |
        +------------------------------------------------------------------+
        | rg0 -> bitset[bucket_count / 8]                                  |
        | rg1 -> bitset[bucket_count / 8]                                  |
        | rg2 -> bitset[bucket_count / 8]                                  |
        | ...                                                              |
        +------------------------------------------------------------------+
                                      |
                                      | OR all rowgroup bitsets
                                      v
        +------------------------------------------------------------------+
        | hash_file_summaries[column]                                      |
        | bucket_count | num_hashes | bitset = OR(rg0, rg1, rg2, ...)      |
        +------------------------------------------------------------------+
```

这形成了当前最核心的布局转换：

```text
+-----------------------------------+       +--------------------------------+
| memory: rowgroup-major            |       | sidecar: bucket-major          |
+-----------------------------------+       +--------------------------------+
| hash_rowgroup_bitsets[column]     |       | bucket_major_bitmaps           |
|                                   |       |                                |
|           b0 b1 b2 ... bN         |       | bucket0 -> bitmap(rg0..rgN)    |
| rg0       0  1  0      0          |       | bucket1 -> bitmap(rg0..rgN)    |
| rg1       0  0  1      0          |       | bucket2 -> bitmap(rg0..rgN)    |
| rg2       1  0  1      0          |       | ...                            |
+-----------------------------------+       +--------------------------------+
                  |                                      ^
                  | BlockFilterIndex::Write              |
                  | WriteHashBucketMajor                 |
                  +--------------------------------------+
```

## 新增 row group 时是否全量构建

结论：**新增 row group 的 summary 构建不是全量的，但 flush 写出阶段仍然是全量重写/全量转置模式。**

新增 row group 的热路径如下：

```text
+-------------------------------+
| WritableBlockImpl::FlushSome  |
+-------------------------------+
                |
                v
+-------------------------------+      +----------------------------------+
| file_writer_->NewRowGroup     | ---> | block_filter_index_->StartNew... |
+-------------------------------+      | num_row_groups += 1              |
                                       +----------------------------------+
                                                     |
                                                     v
                       +-----------------------------------------------+
                       | for each block-filter indexed column          |
                       +-----------------------------------------------+
                                                     |
                                                     v
                       +-----------------------------------------------+
                       | UpdateBlockFilterIndexZeroCopy                |
                       | passes current row group's chunks only        |
                       +-----------------------------------------------+
                                                     |
                                                     v
                       +-----------------------------------------------+
                       | BlockFilterIndex::UpdateSummariesForEach      |
                       +-----------------------------------------------+
                       | scan current row group values                 |
                       | build current rg structured min/max           |
                       | build current rg hash bitset                  |
                       | rowgroups[num_row_groups - 1] = rg bitset     |
                       | file_summary.bitset |= rg bitset              |
                       +-----------------------------------------------+
```

也就是说，`UpdateSummariesForEach` 不会重新扫描历史 row group 的原始数据；它只处理刚新增的 row group 对应的 `chunks`。

但周期 flush 的写出路径是：

```text
+------------------------------------------------+
| WritableBlockImpl::FlushBlockFilterIndex       |
+------------------------------------------------+
                        |
                        v
              +--------------------------+
              | block_filter_summary     |
              | exists on disk?          |
              +--------------------------+
                 | yes              | no
                 v                  v
        +------------------+    +------------------+
        | Read existing    |    | no prefix to     |
        | BFI3 sidecar     |    | merge            |
        +------------------+    +------------------+
                 |                  |
                 v                  |
        +------------------+        |
        | MergePrefixFrom  |        |
        | existing index   |        |
        +------------------+        |
                 |                  |
                 +--------+---------+
                          |
                          v
        +------------------------------------------+
        | BlockFilterIndex::Write(.swp, data)      |
        +------------------------------------------+
        | write BFI3 header                        |
        | write all structured rowgroup summaries   |
        | write hash file_bitset                    |
        | WriteHashBucketMajor(rowgroup_bitsets)    |
        +------------------------------------------+
                          |
                          v
        +------------------------------------------+
        | full transpose scan                      |
        | for bucket in [0, bucket_count):         |
        |   for rg in [0, rowgroup_bitsets.size()):|
        |     test rowgroup_bitsets[rg][bucket]    |
        |     set bucket_bitmap[rg]                |
        +------------------------------------------+
                          |
                          v
        +------------------------------------------+
        | rename block_filter_summary.swp          |
        |     -> block_filter_summary              |
        +------------------------------------------+
```

因此当前行为可以精确区分为：

```text
新增 row group:
  原始数据扫描范围 = 当前 row group
  summary 更新范围 = 当前 row group + file-level OR

flush block_filter_summary:
  文件写出范围 = 当前内存 Data 中的所有 indexed rowgroup slots
  hash 转置扫描范围 = all buckets * all rowgroup_bitsets
  sidecar 文件模式 = 写 .swp 后 rename，等价于重写整个 BFI3 sidecar
```

还有一个重要细节：`BFI3` 大文件读侧是 offset-only/on-demand，历史 bucket-major 数据通常不 materialize 到 `hash_rowgroup_bitsets`。所以 `MergePrefixFrom` 对 hash rowgroup prefix 只能复制已经 materialized 的 rowgroup-major bitset；对大 BFI3，它更多只能拿到 file-level/offset 信息，不能直接复用历史 bucket-major prefix 来追加当前 row group。这也是“新增 row group 不全量扫描原始数据，但 flush 仍被全量转置/重写放大”的根因。

## 查询流程与命中 demo

### 查询流程图

`CandidateRowGroupsByHashSummary` 是 rowgroup-level hash pruning 的核心查询路径。它优先使用 `hash_bucket_major_indexes_` 走 BFI3 on-demand bucket-major 读取；如果没有 on-demand index，才回退到内存里的 `hash_rowgroup_bitsets` 逐 rowgroup 判断。

```text
+-------------------------------+
| query literal + column        |
+-------------------------------+
                |
                v
+-------------------------------+
| hash_file_summaries[column]?  |
+-------------------------------+
       | yes                             | no
       v                                 v
+-------------------------------+   +-------------------------------+
| HashBucketsForLiteral         |   | return nullopt                |
| -> bucket list                |   | no usable hash summary        |
+-------------------------------+   +-------------------------------+
                |
                v
+-------------------------------+
| file-level pruning            |
| any bucket bit == 0 ?         |
+-------------------------------+
       | yes                             | no
       v                                 v
+-------------------------------+   +-------------------------------+
| return definite miss          |   | read rowgroup bucket bitmaps  |
| no candidate row groups       |   | one bitmap per query bucket   |
+-------------------------------+   +-------------------------------+
                                                |
                                                v
                                  +-------------------------------+
                                  | AND bitmaps for all buckets   |
                                  | matches[rg] &= hit(bucket,rg) |
                                  +-------------------------------+
                                                |
                                                v
                                  +-------------------------------+
                                  | return candidate rowgroups    |
                                  +-------------------------------+
```

### Demo：单 hash 命中与非命中

假设一个 block 有 3 个 row group，hash bucket 数为 8，`num_hashes = 1`，所以每个 rowgroup bitset 只有 1 byte。为了演示，设定当前列的 rowgroup-major 内存状态如下：

```text
+----------------------+----+----+----+----+----+----+----+----+
| rowgroup bitsets     | b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7 |
+----------------------+----+----+----+----+----+----+----+----+
| rg0                  | 0  | 1  | 0  | 0  | 0  | 0  | 0  | 0  |
| rg1                  | 0  | 0  | 0  | 0  | 0  | 1  | 0  | 0  |
| rg2                  | 0  | 1  | 0  | 0  | 0  | 1  | 0  | 0  |
+----------------------+----+----+----+----+----+----+----+----+
```

写出到 BFI3 时会转成 bucket-major：

```text
+----------------------+------------------------------+
| sidecar bucket-major  | rowgroup bitmap              |
+----------------------+------------------------------+
| bucket0              | 000                           |
| bucket1              | 101                           |
| bucket2              | 000                           |
| bucket3              | 000                           |
| bucket4              | 000                           |
| bucket5              | 011                           |
| bucket6              | 000                           |
| bucket7              | 000                           |
+----------------------+------------------------------+

bit order: bitmap bit0 = rg0, bit1 = rg1, bit2 = rg2
file_bitset = OR(rg0, rg1, rg2) = buckets {1, 5}
```

命中 case：查询 literal `A`，假设 `HashBucketsForLiteral(A) -> bucket1`。

```text
+-------------------------------+
| query A                       |
| bucket = 1                    |
+-------------------------------+
                |
                v
+-------------------------------+
| file_bitset bucket1 = 1       |
| file-level cannot prune       |
+-------------------------------+
                |
                v
+-------------------------------+
| ReadBucketMajorBitmap(1)      |
| returns 101                   |
+-------------------------------+
                |
                v
+-------------------------------+
| candidate row groups          |
| rg0 = 1, rg1 = 0, rg2 = 1     |
| scan rg0 and rg2              |
+-------------------------------+
```

非命中 case：查询 literal `B`，假设 `HashBucketsForLiteral(B) -> bucket7`。

```text
+-------------------------------+
| query B                       |
| bucket = 7                    |
+-------------------------------+
                |
                v
+-------------------------------+
| file_bitset bucket7 = 0       |
| file-level definite miss      |
+-------------------------------+
                |
                v
+-------------------------------+
| no row group can match        |
| skip whole block/column path  |
+-------------------------------+
```

如果 `num_hashes > 1`，查询会得到多个 bucket。file-level 阶段任意一个 bucket 为 0 就可以直接判定非命中；rowgroup-level 阶段则需要把多个 bucket 的 rowgroup bitmap 做 AND。由于 hash summary 是 Bloom-style，命中只表示“可能存在”，仍可能是假阳性；非命中是确定可剪枝。

## 长周期 eBPF 证据

### 1. 90s profile + uprobe

采样命令保留了 profile、`BlockFilterIndex::Write` 延迟、`FlushBlockFilterIndex` 次数：

```bash
PID=2098966
BIN=$(readlink /proc/$PID/exe)
cd /proc/$PID/cwd
bpftrace -p $PID -e '
profile:hz:99 /pid == '$PID'/ {
  @samples[ustack(perf, 12)] = count();
  @bytid[tid, comm] = count();
}
uprobe:'"$BIN"':_ZN8fringedb6detail16BlockFilterIndex5WriteERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNS1_4DataE /pid == '$PID'/ {
  @bfi_write_count = count();
  @ts[tid] = nsecs;
}
uretprobe:'"$BIN"':_ZN8fringedb6detail16BlockFilterIndex5WriteERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNS1_4DataE /@ts[tid]/ {
  @bfi_write_lat_us = hist((nsecs - @ts[tid]) / 1000);
  @bfi_write_done = count();
  delete(@ts[tid]);
}
uprobe:'"$BIN"':_ZN8fringedb6detail17WritableBlockImpl21FlushBlockFilterIndexEv /pid == '$PID'/ {
  @flush_bfi_count = count();
}
interval:s:90 {
  print(@bytid, 25);
  print(@samples, 12);
  print(@bfi_write_count);
  print(@bfi_write_done);
  print(@flush_bfi_count);
  print(@bfi_write_lat_us);
  exit();
}'
```

关键结果：

```text
@bfi_write_count: 65
@bfi_write_done: 58
@flush_bfi_count: 65

@bfi_write_lat_us:
[64K, 128K)    9
[128K, 256K)  16
[512K, 1M)     2
[1M, 2M)      21
[2M, 4M)       1
[16M, 32M)     2
[32M, 64M)     4
```

90s 内有 65 次 `FlushBlockFilterIndex`，其中已结束的 58 次里有 6 次超过 16s，4 次达到 32~64s。采样结束时还有 7 次 `BlockFilterIndex::Write` 未返回，说明长尾会跨越采样窗口。

热点线程集中在同一组写入 worker：

```text
@bytid[2104043, 90e10c19/w-2]: 4274
@bytid[2104050, 90e10c19/w-9]: 3789
@bytid[2104041, 90e10c19/w-0]: 3663
@bytid[2104052, 90e10c19/w-11]: 3613
@bytid[2104045, 90e10c19/w-4]: 3527
@bytid[2104048, 90e10c19/w-7]: 3485
```

热点用户态栈仍然稳定落在 `BlockFilterIndex::Write -> FlushBlockFilterIndex -> IFlushFiles -> Tick`：

```text
fringedb::detail::BlockFilterIndex::Write(...)+2531
fringedb::detail::WritableBlockImpl::FlushBlockFilterIndex()+475
fringedb::detail::WritableBlockImpl::IFlushFiles(bool)+220
fringedb::detail::WritableBlockImpl::FlushFiles(bool)+38
fringedb::detail::WritableBlockImpl::Tick()+903
asio::detail::scheduler::run(std::error_code&)+1363
```

### 2. 90s IO 与线程 CPU 增量

同窗口内 `/proc/2098966/io` 与线程 CPU 增量：

```text
duration: 90.0s
wchar:       +18966323614 bytes = 200.97 MiB/s
write_bytes: +18941448192 bytes = 200.71 MiB/s
syscw:       +63497 = 705.5/s
rchar:       +820134111 bytes = 8.69 MiB/s
read_bytes:  +0 bytes = 0.00 MiB/s
syscr:       +209280 = 2325.3/s
```

Top CPU 线程：

```text
2104045 90e10c19/w-4   62.5% 56.22s/90.0s
2104050 90e10c19/w-9   61.1% 55.03s/90.0s
2104043 90e10c19/w-2   46.1% 41.48s/90.0s
2104041 90e10c19/w-0   43.1% 38.77s/90.0s
2104046 90e10c19/w-5   40.1% 36.12s/90.0s
2104042 90e10c19/w-1   32.6% 29.38s/90.0s
2104048 90e10c19/w-7   26.6% 23.98s/90.0s
```

写吞吐约 200MiB/s，但热点栈在用户态 bitset 转置而不是内核写路径；因此优化重点应放在写侧数据布局和转置算法。

### 3. 60s Read/MergePrefixFrom 采样

补充采样 `BlockFilterIndex::Read` 与 `MergePrefixFrom`：

```text
@read_count: 34
@read_lat_us:
[16, 32) 14
[32, 64) 20

@merge_count: 34
@merge_lat_us:
[2, 4) 28
[4, 8) 6
```

这说明当前 `Read/MergePrefixFrom` 本身不是 CPU 热点。它们很快，是因为 `BFI3` 对大 sidecar 采用 offset-only/on-demand 的 cache 策略：大文件读取时只解析 header/offset，跳过 file bitset 与 bucket-major 大段数据。这个策略利好读侧扫描，但写侧如果需要全量重写，就没有可复用的 materialized prefix，只能在 `WriteHashBucketMajor` 里重新组织输出。

## Cache 视角分析

### 读侧 cache 是对的，但没有转化为写侧复用

`BlockFilterIndex::Read` 对 `BFI3` 的策略是：

1. 小于等于 `kFullReadThresholdBytes = 256KiB` 的 sidecar 读入 `sidecar_buffer_`。
2. 大文件只记录 `file_bitset_offset` 与 `bucket_major_offset`，跳过大段 bucket-major 数据。
3. 查询时通过 `ReadFileHashBucket` / `ReadBucketMajorBitmap` 按 bucket 读取。

这对读侧是合理的：扫描 query 通常只访问少量 literal 对应的 bucket，不应该把整个大索引读入内存。

但写侧 flush 需要生成完整 sidecar。`FlushBlockFilterIndex` 每次都读取已有 summary 并 `MergePrefixFrom`，随后调用 `BlockFilterIndex::Write` 重写 swap 文件。对于大 `BFI3`，已有 bucket-major prefix 并没有进入 `hash_rowgroup_bitsets`，`MergePrefixFrom` 对 hash rowgroup 只能复制已 materialize 的 rowgroup-major bitset。写侧无法直接复用已落盘的 bucket-major prefix，导致 CPU 成本仍然集中在全量转置。

### 现有 file-level bitset cache 不覆盖 rowgroup bucket-major

写入更新时，`UpdateSummariesForEach` 会维护 file-level hash bitset：

- 每个 row group 构造一个 `rowgroup_bitset`。
- `file_summary.bitset` 对该 rowgroup bitset 做 OR。

这个 cache 可以让 file-level 判断快速完成，但并不能直接写出 rowgroup-level bucket-major 数据。最终 `BlockFilterIndex::Write` 仍要把 `hash_rowgroup_bitsets` 从 rowgroup-major 转为 bucket-major。

### 空 prefix / lazy prefix 会放大写侧扫描

`UpdateSummariesForEach` 按 `rowgroup_index = num_row_groups - 1` 写入当前 rowgroup，必要时会 resize `hash_rowgroup_bitsets[column]`。当一个 block 已有大量历史 rowgroup，而历史 BFI3 以 on-demand 形式存在时，内存中可能只有当前追加 rowgroup 的 bitset，历史 prefix 是空 entry 或仅以 sidecar offset 存在。

当前 `WriteHashBucketMajor` 不区分“空 prefix”和“真实空 bitset”，它按 bucket 遍历所有 rowgroup，并对每个 rowgroup 做：

```cpp
const auto& bitset = rowgroup_bitsets[rg];
if (bucket < bitset.size() * 8U && ...)
```

如果 prefix 中存在大量空 entry，这个判断会在每个 bucket 上重复执行，CPU 复杂度仍是 `O(bucket_count * rowgroup_count)`。这也是 cache 策略和写入模式错配的核心风险。

## 写入模式视角分析

当前写入模式：

```text
每个 row group：UpdateSummariesForEach 维护 rowgroup-major bitset
每次 FlushBlockFilterIndex：Read existing -> MergePrefixFrom -> Write full sidecar
Write full sidecar：file bitset 顺序写 + rowgroup-major 全量转置成 bucket-major
```

`WriteHashBucketMajor` 的输出格式对读侧友好：按 bucket 读取一个 bitmap，就能得到所有 rowgroup 的命中情况。但该格式对追加写不友好：新增一个 rowgroup 会改变每个 bucket 对应 bitmap 的最后一位；当 rowgroup 数跨 8 的倍数时，`rowgroup_bitmap_bytes` 还会增长，理论上每个 bucket 的记录宽度都变化。也就是说，当前单文件 bucket-major 布局天然倾向全量重写。

因此，单纯把 `ofstream` buffer 调大或减少 syscall，不能解决主要 CPU。真正的优化要避免“每次 flush 都重做完整矩阵转置”。

## 优化建议

### P0：增加规模指标与保护阈值

先补观测，降低回归风险：

1. 在 `BlockFilterIndex::Write` 记录每列：`rowgroup_count`、`non_empty_rowgroup_count`、`bitset_bytes`、`bucket_count`、`rowgroup_bitmap_bytes`、估算转置工作量 `bucket_count * rowgroup_count`。
2. 在 `FlushBlockFilterIndex` 记录 `Read`、`MergePrefixFrom`、`Write` 分段耗时。
3. 增加保护阈值：当估算工作量超过阈值时打印 warning，必要时临时降级为只写 file-level summary 或延后 rowgroup-level bucket-major。

预期收益：快速定位高 CPU 表/列/参数，避免“只看到进程 CPU 高”。

### P1：稀疏转置，跳过空 prefix 和空 rowgroup

低风险改造 `WriteHashBucketMajor`：

1. 预先收集 `non_empty_rowgroups`，避免内层循环扫描空 string。
2. 对每个非空 rowgroup 迭代 set bits，将 `(bucket -> rowgroup)` 写入临时 bucket bitmap。
3. 如果 bitset 很稀疏，复杂度从 `O(bucket_count * rowgroup_count)` 降为接近 `O(total_set_bits + 输出大小)`。

注意事项：

- 需要保持 BFI3 文件格式不变。
- 若一次性维护所有 bucket bitmap，内存约为 `bucket_count * rowgroup_bitmap_bytes`，等于输出 bucket-major 区域大小；需要分 chunk 或按 bucket range 分批处理。
- 对非常稠密 bitset，可保留当前扫描作为 fallback，避免稀疏算法退化。

预期收益：对“历史 prefix 大量空 entry”或“每个 rowgroup 只设置少量 bucket”的场景，CPU 会明显下降。

### P1：写侧 bucket-major cache

在 `BlockFilterIndex` 内部维护写侧增量 cache：

```text
UpdateSummariesForEach 已经计算出当前 rowgroup 的 buckets
同时更新 bucket-major builder/cache
Flush 时直接顺序写 bucket-major cache，不再全量扫描 rowgroup_bitsets
```

优点：

- 利用写入时已经计算出的 hash bucket，避免 flush 阶段重复转置。
- CPU 从 flush 尾延迟转移到 rowgroup 更新阶段，且每条记录只处理实际命中的 bucket。

代价：

- 内存占用会上升，约等于 bucket-major 输出大小。
- rowgroup 数增加导致 `rowgroup_bitmap_bytes` 扩容时，需要对 cache 做分段或重排。

更稳的实现是按固定 rowgroup segment（如 64/128 个 rowgroup）维护 bucket-major cache，避免全局 bitmap 宽度频繁变化。

### P2：分段增量 sidecar，避免全量重写

从文件格式上解决追加写不友好问题：

```text
BFI3 当前：一个 column 一个 bucket-major 矩阵，flush 重写全量
建议：一个 column 多个 rowgroup segment，每段独立 bucket-major
```

每次 flush 只为新增 rowgroup 段写一个 segment，metadata 记录 segment 的 rowgroup range、bucket_count、rowgroup_bitmap_bytes、offset。读侧查询一个 bucket 时，按 segment 顺序读取对应 bitmap 并拼接/合并。

优点：

- 写入从 full rewrite 变成 append/replace small segment。
- cache 与读侧 on-demand 模型一致：读侧按 bucket、按 segment 读取。
- 避免 `rowgroup_bitmap_bytes` 随全局 rowgroup 增长导致的全文件重排。

代价：

- 文件格式需要升级，例如 `BFI4`。
- 读侧需要处理多 segment。
- 需要兼容 `BFI1/BFI2/BFI3`。

### P2：延后 rowgroup-level bucket-major 到 close/compaction

如果 rowgroup-level pruning 不是写入期间强依赖，可以在周期 flush 只写 file-level summary，把 rowgroup-level bucket-major 延后到 block close 或后台 compaction。

优点：

- 直接降低热写路径 CPU。
- 周期 flush 不再承担大转置。

代价：

- block 未 close 前 rowgroup-level pruning 可能不可用或退化。
- 需要确保读侧能识别“只有 file-level summary”的中间态。

### P3：调参降低触发频率

当前默认：`row_group_flush_every = 10`、`row_group_flush_interval = 30s`。如果业务可接受更大的 flush 粒度，可适当提高 `row_group_flush_every` 或 `row_group_flush_interval`，减少 `FlushBlockFilterIndex` 次数。

该方案只能降低触发频率，不能降低单次 full transpose 成本，因此应作为止血手段而不是根治。

## 推荐落地顺序

1. **先补指标**：记录 `BlockFilterIndex::Write` 输入规模与分段耗时，确认高 CPU 与特定列/表/rowgroup/bucket 参数的关系。
2. **做稀疏转置优化**：跳过空 rowgroup，并按 set-bit 迭代减少 `bucket_count * rowgroup_count` 全扫描。
3. **引入写侧 segment cache**：把转置从 flush 时全量计算改为 rowgroup 更新时增量维护。
4. **设计 BFI4 分段 sidecar**：从格式上避免周期 flush 全量重写。

## 验证标准

优化后用同样 90s eBPF 采样验证：

- `@bfi_write_lat_us` 不应再出现 `[16M, 64M)` 长尾。
- `BlockFilterIndex::Write` 栈在 profile 中占比明显下降。
- `@flush_bfi_count` 可以保持不变，但单次耗时应下降。
- 写吞吐不下降，`write_bytes` 可保持同量级。
- `block_filter_index_pruned_*` 读侧指标不回退。

## 小结

这次问题本质是读写布局目标冲突：bucket-major 非常适合读侧按 bucket on-demand pruning，但对持续追加写非常不友好。当前 cache 策略主要服务读侧，写侧每次 flush 仍承担全量转置和全量重写，所以 CPU 高。建议优先用稀疏转置和写侧 cache 降低 CPU，再通过分段 sidecar 从格式层面消除 full rewrite。
