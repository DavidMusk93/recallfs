# Dict2 Adaptive Pushdown Plan

## 1. 背景

当前 `dict2` 的第一阶段优化已经能让部分 filter 重新进入 pushdown，例如：

```sql
account_id = '2100402624'
```

可以被改写为：

```sql
account_id_dict_idx = 33
```

并进入 scan pushdown。

但真实 e2e 已经说明：

> “能下推”不等于“有收益”。

某些查询在 `vqos_dict` 上反而比 `vqos_nodict` 更慢，原因并不总是 decode 本身，而是：

1. 过多 filter 被下推后，scan / storage 执行路径发生变化
2. 多个 `dict_execution` 谓词一起参与 pushdown，会带来额外代价
3. 有些 filter 虽然理论上可下推，但收益远低于预期

因此需要一个新的能力：

> 对可下推 filter 做“收益优先级排序”，只保留最值得下推的部分，其余保持为 `dict_execution` 路径。

## 2. 设计目标

### 2.1 核心目标

针对 `AND` 条件链：

```sql
f1 AND f2 AND f3 AND ...
```

引入一套**简单、可解释、可扩展**的 adaptive pushdown 机制：

1. 先拆分出 `and-exprs`
2. 对每个 expr 打两类分：
   - `pushdownScore`
   - `executionOrderScore`
3. 按 `pushdownScore` 选择哪些 expr 进入 pushdown
4. 按 `executionOrderScore` 重排 AND 链顺序
5. 只允许有限数量的高分 `filter-literal-pushdown` 进入 scan
6. 未进入 top-K 的字典相关候选保持为 `dict_execution` 路径

### 2.2 非目标

本次设计不做：

1. 基于统计信息的精确成本模型
2. OR / NOT 全面重写
3. 任意复杂布尔表达式的全局最优搜索
4. 多轮自适应反馈调优

第一版只追求：

1. 规则简单
2. 语义稳定
3. 易于线上观察
4. 可逐步扩展

## 3. 典型问题 Case

### 3.1 Case SQL

```sql
select stream as `stream_name`,
       sum(
         case
           when (is_sla_tag = 'true')
             and (session_type = 'sink')
             and (is_relay = 'false')
             and (local_node_type = 'pull')
             and remote_type = 'user'
           then "count"
         end
       ) as `nvqos_metrics_pull_req_count`,
       toStartOfInterval(toDateTime(ts), interval 1 minute, 'Asia/Shanghai') as nvqos_timestamp
from ti.vqos_dict
where ts >= '2026-04-08 08:00:00 +08:00'
  and ts < '2026-04-09 08:00:00 +08:00'
  and session_type = 'sink'
  and local_node_type = 'pull'
  and is_relay = 'false'
  and remote_type = 'user'
  and format not in ('webrtc','webrtc-rs')
  and response_status_code = '404'
  and volcano_account_id = '2100003528'
group by stream_name, nvqos_timestamp
order by `nvqos_metrics_pull_req_count` DESC
limit 2
```

### 3.2 问题

这类 SQL 往往存在多个可下推的 dict filter：

1. `session_type = 'sink'`
2. `local_node_type = 'pull'`
3. `is_relay = 'false'`
4. `remote_type = 'user'`
5. `response_status_code = '404'`
6. `volcano_account_id = '2100003528'`

如果全部 pushdown：

1. 不一定比只下推时间过滤更快
2. 不一定比只下推其中几个高收益谓词更快

所以要把问题收敛成：

> 哪些 filter 最值得 pushdown？

## 4. 核心思路

### 4.1 AND 拆分

对 `Filter.condition`：

```sql
f1 AND f2 AND f3 AND f4
```

先拆分成：

```scala
Seq(f1, f2, f3, f4)
```

本质上复用现有 `SplitConjunctivePredicates` 思想。

### 4.2 单 expr 打分

每个 expr 不再只有一个分数，而是有两套分数：

1. `pushdownScore`
   - 用于决定“值不值得下推”
2. `executionOrderScore`
   - 用于决定“AND 链里谁先执行”

建议抽象：

```scala
case class PushdownScore(
    expr: Expression,
    pushdownScore: Int,
    executionOrderScore: Int,
    reason: Seq[String],
    kind: PushdownKind)
```

其中：

```scala
sealed trait PushdownKind
case object LiteralIndexPushdown extends PushdownKind
case object DictExecutionPushdown extends PushdownKind
case object NativePushdown extends PushdownKind
case object NonPushdown extends PushdownKind
```

### 4.3 两种排序

#### A. Pushdown 选择排序

按 `pushdownScore` 降序排序：

```scala
scoredExprs.sortBy(e => (-e.pushdownScore, stableOrder))
```

用于选出最值得下推的 top-K。

#### B. AND 链执行顺序排序

按 `executionOrderScore` 升序排序：

```scala
scoredExprs.sortBy(e => (e.executionOrderScore, stableOrder))
```

语义是：

```text
easy / cheap / selective 的 expr 放前面
hard / expensive 的 expr 放后面
```

稳定排序保证：

1. 分数相同的 expr 仍按原始书写顺序
2. 行为可预测，便于调试

### 4.4 限定 pushdown 数量

核心约束：

> 最多只允许前 `K` 个高分 `filter-literal-pushdown` 进入 pushdown。

建议第一版：

```text
K = 3
```

且只限制：

```text
LiteralIndexPushdown / DictExecutionPushdown
```

不限制：

1. 时间范围过滤
2. 分区过滤
3. scan 原生就很稳定的 native pushdown

## 5. 为什么只限制 filter-literal-pushdown 数量

因为当前主要问题不是所有 pushdown 都有害，而是：

1. 某些 `dict_execution` pushdown 会显著影响 scan 路径
2. 某些 `literal-index` pushdown 可能收益不高，甚至稀释整体收益

相比之下：

1. `ts >= ...`
2. `ts < ...`
3. 分区列过滤

往往是“低风险、高收益”的 pushdown。

需要额外强调的是：

> `timestamp` 或主键相关 filter 应优先进入 pushdown 候选前列。

原因是这类条件通常对查询路径影响最大：

1. `timestamp` 范围过滤往往直接决定 scan 范围
2. 主键或高选择性 key 过滤往往直接决定数据收缩程度
3. 这两类条件的前置与否，对整体查询耗时影响往往远高于普通低选择性字典条件

所以第一版策略应足够聚焦：

> 只对“字典相关 pushdown”做数量限制，不要把所有 filter 一锅端。

## 6. AND 链重排

### 6.1 为什么必须补 AND 链重排

仅仅决定“哪些 filter 进入 pushdown”还不够。

对于保留在上层 `Filter` / `dict_execution` 中的谓词，执行顺序本身也会明显影响性能。

典型例子：

```sql
col2 LIKE '%abc%' AND col1 = 123
```

应重排为：

```sql
col1 = 123 AND col2 LIKE '%abc%'
```

原因很直接：

1. `col1 = 123` 更简单
2. `col1 = 123` 通常也更容易先过滤掉大部分行
3. `LIKE '%abc%'` 往往更昂贵
4. 如果把便宜条件放前面，后续昂贵条件需要处理的 row 数会更少

### 6.2 从 Velox 执行模型看为什么这样做

Velox 的 `FilterNode` / 表达式执行本质上仍然遵循一个基本原则：

> 对 AND 条件，前面的谓词越早把 row / row-set 过滤掉，后面的昂贵表达式就越少有机会执行。

即使是向量化执行，这个原则依然成立：

1. 前置简单谓词可以先收缩 active rows
2. 后续复杂函数、字符串匹配、`dict_execution` 只需要在剩余行上继续求值
3. 越昂贵的表达式越应该放在后面

因此，在自适应 pushdown 里，除了“选哪些进 pushdown”，还需要补一个非常简单的执行策略：

> AND 链内部，`easy` 放前面，`hard` 放后面。

### 6.3 executionOrderScore 的设计

建议把 `executionOrderScore` 设计为：

```text
分数越低，越靠前执行
分数越高，越靠后执行
```

第一版规则可直接写死为：

1. `timestamp` / 主键相关过滤：

```text
executionOrderScore = 0
```

说明：

1. `ts >= ...`, `ts < ...`
2. 主键列或高选择性 key 的 `EqualTo`

这类条件应该最前置，因为它们最可能先缩小 active rows。

2. 原生简单等值 / 范围过滤：

```text
executionOrderScore = 10
```

3. `LiteralIndexPushdown` 命中的简单编码等值：

```text
executionOrderScore = 20
```

4. 普通 `DictExecutionPushdown` 等值 / `IN`：

```text
executionOrderScore = 40
```

5. 字符串匹配、复杂 bool expr、`LIKE '%abc%'`：

```text
executionOrderScore = 80
```

6. 明显昂贵的表达式（函数嵌套、复杂 `if`、复杂 UDF）：

```text
executionOrderScore = 100
```

### 6.4 第一版简约规则

第一版不做复杂选择率估计，只按“易/难”分层：

```text
easy:
  - timestamp 范围过滤
  - 主键 / 高选择性 key 等值过滤
  - EqualTo
  - IsNull / IsNotNull
  - 范围过滤
  - 命中的 literal-index equality

medium:
  - In
  - 简单 dict_execution

hard:
  - LIKE '%xxx%'
  - 复杂 bool expr
  - 函数调用 / if / 嵌套表达式
```

然后直接按层级排序。

## 7. 打分设计

### 7.1 设计要求

打分规则要满足：

1. 易于理解
2. 易于扩展
3. 不依赖复杂统计信息
4. 单看日志就能解释“为什么它进/没进 pushdown”

### 7.2 PushdownScore 基础模型

建议 `pushdownScore` 总分：

```text
pushdownScore = baseScore + literalBonus + complexityBonus + operatorBonus + kindBonus
```

### 7.3 第一版推荐规则

#### A. 原生范围 / 分区过滤

例如：

```sql
ts >= ...
ts < ...
partition_col = ...
```

建议：

```text
baseScore = 100
kind = NativePushdown
```

原因：

1. 这类过滤一贯收益高
2. 不应因为 adaptive 机制被挤掉

#### B. Literal Index Pushdown

例如：

```sql
account_id = '2100402624'
response_status_code = '404'
```

建议：

```text
baseScore = 60
```

额外加分：

1. literal 越长，分数越高

```text
literalBonus = min(length(literal), 32)
```

解释：

1. 更长的 string literal 往往选择性更高
2. 更适合优先 pushdown

#### C. DictExecution Pushdown

例如：

```sql
session_type = 'source'
is_last_tag = 'true'
online not in ('0')
```

建议：

```text
baseScore = 40
```

额外加分：

1. `=` 高于 `IN`
2. `IN` 高于复杂布尔表达式

```text
operatorBonus:
EqualTo      +10
In           +5
OtherBool     0
```

#### D. 非推送候选

不能 pushdown 或收益不明确的 expr：

```text
score = 0
kind = NonPushdown
```

### 7.4 复杂度惩罚

为了避免过于复杂的 filter 被误判为高收益，建议引入一个简单惩罚项：

```text
complexityBonus = -nodeCountPenalty
```

例如：

```text
nodeCount <= 3   => 0
nodeCount <= 6   => -5
nodeCount > 6    => -10
```

这能保证：

1. 形态简单的等值条件优先
2. 复杂布尔树不要轻易挤进 top-K

## 8. 一个具体、简约的实现策略

### 8.1 只处理 AND 链

第一版只在：

```sql
f1 AND f2 AND f3 ...
```

上启用 adaptive 策略。

对非 AND 根表达式：

1. 不打散
2. 沿用现有行为

这样实现简单、风险低。

### 8.2 分类

拆分后，每个 expr 先分类为：

1. `NativePushdown`
2. `LiteralIndexPushdown`
3. `DictExecutionPushdown`
4. `NonPushdown`

### 8.3 选择规则

推荐算法：

1. 原生 pushdown 全保留
2. 对 `LiteralIndexPushdown + DictExecutionPushdown` 统一计算 `pushdownScore`
3. 按 `pushdownScore` 降序排序
4. 只保留前 `K` 个
5. 其他字典相关 expr 保持为 `dict_execution`
6. 对最终保留在 Filter 上层执行的 AND expr，再按 `executionOrderScore` 升序重排

伪代码：

```scala
val andExprs = SplitConjunctivePredicates(condition)
val scored = andExprs.map(scoreExpr)

val alwaysPush = scored.filter(_.kind == NativePushdown)
val scoredDict = scored.filter(s =>
  s.kind == LiteralIndexPushdown || s.kind == DictExecutionPushdown)
val topKDict = scoredDict.sortBy(s => (-s.pushdownScore, s.originalOrder)).take(maxDictPushdownCount)

val selectedForPushdown = (alwaysPush ++ topKDict).map(_.expr).toSet
val reorderedResidualExprs =
  andExprs.filterNot(selectedForPushdown).sortBy(e =>
    (scoredByExpr(e).executionOrderScore, scoredByExpr(e).originalOrder))
```

其中：

```text
maxDictPushdownCount = 3
```

### 8.4 为什么“just by sort”足够

因为第一版不追求全局最优：

1. 目标只是避免“所有 dict pushdown 一股脑都下推”
2. 同时让保留在上层的 AND 链执行顺序更合理
3. 通过两套分数 + 排序，就能先拿到一个简单且可解释的近似最优
4. 后续再引入更复杂模型也有演进空间

## 9. 实现落点

### 9.1 Rule 侧

建议在现有 `FilterRewriteStrategy` 中增加一个小的 adaptive 选择步骤：

1. 先得到所有 bool predicate
2. 识别哪些能变成 `LiteralIndexPushdown`
3. 识别哪些会变成 `LowCardDictExecution`
4. 对这些候选计算 `pushdownScore` 和 `executionOrderScore`
5. 只让 top-K 继续参与 pushdown 改写
6. 未入选的字典相关候选不做 pushdown，保持为 `dict_execution` 路径
7. 对残余 AND 链按 `executionOrderScore` 重排

### 9.2 需要新增的抽象

建议新增：

```scala
case class PushdownCandidate(
    originalPredicate: Expression,
    rewrittenPredicate: Expression,
    kind: PushdownKind,
    pushdownScore: Int,
    executionOrderScore: Int,
    reason: Seq[String],
    originalOrder: Int)
```

这个结构专门用于 adaptive 选择，不污染现有 `LiteralIndexRewrite` 数据结构。

### 9.3 配置项

建议新增两层配置：

```text
tide.sql.dictAdaptivePushdown.*          # hot
spark.sql.starry.dictAdaptivePushdown.*  # cold
```

这可以直接归类为经典的 **hot & cold config pattern**：

- `tide.sql.*` 是 hot config
- `spark.sql.starry.*` 是 cold config

推荐优先通过表元数据控制这些开关；`starry-core` 读取时按如下优先级解析：

1. 当前 logical plan 绑定的 table metadata
2. `SQLConf`

同一 source 内，`tide.sql.*` 优先于 `spark.sql.starry.*`。

也就是说，metadata-aware 配置可以覆盖全局默认值，更适合按表控制 adaptive pushdown 行为；而 `spark.sql.starry.*` 仍可继续承担全局 base config 的角色。

默认值建议：

```text
enabled = false
maxDictPushdownCount = 3
reorderAndChains = true
```

原因：

1. 这是收益优化，不是语义修复
2. 应该先通过灰度逐步放开

## 10. 日志与可观测性

为了让这个策略可调、可解释，建议最少增加两类日志：

### 10.1 打分日志

```text
dict2.adaptive_pushdown.score traceId=... predicate=... kind=... pushdownScore=... executionOrderScore=... reason=...
```

### 10.2 选择结果日志

```text
dict2.adaptive_pushdown.selection traceId=... selectedCount=3 droppedCount=4 selected=[...] dropped=[...] reorderedResidual=[...]
```

这样后续就能直接回答：

1. 为什么某个 filter 没进 pushdown
2. 它是因为分数低，还是因为超过 top-K

## 11. 测试计划

### 11.1 单测

建议至少覆盖：

1. `AND` 链拆分后按分数排序
2. literal 越长，分数越高
3. `EqualTo` 高于 `IN`
4. 复杂 expr 分数下降
5. `maxDictPushdownCount = 3` 时只保留前三个 dict pushdown
6. 原生时间过滤不受 top-K 限制
7. `col2 LIKE '%abc%' AND col1 = 123` 被重排为 `col1 = 123 AND col2 LIKE '%abc%'`
### 11.2 E2E

建议在真实表上比较：

1. adaptive 关闭
2. adaptive 开启且 `K=1`
3. adaptive 开启且 `K=3`
4. adaptive 开启且 `K=5`

观察：

1. scan/pushdown 行为
2. 计划形态
3. 执行时间
4. 资源使用

## 12. 分阶段实现建议

### 12.1 第一阶段

只做：

1. AND 拆分
2. 静态打分
3. top-K 筛选
4. AND 链 easy-first 重排
5. 日志可观测

### 12.2 第二阶段

可逐步扩展：

1. 按列设置权重
2. 按表设置白名单 / 黑名单
3. 基于 e2e 数据反馈调权
4. 引入更细粒度的 operator 打分模型

## 13. 方案总结

本方案的核心不是“让更多 filter 被下推”，而是：

> 让真正值得下推的 filter 先下推，把收益不明确或代价偏高的 dict pushdown 保持为 `dict_execution` 路径。

第一版最重要的两个原则是：

1. 打分规则简单、可解释、可扩展
2. `filter-literal-pushdown` 的数量限制具体、简约、稳定

因此推荐第一版直接采用：

1. `AND` 拆分
2. 用 `pushdownScore` 选择前 `3` 个高分 dict pushdown
3. 用 `executionOrderScore` 让 easy expr 在前、hard expr 在后
4. 原生时间 / 分区过滤不受限制

这样既能快速落地，也便于后续继续演进成真正的自适应 pushdown 体系。
