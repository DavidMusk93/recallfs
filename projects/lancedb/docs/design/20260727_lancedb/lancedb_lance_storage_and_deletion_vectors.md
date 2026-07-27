# LanceDB / Lance 文件结构与 Deletion Vector 学习笔记

## 一句话结论

LanceDB 的删除是 **Lance 格式上的按 fragment、按物理行偏移量的 copy-on-write tombstone**：一次局部删除不改写列数据文件，只写一个合并后的 deletion file，并在新版本 manifest 中替换该 fragment 的引用；扫描和索引结果在读取时应用该向量，compact 才会把存活行物化到新数据文件。

## 1. 范围、版本与证据

本文区分两个仓库，不能把数据库 API 层与底层表格式混为一谈。

| 层 | 仓库/版本 | 职责 |
| --- | --- | --- |
| 数据库层 | [`lancedb/lancedb` `ba4558a`](https://github.com/lancedb/lancedb/tree/ba4558a64f23f93ea42b4f78a9889bf0677977a3) | 本地/远程表 API、查询编排、命名空间与 SDK |
| 存储格式层 | [`lance-format/lance` `ddb8e28`](https://github.com/lance-format/lance/tree/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8) | Lance 文件、table manifest、事务、fragment、deletion vector 与执行器 |

证据基线为 2026-07-27 拉取的源码。LanceDB 工作区在 `Cargo.toml` 中将 `lance` 固定为 `v10.0.0-beta.5`，该 tag 解析到 `ddb8e28`；以下格式细节以该版本为准。

相关源码入口：

- [LanceDB `Table::delete`](https://github.com/lancedb/lancedb/blob/ba4558a64f23f93ea42b4f78a9889bf0677977a3/rust/lancedb/src/table.rs#L1158-L1168)
- [LanceDB 删除委托](https://github.com/lancedb/lancedb/blob/ba4558a64f23f93ea42b4f78a9889bf0677977a3/rust/lancedb/src/table/delete.rs#L27-L60)
- [Lance table format specification](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/docs/src/format/table/index.md)
- [Lance 删除写入实现](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/write/delete.rs)

## 2. 总体模型：不可变 snapshot + fragment

Lance 表不是单个文件。它是由不可变版本描述的一组对象构成：

```text
LanceDB Table API
    |
    | Table::delete(predicate)
    v
Lance Dataset (current manifest version V)
    |
    +--> Manifest V
    |      +--> schema / field IDs / configuration
    |      +--> fragments[]
    |      +--> index metadata
    |
    +--> Fragment F
    |      +--> data files: 一或多个 .lance，按列组存放
    |      +--> deletion file: 0 或 1 个，记录 F 内被删除的物理 offset
    |      +--> physical_rows: 包含已删除行的物理总数
    |
    +--> Object store files
           +--> data/
           +--> _deletions/
           +--> _versions/
           +--> _indices/
```

关键不变量：

1. `fragment` 是水平分区；每个 fragment 可有多个数据文件，每个数据文件存一部分列。
2. 删除向量的坐标系是 **fragment 内、从 0 开始、未压缩前的物理行 offset**，不是主键、全局逻辑行号，也不是某个列文件自己的 offset。
3. `physical_rows` 包含 tombstone 行；逻辑可见行数约为 `physical_rows - deleted_count`。
4. 一个 fragment 在一个 manifest snapshot 中至多引用一个 deletion file。连续删除不会在同一 snapshot 形成需要逐个读取的 DV 链，而是读旧向量、并入新 offset、写出新的完整向量并换引用。
5. 旧 manifest 与其所指的旧 deletion file 保留，因而时间旅行和并发读不受新删除影响；清理旧版本才回收它们。

这让删除的写放大从“重写命中行所在的所有列文件”降低为“扫描定位行 + 写很小的 tombstone sidecar + 新 manifest”，代价则转移到后续读过滤和旧文件保留。

## 3. 对象存储布局与元数据组织

典型表根目录如下：

```text
{dataset_root}/
  data/
    *.lance                    # 列式数据文件
  _versions/
    *.manifest                 # 每个 snapshot 一个 manifest
    latest_version_hint.json   # 最新版本发现的可选优化
  _transactions/
    *.txn                      # 提交协调
  _deletions/
    {fragment}-{read_version}-{id}.arrow
    {fragment}-{read_version}-{id}.bin
  _indices/
    {uuid}/...
  _refs/
    tags/*.json
    branches/*.json
```

删除文件命名模式为：

```text
_deletions/{fragment_id}-{read_version}-{random_id}.{arrow|bin}
```

其中 `read_version` 是产生这次变更时所读取的 dataset 版本，`random_id` 避免同一读版本并发写碰撞。数据文件、删除文件和索引元数据均可通过 manifest 的 `base_paths` 与 `base_id` 指向不同对象存储位置，因此归档、跨区域或热冷分层不要求改写 manifest 的逻辑结构。

一个 fragment 在 manifest 中的删除元数据可抽象为：

```text
Fragment {
  id: u64,
  files: [DataFile...],
  physical_rows: Option<usize>,
  deletion_file: Option<DeletionFile>
}

DeletionFile {
  read_version: u64,
  id: u64,
  file_type: Array | Bitmap,
  num_deleted_rows: Option<usize>,
  base_id: Option<u32>
}
```

`num_deleted_rows` 是供统计使用的可选元数据，正确性不能只依赖它；读路径仍以 deletion file 的实际内容构建过滤条件。

参考：

- [布局规范](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/docs/src/format/table/layout.md#L15-L39)
- [`DeletionFile` 定义](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance-table/src/format/fragment.rs#L399-L451)
- [路径生成函数](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance-table/src/io/deletion.rs#L37-L59)

## 4. Deletion Vector 的两级表示

### 4.1 内存表示

实现中的 `DeletionVector` 是三态枚举：

```rust
NoDeletions
Set(HashSet<u32>)
Bitmap(RoaringBitmap)
```

它保存单 fragment 的 `u32` 行 offset。`Set` 适合少量离散删除，`RoaringBitmap` 适合大量或局部连续的删除。当前实现以 5,000 为提升阈值：新增 offset 的数量已知且下界不小于阈值时直接构建 bitmap；未知规模时先建 set，最终元素数超过阈值再提升。该常量的源码注释明确标记为待 benchmark 调优，而非格式兼容性要求。

| 状态 | 适用情况 | 查询/迭代特点 |
| --- | --- | --- |
| `NoDeletions` | fragment 完全存活 | 不产生掩码，快速路径 |
| `Set<HashSet<u32>>` | 稀疏删除 | 单点 membership 快；排序时需要复制并排序 |
| `RoaringBitmap` | 稠密或较大删除集合 | 压缩整数集合；membership、范围基数与升序迭代高效 |

注意：`u32` 限制的是 **单 fragment 的物理 offset 空间**，并不限制表总行数；全局 row address 把 fragment ID 放在高位、local offset 放在低位，构造删除谓词时会显式只取低 32 位。

参考：[内存枚举、阈值与谓词构造](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance-core/src/utils/deletion.rs#L10-L124)。

### 4.2 持久化编码

内存表示直接决定文件格式：

| 内存态 | 文件类型 | 后缀 | 内容 | 适用 |
| --- | --- | --- | --- | --- |
| `Set` | `Array` | `.arrow` | 单列、非空、`UInt32` 的 Arrow IPC 文件，ZSTD 压缩 | 稀疏 offset |
| `Bitmap` | `Bitmap` | `.bin` | Roaring bitmap 的序列化字节 | 稠密/大型集合 |
| `NoDeletions` | 无文件 | 无 | `write_deletion_file` 返回 `None` | 无删除 |

格式规范文字把 `.arrow` 描述为 Int32Array，但 v10.0.0-beta.5 的实际写入代码创建的是 `UInt32Array`，schema 字段名为 `row_id`。排查二进制兼容问题时，应以实现与 protobuf/manifest 元数据为准，不能照抄文档中的该处类型描述。

写入规则：

1. 每个新 deletion file 生成随机 `u64 id`。
2. Array 写为仅一个 record batch 的 Arrow IPC 文件，开启 ZSTD。
3. Bitmap 调用 `RoaringBitmap::serialize_into`。
4. 写对象成功后，返回 `DeletionFile` 元数据；事务提交后，新的 manifest 才会引用它。
5. 读 Array 时实现校验“恰好一个 batch、schema 精确匹配、没有 null”；读 Bitmap 时反序列化 roaring bytes。损坏直接返回错误而不静默忽略 tombstone。

参考：[写入与读取完整实现](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance-table/src/io/deletion.rs#L61-L215)。

## 5. 删除写路径

普通谓词删除的主线如下：

```text
Table::delete(predicate)
  |
  v
LanceDB NativeTable::execute_delete
  |
  v
Lance DeleteBuilder
  |-- 用 scanner 执行 predicate，并投影 _rowid
  |-- 捕获命中行的 row address
  v
按 fragment 分组的 local offsets
  |
  v
FileFragment::extend_deletions
  |-- 读当前 deletion file（如果有）
  |-- old DV UNION new offsets
  |-- 全部物理行都 tombstone ? 移除 fragment : 写新 DV 文件
  v
Operation::Delete { updated_fragments, deleted_fragment_ids, predicate }
  |
  v
CommitBuilder / MVCC rebase
  |
  v
新 manifest version V+1
```

具体行为：

- 谓词优化为 `false` 时不修改任何 fragment。
- 谓词优化为 `true` 时删除所有 fragment，避免逐行扫描和写 DV。
- 局部命中时，命中 row address 的低 32 位成为 fragment-local offset。
- `extend_deletions` 先加载已有 DV 并 union 新 offset，因此重复删除幂等，不会重复计数。
- 若 DV 覆盖 `[0, physical_rows)` 的全部 offset，`write_deletions` 返回 `None`，调用方将 fragment ID 列为从 manifest 移除，而不是保存“全 1”的 deletion file。
- 若向量长度超过物理行数或包含越界 offset，写前报错；这防止损坏 tombstone 让行映射失真。

LanceDB 并不自行实现这种文件级删除。其 native path 将 SQL 字符串谓词交给 `Dataset::delete`，表达式谓词交给 Lance 的 `DeleteBuilder`，然后用返回的新 dataset version 更新本地表状态。

参考：

- [LanceDB 委托层](https://github.com/lancedb/lancedb/blob/ba4558a64f23f93ea42b4f78a9889bf0677977a3/rust/lancedb/src/table/delete.rs#L27-L60)
- [扫描、定位和构造删除事务](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/write/delete.rs#L40-L389)
- [fragment 合并与写 DV](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/fragment.rs#L2025-L2149)

## 6. 读取、take 与索引结果如何避开已删除行

### 6.1 扫描读取

读取 fragment 时，执行器会加载该 fragment 在当前 manifest 指向的 deletion file，读取 data file 后根据物理 row address 生成布尔 predicate：

```text
row offset  : 0 1 2 3 4 5 6
DV          :     X   X
visible     : T T F T F T T
```

批处理函数 `apply_row_id_and_deletes` 将每行的 physical offset 与 DV 做 membership 测试，生成“保留”布尔数组。无删除时 `NoDeletions` 被归一为 `None`，不计算 row address 和 mask，避免零删除开销。

这一点非常重要：数据列文件仍按物理 offset 读取，删除只是 **读取时过滤**，不会让剩余行重新编号。因此 column pages、row address 与大多数索引地址在 delete 后仍可对齐。

### 6.2 按逻辑 offset 的 take

面向用户的逻辑行位置不应落在 tombstone 上。`FileFragment::take` 读取 DV 后，把请求的 live-row ordinal 映射为物理 offset，再执行 `take_rows`。简单实现会排序已删除 offset 后使用二分定位；`OffsetMapper` 还为单调访问保存上一次差值，以减少连续查询的映射成本。

### 6.3 索引搜索

向量/标量/FTS 索引中可能仍保留已删除 row address。预过滤器并行加载有 deletion file 的 fragment，转成 `(fragment_id -> RoaringBitmap)`，再构造 `RowAddrMask` block list 排除这些地址。稳定 row ID 模式还会从 row-id sequence 中掩掉删除位置，形成 allow list，以阻止更新后旧物理地址泄漏或重复返回。

```text
index candidate row addresses
          |
          v
DV-derived RowAddrMask  ----> block deleted offsets / stale fragments
          |
          v
surviving candidates --> fetch / refine --> results
```

所以 deletion vector 既是 table scan 的正确性机制，也是 merge-on-read 索引一致性的必要过滤层；它不是只有垃圾回收意义的元数据。

参考：

- [扫描加载 DV](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/io/exec/filtered_read.rs#L636-L662)
- [批内应用删除掩码](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance-table/src/utils/stream.rs#L227-L350)
- [`take` 的逻辑到物理 offset 映射](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/fragment.rs#L1572-L1608)
- [索引预过滤 DV mask](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/index/prefilter.rs#L95-L275)

## 7. 缓存、MVCC 与并发删除

### 7.1 缓存组织

DV 缓存位于 dataset-scoped metadata cache，key 为：

```text
deletion/{fragment_id}/{read_version}/{id}/{suffix}
```

key 中同时包含读取版本、随机 ID 与格式后缀，因此新的 manifest 引用新 deletion file 时不会错误复用旧 snapshot 的向量。缓存 miss 时按 `base_id` 解析对象存储位置、读取文件、构造 `Arc<DeletionVector>` 并插入缓存。

### 7.2 MVCC rebase

删除事务带有：

- 它读到的 `read_version`
- 更新的 fragment 元数据或彻底删除的 fragment ID
- 原始 predicate
- 尽可能精确的 `affected_rows`

两个并发删除若触及同一 fragment 但删除的是不重叠 physical offsets，可以 rebase：

```text
V10: fragment 42, DV = {2, 7}

writer A: delete {10, 11} -> commits V11
writer B: delete {20}     -> built from V10

rebase B:
  load V11 DV {2, 7, 10, 11}
  intersect with B affected_rows {20} == empty
  write union {2, 7, 10, 11, 20}
  commit V12 with a new deletion-file reference
```

如果交集非空，commit 返回 retryable conflict。若 union 已覆盖整个 fragment，则 rebase 可把该 fragment 转为删除。这个策略能最大化并发删除的可提交性，同时保证同一物理行不会在错误假设下被重复修改。

参考：

- [DV cache key 与命中逻辑](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/session/caches.rs#L101-L120)
- [读取并缓存 dataset deletion file](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/io/deletion.rs#L11-L43)
- [删除向量 union rebase](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/io/commit/conflict_resolver.rs#L1630-L1776)
- [事务 `Operation::Delete`](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/transaction.rs#L313-L337)

## 8. Compact、索引维护与文件回收

删除不是最终物理回收：

```text
delete
  -> 新 manifest + DV（数据文件还在，读时过滤）
  -> compact/rewrite
       -> 只把 live rows 写入新 fragment/data files
       -> 新 fragment 不再引用 deletion file
       -> 原 row addresses 失效，相关 ANN index 需重建或更新
  -> cleanup old versions
       -> 当旧 snapshot 的保留策略允许时，删除旧 data/DV/index files
```

compact 可以移除删除行，降低后续 scan 的 mask 成本，但会改变物理地址。因而需要特别处理依赖物理位置的索引：在重写后，旧 ANN 索引不能继续视为覆盖新 fragment。带 stable row IDs 的场景中，compact 会先对 row-id sequence 应用 DV mask，再把存活逻辑行 ID 重新分块。

实践取舍：

| 情况 | 推荐动作 | 原因 |
| --- | --- | --- |
| 少量、分散删除 | 保持 DV | 写放大低，Array sidecar 很小 |
| 删除比例持续增长 | 计划 compact | scan/filter 与 index mask 开销会累积 |
| 大批量整 fragment 删除 | 直接从 manifest 移除 fragment | 无需写全量 DV |
| 频繁 delete + ANN 查询 | 监控删除比例并安排重写/索引维护 | merge-on-read mask 会增加检索成本 |
| 需要长期 time travel | 延后 cleanup | 旧 manifest 仍引用旧 DV/data files |

参考：

- [compact 会物化删除并使 row address 失效](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/docs/src/guide/read_and_write.md#L434-L467)
- [compact 时掩掉已删除 stable row IDs](https://github.com/lance-format/lance/blob/ddb8e28ca238f29628b8e1795ddccbb7bf75e5c8/rust/lance/src/dataset/optimize.rs#L1845-L1879)

## 9. 与其他删除设计的对照

| 方案 | 写时成本 | 读时成本 | 行地址稳定性 | Lance 的选择 |
| --- | --- | --- | --- | --- |
| 原地删除 | 高，且对象存储不友好 | 低 | 可能改变 | 不采用 |
| 立即重写受影响列文件 | 高写放大 | 低 | 会改变，索引受影响 | 用 compact 延后执行 |
| 每次删除增量 DV 链 | 低 | 随删除次数线性增大 | 稳定 | 不采用；每版本一个合并后 DV |
| 每 fragment 完整 DV + MVCC manifest | 小侧写 | 常量个 DV 加载/过滤 | 稳定到 compact 前 | 当前实现 |

Lance 的平衡点是：将小而频繁的 mutation 与昂贵的物化重写拆开。对对象存储、列式文件和向量索引共存的工作负载，这通常比立即改写数据更合理。

## 10. 学习时应继续追问的问题

1. 5,000 的 Set-to-Roaring 阈值在目标数据分布、对象存储 RTT 和压缩比下是否合理？源码已标明需要 benchmark。
2. 应以“deleted rows / physical rows”、DV 字节数、scan mask 时间、索引候选过滤比例等指标，何时触发 compact？
3. 对 ANN、scalar 与 FTS 各自，DV prefilter 的延迟和召回影响是否一致？
4. stable row ID、update 与 compact 同时存在时，哪些索引可以增量维护，哪些必须 rebuild？
5. 旧版本清理的 retention 与正在运行的长查询/时间旅行 SLA 如何协调？

## 11. 验证记录

- 已以 `git clone --depth 1` 获取并检查上述两个精确 commit。
- 已索引两仓源码，并按 API -> delete writer -> fragment DV -> file IO -> reader/index mask -> transaction rebase -> compact 路径交叉核对。
- 本文是设计学习文档，不修改 LanceDB 或 Lance 源码，也不宣称对上游运行了测试。
