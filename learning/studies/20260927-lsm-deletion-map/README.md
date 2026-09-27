---
doc_id: recallfs-study-lsm-deletion-map-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260927-lsm-deletion-map/demo
  - append-only stores and LSM read paths
depends_on:
  - recallfs-agent-ready-docs-v1
  - recallfs-source-rocksdb-range-tombstone-conversion-20260622
  - learning/studies/20260927-lsm-deletion-map/source.md
supersedes: []
verified_by:
  - DM-RA-1
  - DM-RA-2
  - DM-RA-3
  - DM-RA-4
  - DM-RA-5
  - DM-RA-6
---

# Append-only / LSM Deletion Map 设计

## 1. Decision

**Deletion map 应是带版本的派生加速索引，而不是删除事实本身。** 原始
`PUT`/point tombstone 继续作为 append-only、可恢复的权威历史；只有能看到全部
source、按同一 snapshot 合并出的 iterator 才能把一段连续的“可见删除状态”
总结为 `[start, end)@snapshot`。转换失败必须 fail-open，只损失性能，不能改变
查询结果。

对于 LSM 的有序 key space，map 的主表示是带 sequence 的 half-open range；
对于 row ID 稳定的不可变 segment，主表示通常是
`(segment_id, generation) -> bitmap(row_ordinal)`。这两者解决的是不同坐标系，
不能拿一个无版本全局 bitmap 同时代替。

## 2. Scope

本文回答三个问题：

1. RocksDB 为什么在 application iterator 上做 point-to-range conversion；
2. 如何把该机制抽象成 append-only / LSM deletion map；
3. 如何用一个 C11 模型验证完整视图、MVCC、边界与 fail-open 行为。

源码位于 [`demo/`](demo/)。它是机制实验，不是可嵌入生产的 storage library。

## 3. Non-goals

- 不实现 RocksDB 的 block cache、memtable、SST、WAL 或 compaction。
- 不复现文章中的 `db_bench` 数字，也不从 demo 的 step count 推导性能。
- 不定义通用 byte comparator、prefix extractor 或 user-defined timestamp ABI。
- 不证明真实掉电、并发 iterator、external ingestion 或跨版本磁盘兼容。
- 不把 range summary 当作物理空间回收；回收仍由 compaction/GC 决定。

## 4. Inputs And Outputs

### 4.1 权威输入

LSM 中每个内部记录至少包含：

$$
(user\_key,\ sequence,\ kind,\ value?)
$$

其中 `kind` 是 `PUT` 或 point tombstone。读取 snapshot \(S\) 时，每个 user key
只考虑 `sequence <= S` 的记录，并由最高 sequence 的记录决定该 key 在 \(S\)
的可见状态。

### 4.2 构建输入

deletion-map builder 不应直接吃“某个 level 的 tombstone 列表”。它应接收：

- 在 snapshot \(S\) 上合并全部 mutable memtable、immutable memtable 和 SST
  后的有序 visible-state stream；
- 可见性证明：source 完整、total order、timestamp 完整、I/O 无缺口；
- source/target generation，用于确认写入目标仍是构建时看到的 active
  generation；
- `min_tombstones`，限制短区间产生的额外写入。

### 4.3 输出

每条 key-space summary 是：

$$
Range = [start,\ end)@S
$$

它表达：“对 sequence 不大于 \(S\) 的记录，这个 key 区间在 snapshot \(S\)
已经没有 live value。”它不表达物理数据已被回收。

## 5. Interfaces And Ownership

### 5.1 三层状态

| 层 | 角色 | 是否权威 | 生命周期 |
| --- | --- | --- | --- |
| Point history | `PUT`、point tombstone、sequence | 是 | WAL/LSM 正常持久化与 compaction |
| Key-range map | `[start,end)@sequence` | 否，派生索引 | memtable -> SST -> compaction，可丢失重建 |
| Segment bitmap | `(segment,generation,row_ordinal)` | 取决于产品设计 | COW sidecar + manifest，旧 snapshot 保留旧 generation |

RocksDB 的方案属于第二层。Lance 一类 immutable fragment 的 deletion vector
属于第三层。前者用 comparator key space 和 MVCC sequence，后者用稳定 physical
row offset；坐标系不同。

### 5.2 推荐 API 边界

```text
append(records)
      |
      v
global_snapshot_iter(S, visibility_proof)
      |
      v
observe_deleted_run(min_run)
      |
      v
validate(target_epoch, txn_barrier, ingest_barrier)
      |
      +---- reject ----> metrics.discarded++
      |
      v
append_derived_range([start,end), S)
      |
      v
future_scan merges newer points over range summary
```

所有图中的 `append_derived_range` 都是 best-effort。owner 是当前 active
memtable 或新的 immutable map generation；builder 只提议区间，owner 负责代际、
锁、publication 和重复检查。

### 5.3 读取判定

对 key \(k\) 和 read snapshot \(R\)：

- \(p\)：`sequence <= R` 的最新 point record；
- \(r\)：覆盖 \(k\)、且 `range.sequence <= R` 的最新 range；
- 若 \(r.sequence \ge p.sequence\)，range 删除该 key；
- 若 \(p.sequence > r.sequence\)，由 point record 决定，因而后写 `PUT` 可复活；
- 若 \(R < r.sequence\)，旧 snapshot 完全看不到该 range。

同 sequence 时让 range 覆盖 point entry，是因为 range 是在该 snapshot 的完整
可见状态上证明出来的。生产实现必须与内部 key type ordering 保持一致。

## 6. Invariants

### I1. Canonical history remains

转换只增加冗余 range，不删除原 point tombstone。崩溃丢失 map 后，查询仍可从
权威历史恢复，只是重新承担 point-scan 成本。

### I2. Global visibility before widening

point tombstone 只删除一个 key，range tombstone 会删除区间内所有 key。这个
语义 widening 只有在构建者能证明区间内部无 live key 时才成立。

这里的“连续”不是数值连续。`10, 20, 30` 可以形成一个 run，只要全局
visible-state stream 在它们之间没有 `15`、`25` 等 live key。顺序由数据库
comparator 定义。

### I3. Snapshot stamps the proof

summary 必须使用完成证明时的 snapshot sequence \(S\)，而不是某一个 point
tombstone 的局部 sequence。这样：

- \(R < S\) 的读看不到 summary；
- \(R = S\) 得到与构建时相同的结果；
- sequence \(> S\) 的新写覆盖 summary。

### I4. Half-open boundary is owned

使用 `[first_deleted_key, next_live_key)` 可避免删除终止 run 的 live key。若扫描
在 key space 尾部结束，而实现没有自己拥有、不可变的 exclusive upper bound，
就不能凭空构造 `+infinity`。demo 保守地放弃尾部转换。

RocksDB 当前源码还显示：在 prefix/upper-bound 边界处，不能直接相信某个
memtable iterator 暴露的“下一个 key”，因为 SST iterator 可能已被边界过滤。
实现宁可少覆盖一个 point tombstone，也不能扩大 range。

### I5. Generation and publication are atomic

builder 从 generation \(G\) 读取时，只能向仍处于 mutable 状态的 \(G\) 写入。
memtable switch、manifest change 或 sidecar replacement 发生后应丢弃提案或
重新构建，不能写进 stale owner。

### I6. Newer writes win

range summary 的 sequence 不能高于它所证明的 snapshot。后续 `PUT(k,S+1)`
必须继续可见，不能被旧 map 的“快速跳过”吞掉。

### I7. Incomplete evidence never mutates

以下任一条件出现时都必须拒绝：

- `table_filter` 或 source pruning 隐藏了 SST；
- prefix scan 不能证明 prefix 边界内完整；
- partial timestamp window；
- cache-only/read-tier 返回 incomplete；
- transaction 有无法排除的 uncommitted sequence；
- active memtable 已冻结；
- external ingest 可能引入 sequence 更高但位于 range 下层的文件；
- endpoint 内存不归 owner 所有或可能变化。

## 7. Failure Semantics

| Failure | 行为 | 正确性影响 |
| --- | --- | --- |
| run 小于阈值 | 不生成 range | 无，仅无加速 |
| incomplete view | 返回 `DM_INCOMPLETE_VIEW` | 无，保留 point scan |
| target generation 已变化 | 返回 `DM_STALE_TARGET` | 无，调用方可稍后重试 |
| duplicate covering range | 计 discarded，不重复写 | 无 |
| map 容量/内存分配失败 | 放弃该优化 | 无 |
| map 未写 WAL 且 crash 丢失 | 重启后重新学习 | 无，只损失 warm state |
| canonical tombstone 丢失 | 数据可能复活 | 严重；不属于可容忍的 map failure |
| 错误地从 partial view 扩区间 | live data 被隐藏 | 数据正确性故障 |

“derived 可以不写 WAL”成立的前提是 canonical point history 已持久化且 map 不
参与唯一恢复路径。若系统选择只保留 bitmap 而删除原 tombstone，bitmap 就升级为
权威状态，必须进入 WAL/manifest、checksum、recovery 与兼容合同。

## 8. Worked Examples

### 8.1 错误：只看 L0

```text
L0 @ seq 30:  DEL 10      DEL 20      DEL 30      PUT 40
L1 @ seq 10:  PUT 10  PUT 15  PUT 20  PUT 25  PUT 30  PUT 35  PUT 40
partial idea: [10, 40) @ 30
```

L0 单独看见三个连续 delete，但 snapshot 30 的全局结果是
`[15,25,35,40]`。错误 range 会把结果变成 `[40]`。因此 flush-local 和
compaction-input-local 的连续性都不是安全证明。

### 8.2 正确：全局 snapshot

去掉 L1 中的 `15`、`25`、`35` 后，snapshot 30 的全局 visible states 是：

```text
10=DEL  20=DEL  30=DEL  40=PUT
```

threshold 为 3 时，可生成 `[10,40)@30`。point-only 与 map scan 都只返回
`[40]`。demo 的抽象 iterator step 从 4 变为 2：一次 range 处理加一次 live
entry。它不是 wall-clock benchmark。

### 8.3 旧 snapshot

在 snapshot 20，range 的 sequence 30 尚不可见，旧 `PUT 10/20/30/40`
全部可见。输出为 `[10,20,30,40]`。

### 8.4 新写复活

在 range 创建后追加 `PUT 20@35`。snapshot 35 中 point sequence 35 大于 range
sequence 30，因此输出为 `[20,40]`。一个简单的“key 在 bitmap/range 中就永远
删除”判定会在这里出错。

### 8.5 Append-only segment

若数据文件是 immutable segment，且 compaction 前 row ordinal 稳定，删除可写为：

```text
Manifest V7
  |
  +-- Segment 42 data
  +-- Delete sidecar (segment=42, generation=3, bitmap={7,9,10})
```

更新时读取 generation 3，union 新 offsets，写 generation 4，再原子发布
Manifest V8。snapshot V7 继续引用 generation 3。这里 bitmap 的 key 必须是
`(segment_id,generation,row_ordinal)`，不能只用全局 row number；compact
重排后要生成新 segment 和新 bitmap 坐标。

### 8.6 何时值得生成

若一次转换成本为 \(C_b\)，range entry 的持久化/维护成本为 \(C_w\)，每次后续
scan 对长度为 \(N\) 的 run 节省约 \((N-1)C_s\)，则粗略 break-even 为：

$$
R > \frac{C_b + C_w}{(N-1)C_s}
$$

其中 \(R\) 是该 map 被命中的后续 scan 次数。这解释了为什么阈值应随 run
长度、scan reuse、memtable flush 速度和写放大动态调优，而不是复制一个固定的
`100`。

## 9. Reconciliation Anchors

| Anchor | 输入或条件 | 精确预期 | 验证 |
| --- | --- | --- | --- |
| `DM-RA-1` | 完整 snapshot 30：`DEL 10/20/30`，next live `40`，threshold 3 | 生成唯一 `[10,40)@30`；forward/reverse 输出均与 baseline 相同 | `test_safe_conversion` |
| `DM-RA-2` | L0 deletes，L1 含 live `15/25/35` | partial proof 被拒绝；强行使用 `[10,40)` 时 oracle 明确检测输出从 `[15,25,35,40]` 变为 `[40]` | `test_incomplete_and_stale_views_are_rejected`, `test_partial_level_conversion_is_corrupting` |
| `DM-RA-3` | map 为 `[10,40)@30`，读取 snapshot 0..40 | 每个 snapshot、forward/reverse 都与不读 map 的 point-history oracle 一致 | `test_snapshot_and_newer_write_semantics` |
| `DM-RA-4` | `PUT 20@35` 覆盖旧 range | snapshot 35 输出 `[20,40]` | `test_snapshot_and_newer_write_semantics` |
| `DM-RA-5` | target epoch 8，view epoch 7；或 view 不完整 | 分别返回 `DM_STALE_TARGET`、`DM_INCOMPLETE_VIEW`，map 为空 | `test_incomplete_and_stale_views_are_rejected` |
| `DM-RA-6` | threshold 4；无 owned upper bound 的尾部 run；冲突记录；range 容量耗尽 | 不转换短/尾部 run；冲突报错；容量错误不发布 partial map | `test_threshold_and_unbounded_tail`, `test_conflicting_records_are_rejected`, `test_capacity_failure_publishes_nothing` |

运行：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260927-lsm-deletion-map/demo/include \
  learning/studies/20260927-lsm-deletion-map/demo/src/deletion_map.c \
  learning/studies/20260927-lsm-deletion-map/demo/tests/deletion_map_test.c \
  -o .tmp/deletion-map-test

.tmp/fil-c/bin/filrun .tmp/deletion-map-test
```

预期：

```text
deletion-map correctness passed: 7 suites
```

## 10. Evidence And Unknowns

### 已观察

- 原文与 PR 描述的机制是 read-path、snapshot-aware、best-effort 的冗余 range
  tombstone。
- RocksDB `4052fccd` 的最终源码显式检查完整可见性、mutable memtable、
  snapshot/transaction sequence、重复 range 和 external ingest barrier。
- C demo 在 FIL-C 0.684 与 Apple Clang 21.0.0 下通过七组测试。
- 独立 point-history oracle 覆盖 snapshot 0..40 和双向扫描。

### 上游而未本地复现

- 文章报告 deletion-heavy forward/reverse scan 分别约 99x/368x。
- 文章报告无 tombstone 时启用 bookkeeping 的差异在噪声内。

### 未解决

- 生产实现的 byte-key endpoint ownership 和 comparator ABI。
- 并发 map writer 的去重、重叠 range fragmentation 与 memory accounting。
- WAL-less derived entry 与 memtable flush/manifest publication 的 crash matrix。
- external ingestion、transaction engine、user timestamp 的完整集成测试。
- 目标 workload 上 threshold、read benefit 与 write amplification 的实测
  break-even。

这些未知项意味着本产物只能证明机制与关键安全边界，不能宣称 production ready
或复现 RocksDB 的性能收益。

## 11. 实施清单

若把该机制放进真实系统，按以下顺序做：

1. 先定义 canonical delete history、snapshot 与 comparator，不先选 bitmap。
2. 明确 map 坐标：LSM key range，或 immutable segment 的 stable row ordinal。
3. 给 iterator 输出附带可验证的 completeness capability。
4. 用 snapshot sequence 标记 derived range，并验证 transaction/ingest barrier。
5. 让 owner 检查 generation、复制 endpoint、原子发布并容忍 duplicate。
6. 任何 guard 失败都只增加 `discarded`，不得修改 canonical history。
7. 用 point-history oracle 做 snapshot differential test，再做 crash/concurrency
   fault injection。
8. 最后才在目标机测 read saving、额外写放大、map memory 与 compaction cost，
   据此设 threshold。
