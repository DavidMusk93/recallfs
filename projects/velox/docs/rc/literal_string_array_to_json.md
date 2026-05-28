## Literal String Array To JSON (Root Cause)

本文聚焦“根因 + 修复逻辑”，不记录构建排坑细节（构建经验见 `docs/xp/`）。

---

## 1. 问题定义

现象：

- 同一个字符串数组字面量（`ARRAY<VARCHAR>` literal）在序列化为 native expression JSON 时，`ConstantTypedExpr.valueVector`（base64）不稳定。
- DAG/plan JSON 中能看到明显“垃圾/冗余”数据（例如 string buffer 远大于真实有效字节）。

关键约束：

- 上层会把这类 JSON 当作缓存键/比对对象使用，因此输出必须按**逻辑值**稳定，而不能受底层**物理布局**影响。

---

## 2. 根因：序列化泄露了物理布局

`ConstantTypedExpr::serialize()` 对复杂常量走 `valueVector_` 路径：

- 使用 `VectorSaver::saveVector(...)` 将 `valueVector_` 二进制序列化
- 然后对二进制做 base64，写入 JSON 的 `valueVector` 字段

问题在于：`saveVector` 序列化的是 vector 的 **physical layout**，而不是“只由 logical value 决定的 canonical form”。

当 vector 具有以下物理冗余时，即使 logical value 相同，输出也会变化：

- `ARRAY` 的 `elements` child vector 存在未使用的尾部 rows（`elements.size` 大于 offsets/sizes 实际引用范围）
- `VARCHAR/VARBINARY` 的 `stringBuffers` 包含已分配但未使用的尾部字节（buffer size/capacity 很大）

这类尾部字节可能来自未初始化内存或历史写入残留，因此会表现为：

- base64 内容不同
- JSON 字符串不同
- DAG 中“看起来像垃圾数据”

一句话总结：

- 之前输出依赖 physical layout（buffer 大小/尾部/未使用 rows）
- 但我们需要的契约是：输出只依赖 logical value

---

## 3. 修复点：在序列化边界做 normalize

修复点在 [ConstantTypedExpr::serialize](file:///root/Documents/velox/velox/core/Expressions.cpp#L206-L217)：

- Before：`saveVector(*valueVector_, out)`
- After：先 `normalizeValueVectorForSerialization(valueVector_)`，再 `saveVector(*normalized, out)`

normalize 的目标：把复杂常量规整成“由逻辑值唯一决定”的形态，然后再序列化。

---

## 4. 修复流程图（Before / After）

### Before（序列化依赖物理布局）

```text
Literal(array_string)
  -> ConstantTypedExpr(valueVector_)
    -> serialize()
      -> saveVector(valueVector_)  // writes physical layout
        -> Base64 -> JSON.valueVector  (unstable)
```

### After（序列化前先 normalize）

```text
Literal(array_string)
  -> ConstantTypedExpr(valueVector_)
    -> serialize()
      -> normalizeValueVectorForSerialization(valueVector_)
        -> BaseVector::copy (canonicalize ranges for ARRAY/MAP/ROW)
        -> compactStringBuffersForSerialization (shrink stringBuffers)
      -> saveVector(normalized)
        -> Base64 -> JSON.valueVector  (stable by logical value)
```

---

## 5. `vector->encoding()` 分支行为（理解 compact 的递归）

`compactStringBuffersForSerialization` 会按 `vector->encoding()` 做递归，但它**不假设** `ARRAY/MAP` 的 child 一定是 string。

| encoding | 典型例子（伪结构） | 行为 |
|---|---|---|
| `LAZY` | `LazyVector<...>` | 先物化再递归 |
| `FLAT` | `FlatVector<StringView>` 且 type 是 `VARCHAR/VARBINARY` | 进行 string buffer 压实（只保留真实引用字节） |
| `FLAT` | `FlatVector<int64_t>` 等非 string | 不处理，直接返回 |
| `ROW` | `RowVector{ child0, child1, ... }` | 遍历 children，递归 |
| `ARRAY` | `ArrayVector{ offsets/sizes, elements }` | 递归到 `elements()` |
| `MAP` | `MapVector{ keys, values }` | 分别递归到 `mapKeys()` / `mapValues()` |
| 其他 | `DICTIONARY/CONSTANT/...` | 本次修复中默认不处理；通常已被 `BaseVector::copy` 收敛到上述结构 |

---

## 6. ARRAY encoding vs FLAT encoding（术语澄清）

- `FLAT`：标量列最直接的物理表示，一行一个值。
- `ARRAY`：array 容器表示，一行是一个 **array view/slice**，通过 `offsets[i]`/`sizes[i]` 引用 `elements` child vector 中的一段范围。

关键点：

- `array->elements()` 是 child vector，本身的 encoding **可能是 FLAT，也可能不是**（例如 DICTIONARY/LAZY/CONSTANT）。
- 这就是为什么 ARRAY 分支只做递归，而真正的 string 压实只发生在 `FLAT + VARCHAR/VARBINARY` 分支。

---

## 7. 回归测试

测试用例在 [TypedExprSerdeTest.cpp](file:///root/Documents/velox/velox/core/tests/TypedExprSerdeTest.cpp#L219-L245)：

- 构造两个 logical value 相同但 physical layout 不同的 `ARRAY<VARCHAR>` constant
  - 一个紧凑的 elements/stringBuffers
  - 一个稀疏的 elements（额外 rows）+ 超大 string buffer（例如 64KB）
- 断言两者 `serialize()["valueVector"]` 完全一致
- 再 restore 并校验元素内容与 size 收敛

