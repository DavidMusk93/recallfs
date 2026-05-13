# 项目中“逻辑计划 -> 物理执行计划”流程梳理

## 1. 文档目标

本文聚焦 Tide/Velox 这条执行链路中，Spark SQL 从逻辑计划到物理执行计划的收敛过程，重点回答 4 个问题：

1. 主入口在哪里
2. 逻辑计划如何收敛成 Tide/Velox 可执行的物理计划
3. 类型系统支持到什么程度，边界在哪里
4. 异常是在哪些阶段抛出、回退或收口的

需要特别区分两条路径：

- `parser()`：目标是拿到物理执行计划、做能力校验、构建 Tide 执行图
- `converterSQL()`：目标是把优化后的逻辑计划转成下游 SQL，不是本文主链路

---

## 2. 一句话结论

这个仓库没有重写一套独立 planner，而是：

1. 先让 Spark 原生 `QueryExecution` 完成解析、分析、优化和物理规划
2. 再通过 `StarryPlugin` 注入的规则，把 Spark 计划收敛成 Tide/Velox 可执行的列式物理子集
3. 最后由 `TideSparkSession.parser()` 做白名单校验、类型边界校验和执行图构建

因此，这条链路的核心不是“Spark 怎样生成物理计划”，而是：

- 哪些规则提前改写了逻辑计划和类型
- 哪些物理节点能被列式化
- 哪些边界会让计划退回 Spark row 世界
- 哪些类型和异常会在最终收口点被拒绝

---

## 3. 主链路总图

```text
SQL
 |
 v
+-----------------------------+
| TideSparkSession.parser()   |
| - 设置 catalog/threadlocal  |
| - parsePlan(sql)            |
| - 没有 LIMIT 时补 LIMIT     |
+-------------+---------------+
              |
              v
+-----------------------------+
| LogicalPlan                 |
| Spark parser 产物           |
+-------------+---------------+
              |
              v
+-----------------------------+
| QueryExecution(spark, plan) |
| 1. Analyzer                 |
| 2. Optimizer                |
| 3. Planner                  |
| 4. preparations             |
+-------------+---------------+
              |
              v
+----------------------------------------------+
| SparkSessionExtensions / StarryPlugin 注入规则 |
| Resolution / Optimizer / Columnar            |
+-------------+--------------------------------+
              |
              v
+---------------------------------------------+
| 初始 SparkPlan                               |
| 已是 Spark 物理计划，但未必是 Tide 形态      |
+-------------+-------------------------------+
              |
              v
+-----------------------------------------------+
| ColumnarTransitionRule                         |
| pre:  row -> columnar 改写                     |
| post: 接线、适配、插入 bridge/sink            |
+-------------+---------------------------------+
              |
              v
+-----------------------------------------------+
| Tide/Velox 列式物理计划                        |
| - ColumnarProjectExec                         |
| - ColumnarFilterExec                          |
| - ColumnarAggregateExec                       |
| - ColumnarHashJoin / MergeJoin                |
| - ColumnarRowDataSourceScanExec               |
| - ColumnarTideSinkExec                        |
+-------------+---------------------------------+
              |
              v
+----------------------------------------------+
| parser() 结果校验                             |
| - 非白名单节点 -> 失败                        |
| - 复杂类型依然受白名单/协议边界约束           |
+-------------+--------------------------------+
              |
              v
+----------------------------------------------+
| buildExecutionGraph                           |
| - 收集分区/scan/resource_group               |
| - 为 ColumnarEngineExec 生成 group DAG       |
| - 生成 jsonPlan / groups / partitions        |
+-------------+--------------------------------+
              |
              v
+-----------------------------+
| ParserResult                |
| - schema                    |
| - sparkPlan                 |
| - plans(json)               |
| - groups(DAG)               |
| - partitions                |
| - metaOptions               |
+-----------------------------+
```

---

## 4. 主链路分阶段说明

### 4.1 入口阶段：`TideSparkSession.parser()`

入口方法会先做几件工程化预处理：

- 标记当前查询类型，切到 `tide_catalog`
- 清理/初始化若干 thread-local 开关
- 调用 `spark.sessionState.sqlParser.parsePlan(sql)` 生成 `LogicalPlan`
- 原 SQL 没有 `LIMIT` 时，自动补一个 `LIMIT 1000000`

这里有两个快捷返回分支：

- `LIMIT 0`：直接返回 schema，不进入 Tide 执行图构建
- lite query：常量查询、本地 `LocalRelation` / `OneRowRelation` 直接本地求值

这说明主链路主要服务于“需要真实扫描和下游执行引擎”的查询。

### 4.2 Spark 原生阶段：`QueryExecution`

`parser()` 里直接构造：

```scala
val qe = new QueryExecution(spark, plan, tracker)
val sparkPlan = qe.executedPlan
```

这里的关键点是：

- 逻辑计划分析和物理规划主导者仍然是 Spark
- 项目没有自建一套完全独立的 planner
- 自定义能力主要通过扩展规则注入进 Spark 原有生命周期

因此，这条链路更准确的说法是：

- Spark 负责生成标准逻辑/物理计划
- Tide/Starry 负责把它收敛到可执行的列式子集

### 4.3 规则注入：`StarryPlugin.injectExtensions`

物理计划能否转成 Tide/Velox 可执行，关键依赖这里注入的三层规则。

#### 4.3.1 Resolution 层

仍属于逻辑计划阶段，负责让计划可分析、可改写：

- `FindPtDateValueRule`
- `ExpandPtDateRangeViewRule`
- `ResolveClickhouseAggregateRule`
- `ResolveClickhouseAggregateOrdinalRule`
- `OptimizeIn`
- `TimestampCast`
- `TimestampModRule`
- `TimestampCompare`

其中 `FindPtDateValueRule` / `ExpandPtDateRangeViewRule` 会提前提取并消费 `pt_d` 条件，为 view 展开做准备。

#### 4.3.2 Optimizer 层

- `AggregateFunctionRewriteRule`
- `RewriteWithGlobalDict`
- `CountDistinctToBitmap`
- `TimestampCompare`
- `OrEqualsToInOptimizeRule`

这里最重要的是 `RewriteWithGlobalDict`，它会在逻辑计划阶段把部分字符串语义列重写为字典编码执行列，例如 `${col}_dict_idx`。

#### 4.3.3 Planner / Columnar 层

- `JoinSelectionOverrides`
- `ColumnarTransitionRule`

真正把 Spark 常规物理节点换成 Tide/Velox 列式节点的核心在 `ColumnarTransitionRule`。

### 4.4 物理节点列式化：`ColumnarTransitionRule`

这是主链路最关键的一步。整体形态可以简化为：

```text
SparkPlan
  -> preColumnarTransitions
  -> postColumnarTransitions
  -> 更接近 Tide/Velox 的 SparkPlan
```

#### 4.4.1 pre 阶段：`PreRuleReplaceRowToColumnar`

按顺序应用：

```text
ConvertParquetFileFormat
  -> ColumnarRewriteRule
  -> SingleAggregateRule
  -> ColumnarTransformRule
  -> OptimizeExchange
  -> OptimizeSort
  -> CollapseProjectExec
```

这里最重要的是 `ColumnarTransformRule`。

#### 4.4.2 `ColumnarTransformRule` 典型替换关系

```text
ProjectExec                -> ColumnarProjectExec
FilterExec                 -> ColumnarFilterExec
HashAggregateExec          -> ColumnarAggregateExec
ObjectHashAggregateExec    -> ColumnarAggregateExec
SortExec                   -> ColumnarSortExec
GenerateExec               -> ColumnarGenerateExec
RowDataSourceScanExec      -> ColumnarRowDataSourceScanExec
ShuffledHashJoinExec       -> ColumnarHashJoinExec
BroadcastHashJoinExec      -> ColumnarBroadcastHashJoinExec
SortMergeJoinExec          -> ColumnarMergeJoinExec
ShuffleExchangeExec        -> ColumnarShuffleExchangeExec
GlobalLimitExec            -> ColumnarLimitExec
LocalLimitExec             -> ColumnarLimitExec
```

当 `StarryConf.transformWithTide` 开启时，部分 `Limit` / `Shuffle` 路径还会被进一步包成：

- `ColumnarTideSinkExec`
- `ColumnarTideSourceExec`

这些节点用于生成 Tide 需要的分阶段执行图。

#### 4.4.3 post 阶段：`VeloxColumnarPostRule`

post 阶段更偏“接线”和“边界适配”，而不是语义改写：

- 子节点不是列式时，补 `ColumnarInputAdapter`
- `RowToColumnarExec` 转 `RowToVeloxColumnarExec`
- `ColumnarToRowExec` 转 `VeloxColumnarToRowExec`
- 顶层列式节点包 `ColumnarEngineExec`
- Tide 模式下，首个 engine 节点下挂 `ColumnarTideSinkExec(RPC)`

可以把它理解成：

- pre 阶段：换算子
- post 阶段：补适配器、补执行边界

#### 4.4.4 `VeloxColumnarToRowExec`：列式退化信号和常见报错暴露点

`VeloxColumnarToRowExec` 的职责非常单一：

- 输入：`RDD[ColumnarBatch]`
- 输出：`RDD[InternalRow]`
- 作用：在下游节点仍要求 row 语义时，把列式结果转成 Spark row

它的实现方式也很直接：

- `doExecute()` 调用 `child.executeColumnar()`
- 再包装成 `ColumnarToRowRDD`
- 最终通过 `batch.rowIterator()` 逐行展开成 `InternalRow`

这里要特别注意两点：

1. 它不等于“物理计划生成失败”
2. 但它通常意味着计划已经没有保持端到端列式，而是退回了 Spark row 世界

因此，在线上排障时更应该把它理解成：

- 一个列式退化信号
- 一个高风险边界
- 一个常见异常暴露点

出现它通常意味着至少有一个事实成立：

- 某个算子仍依赖 Spark row 语义
- 某个 `Exchange` / `Shuffle` 边界仍走 row 格式
- 结果返回链路还需要 row 物化
- 复杂类型在 row 返回边界更容易暴露兼容性问题

所以，很多报错虽然“最终落在 `VeloxColumnarToRowExec`”，但它往往是暴露点，不一定是根因点。

#### 4.4.5 一条典型计划里它代表什么

如果计划片段类似：

```text
Scan(Columnar)
  -> ColumnarLimit
  -> ColumnarPartitionedOutput
  -> ColumnarTideSink(SHUFFLE_WRITER)
  -> VeloxColumnarToRowExec
  -> Exchange(row shuffle)
  -> InputAdapter
  -> ColumnarTideSource(SHUFFLE_READER)
  -> ColumnarLimit
  -> ColumnarTideSink(RPC)
```

可以把这段计划进一步细化成下面这张图：

```text
+---------------------------------+
| 列式世界                        |
+---------------------------------+
                |
                v
+---------------------------------+
| Columnar*Exec                   |
| Scan / Filter / Project / Limit |
+---------------------------------+
                |
                v
+---------------------------------+
| ColumnarPartitionedOutput       |
| 输出 ColumnarBatch              |
+---------------------------------+
                |
                v
+---------------------------------+
| ColumnarTideSink(SHUFFLE_WRITER) |
| 写入 shuffle stage              |
+---------------------------------+
                |
                v
+---------------------------------+
| 退回 row 世界                   |
+---------------------------------+
                |
                v
+---------------------------------+
| VeloxColumnarToRowExec          |
| ColumnarBatch -> InternalRow    |
+---------------------------------+
                |
                v
+---------------------------------+
| Exchange(hashpartition)         |
| Spark row shuffle               |
+---------------------------------+
                |
                v
+---------------------------------+
| 再回到列式世界                  |
+---------------------------------+
                |
                v
+---------------------------------+
| InputAdapter                    |
| 包装 row child                  |
+---------------------------------+
                |
                v
+---------------------------------+
| ColumnarTideSource              |
| (SHUFFLE_READER)                |
| 读回列式数据                    |
+---------------------------------+
                |
                v
+---------------------------------+
| ColumnarLimit / 上层列式节点    |
+---------------------------------+
                |
                v
+---------------------------------+
| ColumnarTideSink(RPC)           |
| 最终 RPC 输出边界               |
+---------------------------------+
```

它表示的是：

- 下半段 `Scan -> ColumnarTideSink(SHUFFLE_WRITER)` 已经是列式执行
- 中间的 `Exchange` 仍是 Spark row 风格
- 所以前面插了一个 `VeloxColumnarToRowExec`
- `Exchange` 之后，又通过 `InputAdapter` / `ColumnarTideSource` 回到列式世界

这类计划说明：

- 计划已经成功生成
- 但中间出现了列式和 row 的退化边界
- 对 Tide 查询来说，这类边界通常意味着更高失败风险

### 4.5 扫描与下推：`TideScanBuilder`

扫描侧虽然不直接“生成顶层物理计划”，但会深刻影响整个物理计划能否成立。

它负责：

- 列裁剪
- filter pushdown
- limit / topN / aggregate pushdown 接口
- view alias 到真实列名的映射
- 字典列逻辑名与物理名映射

#### 4.5.1 字段映射逻辑

`TideScanBuilder` 会在以下名字之间做映射：

- 逻辑列名
- view alias
- 原始物理列名
- 字典列名，如 `_dict_idx`

因此，逻辑计划里看到的列名，不一定等于底层 connector 真正读取的列名。

#### 4.5.2 filter 支持程度

当前 `filterToExpr` 明确支持：

- `EqualTo`
- `GreaterThan`
- `GreaterThanOrEqual`
- `LessThan`
- `LessThanOrEqual`
- `In`
- `IsNull`
- `IsNotNull`
- `And`

明确不支持或未开启下推的包括：

- `Or`
- `Not`
- `EqualNullSafe`
- `StringStartsWith`
- `StringEndsWith`
- `StringContains`

策略上属于：

- 能转就下推
- 不能转就放进 `notPushedFilter`
- 某些特定 connector 场景如果完全没有分区过滤，则直接拒绝

### 4.6 收口阶段：`parser()` 校验与 `buildExecutionGraph`

`parser()` 在拿到 `executedPlan` 后，还要做两次重要收口：

#### 4.6.1 物理计划白名单校验

若出现以下情况，会直接失败：

- 出现非白名单物理节点
- 结果计划无法收敛到 Tide/Velox 可接受的节点子集

这说明系统目标不是“能跑就行”，而是：

- 必须收敛成 Tide/Velox 可接受的物理计划
- 否则宁可失败

#### 4.6.2 执行图构建

`buildExecutionGraph` 会进一步：

- 打开 timestamp / complex type 相关 thread-local
- 收集 scan `metaOptions` / `resource_group`
- 为 `ColumnarEngineExec` 生成 group DAG
- 生成 `jsonPlan`、`groups`、`partitions`

最后收口到 `ParserResult`。

---

## 5. 类型系统支持程度

### 5.1 总体判断：类型支持是分层的

项目的类型支持不是“Spark 全量类型等价支持”，而是明显分层的：

```text
强支持:
  基本原子类型 + 常见时间类型 + 常见聚合/比较所需类型

中等支持:
  Decimal / Array / Struct

弱支持或有边界:
  Map / 复杂嵌套类型 / 结果集复杂类型回传
```

更准确地说，排查类型问题至少要同时看 4 层：

1. SQL parser / analyzer 是否能识别
2. 逻辑规则是否保持原有语义
3. 列式表达式能否转成 native JSON
4. 最终结果 schema 能否回传给网关/客户端

某个类型在第 1 层可解析，不等于它在第 4 层也完整可返回。

### 5.2 基本原子类型

从 `ColumnarTransformRule.canHashBuild()` 和 `ExpressionConverter` 的实现看，最稳定的是：

- `StringType`
- `IntegerType`
- `LongType`
- `ShortType`
- `ByteType`
- `DoubleType`
- `FloatType`
- `BooleanType`
- `DateType`
- `TimestampType`
- `BinaryType`

这些类型在以下环节都有较完整支持：

- 表达式 native 化
- join / group / filter / order 常规算子
- 结果 schema 回传

### 5.3 时间类型

时间类型支持较强，但内部表示不是完全透明传递。

#### 5.3.1 `TimestampType`

在不同阶段会有不同表示：

- `ExpressionConverter.convertTimestampToInteger`
- `TideScanBuilder.resolveAttr()` 中按 `LongType` 视角参与下推
- `compileValue()` 中按秒级时间戳处理

结论是：

- 业务语义层面支持 `TimestampType`
- 执行表示层面有多处适配
- 调试时间类型问题必须同时看逻辑规则、表达式转换和下推链路

#### 5.3.2 `DateType`

- `VeloxTypeResolver` 可把 native type JSON 解析为 `DateType`
- 网关结果类型也有 `DATE`

所以 `DateType` 的整体支持通常强于复杂类型。

### 5.4 Decimal

`VeloxTypeResolver` 明确支持 `DecimalType(precision, scale)`，说明 native 类型解析能力存在。

但要注意：

- “能识别 decimal”不等于“所有 decimal 表达式和算子都稳”
- 是否能列式执行，还取决于表达式转换和原生函数支持

因此更稳妥的结论是：

- `DecimalType` 有基础支持
- 具体能力要按表达式级别核实

### 5.5 复杂类型

#### 5.5.1 Array

- `VeloxTypeResolver` 支持 array 类型解析
- `ComplexTypeConvert` 专门处理数组下标语义和 Spark / Velox 差异

说明：

- Array 不是完全不支持
- 但依赖专门兼容逻辑，属于“可用但有语义边界”的类型

#### 5.5.2 Struct

- `VeloxTypeResolver` 支持把 native `row` 解析成 Spark `StructType`

说明：

- 执行层内部理解 struct / row
- 最终是否能稳定回传，还要看下游 schema 和协议能力

#### 5.5.3 Map

Map 是当前最明确的边界之一：

- `VeloxTypeResolver` 能解析 `MapType`
- `VeloxRowToColumnConverter` 实现了 `MapConverter`
- `NativeColumnarBatchSuite` 里有 `getMap()` 读取 map 列的测试
- `SparkFieldDescUtils` 已把 Spark `MapType` 递归映射成 `FieldDesc.createMapField(...)`

因此当前状态不是“内部部分识别、出口禁止”，而是：

- 内部执行层具备 map 读写能力
- `parser()` 不再在结果收口阶段硬拒绝 `MapType`
- `VeloxExecutor` / `BIExecutor` 的 Spark schema -> Result schema 已支持结构化 map
- thrift `ExecuteResponse.schema` 受协议约束，目前仍只能保留顶层 `FieldType.MAP`

#### 5.5.4 `dict2`

`dict2` 不是新增一种 Spark `DataType`，而是改变字符串列在执行过程中的物理表示：

```text
原始语义列:      domain: StringType
执行中间表示:    domain_dict_idx: IntegerType
最终输出边界:    decode 回 StringType
```

因此 `dict2` 对类型系统的影响是：

- 逻辑语义仍是字符串
- 物理执行尽量保持编码态
- 只有在语义边界或输出边界才解码

### 5.6 结果回传层弱于执行层

结果回传链路的类型支持明显窄于执行层：

- `BIExecutor` / `VeloxExecutor` 已改为递归 `DataType -> FieldDesc`
- `Result.toResponse()` 已能把 `MAP/ARRAY/ROW` 值反序列化成 JSON 对象或数组
- 但 thrift `ExecuteResponse.schema` 仍只有顶层 `FieldType + name`

因此，执行层和结果层已经打通复杂类型元数据，但不同出口的“结构化细节保真度”仍受协议影响。

---

## 6. 异常处理与失败模式

### 6.1 总体策略

这个项目的异常处理不是统一一层兜底，而是分阶段收口：

```text
解析期:
  尽量给出 ParseException

分析/规则期:
  不满足语义约束时直接 fail

列式转换期:
  局部允许 soft fallback

最终 Tide 入口:
  用白名单和结果类型做 hard fail 收口
```

因此系统行为可以概括为：

- 局部模块允许软回退
- 但 Tide 主链路总体仍偏严格失败

### 6.2 解析阶段异常

SQL parser 报错主要集中在：

- `gateway-catalyst/.../QueryParsingErrors.scala`

特点是：

- 语法错误通常统一构造成 `ParseException`
- 错误信息相对明确
- 这时通常还没进入物理计划阶段

### 6.3 分析与逻辑规则阶段异常

常见来源：

- Spark analyzer 自身 `checkAnalysis`
- 业务规则主动抛错

例如：

- `FindPtDateValueRule` / `BIResolveRule` 对非法 `pt_d` 条件抛 `AnalysisException`

特点是：

- 一旦失败，不会继续进入物理计划生成

### 6.4 表达式转换与列式改写异常

#### 6.4.1 表达式转换

`ExpressionConverter` 有两种失败方式：

- 硬失败：native 函数解析失败且无回退时，直接抛 `RuntimeException`
- 软失败：记录日志并保留原 expression，导致上层算子可能无法列式化

#### 6.4.2 列式改写

`PreRuleReplaceRowToColumnar.apply()` 对整段改写包了一层 catch：

```text
try replaceWithColumnarPlan(plan)
catch
  logError(...)
  return original plan
```

这属于模块内部的 soft fallback。

但在主链路里，这种 soft fallback 往往会在 `parser()` 后续白名单校验时演变成 hard fail。

### 6.5 物理计划白名单与结果类型边界

`parser()` 拿到 `executedPlan` 后会做严格校验：

- 非白名单物理节点：失败
- 复杂类型是否能真正漏出，还受结果协议和调用方能力影响

所以这套链路不接受“部分 Tide、部分普通 Spark”的混合结果长期漏出。

### 6.6 扫描下推异常

`TideScanBuilder` 更偏“尽量降级”：

- filter 转换失败时，可放入 `notPushedFilter`
- 属性不存在、filter 类型非法、特定 connector 约束不满足时，再直接失败

因此扫描侧策略是：

- 普通能力不足优先不下推
- 触碰 schema 或 connector 约束时直接拒绝

### 6.7 执行图构建与服务层异常

`buildExecutionGraph` 阶段可能因 child 类型不符合预期而抛 `UnsupportedOperationException`。

不过这里通过 `finally` 做了 thread-local 清理，资源恢复相对完整。

服务层异常则更直接，常见于：

- engine type 不支持
- schema 长度不匹配
- HTTP 超时
- 下游返回非 200
- JSON 解析失败

从用户视角看，一次失败可能来自三层：

1. Spark SQL 解析 / 分析失败
2. Tide / Velox 计划能力失败
3. 下游执行引擎或网关协议失败

---

## 7. 专题补充

### 7.1 为什么 `col = 1` 会出现 `isNotNull(col)`

这个现象的直接出处，不在本仓库自定义规则里，而在当前依赖版本 Spark Catalyst 的 `InferFiltersFromConstraints`。

#### 7.1.1 项目内调用路径

`parser()` 直接走：

```scala
val qe = new QueryExecution(spark, plan, tracker)
val sparkPlan = qe.executedPlan
```

因此会吃到 Spark 标准 optimizer 的约束推导。

而 `converterSQL()` 虽然保留了 `InferFiltersFromConstraints` 这个 batch 名字，但又显式关闭了：

```scala
spark.sql.constraintPropagation.enabled = false
```

所以它通常不是 `IsNotNull` 推导的主要来源。

#### 7.1.2 当前依赖版本中的真实出处

当前依赖版本可追到这条链：

```text
InferFiltersFromConstraints.apply()
  -> inferFilters(...)
  -> inferNewFilter(plan, constraints)
  -> constructIsNotNullConstraints(constraints, plan.output)
  -> inferIsNotNullConstraints(expr)
  -> scanNullIntolerantAttribute(expr)
  -> 生成 IsNotNull(attr)
```

其中最关键的逻辑是：

- `inferNewFilter()` 会把新增约束包成一层新的 `Filter`
- `scanNullIntolerantAttribute()` 会从 null-intolerant 表达式里提取属性
- `EqualTo(col, 1)` 正属于 null-intolerant 表达式

#### 7.1.3 为什么语义上成立

SQL 是三值逻辑。

对于：

```text
WHERE col = 1
```

当 `col = null` 时，结果是 `null` 而不是 `true`，因此该行本来就不会被保留。

所以：

```text
WHERE col = 1
```

与：

```text
WHERE col IS NOT NULL AND col = 1
```

在过滤结果上等价。

#### 7.1.4 为什么项目后续阶段还能看到它

因为后续组件会继续消费这条显式约束：

- `TideScanBuilder.filterToExpr()` 支持 `IsNotNull`
- 各类 SQL dialect 会把它翻译成目标 SQL 的 `IS NOT NULL`

所以在 optimized logical plan、physical plan 和最终下推 SQL 中，都可能继续看到这条谓词。

### 7.2 `MapType` 报错与 `VeloxColumnarToRowExec`

#### 7.2.1 现在的链路状态

Map 结果返回当前已经按方案 B 打通了主链路：

1. `parser()` 不再在结果校验阶段硬拒绝 `MapType`
2. Spark `DataType` 已递归映射到 `FieldDesc`
3. `VeloxExecutor` / `BIExecutor` 本地结果返回保留 `MAP/ARRAY/ROW` schema
4. `Result.toResponse()` 对复杂类型值按 JSON 结构反序列化

#### 7.2.2 仍然需要注意的协议边界

虽然主链路已经放开，但不同出口的表达能力并不完全一致：

- `Result` 内部 schema 可以保留 `FieldDesc.createMapField(...)`
- REST JSON 返回可以保留结构化对象值
- thrift `ExecuteResponse.schema` 由于 `gateway.thrift` 里 `Field` 只有 `type + name`，目前只能保留顶层 `FieldType.MAP`

也就是说：

- 结构化值已经能走通
- 但 thrift schema 还不能精确表达 map 的 key/value 子类型

#### 7.2.3 与 `VeloxColumnarToRowExec` 的关系

即使 `MapType` 已放开，`VeloxColumnarToRowExec` 仍然值得重点关注：

- 它依然是列式退化边界
- 复杂类型问题仍然最容易在这里暴露
- 如果后续再出现复杂类型报错，优先看这里前后的 row/columnar 交界

---

## 8. 设计取舍

### 8.1 不是“支持 Spark 全部能力”，而是“支持 Tide 可控子集”

从白名单校验、复杂类型协议边界、filter pushdown 限制都能看出来，这套实现不是为了完整复刻 Spark 执行能力，而是：

- 在一组可控 SQL、算子和类型子集上，稳定转成 Tide/Velox 可执行计划

### 8.2 类型问题必须分层看

排查类型问题时，至少要同时看：

```text
逻辑规则层
  -> dict2 / timestamp rewrite 是否改了语义

表达式转换层
  -> 是否能转 native JSON

scan pushdown 层
  -> 是否按 connector 需要改写

结果回传层
  -> 网关是否真的认识该类型
```

任何一层不支持，最终都可能表现成“该类型不支持”。

### 8.3 异常策略是“前段保留信息，后段严格收口”

这条链路不是完全 fail-fast，但也不是全程宽松回退：

- 前段：parser / analyzer / rule 尽量保留具体错误
- 中段：表达式转换和列式改写允许局部 soft fallback
- 后段：`parser()` 用白名单和结果类型边界做硬收口

因此整体行为仍然相对确定。

---

## 9. 推荐阅读顺序

如果要继续深入，建议按下面顺序阅读：

1. `gateway-catalyst/.../TideSparkSession.scala`
2. `starry/starry-core/.../StarryPlugin.scala`
3. `starry/starry-core/.../ColumnarTransitionRule.scala`
4. `starry/starry-core/.../ColumnarTransformRule.scala`
5. `starry/starry-core/.../ExpressionConverter.scala`
6. `gateway-catalyst/.../TideScanBuilder.scala`
7. `starry/starry-core/.../RewriteWithGlobalDict.scala`
8. `gateway-catalyst/.../QueryParsingErrors.scala`
9. `gateway-thrift-service/.../BIExecutor.java`

---

## 10. 总结

这套链路的本质可以概括为：

```text
Spark 负责把 SQL 变成标准逻辑/物理计划
      +
Starry/Tide 规则负责把它压缩到 Tide/Velox 能执行的列式子集
      +
parser() 再用白名单、类型边界和执行图构建做最终收口
```

因此，理解这条链路时最重要的不是“Spark 怎样生成物理计划”，而是：

- 哪些逻辑规则提前改变了语义和类型
- 哪些物理节点能保持列式闭环
- 哪些边界会退回 Spark row 世界
- 哪些类型和异常会在 Tide 收口点被直接拒绝
