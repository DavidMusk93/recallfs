# PostgreSQL DISTINCT 扩展性与分布式数据库启示

> 原文：[Postgres SELECT DISTINCT Does Not Scale](https://www.dbos.dev/blog/postgres-select-distinct-does-not-scale)
>
> 验证日期：2026-09-09。使用 Docker 中的 PostgreSQL 17.10 与 18.4，
> 对普通 `DISTINCT` 和递归公共表表达式（CTE）模拟的 loose index scan
> 做双轴实验。

## 1. 结论先行

这篇文章揭示的核心不是“`DISTINCT` 语义无法扩展”，而是：

> **输出基数很小，不代表访问代价很小。若存储访问方法只能逐项 `next()`，
> 上层 `Unique` 再聪明也必须消费全部 $N$ 个匹配项。**

对分布式数据库开发，最重要的启示有六条：

1. **把 distinct-prefix/loose scan 做成存储层原生能力。** 需要的是
   `seek(next distinct prefix)`，不是在结构化查询语言（SQL）层循环发起
   单 key 查询。
2. **分开优化本地扫描与全局去重。** 本地从 $N_s$ 降到 $D_s$ 后，仍需根据
   distribution property 决定直接 gather、hash exchange 或有序 merge。
3. **cost model 必须同时知道 $N$、$D$、每分片 $D_s$、跨分片重复度与
   seek/远程过程调用（RPC）成本。** 只有总行数或全局不同值数量（NDV）
   都不足以选计划。
4. **不要无条件选择 loose scan。** 本实验在 $N=10^6,\ D=10^4$ 时仍快
   1.82 倍；到 $D=10^5$ 时已慢 7.75 倍。应由代价模型或运行时反馈切换。
5. **分布键决定能否消掉 exchange。** 只有查询常量与 distinct key 能确定
   完整分布键时，每个结果 key 才只有一个 owner；否则仍需全局重分布。
6. **“枚举 active queue partition”更像控制面状态，不一定该反复从任务事实
   表推导。** 高频路径可维护事务一致的 active-partition 表，以写放大换取
   稳定的 $\mathcal{O}(D)$ 读取。

## 2. 术语与缩略词

| 术语 | 英文全称 | 本文含义 |
| --- | --- | --- |
| SQL | Structured Query Language | 结构化查询语言；本文的查询与 CTE 均使用 SQL 表达 |
| CTE | Common Table Expression | 公共表表达式；`WITH RECURSIVE` 允许查询递归引用自己的输出 |
| NDV | Number of Distinct Values | 不同值数量；本文记为 $D$，表示 distinct key 的基数 |
| RPC | Remote Procedure Call | 远程过程调用；分布式执行中一次跨节点请求及其往返开销 |
| MVCC | Multi-Version Concurrency Control | 多版本并发控制；用同一读时间戳获得一致快照 |
| JIT | Just-In-Time Compilation | 即时编译；实验中关闭，以隔离索引访问成本 |
| LSM-tree | Log-Structured Merge-tree | 日志结构合并树；常见于分布式数据库的底层有序存储 |
| B-tree | B-tree（不是缩略词） | 多路平衡搜索树；PostgreSQL 默认的有序索引结构之一 |
| VM | Virtual Machine | 虚拟机；本实验的 Docker daemon 运行在 Colima Linux VM 中 |
| vCPU | Virtual Central Processing Unit | 虚拟中央处理器；表示分配给 VM 的逻辑计算资源 |
| RAM | Random-Access Memory | 随机存取内存 |
| arm64 | 64-bit Arm architecture | 64 位 Arm 指令集架构；本实验宿主与 Linux VM 均使用该架构 |
| GiB | Gibibyte | 二进制容量单位，$1\ \mathrm{GiB}=2^{30}$ 字节 |
| ms | millisecond | 毫秒，$1\ \mathrm{ms}=10^{-3}$ 秒 |
| NaN | Not a Number | 浮点运算中的“非数”特殊值 |
| DBOS | 品牌名，不展开为缩写 | 原文作者团队及其持久化执行系统的名称；[官方文档](https://docs.dbos.dev/python/prompting) 明确说明 “DBOS does NOT stand for anything” |
| P0/P1/P2 | Priority 0/1/2 | 本文工程建议的优先级；数字越小，优先级越高 |
| `EXPLAIN ANALYZE` | PostgreSQL execution-plan command | 执行查询并返回实际算子、行数、耗时和缓冲区计数 |

本文还使用 loose index scan 表示“从当前有序 key 直接跳到下一个不同 key”
的访问方法；它不是缩略词，也不同于 PostgreSQL 18 的 multicolumn skip
scan。

## 3. 原文现象到底是什么

索引为：

```text
(queue_name, status, queue_partition_key)
```

查询固定前两个字段，返回第三个字段的唯一值。叶子项已经按 partition key
聚集，但 PostgreSQL 的普通计划仍是：

```text
Index Only Scan -> Unique -> Result
 matching rows   distinct rows   result rows
```

`Unique` 可以流式去掉相邻重复项，内存并不是问题；问题是下层扫描器仍把
每个匹配项交给它。文章的递归 CTE 改成：

```text
seek min(key)
seek min(key > previous)
...
stop on NULL
```

因此普通路径的工作接近 $\mathcal{O}(N)$，CTE 路径更准确地说接近
$\mathcal{O}(D\log N)$，而不是严格的 $\mathcal{O}(D)$。在 B-tree
高度稳定且内页驻留缓存时，它实测看起来近似只随 $D$ 变化。

PostgreSQL 18 的 skip scan 没有补上这个能力。它针对的是“前导索引列没有
等值条件、后续列有条件”的访问；这里前导列已有等值条件，缺的是“返回每个
有序组一次”的 loose index scan。

## 4. Docker 实测

### 4.1 环境与控制

- PostgreSQL：17.10、18.4 官方 Debian 镜像
- Docker：Colima Linux/arm64 VM，4 vCPU，6 GiB RAM
- `shared_buffers=1GB`，`work_mem=128MB`
- 禁用 parallel gather、JIT 和 sequential scan，隔离 B-tree 行为
- 每次装载后执行 `VACUUM (FREEZE, ANALYZE)`
- 每项先 warmup，再由 `pgbench` 单连接运行一次，包含 7 个事务
- 两条查询均返回 `count(*)`，避免客户端传输大量结果干扰
- 所有 case 均用双向 `EXCEPT` 比较两条查询，并分别对照独立生成的期望 key

完整数据见
[`evidence/benchmark.csv`](evidence/benchmark.csv) 和
[`evidence/plan-metrics.csv`](evidence/plan-metrics.csv)。

### 4.2 固定 $D=10$，增加 $N$

PostgreSQL 18.4：

| 每 partition 行数 | 总行数 $N$ | DISTINCT 平均延迟 | Loose CTE 平均延迟 | 前者/后者 |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 1,000 | 0.141 ms | 0.167 ms | 0.84x |
| 1,000 | 10,000 | 0.354 ms | 0.169 ms | 2.09x |
| 10,000 | 100,000 | 2.608 ms | 0.192 ms | 13.58x |
| 100,000 | 1,000,000 | 24.444 ms | 0.180 ms | 135.80x |
| 1,000,000 | 10,000,000 | 244.272 ms | 0.185 ms | 1,320.39x |

`DISTINCT` 随 $N$ 近似线性增长，CTE 在固定 $D$ 时基本保持不变。小表上
CTE 反而略慢，说明固定开销不可忽略。

最大 case 的 PostgreSQL 18 `EXPLAIN ANALYZE`：

| 指标 | DISTINCT | Loose CTE |
| --- | ---: | ---: |
| 匹配/产出的 index tuples | 10,000,000 | 10 |
| `Index Searches` | 1 | 11 |
| Heap fetches | 0 | 0 |
| Shared hit blocks | 8,713 | 36 |
| Instrumented execution | 614.915 ms | 0.135 ms |

普通路径不是“没用索引”，恰恰是用了最理想的 index-only scan 后仍把全部
索引项读完。PostgreSQL 17.10 的形状一致；其 $N=10^7$ case 的 `pgbench`
平均值为 255.515 ms 与 0.214 ms。

### 4.3 固定 $N=10^6$，增加 $D$

PostgreSQL 18.4：

| NDV $D$ | $D/N$ | DISTINCT 平均延迟 | Loose CTE 平均延迟 | 前者/后者 |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 0.001% | 24.444 ms | 0.180 ms | 135.80x |
| 100 | 0.01% | 25.242 ms | 0.326 ms | 77.43x |
| 1,000 | 0.1% | 25.438 ms | 1.742 ms | 14.60x |
| 10,000 | 1% | 25.663 ms | 14.107 ms | 1.82x |
| 100,000 | 10% | 28.007 ms | 217.115 ms | 0.13x |

普通路径主要由 $N$ 决定；loose 路径主要由 $D$ 决定。实际 crossover 取决于
树高、缓存命中、key 宽度、存储介质、并行度和远程访问成本，不能硬编码为
某个 $D/N$ 阈值。

## 5. 分布式执行应怎样拆

定义：

- $N_s$：shard $s$ 上谓词匹配的输入行数；
- $D_s$：shard $s$ 的局部 NDV；
- $D$：全局 NDV；
- $S$：参与 shard 数；
- $W$：一个 distinct key 序列化后的平均字节数。

下文以 $B$ 表示估算的网络字节量，以 $C$ 表示估算的本地执行工作量。

一个稳健的 exact distinct pipeline 是：

```text
+---------------------+
| Shard-local scan    |  matching rows
+----------+----------+
           |
           v
+---------------------+
| Local distinct      |  local keys
+----------+----------+
           |
           v
+---------------------+
| Exchange by key     |  shuffled local keys
+----------+----------+
           |
           v
+---------------------+
| Final distinct      |  global keys
+----------+----------+
           |
           v
        result
```

只有当查询中的常量列与 distinct key 能共同确定完整分布键时，每个结果 key
才只有一个 owner，此时 `Exchange by key` 可以消掉。例如按
`hash(queue_name, partition_key)` 分布且查询固定 `queue_name` 时，这一条件
成立；任意“分布键前缀”本身并不充分。若各 shard 的 key 高度重叠，则局部
去重后的网络字节数近似为

$$
B_{\mathrm{local}} \approx W\sum_{s=1}^{S}D_s.
$$

跨 shard 高度重叠时，最坏可接近

$$
B_{\mathrm{overlap}} \approx WSD,
$$

但仍可能远小于不做局部去重时传输原始输入的

$$
B_{\mathrm{raw}} \approx W\sum_{s=1}^{S}N_s.
$$

本地访问方法则有两种候选：

$$
C_{\mathrm{full}}
\approx
\sum_{s=1}^{S}N_s\,c_{\mathrm{next}},
$$

$$
C_{\mathrm{loose}}
\approx
\sum_{s=1}^{S}(D_s+1)\,c_{\mathrm{seek}}(N_s).
$$

其中，$c_{\mathrm{next}}$ 表示顺序读取一个索引项的成本，
$c_{\mathrm{seek}}(N_s)$ 表示在 shard $s$ 的索引中重新定位一次的成本。

分布式 wall time 还要加入最慢 shard、exchange、spill 和 coordinator
merge。若一个 SQL 递归步骤对应一次远程 RPC，公式会多出
$D_s\,L_{\mathrm{RPC}}$，其中 $L_{\mathrm{RPC}}$ 是一次远程调用的往返延迟；
这一项足以吞掉所有收益。因此 loose scan 必须在 tablet / range / storage
node 内部循环，并按批次返回结果和 continuation token。

## 6. 对内核设计的具体要求

### 6.1 存储与执行器

建议提供原生 `DistinctPrefixScan` 或等价能力：

- 输入：固定前缀、distinct key 列、边界、方向、snapshot timestamp；
- 行为：返回当前 key 后直接 seek 到该 key 的 successor；
- 输出：批量 key、continuation token、扫描/seek 统计；
- 实现：B-tree 使用重新定位，LSM-tree 使用 iterator seek，避免 coordinator
  每 key 一次 RPC；
- 回退：运行时发现 $D$ 高于估计时，可切到连续扫描并流式 unique。

这个算子应保持有序输出。上层可利用顺序做 k-way merge，避免 hash table；
也可在需要重新分布时直接按 key hash exchange。

### 6.2 优化器与统计信息

计划选择至少需要：

| 统计/成本 | 原因 |
| --- | --- |
| 谓词后的 $N$ | 决定完整扫描工作 |
| 谓词后的 conditional NDV $D$ | 决定 seek 次数与结果下界 |
| 每 shard 的 $D_s$ | 决定局部工作与网络输出 |
| $\frac{\sum_{s=1}^{S}D_s}{D}$ | 衡量跨 shard 重复度 |
| run-length / skew | 识别少数超深 key 和慢 shard |
| 顺序读、随机 seek、远程 RPC 单价 | 找真实 crossover |
| key 宽度与内存预算 | 估计 exchange、hash state 和 spill |

只收集全表单列 NDV 不够。应有条件 NDV、组合列统计、分片级统计和运行时
反馈。文章中的条件基数

$$
\operatorname{NDV}(\texttt{partition\_key}\mid\texttt{queue\_name},\texttt{status})
$$

正是多列相关性问题。

### 6.3 Distribution property

对 queue workload，分布键选择存在明确取舍：

| 分布方式 | active partition 查询 | 写入扩展性 |
| --- | --- | --- |
| `hash(queue_name)` | 单 shard，无 exchange | 深队列容易成为热点 |
| `hash(queue_name, partition_key)` | fan-out，但 key 有唯一 owner | partition 可水平扩展 |
| 与 partition 无关 | fan-out + global distinct | 写均衡但读代价最高 |

优化器必须把“distinct key 是否由分布属性唯一拥有”作为物理属性传播，不能
只在 logical aggregate 阶段处理。

### 6.4 正确性合同

原生实现还必须覆盖：

- 所有 shard seek 使用同一 MVCC read timestamp；
- range split、lease transfer 和 retry 后 continuation 不重不漏；
- `NULL` 只形成一个 distinct group；本案例显式排除了它；
- collation、NaN 和类型 equality 在所有节点一致；
- hash exchange 的 hash/equality 版本一致；
- spill、重试和 speculative execution 可重复输入，但 final distinct 不能
  接受缺失输入。

递归 CTE 在单条 PostgreSQL statement 内共享 snapshot。把相同逻辑拆成
应用层多条查询，会失去这个保证。

## 7. 对 queue 数据模型的进一步启示

DBOS 的查询不是任意 ad-hoc 分析，而是在 dequeue 热路径上发现 active
partition。更直接的模型是维护：

```text
active_partition(queue_name, partition_key, enqueued_count, lease_state)
```

enqueue/dequeue/status transition 在同一事务中更新计数；$0\to1$ 插入
active key，$1\to0$ 删除或标记 inactive。读取从事实表的
$\mathcal{O}(N)$ 推导变成 active set 的 $\mathcal{O}(D)$ 扫描。

代价是写放大与并发正确性，因此需要：

- 原子计数或幂等状态转移；
- 防止重复完成导致负计数；
- 崩溃后的对账/重建路径；
- 与 workflow 状态相同的事务边界；
- 明确 lease 与“存在 ENQUEUED row”是否同一语义。

若读频率高、队列很深，这通常比持续依赖查询优化更稳；若写极密集或 active
集合很少读取，原生 loose scan 更合适。

## 8. 工程优先级

1. **P0：可观测性。** `EXPLAIN ANALYZE` 暴露 input rows、output rows、
   storage seeks、RPC、exchange bytes、spill bytes 和 per-shard skew。
2. **P0：回归矩阵。** 同时扫描 $N$、$D/N$、shard 数、分布对齐、冷热缓存、
   skew 和并发 snapshot；只增大总行数会漏掉 crossover。
3. **P1：local distinct pushdown + distribution-aware final distinct。**
4. **P1：storage-local loose scan。** 不把逐 key 循环放在 coordinator。
5. **P2：adaptive fallback。** 实际 NDV 或 seek 延迟偏离估计时切连续扫描。
6. **P2：为固定业务语义评估 active-set 物化。**

`HyperLogLog` 只适合 `COUNT(DISTINCT)` 等允许误差的计数，不能替代 dequeue
所需的精确 partition key 枚举。

## 9. 复现

```bash
cd learning/studies/20260909-postgres-distinct-scale/demo
./scripts/run.sh
```

完整命令、可调参数和证据文件说明见
[`demo/README.md`](demo/README.md)。探索过程与失败记录见
[`exploration.md`](exploration.md)，来源边界见 [`source.md`](source.md)。
