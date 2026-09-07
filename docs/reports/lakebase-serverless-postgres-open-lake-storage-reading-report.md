# 《Lakebase: Serverless Postgres over Open Lake Storage》阅读报告

> Jasraj Dange, Andrei Dragus, Ali Ghodsi, Haoyu Huang, Yihe Huang,
> Stas Kelvich, Heikki Linnakangas, Hans Norheim, Ippokratis Pandis,
> Nikita Shamgunov, Em Sharnoff, John Spray, Zhou Sun, Reynold Xin,
> Matei Zaharia. *Lakebase: Serverless Postgres over Open Lake Storage*.
> PVLDB 19(12): 4385-4398, 2026.
> DOI: [10.14778/3827998.3828040](https://doi.org/10.14778/3827998.3828040).
>
> 原文：[本地 PDF](../pdfs/p4385-pandis.pdf) |
> [VLDB](https://www.vldb.org/pvldb/vol19/p4385-pandis.pdf)
>
> 阅读日期：2026-09-07。本文将论文明确陈述的事实、论文评测能支持的
> 结论和工程推导分开描述。PDF SHA-256：
> `8dc6a9001df043f1e30775c8e21f560d81dcf2165465c0b934a367bf211c12ef`。

## 0. 结论先行

这篇论文解决的核心问题是：

> 如何保留 PostgreSQL 的事务语义和生态，同时把数据库从“长期运行、拥有
> 本地磁盘的服务器”改造成“可随时创建和销毁的事务计算层”，并让同一份
> 持久数据能够被分支、恢复和其他计算引擎直接访问。

Lakebase 的关键做法不是简单地把 PostgreSQL 数据目录搬到 S3。对象存储延迟
不足以直接支撑 OLTP，因此系统把传统数据库的“存储”拆成三个不同职责：

| 职责 | 组件 | 核心契约 |
| --- | --- | --- |
| 前台事务计算 | PostgreSQL Compute | 执行 SQL、并发控制和事务逻辑；本地状态可丢弃。 |
| 同步提交与写者隔离 | Safekeeper | WAL 经多数派落盘后提交；term 对主写者做 fencing。 |
| 页面物化与历史读取 | Pageserver | 将 WAL 异步组织为 `page@LSN`，支撑读、恢复、PITR 和分支。 |
| 长期持久化 | Cloud Object Storage | 保存 WAL、image/delta layer 和索引，是长期存储底座。 |
| 热数据加速 | Compute / Pageserver local SSD | 只承担缓存，故障后可以重建。 |

对三个问题的直接回答如下。

| 问题 | 结论 |
| --- | --- |
| 文章解决了什么问题 | 解决数据库计算与持久状态绑定导致的闲置成本、慢扩缩容、慢克隆、运维脆弱性，以及第二代云数据库仍由单一引擎和私有存储格式垄断数据访问的问题。 |
| 创新点在哪里 | 最有区分度的贡献是：以 PostgreSQL page/WAL 和 `LSN` 为跨组件数据契约，把 serverless OLTP、任意时点页面重建、`O(1)` copy-on-write 分支、对象存储持久化和外部分析引擎直读组合成生产系统；Direct-to-Storage 与 Direct Access 尤其体现“存储层成为多引擎接口”。 |
| 对数据库开发的启发 | 应将 durability、materialization、serving、caching 和 compute lifecycle 分开设计；把日志位置变成全系统的一致性坐标；让缓存真正可丢弃；将分支、PITR、fencing、GC 和 backpressure 作为存储协议的一部分，而不是外围功能。 |

但论文的“第三代数据库”是作者提出的架构分类，不是已经形成共识的学术定义。
计算存储分离、WAL quorum、Pageserver/Safekeeper、copy-on-write branching、
warm pool 和 autoscaling 都有明确前序工作，且很多机制直接建立在开源 Neon
架构之上。论文真正的价值是**系统组合、开放访问边界和生产规模验证**，不应
包装成所有组件的首次发明。

## 1. 它重新定义了什么问题

### 1.1 第一代和第二代架构的缺口

论文将数据库架构分为三代：

| 代际 | 典型形态 | 改善了什么 | 仍然缺什么 |
| --- | --- | --- | --- |
| 第一代 | PostgreSQL、MySQL、Oracle 式 compute + local storage | 单机内低延迟、实现边界直接 | 计算和容量一起配置；恢复、复制、扩容、克隆代价高。 |
| 第二代 | Aurora、Socrates、AlloyDB 式内部计算存储分离 | 提高弹性、可用性和存储并行度 | 持久格式和访问路径通常仍为厂商私有，只能经主数据库引擎访问。 |
| 论文所谓第三代 | elastic transactional compute over open lake storage | 计算独立伸缩，数据进入对象存储并允许多引擎访问 | 必须重新解决远端读延迟、缓存、页面重建、格式兼容和控制面复杂性。 |

这一定义的关键不只是 `compute-storage separation`，而是
**storage-engine separation**：持久状态不再只是远端块设备，而成为可由不同
执行引擎消费的共享数据层。

### 1.2 AI agent 是需求放大器，不是存储算法前提

论文给出 Databricks Lakebase 的生产观测：

- 每天创建和管理超过 1,000 万个数据库；
- AI agent 创建的数据库数量约为人类用户的 4 倍；
- 很多 compute 生命周期不足 10 秒；
- 部分项目的 branch depth 超过 500。

这些数据说明传统“一个应用对应一个长期在线实例”的容量模型不再适合大量
短命、分叉、实验性的数据库。Agent 不是 Lakebase 正确性的前提；即使没有
AI，preview environment、CI、测试数据隔离、PITR 和突发负载也会产生同样
需求。Agent 的作用是把这些需求从偶发操作放大为主路径。

### 1.3 论文实际要同时满足六个约束

1. PostgreSQL 协议、SQL 语义和生态兼容。
2. 事务提交不能等待高延迟对象存储。
3. compute 可被销毁，不能拥有唯一持久状态。
4. 任意 `LSN` 的一致页面必须可重建。
5. 分支不能复制全量数据库。
6. 外部引擎访问不能破坏 MVCC、权限和在线 OLTP 隔离。

这些约束彼此冲突。Lakebase 的架构价值在于没有用一个组件同时承担全部职责。

## 2. 架构全景

### 2.1 数据面

```text
SQL clients
    |
    v
Proxy and Lakebase Manager
    |
    v
Ephemeral PostgreSQL Compute
    |                         \
    | page read                \ WAL stream
    v                           v
Local SSD Cache          Safekeeper Quorum
    | miss                     |
    v                          +------> WAL in Object Storage
Pageserver <-------------------+
    |
    | GetPage(key, LSN)
    v
Delta and Image Layers in Object Storage

External engines <---- immutable pages and snapshots ----> Storage
```

读写路径并不对称：

- **写路径以 WAL 为主。** 事务提交依赖 Safekeeper quorum，不等待
  Pageserver 生成新页面，也不等待对象存储上传完成。
- **读路径以 page image 为主。** compute 本地缓存未命中时，向 Pageserver
  请求某个 key 在某个 `LSN` 的页面。
- **对象存储是长期真相源，但不是同步提交点。** 最近提交的数据先由多个
  Safekeeper 的本地盘保护，再异步进入对象存储。

### 2.2 控制面

| 控制面组件 | 责任 |
| --- | --- |
| Lakebase Manager | 管理 PostgreSQL compute fleet、连接唤醒和 warm pool。 |
| Storage Controller | 管理 tenant/timeline，放置 Safekeeper 和 Pageserver，处理故障迁移与分片。 |
| Autoscaler agent | 每 5 秒采集 compute 指标，协调 CPU、memory 和 cache 尺寸。 |
| Guest agent | 配合 NeonVM 在线调整 CPU、virtio-mem 和本地缓存，并以 100 ms 周期感知紧急内存压力。 |

这里有一个重要的工程现实：compute 数据面可以接近无状态，但整个数据库服务
并没有变简单。状态被移到了 timeline metadata、placement、lease、term、
LSN watermark、cache residency 和 warm-pool inventory 等控制面对象中。

## 3. 核心机制

### 3.1 写路径：日志先成为唯一必要的同步持久状态

PostgreSQL compute 使用定制的 `SMGR`：

- 页面写只更新 compute 的 local file cache；
- WAL proposer 将 WAL 同时发送到 3 个 Safekeeper；
- 多数派 Safekeeper flush 后，compute 推进 commit LSN；
- Safekeeper 再把 quorum-committed WAL 上传到对象存储；
- Pageserver 从 Safekeeper 消费 WAL，异步生成页面版本。

```text
Transaction
    |
    v
Generate PostgreSQL WAL
    |
    v
Replicate to 3 Safekeepers
    |
    v
Quorum disk flush
    |
    +------> acknowledge commit
    |
    +------> upload WAL to object storage
    |
    +------> Pageserver materializes pages
```

Safekeeper 的 term 是写者 fencing token。新 primary 必须以更高 term 在 quorum
上完成选举；Safekeeper 只接受已见最高 term 的写入。这比“控制面认为旧主已经
死了”更强，因为写入仲裁点本身拒绝 stale writer。

当 Pageserver 的 ingest、local flush 或 remote upload LSN 落后 commit LSN
超过阈值时，系统对前台写入施加 backpressure。这里体现了一个基本不变量：

> 异步 materialization 可以不在事务延迟中，但不能无限落后；否则读放大、
> 故障恢复时间和 Safekeeper 保留空间会失控。

### 3.2 读路径：`page@LSN` 是存储服务接口

compute 的 local file cache 命中时直接返回页面。未命中时，请求
`GetPage@LSN`。Pageserver：

1. 找到目标 key 在请求 LSN 之前最近的 full-page image；
2. 找出此后所有影响该 key 的 WAL records；
3. redo 到目标 LSN；
4. 返回完整 PostgreSQL page。

简单地请求 current LSN 会迫使 Pageserver 等到 WAL ingest 追平。Lakebase
因此携带两个 LSN：

| 字段 | 含义 |
| --- | --- |
| request LSN | compute 需要观察的事务时间点。 |
| not-modified-since LSN | compute 证明该页面从此位置到 request LSN 没有变化。 |

compute 维护固定大小的 last-written-LSN cache。命中时，Pageserver 只需追到
较早的 not-modified-since LSN，就能安全返回页面，避免无谓等待当前 WAL。

这个优化的本质不是 cache trick，而是将**负知识**作为协议的一部分：调用方
不只说“我要哪个版本”，还证明“这段时间没有影响该 key 的写入”。

### 3.3 Pageserver：二维版本存储

Pageserver 不是远程 PostgreSQL data directory。它在两个维度组织数据：

```text
Page key axis
    |
    |  image layer: full pages at one LSN
    |  delta layer: WAL records over key and LSN ranges
    v
Version history ----------------------------------> LSN axis
```

新 WAL 先顺序进入 delta layer，不在 ingest 热路径做 redo。后台 compaction
把 image 之后的 WAL 应用成新 image，以限制读时 redo chain。

论文用两个参数描述代价：

- `I`：同一 key range 上允许叠加的最大 delta layers，约束单页读取为
  `O(I)` 次 layer visit；
- `R`：累计历史达到旧 base 的多少倍后触发 GC compaction。

论文给出的结论是：

- 写放大为常数阶：delta ingest `1`，另加 image materialization 的
  `O(1/I)` 和 GC compaction 的 `O(1/R)`；
- 读取为 `O(I)` layer visits；
- 空间放大约为 `O(1 + R + wP/N)`，其中 `wP` 是 PITR 窗口必须保留的历史；
- 生产 fleet 观测到约 `7x` space amplification；
- 评测中 compute cache hit rate 通常超过 `98.5%`。

所以低延迟并不是对象存储本身变快，而是绝大多数请求被 compute cache 截住；
Pageserver local SSD 再截住一层；只有冷 layer 才访问对象存储。任何复用该
架构的系统都必须把 cold miss tail latency 当作一级指标。

### 3.4 分支：timeline metadata，而不是数据库复制

一个 branch 由 parent timeline ID 和 branch LSN 定义。branch LSN 之前读取
父 timeline，之后写入子 timeline：

```text
Parent timeline
--------------------+----------------------------->
                    |
                    | branch LSN
                    v
                    +----------------------------->
                    Child timeline
```

创建 branch 只写一个远端 metadata file，因此是 `O(1)` metadata operation。
未在 branch LSN 前提交的事务按 crash recovery 规则回滚，不会在子分支可见。

代价没有消失，只是被延后：

- 子分支缺少页面时递归查父 timeline；
- branch depth 越深，读路径越长；
- compaction、GC、PITR lease 和计费必须理解 ancestry；
- detach 只是立即更新 metadata，祖先数据在后台异步复制；
- 当前不支持 branch merge，因为物理历史分叉后没有通用的语义合并规则。

因此“`O(1)` branching”只描述创建动作，不代表分支整个生命周期是常数成本。

### 3.5 Serverless：CPU、内存和工作集必须联合伸缩

Lakebase 在 Kubernetes 上用 NeonVM 运行独立 QEMU/KVM VM，并支持在线调整：

- CPU：guest agent online/offline vCPU；
- memory：通过 `virtio-mem` 调整，并限制范围以降低碎片导致的缩容失败；
- disk cache：文件系统 discard 转成 host backing file hole punching；
- compute cache：随 VM 同步改变容量。

自动扩缩容分别估算 CPU、memory 和 cache 所需单位，再取最大值。最有启发的
机制是工作集估算：

- 修改 HyperLogLog bucket，把 bit 替换为最近命中该 bit 的时间戳；
- 估计过去 1 到 60 分钟不同窗口内的 unique pages；
- 用 plateau detection 判断工作集；
- 对多个 cutoff 加权，而不是使用会抖动的单一阈值；
- 普通内存约束为 allocation + target page cache 小于总内存 90%；
- allocation-heavy 情况还要求 allocation 小于总内存 75%；
- guest agent 每 100 ms 检查内存压力，提供绕过 5 秒指标周期的快速扩容路径。

论文报告的低于 500 ms 启动来自**预先创建的 warm VM pool**、预热 binary 和
无需在 compute 做 crash redo。它是数据库 compute 冷启动，不是底层云 VM
真正从零创建；没有 warm pool 时，论文观测到分钟级甚至小时级尾延迟。

### 3.6 多引擎访问：把存储层变成执行引擎接口

论文展示三个方向：

| 方向 | 机制 | 一致性边界 |
| --- | --- | --- |
| Lakebase -> Lakehouse | `wal2delta` 解码 logical replication，写入 Delta SCD Type 2 | 异步变更流。 |
| Lakehouse -> Lakebase | Spark 直接生成 PostgreSQL heap pages，上传对象存储 | 全量页面先持久化，再由 PostgreSQL 事务执行 metadata-only relation swap。 |
| Lakebase -> Analytics | Lakehouse//RT 在目标 LSN 直接读 immutable PostgreSQL pages | SIMD page parser + 简化 MVCC；modified-block bitmap 只使失效 Arrow cache 刷新。 |

Direct-to-Storage 的关键不是“绕过 SQL”本身，而是一个批量导入提交协议：

```text
Build all table pages in parallel
    |
    v
Persist complete page set
    |
    v
Commit metadata-only relation swap
    |
    v
New relation becomes visible atomically
```

并发 DML 在 snapshot load 期间被拒绝。这说明 bypass path 只有在可声明清楚
原子可见边界时才安全；不能让多个写者随意修改物理页。

Direct Access 则把分析 CPU 从 OLTP compute 移走。它通过 `(relation,
page range)` 缓存 Arrow batches，并使用 Pageserver 给出的 LSN 区间
modified-blocks roaring bitmap 做精确失效。共享持久数据不等于共享执行资源，
因此分析扫描不会增加 primary 的写延迟。

## 4. 创新点及其边界

### 4.1 最有价值的创新

#### 1. 将 `LSN-addressable page` 提升为跨引擎存储契约

传统远程块存储只回答“给我 block X”，Lakebase 回答“给我 key X 在逻辑时间
T 的事务一致版本”。这个接口同时支撑：

- 无状态 compute 恢复；
- read replica；
- static replica；
- PITR；
- branch；
- 外部分析引擎快照读取。

一个正确的时间寻址存储接口，替代了六套外围复制机制。

#### 2. 将事务提交、页面物化和长期归档解耦

前台只同步等待小而顺序的 WAL quorum；Page reconstruction、compaction 和
S3 upload 后移。这使 compute 不再承担 crash redo 和完整 page persistence，
是 sub-second attach、快速故障替换和高写吞吐的共同基础。

#### 3. 让开放持久层支持双向 bypass

外部引擎不只导出数据，还能：

- 直接构造合法 PostgreSQL pages 做并行 bulk load；
- 直接读取一致 page snapshot 做向量化分析；
- 通过 metadata transaction 和 modified-block bitmap 与 OLTP 协调。

这比“用 CDC 再维护一份分析副本”更接近真正的共享数据层。

#### 4. 把数据库分支变成版本存储原语

branch 不是备份恢复命令的 UI 包装，而是 timeline ancestry、LSN、lease、
GC 和 page lookup 共同理解的结构。只有做到这一层，分支才能成为 agent 和
CI 的高频操作。

#### 5. 工作集感知的联合扩缩容

CPU 利用率无法决定数据库内存。Lakebase 把 unique-page working set、page
cache、匿名分配和 CPU load 合并到扩缩容决策中，体现了 serverless database
与 stateless HTTP service 的根本差异。

### 4.2 哪些不是单点首创

| 机制 | 已有基础 | 论文中的增量 | 判断 |
| --- | --- | --- | --- |
| compute-storage separation | Aurora、Socrates、AlloyDB 等 | 将分离扩展到开放对象存储和多引擎访问 | 架构演进，不是首次提出。 |
| Safekeeper + Pageserver | 开源 Neon 已公开同名组件和相同基本职责 | Databricks 产品化、HA、控制面和 Lakehouse 集成 | 继承并扩展。 |
| WAL quorum replication | 共识复制和 log-as-database 设计已有长期研究 | 与 PostgreSQL WAL、term fencing、Pageserver backpressure 集成 | 工程组合贡献。 |
| copy-on-write branching | 版本存储、快照和 Neon branching 已存在 | 生产中的深分支、快速 compute attach 和 agent workload | 规模与产品路径贡献。 |
| warm pool / VM autoscaling | Serverless 与 MicroVM 系统已有大量工作 | CPU、memory、disk cache、working set 联合伸缩 | 策略和集成有价值。 |
| Direct-to-Storage / Direct Access | bulk bypass、shared storage 和 HTAP 并非新概念 | 直接生成/解析 PostgreSQL pages，并以 LSN/MVCC/bitmap 协调 | 本文最有辨识度的实现之一。 |

因此更准确的评价是：

> Lakebase 不是靠一个全新算法成立，而是选定 `PostgreSQL WAL + page + LSN`
> 作为稳定中间表示，把原本绑定在单个数据库进程中的职责编译成多个可独立
> 伸缩、恢复和复用的服务。

### 4.3 “开放格式”需要降温理解

论文把 PostgreSQL page format 称为 open format，这比完全私有的远端存储
格式更开放，但不等于任意引擎可零成本互操作：

1. 外部 reader 仍需理解 heap page、tuple header、catalog、TOAST、visibility、
   checksum、WAL 和 PostgreSQL 版本差异。
2. Pageserver 的二维 layer index、timeline ancestry、`GetPage@LSN` 和
   modified-blocks API 仍是系统特定协议。
3. 论文的 Direct Access 使用“simplified MVCC checker”，当前只支持
   sequential scan；这证明物理页可读，不等于完整 PostgreSQL 语义已被标准化。
4. 外部 writer 风险更高。Direct-to-Storage 被限制为构造冻结页面、先完整
   上传、再由 PostgreSQL 事务切换 relation，不能泛化为任意多主写入。
5. 论文没有评测跨 PostgreSQL major version 升级、extension-defined storage、
   corruption checking 或第三方独立实现兼容性。

所以它降低的是**结构性锁定**，不是消除所有格式和服务依赖。

## 5. 评测能证明什么

### 5.1 关键结果

| 结果 | 论文数据 | 支持的结论 |
| --- | ---: | --- |
| branch create | 写一个 remote metadata file，`O(1)` | 创建不随数据库大小增长。 |
| compute startup | 生产 p95 持续低于 `500 ms` | warm pool + slim basebackup + storage-side redo 可快速 attach。 |
| cache hit | 评测中通常 `>98.5%` | 热工作集下远端 page reconstruction 被大幅隐藏。 |
| Pageserver space amplification | 生产约 `7x` | PITR 历史和 GC slack 是显著成本。 |
| OLTP | 24 vCPU 下比 Gen-2 高 `26%`，p95 `<20 ms` | 缓存命中场景中，架构没有必然牺牲 OLTP 吞吐。 |
| bulk load | 1 TB 时 Direct-to-Storage 比 COPY 快 `73.3x` | 分布式页面构造绕开单 writer 瓶颈。 |
| analytics | TPC-H SF10 总时间快 `10x`，geomean latency 快 `3x` | 在同一份页面上换向量化分析引擎有明显收益。 |
| concurrent OLTP | 400 QPS 下 Direct Access scan 慢 `1.76x`，primary write latency 不变 | cache invalidation 有成本，但计算隔离保护 primary。 |

### 5.2 不能从评测推出的结论

| 论文未充分覆盖的维度 | 为什么重要 |
| --- | --- |
| cold-cache OLTP 和 object-store tail | TPROC-C 明确让整个 working set 驻留 DRAM，不能代表冷启动后的真实 p99。 |
| 故障注入与数据丢失窗口 | 论文描述协议，但没有展示 SK/PS/AZ 故障矩阵、RTO/RPO 分布或 split-brain 测试。 |
| 深分支读放大 | 生产 branch depth 可超过 500，但没有给出 depth 对 page miss、compaction 和 GC 的曲线。 |
| autoscaling tail latency | 图证明 actual QPS 跟随 target QPS，但没有充分量化扩容期间的事务 p95/p99 和 cache miss 惩罚。 |
| 完整成本模型 | 只用 compute 与 S3 retention 价格说明量级，未展开 GET/PUT、跨 AZ、compaction、7x history 和 warm pool 成本。 |
| 独立可复现性 | Gen-1/Gen-2 对手匿名，Lakebase 控制面和 Lakehouse//RT 不是完整公开 artifact。 |
| PostgreSQL 兼容深度 | 未给出 extension、major upgrade、physical layout 变化和 direct reader 兼容矩阵。 |
| 分支合并 | 系统明确不支持 merge；“Git-like”只适合 branch/restore，不应推导出 Git 的完整语义。 |

还要注意两个 benchmark 口径：

- `73.3x` 是分布式 Spark direct write 对单 PostgreSQL `COPY` writer，证明
  旁路架构的可扩展性，不是同等资源下某个局部算法快 73 倍。
- Direct Access 相对 PostgreSQL 的 TPC-H 优势同时来自 SIMD parser、
  vectorized execution、threaded parallelism 和更好的 optimizer，不能全部
  归因于开放存储格式。

## 6. 对数据库开发的启发

### 6.1 先按持久性职责拆系统，不要先按进程拆

最稳定的边界是：

```text
Commit durability  -> quorum WAL
Version serving    -> page reconstruction
Long-term state    -> object storage
Low latency        -> disposable caches
Execution          -> replaceable compute
```

如果一个节点同时拥有唯一 page、唯一 WAL、唯一 cache 和唯一执行上下文，它就
无法被快速替换。相反，只要恢复所需状态已经进入 WAL/timeline 协议，compute
就可以真正 ephemeral。

### 6.2 把日志位置变成系统级一致性坐标

Lakebase 的 LSN 不只是 recovery offset，而是：

- commit watermark；
- replica visibility；
- page version；
- branch point；
- PITR pin；
- cache invalidation range；
- external analytics snapshot。

数据库开发中常见的复杂性来自每个子系统发明自己的 epoch/version。若日志
已经给出全序位置，应优先验证它能否成为统一坐标，再增加额外版本体系。

### 6.3 “可丢弃计算”要求恢复工作已经被提前做掉

compute 启动快不是因为 PostgreSQL binary 启动快，而是：

- Pageserver 已持续 ingest 和 materialize WAL；
- basebackup 只包含启动 metadata；
- compute 从 Pageserver last-record LSN 开始，不做传统 checkpoint 后的长
  crash redo；
- warm pool 已经承担底层 VM provision 和 binary page fault。

这揭示了 serverless 的守恒关系：启动成本没有消失，而是从请求时转移到持续
运行的 storage service、background compaction 和 warm capacity。

### 6.4 数据库扩缩容首先是工作集问题

仅按 CPU 调整资源会出现两种失败：

- CPU 降下来了，但 cache 被缩小，随后 miss storm 使延迟暴涨；
- 内存看似空闲，但可回收 page cache 正承载热工作集，缩容后马上抖动。

因此 autoscaler 至少要同时观测：

- query load 与 CPU saturation；
- unique pages over time；
- cache hit/miss 和 eviction rate；
- anonymous allocation 与 kernel/page-cache memory；
- remote read latency；
- scale action 后的 refill cost。

`desired_size = max(cpu_need, memory_need, cache_need)` 是比单一利用率阈值更可靠
的基本框架。

### 6.5 给批量操作设计受约束的 storage bypass

大规模导入、索引构建和物理 schema change 经单个事务执行器串行化，往往浪费
分布式计算能力。Lakebase 的 Direct-to-Storage 提供了可复用模式：

1. 在隔离空间并行构造完整不可变结果；
2. 校验格式、checksum 和引用完整性；
3. 先持久化全部数据；
4. 由权威事务引擎提交一个很小的 metadata swap；
5. 失败时旧版本仍然可见，新结果作为 orphan 回收。

关键不是绕过数据库，而是只绕过数据搬运，并保留一个清晰、原子的控制点。

### 6.6 多引擎共享数据必须共享快照协议

共享对象存储只能解决“字节在哪里”，不能解决“哪些字节在当前事务可见”。
外部 reader 至少需要：

- snapshot/LSN；
- MVCC visibility；
- schema/catalog version；
- deleted/modified block tracking；
- cache invalidation；
- format version；
- authorization identity。

没有这些协议的“多引擎共享”只是并发读裸文件，无法提供数据库语义。

### 6.7 Backpressure 应按恢复债务触发

Pageserver 落后不仅影响当前读取，还累积三类债务：

- 需要保留更多 Safekeeper WAL；
- failover 后需要追赶更多日志；
- page miss 需要更长的 redo chain。

因此 backpressure 不应只看队列长度。应分别跟踪 ingest、local flush、
remote consistent 和 GC horizon 到 commit watermark 的距离，再按可用空间、
恢复时间预算和读延迟预算设阈值。

### 6.8 “开放格式”必须被当成长期 ABI

一旦允许外部 writer/reader 直接访问 PostgreSQL pages，内部布局不再只是实现
细节。开发团队必须承担：

- major-version format negotiation；
- extension 和自定义类型兼容；
- checksum 与 corruption policy；
- catalog/MVCC 语义版本化；
- reader capability discovery；
- rolling upgrade 期间双版本读取；
- malformed page 的隔离边界。

开放格式会减少平台锁定，但会显著提高演进纪律。

## 7. 何时值得采用类似架构

### 7.1 高匹配场景

- 大量数据库大部分时间空闲，但偶尔突发；
- preview、CI、agent sandbox 需要频繁创建隔离副本；
- 数据量大，完整复制不可接受；
- 同一份 operational data 同时服务 PostgreSQL 和分析引擎；
- 希望计算跨机型、跨集群甚至跨云迁移；
- 团队能够运营多租户日志服务、版本存储和复杂控制面。

### 7.2 低匹配或需要谨慎的场景

- 单个数据库长期满载，compute 很少缩到零；
- 极端稳定的亚毫秒本地读比弹性、分支和开放访问更重要；
- 工作集远大于可承担的 compute/Pageserver cache；
- 写入极重且无法接受 background materialization/compaction 债务；
- 依赖大量 PostgreSQL extension，而外部引擎必须正确理解其物理语义；
- 团队没有能力验证 quorum、fencing、timeline GC 和跨 AZ failover。

对这类场景，传统本地存储或第二代共享存储可能更简单。架构先进性不能抵消
运维能力不足。

## 8. 落地时应补齐的验证

若要基于本文思想开发数据库，至少应建立以下 E2E 测试矩阵：

| 类别 | 必测场景 |
| --- | --- |
| durability | quorum ACK 前后 kill primary；SK disk loss；对象存储延迟/失败；恢复后逐事务核对。 |
| fencing | 旧 primary 网络分区后恢复；新旧 term 并发写；确认旧写者在存储仲裁点被拒绝。 |
| page correctness | 随机 key/LSN 与 PostgreSQL recovery 结果逐页对比；覆盖 aborted transaction 和 hint bits。 |
| branch | 深 ancestry、detach、父分支 GC、PITR lease、branch 删除与 orphan cleanup。 |
| cache | warm/cold/mixed working set；compute cache 和 Pageserver cache 分别失效；测 p95/p99.9。 |
| backpressure | PS ingest、local flush、remote upload 分别落后；验证前台限流和恢复时间上界。 |
| autoscaling | CPU、anonymous memory、page cache 三种压力独立注入；扩缩容时观测延迟和 refill。 |
| external read | OLTP 并发更新时固定 LSN 扫描；DDL、VACUUM、TOAST、索引和 major-version 兼容。 |
| direct write | 上传中断、重复提交、metadata swap 失败、并发 DML、坏页和部分对象可见。 |
| cost | S3 request、跨 AZ、7x history、compaction、warm pool 和 cache refill 的完整账单。 |

这些测试必须运行真实 PostgreSQL、Safekeeper、Pageserver、对象存储和故障注入，
只测 page parser 或单组件 unit test 不能证明数据库级正确性。

## 9. 最终判断

Lakebase 最深的启发不是“PostgreSQL 也可以 serverless”，而是数据库身份已经
不必等于某个进程或某组磁盘：

```text
Database identity = timeline metadata
Durability        = quorum WAL plus archived history
Snapshot          = timeline and LSN
Serving           = replaceable compute and caches
Branch            = parent timeline and branch LSN
```

一旦这个模型成立，快速恢复、分支、PITR、read replica、scale-to-zero 和
多引擎访问不再是彼此独立的附加功能，而是同一个 versioned storage substrate
的不同投影。

论文的局限也同样清楚：低延迟严重依赖缓存和 warm pool；PITR 带来约 `7x`
生产空间放大；深分支会增加读成本；开放 PostgreSQL page 并不自动等于开放
数据库语义；评测对 cold miss、故障恢复、兼容性和完整成本仍不充分。

因此，对数据库开发最合理的采纳方式不是复制它的组件名称，而是复用四条原则：

1. 用最小同步日志路径保证提交，用异步物化路径构造可读状态。
2. 用统一时间坐标连接恢复、版本、分支、缓存和外部读者。
3. 只让可重建状态进入 compute，把唯一持久状态移出 compute 生命周期。
4. 让开放存储成为有版本、有快照、有权限的协议，而不只是可下载的文件。

## 参考与核对

- 论文 §3-§7：架构、存储分离、autoscaling、Lakehouse integration 和评测。
- [Neon 官方仓库](https://github.com/neondatabase/neon)：公开描述 stateless
  PostgreSQL compute、Pageserver 和 Safekeeper 的基本架构，用于判断哪些机制
  是 Lakebase 论文前已有基础。
- [Neon storage architecture](https://neon.com/storage)：公开描述对象存储、
  copy-on-write branching 和分层缓存，用于核对系统继承关系。
- [Databricks VLDB 2026 overview](https://www.databricks.com/blog/building-ai-era-lakebase-streaming-and-lakehouse-innovations-vldb-2026)：
  用于核对论文的公开定位；具体机制和数字仍以本地论文为准。
