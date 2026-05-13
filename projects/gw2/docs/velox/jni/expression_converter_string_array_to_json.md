# ExpressionConverter: String Array -> Native JSON

本文聚焦 `ExpressionConverter.convertToNativeJson` 在遇到 `array<string>`（String Array）相关表达式/常量时，如何构造 Velox 侧可消费的 “native json expression” 字符串。

结论先行：

- `ExpressionConverter` 并不是 “把 Scala/Java 的 Array[String] 直接转成 JSON 文本”，而是把 Spark Catalyst 的 `Expression` 转成 Velox 的 `ITypedExpr`（如 `ConstantTypedExpr`/`CallTypedExpr`/`FieldAccessTypedExpr`）的 **序列化 JSON**。
- `array<string>` 的关键在于 **常量**（`Literal`）的处理：Scala 侧会先把常量写入一份 **Velox NativeColumnVector**（通过 `VeloxRowToColumnConverter`），再交给 JNI `nativeCreateConstantTypedExpr` 生成一个 `ConstantTypedExpr` 并序列化成 JSON。
- 对于 `array(...)` 这类构造数组的表达式（非整体 Literal），本质是一个 `CallTypedExpr("array", ...)`，其参数通常是若干个 `ConstantTypedExpr`（string literal）。

---

## 1. 入口与调用链

常见调用入口：

- dict2 rewrite：`RewriteWithGlobalDict` 通过 `DefaultExpressionJsonConverter` 调用 `ExpressionConverter.convertToNativeJson`
  - [RewriteWithGlobalDict.scala](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/dict2/RewriteWithGlobalDict.scala)
- 扫描侧 pushdown：`TideScanBuilder` 将 filter expr 转为 native json，下发给 Tide/Velox
  - [TideScanBuilder.scala](file:///root/Documents/gw2/gateway-catalyst/src/main/scala/org/apache/spark/sql/execution/TideScanBuilder.scala)

核心入口函数：

- [ExpressionConverter.convertToNativeJson](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L166-L171)

整体转换流程（简化）：

1. `ExpressionConverter.convertToNativeJson(expr)`
2. `convertToNative(expr)`：自底向上 `transformUp`
3. `AttributeReference` -> `nativeCreateFieldAccessTypedExpr`
4. `Literal` -> `nativeConstant` -> `nativeCreateConstantTypedExpr`
5. 其他表达式：
   - 命中特殊映射：`ExpressionConvertMapping.expressionsMap`（如 `In`/`GetArrayItem` 等）
   - 否则走兜底：`nativeCreateCallTypedExpr(prettyName, retType, argsJson...)`

参考实现：

- [ExpressionConverter.convertToNative](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L173-L210)
- [ExpressionConvertMapping](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/convert/ExpressionConvertMapping.scala#L60-L108)

---

## 2. String Array 的两种常见形态

### 2.1 `array("a","b",...)`：数组构造表达式（CallTypedExpr）

Spark Catalyst 中 `array(...)` 通常是 `CreateArray`（或类似表达式）。当前仓库没有为 `CreateArray` 写专用 converter，因此一般走兜底路径：

- 先把每个 string literal 转成 `ConstantTypedExpr`（见 2.2）
- 再把整个 `array(...)` 转成 `CallTypedExpr("array", "array<string>", args...)`

兜底逻辑位置：

- [ExpressionConverter.functionCall](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L122-L152)

注意：

- 这里的 “JSON” 不是 Spark 的 `to_json` 语义，而是 “Velox 表达式树的 JSON 序列化结果”。

### 2.2 `Literal(array<string>)`：整体数组常量（ConstantTypedExpr）

这类更贴近你问的 “string array 转 json”，典型来源是 `IN (...)` 重写时构造的 set 常量：

- [InConvert](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/convert/PredicateConvert.scala#L26-L56)
  - 会把 `IN (lit1, lit2, ...)` 的 list 合并成一个 `Literal(ArrayType(...))`
  - 然后走 `ExpressionConverter.nativeConstant(...)`

核心实现：

- [ExpressionConverter.nativeConstant](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L79-L104)

逻辑分解：

1. 用 `InternalRow.fromSeq(Seq(literal.value))` 构造一行 row（只有 1 列，值就是 `ArrayData`）
2. `VeloxRowToColumnConverter.getConverterForType(ArrayType(StringType), ...)`
3. 创建 `VeloxWritableColumnVector.createVector(1, ArrayType(StringType))`
4. `converter.append(row, 0, vector)` 把 array 内容写进 native vector
5. 对 array 做一次 child reserve（见下）
6. JNI：`NativeExpressionConvert.nativeCreateConstantTypedExpr(dt.catalogString, vector.getNative)`
7. 返回的是一个 `NativeJsonExpression(json, originalLiteral)`

其中这段是 array 常量的关键优化：

- `vector.getChild(0).reserve(row.getArray(0).numElements()) // to reduce json size`
  - [ExpressionConverter.scala](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L92-L96)
  - 背景：`VeloxWritableColumnVector` 初始化 array child 的默认 capacity 比较小/固定，reserve 到实际元素数后，native 侧序列化（或底层 vector metadata）更紧凑，因此注释里写 “reduce json size”。

---

## 3. Array<string> 写入 NativeColumnVector 的细节

`VeloxRowToColumnConverter` 负责把 Spark 的 row/array/utf8string 逐层写入 `WritableColumnVector`（最终 backed by native）。

关键点：

- Array：
  - `ArrayConverter.append`：`cv.appendArray(numElements)`，随后把每个元素写入 `cv.arrayData()`（child vector）
  - [VeloxRowToColumnConverter.ArrayConverter](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/plan/VeloxRowToColumnConverter.scala#L188-L198)
- String：
  - `StringConverter.append`：取 `UTF8String.getBytes`，用 `cv.putByteArray(...)` 写入
  - [VeloxRowToColumnConverter.StringConverter](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/plan/VeloxRowToColumnConverter.scala#L171-L176)
- putByteArray 的存储策略：
  - `VeloxWritableColumnVector.putByteArray` 会把短字符串 inline 存在 fixed region，长字符串走 `nativeColumnVector.allocateStringData(...)` 分配 buffer 并拷贝
  - [VeloxWritableColumnVector.putByteArray](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/VeloxWritableColumnVector.java#L734-L760)

因此，“string array 常量”在 JVM 侧会被编码成：

- 1 行的 array vector（offset/length）
- child string vector：每个元素的 bytes + length（以及可能的外部分配 buffer）

---

## 4. JNI：从 NativeColumnVector 到 JSON 的生成点

JVM 侧 JNI 声明：

- [NativeExpressionConvert.java](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/jni/NativeExpressionConvert.java)
  - `nativeCreateConstantTypedExpr(String resultType, NativeColumnVector vectorBatch)`
  - `nativeCreateCallTypedExpr(String functionName, String resultType, String[] args, boolean skipResolve)`
  - `nativeCreateFieldAccessTypedExpr(String column, String dt)`

实现位置（本仓库不包含 C++ 源码）：

- `starry-core` 以打包的 `native_engine` 资源形式携带 native 库：
  - `starry/starry-core/src/main/resources/native_engine`（gzip 压缩包）
  - 解压可见 `lib/libvelox.so`
- 通过对 `libvelox.so` 做符号/字符串扫描可以确认其内部包含以下实现符号：
  - `facebook::velox::sdk::expression::NativeExpressionConvert::nativeCreateConstantTypedExpr(...)`
  - `facebook::velox::sdk::expression::NativeExpressionConvert::nativeCreateCallTypedExpr(...)`
  - `facebook::velox::sdk::expression::NativeExpressionConvert::nativeCreateFieldAccessTypedExpr(...)`
  - 以及 `facebook::velox::sdk::vector::NativeColumnarVector::nativeSerialize(...)`

这里的 “native json” 由 Velox 侧的 `ConstantTypedExpr`/`CallTypedExpr`/`FieldAccessTypedExpr` 等对象 `serialize()` 得到（序列化细节在该 repo 内不可见）。

---

## 5. 实战定位：IN + String List 为什么会变成 array<string> 常量

当你写：

```sql
where col in ('a','b','c')
```

在 columnar/native 路径里，`InConvert` 会把 list 常量合并成一个数组常量（`ArrayType(colType)`），再交给 native：

- 这一步决定了会走 2.2 的 “整体数组常量 -> ConstantTypedExpr” 路径，而不是把 `in` 直接展开成多个参数。
- 这样做通常是为了匹配 native 侧 `in`/`in_set` 之类函数签名（一个 value + 一个 set）。

参考：

- [InConvert](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/convert/PredicateConvert.scala#L26-L56)
- [ExpressionConverter.nativeConstant](file:///root/Documents/gw2/starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/ExpressionConverter.scala#L79-L104)

---

## 6. 你可能会关心的边界与坑

- `ExpressionConverter` 对 array 的 “转 json” 依赖 JNI/native lib，如果单测环境不加载 JNI，相关路径通常会被 mock 掉（dict2 suite 里就是这样做的）。
- `nativeConstant` 对 `ArrayType` 的 child reserve 在 `literal.value == null` 时会被 catch 掉（避免 NPE），因此 `null` array 常量可能会产生不同的序列化体积/形态。
- `array<string>` 里元素的 nullability 由 `ArrayType.containsNull` 决定，最终是否写 null bitmap 取决于 `VeloxWritableColumnVector` 的 null 处理逻辑。

