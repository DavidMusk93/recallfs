# `castNOTNULL` / `castnotnull` 在生成 DAG 时如何处理

本文梳理 `castNOTNULL`（常见写法也会出现 `castnotnull`）在 JobManager 侧的处理路径，重点是它如何影响表达式的返回类型（nullability），以及最终如何进入 DAG。

## 1. 它是什么

在本仓库里，`castNOTNULL` 不是一个显式的 Rust 函数实现；它是 Substrait Plan 里的 *scalar function* 名称，进入逻辑计划后会以 `Expression::Function` 的形式存在。

和普通 `castINT` / `castBIGINT` 等类似，`castNOTNULL` 主要用于“表达式类型层面”的约束：告诉下游该表达式结果不会为 null。

## 2. 生成 DAG 时它出现在哪里

Job 提交生成 DAG 的关键链路（高层）：

1. `Submitting::dispatch_impl` 把 `PhysicsPlan` 转成 `TideExecutionGraph`，然后打包/压缩后下发（submit 阶段）。
   - 参考 [submit.rs](file:///root/Documents/jobmanager/jobmanager/src/managedjob/job/action/submit/submit.rs) 中的 `let graph: TideExecutionGraph = (&*action_context.plan.read().await).into();`
2. `(&PhysicsPlan).into()` 的实现把 `PhysicsPlan` 的 group/operator/task 信息组装成 `TideExecutionGraph`。
   - 参考 [dag.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/dag.rs) 中 `impl Into<TideExecutionGraph> for &PhysicsPlan`

注意：`plan/dag.rs` 本身并不关心表达式细节。`castNOTNULL` 的处理发生在“Substrait 表达式 -> LogicPlan 表达式”以及“推导输出类型”阶段，最终以“算子 options / 序列化计划内容”进入 DAG。

## 3. 从 Substrait 到 `Expression::Function`

Substrait reader 会构建 `function_extensions`，把 `function_reference`（数字锚点）映射到函数名：

- 参考 [substrait_reader/mod.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/substrait_reader/mod.rs#L486-L543)
  - 它从 `Plan.extensions` 中读取 `extension_function.name`
  - `name` 会被 `split(':').next()` 截取，得到形如 `castNOTNULL` 的函数名
  - 最终写入 `Context.function_extensions`

随后 `ExpressionBuilder::build_expression` 在遇到 `RexType::ScalarFunction` 时：

- 通过 `scalar_function.function_reference` 查 `context.function_extensions` 得到 `(funcname, field_extension, return_type)`
- 将 Substrait arguments 递归构造成 `ExpressionDesc`
- 调用 `Function::make(funcname, ...)` 生成 `Expression::Function`

参考 [expression.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L1915-L2013)。

## 4. `castNOTNULL` 的核心处理：返回类型的 nullability 推导

JobManager 在推导 `Expression::Function` 的输出字段时（`to_out_field`），会根据版本决定是否启用 TM adaptor 来“修正函数返回类型属性”，特别是 nullability。

### 4.1 触发条件：`use_tm_udf_type`

- 当 `request_context.version.use_tm_udf_type()` 为 `true` 时（目前条件是 `major >= 2`），会走 TM adaptor 逻辑。
- 参考 [common/mod.rs](file:///root/Documents/jobmanager/jobmanager/src/common/mod.rs#L88-L116)

### 4.2 具体逻辑：`Function::to_out_field` 调 TM adaptor

`Function::to_out_field` 会先计算参数类型（每个参数表达式的 `to_out_field.typ`），然后调用：

`request_context.tmadaptor.query(&self.name, &self.out_type, &arg_types)`

参考 [expression.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L405-L456)。

### 4.3 TM adaptor 的数据来源：`tmfuncs.json`

TM adaptor 会在启动时读取 `jobmanager/config/adaptors/tmfuncs.json`，得到“函数名 -> 返回属性推导策略”的配置表。

- 参考 [tmadaptor.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L175-L190)
- 配置文件： [tmfuncs.json](file:///root/Documents/jobmanager/jobmanager/config/adaptors/tmfuncs.json)

其中明确包含：

- `castNOTNULL`: `"flag": "NullNever"`
  - 参考 [tmfuncs.json](file:///root/Documents/jobmanager/jobmanager/config/adaptors/tmfuncs.json#L34-L46)

### 4.4 `NullNever` 的含义

TM adaptor 的 `query()` 对 `NullNever` 的处理是：

- 无论输入参数是否 nullable，输出类型的 nullability 强制改为 `Required`（即 not null）。

实现位于：

- [tmadaptor.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L52-L96)

对比一下另一类策略：

- `NullIfNull`：如果所有参数都是 `Required`，输出就是 `Required`；否则输出是 `Nullable`。

## 5. 对 DAG 的实际影响是什么

JobManager 侧对 `castNOTNULL` 的“处理”主要体现在：

- 在逻辑计划表达式 `Function::to_out_field` 推导输出字段类型时，把该表达式的返回类型标记为 not null（`Nullability::Required`）。
- 该类型信息会影响后续逻辑/物理计划阶段的字段类型、算子输出 schema、以及可能的 SQL 生成/下推行为（例如某些组件在生成 SQL 时会依赖字段类型与 nullability）。

而 DAG 组装阶段（`plan/dag.rs`）只是把已经构建好的 plan 结构（operators、tasks、options 等）打包成 `TideExecutionGraph`，不再对 `castNOTNULL` 做特殊分支。

## 6. 为什么 `CASTNOTNULL(...) AS event_date` 仍可能被推导成 nullable

下面以示例表达式为背景：

```sql
CASTNOTNULL(
  CAST(
    CAST(
      IF(event_date IS NULL,
         IF(event_time IS NULL, 'time', event_time),
         event_date) AS BIGINT
    ) / 1000 AS TIMESTAMP
  )
) AS event_date
```

如果你看到 `event_date` 最终类型仍然是 `Nullable`，在本仓库实现里通常有 3 个原因（按优先级）：

### 6.1 TM adaptor 查不到 `CASTNOTNULL`（大小写敏感）

`castNOTNULL` “把返回类型改成 not null”的逻辑并不是硬编码在表达式处理里，而是依赖 TM adaptor（`tmfuncs.json`）配置。

关键点：

- TM adaptor 的函数名查表是大小写敏感的：`self.funcs.get(name)`。
  - 参考 [TMAdaptor::query](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L52-L62)
- 当前配置文件中 key 是 `castNOTNULL`（小写 `c`），而不是 `CASTNOTNULL`。
  - 参考 [tmfuncs.json](file:///root/Documents/jobmanager/jobmanager/config/adaptors/tmfuncs.json#L34-L46)

因此，当 Substrait plan 里 extension function name 解析出来的函数名是 `CASTNOTNULL`（全大写）时：

1. `Function::to_out_field` 调用 TM adaptor 时会查不到配置项；
2. 于是回退使用协议里带来的 `out_type`（通常是 `Nullable`）；
3. 最终 `CASTNOTNULL(...)` 的返回类型仍然是 nullable。

回退路径见：

- [Function::to_out_field](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L405-L435)

### 6.2 未开启 TM adaptor 分支（版本开关）

即使函数名大小写匹配正确，TM adaptor 分支也只有在 `request_context.version.use_tm_udf_type()` 为 `true` 时才启用。

- 当前实现条件是 `major >= 2`
  - 参考 [Version::use_tm_udf_type](file:///root/Documents/jobmanager/jobmanager/src/common/mod.rs#L113-L115)

如果版本不满足，该路径会直接使用协议提供的 `out_type`，不会应用 `castNOTNULL => NullNever` 的修正。

### 6.3 `IF(...)` 的输出 nullability 合并规则会把表达式变成 nullable（对比用）

你的表达式内部包含 `IF(...)`，在逻辑计划里会被映射成 `IfThen`。

`IfThen` 推导输出类型时，会对每个 `then/else` 分支做类型匹配；当任一分支类型为 `Nullable`，最终输出就会合并成 `Nullable`：

- 参考 [IfThen::batch_match](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L997-L1039)

这解释了为什么“没有 `castNOTNULL` 修正”时，`IF(event_date IS NULL, ..., event_date)` 往往会推导成 nullable：因为 `event_date` / `event_time` 这类输入字段本身常常是 nullable。

注意：`IF` 的这个规则本身不是 bug，它是“类型合并”逻辑；真正让最外层 `CASTNOTNULL` 没能把结果变回 not null 的，通常还是 6.1/6.2 的原因。

## 7. 快速定位要点

- DAG 生成入口： [Submitting::dispatch_impl](file:///root/Documents/jobmanager/jobmanager/src/managedjob/job/action/submit/submit.rs)
- DAG 组装： [plan/dag.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/dag.rs)
- Substrait function name 映射： [substrait_reader/mod.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/substrait_reader/mod.rs#L486-L543)
- Scalar function 构造： [ExpressionBuilder::build_expression](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L1915-L2013)
- 输出类型修正（TM adaptor）： [Function::to_out_field](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L405-L456)
- `castNOTNULL` 配置： [tmfuncs.json](file:///root/Documents/jobmanager/jobmanager/config/adaptors/tmfuncs.json#L34-L46)
- `NullNever` 语义实现： [TMAdaptor::query](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L52-L96)

## 8. How To Configure TM Adaptor

TM adaptor is a small “function return property” adapter used during expression type deduction (mainly nullability). It is configured by a JSON file shipped with the service.

### 8.1 Where it is loaded

- Loader code: [tmadaptor.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L175-L183)
  - It reads a relative path: `config/adaptors/tmfuncs.json`
  - It parses the file into a `HashMap<String, Function>` keyed by the function name
- It is instantiated during service wiring:
  - [service.rs](file:///root/Documents/jobmanager/jobmanager/src/server/service.rs#L1921-L1946) sets `tmadaptor: make_shared_tmadaptor()?`

This means the process working directory must contain `config/adaptors/tmfuncs.json` at runtime (for example, when running from the `jobmanager/` directory or when the packaged `output/` layout includes `config/`).

### 8.2 File format and knobs

File: `jobmanager/config/adaptors/tmfuncs.json` ([tmfuncs.json](file:///root/Documents/jobmanager/jobmanager/config/adaptors/tmfuncs.json))

Each entry is:

- **key**: function name (string)
- **value**: object with:
  - `flag`: `"NullIfNull"` or `"NullNever"`
  - `depends_field_index`: integer (currently present, but the current implementation of `NullIfNull` checks *all* args; the index-based behavior is commented out in [tmadaptor.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L64-L95))

Example (existing):

```json
{
  "castNOTNULL": { "flag": "NullNever", "depends_field_index": 0 },
  "castBIGINT":  { "flag": "NullIfNull", "depends_field_index": 0 }
}
```

Semantics in code:

- `NullNever`: output nullability is forced to `Required` (NOT NULL)
- `NullIfNull`: output becomes `Required` only if all arguments are `Required`; otherwise `Nullable`

Reference: [TMAdaptor::query](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L52-L96)

### 8.3 Function name matching rules (important)

- Matching is **case-sensitive**: adaptor uses `HashMap::get(name)` with the function name as-is.
  - If your Substrait extension function name is `CASTNOTNULL` but JSON key is `castNOTNULL`, it will not match and the system will fall back to the protocol-provided return type.
  - Reference: [TMAdaptor::query](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/tmadaptor.rs#L52-L62)
- The name comes from Substrait extension function declarations, normalized by taking the substring before `:`:
  - Reference: [substrait_reader/mod.rs](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/substrait_reader/mod.rs#L515-L532)

Practical guidance:

- Use the exact function name that appears in Substrait (after `split(':').next()`), including case.
- If you need both `castNOTNULL` and `CASTNOTNULL`, you must add both keys (or change code to normalize case).

### 8.4 When it takes effect (version gate)

TM adaptor is only used when `request_context.version.use_tm_udf_type()` is true (currently `major >= 2`).

- Reference: [use_tm_udf_type](file:///root/Documents/jobmanager/jobmanager/src/common/mod.rs#L113-L115)
- The hook point is [Function::to_out_field](file:///root/Documents/jobmanager/jobmanager/src/plan/logicplan/expression.rs#L405-L435)

If the version gate is off, the adaptor configuration will not be consulted and you will see the protocol `out_type` used directly.

### 8.5 Deployment / reload

- The file is read once when `make_shared_tmadaptor()` runs during service initialization.
- Changes require a process restart to take effect (no hot reload implemented).
