# 《How to Write to SSDs》数据库内核学习笔记

> Bohyun Lee, Tobias Ziegler, Viktor Leis. *How to Write to SSDs*. PVLDB
> 19(7): 1469-1483, 2026. DOI: 10.14778/3801059.3801063.
>
> 原文：[本地 PDF](../pdfs/p1469-lee.pdf) |
> [VLDB](https://www.vldb.org/pvldb/vol19/p1469-lee.pdf) |
> [Artifact](https://github.com/LeeBohyun/ZLeanStore)

本文的 18 个 C 代码块已使用 FIL-C 0.684 编译，并通过 6 组运行时测试。复验：

```bash
rustc tools/docs/verify_markdown_c.rs -O -o .tmp/verify-markdown-c
.tmp/verify-markdown-c \
  --compiler .tmp/fil-c/bin/filcc \
  --runner .tmp/fil-c/bin/filrun \
  --harness tools/docs/tests/how_to_write_to_ssds_harness.c \
  docs/reports/how-to-write-to-ssds-database-engineering-notes.md
```

## 阅读前：名词地图

本文保留论文和代码中常用的英文缩写，便于继续查阅资料；首次阅读时可以先把
整条写路径理解成“数据库生成逻辑页，SSD 固件再把逻辑地址翻译成闪存位置”：

```text
Application update
        |
        v
DB page identified by PID
        |
        | DBMS: WAL, DWB, compression, DB GC
        v
Host write addressed by LBA
        |
        | SSD firmware: FTL, wear leveling, SSD GC
        v
NAND page inside erase block / superblock
```

这里的 `page`、`block` 和 `zone` 不能互换：

| 对象 | 谁能看见 | 直觉解释 |
| --- | --- | --- |
| DB page（数据库页） | DBMS | B-tree、buffer pool 和 WAL 操作的数据单位，常见为 4/8/16 KiB。 |
| LBA block（逻辑块） | OS/DBMS/SSD | 主机提交给块设备的逻辑地址；只是编号，不等于真实闪存位置。 |
| NAND page（闪存页） | SSD 固件 | NAND 能读取或 program（写入）的内部单位，通常不直接暴露给 DBMS。 |
| Erase block（擦除块） | SSD 固件 | 多个 NAND page 的集合；NAND 必须整块擦除，不能只擦其中一页。 |
| Superblock（超级块） | SSD 固件 | 跨 plane/die 组合的一组 erase block，用来并行写入和回收；不是文件系统 superblock。 |
| DB zone（数据库分区） | DBMS | out-of-place 引擎自定义的连续回收单位。 |
| ZNS zone（设备分区） | 主机与 ZNS SSD | 设备公开的顺序写入、整区 reset 单位。 |

### 核心指标

| 名词 | 全称 | 面向软件开发者的直觉 |
| --- | --- | --- |
| WAF | Write Amplification Factor，写放大系数 | “业务本想写 1 字节，某一层实际写了多少字节”的倍率。`1` 最理想，`4` 表示实际写了 4 倍。 |
| DB WAF | Database WAF | DBMS 下发字节 / 原始脏页字节；doublewrite、checkpoint 和 DB GC 都会改变它。 |
| SSD WAF | SSD WAF | NAND 实际写入字节 / 主机下发字节；主要来自 FTL 搬迁和 SSD GC。 |
| Total WAF | End-to-end WAF | NAND 字节 / 原始脏页字节，也等于 `DB WAF * SSD WAF`。它才直接影响寿命和设备写带宽。 |
| User writes | 用户页写入 | 因正常 eviction/checkpoint 必须写出的原始脏页，是论文计算 DB WAF 的基准。这里不是 SQL 文本大小。 |
| Host writes | 主机写入 | DBMS/OS 真正提交给 SSD 的字节，包括额外副本和 DB GC 搬迁。 |
| Flash/NAND writes | 闪存物理写入 | SSD 内部最终 program 到 NAND 的字节；普通 block I/O 统计通常看不到它。 |
| B/op | Bytes per operation | 每完成一次 benchmark 操作，平均产生多少写入字节。 |
| OPS | Operations per second | 每秒完成的操作数；类似吞吐量，但“operation”由 benchmark 定义。 |
| DWPD | Drive Writes Per Day | 保修期内每天允许写满整盘多少次；`1 DWPD` 表示每天可写入约一个设备容量。 |

用论文的 in-place LeanStore 数字代入，一次 4 KiB 用户页写入会变成：

```text
4 KiB user page
   |  DB WAF = 2.00
   v
8 KiB host writes
   |  SSD WAF = 2.36
   v
18.88 KiB NAND writes

Total WAF = 18.88 / 4 = 4.72
```

因此“DBMS 下发 8 KiB”并不是终点；SSD 内部还可能再写 10.88 KiB。

### 数据库写路径

| 名词 | 全称 | 面向软件开发者的直觉 |
| --- | --- | --- |
| DBMS | Database Management System | 数据库管理系统；本文重点指 buffer manager、WAL、space manager、GC 等存储引擎部分。 |
| Buffer pool | 缓冲池 | DBMS 在内存中缓存数据库页的区域。 |
| Dirty page | 脏页 | 内存中已修改、尚未持久化的数据库页。 |
| Eviction | 淘汰 | buffer pool 空间不足时移出页面；若页面是脏的，通常要先写盘。 |
| Checkpoint | 检查点 | 把某个恢复进度及相关脏页/元数据持久化，缩短崩溃后的 WAL 重放范围。 |
| WAL | Write-Ahead Log，预写日志 | 先持久化“如何重做修改”，再允许对应数据页落盘。它解决事务恢复，不等同于 page mapping log。 |
| Mapping WAL | 地址映射日志 | 记录 `PID -> 新 offset` 的变化；必须在新页数据持久化之后提交。 |
| DWB / doublewrite | Doublewrite Buffer，双写缓冲 | 覆盖原页前先保存一份安全副本，避免掉电只写了半页；代价通常接近多写一遍。 |
| In-place write | 原地更新 | 同一个 PID 总写回固定 offset，像修改数组的固定下标。 |
| Out-of-place write | 异地更新 | 新版本追加到新位置，持久化后再切换映射；像 copy-on-write。 |
| PID | Page Identifier，页标识 | DBMS 认识的稳定逻辑页 ID；out-of-place 后不能再简单用 `PID * page_size` 定位。 |
| LSN | Log Sequence Number，日志序号 | WAL 中单调递增的位置/时间轴，用于判断恢复顺序和估计页面更新间隔。 |
| Extent | 区段 | 一段连续存储空间；旧版本失效后成为可回收 extent。 |
| GC | Garbage Collection，垃圾回收 | 搬走仍有效的数据，再回收混有旧版本的空间。本文同时存在 DB GC 和 SSD GC，必须看清所在层。 |
| Victim | 回收受害者 | 本轮 GC 选择回收的 zone/block，不表示数据损坏。 |
| Valid ratio | 有效数据比例 | victim 中仍需保留的数据占比；越高，需要 copyback 的数据越多。 |
| Copyback | 有效数据搬迁 | GC 为释放整个回收单位而重写其中仍有效的数据。 |
| OP | Over-Provisioning，预留空间 | 不存放当前用户数据的余量。空间越多，GC 越能等待旧版本失效；这里不是 operation。 |
| Page packing | 页装箱 | 把多个压缩页塞进对齐的 4 KiB slot，同时保证单个压缩页不跨 slot。 |
| Slot | 槽位 | page packing 的一个对齐物理读单元，本文为 4 KiB。 |
| Deathtime / EDT | Expected Death Time，预计失效时间 | 预计页面下次被新版本替代的 LSN；不是事务过期时间。 |
| B-tree | Balanced Tree，平衡树索引 | 通过固定大小节点页支持点查和范围查；本文改造的 LeanStore 属于这一类。 |
| LSM-tree | Log-Structured Merge Tree | 先顺序写入新数据，再后台合并多层有序文件；天然 out-of-place，但 compaction 仍会写放大。 |
| Compaction | 压实/归并 | 重写多个旧文件或区段，丢弃过期版本并形成新布局；可视为 LSM/日志结构系统的一类 GC。 |

### SSD 内部与新接口

| 名词 | 全称 | 面向软件开发者的直觉 |
| --- | --- | --- |
| NAND | NAND flash | SSD 的实际存储介质；有有限擦写寿命，且写入前通常需要按 erase block 擦除。 |
| LBA | Logical Block Address，逻辑块地址 | 主机看到的地址，类似虚拟地址。 |
| PBA | Physical Block Address，物理块地址 | 数据在 NAND 中的真实位置，类似物理地址；通常只由 SSD 固件掌握。 |
| FTL | Flash Translation Layer，闪存转换层 | SSD 固件中的 `LBA -> PBA` 映射、GC 和磨损均衡层，可以把它看作设备内部的小型存储引擎。 |
| Wear leveling | 磨损均衡 | 让擦写分散到不同闪存块，避免少数块提前报废；可能改变 DBMS 推测的物理放置。 |
| Multiplexing | 写流混排 | SSD 把多条主机写流交错放入同一 superblock，导致生命周期不同的数据被绑在一起回收。 |
| Active group | 活跃组 | 同一时期并发写入的一组 DB zone；NoWA 需要保持组内重写频率平衡。 |
| ZNS | Zoned Namespace | NVMe 分区接口：zone 内顺序写、整区 reset，把主要放置和 GC 责任交给主机。 |
| FDP | Flexible Data Placement | NVMe 灵活数据放置：主机给写入附加 placement hint，让 SSD 隔离不同生命周期的数据。 |
| RU | Reclaim Unit，回收单元 | FDP SSD 内部进行回收的空间粒度；DB zone 应与它对齐。 |
| RUH | Reclaim Unit Handle，回收单元句柄 | FDP 提供的独立写流入口；并发 active zone 数不能超过可用 RUH 数。 |
| PlID | Placement Identifier，放置标识 | I/O 请求携带的 FDP 提示值，用来选择 RUH。 |
| NoWA | No Write Amplification pattern | 普通 SSD 上通过 active-group 约束和补偿写，尽量让 SSD 获得整块失效空间的写入模式。 |
| OCP telemetry | Open Compute Project SSD telemetry | 部分数据中心 SSD 暴露的物理写入遥测，论文用它读取 NAND writes。 |
| Namespace | NVMe Namespace | 主机看到的一块逻辑 NVMe 盘及其 LBA 空间；ZNS 是带 zone 约束的一种 namespace。 |

### 工作负载与延迟

| 名词 | 全称 | 面向软件开发者的直觉 |
| --- | --- | --- |
| OLTP | Online Transaction Processing | 大量短事务、随机读写和并发更新的在线事务负载。 |
| YCSB-A | Yahoo! Cloud Serving Benchmark A | 典型 50% read、50% update 的固定数据集 KV 负载。 |
| TPC-C | Transaction Processing Performance Council Benchmark C | 模拟订单、库存和支付的关系型事务负载，数据会随新订单增长。 |
| Zipf / theta | Zipf 偏斜分布及参数 | 少量热点键承载大部分访问；`theta` 越高通常越偏斜。 |
| QD | Queue Depth，队列深度 | 同时在设备队列中等待/执行的 I/O 数量。 |
| p99 / p999 | 99/99.9 分位延迟 | 99%/99.9% 请求不超过的延迟，用来观察少数慢请求和 GC 停顿。 |
| fio | Flexible I/O Tester | 绕开数据库、直接生成块 I/O 的压测工具；只能回答设备特征，不能代替数据库端到端测试。 |
| blkdiscard | Linux block discard 工具 | 通知 SSD 某个逻辑范围全部无效，近似重置测试前的 FTL 状态；会破坏该范围内的数据。 |

## 0. 结论先行

这篇论文最有价值的结论不是“SSD 喜欢顺序写”，而是：

> 数据库管理系统（DBMS）必须以最终写入 NAND 闪存介质的字节数为目标，
> 同时控制数据库层和 SSD 层的写放大。Out-of-place（异地更新，新版本写到
> 新位置）不是优化本身，而是获得写入时机、分组和位置控制权的前提。

端到端写路径可以概括为：

```text
事务产生的脏页
    |
    |  doublewrite / checkpoint / DB GC / compression
    v
DBMS 下发给设备的字节                 DB WAF
    |
    |  FTL 映射 / SSD GC / wear leveling
    v
NAND 实际写入的字节                   SSD WAF

Total WAF = DB WAF * SSD WAF
```

对数据库开发最重要的七点是：

1. **只优化 DB WAF（数据库层写放大）不够。** DBMS 少写了一点，却让 SSD
   内部混合了不同生命周期的数据，最终 NAND 可能写得更多。
2. **朴素 out-of-place 可能比 in-place（固定位置覆盖）更差。** 论文的
   800 GB 实验中，
   DB WAF 从 `2.00` 上升到 `4.06`，物理写入从 `4,378 B/op` 上升到
   `7,274 B/op`。原因是数据库自己的 GC 尚未治理。
3. **压缩的收益不只来自少写数据。** 它还扩大等效 over-provisioning
   （OP，供回收周转的预留空间），让 GC 可以等待更多旧版本失效，降低
   victim zone（本轮被选中回收的分区）的有效页比例。
4. **压缩页必须遵守设备读粒度。** 变长压缩页跨越 4 KiB 边界会把一次
   逻辑读取变成两次物理读取。Page packing（页装箱）的目标是“一页一次
   4 KiB I/O”。
5. **GC（垃圾回收）的核心不是更聪明地挑 victim，而是写入时就不要混放
   不同 deathtime（预计失效时间）的页。** 放置决策决定了未来 GC 的上限。
6. **并发写流会在普通 SSD 内部被 multiplex（混排）。** 即使每条流都是
   顺序写，生命周期不同的流被混进同一 superblock（SSD 内部并行写入/
   回收单元），仍会产生 SSD GC 写放大。
7. **崩溃一致性是第一约束。** 新页必须先持久化，再提交
   `PID（逻辑页 ID）-> offset（当前物理位置）` 映射；否则省掉
   doublewrite（双写保护）的同时会引入不可恢复的 torn mapping
   （只持久化了一半状态的地址映射）。

## 1. 论文解决了什么问题

### 1.1 三种字节必须分开计量

论文把写入分成三层：

| 层次 | 含义 | 典型来源 |
| --- | --- | --- |
| User writes | 正常 eviction/checkpoint 本来要落盘的页 | 脏页 |
| DBMS writes | DBMS 实际下发的字节 | doublewrite、DB GC、压缩 |
| Flash writes | NAND 实际写入的字节 | SSD 内部 GC、搬迁 |

对应定义：

```c
#include <stdbool.h>
#include <stdint.h>

struct write_counters {
    uint64_t user_page_bytes; /* 压缩前，因 eviction/checkpoint 产生 */
    uint64_t host_data_bytes; /* DBMS 实际下发的数据页和 DB GC 字节 */
    uint64_t nand_bytes;      /* OCP/厂商遥测暴露的 NAND 物理写入 */
    uint64_t wal_bytes;       /* 单列，避免定义漂移 */
    uint64_t operations;
};

struct waf {
    double db;
    double ssd;
    double total;
};

static bool compute_page_waf(const struct write_counters *c, struct waf *out)
{
    if (c->user_page_bytes == 0 || c->host_data_bytes == 0)
        return false;

    out->db = (double)c->host_data_bytes / (double)c->user_page_bytes;
    out->ssd = (double)c->nand_bytes / (double)c->host_data_bytes;
    out->total = (double)c->nand_bytes / (double)c->user_page_bytes;
    return true;
}
```

这里有两个容易踩的口径问题：

- 压缩后 `DB WAF < 1` 是合法的，因为分子是压缩后的 host bytes，分母是
  原始脏页字节。
- 论文为隔离数据页路径，在开篇分解中忽略了 WAL。生产监控不能悄悄照搬。
  应同时报告 `page_total_waf` 和包含 WAL、元数据、文件系统日志后的
  `all_media_bytes/op`。

### 1.2 为什么 in-place 会在两层同时放大

传统页式引擎把 `PID` 固定映射为文件偏移。覆盖旧页时，为防止 torn page
（掉电后页面只有一部分是新数据），通常先写 doublewrite/full-page image
（完整页镜像），再覆盖目标位置，因此 DB WAF 接近 `2`。SSD 自己仍然
out-of-place，并可能再次搬迁有效 NAND page。

论文在 Samsung PM9A3 上测得 in-place LeanStore：

```c
static double total_waf(double db_waf, double ssd_waf)
{
    return db_waf * ssd_waf;
}

/* total_waf(2.00, 2.36) == 4.72 */
```

也就是说，一个 4 KiB B-tree 页更新最终约产生 `18.85 KiB` flash writes。
论文用 1 DWPD（每天允许写满整盘一次）、持续约 `400 MB/s` 的写速率粗略
估算，设备耐久额度约 1.5 个月就会耗尽。这个寿命数字依赖持续负载假设，
不应直接用于容量规划，但它准确揭示了量级风险。

### 1.3 为什么 out-of-place 只是起点

out-of-place 让旧版本在新版本持久化前仍然有效，因此可以去掉
doublewrite；同时，DBMS 获得以下控制权：

```c
struct placement_decision {
    uint64_t when_to_flush;
    uint64_t target_zone;
    uint64_t expected_death_lsn;
    uint32_t packed_slot;
    uint16_t placement_id; /* FDP */
};
```

但代价是 DBMS 自己必须维护地址映射、回收旧版本。如果只是把页追加到日志，
然后使用朴素 greedy GC（每次挑有效数据最少的 zone），数据库 GC 的
copyback（搬走仍有效的数据）会吞掉全部收益。论文
800 GB 实验的朴素 out-of-place 结果就是反例：

| 版本 | OPS | DB WAF | SSD WAF | 物理写入 |
| --- | ---: | ---: | ---: | ---: |
| in-place | 229K | 2.00 | 2.36 | 4,378 B/op |
| 朴素 out-of-place | 230K | 4.06 | 1.94 | 7,274 B/op |
| 全部优化 | 535K | 0.60 | 1.00 | 567 B/op |
| 全部优化但关闭压缩 | 328K | 3.58 | 1.00 | 3,364 B/op |

所以设计评审中不能接受“已经改成 append-only，所以 SSD 友好”这种结论。

## 2. 正确的 out-of-place 持久化协议

论文修改了 buffer manager（缓存页）、I/O interface（发起设备请求）、
space manager（维护页到位置的映射）和 GC。最关键的正确性不变量是：

```text
使页面版本可恢复的 WAL
        happens-before
新页面数据持久化
        happens-before
PID -> 新 offset 的映射提交
        happens-before
旧页面空间可被回收
```

图中的 `happens-before` 表示持久化先后约束：前一步必须确认掉电后仍然存在，
才能提交后一步，而不只是代码执行顺序。

下面是 C 风格的协议骨架。`persist_*` 表示必须跨掉电持久化，不能仅以
异步 I/O completion（请求已完成的通知）代替；completion 不一定等价于数据
已经越过设备易失缓存。

```c
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t page_id_t;
typedef uint64_t lsn_t;
typedef uint64_t disk_off_t;

struct page_image {
    const void *data;
    uint32_t len;
    lsn_t page_lsn;
};

struct map_update {
    page_id_t pid;
    disk_off_t old_off;
    disk_off_t new_off;
    uint32_t new_len;
    lsn_t page_lsn;
};

/* 下列函数由 WAL、I/O 和映射模块实现。 */
bool persist_wal_through(lsn_t lsn);
bool append_page(const struct page_image *page, disk_off_t *new_off);
bool persist_page_range(disk_off_t off, uint32_t len);
bool append_mapping_wal(const struct map_update *u, lsn_t *record_lsn);
bool persist_mapping_wal(lsn_t record_lsn);
void publish_mapping(const struct map_update *u);
void retire_old_extent_after_epoch(disk_off_t old_off);

bool flush_page_out_of_place(page_id_t pid,
                             disk_off_t old_off,
                             const struct page_image *page)
{
    disk_off_t new_off;
    lsn_t map_lsn;

    /* WAL rule 1: 页面包含的逻辑更新必须先可重放。 */
    if (!persist_wal_through(page->page_lsn))
        return false;

    /* 旧版本仍有效；失败只会留下可扫描回收的 orphan extent。 */
    if (!append_page(page, &new_off))
        return false;
    if (!persist_page_range(new_off, page->len))
        return false;

    struct map_update u = {
        .pid = pid,
        .old_off = old_off,
        .new_off = new_off,
        .new_len = page->len,
        .page_lsn = page->page_lsn,
    };

    /* WAL rule 2: durable data before durable mapping. */
    if (!append_mapping_wal(&u, &map_lsn))
        return false;
    if (!persist_mapping_wal(map_lsn))
        return false;

    publish_mapping(&u);
    retire_old_extent_after_epoch(old_off);
    return true;
}
```

崩溃点应逐个做 fault injection（在指定持久化边界主动模拟掉电）：

| 崩溃位置 | 恢复结果 |
| --- | --- |
| 新页持久化前 | checkpoint 中仍指向旧页 |
| 新页已持久化、mapping WAL 未提交 | 新页是 orphan（没有映射指向的孤儿空间），旧页仍有效 |
| mapping WAL 已提交、内存映射未发布 | recovery 重放 mapping WAL 后指向新页 |
| 旧 extent 标记失效后 | 新映射必须已经 durable |

checkpoint 至少需要保存 `PID2OffsetTable` 和影响放置恢复的
`ActiveGroupHistory`。论文从 checkpoint LSN 开始重放 mapping WAL，再从
最终映射反建 `StorageSpace` 反向索引。这里不能只测试 clean shutdown
（进程按正常流程刷盘并退出）；
必须覆盖数据写、mapping log 写、checkpoint rename 和 zone reclaim 中间的
掉电点。

## 3. 压缩与 4 KiB page packing

### 3.1 压缩为什么同时影响三件事

论文使用 LZ4（偏低 CPU 开销）/ZSTD（通常压缩率更高）测得多种 OLTP/
真实数据集的压缩后大小为原始数据的 `14% - 49%`。它产生三个联动收益：

1. 每次 flush 的 host bytes 下降；
2. 数据集占用下降，等效 OP 空间增加；
3. GC 更晚发生，届时更多旧页已失效，victim valid ratio 下降。

在 800 GB 实验里，压缩后数据集约为 418 GB，使设备等效空闲空间达到
53%，GC valid ratio 从 75% 降到 14%。因此，不能把论文的主要收益简单归因
为“LZ4 更快”：空间余量改变了整个 GC 工作点。

### 3.2 不能让压缩页跨 4 KiB 边界

一个 3,000 B 压缩页如果跨 4 KiB 边界，需要两次 4 KiB 读取，读取量相对
压缩页达到约 `2.73x`。论文用 best-fit packing（每次选择放入后剩余空间
最小的可用 slot）把多个压缩页装入对齐的 4 KiB slot，保证每个页只需读取
一个 slot。

下面的代码表达核心不变量，不包含排序步骤；生产实现应先按压缩长度降序，
再做 best-fit。

```c
#include <stddef.h>
#include <stdint.h>

enum { IO_GRANULE = 4096, MAX_SLOTS = 64 };

struct slot {
    uint16_t used;
};

struct packed_ref {
    uint16_t slot_index;
    uint16_t offset_in_slot;
    uint16_t compressed_len;
};

static int pack_one(struct slot slots[MAX_SLOTS],
                    size_t *slot_count,
                    uint16_t compressed_len,
                    struct packed_ref *out)
{
    if (compressed_len == 0 || compressed_len > IO_GRANULE)
        return -1;

    size_t best = MAX_SLOTS;
    uint16_t best_left = UINT16_MAX;

    for (size_t i = 0; i < *slot_count; ++i) {
        uint16_t left = (uint16_t)(IO_GRANULE - slots[i].used);
        if (left >= compressed_len &&
            (uint16_t)(left - compressed_len) < best_left) {
            best = i;
            best_left = (uint16_t)(left - compressed_len);
        }
    }

    if (best == MAX_SLOTS) {
        if (*slot_count == MAX_SLOTS)
            return -1;
        best = (*slot_count)++;
        slots[best].used = 0;
    }

    out->slot_index = (uint16_t)best;
    out->offset_in_slot = slots[best].used;
    out->compressed_len = compressed_len;
    slots[best].used = (uint16_t)(slots[best].used + compressed_len);

    /* 每个对象完全位于单个 4 KiB slot 中。 */
    return slots[best].used <= IO_GRANULE ? 0 : -1;
}
```

落盘和读取约束：

```c
#include <assert.h>
#include <stdint.h>

static uint64_t slot_disk_offset(uint64_t batch_base, uint16_t slot_index)
{
    assert((batch_base % IO_GRANULE) == 0);
    return batch_base + (uint64_t)slot_index * IO_GRANULE;
}

static uint64_t read_offset_for_page(uint64_t packed_page_offset)
{
    /* 读取整个 4 KiB slot，再按 offset/len 解压目标页。 */
    return packed_page_offset & ~(uint64_t)(IO_GRANULE - 1);
}
```

实现时还要守住四条边界：

- 压缩结果大于等于原页时，回退到一个独占、带格式标志的未压缩 slot；
- 已落盘的 packed slot 是 immutable 的，不能原地改其中一个页；
- mapping 中保存版本、checksum、压缩算法、slot offset 和 compressed size；
- 启动时实测设备最优读粒度，不要把论文的 4 KiB 无条件推广到所有介质。

## 4. 用 deathtime 控制数据库 GC

### 4.1 valid ratio 决定 GC 写放大

若 victim zone 的有效数据比例为 `r`，为腾出 `1-r` 的空间必须搬走 `r`：

```c
#include <math.h>

static double gc_waf(double valid_ratio)
{
    if (valid_ratio < 0.0 || valid_ratio >= 1.0)
        return INFINITY; /* 配置/统计错误或无法回收 */
    return 1.0 / (1.0 - valid_ratio);
}
```

`r = 0.75` 时，GC WAF 为 `4`。所以到高水位（可用空间逼近安全下限）才启动
GC，往往已经太晚：
空闲空间越少，候选 zone 的有效页比例越高，GC 越慢，前台又越容易追上 GC，
最终形成写停顿。

### 4.2 预测页的下一次失效时间

论文为每页保存最近 `n` 次正常写入的 LSN，估计：

```text
EDT = current_lsn + (last_write_lsn - first_write_lsn) / (n - 1)
```

GC copyback 不得更新历史，否则会把“被迫搬迁”误认为业务热度：

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { WRITE_HISTORY = 4 };

struct death_history {
    uint64_t lsn[WRITE_HISTORY];
    uint8_t count;
};

static uint64_t saturating_add_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t estimate_death_lsn(const struct death_history *h,
                                   uint64_t current_lsn)
{
    if (h->count < 2)
        return UINT64_MAX; /* 新页另按 index/table ID 分组 */

    uint64_t span = h->lsn[h->count - 1] - h->lsn[0];
    uint64_t interval = span / (uint64_t)(h->count - 1);
    return saturating_add_u64(current_lsn, interval);
}

static void record_persist(struct death_history *h,
                           uint64_t lsn,
                           bool is_gc_copy)
{
    if (is_gc_copy)
        return;

    if (h->count < WRITE_HISTORY) {
        h->lsn[h->count++] = lsn;
        return;
    }

    for (size_t i = 1; i < WRITE_HISTORY; ++i)
        h->lsn[i - 1] = h->lsn[i];
    h->lsn[WRITE_HISTORY - 1] = lsn;
}
```

这个预测器很简单，但设计方向比模型精度更重要：DBMS 知道 PID、index ID、
表、租户、checkpoint、事务阶段和历史更新频率，SSD 不知道。应先用这些
稳定语义分组，再考虑复杂机器学习。

### 4.3 放置和回收必须使用同一套分类

写入时选择平均 EDT 最接近的 active zone（仍有空间、当前允许追加的分区）：

```c
#include <stddef.h>
#include <stdint.h>

struct zone {
    uint64_t id;
    uint64_t average_edt;
    uint64_t free_bytes;
    uint64_t valid_bytes;
    uint64_t invalid_bytes;
    int state; /* EMPTY, OPEN, FULL */
};

static struct zone *choose_zone(struct zone *zones,
                                size_t count,
                                uint64_t edt,
                                uint64_t bytes)
{
    struct zone *best = NULL;
    uint64_t best_distance = UINT64_MAX;

    for (size_t i = 0; i < count; ++i) {
        if (zones[i].state != 1 || zones[i].free_bytes < bytes)
            continue;

        uint64_t distance = zones[i].average_edt > edt
            ? zones[i].average_edt - edt
            : edt - zones[i].average_edt;
        if (distance < best_distance) {
            best = &zones[i];
            best_distance = distance;
        }
    }
    return best;
}
```

GC 也必须按 EDT 重放有效页。否则 GC copyback 会再次把冷热页混在一起，
破坏写入阶段建立的分组。论文的做法是：

1. 选择足以释放一个 zone 空间的多个 victim；
2. 读取仍有效且不在 buffer pool 的页；
3. 按 EDT 降序排序并重新 packing；
4. 冷页写入最冷的 zone，热页进入低 EDT zone；
5. 永不更新的页使用 `UINT64_MAX`；
6. 回收后重新分配 zone 的 EDT 范围。

## 5. 对齐数据库 GC 与 SSD GC

### 5.1 先区分三类设备

```c
enum ssd_mode {
    SSD_CONVENTIONAL,
    SSD_ZNS,
    SSD_FDP
};

struct device_caps {
    enum ssd_mode mode;
    uint64_t zns_zone_bytes;
    uint64_t fdp_ru_bytes;
    uint32_t max_open_zones;
    uint32_t fdp_ruh_count;
    uint64_t inferred_gc_unit_bytes;
};

struct db_layout {
    uint64_t zone_bytes;
    uint32_t max_open_zones;
    bool use_zone_append;
    bool use_placement_hints;
    bool use_nowa;
};

static bool make_layout(const struct device_caps *d, struct db_layout *out)
{
    switch (d->mode) {
    case SSD_ZNS:
        out->zone_bytes = d->zns_zone_bytes;
        out->max_open_zones = d->max_open_zones;
        out->use_zone_append = true;
        out->use_placement_hints = false;
        out->use_nowa = false;
        return out->zone_bytes != 0 && out->max_open_zones != 0;

    case SSD_FDP:
        out->zone_bytes = d->fdp_ru_bytes;
        out->max_open_zones = d->fdp_ruh_count;
        out->use_zone_append = false;
        out->use_placement_hints = true;
        out->use_nowa = false;
        return out->zone_bytes != 0 && out->max_open_zones != 0;

    case SSD_CONVENTIONAL:
        out->zone_bytes = d->inferred_gc_unit_bytes;
        out->max_open_zones = 1; /* 经设备画像后再提高 */
        out->use_zone_append = false;
        out->use_placement_hints = false;
        out->use_nowa = true;
        return out->zone_bytes != 0;
    }
    return false;
}
```

三类策略不能混为一谈；“普通 SSD”在这里指传统 block namespace，主机只能
提交 LBA 和数据，不能直接控制 NAND 放置：

| 设备 | 数据库 zone 大小 | 避免 SSD multiplex 的方式 |
| --- | --- | --- |
| ZNS | 设备报告的 zone size | zone append + reset；设备保证顺序约束 |
| FDP | 设备报告的 RU size | 每个 zone 固定映射 RUH/placement ID |
| 普通 SSD | 实测推断 GC unit 上界 | NoWA active group + compensation |

FDP 的基本约束是：

```c
static uint16_t placement_id(uint64_t zone_id, uint16_t ruh_count)
{
    return (uint16_t)(zone_id % ruh_count);
}

static bool fdp_layout_is_valid(uint64_t db_zone_bytes,
                                uint64_t ru_bytes,
                                uint32_t open_zones,
                                uint32_t ruh_count)
{
    return db_zone_bytes == ru_bytes &&
           open_zones > 0 &&
           open_zones <= ruh_count;
}
```

### 5.2 普通 SSD 上为什么“多条顺序流”仍会放大

SSD controller 会把并发写流 multiplex 到内部 superblock。若同组 zone 的
失效频率不同，回收某个 zone 后只能让 superblock 部分失效，SSD GC 仍需搬迁
剩余有效页。

NoWA 的两个关键规则是：

```c
struct active_group {
    uint64_t zone_id[64];
    uint64_t rewrite_generation[64];
    uint32_t count;
    bool all_zones_full;
};

static bool may_open_next_group(const struct active_group *g)
{
    /* 当前组未写满时，不允许新组与它在设备内部继续交错。 */
    return g->all_zones_full;
}

static int find_under_rewritten_zone(const struct active_group *g)
{
    if (g->count == 0)
        return -1;

    uint32_t victim = 0;
    for (uint32_t i = 1; i < g->count; ++i) {
        if (g->rewrite_generation[i] <
            g->rewrite_generation[victim])
            victim = i;
    }
    return (int)victim; /* 对它做 compensation rewrite */
}
```

完整 NoWA 还必须记录 active-group history、估计 SSD 最低 free-superblock
阈值，并在 SSD GC 被触发前完成 compensation write（主动重写组内落后的
zone，使同组数据以相近频率失效）。上面的代码只表达不变量，不是完整实现。

论文给出：

```text
max_open_zones * db_zone_size
    == inferred_ssd_gc_unit
    或者是 inferred_ssd_gc_unit 的整数倍
```

例如总 active group 为 8 GB 时，可以是 `16 * 512 MiB`，也可以是
`8 * 1,024 MiB`。较小 zone 有助于降低 DB GC 尾延迟。

需要谨慎理解论文的“保证 SSD WAF = 1”：作者随后明确承认普通 SSD 可能因
调度、wear leveling、flash 状态和 open-superblock 限制重排数据。因此，
**对普通 SSD，NoWA 是经设备画像验证的经验保证；只有 ZNS/FDP 这类明确接口
才提供可依赖的放置语义。**

### 5.3 不知道内部 GC unit 时怎么办

论文在单 active zone 下逐步增大 DB zone，观察 SSD WAF 首次收敛到 `1` 的
位置，将其作为内部 GC unit（SSD 每次希望整体回收的空间粒度）的上界。
六块企业盘中通常落在 `4-8 GB`；无 OCP/FDP 信息时，论文建议 `32 GB`
作为保守上界。

不能把 `32 GB` 写死进默认配置。正确做法是保存设备画像：

```c
struct device_profile {
    char model[64];
    char firmware[32];
    uint64_t namespace_bytes;
    uint64_t inferred_gc_unit_bytes;
    uint64_t tested_fill_permille;
    double measured_ssd_waf;
};

static bool profile_matches(const struct device_profile *p,
                            const char *model,
                            const char *firmware,
                            uint64_t namespace_bytes);
```

型号、固件、namespace 容量或 RAID/虚拟化拓扑变化后，旧画像必须失效。
同型号不同固件也可能有不同 FTL 行为。

## 6. Buffer pool、GC 与前台延迟

论文让 GC 与 worker 共享 buffer pool，以复用已缓存页，并把 GDT GC 读取
变成潜在预取。但 GC 需要 frame；若 buffer pool 已满，GC 为读有效页而触发
脏页 eviction，会形成递归写放大甚至活锁。

论文维护 clean frame reserve：

```c
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

struct frame_budget {
    atomic_uint_fast64_t clean_frames;
    uint64_t gc_reserve;
};

static bool gc_may_pin_clean_frame(struct frame_budget *b)
{
    uint64_t cur = atomic_load_explicit(&b->clean_frames,
                                        memory_order_relaxed);
    while (cur > b->gc_reserve) {
        if (atomic_compare_exchange_weak_explicit(
                &b->clean_frames, &cur, cur - 1,
                memory_order_acquire, memory_order_relaxed))
            return true;
    }
    return false;
}

static bool checkpoint_should_preclean(const struct frame_budget *b)
{
    return atomic_load_explicit(&b->clean_frames,
                                memory_order_relaxed) <= b->gc_reserve;
}
```

工程上还需要：

- foreground GC（业务写入被迫同步等待的回收）和 background GC（提前在
  后台执行的回收）分开计时、计量；
- 为 GC 预留 I/O queue depth，而不是只预留内存；
- 同时观测 `p99/p999 commit latency`、GC pause 和 free-zone 水位；
- 在达到紧急水位前预清理，保留停止接受写入的最后保护线；
- GC 读取不得污染业务 replacement policy，至少应有独立 admission 标志。

## 7. 如何验证，而不是只跑一个 fio

`fio` 只直接测块设备 I/O；它不知道数据库的 WAL、buffer pool、checkpoint
和 DB GC，因此不能证明数据库端到端写路径已经优化。

论文的可信之处在于它不是只测 fresh drive（刚清空、尚未进入稳态 GC 的盘）：

- 每次实验前 `blkdiscard`（通知设备整段 LBA 已无效）重置映射状态；
- 运行到累计写入至少达到设备容量的 `4x`；
- 使用最后一小时的平均值；
- 数据集占设备 90%，buffer pool 为数据集的 5%-20%；
- 同时记录 DB WAF、SSD WAF、吞吐、命中率、CPU 和内存；
- 覆盖 YCSB-A、TPC-C、多个容量和八块企业 SSD。

稳态停止条件可以编码为：

```c
#include <stdbool.h>
#include <stdint.h>

struct run_state {
    uint64_t device_capacity_bytes;
    uint64_t cumulative_host_bytes;
    uint64_t stable_window_seconds;
    double recent_ssd_waf_cv; /* 最近窗口的变异系数 */
};

static bool reached_measurement_state(const struct run_state *s)
{
    if (s->device_capacity_bytes == 0)
        return false;

    bool enough_aging =
        s->cumulative_host_bytes / s->device_capacity_bytes >= 4;
    bool long_enough = s->stable_window_seconds >= 3600;
    bool waf_is_stable = s->recent_ssd_waf_cv <= 0.05;
    return enough_aging && long_enough && waf_is_stable;
}
```

建议数据库团队按以下矩阵验收：

| 维度 | 至少覆盖 |
| --- | --- |
| 填充率 | 50%、75%、90%、接近满盘 |
| 分布 | uniform、Zipf 多个 theta、真实 trace |
| 读写比 | 写密集、混合、读密集 |
| 工作集 | 小于、接近、大于 buffer pool |
| 生命周期 | fresh、1 DW（累计写满一盘）、4 DW（累计写满四盘）、长稳态 |
| 并发 | 单写流到最大 worker/QD |
| 故障 | 每个持久化边界注入掉电 |
| 指标 | host/NAND bytes、OPS、p99/p999、GC、CPU、内存 |

`blkdiscard` 是破坏性操作，只能在隔离测试设备上执行。生产盘不能为了
“复现实验”清空 FTL 状态。

## 8. 实验结果应怎样解释

### 8.1 有说服力的结果

- Samsung PM9A3、800 GB YCSB-A：总 WAF `4.72 -> 0.60`，吞吐
  `229K -> 535K OPS`。
- 六种普通/FDP 企业 SSD、90% 满盘：总 WAF 相对 in-place 降低
  `6.2x - 9.76x`。
- TPC-C 15,000 warehouses：相同时间内完成 `2.45x` new-order
  事务；相同事务数下 flash writes 减少 `7.2x`。
- FDP 用 placement hint 代替 NoWA 后：DB WAF `0.57 -> 0.54`，
  吞吐 `541K -> 553K OPS`，说明补偿写确有成本，但不大。

### 8.2 ZNS 的主要收益容易被误读

相同 1,500 GB 数据集时，ZNS 比普通 namespace（传统 NVMe 逻辑盘）快
31%；但相同填充率时只快约 10%。这说明大部分收益来自 ZNS 暴露了更多
可用容量，而不是 zone append 本身更快。

选型时应分开计算：

```c
struct zns_value {
    double gain_from_more_usable_capacity;
    double gain_from_ssd_waf_one;
    double host_gc_cpu_cost;
    double operational_complexity;
};
```

否则很容易把容量差异误归因于接口性能。

### 8.3 论文表 1 有一处数字矛盾

表 1 的 `+ comp. + pagepack` 行同时给出：

```text
DB WAF = 0.62
SSD WAF = 1.95
Logical writes = 566 B/op
Physical writes = 566 B/op
```

但按定义应满足：

```c
double expected_physical = 566.0 * 1.95; /* 约 1,104 B/op */
```

Figure 13b 的柱状图也显示该版本仍有约一倍 SSD GC 写入。因此
`Physical writes = 566 B/op` 很可能是排版或数据错误；下一行 GDT 的
`566 * 1.96 = 1,109 B/op` 与表中的 `1,110 B/op` 一致。引用这张表时不要
原样传播该单元格。

## 9. 论文没有替你证明什么

这些限制决定了方案不能直接照搬：

1. **主要假设是 DBMS 直接写 block device（如 `/dev/nvme...` 暴露的线性
   逻辑块接口）。** 文件系统、device mapper（Linux 块设备映射层）、RAID、
   云盘和虚拟化层都可能重排或合并写入。
2. **NoWA 不具备跨设备的协议级保证。** 论文在六块企业 SSD 上实测成功，
   但也承认固件可重排数据；消费级 SSD 风险更高。
3. **恢复设计有描述，没有系统性的掉电实验结果。** 去掉 doublewrite 前，
   必须自己做 torn write、mapping WAL 和 checkpoint fault injection。
4. **没有给出 GC tail latency 的完整结果。** 平均 OPS 提升不能证明
   `p999` 或写停顿可接受。
5. **内存成本不小。** 满盘时额外 metadata 最坏达到 `10.9 GB`，大容量盘
   或多盘部署必须设计 metadata paging/sharding。
6. **压缩掩盖了部分 GC 难题。** 800 GB 数据压到 418 GB 后，工作点从
   90% 满盘变成大量空闲；必须额外看关闭压缩的结果。
7. **实验集中在企业 SSD、写密集且 out-of-memory（数据集大于内存）的
   OLTP。** 结论不能无条件外推到消费盘、云 EBS、read-heavy 或全内存
   工作负载。
8. **未覆盖多设备和共享设备。** 这两项也被作者列为未来工作。多租户写流
   会破坏单 DBMS 对物理放置的推断。

## 10. 面向数据库内核的实施顺序

不要一次性重写存储层。合理顺序是：

```text
Phase 0  先把 user/host/NAND/GC/WAL bytes 量准
   |
Phase 1  引入 out-of-place + PID mapping + recovery
   |     验证所有崩溃点，暂不追求 WAF
Phase 2  增加压缩 + 4 KiB page packing
   |
Phase 3  zone space manager + watermark + clean-frame reserve
   |
Phase 4  GDT placement + GDT-aware GC
   |
Phase 5  按设备选择 ZNS / FDP / profiled NoWA
```

其中 `watermark` 是触发后台/紧急 GC 的空间水位，`clean-frame reserve`
是专门留给 GC 读取使用的干净 buffer frame 数量。

每一阶段都保留 feature flag 和回退路径：

```c
struct storage_features {
    bool out_of_place;
    bool page_compression;
    bool page_packing;
    bool gdt_placement;
    bool gdt_gc;
    bool nowa;
    bool zns;
    bool fdp;
};

static bool valid_features(const struct storage_features *f)
{
    if ((f->page_packing || f->gdt_placement || f->gdt_gc ||
         f->nowa || f->zns || f->fdp) && !f->out_of_place)
        return false;
    if (f->gdt_gc != f->gdt_placement)
        return false; /* 单边开启会重新混合生命周期 */
    if ((f->zns ? 1 : 0) + (f->fdp ? 1 : 0) + (f->nowa ? 1 : 0) > 1)
        return false;
    return true;
}
```

阶段门禁至少包括：

- 恢复后每个 PID 指向 checksum 正确且 LSN 合法的唯一版本；
- orphan extent 可回收，live extent 不会被提前回收；
- 压缩页始终单次 4 KiB read；
- 高水位下 foreground write 不会追上 GC；
- device profile 变化时自动禁用 NoWA/FDP 参数；
- 优化以 `NAND bytes/op` 和尾延迟为准，不以 host throughput 单指标拍板。

## 11. 对不同引擎的启发

### B-tree / 页式引擎

相关性最高。收益来自去 doublewrite、页压缩和生命周期分组；最大工程风险是
`PID -> offset` 映射的内存成本、checkpoint 和崩溃恢复。

### LSM-tree

LSM 已经 out-of-place，但不能据此认为问题已解决。compaction policy 会同时
改变 DB WAF、空间放大和 SSD WAF。应把 SST（Sorted String Table，LSM 的
不可变有序文件）的预计删除/compaction 时间映射到 ZNS zone 或 FDP
placement ID，而不是只优化 level（LSM 层级）的 bytes written。

### PostgreSQL / InnoDB 类固定页位置引擎

彻底采用本文方案是架构级改动，不能只删掉 doublewrite/full-page writes。
在改造前可先做：

- 分离 WAL、数据和临时写流；
- 校准压缩后实际 block I/O，而不是只看逻辑压缩率；
- 监控 host writes 与 NAND writes；
- 避免数据盘长期逼近满盘；
- 在明确支持时评估 atomic write（设备保证全写或全不写）/FDP，而不是根据
  型号猜测。

### Append-only / 列存 / 对象存储

更容易采用 zone 和 deathtime 分组，但仍要防止 compaction/merge 把不同
生命周期的数据重新混合。对象删除时间、partition TTL 和 compaction epoch
都是比块设备热度推断更强的信号。

## 12. 最终检查表

设计一个 SSD 友好的数据库写路径前，应能回答：

- [ ] `user bytes`、`host bytes`、`NAND bytes` 的定义是否固定？
- [ ] 是否报告 `DB WAF`、`SSD WAF`、`Total WAF` 和 bytes/op？
- [ ] 是否在 90% 等高填充率、至少数个 drive writes 后测稳态？
- [ ] out-of-place mapping 的持久化顺序是否经过掉电验证？
- [ ] mapping、reverse mapping、zone metadata 的内存上界是多少？
- [ ] 压缩页是否保证一次对齐 I/O 可读？
- [ ] GC 是否有 clean frame、I/O credit 和 emergency headroom？
- [ ] 正常写与 GC copy 是否使用同一 deathtime 分类？
- [ ] 是否识别 ZNS/FDP 能力，并遵守 open-zone/RUH 上限？
- [ ] 普通 SSD 的 GC unit 是否按型号和固件实测，而非写死？
- [ ] 多线程 active group 是否保持生命周期和 rewrite frequency 平衡？
- [ ] 是否同时验证平均吞吐、p99/p999、恢复时间和设备寿命预算？

一句话总结：**让 DBMS 负责“哪些数据应该一起死”，让设备接口负责“这些
数据确实被放在一起”；两者缺一，out-of-place 只会把 doublewrite 问题换成
GC 问题。**
