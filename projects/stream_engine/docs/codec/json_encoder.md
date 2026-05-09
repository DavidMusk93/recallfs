# JSON Encoder 实现梳理

本文梳理当前仓库里的 JSON encoder 实现，重点说明：

- 入口和调用链
- 编码主流程
- 当前实际支持的类型
- `column` / `row` 两种模式的差异
- 几个和输出行为直接相关的配置项

核心代码位置：

- 创建入口： [encoder_factory.h](file:///root/Documents/stream_engine/src/sql/encdec/factory/encoder_factory.h#L16-L67)
- JSON encoder 构造： [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.h#L43-L71)
- JSON encoder 主实现： [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L33-L128)
- 列式遍历 / 行式遍历： [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L52-L303)
- Arrow 类型分发与嵌套类型递归： [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L149-L714)
- JSON 文本格式化器： [simdjson_format.h](file:///root/Documents/stream_engine/src/util/arrowx/formats/simdjson_format.h#L12-L79)
- 文件 sink 的 JSON 输出流： [json_format_output_stream.h](file:///root/Documents/stream_engine/src/sink/filesystem/io/format/json_format_output_stream.h#L16-L91)

---

## 1. 实现入口

通用入口是：

- `EncoderFactory::Create(options, schema, ignore_fields)`
  - `format.type=json` 时进入 JSON encoder 分支
  - 如果调用方额外传了 `ignore_fields`，会先把字段索引转换成字段名，再写回 `ignore.json.fields`

代码：

- [encoder_factory.h](file:///root/Documents/stream_engine/src/sql/encdec/factory/encoder_factory.h#L16-L67)

JSON encoder 的实际构造函数是：

- `NewJSONEncoder(options, schema)`

它会解析 4 个和 encoder 直接相关的 option：

- `json.unescape.fields`
- `ignore.json.fields`
- `json.encoder.mode`
- `json.unfold.carry.field.name`

然后根据 `json.encoder.mode` 创建：

- `JSONStructuredEncoder<true>`: `column` 模式，默认值
- `JSONStructuredEncoder<false>`: `row` 模式

代码：

- [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.h#L43-L71)

另外，文件系统 sink 没有走 `EncoderFactory::Create()`，而是直接调用 `NewJSONEncoder()`；它还会强制写入：

- `format.type=json`
- `json.unescape.fields=*`
- `ignore.json.fields=<被排除列>`

代码：

- [json_format_output_stream.h](file:///root/Documents/stream_engine/src/sink/filesystem/io/format/json_format_output_stream.h#L42-L64)

---

## 2. 编码流程

### 2.1 总体流程

```text
+---------------------------+
| options + arrow::Schema   |
+---------------------------+
             |
             v
+---------------------------+
| EncoderFactory::Create    |
| or NewJSONEncoder         |
+---------------------------+
             |
             v
+---------------------------+
| JSONStructuredEncoder     |
| <true> or <false>         |
+---------------------------+
             |
             v
+---------------------------+
| Init()                    |
| - 解析 unescape fields    |
| - 解析 ignore fields      |
| - 初始化 TableVisitor     |
+---------------------------+
             |
             v
+---------------------------+
| Encode(table)             |
+---------------------------+
      |                 |
      | column mode     | row mode
      v                 v
+-----------------+   +-----------------+
| Visit()         |   | VisitByRow()    |
| VisitColumnGroup|   | VisitRowGroup   |
+-----------------+   +-----------------+
      |                 |
      +--------+--------+
               |
               v
+---------------------------+
| TableVisitor              |
| - 按 Arrow type 分发      |
| - 递归处理 list/map/struct|
+---------------------------+
             |
             v
+---------------------------+
| SIMDJSONFormat            |
| mini_formatter 逐行写 JSON|
+---------------------------+
             |
             v
+---------------------------+
| Finish(values)            |
| -> vector<string_view>    |
+---------------------------+
             |
             v
+---------------------------+
| arrow::BinaryBuilder      |
| -> arrow::BinaryArray     |
+---------------------------+
             |
             v
+---------------------------+
| 下游 sink/算子消费        |
| 每个元素就是一行 JSON     |
+---------------------------+
```

### 2.2 `Init()` 做什么

`JSONStructuredEncoder::Init()` 本身不做编码，只做编码前准备：

- 解析 `json.unescape.fields`
  - 如果包含 `*`，则所有字段都按 raw string 输出
  - 否则把字段名映射成 schema index，记录到 `m_fieldUnescapeFlags`
- 解析 `ignore.json.fields`
  - 同样映射成 schema index，记录到 `m_fieldIgnoreFlags`
- 调用 `m_visitor->Init(...)`

代码：

- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L33-L77)

### 2.3 `Encode()` 产物是什么

`Encode()` 返回的是：

- `std::shared_ptr<arrow::BinaryArray>`

语义上：

- 每个 row 对应 `BinaryArray` 中的一个元素
- 每个元素是一整行 JSON object 文本

代码：

- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L83-L128)

文件 sink 再把每条 JSON 后面补一个换行符 `\n` 写入文件。

代码：

- [json_format_output_stream.h](file:///root/Documents/stream_engine/src/sink/filesystem/io/format/json_format_output_stream.h#L66-L84)

---

## 3. 两种执行模式

### 3.1 `column` 模式

默认模式，创建的是 `JSONStructuredEncoder<true>`。

执行路径：

- `Encode(table)`
- `m_visitor->Visit(table)`
- `VisitColumnGroup(...)`
- 对每一列执行 `chunk->Accept(this)`
- 由 `arrow::ArrayVisitor` 的 `Visit(Int32Array)` / `Visit(StringArray)` / `Visit(ListArray)` 等重载完成真实写入

代码：

- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L83-L107)
- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L62-L131)
- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L149-L232)

特点：

- 支持类型最完整
- 能处理 chunked array
- `json.unfold.carry.field.name` 的特殊展开逻辑只在这个路径里生效

### 3.2 `row` 模式

创建的是 `JSONStructuredEncoder<false>`。

执行路径：

- `Encode(table)`
- `m_visitor->VisitByRow(table)`
- `VisitRowGroup(...)`
- 每行每列用 `switch (col->type()->id())` 逐个取值并写 JSON

代码：

- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L83-L107)
- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L145-L303)

特点：

- 代码路径更直接
- 但支持类型比 `column` 模式更窄
- 对 `DICTIONARY` 的支持也更保守

---

## 4. 支持类型总表

下面的“支持”都指“当前代码里有明确分支处理”，不是泛指 Arrow 理论上能表示。

### 4.1 顶层字段支持

| Arrow 类型 | `column` 模式 | `row` 模式 | 输出形态 | 代码依据 |
|---|---|---|---|---|
| `BOOL` | 支持 | 支持 | `true` / `false` | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L173-L175), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L175-L177) |
| `INT32` | 支持 | 支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L149-L151), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L178-L180) |
| `INT64` | 支持 | 支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L153-L155), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L181-L183) |
| `UINT32` | 支持 | 支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L157-L159), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L184-L186) |
| `UINT64` | 支持 | 支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L161-L163), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L187-L189) |
| `FLOAT` | 支持 | 不支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L165-L167), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L174-L297) |
| `DOUBLE` | 支持 | 支持 | JSON number | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L169-L171), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L190-L192) |
| `STRING` | 支持 | 支持 | JSON string 或 raw string | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L177-L179), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L193-L203) |
| `BINARY` | 支持 | 不支持 | 按字符串路径输出，无 base64 特判 | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L181-L183) |
| `TIME32` | 支持 | 支持 | 格式化后的字符串 | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L185-L187), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L204-L206) |
| `TIME64` | 支持 | 支持 | 格式化后的字符串 | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L189-L191), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L207-L209) |
| `TIMESTAMP` | 支持 | 支持 | 格式化后的字符串 | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L193-L195), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L210-L213) |
| `LIST` | 支持 | 支持 | JSON array | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L199-L202), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L214-L218) |
| `MAP` | 支持 | 支持 | JSON object | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L204-L217), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L219-L226) |
| `STRUCT` | 支持 | 支持 | JSON object | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L219-L225), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L227-L234) |
| `DICTIONARY` | 支持 | 支持，但值类型更少 | 取字典值后再编码 | [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L227-L232), [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L235-L293) |

### 4.2 `LIST<T>` 支持哪些元素类型

`LIST` 的元素类型最终走 `ValueReader(...)`。

当前有明确分支的 `T` 是：

- `INT32`
- `INT64`
- `UINT32`
- `UINT64`
- `FLOAT`
- `DOUBLE`
- `STRING`
- `BOOL`
- `LIST`
- `MAP`
- `STRUCT`

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L245-L271)
- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L639-L714)

这也意味着当前 `LIST<T>` 没有看到对下列元素类型的专门分支：

- `BINARY`
- `TIME32`
- `TIME64`
- `TIMESTAMP`
- `DICTIONARY`
- `DECIMAL`
- `DATE32` / `DATE64`

### 4.3 `MAP<string, T>` 支持哪些 value 类型

当前 map 的 key 被直接假定为 `StringArray`，所以 key 必须是字符串。

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L204-L217)
- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L273-L298)

当前有明确分支的 `T` 是：

- `STRING`
- `INT64`
- `INT32`
- `UINT64`
- `UINT32`
- `FLOAT`
- `DOUBLE`
- `BOOL`
- `LIST`
- `MAP`

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L273-L298)

注意：

- 没有 `MAP<string, STRUCT>` 分支
- 也没有 `MAP<string, TIME*>`、`MAP<string, TIMESTAMP>`、`MAP<string, BINARY>` 分支

### 4.4 `STRUCT` 支持哪些成员类型

`STRUCT` 最终走 `StructReader(...)`。

当前有明确分支的成员类型是：

- `INT32`
- `INT64`
- `UINT32`
- `UINT64`
- `FLOAT`
- `DOUBLE`
- `STRING`
- `BOOL`
- `LIST`
- `MAP`
- `STRUCT`

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L300-L365)

额外说明：

- `STRING` 成员还有一个特判
  - 如果底层数据数组实际上是 `DICTIONARY`
  - 会先解字典，再按字符串输出

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L329-L339)

### 4.5 `DICTIONARY` 支持范围

`DICTIONARY` 在两种模式下支持范围不完全一样。

`column` 模式：

- `Visit(const arrow::DictionaryArray&)` 会先把 `dictionaryArray_` 设好
- 然后对“字典值数组”再次调用 `Accept(this)`
- 因而它基本复用了当前 visitor 已有的类型分支

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L227-L232)

`row` 模式：

- 只对下面这些 value type 有显式 `switch` 分支：
  - `BOOL`
  - `TIME32`
  - `INT32`
  - `INT64`
  - `UINT32`
  - `UINT64`
  - `DOUBLE`
  - `STRING`

代码：

- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L235-L293)

所以如果只看当前实现，`row` 模式下的 `DICTIONARY` 支持面明显比 `column` 模式窄。

---

## 5. 输出细节

### 5.1 标量如何落成 JSON

最终写 JSON 文本的是 `SIMDJSONFormat`，它基于 `simdjson::internal::mini_formatter`。

映射关系很直接：

- 整数 / 浮点 -> `number(...)`
- `bool` -> `true_atom()` / `false_atom()`
- 字符串 -> `string(...)`
- raw string -> `mini_formatter_x::raw_string(...)`
- object / array -> `start_*` + `end_*`
- `null` -> `null_atom()`

代码：

- [simdjson_format.h](file:///root/Documents/stream_engine/src/util/arrowx/formats/simdjson_format.h#L12-L79)

### 5.2 时间类型如何输出

`TIME32` / `TIME64` / `TIMESTAMP` 不直接输出 epoch 数字，而是先经过：

- `timestamp_to_string(...)`

再作为字符串写入 JSON。

代码：

- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L235-L242)
- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L185-L195)

### 5.3 `json.unescape.fields`

这个配置控制某些字段按 raw string 输出，而不是普通 JSON string。

例如：

- 普通路径：写成 `"field":"hello\nworld"`
- raw 路径：直接把原始片段写进 JSON buffer

配置读取与初始化：

- [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.h#L46-L52)
- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L33-L55)

### 5.4 `ignore.json.fields`

这个配置会让对应字段在编码时被直接跳过。

代码：

- [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.h#L50-L52)
- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json/encoder.cpp#L57-L72)
- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L74-L78)
- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L165-L168)

### 5.5 `json.unfold.carry.field.name`

这个配置的作用不是“忽略列”，而是：

- 当当前列名命中该配置
- 且该列本身是 `MAP<string, T>`
- encoder 不再把这列写成：
  - `"carry": {"k1": v1, "k2": v2}`
- 而是把 map 中的 key/value 直接展开到外层对象：
  - `"k1": v1, "k2": v2`

代码：

- [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L88-L95)
- [visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L508-L545)

这个能力当前只在 `column` 模式路径里有显式开关。

---

## 6. 结论

如果只总结当前代码状态，可以归纳成下面几条：

- 默认 JSON encoder 是 `column` 模式，支持类型最完整
- 顶层字段明确支持：
  - `BOOL`
  - `INT32`
  - `INT64`
  - `UINT32`
  - `UINT64`
  - `FLOAT`（仅 `column`）
  - `DOUBLE`
  - `STRING`
  - `BINARY`（仅 `column`，按字符串路径处理）
  - `TIME32`
  - `TIME64`
  - `TIMESTAMP`
  - `LIST`
  - `MAP`
  - `STRUCT`
  - `DICTIONARY`
- `LIST<T>` 的 `T` 当前明确支持到：
  - 标量数值 / `STRING` / `BOOL`
  - 以及 `LIST` / `MAP` / `STRUCT`
- `MAP<string, T>` 要求 key 为字符串，`T` 当前明确支持：
  - `STRING`
  - 数值
  - `BOOL`
  - `LIST`
  - `MAP`
- `STRUCT` 当前明确支持成员类型：
  - 数值
  - `STRING`
  - `BOOL`
  - `LIST`
  - `MAP`
  - `STRUCT`
- `row` 模式不是和 `column` 模式完全等价的实现
  - 顶层缺少 `FLOAT` / `BINARY` 分支
  - `DICTIONARY` 的 value type 支持也更少

如果后续还要补这一块，最值得继续梳理的是：

- `row` 模式和 `column` 模式的能力差异是否是有意设计
- 未覆盖的 Arrow 类型是否需要显式报错或补齐
- `BINARY` 是否应该做 base64，而不是复用字符串路径
