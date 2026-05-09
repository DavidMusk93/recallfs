# JSON Encoder Map Key 支持方案重写

本文只讨论一件事：`Arrow MAP<K, V>` 如何稳定地编码成 JSON。

当前文档里的主要问题不是“功能不够多”，而是把“安全修复”“输出语义”“配置策略”混在一起，导致方案显得重，也不够自然。更合适的方向是先把语义收敛，再反推实现。

相关代码：

- `column` 模式 map 顶层分支：[visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L204-L217)
- `row` 模式 map 顶层分支：[visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L219-L226)
- map value 分发：[visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L273-L298)
- map/object 写出逻辑：[visitor.h](file:///root/Documents/stream_engine/src/util/arrowx/visitor.h#L508-L611)
- 其他模块对 map key 的保守检查：[array_builder.cpp](file:///root/Documents/stream_engine/src/util/arrowx/array_builder.cpp#L523-L527)、[decoder.h](file:///root/Documents/stream_engine/src/sql/encdec/prom/decoder.h#L111-L118)

---

## 1. 当前问题的本质

当前实现并不是“暂时不支持非 string key”，而是：

- 没有先检查 `map_type->key_type()`
- 直接把 key array 强转成 `arrow::StringArray`
- 继续按字符串布局读取 key

一旦真实 key 不是 `STRING`，就会进入未定义行为，结果可能是：

- 读出错误 key
- 生成内容损坏的 JSON
- 访问非法内存甚至崩溃

所以这里首先是一个 **类型安全问题**，其次才是 **JSON 投影策略问题**。

---

## 2. 先参考 Doris / ClickHouse 的取向

公开资料里，两类系统的取向很接近：

### 2.1 Doris 的启发

- `MAP<K, V>` 类型本身允许多种 key 类型
- 但生成 JSON object 的接口会把 “object key 必须可表示成字符串” 当成单独约束

这说明 Doris 的思路不是“Map 一旦进入 JSON 就自动神奇兼容一切 key”，而是把：

- **内部 Map 类型能力**
- **JSON object 的表达边界**

明确分层。

### 2.2 ClickHouse 的启发

- 在 JSON 格式里，`Map` 的 key 本质上也是按字符串对象键来表达
- 社区也专门讨论过：当 key 不是天然字符串时，是否应该允许用 array-of-tuples / entries 的形式输出

这说明 ClickHouse 也碰到了同一个根问题：

- JSON object 只能接受 string key
- typed map key 和 JSON object key 不是同一层抽象

### 2.3 可以提炼出的共同点

比起堆很多策略开关，更自然的设计是先固定一条原则：

> JSON object 只是 `Map` 的一种投影，不是 `Map` 的唯一真身。

于是后面的实现就会很顺：

- 能安全投影成 object 的，输出 object
- 不能安全投影成 object 的，输出 entries
- 任何情况下都先做类型检查，绝不做未校验强转

---

## 3. 推荐的语义模型

建议不要把主设计建立在 `strict` / `stringify` / `pairs` / `auto` 这样一组并列策略上。

更优雅的模型是：

### 3.1 一个默认语义

默认规则只保留一条：

1. `string` key
   - 输出 JSON object
2. 可稳定字符串化的标量 key
   - 输出 JSON object
   - key 先做规范化字符串化
3. 其余 key
   - 输出 entry array

也就是：

```text
object if safe, entries if not
```

这比“四档策略”更容易理解，也更接近 Doris / ClickHouse 体现出的边界意识。

### 3.2 两种输出形态

#### 形态 A：JSON object

适用于：

- `map<string, T>`
- `map<bool, T>`
- `map<int32, T>`
- `map<int64, T>`
- `map<uint32, T>`
- `map<uint64, T>`

示例：

```json
{"a":1,"b":2}
```

```json
{"1":"x","2":"y"}
```

```json
{"true":"x","false":"y"}
```

#### 形态 B：entry array

适用于：

- `map<struct<...>, T>`
- `map<list<...>, T>`
- `map<map<...>, T>`
- 以及第一版尚未定义稳定字符串格式的 key 类型

示例：

```json
[
  {"key":{"id":1},"value":"x"},
  {"key":{"id":2},"value":"y"}
]
```

这个形态有三个优点：

- 不丢 key 的结构信息
- 不伪造 JSON object key
- 不要求复杂 key 做不可逆字符串压缩

---

## 4. 推荐的 ASCII Graph

### 4.1 外部系统启发

```text
                 +----------------------+
                 |  Typed MAP<K, V>     |
                 +----------------------+
                            |
        +-------------------+-------------------+
        |                                       |
        v                                       v
+----------------------+           +---------------------------+
| Doris-style boundary |           | ClickHouse-style boundary |
+----------------------+           +---------------------------+
| MAP type is generic  |           | MAP type is generic       |
| JSON object key is   |           | JSON object key is        |
| a separate contract  |           | string-oriented in JSON   |
+----------------------+           +---------------------------+
        |                                       |
        +-------------------+-------------------+
                            |
                            v
                 +----------------------+
                 | Our encoder contract |
                 +----------------------+
                 | object if safe       |
                 | entries if not       |
                 +----------------------+
```

### 4.2 推荐编码流程

```text
+---------------------------+
| Encode Arrow MAP<K, V>    |
+---------------------------+
              |
              v
+---------------------------+
| Inspect key_type()        |
+---------------------------+
              |
              v
   +----------+------------------+------------------------+
   |                             |                        |
   v                             v                        v
+-----------+      +--------------------------+   +------------------+
| STRING    |      | Stable scalar key        |   | Complex key      |
| key       |      | BOOL/INT/UINT ...        |   | STRUCT/LIST/MAP  |
+-----------+      +--------------------------+   +------------------+
   |                             |                        |
   v                             v                        v
+-----------+      +--------------------------+   +------------------+
| emit      |      | stringify key            |   | emit entries     |
| object    |      | emit object              |   | [{"key":...}]    |
+-----------+      +--------------------------+   +------------------+
              \              |                        /
               \             |                       /
                +------------+----------------------+
                             |
                             v
                  +-----------------------+
                  | Emit valid JSON       |
                  | with no UB and        |
                  | no fake key coercion  |
                  +-----------------------+
```

---

## 5. 为什么这个模型更优雅

### 5.1 它把“修 bug”和“定语义”分开了

必须立即做的事只有一个：

- 去掉所有未校验的 `StringArray` 强转

这件事与最终支持哪些 key 类型无关，不应该依赖某个新配置项落地。

### 5.2 它把配置面缩小了

原方案的问题之一，是把很多分支都暴露成用户配置。

但从语义上说，真正稳定的公共行为只有两种：

- object
- entries

因此更建议：

- 先把默认行为固定好
- 如确实需要兼容开关，只保留极少数调试或保守模式

### 5.3 它更接近 JSON 本身

JSON 里：

- object key 只能是 string
- array 才适合表达任意结构化元素

所以复杂 key 落到 entries，不是“退化”，而是和 JSON 模型更对齐。

---

## 6. 第一版建议边界

建议第一版明确支持：

- `STRING`
- `BOOL`
- `INT32`
- `INT64`
- `UINT32`
- `UINT64`

这些类型进入 object 路径时，都要求使用确定性字符串化：

- 不依赖 locale
- 不依赖不稳定格式
- 同值同文本

第一版不建议急着支持这些 key 直接进 object：

- `FLOAT`
- `DOUBLE`
- `DECIMAL`
- `TIME32`
- `TIME64`
- `TIMESTAMP`

原因不是做不到，而是它们的 object-key 文本规范更容易引入争议：

- 精度问题
- 时区问题
- 文本规范化问题

这些类型第一版可以先走 entries。

---

## 7. 实现建议

### 7.1 抽一个统一的 key classifier

不要在 `column` / `row` 两条路径里各自写 key 假设。

建议抽出统一逻辑：

- `ClassifyMapKeyType()`
- `TryFormatMapKeyAsObjectKey()`
- `WriteMapAsObject()`
- `WriteMapAsEntries()`

这样能保证两种 encoder mode 行为一致。

### 7.2 保留 `map<string, T>` 快路径

常见场景仍然应该是：

- 直接 object 输出
- 不引入额外包装

这样不会破坏现有主流行为，也不会给常见 case 增加无意义开销。

### 7.3 明确错误边界

对于未支持直接 object 化的 key：

- 不要再强转
- 不要尝试模糊兜底
- 直接进入 entries，或者在明确要求 object-only 的场景里 fail-fast

---

## 8. 是否还需要配置项

建议文档里不要把配置项当成主方案。

如果实现阶段确实需要兼容或调试开关，最多保留：

- `default`
  - 使用本文推荐的默认语义
- `strict_object`
  - 只允许 `map<string, T>` 或显式允许的标量 key 进入 object
  - 其余直接报错，不走 entries

也就是说：

- **默认语义是产品行为**
- **strict 是防守或排障工具**

不要反过来把多个策略都抬成一等公民。

---

## 9. 落地计划

### 9.1 第一阶段：先收口风险

1. 找出所有 map key 的 `StringArray` 强转点
2. 补 `key_type()` 检查
3. 对非法路径先 fail-fast，彻底移除未定义行为

### 9.2 第二阶段：补默认语义

1. 支持 `string` key -> object
2. 支持稳定标量 key -> stringify -> object
3. 支持复杂 key -> entries

### 9.3 第三阶段：补测试与总文档

1. `map<int32, string>`
2. `map<uint64, int32>`
3. `map<bool, string>`
4. `map<struct<...>, string>`
5. 同时覆盖 `json.encoder.mode=column` 和 `json.encoder.mode=row`
6. 回写总文档 [json_encoder.md](file:///root/Documents/stream_engine/docs/codec/json_encoder.md)

---

## 10. 最终建议

建议把这次改动的主叙事收敛成一句话：

> 不要把“支持任意 map key”理解成“所有 key 都硬塞进 JSON object”。

更合理的编码契约是：

- **先做严格类型检查**
- **能安全投影成 object 的再输出 object**
- **否则输出 entries**

这样既能消除当前未定义行为，也更接近 Doris / ClickHouse 这类系统对 “typed map” 与 “JSON object” 关系的实际处理方式。

---

## 11. Doris / ClickHouse Case Demo 与出处

下面补充“可直接对照”的 case。分两类：

- 文档已明确的当前行为
- 社区讨论中的演进方向（不是已发布能力）

### 11.1 Doris Case

#### Case A：`MAP<K, V>` 类型本身允许多种 key 类型

结论：

- Doris 的 `MAP<key_type, value_type>` 文档明确 key 可以是多种基础类型
- 这说明 “Map 类型能力” 与 “JSON object 键约束” 是两层语义

Demo（基于文档示例改写）：

```sql
-- 文档示例中常见的是 MAP<STRING, ...>，但类型定义允许多种 key_type
CREATE TABLE demo_map (
  id INT,
  m MAP<STRING, INT>
);

INSERT INTO demo_map VALUES (1, {'a':100, 'b':200});
SELECT m['a'] FROM demo_map;
-- 预期: 100
```

出处：

- Doris MAP 类型文档：[MAP | Semi Structured](https://doris.apache.org/docs/dev/sql-manual/basic-element/sql-data-types/semi-structured/MAP/)

#### Case B：`JSON_OBJECT` 的 key 约束与字符串化

结论：

- `JSON_OBJECT` 文档把 key 作为 object key 语义处理
- 文档示例明确“非 string key 会被转成字符串 key”

Demo（文档原型）：

```sql
SELECT json_object(123, 456);
-- 预期: {"123":456}
```

```sql
SELECT json_object(NULL, 456);
-- 预期: 报错（key 不能为 NULL）
```

出处：

- Doris JSON_OBJECT 文档：[JSON_OBJECT](https://doris.apache.org/docs/dev/sql-manual/sql-functions/scalar-functions/json-functions/json-object/)

#### Case C：MAP 进入 JSON object value 时的行为

结论：

- 在 JSON_OBJECT 里，value 侧可以承载复杂类型（版本差异下可通过 TO_JSON/CAST 路径）
- 这进一步说明 Doris 在语义上是把 “key” 与 “value” 的 JSON 约束分开处理

Demo（文档示例风格）：

```sql
SELECT json_object('key', cast(map('abc', 'efg') as json));
-- 预期: {"key":{"abc":"efg"}}
```

出处：

- Doris JSON_OBJECT 文档（示例与说明）：[JSON_OBJECT](https://doris.apache.org/docs/dev/sql-manual/sql-functions/scalar-functions/json-functions/json-object/)

### 11.2 ClickHouse Case

#### Case D：Map 的底层语义是 `Array(Tuple(K, V))`

结论：

- ClickHouse 文档明确 `Map(K,V)` 内部实现等价于 `Array(Tuple(K,V))`
- 因此它天然更接近 “entries 序列”，而不是强绑定 JSON object

Demo（文档原型）：

```sql
SELECT CAST(([1, 2, 3], ['Ready', 'Steady', 'Go']), 'Map(UInt8, String)') AS m;
-- 预期: {1:'Ready',2:'Steady',3:'Go'}
```

出处：

- ClickHouse Map 类型文档：[Map(K, V)](https://clickhouse.com/docs/en/sql-reference/data-types/map)

#### Case E：Map key 并不要求必须是 String（类型层）

结论：

- ClickHouse `Map` 的 K 在类型系统层可为多种类型（受 Nullable 等约束）
- 但 JSON object 的 key 仍然是字符串语义，这两层并不等价

Demo（文档原型）：

```sql
CREATE TABLE tab (m Map(String, UInt64)) ENGINE=Memory;
INSERT INTO tab VALUES ({'key1':1, 'key2':10});
SELECT m['key2'] FROM tab;
-- 预期: 10
```

出处：

- ClickHouse Map 类型文档：[Map(K, V)](https://clickhouse.com/docs/en/sql-reference/data-types/map)

#### Case F：JSON 格式下 Map key 字符串化与 entries 形态的社区讨论

结论（讨论态，不是已发布承诺）：

- ClickHouse 社区 issue 明确提到：JSON 中 Map key 通常按字符串表示
- 并讨论在 JSON I/O 中支持 array-of-tuples 形态以减少类型损失

Demo（问题复现思路）：

```text
Map(Int64, String)
-> FORMAT JSONEachRow
-> key 会进入 JSON object key 语义（字符串化）
```

```text
社区提案方向:
Map(Int64, String)
<-> [[1,"a"],[2,"b"]]   (array-of-tuples / entries)
```

出处：

- ClickHouse issue（动态 key 场景）：[Support JSON structures with (maps) dynamic keys #78699](https://github.com/ClickHouse/ClickHouse/issues/78699)
- ClickHouse issue（Map JSON tuples 提案）：[Allow input/output Map data type as array of tuples for JSON formats #82085](https://github.com/ClickHouse/ClickHouse/issues/82085)

### 11.3 对本项目的直接启发

综合这些 case，可以把本项目的编码策略落成三条：

1. 类型层允许的 `Map<K,V>`，不等于 JSON object 必须能无损承载
2. object key 只在“可稳定字符串化”时走 object 路径
3. 其余 key 走 entries 路径，避免假性兼容和不可逆损失

### 11.4 Doris vs ClickHouse 对照表

| 维度 | Doris | ClickHouse | 对本项目的启发 |
| --- | --- | --- | --- |
| `Map` 类型定义 | `MAP<K, V>`，`K/V` 支持多种基础类型，也可嵌套复杂类型，见 [MAP](https://doris.apache.org/docs/dev/sql-manual/basic-element/sql-data-types/semi-structured/MAP/) | `Map(K, V)`，文档明确 `K/V` 可为多种类型，且内部实现为 `Array(Tuple(K, V))`，见 [Map(K, V)](https://clickhouse.com/docs/en/sql-reference/data-types/map) | `Map` 的类型能力应独立于 JSON object 的表达能力 |
| `Map` 的内部语义 | 更偏逻辑上的键值容器 | 文档直接暴露底层语义是 `Array(Tuple(K, V))` | 更容易接受 `entries` 视角，而不是只盯着 object |
| JSON object key 规则 | `JSON_OBJECT` 把 key 视为 object key；key 会转成 text，`NULL` key 直接报错，见 [JSON_OBJECT](https://doris.apache.org/docs/dev/sql-manual/sql-functions/scalar-functions/json-functions/json-object/) | JSON 输出同样受 “object key 只能是 string” 约束；社区 issue 也明确讨论了这一点，见 [#82085](https://github.com/ClickHouse/ClickHouse/issues/82085) | 进入 object 路径前，必须先判断 key 能否稳定字符串化 |
| 非 string key 进入 JSON object | 官方示例明确 `json_object(123, 456)` -> `{"123":456}` | 社区讨论默认 JSON 中 map key 会字符串化 | 标量 key 可以进入 object，但要有清晰、确定性的格式约束 |
| `NULL` key 行为 | `JSON_OBJECT(NULL, ...)` 报错，见 [JSON_OBJECT](https://doris.apache.org/docs/dev/sql-manual/sql-functions/scalar-functions/json-functions/json-object/) | 公开文档重点不在 `NULL` key，而在 JSON I/O 中 key 的字符串化限制 | object 路径需要 fail-fast，而不是模糊兜底 |
| 复杂 value 进入 JSON | `map/struct/json` 可作为 value 进入 JSON object，见 [JSON_OBJECT](https://doris.apache.org/docs/dev/sql-manual/sql-functions/scalar-functions/json-functions/json-object/) | value 侧支持复杂结构不是核心矛盾，核心仍是 key 的 JSON 表达 | key 与 value 的编码策略必须拆开看 |
| 复杂 key 的公开取向 | 官方文档没有主张“复杂 key 自动转 object key” | 社区已讨论用 `array-of-tuples` 表示 `Map` 的 JSON I/O，见 [#82085](https://github.com/ClickHouse/ClickHouse/issues/82085) | 复杂 key 更适合走 `entries`，不要硬塞成 object key |
| 动态 key / 半结构化场景 | 更偏用 JSON 函数和类型转换处理 object 语义 | 社区单独讨论了 dynamic keys 的 JSON structure 支持，见 [#78699](https://github.com/ClickHouse/ClickHouse/issues/78699) | 动态 key 应视为 JSON object 问题，不应混同普通 typed map |
| 更接近的设计哲学 | 强调 “Map 类型能力” 与 “JSON object 约束” 分层 | 更强调 “Map 本质是 entries，JSON object 只是受限投影” | 推荐默认规则：`object if safe, entries if not` |
