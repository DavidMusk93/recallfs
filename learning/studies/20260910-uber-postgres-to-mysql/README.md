---
doc_id: study-20260910-uber-postgres-to-mysql
kind: study
status: active
authority: analytical
applies_to:
  - database-engine-selection
  - postgres-mysql-migration
depends_on: []
supersedes: []
verified_by:
  - uber-2016-source-review
  - postgresql-18-documentation-review
  - mysql-8.4-documentation-review
---

# Uber 为什么从 PostgreSQL 迁到 MySQL

> 汇报主题：很多人都在推崇 PostgreSQL，为什么企业仍会选择 MySQL？
>
> 核心案例：Uber Engineering, [Why Uber Engineering Switched from
> Postgres to MySQL](https://www.uber.com/in/en/blog/postgres-to-mysql-migration/),
> 2016-07-26。
>
> 核验日期：2026-09-10。本文将 2016 年 PostgreSQL 9.2 经验与
> PostgreSQL 18、MySQL 8.4 官方文档对照，不把历史结论直接套到今天。

## 1. 一页结论

### 1.1 先纠正问题

Uber 的案例不是“从先进的 PostgreSQL 回退到落后的 MySQL”，而是：

1. 早期系统是 Python 单体应用加 PostgreSQL。
2. 随着业务增长，Uber 转向微服务和自研分布式存储平台。
3. 被替代的不是单独一个数据库进程，而是原有的数据访问和扩展方式。
4. 新主路径是 `Schemaless API + 自研分片层 + MySQL/InnoDB`，另有 Cassandra
   等专用存储；PostgreSQL 的部分实例仍然保留。

```text
Early

Application monolith
        |
        v
   PostgreSQL

Later

Microservices
        |
        v
Schemaless storage API
        |
        v
Sharding and replication control
        |
        v
 MySQL / InnoDB shards
```

因此，这次迁移的正确描述是：

> **Uber 用一个受控、窄接口、自带分片能力的数据平台，替换了难以继续水平扩展的
> PostgreSQL 9.2 部署；MySQL 是该平台选择的存储引擎，不是全部架构。**

### 1.2 为什么选 MySQL

Uber 当时最在意的是高频更新、很多二级索引、跨数据中心复制、只读副本行为、
在线升级和大量连接。InnoDB 的聚簇主键、二级索引指向主键、undo MVCC 和逻辑
binlog，更符合它的访问模型与运维目标。

这不证明 MySQL 普遍优于 PostgreSQL，只证明数据库选型是多维局部最优：

> **企业购买的不是“功能最多的数据库”，而是在特定 workload、组织能力和故障
> 模型下，可预测的总拥有成本。**

### 1.3 对原文的最终评级

| 维度 | 评级 | 判断 |
| --- | --- | --- |
| 历史价值 | 高 | 真实揭示了存储布局、MVCC、复制和升级如何变成规模瓶颈 |
| 技术方向 | 大体成立 | 多数矛盾来自真实的架构差异，而非参数没调好 |
| 论证完整性 | 中 | 没有公开 A/B benchmark、WAL/binlog 字节量或迁移成本 |
| 今天的适用性 | 有限 | 文章基于 PostgreSQL 9.2，逻辑复制和升级能力已明显变化 |
| 能否作为选型结论 | 不能 | 只能转化为待验证假设，不能直接推出“应该选 MySQL” |

## 2. Uber 当时真正遇到了什么

原文列出五类 PostgreSQL 问题：

1. 写入架构低效；
2. 复制低效；
3. 一次 PostgreSQL 9.2 timeline switch 缺陷导致副本数据损坏；
4. 物理只读副本上的查询与 WAL replay 冲突；
5. 跨大版本升级困难。

此外，文章还认为 PostgreSQL 的缓存路径和进程模型不如 InnoDB buffer pool
与 MySQL thread-per-connection 适合其部署。

这些问题不是互相独立的，核心因果链是：

```text
Heap tuple gets a new physical location
                |
                v
Affected indexes need new tuple references
                |
                v
More heap, index, and WAL writes
                |
                v
More replication traffic and cleanup work
                |
                v
Cross-region lag and higher operating risk
```

### 2.1 写放大：真实，但原文没有讲完整

PostgreSQL 的索引记录通常指向 heap tuple 的物理位置 `ctid`。普通 `UPDATE`
会产生新 tuple；当一次更新不能使用 HOT 时，即使某些索引列的值没变，也需要为
新 tuple 建立索引项。

Uber 的示例更新了已被索引的 `birth_year`，所以该更新不能使用 HOT。表上索引
越多，额外维护越重。这个核心矛盾今天仍然存在。

但原文没有充分讨论 PostgreSQL 8.3 就已引入、PostgreSQL 9.2 已经具备的
Heap-Only Tuple（HOT）优化。满足以下条件时，HOT 不需要创建新索引项：

- 更新没有修改普通索引引用的列；
- 同一 heap page 有空间容纳新 tuple。

因此，“PostgreSQL 每次更新都重写所有索引”是错误概括。更准确的说法是：

> **修改任一普通索引涉及的列，或新版本无法留在原 page 时，HOT 失效；
> 此时多索引、高更新率表会承受明显写放大。**

InnoDB 的二级索引记录保存主键值，而不是 heap 物理地址。非索引列更新通常不需要
修改二级索引；代价则转移为：

- 二级索引回表需要再查一次聚簇主键；
- 长主键会复制进每个二级索引，占用更多空间；
- 更新主键代价高；
- InnoDB 仍然要维护 redo、undo 和 binlog，并非没有写放大。

两者是在不同位置支付成本，不是一个“有成本”、另一个“零成本”。

### 2.2 复制：Uber 需要更小、更可控的跨地域变更流

PostgreSQL 9.2 的主流高可用路径是物理 WAL 流复制。heap 和索引产生的物理
变化都会进入 WAL。Uber 的跨美国东西海岸链路和 WAL 归档曾跟不上变更速度，
因此数据库内部写放大继续放大成网络与恢复压力。

MySQL binlog 可以记录 statement、row 或 mixed 格式；MySQL 8.4 默认 row
格式。副本依据逻辑行变更维护自身索引，不需要传输主库每个页级变化。这是 Uber
偏好 MySQL 的关键原因之一。

但原文“binlog 显著小于 WAL”的判断没有给出测量数据，不能直接泛化：

- statement 格式可能很小，但有确定性和兼容性约束；
- row 格式大小取决于变更行数、row image、事务形状和压缩；
- PostgreSQL WAL 大小取决于 checkpoint、full-page write、HOT 命中率、
  索引数量、WAL compression 和 workload；
- MySQL 同时写 redo、undo、binlog，端到端成本不能只看复制流。

正确的选型指标不是“物理一定大、逻辑一定小”，而是同一业务事务在目标配置下的
`bytes/commit`、复制延迟、恢复点目标和故障恢复时间。

### 2.3 只读副本：冲突是物理复制的结构性权衡

PostgreSQL 物理 standby 重放主库已提交的 WAL。当 standby 上的长查询仍需要
某个旧行版本，而 WAL 要清理该版本时，只能在两件事中选择：

1. 延迟 WAL replay，让副本继续变旧；
2. 取消冲突查询，让副本追上主库。

PostgreSQL 18 仍明确记录这一行为。`max_standby_streaming_delay` 控制等待上限；
`hot_standby_feedback` 可以减少清理冲突，但会把代价转成主库 dead tuple
保留和表膨胀。

所以 Uber 这一观察仍成立，但应限定为“物理 standby 的查询冲突”，不能写成
“PostgreSQL 没有 MVCC”。主库有完整 MVCC，逻辑订阅端也不以物理恢复模式运行。

### 2.4 大版本升级：2016 年的痛点已明显缓解

Uber 的 PostgreSQL 9.1 到 9.2 升级需要停主、运行 `pg_upgrade`、重建快照并
逐个恢复副本。数据量继续增长后，团队认为无法再承受同一路径。

今天需要分开看：

- 物理流复制仍要求主从的大版本兼容，约束没有消失；
- PostgreSQL 10 起提供内置逻辑复制，PostgreSQL 18 官方列出的用途包括跨大
  版本复制；
- `pg_upgrade` 支持 hard link、reflink clone、`copy_file_range` 和 swap，
  可避免完整数据复制或显著缩短文件迁移时间；
- 扩展兼容、catalog 变化、回滚方案、统计信息恢复和应用验证仍然使大版本升级
  成为工程项目。

结论不是“升级问题已解决”，而是 Uber 当年的停机路径不再是今天唯一的路径。

### 2.5 数据损坏：事故证据不能升级成架构定理

Uber 遇到的是特定 PostgreSQL 9.2 版本中已修复的 timeline switch 缺陷。
这个事故足以改变一个团队的风险偏好，也解释了其迁移动机。

但原文进一步推导“逻辑复制不太可能造成灾难性损坏”时，证据不足：

- 物理复制确实可能传播底层错误或损坏；
- 逻辑复制隔离了部分物理布局问题；
- 逻辑事件错误、主键错误、应用误写和复制实现缺陷仍可能传播；
- 原文没有给出两个引擎同类故障的发生率和恢复时间对比。

对选型有价值的问题不是“谁永不损坏”，而是能否检测、隔离、重建和证明恢复后的
一致性。

### 2.6 缓存和连接模型：事实存在，权重取决于平台

PostgreSQL 使用共享缓存并大量依赖操作系统 page cache，连接采用进程模型；
MySQL/InnoDB 使用更集中的用户态 buffer pool，连接采用线程模型。这些差异不是
虚构的。

但原文把它们直接推导成 MySQL 显著更快，缺少同机 benchmark。现实系统还会加入
PgBouncer、ProxySQL、连接池、容器限额和托管数据库代理。对于大量短连接，
“是否强制经过池化”往往比“进程还是线程”更能决定事故率。

## 3. 哪些结论在 2026 年仍然成立

| Uber 2016 论点 | 2026 判断 | 原因 |
| --- | --- | --- |
| PostgreSQL heap MVCC 会产生新 tuple | 仍成立 | 基础存储模型未改变 |
| 更新会维护所有索引 | 有条件成立 | 非 HOT 更新成立；原文漏掉当时已有的 HOT |
| 多索引更新会放大 WAL | 仍成立 | 具体幅度必须实测 |
| 物理 standby 会取消冲突查询或落后 | 仍成立 | PostgreSQL 18 文档仍明确描述 |
| PostgreSQL 无法逻辑复制 | 已过时 | PostgreSQL 10 起内置逻辑复制 |
| 跨大版本只能长停机升级 | 已过时 | 现有逻辑复制与多种 `pg_upgrade` 快速模式 |
| MySQL 二级索引保存主键 | 仍成立 | MySQL 8.4 InnoDB 文档确认 |
| MySQL row binlog 记录逻辑行变更 | 仍成立 | MySQL 8.4 默认 row 格式 |
| MySQL 复制流一定显著更小 | 未证明 | 原文没有数据，结果依赖 workload 与配置 |
| 一次 PostgreSQL 损坏说明其普遍不可靠 | 不成立 | 特定旧版本事故不能代表总体故障率 |
| MySQL 在 Uber 场景更合适 | 历史上成立 | 其自研平台、访问模型和团队能力共同决定 |
| 其他公司应该照搬 | 不成立 | 缺少 workload、SLO、团队和迁移成本同构条件 |

## 4. 为什么“很多人推崇 PostgreSQL”和“企业选 MySQL”不矛盾

### 4.1 双方优化的目标函数不同

开发者推崇 PostgreSQL，通常在评价：

- SQL 表达能力与标准一致性；
- 复杂查询、窗口函数、CTE 和优化器能力；
- JSONB、PostGIS、全文检索和扩展生态；
- 类型系统、约束、事务语义与数据正确性；
- 一个数据库覆盖更多业务模型的能力。

大型平台团队选择 MySQL，可能在评价：

- 已标准化的分库分表和故障切换体系；
- 大量简单 point read/write 的成本稳定性；
- InnoDB 聚簇索引是否匹配主键访问；
- 团队已有的 DBA、代理、备份和容量治理能力；
- 跨地域复制流量、在线升级和多年故障经验；
- 是否已有 Vitess、自研中间层或云厂商能力。

前者更关注“数据库能做什么”，后者更关注“在本组织里十年稳定地做这件事要付出
什么”。

### 4.2 PostgreSQL 的优势可能被上层平台主动屏蔽

Schemaless 提供的是受限数据模型和平台 API。应用不直接依赖大量复杂 SQL、
扩展、外键和 ad hoc 查询时，PostgreSQL 的功能优势无法充分兑现；底层写放大、
复制和维护成本却仍然存在。

反过来，如果业务依赖复杂事务、关系约束、PostGIS、JSONB、多种索引和临时分析，
把它压成窄 KV 接口会把复杂度转移到应用与数据平台。此时 MySQL 底层的某项优势
未必能覆盖重建上层能力的成本。

### 4.3 企业通常选择“已有能力”，不是理论最优

数据库是带有强路径依赖的组织系统。真实决策还包括：

- 已有运维人员和 on-call 经验；
- 监控、备份、恢复、审计和变更平台；
- 云厂商支持与采购关系；
- 历史 schema、ORM 和数据迁移成本；
- 故障时团队更熟悉哪种失败方式。

因此，一家拥有成熟 MySQL 平台的公司继续选 MySQL，可能比引入功能更强但无人
能稳定运营的 PostgreSQL 更理性。

### 4.4 技术舆论有明显抽样偏差

公开讨论更容易展示新功能、优雅 SQL 和开发体验；企业数据库决策中的升级演练、
值班负担、跨地域带宽、恢复时长和人员供给不容易成为热门内容。

所以“社区声量”与“企业存量”本来就不是同一个指标。流行观点可以说明产品体验，
不能代替生产约束。

## 5. 选型时应该怎样判断

### 5.1 场景倾向

| 场景 | 更应优先验证 PostgreSQL | 更应优先验证 MySQL/InnoDB |
| --- | --- | --- |
| 查询形态 | 复杂 join、窗口、地理、JSONB、ad hoc | 主键/二级键点查，固定短事务 |
| 数据正确性 | 强类型、复杂约束、扩展能力重要 | 数据模型由上层平台严格收窄 |
| 更新模式 | 索引少、HOT 命中率高或写入非瓶颈 | 高频更新且表上二级索引很多 |
| 扩展路径 | 单机纵向扩展、读副本或 PG 生态分布式方案 | 已有成熟 MySQL 分片/Vitess 平台 |
| 组织能力 | PG 运维、扩展和升级经验成熟 | MySQL DBA、代理和恢复体系成熟 |
| 长查询 | 主库或独立分析副本可承载 | 希望复制副本与 OLTP 读负载解耦 |

这张表只决定“先验证谁”，不能代替实测。

### 5.2 高写入 OLTP 的最低验证门禁

不要做空表 `sysbench` 后宣布胜负。至少使用真实 schema、索引数、行宽、事务
组合和数据分布，测量：

1. 业务吞吐及 p50、p95、p99、p999 延迟；
2. 每个业务事务产生的 heap/data、index、redo/WAL、undo 和 binlog 字节；
3. HOT 命中率、dead tuple、purge/vacuum backlog 与索引膨胀；
4. 同城与跨地域副本的 apply lag、网络字节和追赶时间；
5. 长查询对复制 replay 或 purge 的影响；
6. checkpoint、备份、恢复和主从切换时的尾延迟；
7. schema change 与大版本升级的停机时间、回滚时间和人工步骤；
8. 故障注入后的 RPO、RTO 和数据一致性验证；
9. 三年容量、机器、网络、托管服务和人力总成本。

### 5.3 迁移决策公式

只有下面的不等式在有证据时成立，迁移才有经济意义：

```text
Expected benefit over evaluation horizon
>
Migration cost
+ dual-run cost
+ correctness risk
+ operational relearning
+ rollback reserve
```

一个旧版本事故或一篇热门文章，都不能单独让这个不等式成立。

## 6. 给决策者的建议

### 新系统

- 若组织没有既定平台，且业务需要复杂关系能力，PostgreSQL 是合理默认项。
- 若组织已有成熟 MySQL 平台，普通 OLTP 服务沿用 MySQL 通常比追逐社区偏好更
  便宜。
- 若目标是超大规模、窄接口、高写入数据平台，应先决定分片、一致性、复制和恢复
  模型，再选择底层引擎。

### 已有 PostgreSQL 系统

不要因为 Uber 2016 文章迁移。先确认：

- 是否真的受 non-HOT update、WAL 带宽或 standby conflict 限制；
- 索引是否过量，`fillfactor` 和 autovacuum 是否符合更新模式；
- 逻辑复制、连接池、分区或扩展是否能在原引擎内解决；
- 引擎迁移是否比架构治理更便宜。

### 已有 MySQL 系统

也不要因为 PostgreSQL 社区声量迁移。只有在复杂 SQL、数据类型、扩展、约束或
特定性能问题形成可量化缺口时，才值得承担迁移成本。

## 7. 汇报口径

可以用下面三句话结束汇报：

1. **Uber 不是从 PostgreSQL “回退”到 MySQL，而是从单体数据库升级为自研
   分布式数据平台，MySQL 恰好更适合其底层 workload。**
2. **文章指出的 heap MVCC、物理复制冲突等架构权衡仍有价值，但逻辑复制、升级
   能力等结论已经过时，且原文缺少定量 A/B 证据。**
3. **PostgreSQL 的功能上限更高，不等于每个企业场景的总成本更低；数据库选型
   必须由真实 workload、SLO、故障模型和组织能力共同决定。**

## 8. 证据账本

| ID | 证据 | 支持的结论 | 强度与限制 |
| --- | --- | --- | --- |
| E1 | Uber 2016 原文 | 迁移背景、PG 9.2 痛点、Schemaless/MySQL 选择 | 一手经验；无公开 A/B 数据 |
| E2 | PostgreSQL 8.3 release notes | HOT 在 2008 年引入，PG 9.2 已具备 | 官方历史文档 |
| E3 | PostgreSQL 18 HOT 文档 | HOT 条件、收益与限制 | 官方当前文档 |
| E4 | PostgreSQL 10 release notes 与 PostgreSQL 18 logical replication 文档 | 10 引入内置逻辑复制；当前支持跨大版本复制 | 官方历史与当前文档 |
| E5 | PostgreSQL 18 hot standby 文档 | replay 冲突、取消、延迟及 feedback/bloat 权衡 | 官方当前文档 |
| E6 | PostgreSQL 18 `pg_upgrade` 文档 | link、clone、copy-file-range、swap 等升级路径 | 官方当前文档；不是零风险承诺 |
| E7 | MySQL 8.4 InnoDB index 文档 | 聚簇主键与二级索引保存主键 | 官方当前文档 |
| E8 | MySQL 8.4 replication format 文档 | statement、row、mixed，row 为默认 | 官方当前文档 |

本文没有独立复现 Uber 的生产 workload，也没有证据判断 Uber 2026 年全部存储
现状。凡涉及“性能更高”“复制更小”“成本更低”的结论，都应视为待真实 workload
验证的假设。

## 9. 参考资料

1. Uber Engineering, “Why Uber Engineering Switched from Postgres to
   MySQL,” 2016-07-26:
   https://www.uber.com/in/en/blog/postgres-to-mysql-migration/
2. PostgreSQL 8.3 release notes, HOT introduction:
   https://www.postgresql.org/docs/9.2/release-8-3.html
3. PostgreSQL 18, “Heap-Only Tuples (HOT)”:
   https://www.postgresql.org/docs/18/storage-hot.html
4. PostgreSQL 10 release notes, built-in logical replication:
   https://www.postgresql.org/docs/10/release-10.html
5. PostgreSQL 18, “Logical Replication”:
   https://www.postgresql.org/docs/18/logical-replication.html
6. PostgreSQL 18, “Hot Standby: Handling Query Conflicts”:
   https://www.postgresql.org/docs/18/hot-standby.html#HOT-STANDBY-CONFLICT
7. PostgreSQL 18, `pg_upgrade`:
   https://www.postgresql.org/docs/18/pgupgrade.html
8. MySQL 8.4, “Clustered and Secondary Indexes”:
   https://dev.mysql.com/doc/refman/8.4/en/innodb-index-types.html
9. MySQL 8.4, “Replication Formats”:
   https://dev.mysql.com/doc/refman/8.4/en/replication-formats.html
