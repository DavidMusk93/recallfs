# 常量执行引擎设计与实现梳理

## 1. 文档目标

本文梳理仓库里与"常量执行 / 常量折叠 / 谓词证明"相关的现有实现，重点回答以下问题：

- 常量执行引擎在当前系统里处于什么位置
- 现在到底有几套求值能力，它们分别处理什么输入
- 主键过滤条件是如何被证明、裁剪并进入归一化结果的
- 当前实现的边界、重复点和后续提案值得关注的地方是什么

结论先行：

- 代码里并不是单一的一套"常量执行引擎"，而是三层能力并存
- `ConstexprEngine` 是 AST 级常量求值器，能对 `ExpressionNode` 做纯常量计算
- `DefaultExprSimplifier` 是 AST 级表达式简化器，做局部折叠和布尔恒等式消解
- 生产路径真正接入的是 `DefaultPredicateReducer`，它处理的是"顶层 AND 拆分后的字符串过滤条件"，通过规则系统做主键谓词证明与裁剪
- `DateBucketPartitionHintAnalyzer` 已实现，但目前还是独立组件，尚未接入主归一化链路

可以把当前架构理解为：

```text
AST world
  |
  +-- ConstexprEngine
  |      纯 AST 常量求值
  |
  +-- DefaultExprSimplifier
  |      纯 AST 局部重写
  |
  `-- DateBucketPartitionHintAnalyzer
         AST 级 partition hint 判定

filter-string world
  |
  `-- DefaultPredicateReducer
         主键过滤条件证明 / 冗余裁剪

normalization world
  |
  `-- ClickHouseSqlNormalizer
         只接入 DefaultPredicateReducer
```

## 2. 系统位置

常量执行相关能力不是在 SQL 入口最前面运行，而是散落在解析、归一化、过滤条件提取之后。

当前主链路可以概括为：

```text
raw sql
  |
  v
+----------------------------+
| ANTLR parser               |
| ClickHouseLexer/Parser     |
+-------------+--------------+
              |
              v
+----------------------------+
| QueryFactsExtractor        |
| - table refs               |
| - function names           |
| - where/having and-chain   |
+-------------+--------------+
              |
              v
+----------------------------+
| ClickHouseSqlNormalizer    |
| - canonical sql            |
| - template sql             |
| - split top-level AND      |
| - classify pk filters      |
+-------------+--------------+
              |
              +-----------------------------+
              |                             |
              v                             v
+----------------------------+  +---------------------------+
| business filter reduction  |  | primary key filter reduce |
| InListIntersectionRule     |  | DefaultPredicateReducer   |
+-------------+--------------+  +-------------+-------------+
              |                             |
              +-------------+---------------+
                            |
                            v
+-----------------------------------------------------------+
| SqlNormalizationResult                                     |
| - whereBusinessFilters                                     |
| - whereBusinessFilterTemplates                             |
| - primaryKeyFilters                                        |
| - primaryKeyFilterTemplate                                 |
+-----------------------------------------------------------+
                            |
                            v
+----------------------------+
| DuckDbAnalysisWriter       |
| - write normalized fields  |
| - derive queryRangeSeconds |
+----------------------------+
```

这里最重要的一点是：

- 业务过滤条件走 `InListIntersectionRule`
- 主键及主键派生过滤条件走 `DefaultPredicateReducer`
- `ConstexprEngine` 和 `DefaultExprSimplifier` 目前没有接入这条主链路

## 3. 三层能力总览

### 3.1 能力分层

| 层次 | 核心类 | 输入 | 输出 | 是否接入主归一化链路 | 主要用途 |
| --- | --- | --- | --- | --- | --- |
| AST 常量求值 | `ConstexprEngine` | `ExpressionNode` | `ConstValue` | 否 | 对纯常量 AST 表达式做递归求值 |
| AST 局部简化 | `DefaultExprSimplifier` | `ExpressionNode` | `SimplifyResult` | 否 | 做局部常量折叠和布尔恒等式消解 |
| 字符串表达式求值 | `ClickHouseExpressionEvaluator` | `String expression` + `bindings` | `ConstValue` | 间接接入 | 为谓词证明阶段计算 `f(ts)` 这类派生表达式 |
| 谓词证明 / 归约 | `DefaultPredicateReducer` | `List<String>` and-chain | `ReductionResult` | 是 | 证明主键派生过滤条件冗余、保留或矛盾 |
| partition hint 判定 | `DateBucketPartitionHintAnalyzer` | `ExpressionNode` + `TimeRangeResult` | `PartitionHintDecision` | 否 | 判断 `toDate(ts)=...` 一类 hint 是否被时间范围覆盖 |

### 3.2 设计意图差异

- `ConstexprEngine`
  目标是回答"这个 AST 表达式能否被完整求成一个 `ConstValue`"
- `DefaultExprSimplifier`
  目标是回答"这个 AST 表达式能否在不完整求值的前提下做局部折叠"
- `DefaultPredicateReducer`
  目标是回答"一个主键派生过滤条件，是否能被另一个更直接的主键范围条件证明为恒真/矛盾"

三者虽然都属于"常量执行"范畴，但问题空间不同，因此实现也被拆散了。

## 4. AST 级常量求值器：`ConstexprEngine`

## 4.1 输入输出模型

`ConstexprEngine` 接收：

- `ExpressionNode expression`
- `EvalContext context`
- `AnalysisTrace trace`

返回值是 `ConstValue`。它不是简单的字面量，而是一个带类型标签的值包装：

```text
ConstValue.Kind
  BOOLEAN
  INTEGER
  FLOAT
  STRING
  DATE
  DATETIME
  NULL
  UNKNOWN
```

这意味着引擎内部采用的是"已知/未知"二元可计算模型，而不是完整的 SQL 三值逻辑执行器。

## 4.2 求值分派

`ConstexprEngine` 的递归分派非常直接：

```text
eval(expr)
  |
  +-- LiteralExpression       -> 字面量转 ConstValue
  +-- ParenthesizedExpression -> 递归 inner
  +-- BinaryExpression        -> 二元运算
  +-- FunctionCallExpression  -> 内建函数求值
  +-- IdentifierExpression    -> UNKNOWN
  `-- RawExpression           -> UNKNOWN
```

ASCII 流程如下：

```text
                 +----------------------+
                 | ExpressionNode input |
                 +----------+-----------+
                            |
        +-------------------+-------------------+
        |                   |                   |
        v                   v                   v
  +-----------+      +-------------+     +-------------+
  | literal   |      | binary expr |     | function    |
  +-----+-----+      +------+------+     +------+------+ 
        |                   |                   |
        v                   v                   v
  ConstValue          eval left/right      eval args first
                            |                   |
                            v                   v
                     operator dispatch    builtin dispatch
                            |                   |
                            +---------+---------+
                                      |
                                      v
                                ConstValue
```

## 4.3 支持的运算

### 字面量

- `BOOLEAN`
- `INTEGER`
- `FLOAT`
- `STRING`
- `NULL`

### 二元运算

- 比较：`= != > >= < <=`
- 数值：`+ - * /`
- 布尔：`AND OR`

### 内建函数

- `concat`
- `toString`
- `lpad`
- `toDateTime`
- `toDate`
- `toHour`

### 时区行为

- `toDateTime(epoch_seconds)` 和 `toDate(epoch_seconds)` 依赖 `EvalContext.timezone`
- 默认时区来自 `EvalContext.defaultContext()`，当前是 `Asia/Shanghai`

## 4.4 关键语义

### 1. 未知标识符直接返回 `UNKNOWN`

引擎不是符号执行器。只要遇到：

- `IdentifierExpression`
- `RawExpression`
- 未注册函数
- 参数中出现 `UNKNOWN`

就会回退到 `ConstValue.unknown()`

### 2. 布尔短路只做最小证明

`AND` / `OR` 的处理并不是完整 SQL 布尔代数，而是最小充分短路：

```text
false AND x -> false
true  OR  x -> true
true  AND true  -> true
false OR  false -> false
other cases     -> unknown
```

这保证了保守性，但也意味着：

- 不会继续传播 `NULL`
- 不会做更多基于三值逻辑的精细推断

### 3. `NULL` 的处理是保守的

对于普通二元运算，只要左右任一侧是 `NULL`，就直接返回 `UNKNOWN`，而不是显式返回 SQL 语义下的 `NULL` 结果。

因此当前实现更接近：

```text
无法稳定证明 = UNKNOWN
```

而不是：

```text
严格 SQL 执行结果
```

### 4. 数值除法统一输出浮点

`DIVIDE` 采用：

- `scale = 12`
- `RoundingMode.HALF_UP`
- 结果 `stripTrailingZeros()`

如果除数为 0，则返回 `UNKNOWN`。

## 4.5 适用场景

`ConstexprEngine` 当前更像"可复用的 AST 常量求值基础设施"，适合：

- 纯字面量函数链求值
- 测试中验证特定表达式能否被求值
- 将来 AST 归一化阶段接入真正的常量折叠

但它目前不在主归一化路径中直接使用。

## 5. AST 级简化器：`DefaultExprSimplifier`

`DefaultExprSimplifier` 与 `ConstexprEngine` 很接近，但职责更偏"重写"而不是"求值"。

它的核心行为只有两类：

- `foldBinary`：当左右都是 `LiteralExpression` 时做运算折叠
- `foldBooleanIdentity`：做布尔恒等式化简

其化简模式大致如下：

```text
true  AND x -> x
false AND x -> false
false OR  x -> x
true  OR  x -> true
1 + 2      -> 3
(1 + 2) = 3 -> true
```

值得注意的点：

- 它不调用 `ConstexprEngine`
- 它自己又实现了一遍数值比较、运算和布尔规则
- 它只处理 AST `BinaryExpression`
- 它的输出仍然是 `ExpressionNode`，而不是 `ConstValue`

因此从设计上看：

```text
ConstexprEngine       = 求值器
DefaultExprSimplifier = 重写器
```

但从代码实现上看，两者已经出现明显重复：

- 数值运算重复
- 比较逻辑重复
- 布尔折叠重复
- 函数求值能力不共享

这对后续提案是一个很重要的观察点。

## 6. 生产主路径：`DefaultPredicateReducer`

## 6.1 为什么真正生效的是它

在当前仓库里，归一化主路径 `ClickHouseSqlNormalizer` 会：

1. 把 `WHERE` 顶层 `AND` 链拆开
2. 识别主键相关过滤条件
3. 将这些过滤条件原样收集到 `primaryKeyFilterTemplates`
4. 调用 `DefaultPredicateReducer.reduce(...)`
5. 把归约结果写入 `primary_key_filter_template`

所以用户最终能在 DuckDB 里看到的"常量执行结果"，实际上主要来自谓词归约器，而不是 AST 求值器。

## 6.2 输入不是 AST，而是字符串过滤条件

`DefaultPredicateReducer` 的输入是：

```text
List<String> andChain
```

即已经被拆开的过滤条件文本，例如：

```text
[
  "concat (todate (ts),' ',lpad (tostring (tohour (todatetime (ts))),2,'0 ')) >= '2026-04-07 04'",
  "ts >= 1775508660"
]
```

这也是当前设计里一个非常关键的现实：

- AST 路径和生产归约路径并没有统一
- 主键证明依赖的是"字符串解析 + 规则系统"，不是 AST 重写

## 6.3 归约流程

整体流程如下：

```text
and-chain filters
      |
      v
+-----------------------+
| PredicateUnitParser   |
| parse each filter     |
+-----------+-----------+
            |
            v
+-----------------------+
| PredicateUnit         |
| subject/op/constant   |
+-----------+-----------+
            |
            v
+-----------------------+
| collectBindings       |
| direct field bounds   |
+-----------+-----------+
            |
            v
+-----------------------+
| collectRanges         |
| lower/upper/exact     |
+-----------+-----------+
            |
            v
+-----------------------+
| prove(...)            |
| range rules first     |
| then binding rules    |
+-----------+-----------+
            |
    +-------+--------+
    |       |        |
    v       v        v
  TRUE    FALSE   UNKNOWN
    |       |        |
    |       |        |
remove   output     keep
filter   false      filter
```

更细一点的证明链路：

```text
derived filter: f(ts) >= c1
direct filter : ts    >= c2
                         |
                         v
                 build FieldBinding
                         |
                         v
                substitute ts := c2
                         |
                         v
          ClickHouseExpressionEvaluator
          + PredicateRuleRegistry
                         |
                         v
                eval f(c2) >= c1
                         |
           +-------------+-------------+
           |                           |
           v                           v
         true                        false/unk
           |                           |
           v                           v
   derived filter redundant      keep or contradiction
```

## 6.4 核心抽象

### `PredicateUnit`

单个过滤条件被解析为：

```text
PredicateUnit(
  subject,
  operator,
  constant,
  originalText
)
```

其中 `subject` 分三类：

```text
Field(name, text)
Derived(expression, baseField)
Opaque(text)
```

含义如下：

- `Field`：直接字段谓词，比如 `ts >= 1`
- `Derived`：派生表达式谓词，比如 `toDate(ts) = '2026-04-07'`
- `Opaque`：看不懂但先保留

### `PredicateOperator`

只支持：

- `=`
- `!=`
- `>`
- `>=`
- `<`
- `<=`

它还带两个重要派生能力：

- `boundSide()`: `LOWER / UPPER / EXACT / NONE`
- `inclusive()`

这使得后续可以把 `ts >= c`、`ts < c` 抽象成上下边界。

### `FieldBinding`

直接字段过滤条件被转成：

```text
FieldBinding(
  field,
  side,
  inclusive,
  value,
  operator,
  sourceText
)
```

### `FieldRange`

多个 `FieldBinding` 会进一步聚合成：

```text
FieldRange(
  field,
  lower,
  upper,
  exact
)
```

它代表某个字段在当前 AND 链里能提炼出的最强边界集合。

## 6.5 `PredicateUnitParser` 的策略

这个解析器是字符串级别的轻量实现，不依赖 AST。它做了几件事：

1. 扫描表达式，找最外层比较运算符
2. 支持跳过字符串字面量和括号嵌套
3. 只把一侧可识别为常量的比较式转成 `PredicateUnit`
4. 如果常量在左边，就自动反转运算符

例如：

```text
1775508660 <= ts
```

会被解析成：

```text
ts >= 1775508660
```

局限也很明显：

- 只理解比较表达式
- 不理解 `IN / NOT IN / IS NULL / BETWEEN`
- `Derived` 的识别只是"表达式文本里包含主键字段名"
- 并没有真正构造表达式 AST

因此它是够用型实现，而不是通用谓词解析框架。

## 6.6 规则注册表：`PredicateRuleRegistry`

谓词归约依赖 `PredicateRuleRegistry` 聚合四类规则：

```text
FunctionRule
BindingRule
DerivedPredicateRule
RangePredicateRule
```

默认注册过程：

```text
defaultRegistry()
  |
  +-- CorePredicateRules
  +-- ClickHouseTimeKeyRules
  `-- ServiceLoader(ReductionRulePlugin)
```

这意味着系统已经具备插件扩展点，项目外也可以通过 `ServiceLoader` 注入规则。

## 7. 规则实现

## 7.1 `CorePredicateRules`

`CorePredicateRules` 目前主要做一件事：把"直接主键谓词"提炼成绑定。

### 绑定生成条件

必须同时满足：

- `subject` 是 `Field`
- 字段名等于 `context.primaryKey()`
- 常量可识别
- 运算符具有边界语义

### 时间字面量强制转换

如果主键常量是字符串时间：

```text
'2026-04-07 10:01:00 +08:00'
```

会借助 `TimestampLiteralParser` 转成 epoch second，再参与后续比较。

因此当前归约器天然支持两种主键边界输入：

- 数字 epoch
- 时间字符串

## 7.2 `ClickHouseTimeKeyRules`

这是最关键的领域规则插件，负责两块能力：

- 注册函数求值规则
- 注册主键派生谓词证明规则

### 支持的函数

```text
concat
toString
lpad
toDateTime
toDate
toHour
```

### 派生谓词证明

它处理的是下面这种模式：

```text
derived: f(ts) op c1
binding: ts    op c2
```

只要满足：

- `derived.subject` 是 `Derived`
- `baseField == binding.field`
- 运算符方向一致，或者绑定是 `EXACT`

就会执行：

```text
1. 用 binding.value 绑定 ts
2. 计算 f(ts)
3. 比较 f(ts) op c1
4. 得出 TRUE / FALSE / UNKNOWN
```

### 方向一致性

这里的方向一致性是安全证明的关键：

```text
lower-bound family: >, >=
upper-bound family: <, <=
exact family      : =
```

例如：

```text
concat(...) >= '2026-04-07 04'
ts >= 1775508660
```

两个都是 lower-bound，可以尝试证明冗余。

但：

```text
concat(...) <= '2026-04-07 04'
ts >= 1775508660
```

方向相反，就不会贸然裁剪。

### 范围证明

`ClickHouseTimeKeyRules` 还支持范围级证明，主要面向等值派生谓词：

```text
toDate(ts) = '2026-04-07'
toHour(toDateTime(ts)) = 10
```

当主键有下界和上界时，它会：

1. 计算范围端点值
2. 对派生表达式分别求下端点和上端点
3. 如果上下端点落在同一 bucket
4. 且两端都满足等值条件
5. 则认为整个范围内该派生谓词恒真，可以移除

流程如下：

```text
ts >= lower
ts <  upper
   |
   v
normalize endpoints
   |
   v
eval f(lower), f(upper')
   |
   v
same bucket ?
   |
   +-- no  -> unknown
   |
   `-- yes
        |
        v
   both equal expected ?
        |
        +-- yes -> true
        `-- no  -> false/unknown
```

这里的 `upper'` 指对排他上界做一秒回退，例如：

```text
ts < 1775527380
```

会变成：

```text
upper endpoint = 1775527379
```

这个细节保证了时间桶证明不会把开区间误当成闭区间。

## 6.7 当前派生谓词证明主流程

在当前实现里，"派生谓词证明"专指这类判断：

```text
已知直接主键条件:
  ts >= 1775508660

是否可以证明派生条件恒真:
  concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 ')) >= '2026-04-07 04'
```

也就是：

- 从一组主键相关过滤条件里抽出直接事实
- 用这些事实去证明派生表达式谓词是否冗余
- 若能证明恒真，则删除派生谓词
- 若能证明恒假，则整组条件折叠为 `false`
- 否则原样保留

这里要特别注意一个实践约束：

- 实践中，派生谓词大多是单边比较，也就是 `>=` 或 `<=`
- 而直接主键 range 表达式通常来自上下边界组合，因此可出现 `>`、`>=`、`<`、`<=`

可以把典型输入理解为：

```text
derived predicate:
  f(ts) >= c
  f(ts) <= c

range predicates:
  ts >  lower
  ts >= lower
  ts <  upper
  ts <= upper
```

这也是为什么当前证明系统会把：

- 派生谓词证明，重点建模成 lower-bound / upper-bound family 对齐问题
- range 事实提取，重点建模成 `FieldRange(lower, upper, exact)`

而不是假设所有输入都会落成 `=`。

当前真实链路如下：

```text
primary-key related WHERE filters
              |
              v
+-----------------------------------+
| ClickHouseSqlNormalizer           |
| reducePrimaryKeyExpressions(...)  |
+----------------+------------------+
                 |
                 v
+-----------------------------------+
| DefaultPredicateReducer           |
| reduceExpressions(andChain)       |
+----------------+------------------+
                 |
                 v
+-----------------------------------+
| PredicateFactExtractor            |
| parse each expr -> PredicateUnit  |
+----------------+------------------+
                 |
     +-----------+-----------+
     |                       |
     v                       v
+-----------+         +---------------+
| bindings   |         | ranges        |
| FieldBinding         | FieldRange    |
+-----+-----+         +-------+-------+
      |                         |
      +-----------+-------------+
                  |
                  v
      iterate each original expression
                  |
                  v
+-----------------------------------+
| PredicateUnitParser               |
| classify expr as                  |
| Field / Derived / Opaque          |
+----------------+------------------+
                 |
     +-----------+-----------+
     |                       |
     | not Derived           | Derived
     |                       |
     v                       v
 keep original        +----------------------+
 expression           | prove(derived, facts)|
                      +----------+-----------+
                                 |
                +----------------+----------------+
                |                                 |
                v                                 v
      rangePredicateRules first         derivedPredicateRules second
                |                                 |
                |                                 |
                +----------------+----------------+
                                 |
                                 v
                     +----------------------+
                     | ProofResult          |
                     | TRUE/FALSE/UNKNOWN   |
                     +----+-----------+-----+
                          |           |
             +------------+           +-------------+
             |                                      |
             v                                      v
      TRUE -> remove                         UNKNOWN -> keep
      FALSE -> contradiction                 original expr
               output false
```

更细一点，单条派生谓词证明内部又分成两条证明通道：

```text
derived predicate
  f(ts) op const
      |
      v
+---------------------------+
| prove(derived, facts)     |
+-------------+-------------+
              |
   +----------+----------+
   |                     |
   v                     v
range proof         binding proof
first               fallback
   |                     |
   v                     v
+-----------+      +------------------+
| FieldRange |      | FieldBinding     |
| ts range   |      | ts := value      |
+-----+-----+      +---------+--------+
      |                        |
      v                        v
eval f(lower), f(upper')   eval f(binding.value)
      |                        |
      v                        v
compare with expected      compare with expected
      |                        |
      +-----------+------------+
                  |
                  v
        ProofResult(TRUE/FALSE/UNKNOWN)
```

可以把它理解成：

```text
Fact extraction
  -> Rule dispatch
  -> Boundary substitution
  -> Expression evaluation
  -> Truth decision
  -> Rewrite original filter list
```

### 当前证明顺序

当前实现里，证明顺序是固定的：

1. 先对整组主键条件提取 `FactSet`
2. 对每个原始表达式再次分类
3. 只有 `PredicateSubject.Derived` 才进入证明
4. 证明时先跑 `rangePredicateRules`
5. 若仍然 `UNKNOWN`，再跑 `derivedPredicateRules`
6. 得到 `TRUE/FALSE/UNKNOWN` 后决定删除、矛盾折叠或保留

这个顺序对应代码中的：

- [DefaultPredicateReducer](file:///Users/bytedance/Downloads/sql-ast-analyzer/src/main/java/com/example/sqlast/eval/DefaultPredicateReducer.java)
- [PredicateFactExtractor](file:///Users/bytedance/Downloads/sql-ast-analyzer/src/main/java/com/example/sqlast/eval/PredicateFactExtractor.java)
- [PredicateUnitParser](file:///Users/bytedance/Downloads/sql-ast-analyzer/src/main/java/com/example/sqlast/eval/PredicateUnitParser.java)

### 绑定证明是怎么做的

绑定证明对应：

```text
derived: f(ts) >= c1   or   f(ts) <= c1
binding: ts    >= c2   or   ts >  c2
binding: ts    <= c3   or   ts <  c3
```

实践里最常见的是：

```text
derived lower-bound:
  f(ts) >= c1

direct lower-bound:
  ts >= c2
  ts >  c2
```

或者：

```text
derived upper-bound:
  f(ts) <= c1

direct upper-bound:
  ts <= c2
  ts <  c2
```

流程如下：

```text
PredicateSubject.Derived
       |
       v
check baseField == binding.field
       |
       v
check operator family aligned
       |
       v
substitute ts := binding.value
       |
       v
ClickHouseExpressionEvaluator
  + PredicateRuleRegistry
       |
       v
evaluate f(ts)
       |
       v
compare evaluated result with c1
       |
       v
TRUE    -> derived predicate redundant
FALSE   -> contradiction only when binding is EXACT
UNKNOWN -> keep original predicate
```

实现入口在 [ClickHouseTimeKeyRules.proveDerivedPrimaryKeyPredicate](file:///Users/bytedance/Downloads/sql-ast-analyzer/src/main/java/com/example/sqlast/eval/ClickHouseTimeKeyRules.java#L36-L67)。

这里的 `aligned` 不是要求运算符完全相同，而是要求它们属于同一边界家族：

```text
lower-bound family:
  >
  >=

upper-bound family:
  <
  <=

exact:
  =
```

因此：

```text
f(ts) >= c1   with   ts >  c2
f(ts) >= c1   with   ts >= c2
f(ts) <= c1   with   ts <  c2
f(ts) <= c1   with   ts <= c2
```

都属于可尝试证明的形状。

但：

```text
f(ts) >= c1   with   ts <= c2
f(ts) <= c1   with   ts >= c2
```

方向相反，就不会进入安全裁剪。

### 范围证明是怎么做的

范围证明对应：

```text
derived: f(ts) = const
range  : ts >/= lower and ts </= upper
```

也就是说，range 侧在实践中天然允许 4 种边界运算符：

```text
lower side:
  ts >  lower
  ts >= lower

upper side:
  ts <  upper
  ts <= upper
```

当前实现会先把这些边界统一抽象成：

```text
FieldRange(
  lower = (value, inclusive/exclusive),
  upper = (value, inclusive/exclusive),
  exact = optional
)
```

然后再做端点求值与一秒回退处理。

流程如下：

```text
FieldRange
   |
   v
materialize lower endpoint and upper endpoint
   |
   v
for exclusive upper bound, use upper - 1 second
   |
   v
evaluate f(lower)
evaluate f(upper')
   |
   v
same bucket / same derived value ?
   |
   +-- both equal const -> TRUE
   +-- both differ      -> FALSE
   `-- otherwise        -> UNKNOWN
```

实现入口在 [ClickHouseTimeKeyRules.proveDerivedPrimaryKeyRangePredicate](file:///Users/bytedance/Downloads/sql-ast-analyzer/src/main/java/com/example/sqlast/eval/ClickHouseTimeKeyRules.java#L69-L121)。

这里也要强调一个边界：

- 当前代码里的 range proof 主要面向 `derived.operator == EQ`
- 也就是它最擅长证明 `toDate(ts) = '2026-04-07'`、`toHour(...) = 10` 这类"桶等值谓词"
- 对实践中更常见的 `derived >= ...` / `derived <= ...`，当前主要还是依赖 binding proof，而不是 range proof

所以从实践分布看，可以总结成：

```text
most common:
  derived >= / <=
  + direct ts >/>=/</<=
  -> binding proof

bucket equality special case:
  derived = const
  + direct ts range
  -> range proof
```

### 最终改写结果

证明结果对过滤条件列表的影响只有三种：

```text
TRUE
  -> 删除当前派生谓词

FALSE
  -> 认为当前主键条件组矛盾
  -> 输出单个 false

UNKNOWN
  -> 保留原表达式
```

ASCII 汇总如下：

```text
derived predicate
      |
      v
   prove(...)
      |
  +---+---+------------------+
  |       |                  |
  v       v                  v
 TRUE   FALSE             UNKNOWN
  |       |                  |
  v       v                  v
drop    whole chain       keep
expr    => false          expr
```

这就是当前仓库里"派生谓词证明"真正影响归一化输出的完整流程。

### 不同场景与证明通道对照表

为了更直观看出当前能力边界，以及后续 proposal 应该优先补哪些点，可以把不同场景收敛成下面这张三列表：

| 分类 | 内容 | 说明 |
| --- | --- | --- |
| 当前支持 | `derived >= / <= / > / <` + 同向 direct binding `-> binding proof` | 当前最稳定、最常见的主路径，依赖 `boundSide()` 家族对齐 |
| 当前支持 | `derived = const` + direct primary-key range `-> range proof` | 主要用于 `toDate(ts)=...`、`toHour(...)=...` 这类 bucket equality proof |
| 当前支持 | `derived ...` + direct exact binding `ts = c` | `binding.side == EXACT` 时能力最强，既可能证明冗余，也可能证明矛盾 |
| 当前支持 | direct range 操作符覆盖 `>`、`>=`、`<`、`<=` | 会先抽象成 `FieldRange(lower, upper, exact)`，再进入 range proof |
| 当前支持 | 派生谓词通常是 `>=` 或 `<=` 的实践主流场景 | 这也是现在最值得依赖的证明通道 |
| 建议扩展 | `derived != const` 的可证明冗余/矛盾 | 当前几乎不做，后续可结合 monotonicity 或 complement reasoning 扩展 |
| 建议扩展 | `derived >= / <=` + 完整 direct range 的区间级证明 | 现在 range proof 主要只处理 `derived = const`，对单边派生区间还不够强 |
| 建议扩展 | 非比较类派生条件进入统一证明主路径 | 如 `IN`、`LIKE`、`IS NULL`、`CASE` 相关派生谓词，目前大多还停留在 AST 语义层，不进入证明裁剪 |
| 建议扩展 | 多字段派生表达式或多列联合证明 | 当前 `PredicateSubject.Derived` 本质仍围绕单个 `baseField` 设计 |
| 建议扩展 | 结构化 explain / proof trace | 当前 `ProofStep(stage, detail)` 足够调试，但不够支撑 proposal 里的 explain/report 场景 |
| 风险点 | 方向相反时如果强行证明，容易产生不安全裁剪 | 例如 `f(ts) >= c` 配 `ts <= x`，当前保守地不裁剪是合理的 |
| 风险点 | 单侧边界不足时，range proof 容易误推整体恒真 | 缺少完整 lower+upper 时，应优先返回 `UNKNOWN`，不能过度乐观 |
| 风险点 | bucket proof 对时区、开闭区间和端点回退非常敏感 | `upper - 1 second` 这类细节一旦处理错，就会跨天误判 |
| 风险点 | 现有证明仍建立在有限规则和领域函数之上 | 新函数、新时间语义、新类型转换若未补规则，容易出现 `UNKNOWN` 或能力漂移 |
| 风险点 | `PredicateUnitParser` 仍是比较谓词优先，不是完整谓词分类器 | 复杂派生表达式虽然已进入 AST，但是否能进入证明主路径仍取决于当前分类能力 |

如果把这张表再压成一句话，可以概括为：

```text
当前实现最强的是：
  单边派生谓词 + 同向主键绑定
以及：
  桶等值派生谓词 + 完整主键范围

后续扩展重点是：
  更多操作符
  更强区间证明
  更完整谓词类型
  更结构化 explain

主要风险在于：
  方向误判
  端点误判
  时区误判
  以及过度证明导致的不安全裁剪
```

## 8. 字符串求值器：`ClickHouseExpressionEvaluator`

谓词证明阶段并不使用 AST `ConstexprEngine`，而是使用 `ClickHouseExpressionEvaluator`。

这是另一套轻量表达式求值器，特点如下：

- 输入是字符串表达式
- 可以解析字符串、数字、标识符、函数调用
- 不支持通用二元算术表达式树
- 依赖 `PredicateRuleRegistry.evaluateFunction(...)` 调用函数规则

它的能力边界可以概括成：

```text
支持:
  'abc'
  123
  -1
  concat(...)
  todate(ts)
  tohour(todatetime(ts))
  quoted_identifier

不支持:
  a + b
  x * y
  complex nested operators
  SQL 完整表达式语法
```

所以它更像是"为了主键派生证明而定制的函数表达式解释器"。

## 9. 归一化接入点：`ClickHouseSqlNormalizer`

## 9.1 `WHERE` 处理

`ClickHouseSqlNormalizer` 在 `visitWhereClause` 里会：

1. 把顶层 `AND` 链拆开
2. 判定哪些过滤条件和主键相关
3. 非主键过滤条件走 `InListIntersectionRule`
4. 主键过滤条件保留原文本
5. 在 `primaryKeyFilterTemplates()` 里调用 `DefaultPredicateReducer`

其决策模型如下：

```text
WHERE expr
  |
  v
split top-level AND
  |
  +-- business filter --------------------+
  |                                       |
  |                               InListIntersectionRule
  |                                       |
  |                                       v
  |                              whereBusinessFilters
  |
  `-- primary-key related filter ---------+
                                          |
                                          v
                              DefaultPredicateReducer
                                          |
                                          v
                              primaryKeyFilterTemplate
```

## 9.2 主键过滤条件识别策略

当前 `isPrimaryKeyFilter(...)` 是基于文本匹配的启发式实现：

- 文本里包含主键字段名
- 或包含 `todate(pk)`
- 或包含 `tohour(todatetime(pk))`

这意味着：

- 实现简单
- 但存在误判/漏判空间
- 对更复杂的派生表达式支持不够泛化

## 9.3 输出到存储

归一化结果会输出两套主键相关结果：

- `primaryKeyFilters`
  原始主键相关过滤条件
- `primaryKeyFilterTemplate`
  经谓词归约后的结果

`DuckDbAnalysisWriter` 后续又会使用 `primaryKeyFilterTemplate` 推导 `queryRangeSeconds`，所以当前常量执行引擎的产物不仅影响模板文本，也影响时间范围统计。

## 10. 独立组件：`DateBucketPartitionHintAnalyzer`

这个组件处理的是 AST 级 partition hint 判定，目前只支持：

```text
toDate(ts) = 'yyyy-MM-dd'
```

它会拿这个 hint 去和已抽取出的 `TimeRangeResult` 比较：

- 若时间范围完全落在同一天，则认为该 hint 冗余
- 否则认为它是 active hint

流程如下：

```text
candidate expr
  |
  v
parse toDate(pk) = 'date'
  |
  +-- fail -> not partition hint
  |
  `-- success
        |
        v
   compare with each time range
        |
        +-- fully covered -> redundant
        `-- not covered   -> active
```

这和 `DefaultPredicateReducer` 的目标接近，但两者仍然是分离实现：

- 一个基于 AST
- 一个基于字符串过滤条件
- 一个分析 partition hint
- 一个分析主键派生谓词

而且当前代码里尚未看到它被接入主归一化流程。

## 11. Trace 与测试覆盖

## 11.1 Trace

当前常量执行相关模块都会向 `AnalysisTrace` 记录事件：

- `ConstexprEngine` 记录 `constexpr/evaluate`
- `DefaultExprSimplifier` 记录 `constexpr/fold`、`constexpr/bool-eval`
- `DefaultPredicateReducer` 记录 `predicate-reducer/remove-true`、`predicate-reducer/contradiction`、`predicate-reducer/proof`
- `DateBucketPartitionHintAnalyzer` 记录 `partition-hint/redundant` 或 `partition-hint/active`

这使得后续如果要做 explain/debug/report，是有基础设施可以接上的。

## 11.2 测试证明了什么

从现有测试看，系统已经覆盖了几个关键场景：

### `ConstexprEngineTest`

- 算术比较求值
- `AND` 短路
- 字符串函数链求值
- 时区相关时间函数求值
- 未知标识符/未知函数回退为 `UNKNOWN`

### `DefaultExprSimplifierTest`

- 常量比较折叠
- 布尔恒等式化简
- 符号表达式保持不变

### `DefaultPredicateReducerTest`

- lower-bound 主键派生谓词裁剪
- 方向不一致时保持原样
- 等值矛盾折叠为 `false`
- 范围内同 bucket 的 `toDate` / `toHour` 等值谓词裁剪
- 字符串时间主键边界解析

### `DateBucketPartitionHintAnalyzerTest`

- `toDate(ts)=date` 被时间范围覆盖时判定为冗余
- 跨天范围时保持 active

## 12. 当前实现的关键观察

这是最值得为后续提案保留的一节。

## 12.1 现在实际上存在两套求值器

```text
AST evaluator    : ConstexprEngine
string evaluator : ClickHouseExpressionEvaluator
```

两者服务不同路径，但也带来几个问题：

- 函数支持需要重复维护
- 时间/字符串语义容易漂移
- 后续新规则要决定接在哪一边

## 12.2 AST 简化器和 AST 求值器存在重复实现

`DefaultExprSimplifier` 没有复用 `ConstexprEngine`，而是自己实现了一套：

- 常量比较
- 数值运算
- 布尔恒等式

这说明当前代码更像"先做出功能，再逐步收敛架构"，还没有形成统一的执行内核。

## 12.3 生产主路径依赖字符串启发式，而不是 AST 语义

当前真正会影响归一化结果的是：

- `PredicateUnitParser`
- `ClickHouseExpressionEvaluator`
- `isPrimaryKeyFilter(...)`

它们都偏文本/启发式。

优点：

- 实现快
- 规则开发成本低
- 对当前已知模式足够有效

缺点：

- 通用性弱
- 复杂语法场景可靠性有限
- 对未来扩展更多函数模式时，字符串解析复杂度会快速上升

## 12.4 `EvalContext` 还没有被充分用起来

`EvalContext` 当前包含：

- `timezone`
- `columnTypes`
- `nullSemantics`

但就现有实现看：

- `timezone` 被广泛使用
- `columnTypes` 基本未参与求值/证明
- `nullSemantics` 目前也没有真正驱动求值分支

这说明上下文模型已经预留了扩展位，但引擎语义还没完全填满。

## 12.5 已有插件机制，但规则粒度还偏粗

`PredicateRuleRegistry` 已经支持通过 `ServiceLoader` 扩展，是很好的基础。

但目前插件主要集中在：

- 函数求值
- 绑定证明
- 范围证明

还没有统一到更高层的：

- AST 级可组合执行规则
- 统一的类型提升/类型转换策略
- 更系统的 null semantics
- 更完整的可解释证明模型

## 12.6 partition hint 分析与 predicate reducer 仍是两条线

它们都在证明"某个派生时间条件是否冗余"，但：

- 输入模型不同
- 复用能力有限
- 没有统一决策出口

如果后续提案要统一主键提示、时间桶谓词、派生过滤条件证明，这里很可能是一个重构汇合点。

## 13. 对后续提案最有价值的切入点

如果后面要提出新的常量执行引擎方案，我建议优先关注以下问题。

### 13.1 是否统一执行内核

当前可以考虑收敛成：

```text
single evaluation core
  |
  +-- AST evaluation
  +-- AST simplification
  +-- predicate proof
  `-- partition hint analysis
```

这样可以减少：

- 函数实现重复
- 时间语义漂移
- 文本路径和 AST 路径的能力分叉

### 13.2 是否把生产主路径从字符串归约迁移到 AST 归约

这是最大的结构性问题。

如果继续沿字符串路径演化：

- 开发快
- 改造小
- 但复杂度会继续堆在启发式解析上

如果迁移到 AST 路径：

- 初始改造大
- 但长期可维护性更高
- 且能更自然支持更多表达式类型

### 13.3 是否统一"证明"与"执行"

当前证明逻辑本质上是：

```text
代入边界值
  -> 执行派生表达式
  -> 比较
  -> 得出 truth
```

这已经非常接近一个小型证明执行机了。

后续完全可以把它提升为更显式的模型：

```text
Bind
Evaluate
Compare
Prove
Rewrite
Trace
```

### 13.4 是否让 Trace 结构化升级

现在的 `ProofStep(stage, detail)` 已够用，但如果后续提案要支撑：

- explain 输出
- 调试 UI
- 冗余裁剪报告
- 回归用例生成

则可以考虑把 trace 从字符串详情升级为结构化字段。

## 14. 一句话总结

当前仓库里的"常量执行引擎"并不是单点模块，而是一组围绕常量求值、表达式折叠、主键派生谓词证明而逐步长出来的能力集合。

其中：

- `ConstexprEngine` 和 `DefaultExprSimplifier` 代表 AST 世界的基础能力
- `DefaultPredicateReducer` 代表当前真正进入生产归一化链路的证明引擎
- `DateBucketPartitionHintAnalyzer` 代表还未并入主链路的辅助分析能力

如果后续要做新提案，最有价值的方向不是再加一套新逻辑，而是把这几条能力线收敛成一个统一、可扩展、可解释的执行与证明框架。

## 15. 统一执行与证明框架提案

这一节给出一个可以直接用于后续设计讨论的统一方案。目标不是简单把几个类合并，而是收敛成一套共享抽象、共享语义、共享 trace 的执行内核。

## 15.1 设计目标

统一框架需要同时满足以下目标：

- 统一输入模型：生产链路不再依赖字符串启发式和 AST 路径并存
- 统一执行语义：函数求值、布尔求值、时间转换、类型提升只保留一套
- 统一证明流程：绑定、比较、范围证明、hint 冗余判定都走同一套 prove pipeline
- 可扩展：函数规则、领域规则、单调性规则、重写规则都可以插件化注册
- 可解释：每一步执行和证明都输出结构化 trace，支持 explain、调试和回归
- 可渐进迁移：允许先兼容现有归一化结果，再逐步替换旧路径

## 15.2 新的总览架构

建议把现有能力线收敛成如下架构：

```text
                           +----------------------+
                           | UnifiedExprEngine    |
                           | execution core       |
                           +----------+-----------+
                                      |
        +-----------------------------+-----------------------------+
        |                             |                             |
        v                             v                             v
+------------------+        +-------------------+        +-------------------+
| Evaluator        |        | Prover            |        | Rewriter          |
| expr -> value    |        | facts -> truth    |        | expr -> expr      |
+--------+---------+        +---------+---------+        +---------+---------+
         |                            |                            |
         v                            v                            v
  ConstValue / Unknown         ProofResult / Facts          SimplifyResult
         |                            |                            |
         +----------------+-----------+----------------+-----------+
                          |                            |
                          v                            v
                 +-------------------+        +--------------------+
                 | Trace Recorder    |        | Rule Registry      |
                 | structured steps  |        | pluggable rules    |
                 +-------------------+        +--------------------+
```

这里的核心变化是：

- `ConstexprEngine`、`DefaultExprSimplifier`、`ClickHouseExpressionEvaluator` 不再是彼此独立的执行器
- 它们变成同一执行内核在不同 mode 下的三种使用方式
- `DefaultPredicateReducer` 和 `DateBucketPartitionHintAnalyzer` 不再自己维护一套局部语义，而是复用 `Prover`

统一后的端到端流程建议固定为：

```text
raw sql / filter expr
        |
        v
+-------------------------------+
| parser / expression builder   |
| query AST + predicate AST     |
+---------------+---------------+
                |
                v
+-------------------------------+
| UnifiedExprEngine             |
| mode = EVALUATE / SIMPLIFY /  |
|        PROVE                  |
+---------------+---------------+
                |
    +-----------+------------+------------------+
    |                        |                  |
    v                        v                  v
+-----------+        +---------------+   +---------------+
| Evaluator |        | Fact Extractor|   | Rewriter      |
| const eval|        | range/binding |   | remove/fold   |
+-----+-----+        +-------+-------+   +-------+-------+
      |                      |                   |
      v                      v                   v
 ConstValue              FactSet             rewritten AST
      |                      |                   |
      +-----------+----------+-------------------+
                  |
                  v
        +---------------------------+
        | Prover                    |
        | redundancy / contradiction|
        | hint coverage / equality  |
        +-------------+-------------+
                      |
                      v
        +---------------------------+
        | Trace / Explain           |
        | structured proof steps    |
        +-------------+-------------+
                      |
                      v
        +---------------------------+
        | renderer / normalizer     |
        | canonical_sql             |
        | template_sql              |
        | primary_key_filter_*      |
        +---------------------------+
```

## 15.3 统一抽象

### 1. 表达式统一为 AST

统一框架的首要原则是：

```text
all execution happens on AST
```

也就是：

- 归一化主链路内部不再以 `List<String>` 作为证明输入
- 顶层 AND 链仍然可以拆分，但拆出来的是 `ExpressionNode`
- `PredicateUnit` 不再从字符串解析，而是从 AST 分类得到

建议的新模型：

```text
QueryPredicate
  - originalExpr      : ExpressionNode
  - normalizedExpr    : ExpressionNode
  - subject           : PredicateSubject
  - operator          : PredicateOperator
  - constant          : ConstValue
  - metadata          : PredicateMetadata
```

### 2. 常量值模型统一

现有 `ConstValue` 可以保留，但建议扩展成更明确的值域：

```text
ConstValue
  - kind
  - value
  - nullability
  - source
```

其中：

- `kind`：数据类型
- `value`：实际值
- `nullability`：是否是 null / unknown / known
- `source`：literal、bound-substitution、derived-eval 等来源

这样 trace 和 explain 可以更清晰地告诉用户：

```text
这个值是原始字面量
还是由 ts := 1775508660 代入后求得
```

### 3. 事实模型统一

证明系统不应只依赖 `FieldBinding`。建议引入统一事实层：

```text
Fact
  - RangeFact(field, lower, upper, exact)
  - EqualityFact(field, value)
  - DerivedFact(expr, truth)
  - TimeBucketFact(field, granularity, zone)
  - HintCoverageFact(expr, coveredByRange)
```

这样可以把：

- 直接主键边界
- 时间桶等值
- partition hint 覆盖关系
- 已证明为 true/false 的派生谓词

全部放进同一事实空间中复用。

## 15.4 统一执行模式

同一个引擎提供 3 种 mode 即可覆盖现有全部需求。

| mode | 输入 | 输出 | 对应当前能力 |
| --- | --- | --- | --- |
| `EVALUATE` | `ExpressionNode` + bindings | `ConstValue` | `ConstexprEngine`、`ClickHouseExpressionEvaluator` |
| `SIMPLIFY` | `ExpressionNode` + facts | `ExpressionNode` | `DefaultExprSimplifier` |
| `PROVE` | `ExpressionNode` + facts + target | `ProofResult` | `DefaultPredicateReducer`、`DateBucketPartitionHintAnalyzer` |

运行方式示意：

```text
ExpressionNode
     |
     +--> engine.evaluate(...)  -> ConstValue
     +--> engine.simplify(...)  -> ExpressionNode
     `--> engine.prove(...)     -> ProofResult
```

这样几个现有模块的角色就会变化为：

- `ConstexprEngine` 退化为 facade，内部调用 unified engine 的 `evaluate`
- `DefaultExprSimplifier` 退化为 facade，内部调用 unified engine 的 `simplify`
- `DefaultPredicateReducer` 只负责编排 AND-chain，不再自带求值语义
- `DateBucketPartitionHintAnalyzer` 只负责构造 prove target，不再手写比较逻辑

## 15.5 执行管线

建议统一框架的单次执行管线如下：

```text
input expression
      |
      v
+-------------------------+
| normalize ast           |
| canonical operators     |
+------------+------------+
             |
             v
+-------------------------+
| infer type / subject    |
| field, derived, literal |
+------------+------------+
             |
             v
+-------------------------+
| bind known facts        |
| ts := c, range, eq      |
+------------+------------+
             |
             v
+-------------------------+
| evaluate subexpressions |
| deterministic only      |
+------------+------------+
             |
             v
+-------------------------+
| prove / simplify        |
| compare / fold / cover  |
+------------+------------+
             |
             v
+-------------------------+
| rewrite result          |
| keep / remove / false   |
+------------+------------+
             |
             v
+-------------------------+
| emit trace              |
| structured proof steps  |
+-------------------------+
```

这条管线的关键价值在于：

- 执行和证明只共享一套类型与函数语义
- 局部折叠和范围证明可以发生在同一套 AST 上
- hint 覆盖也能作为 prove target 复用事实层

## 15.6 规则系统重构

建议将现有 `PredicateRuleRegistry` 升级为更高层的 `ExecutionRuleRegistry`。

### 新的规则分类

| 规则类型 | 作用 | 示例 |
| --- | --- | --- |
| `ValueRule` | 求值某类字面量或运算 | 数字计算、字符串拼接 |
| `FunctionRule` | 求值确定性函数 | `toDate`、`toHour`、`concat` |
| `TypeRule` | 统一类型提升与转换 | int -> float、string -> timestamp |
| `BindingRule` | 从谓词提取事实 | `ts >= c` -> `RangeFact` |
| `ProofRule` | 基于事实证明目标 | `f(ts) >= x` 被 `ts >= c` 覆盖 |
| `RewriteRule` | 根据证明结果改写 AST | `true and x -> x` |
| `ExplainRule` | 生成人类可读解释 | why removed / why unknown |

### 注册方式

```text
ExecutionRuleRegistry.defaultRegistry()
  |
  +-- CoreValueRules
  +-- CoreBooleanRules
  +-- CoreTypeRules
  +-- ClickHouseTimeRules
  +-- ClickHouseProofRules
  `-- ServiceLoader(ExecutionRulePlugin)
```

这比当前只注册 `FunctionRule / BindingRule / DerivedPredicateRule / RangePredicateRule` 更完整，也更接近一个真正的执行内核。

## 15.7 统一证明接口

当前 `proveDerivedPrimaryKeyPredicate`、`proveDerivedPrimaryKeyRangePredicate`、partition hint 判定都可以抽象成同一个接口：

```java
interface ProofRule {
    Optional<ProofResult> prove(
            ExpressionNode target,
            FactSet facts,
            ExecutionContext context,
            TraceRecorder trace);
}
```

其中：

- `target`：要证明的目标表达式，例如 `toDate(ts) = '2026-04-07'`
- `facts`：已知事实，例如 `ts >= 1775526900` 且 `ts < 1775526960`
- `context`：时区、列类型、null 语义、主键信息
- `trace`：逐步记录为什么成功、为什么失败、为什么未知

这样几类问题就能被统一处理：

```text
1. derived predicate redundancy
2. contradiction detection
3. partition hint coverage
4. bucket equality over range
5. constant true/false proof
```

## 15.8 可解释性设计

当前 `AnalysisTrace` 已经有：

- `stage`
- `action`
- `attributes`

这已经是不错的基础。建议保留 `AnalysisTrace`，但在统一框架中约定一套标准事件模型。

例如：

```text
stage=engine
action=bind
attributes:
  field=ts
  value=1775508660
  source=predicate

stage=engine
action=evaluate-function
attributes:
  function=toDate
  input=1775508660
  output=2026-04-07

stage=engine
action=prove-covered
attributes:
  expr=toDate(ts) = '2026-04-07'
  reason=range_same_bucket

stage=engine
action=rewrite-remove
attributes:
  expr=toDate(ts) = '2026-04-07'
```

为了更强 explain 能力，建议把 `ProofStep` 从：

```text
(stage, detail)
```

升级为：

```text
ProofStep
  - phase
  - rule
  - input
  - output
  - decision
  - attributes
```

这样可以直接支持：

- explain 文本
- 调试 UI
- 回归最小复现
- 规则命中统计

## 15.9 归一化链路如何改造

`ClickHouseSqlNormalizer` 的目标不是继续维护多个局部 reducer，而是变成统一框架的 orchestrator。

建议新链路如下：

```text
WHERE AST
  |
  v
split top-level AND into ExpressionNode units
  |
  v
classify each unit
  |
  +-- business predicate
  |      -> generic simplify/template pipeline
  |
  +-- primary-key predicate
  |      -> fact extraction + proof + rewrite
  |
  `-- partition hint predicate
         -> coverage proof + rewrite
  |
  v
render canonical and template outputs
```

核心变化是：

- 主键过滤条件不再先渲染成字符串再证明
- partition hint 不再由旁路分析器独立判定
- 所有删除、保留、矛盾折叠都发生在 AST 上

最终落盘时再统一 render 回字符串：

- `where_business_filters`
- `primary_key_filters`
- `primary_key_filter_template`
- `template_sql`

## 15.10 分阶段迁移方案

为了降低风险，建议分 4 个阶段迁移。

### Phase 1：统一函数语义

目标：

- 把 `ConstexprEngine` 与 `ClickHouseExpressionEvaluator` 的函数实现收敛到同一个 `FunctionRule` 集合
- 不改主链路输入模型

收益：

- 立刻消除函数实现重复
- 降低时间语义漂移风险

### Phase 2：统一 AST 求值与简化

目标：

- 让 `DefaultExprSimplifier` 复用统一 evaluator / rewrite rules
- 形成真正可复用的 `evaluate + simplify` 内核

收益：

- 消除 AST 路径重复逻辑
- 为后续证明迁移打底

### Phase 3：主键证明迁移到 AST

目标：

- 在 `ClickHouseSqlNormalizer` 内保留 AST 形式的 AND unit
- 用 AST classifier 替换 `PredicateUnitParser`
- 用 `FactSet + ProofRule` 替换字符串证明路径

收益：

- 生产链路与 AST 路径统一
- 支持更复杂的派生表达式

### Phase 4：接入 partition hint 与 explain

目标：

- 将 `DateBucketPartitionHintAnalyzer` 改造成 `ProofRule`
- 输出结构化 explain / trace

收益：

- 完成所有能力线收敛
- 支撑提案里更高级的可解释性需求

## 15.11 预期收益

统一后，系统会得到几方面明显提升。

| 维度 | 当前状态 | 统一后 |
| --- | --- | --- |
| 执行语义 | 多套实现并存 | 单一执行内核 |
| 输入模型 | AST 与字符串并存 | AST 为主，字符串只负责渲染 |
| 扩展方式 | 局部规则拼接 | 统一规则注册表 |
| 可解释性 | trace 粒度不统一 | 结构化 proof/explain |
| 维护成本 | 函数和规则重复实现 | 共享规则、共享测试 |
| 风险控制 | 复杂场景依赖启发式 | 更强语义一致性 |

## 15.12 最终建议

如果要正式推进提案，我建议把统一框架定义为：

```text
一个 AST-first 的表达式执行、事实提取、谓词证明、结果重写平台
```

它应当具备以下最小核心：

- 一个统一的 `ExecutionContext`
- 一个统一的 `ExecutionRuleRegistry`
- 一个统一的 `Evaluator / Prover / Rewriter`
- 一个统一的 `FactSet`
- 一个统一的结构化 `Trace` / `Explain` 输出

这样，当前几条分散能力线就能被真正收敛成：

```text
Parse AST
  -> Build facts
  -> Evaluate
  -> Prove
  -> Rewrite
  -> Explain
  -> Render
```

这会比"继续在现有多个局部引擎上叠加新能力"更稳，也更适合作为后续长期演进的底座。
