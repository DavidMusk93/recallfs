# JSON Encoder LLVM14 JIT 方案设想

本文讨论的不是“是否能把 JSON encoder 再优化一点”，而是：

- 现有 JSON encoder 是否本质上属于一个适合 JIT 的问题
- 为什么这不是牵强附会，而是一个很自然的演进方向
- 如果引入 `LLVM 14 ORC/LLJIT`，应该如何设计
- 预期收益在哪里，收益边界又在哪里

如果需要看“结合当前 `json_v2` case 的 LLVM 实战开发教程”,包括:

- 生成 `kernel.ll -> kernel.bc -> kernel.o -> kernel.so` 的实际链路
- `RuntimeVTable` / IR ABI 如何保持一致
- 怎么从 `compile.log`、profile、benchmark 快速定位问题
- 如何把一次 LLVM 改动做成可验证、可回退、可验收的闭环

请参考:

- [llvm_development_tutorial_json_v2.md](file:///root/Documents/stream_engine/docs/jit/llvm_development_tutorial_json_v2.md)

本文新增两个明确前提：

- **JIT 方案只考虑按列编码，不再保留按行模式**
- **JIT 方案把输入统一抽象成 `schema + table`，底层 table 可以来自 Arrow 或 Velox**

相关代码：

- JSON encoder 创建与 `Init/Encode`： [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.h#L43-L71), [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L33-L128)
- 现有主执行路径： [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L52-L303)
- 现有类型分派与嵌套处理： [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L149-L714)
- JSON 格式化器： [simdjson_format.h](file:///root/Documents/stream_engine/src/util/arrowx/formats/simdjson_format.h#L12-L79)
- 项目内已有 LLVM/Gandiva 使用： [expression_group.cpp](file:///root/Documents/stream_engine/src/sql/columngroup/expression_group.cpp#L117-L150)

外部参考：

- LLVM 对 JIT 调试的说明： [Debugging JIT-ed Code](https://llvm.org/docs/DebuggingJITedCode.html)
- LLVM 源级调试元信息： [Source Level Debugging with LLVM](https://llvm.org/docs/SourceLevelDebugging.html)
- LLVM 异常处理 / CFI / unwind 背景： [Exception Handling in LLVM](https://llvm.org/docs/ExceptionHandling.html)

---

## 1. 背景判断

先给结论：

- JSON encoder 是一个 **非常自然的 JIT 场景**

原因不是“JIT 很酷”，而是它同时满足了几个典型条件：

1. **输入 schema 在 encoder 生命周期内通常是固定的**
2. **输出格式在 `Init()` 后基本固定**
3. **当前实现包含大量与 schema 无关、但在运行时重复执行的分支判断**
4. **编码是热路径，常常对同一 schema 批量、反复执行**

这几个条件叠在一起，意味着当前代码里有很多：

- 本可以在初始化期决定
- 却被放到运行期反复判断

这正是 JIT 最擅长消掉的成本。

---

## 2. 为什么这是“自然而然”的场景

### 2.1 schema 是确定的

对一个 encoder 实例来说，`SetSchema()` 后 schema 就固定了。

后续 `Init()` 已经在做这种“把配置和 schema 绑定”的工作：

- 解析 `json.unescape.fields`
- 解析 `ignore.json.fields`
- 记录 `json.unfold.carry.field.name`

代码：

- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L33-L77)

不过从 JIT 方案视角，`json.encoder.mode` 不应继续保留为长期设计自由度。

原因是：

- `row mode` 的访问模式天然更容易出现 cache ping-pong
- JIT 更适合围绕列式批处理生成专用 kernel
- 如果同时维护 row/column 两套 JIT 代码生成链路，复杂度会明显上升，但收益并不对称

因此这里建议直接做一个架构决策：

- **JIT encoder 只保留 column mode**
- **现有 row mode 作为存量兼容路径逐步删除**

也就是说，当前实现已经承认了一件事：

- **编码行为取决于 schema + options 的组合**

这本身就很接近 JIT 的输入。

### 2.2 输出格式也是确定的

对固定 schema、固定 options 来说，每一列输出成什么 JSON 形状，其实在 `Init()` 后就可以确定：

- 字段名固定
- 字段顺序固定
- 是否忽略固定
- 是否 raw string 固定
- 某列是不是 carry unfold 固定
- 顶层是 object 固定
- 某列的类型分派固定

运行时真正变化的，主要只是：

- 当前 batch 的 row 数
- 每个 cell 的值
- null 分布
- chunk 边界

这意味着：

- 运行期不需要一遍遍“重新理解 schema”
- 只需要执行一个对该 schema 特化后的编码程序

### 2.3 当前实现里有太多“解释执行式”分支

现有实现虽然表面上叫 `Visitor`，但从性能视角看，更像是：

- 一个围绕 Arrow 类型系统写出来的解释器

主要分支来源包括：

1. **按列进入 `Accept(this)`**
   - 再走 `arrow::ArrayVisitor` 的虚分派

2. **当前实现还额外维护了一套按行路径**
   - 它会把本来适合列式批处理的问题重新打散成逐 row 访问
   - 从 cache 行为和 JIT specialization 的角度都不理想

3. **嵌套类型递归再做一轮类型判断**
   - `ValueReader(...)`
   - `KVReader(...)`
   - `StructReader(...)`

4. **同样的 schema 约束在每个 batch 甚至每个 row 重复判断**

代码位置：

- `column` 路径： [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L62-L131)
- `row` 路径： [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L145-L303)
- 嵌套类型读取： [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L245-L365)
- list/map/object 写出： [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L418-L714)

这些判断在“功能正确性”上没问题，但在“固定 schema 的批量编码”场景下，它们大多是可以提前静态化的。

另外，`row` 路径本身也强化了一个不必要的复杂度源：

- 它迫使同一个 encoder 同时维护两套完全不同的数据访问模型

而 JIT 更合适的方向是：

- **只保留列式访问，把所有优化预算集中到 column path**

### 2.4 项目里已经有 LLVM 思维方式

这个仓库不是第一次接触 LLVM。

SQL 表达式执行这条链路已经使用 Gandiva/LLVM：

- [expression_group.cpp](file:///root/Documents/stream_engine/src/sql/columngroup/expression_group.cpp#L117-L150)

其中已经有：

- 构建表达式
- 编译成 LLVM generator
- 缓存和执行

这说明两件事：

1. 工程团队对“把高层逻辑编译成专用执行单元”的思路并不陌生
2. 依赖和工程接受度上，LLVM JIT 不是从零开始的外来物

所以从项目演进路径上看，把 JSON encoder 朝 “plan + codegen + compiled kernel” 推进，是顺势而为。

---

## 3. 当前实现为什么不够匹配“确定性输入、确定性输出”

### 3.1 它对 schema 做了太少的“预编译”

现有 `Init()` 主要只做了：

- 记录字段级 flag
- 初始化 visitor

但没有把 schema 转成一个真正的执行计划，例如：

- 第 0 列一定是 `INT64`
- 第 1 列一定是 `STRING(raw=false)`
- 第 2 列一定是 `LIST<INT32>`
- 第 3 列一定是 `MAP<string,double>`

因此，后续编码仍然依赖运行期解释。

### 3.2 Visitor 在这里更像分派外壳，而不是优化手段

`Visitor` 这个结构在抽象上没问题，但它并没有减少分支，反而把分支分散到了多层：

- `Accept(this)`
- `Visit(T)`
- `ValueReader`
- `KVReader`
- `StructReader`

对于“类型很多、schema 变化频繁”的通用库，这种写法合理。

但对固定 schema encoder，它的代价是：

- 编译器很难把整条路径优化成一条线性 hot path
- 代码读起来是通用框架，跑起来也是通用框架

### 3.3 真正热的不是“类型系统”，而是“值拷贝 + JSON 拼装”

JSON encoder 的核心工作，本质上应是：

- 读值
- 判断 null
- 写 key
- 写值
- 拼接标点

但当前热路径里夹杂了大量：

- 类型识别
- 递归分派
- 布尔 flag 检查
- 模式判断

这些都在抢占真正应该热的路径。

---

## 4. JIT 方案的核心思想

一句话概括：

- **把 “schema + options -> encoder 执行程序” 这一步提前到初始化期完成**

运行期就不再解释 schema，而是直接执行一个：

- 为该 schema 专门生成的编码函数

### 4.1 目标形态

从概念上说，今天的 JSON encoder 更像这样：

```text
schema + table -> generic visitor -> runtime type dispatch -> formatter -> binary array
```

目标形态则是：

```text
schema + table -> compiled encode kernel(schema-specialized) -> formatter/builder -> binary array
```

### 4.2 两层结构

推荐拆成两层：

1. **Encoder Plan**
   - 纯静态描述
   - 负责把 schema 和 options 归一化成一个中间计划

2. **Input Adapter**
   - 把不同物理输入统一成 `schema + table view`
   - 对 JIT 暴露稳定、列式的读取模型

3. **JIT Kernel**
   - 由 LLVM 根据 plan 生成
   - 直接执行编码

这样可以避免“一上来就把所有逻辑直接写成 IR”，提高可维护性。

### 4.3 明确选型：LLVM 14 ORC/LLJIT，而不是 Gandiva

既然已经决定选 LLVM，那么这里建议明确：

- **不复用 Gandiva 作为 JIT 抽象层**
- **直接基于 LLVM 14 ORC/LLJIT 做 encoder kernel 编译和装载**

原因：

1. `Gandiva` 的抽象中心是表达式求值
   - 它适合：
     - expression tree
     - filter/project
     - Arrow expression execution
   - 它不天然适合：
     - schema-specialized codec kernel

2. JSON encoder 的目标不是“表达式求值”
   - 而是：
     - plan lowering
     - 专用遍历
     - 专用写出
     - 专用 null/分隔符控制

3. ORC/LLJIT 更接近我们真正需要的能力边界
   - object/materialization
   - symbol 管理
   - 代码装载
   - 调试信息接入
   - 未来缓存与多层 JIT 策略

因此本文后面的 “LLVM JIT” 都默认指：

- **LLVM 14 ORCv2 / LLJIT 路线**

---

## 5. 推荐架构

### 5.1 高层流程图

```text
+-----------------------------------+
| schema + table + encoder options  |
+-----------------------------------+
               |
               v
+-----------------------------+
| Build EncoderPlan           |
| - normalize schema          |
| - resolve field behavior    |
| - lower nested types        |
+-----------------------------+
               |
               v
+-----------------------------+
| Build InputAdapter          |
| - ArrowTableAdapter         |
| - VeloxRowVectorAdapter     |
+-----------------------------+
               |
               v
+-----------------------------+
| Fingerprint(plan)           |
+-----------------------------+
               |
               v
      +--------+--------+
      | cache hit?      |
      +--------+--------+
               |
        +------+------+
        |             |
        v             v
+---------------+  +----------------------+
| reuse kernel  |  | LLVM IR codegen      |
+---------------+  | + JIT compile        |
                   +----------------------+
                              |
                              v
                   +----------------------+
                   | Compiled EncodeKernel |
                   +----------------------+
                              |
                              v
                   +----------------------+
                   | Encode schema+table   |
                   +----------------------+
                              |
                              v
                   +----------------------+
                   | BinaryArray output    |
                   +----------------------+
```

### 5.2 `EncoderPlan` 应该长什么样

`EncoderPlan` 不应直接等于 Arrow schema，而应是一个“编码视角”的 lowering 结果。

示例：

```text
RootObject
  Field("uid", Int64, nullable=false)
  Field("name", String, raw=false, nullable=true)
  Field("tags", List<Int32>, nullable=true)
  Field("attrs", Map<String, Double>, nullable=true)
  Field("carry", Map<String, String>, unfold=true)
```

它至少需要编码出：

- 字段顺序
- 字段名常量
- 类型类别
- nullable 与 null 路径
- raw string / escaped string 语义
- ignore / unfold 等选项效果
- 嵌套类型的子计划

`EncoderPlan` 是 JIT 的输入，也是 fallback interpreter 的输入。

### 5.3 输入适配层：统一成 `schema + table`

既然上层语义总是“schema + table”，那么 JIT 前不妨显式增加一层输入适配。

这里的关键不是把所有输入都强行转成 Arrow Table，而是：

- 给 encoder 暴露一个统一的列式读取视图

推荐抽象：

```text
SchemaTable
  - logical schema
  - row count
  - column count
  - column(i) -> ColumnView
```

其中 `ColumnView` 再继续暴露：

- type info
- nullable info
- 访问当前批次数据的列式句柄
- 对 nested 类型的子 view

### 5.4 为什么要有适配层

因为仓库里已经存在两类输入：

1. **Arrow Table**
   - 当前 JSON encoder 原生处理对象

2. **Velox RowVector**
   - 当前部分链路会先转 Arrow 再走 encoder
   - 例如 [tide_sink.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/velox/plan/sink/tide_sink.cpp#L240-L266) 里先 `exportToArrow(...)`

此外，仓库已经有同时校验 Arrow / Velox “表形态”的工具：

- [table.h](file:///root/Documents/stream_engine/src/util/arrowx/table.h#L24-L106)

这说明项目其实已经接受一个事实：

- 逻辑上都是 “schema + table”
- 只是物理承载分别来自 Arrow 和 Velox

所以 JIT 设计完全可以顺势做成：

- **逻辑层统一**
- **物理层适配**

### 5.5 适配层不应重新引入热路径分支

这里要避免一个坏设计：

- 在 JIT kernel 内部再通过虚函数/大 `switch` 去问“你是 Arrow 还是 Velox”

正确方向应该是：

1. 先构建逻辑 `EncoderPlan`
2. 再根据物理输入构建 `InputAdapterPlan`
3. 最终生成：
   - `ArrowColumnKernel`
   - 或 `VeloxColumnKernel`

也就是说：

- **逻辑 plan 可以共享**
- **物理访问代码应按 backend 特化**

这样才能既保留统一架构，又不把适配成本重新打回热路径。

### 5.6 推荐的两层 plan

建议明确拆成：

1. **Logical EncoderPlan**
   - 描述输出 JSON 语义
   - 与 Arrow / Velox 无关

2. **Physical AccessPlan**
   - 描述如何从当前 backend 读取列数据
   - 例如：
     - Arrow chunked array 访问
     - Velox flat / dictionary / lazy vector 访问

最后 codegen 读的是：

```text
Logical EncoderPlan + Physical AccessPlan -> backend-specific EncodeKernel
```

### 5.7 第一版建议

第一版推荐这样切：

- 逻辑接口统一成 `schema + table`
- 物理实现先支持：
  - `ArrowTableAdapter`
- `VeloxRowVectorAdapter` 只先完成接口设计和最小实现骨架
- 不要求第一版就绕过现有 `exportToArrow(...)`

原因：

- 先把抽象边界立住
- 再决定 Velox 路径是直接 codegen 访问，还是过渡期继续走 Arrow 导出

如果后续验证 Velox 直读收益明显，再补：

- `VeloxPhysicalAccessPlan`
- `VeloxColumnKernel`

### 5.8 删除 row mode 后，适配层更自然

一旦不再维护 `row mode`，整个系统会更干净：

- 输入统一成列式 `table view`
- JIT kernel 永远按列处理
- Arrow / Velox 的差异只存在于物理访问层

这比：

- Arrow 有 row/column 两套
- Velox 再来一套特殊路径

要自然得多。

### 5.9 JIT kernel 的职责边界

JIT kernel 不一定要负责“一切”。

推荐边界：

- **JIT 负责类型特化后的遍历与分支消除**
- **格式化器继续复用现有 `SIMDJSONFormat` 或其薄包装**

也就是说，第一版不需要把整个 JSON writer 重新发明一遍。

可以让 JIT kernel 做这些事：

- 按 schema 固定顺序访问列
- 消除 `Visit(T)` / `switch(type)` 分支
- 内联 null 检查、字段 key 输出、值写出调用顺序
- 针对嵌套结构生成专用 traversal

而这些底层动作仍然可以调用：

- `start_object`
- `key`
- `write`
- `start_array`
- `comma`
- `end_array`
- `end_object`

代码参照：

- [simdjson_format.h](file:///root/Documents/stream_engine/src/util/arrowx/formats/simdjson_format.h#L30-L74)

这样更现实，也更容易渐进落地。

### 5.10 代码生成单元的组织方式

建议不要把整张 schema 生成为一个巨大的匿名函数。

更合理的组织方式是：

1. `Module`
   - 对应一个 `EncoderPlan`
   - 包含：
     - 主入口函数
     - 若干 nested helper 函数
     - 调试元信息

2. `Entry Function`
   - 例如：`encode_root_batch(plan_hash)`
   - 负责驱动整批 row 编码

3. `Field/Nested Helpers`
   - 例如：
     - `encode_field_0_uid`
     - `encode_field_3_tags_list_int32`
     - `encode_field_4_attrs_map_string_double`

这样做的好处是：

- 生成代码更容易管理
- 调试符号更稳定
- crash 时栈更可读
- 单个函数不会因为 schema 过大而膨胀失控

---

## 6. Generated Code 管理

JIT 方案里，generated code 不是“生成完就算了”的临时品，而应当被当成正式产物管理。

### 6.1 产物分层

建议把产物分成 4 层：

1. **Plan 层**
   - `EncoderPlan`
   - 作用：稳定描述 “编什么”

2. **IR 层**
   - LLVM IR 文本或 bitcode
   - 作用：调试 codegen、对比 diff、问题复现

3. **Object 层**
   - in-memory object
   - 可选持久化到磁盘
   - 作用：调试器注册、符号化、core 后分析

4. **Executable Layer**
   - 最终加载到进程地址空间的可执行内存
   - 作用：实际跑编码

### 6.2 产物 ID

每个 generated module 都应有稳定 ID，例如：

```text
encoder_kind=json
mode=column
plan_hash=ab12cd34
version=v1
```

建议统一成：

- `jit://json_encoder/<plan_hash>/<version>`

这个 ID 应贯穿：

- plan dump
- IR dump
- object dump
- symbol name
- metrics
- crash log

### 6.3 磁盘落盘策略

推荐支持一个显式 debug 选项：

- `json.encoder.jit.dump_dir=/path`

开启后为每个 plan 落盘：

- `<plan_hash>.plan.json`
- `<plan_hash>.ll`
- `<plan_hash>.bc`
- `<plan_hash>.o`
- `<plan_hash>.symbols.json`

其中 `symbols.json` 至少记录：

- module id
- plan hash
- function name
- load address range
- object file path
- schema fingerprint
- option fingerprint
- build timestamp

### 6.4 生命周期管理

需要明确 generated code 的 ownership：

1. `EncoderKernelHandle`
   - 持有：
     - plan
     - module metadata
     - JIT symbol handles
     - debug side artifacts metadata

2. `KernelCache`
   - 按 plan hash 缓存
   - 负责引用计数和淘汰

3. `CodeRegistry`
   - 记录当前进程内已加载 JIT 代码的地址范围
   - 提供：
     - 地址 -> module id
     - 地址 -> function name
     - 地址 -> object/debug artifact

这部分对调试非常关键，因为 core 分析和 crash hook 都需要先回答：

- “这个 PC 地址属于哪段 JIT 代码？”

### 6.5 版本化与失效

generated code 必须显式失效，不能只靠进程重启。

缓存 key 至少应包含：

- schema fingerprint
- encoder options fingerprint
- plan lowering version
- codegen version
- LLVM backend version

这样可以避免：

- 代码生成逻辑改了，但旧 kernel 继续被错误复用

---

## 7. 具体怎么做

### 7.1 第一步：先做 plan，不急着全量 JIT

先把当前 encoder 重构成：

- `schema + options -> EncoderPlan`
- `EncoderPlan -> interpreter encode`

这一步虽然还没上 LLVM，但价值很大：

- 把现在散落在 visitor 层的 schema 逻辑收口
- 明确哪些分支是“编译期分支”，哪些是“运行期分支”
- 给 JIT 提供稳定输入

### 7.2 第二步：JIT flat schema

第一阶段只支持：

- 顶层 object
- 标量列
- `STRING`
- `BOOL`
- `INT32/64`
- `UINT32/64`
- `FLOAT/DOUBLE`
- `TIME/TIMESTAMP`

不先碰：

- `LIST`
- `MAP`
- `STRUCT`

理由很简单：

- flat schema 已经能覆盖大量实际 sink 输出
- 也是最容易测出“分支消除”收益的一层

### 7.3 第三步：JIT nested schema

在 flat schema 成功后，再把 plan lowering 到：

- `LIST<T>`
- `STRUCT`
- `MAP`

这一步的关键不是“支持嵌套”，而是：

- 为每个嵌套子树生成专用子 kernel 或专用子片段

例如：

- `LIST<INT32>` 和 `LIST<STRING>` 应该是两段不同代码
- `STRUCT<a:int64,b:string>` 也应编译成固定字段序列

### 7.4 第四步：缓存 compiled kernel

JIT 成本不能每次都付。

因此需要：

- `plan fingerprint`
- kernel cache

缓存 key 可以由以下信息组成：

- schema fingerprint
- `json.encoder.mode`
- `json.unescape.fields`
- `ignore.json.fields`
- `json.unfold.carry.field.name`
- 未来新增的 map key strategy 等

缓存命中后直接复用 compiled kernel。

### 7.5 第五步：保留 fallback

JIT 不应成为单点风险。

推荐保留两条路径：

- JIT path
- existing interpreter path

在这些场景回退：

- LLVM 代码生成失败
- 遇到未支持的类型组合
- 编译超时
- 显式关闭 JIT

---

## 8. 为什么 LLVM JIT 比“继续堆模板/手写分支”更合适

### 8.1 模板能做的是“按类型编译”

模板当然能消掉一部分分支，但它有天然边界：

- schema 组合是运行时才知道的
- 字段个数、顺序、嵌套结构也是运行时才知道的

这意味着：

- 模板更适合“有限静态类型组合”
- 不适合“运行时按 schema 生成专用程序”

### 8.2 手写专用代码不可维护

你当然可以手写：

- scalar fast path
- nested fast path
- string-heavy fast path
- map fast path

但很快就会遇到组合爆炸：

- 10 列和 30 列不同
- `raw/unescape/ignore` 交叉组合不同
- nested 结构不同

LLVM JIT 的价值就在于：

- 不靠人工维护所有组合
- 而是由 plan 自动生成对应程序

### 8.3 这和 Gandiva 的思路一致

项目里已经接受了：

- “表达式树 -> LLVM generator -> specialized execution”

JSON encoder 完全可以对应成：

- “EncoderPlan -> LLVM generator -> specialized encode kernel”

这不是另起炉灶，而是把现有工程思维扩展到 codec。

---

## 9. 调试与可观测性设计

JIT 如果没有调试设计，后续会非常痛苦。

### 9.1 最低要求

第一版就应具备：

- plan dump
- IR dump
- stable symbol naming
- 代码地址注册表
- 开关可强制回退 interpreter
- metrics 区分 compile / execute / cache-hit

### 9.2 调试信息生成

建议在生成 IR 时同步生成：

- `DICompileUnit`
- `DISubprogram`
- `DILocation`
- 关键 local variable 的 debug metadata

这至少能带来两类收益：

1. 在线调试时
   - 调试器更容易识别 JIT 函数

2. 离线分析时
   - 可以把地址映射回：
     - module
     - helper function
     - 甚至 plan 中的字段节点

### 9.3 符号命名规范

不要让 JIT 函数名字是匿名的。

建议统一命名：

- `tide_json_encode_root_<planhash>`
- `tide_json_encode_field_<planhash>_<fieldidx>_<fieldname>`
- `tide_json_encode_list_<planhash>_<path>`

这样在：

- crash log
- perf
- gdb/lldb
- 内部 metrics

里都能快速识别问题位置。

### 9.4 与调试器/分析器对接

对 LLVM JIT 来说，推荐至少考虑三条通路：

1. **GDB JIT Interface / JIT 注册**
   - 让调试器知道新加载的 JIT object

2. **DWARF debug info**
   - 让符号、行号、变量信息可见

3. **perf jitdump / perf-map 兼容落盘**
   - 让 Linux 性能分析工具能识别 JIT 代码

如果第一版三者不能同时做全，建议优先级是：

1. object 落盘 + symbol registry
2. GDB JIT registration
3. perf jitdump

### 9.5 运行时可观测性

建议增加以下 metrics：

- `json_encoder_jit_compile_count`
- `json_encoder_jit_compile_ms`
- `json_encoder_jit_cache_hit_count`
- `json_encoder_jit_execute_rows`
- `json_encoder_jit_execute_ns`
- `json_encoder_jit_fallback_count`
- `json_encoder_jit_crash_guard_count`

并在错误日志里至少打印：

- module id
- plan hash
- active symbol
- object path
- fallback state

---

## 10. JIT 崩溃、core 与堆栈恢复

这是 JIT 设计里最容易被低估的问题。

### 10.1 问题本质

你提到的核心问题是成立的：

- **JIT 代码如果只存在于匿名可执行内存里，core 之后往往很难像普通 ELF 那样优雅恢复堆栈和符号**

困难点包括：

- 没有稳定文件路径
- 调试器不知道这段地址属于哪个 JIT module
- 没有 debug info / unwind info 时，栈展开会断
- core 分析时丢失 “地址 -> JIT object -> IR/plan” 的映射

所以“优雅实践”的关键不是事后魔法恢复，而是：

- **在生成时就把可恢复性设计进去**

### 10.2 业界上更优雅的做法

比较成熟的实践通常是组合拳，而不是单点方案：

1. **为 JIT 代码生成 debug info**
2. **为 JIT 代码注册 debugger 接口**
3. **为 JIT 代码注册 unwind / EH frame**
4. **持久化 object / IR / 符号侧车文件**
5. **维护进程内地址注册表**
6. **提供 crash hook，把当前 PC 对应的 JIT module 信息打出来**

### 10.3 unwind 信息是硬要求

如果想让 core 栈尽量不断：

- JIT 代码必须具备可展开的 frame 信息

也就是要认真处理：

- CFI
- EH frame
- 平台对应的 unwind metadata

否则即使有符号名，栈也可能在 JIT frame 处断开。

这里的原则很明确：

- **没有 unwind 信息，就不要期待 postmortem stack 恢复会优雅**

### 10.4 推荐实践：把 object 当正式调试产物保存

要优雅支持 core 分析，我建议默认就支持：

- 可选 object 落盘

即便线上默认不开，也要保证：

- 一旦进入 debug / canary / perf 环境，可以打开

推荐做法：

1. JIT 编译生成 object
2. 在装载前/后把 object 写到磁盘
3. 记录 load address range
4. 把映射写入 registry / sidecar

这样做的价值是：

- core 后仍有机会把 PC 地址映射回 object
- object 内的 DWARF / symbol / EH 信息可以参与分析

### 10.5 推荐实践：进程内 `CodeRegistry`

建议做一个轻量的全局注册表：

```text
[start_addr, end_addr) -> {
  module_id,
  plan_hash,
  object_path,
  symbol_table,
  compile_ts,
  codegen_version
}
```

用途：

- 崩溃信号处理时打印当前 JIT frame 所属 module
- 在线诊断某个地址来自哪个 kernel
- 辅助 core 离线符号化

### 10.6 推荐实践：崩溃时优先打印 JIT 侧信息

当 SIGSEGV/SIGABRT 发生时，不要只依赖系统 backtrace。

建议 crash hook 额外打印：

- fault PC
- 所属 JIT module id
- plan hash
- 当前 helper symbol
- object dump path
- IR dump path

这样即使标准回溯栈不完整，也能快速定位：

- 是哪个 schema-specialized kernel 崩了

### 10.7 推荐实践：保持 fallback 和可复现路径

对于生产系统，最优雅的恢复方式通常不是“在线强行继续跑”，而是：

1. 记录崩溃 kernel 的 plan / IR / object
2. 进程恢复后对该 plan 自动熔断 JIT
3. 同 schema 暂时回退 interpreter
4. 离线复现该 JIT module

这比“出 core 后试图在线修复栈”更现实。

### 10.8 是否存在“完全优雅恢复 core 栈”的银弹

没有银弹。

更准确地说：

- **可以把 JIT core 的可调试性做得接近普通本地代码**
- **但前提是提前做好 object/debug/unwind/registry 这整套建设**

如果没有这些建设，事后几乎不可能优雅补救。

所以结论不是：

- “JIT core 无法恢复堆栈”

而是：

- “JIT core 的堆栈恢复必须前置设计，否则事后恢复会非常差”

---

## 11. 预期收益

这里的“收益”应理解为目标和假设，不是已经测得的数字。

### 11.1 直接收益

1. **消除运行期类型分派**
   - 减少 `Accept(this)` / `Visit(T)` / `switch(type)` 的开销

2. **减少热路径上的分支判断**
   - `ignore/raw/unfold/type dispatch` 中可静态化的部分前移

3. **提升编译器内联机会**
   - 让固定 schema 的编码路径更接近线性代码

4. **改善 cache locality 与 branch prediction**
   - 让热点代码更稳定

### 11.2 工程收益

1. **让“解释执行型 encoder”变成“plan 驱动 encoder”**
2. **为后续 codec 优化提供统一框架**
3. **让性能问题更容易定位到 plan 或 kernel，而不是 visitor 层迷宫**

### 11.3 可量化目标

建议把第一阶段目标设成：

- flat schema 下吞吐提升 `1.3x ~ 2.5x`
- CPU cycles / row 明显下降
- branch miss 显著下降
- JIT compile 开销在缓存命中后被摊平

对重嵌套 schema：

- 提升幅度可能更高
- 但实现复杂度也更高

所以不要一开始就承诺统一倍率。

---

## 12. 预期成本与风险

### 12.1 编译时延

JIT 的常见问题不是执行慢，而是：

- 首次编译有开销

所以必须有：

- cache
- fallback
- 编译阈值

例如：

- 小 batch 不走 JIT
- 高频重复 schema 才走 JIT

### 12.2 复杂类型 lowering 难度高

`LIST/MAP/STRUCT` 的问题不在于“不能做”，而在于：

- plan 到 IR 的 lowering 容易迅速复杂化

因此应坚持：

- flat first
- nested later

### 12.3 调试难度上升

JIT kernel 的问题定位比普通 C++ 更难。

因此需要：

- 输出 plan dump
- 输出 generated IR
- 支持关闭 JIT 回退解释器
- 有可比对的 golden test

### 12.4 依赖治理

虽然仓库已有 Gandiva/LLVM 使用，但 codec 侧新增 JIT 仍需确认：

- 依赖链是否复用现有设施
- 是否直接依赖 Gandiva 的 LLVM 封装
- 还是自建更轻量的 ORC JIT 层

我的建议是：

- 第一版尽量复用现有 LLVM 经验
- 不通过 Gandiva 间接封装 ORC/LLJIT
- 不要一开始就自造一套超大 JIT 基础设施，但 `CodeRegistry + dump + unwind` 这几件事要在一开始就定下来

---

## 13. 分阶段落地计划

### 阶段 0：基线与观测

目标：

- 建立当前 JSON encoder 的性能画像

工作：

- 选 3 类基准 schema
  - flat scalar schema
  - string-heavy schema
  - nested schema
- 统计：
  - rows/s
  - bytes/s
  - CPU cycles/row
  - branch miss
  - compile vs execute 时间占比

补充要求：

- 明确把 `row mode` 从观测范围中排除
- benchmark 只针对 column path
- 如果需要对比 row path，仅作为“为何删除”的历史基线，不作为未来目标路径

### 阶段 0.5：调试基础设施先行

目标：

- 先把 generated code 管理和 crash 可恢复性骨架定下来

工作：

- 定义 `module id / plan hash / symbol naming`
- 实现 `CodeRegistry`
- 支持 plan / IR / object dump
- 预留 GDB JIT registration 和 unwind 注册接口

交付标准：

- 即使 kernel 逻辑还没完全落地，也能追踪：
  - 哪个 plan 编出了哪段代码
  - 哪段代码加载到了哪个地址范围

### 阶段 1：引入 `EncoderPlan`

目标：

- 把 schema 决策从 visitor 中抽出来

工作：

- 新建 `EncoderPlan`
- 新建统一的 `SchemaTable` / `InputAdapter` 抽象
- 用 `EncoderPlan + InputAdapter` 驱动现有 interpreter
- 让现有逻辑先完成“可编译化”

交付标准：

- 无功能变化
- 现有测试通过
- plan dump 可读
- module metadata 可追踪

### 阶段 2：JIT flat scalar path

目标：

- 让最常见场景先受益

工作：

- 生成 flat schema 的 `ArrowColumnKernel`
- 支持 null / key / comma / scalar write
- 接到现有 `SIMDJSONFormat` 接口上
- 生成并注册 debug/unwind 信息

交付标准：

- scalar schema 可走 JIT
- fallback 路径可切换
- 有基准对比
- crash 时可打印 module id / symbol / object path
- 逻辑层不再依赖 row mode

### 阶段 3：JIT nested path

目标：

- 扩展到 `LIST/STRUCT/MAP`

工作：

- 递归 lowering nested plan
- 生成子 kernel 或 IR 片段
- 处理 null 与分隔符细节
- 补 `VeloxRowVectorAdapter`
- 评估是否需要 `VeloxColumnKernel` 直接读取 Velox 数据

交付标准：

- 至少覆盖 `LIST<INT32>`、`STRUCT<...>`、`MAP<string,T>`

### 阶段 4：缓存与生产化

目标：

- 让 JIT 可持续使用

工作：

- plan fingerprint
- compiled kernel cache
- metrics
- 开关与降级策略
- object/debug artifact 保留策略

交付标准：

- 重复 schema 编码命中缓存
- 编译失败自动回退
- 崩溃 plan 可熔断并自动退回 interpreter

---

## 14. 我建议的第一版边界

如果要把这件事做成一个现实项目，我建议第一版范围严格控制在：

- 只做 JSON encoder
- 只做 `column` 模式
- 删除或冻结 `row` 模式，不再为其投入新设计
- 只做 flat scalar schema
- 上层统一成 `schema + table`
- 保留现有 formatter
- 不先重写 nested encoder
- 不先处理 map key 扩展问题
- 第一版就把 generated code 管理、dump、symbol naming、registry 做进去
- 第一版物理输入只强保证 Arrow，Velox 先完成适配接口设计

原因：

- 这样可以最小化变量
- 最快验证“JIT 对这个问题是否真的有效”
- 也最容易把收益和复杂度对应起来

如果第一版 flat scalar 都没有明显收益，就没必要继续把复杂类型全量推进。

---

## 15. 结论

一句话总结：

- JSON encoder 的“固定 schema、固定输出协议、重复批量执行”特征，使它天然适合 LLVM JIT

更具体一点：

- 当前实现本质上在用通用 visitor 解释固定 schema
- JIT 的价值，就是把这些固定事实提前编译成一段专用编码程序
- 对这个目标来说，`row mode` 不是资产，而是噪音
- 更自然的总体模型是：**`schema + table`，其上挂统一逻辑 plan，其下挂 Arrow/Velox 输入适配**

因此，这不是“为了用 JIT 而用 JIT”，而是：

- **一个已经具备 plan 化条件、并且当前热路径确实存在可消除运行期分支的模块，顺理成章地走向 JIT**

如果后续真要开始做，我建议顺序是：

1. 先做 `EncoderPlan`
2. 再做 flat scalar JIT
3. 再决定 nested 类型是否值得继续推进
