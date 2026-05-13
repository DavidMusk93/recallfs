# SSoT：Query Interceptor Framework（Catalyst Resolution Rule）

## 1. 背景与问题

当前 Tide / Starry 查询链路里，`GROUP BY` 对高基列缺少统一的前置拦截能力，导致以下问题：

1. 查询虽然语法合法，但很容易在下游执行引擎产生高内存、高 shuffle、高聚合状态开销。
2. 风险信息目前散落在表元数据 `options / properties` 中，没有被统一消费。
3. 后续如果还要扩展更多“查询准入规则”，继续在单点规则里硬编码会越来越难维护。

因此，需要在 **Catalyst Resolution Rule** 阶段引入一个统一的、可扩展的 **查询拦截器框架**，先实现第一条规则：

> 拦截 `GROUP BY` 命中高基列的查询，并返回友好的 `AnalysisException`。

---

## 2. 目标与非目标

### 2.1 目标

- 在 `Resolution Rule` 阶段统一执行查询拦截。
- 框架具备：
  - interceptor trait
  - result ADT（通过 / 拦截+原因）
  - chain of responsibility 风格统一入口
  - 总开关 + 规则级开关
- 第一条规则支持：
  - 表级高基列列表元数据
  - 若干别名 key 兼容
  - 列级元数据 key 兼容（低成本一起支持）
- 尽量处理常见 plan 包装：`Project` / `SubqueryAlias` / 简单 passthrough plan。
- 错误信息友好、稳定、可测试。

### 2.2 非目标

- 本次不做物理执行期兜底。
- 本次不做成本评估或自动改写，只做显式 reject。
- 本次不引入新的强类型 Metadata schema；仍以 `table properties / metadata options` 为数据源。

---

## 3. 拦截点为什么选在 Resolution Rule 阶段

选择 Resolution Rule，而不是 Optimizer / Planner / Runtime，原因如下：

1. **语义已经基本稳定**
   - `Aggregate.groupingExpressions`、`Project` alias、`SubqueryAlias` 输出列已经解析完成。
   - 可以基于解析后的 `Attribute / ExprId` 做稳定判断，而不是依赖 SQL 文本字符串匹配。

2. **足够早，能尽早失败**
   - 在计划真正进入优化、物理规划、下推前就失败，用户更快获得可理解的报错。
   - 避免高风险查询进入后续链路造成不必要的 CPU / IO / 远端调用。

3. **能复用前置 alias 修正规则**
   - 当前仓库已经有 `ResolveClickhouseAggregateRule`、`ResolveClickhouseAggregateOrdinalRule` 等聚合相关 Resolution Rule。
   - 把新规则注册在它们之后，可以优先消费已经被修正过的 `GROUP BY alias / ordinal` 结果，减少重复处理。

4. **扩展成本低**
   - 后续新增“禁用无过滤全表扫描”“限制 select *”“限制危险 UDF”等规则时，可沿用同一入口。

---

## 4. 总体设计

### 4.1 框架结构

新增统一规则：

- `QueryInterceptorRule`

内部结构：

- `QueryInterceptor`：单条规则接口
- `QueryInterceptionResult`：结果 ADT
- `QueryInterceptorConf`：开关读取
- `interceptors: Seq[QueryInterceptor]`：责任链

### 4.2 结果 ADT

```scala
sealed trait QueryInterceptionResult
object QueryInterceptionResult {
  case object Passed
  final case class Rejected(interceptor: String, reason: String)
}
```

语义：

- `Passed`：当前规则不拦截
- `Rejected`：当前规则命中，直接终止责任链，并抛出 `AnalysisException`

### 4.3 Interceptor 接口

```scala
trait QueryInterceptor {
  def name: String
  def confKey: String
  def intercept(plan: LogicalPlan): QueryInterceptionResult
}
```

设计要点：

- 每条规则有独立名字，便于后续日志、监控、排障。
- 每条规则有独立 conf key，便于灰度/回滚。
- `intercept` 内部统一先判规则级开关，再执行具体逻辑。

### 4.4 Chain of Responsibility

`QueryInterceptorRule.apply(plan)` 的执行顺序：

1. 检查全局开关；关闭则直接放行。
2. 仅在 `plan.resolved == true` 时执行；未完成解析则先放行，等待下一轮 Analyzer。
3. 顺序遍历 `interceptors`。
4. 第一条返回 `Rejected` 的规则直接抛 `AnalysisException`。
5. 若所有规则都 `Passed`，返回原计划。

这保证了：

- 框架可扩展
- 逻辑单一职责
- 不引入 plan rewrite，仅做准入校验

---

## 5. 注册设计

注册文件：

- `starry/starry-core/src/main/scala/com/prx/starry/StarryPlugin.scala`

注册顺序：

1. `ResolveClickhouseAggregateRule`
2. `ResolveClickhouseAggregateOrdinalRule`
3. `QueryInterceptorRule`
4. 其他后续 Resolution Rule

原因：

- 先让已有 ClickHouse 聚合兼容逻辑把 alias / ordinal 规整到更稳定形态；
- 再做 query reject，减少误判。

---

## 6. 配置设计

### 6.1 全局开关

- `spark.sql.tide.query.interceptors.enabled`
- 默认：`true`

### 6.2 规则级开关

- `spark.sql.tide.query.interceptors.groupByHighCardinality.enabled`
- 默认：`true`

### 6.3 读取方式

- 直接从 `SQLConf.get.getConfString(...)` 读取
- 兼容常见布尔字符串：`true/false/1/0/yes/no/on/off`

这样既满足 Spark SQLConf 读取要求，也避免本次引入新的强类型 config entry。

---

## 7. 高基列 GROUP BY 规则设计

第一条规则名称：

- `GroupByHighCardinalityQueryInterceptor`

### 7.1 识别目标

命中条件：

- `LogicalPlan` 中存在已解析的 `Aggregate`
- 其 `groupingExpressions` 能追溯到某个底表的高基列

命中后报错：

```text
Query rejected: GROUP BY on high-cardinality column 'deviceid' is not allowed. Please use a lower-cardinality column or add additional filters.
```

### 7.2 元数据来源

高基列来源统一从 relation 的 `table.properties / metadata options` 中读取。

#### 表级 key（至少支持）

- `tide.sql.high_cardinality.columns`  ← canonical key
- `high_cardinality_columns`
- `tide.sql.high_card.columns`
- 额外低成本兼容：`tide.sql.high_cardinality.cols` / `tide.sql.high_card.cols`

值格式：

- 逗号分隔为主
- 顺手兼容 `;` / `|`
- 统一 trim + 去反引号/双引号 + lower-case 归一化

示例：

```text
tide.sql.high_cardinality.columns=deviceid,user_id,trace_id
```

#### 列级 key（低成本兼容）

- `tide.column.<col>.high_cardinality=true`
- `tide.column.<col>.cardinality=high`
- 额外兼容 `column.<col>.high_cardinality=true`
- 额外兼容 `column.<col>.cardinality=high`

示例：

```text
tide.column.deviceid.high_cardinality=true
tide.column.trace_id.cardinality=high
```

### 7.3 如何从 LogicalPlan 提取 GROUP BY 列

核心不是直接读 SQL 文本，而是读取解析后的：

- `Aggregate.groupingExpressions`

但是 `groupingExpressions` 里的列不一定直接等于底表原始列，常见情况包括：

- `Project` alias：`select deviceid as did ... group by did`
- `SubqueryAlias`：`select did from (select deviceid as did ...) t group by did`
- `Filter / Sort / Distinct / Join` 等 passthrough 包装

因此，本次实现不直接做“列名字符串硬比对”，而是做 **exprId-based lineage 追踪**。

### 7.4 Lineage 设计

新增一个轻量 lineage 解析器：

- `HighCardinalityLineageResolver`

职责：

- 递归遍历 `LogicalPlan`
- 为 `plan.output` 的每个 `ExprId` 建立“它是否源自某个高基列”的映射

大致规则：

1. **底表 relation**
   - `LogicalRelation`
   - `DataSourceV2Relation`
   - `DataSourceV2ScanRelation`
   - 从 `properties()` 提取高基列集合
   - 若输出列名命中高基集合，则记录 `ExprId -> HighCardinalityOrigin`

2. **Project**
   - 对 `projectList` 里的每个 `NamedExpression`
   - 基于其 `references` 向 child 追溯
   - 把 child 的高基来源透传到新的输出 `ExprId`

3. **Aggregate**
   - 对 `aggregateExpressions` 建立输出 lineage
   - 便于外层 subquery / project 再继续追踪

4. **其他常见 passthrough 节点**
   - 通过 `output` 与 children `output` 的 `exprId / 位置` 做保守透传
   - 覆盖 `SubqueryAlias`、`Filter`、`Sort`、`Distinct`、`Join` 等常见包裹场景

### 7.5 匹配逻辑

对每个 `Aggregate.groupingExpressions`：

1. 调用 `HighCardinalityLineageResolver.resolveExpression(aggregate.child, groupingExpression)`
2. 收集该 grouping expression 对应的底层高基列来源
3. 如果至少命中一个高基列，则选第一个稳定排序后的列作为报错列名
4. 返回 `Rejected`

### 7.6 为什么这套做法足够稳

- 不依赖 parser 输出字符串
- 不依赖 `qualifier` 文本
- 对 alias / subquery 更稳
- 与 Spark 的 `ExprId` 解析机制一致
- 对“至少常见 `Aggregate(group by attr)` 场景稳定”这一目标是充分的

---

## 8. Mock Input / Logic / Mock Output

### 8.1 Mock Input

#### Case A：低基列通过

```sql
select country, count(*)
from tide source
group by country
```

元数据：

```text
tide.sql.high_cardinality.columns=deviceid
```

#### Case B：高基列拦截

```sql
select did, count(*)
from (
  select deviceid as did
  from tide source
) tide sink
group by did
```

元数据：

```text
tide.sql.high_cardinality.columns=deviceid
```

#### Case C：无高基元数据时放行

```sql
select deviceid, count(*)
from tide source
group by deviceid
```

元数据：

```text
<empty>
```

### 8.2 Logic

- A：`country` 不在高基集合中，放行
- B：`did` 通过 `Project/SubqueryAlias` 追溯到底表 `deviceid`，命中高基集合，拦截
- C：底表没有高基元数据，放行

### 8.3 Mock Output

#### 放行

- 返回原始 `LogicalPlan`
- 不改写 plan
- 不注入额外 hint

#### 拦截

抛出：

```text
AnalysisException(
  "Query rejected: GROUP BY on high-cardinality column 'deviceid' is not allowed. Please use a lower-cardinality column or add additional filters."
)
```

---

## 9. Whitebox Rules

1. 仅在 `plan.resolved == true` 时执行，避免误判 unresolved plan。
2. 规则本身只做 reject，不做 rewrite，避免污染后续 optimizer 假设。
3. 责任链按顺序执行，首个 reject 即停止。
4. relation 元数据读取优先使用 `table.properties / metadata options` 当前已有载体，不新增 schema 依赖。
5. 列名匹配统一 normalize：trim、去引号、lower-case。
6. alias / subquery 通过 `ExprId` lineage 传播，而不是仅靠列名字符串替换。
7. 对无高基元数据的 relation 必须默认放行，避免误杀。
8. 对开关关闭场景必须零副作用直接放行。

---

## 10. Failure Modes

### 10.1 元数据缺失

现象：表没有任何高基配置。

处理：

- 规则直接放行
- 不报错，不猜测

### 10.2 元数据 key 大小写、空格、引号不规范

处理：

- 通过 normalize 尽量兼容

### 10.3 grouping expression 经过 alias / subquery 包装

处理：

- 通过 lineage resolver 追溯 child references

### 10.4 复杂表达式中引用高基列

处理：

- 当前实现会把 expression references 中命中的高基来源视作风险并拦截
- 这是保守策略，优先安全

### 10.5 多表 Join

处理：

- 通过 children output 的 passthrough 聚合 lineage
- 只要某个 grouping expression 追到高基来源，即可拦截

### 10.6 配置写错布尔值

处理：

- 无法解析时回退默认值
- 不因开关文本异常导致 analyzer 崩溃

---

## 11. 测试计划

### 11.1 A/B/C 三类测试矩阵

| 类别 | 目标 | SQL 形态 | 元数据 | 预期 |
|---|---|---|---|---|
| A 正常通过 | 低基列不过滤 | `GROUP BY country` | `deviceid` 为高基 | 通过 |
| B 命中拦截 | 高基列拒绝 | `GROUP BY did`，其中 `did -> deviceid` | `deviceid` 为高基 | 抛 `AnalysisException` |
| C 边界 | 无元数据不拦截 | `GROUP BY deviceid` | 无高基配置 | 通过 |

### 11.2 单测落点

优先使用轻量回归：

- `gateway-thrift-service/src/test/scala/com/bytedance/tide/catalyst/QueryInterceptorFunSuite.scala`
- 复用 `FakeTideSparkSession / FakeMetadata`

理由：

- 不依赖 Starry native lib
- 可直接验证解析+analyzer 链路
- 更接近真实 Tide 元数据解析路径

### 11.3 覆盖点

必须覆盖：

1. **正常通过**：低基列 `GROUP BY` 不被拦截
2. **命中拦截**：高基列 `GROUP BY` 抛出 `AnalysisException`，并断言错误信息
3. **边界**：无高基元数据时不拦截

建议额外覆盖（本次已通过案例形态顺带覆盖）：

- `Project + SubqueryAlias` 包装下的 alias 追溯

---

## 12. 实现拆分

### 12.1 新增文件

- `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/rule/QueryInterceptorRule.scala`

包含：

- `QueryInterceptionResult`
- `QueryInterceptor`
- `QueryInterceptorRule`
- `QueryInterceptorConf`
- `HighCardinalityMetadataParser`
- `HighCardinalityLineageResolver`
- `GroupByHighCardinalityQueryInterceptor`

### 12.2 修改文件

- `starry/starry-core/src/main/scala/com/prx/starry/StarryPlugin.scala`
  - 注册 `QueryInterceptorRule`
- `gateway-thrift-service/src/test/scala/com/bytedance/tide/catalyst/ParserTestSupport.scala`
  - 轻量测试 session 注入 `QueryInterceptorRule`
  - Fake metadata 增加测试字段与高基元数据样本
- `gateway-thrift-service/src/test/scala/com/bytedance/tide/catalyst/QueryInterceptorFunSuite.scala`
  - 新增定向回归

---

## 13. 兼容性与回滚

### 13.1 兼容性

- 仅新增 analyzer reject 逻辑，不改已有 plan rewrite 语义
- 无元数据时默认无感知
- 开关默认开启，但支持快速按 SQLConf 关闭

### 13.2 回滚手段

- 全局关闭：

```text
spark.sql.tide.query.interceptors.enabled=false
```

- 单规则关闭：

```text
spark.sql.tide.query.interceptors.groupByHighCardinality.enabled=false
```

- 代码回滚只涉及单一新规则注册与新文件，影响面小

---

## 14. 后续扩展方式

后续新增规则时，只需要：

1. 新增一个 `QueryInterceptor` 实现
2. 在 `QueryInterceptorRule.interceptors` 中注册
3. 补充独立 conf key
4. 加对应单测

可扩展方向包括但不限于：

- 禁止无过滤大表扫描
- 禁止 `SELECT *` 访问敏感表
- 限制危险 UDF / 非确定性 UDF
- 限制超大 `IN (...)` 常量列表
- 限制无 `WHERE` 的明细查询落到特定 connector

---

## 15. 验收标准

1. `StarryPlugin` 已在 `ResolveClickhouseAggregateRule / ResolveClickhouseAggregateOrdinalRule` 之后注册 `QueryInterceptorRule`
2. `GROUP BY` 高基列能稳定抛出友好异常
3. 低基列 / 无元数据场景不误拦截
4. alias / subquery 常见包装场景可回归
5. 有最小编译与最相关定向测试证据
