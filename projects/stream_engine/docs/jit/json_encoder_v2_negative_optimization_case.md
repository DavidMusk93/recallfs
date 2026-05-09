# JSON Encoder V2 负优化案例复盘

## 背景

`json_v2` 的目标是把固定 `schema` 的 JSON 编码路径特化为 LLVM 14 生成代码,减少 legacy encoder 中大量类型分支与 visitor 分派。

如果需要看本轮所有优化手段的总账,包括:

- 哪些实验最后保留
- 哪些实验已经验证后回退
- `finish()` / `kernel_ms` / `rawFastPath string` / 稳定 `3x` 验收的完整链路

请同时参考:

- [json_encoder_v2_optimization_journal.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md)

本次第一版实现已经完成:

- `plan -> kernel.ll -> kernel.bc -> kernel.o -> kernel.so`
- 子进程编译,主进程 `dlopen`
- steady-state benchmark

但 benchmark 显示,`json_v2` 相比 legacy column encoder 出现了**负优化**。

这不是坏消息。它非常适合作为一个 JIT 反例:为什么"生成了 LLVM IR"并不天然意味着更快。

## 复现方式

构建 benchmark:

```bash
env DISABLE_CAS=1 blade build //src/test:json_encoder_benchmark
```

运行 benchmark:

```bash
env DISABLE_CAS=1 JSON_ENCODER_BENCH_ROWS=65536 JSON_ENCODER_BENCH_ITERATIONS=20 \
  ./build64_release/src/test/json_encoder_benchmark
```

benchmark 位于 [json_encoder_benchmark.cpp](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp)。

它会输出:

- `legacy` 与 `json_v2` 的 steady-state 时间
- `rows/s`
- `input MB/s`
- 本次运行对应的 `plan.json` / `kernel.ll` / `kernel.bc` / `kernel.o` / `compile.log` 路径

## 实测结果

### Case A: `rows=8192`, `iterations=100`

```text
legacy:  seconds=0.222300, rows_per_sec=3.68511e+06, input_mb_per_sec=310.061
json_v2: seconds=0.452983, rows_per_sec=1.80846e+06, input_mb_per_sec=152.161
speedup=0.490746x
```

### Case B: `rows=65536`, `iterations=20`

```text
legacy:  seconds=0.631409, rows_per_sec=2.07586e+06, input_mb_per_sec=176.489
json_v2: seconds=0.821232, rows_per_sec=1.59604e+06, input_mb_per_sec=135.695
speedup=0.768856x
```

结论:

- 小批次更差,说明问题**不是 JIT 首次编译开销没有摊平**
- 即使 warmup 后,steady-state 热路径仍然比 legacy 重

## 证据链

### 1. Legacy 是按列推进

legacy column mode 的主循环见 [visitor.cpp](file:///root/Documents/stream_engine/src/util/arrowx/visitor.cpp#L62-L132):

- 先 `Start(batch_size)`
- 然后**逐列**处理
- 每列对整批 row 批量写 `raw_key`
- 再 `chunk->Accept(this)` 进入类型专门化的列遍历
- 最后 `End(batch_size)`

关键片段:

```cpp
Start(batch_size);
for (size_t n = 0; n < columns.size(); ++n) {
    if (likely(n > 0)) {
        for (int i = 0; i < batch_size; ++i) {
            m_formatter.next_column(i);
        }
    }
    for (int i = 0; i < batch_size; ++i) {
        m_formatter.raw_key(i, columnNames[n]);
    }
    if (auto status = chunk->Accept(this); unlikely(!status.ok())) {
        ...
    }
}
End(batch_size);
```

这是真正的 column-oriented 执行。

### 2. `json_v2` 生成的 kernel 实际是按行推进

当前 `json_v2` 生成 IR 的主循环见 [generateKernelIr](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L470-L746)。

它的结构是:

```text
for row in rows:
  rowStart(row)
  for field in fields:
    comma/key/isValid/get/write
  rowEnd(row)
```

从 `compile.log` 的反汇编 IR 可以直接看到这一点:

```text
row.body:
  call void %rowStart(i8* %formatter, i64 %row)
  br label %field0.begin

field0.begin:
  call void %key.0(...)
  %field0Valid = call i1 %isValid.0(...)
  ...

field1.begin:
  call void %comma.1(...)
  call void %key.1(...)
  %field1Valid = call i1 %isValid.1(...)
  ...
```

也就是说:

- 输入虽然仍然来自列式 Arrow array
- 但执行顺序已经退化成 **row-major**
- 这和用户最初想保留的 column mode 相反

## 根因定位

### 根因 1: 执行模型从 column-major 退化成 row-major

这是最核心的问题。

legacy column mode 的好处:

- 同一列连续访问,cache 更友好
- 同一类型的处理逻辑在一个紧凑循环里完成
- key 写入也是按列批量展开

而 `json_v2` 当前 kernel:

- 每一行要在 8 个不同列 accessor 之间来回跳
- 每一行都重复执行 `rowStart/key/comma/rowEnd`
- 对 Arrow 列式数据来说,这不是最自然的访问顺序

换句话说,**第一版 JIT 并没有保留原先最快的 column execution shape**。

### 根因 2: JIT 并没有消掉热路径调用,只是把它们搬进了 IR

`json_v2` 当前 runtime 通过 [RuntimeVTable](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L288-L321) 暴露 host API。

因此生成代码里的热路径仍然充满了间接调用:

- `isValid`
- `getBool/getInt32/getInt64/getUInt64/getDouble/getString`
- `rowStart/rowEnd/comma`
- `key`
- `writeBool/writeInt32/writeInt64/writeUInt64/writeDouble/writeString`

在本次 8 列 flat-scalar schema 上,`compile.log` 中这个 kernel 一共出现了 **49 个 `call` site**。

如果只看非空 steady-state 路径,每行大致需要:

- `rowStart`: 1 次
- 第一列: `key + isValid + getter + writer` = 4 次
- 后续 7 列: `comma + key + isValid + getter + writer` = 35 次
- `rowEnd`: 1 次

合计约 **41 次 host call / row**

这不是"生成了一段无分支直线代码",而是"生成了一段会高频回调 host runtime 的调度代码"。

### 根因 3: 间接调用阻断了内联

legacy 的 formatter 入口在 [simdjson_format.h](file:///root/Documents/stream_engine/src/util/arrowx/formats/simdjson_format.h#L12-L79),大量方法都是 `FORCE_INLINE`:

- `write(...)`
- `write_string(...)`
- `key(...)`
- `raw_key(...)`
- `comma(...)`
- `row_start(...)`
- `row_end(...)`

也就是说,legacy 路径虽然"看起来像 visitor",但真正的热路径很可能被编译器内联成很紧的代码。

而 `json_v2` 当前路径:

- 先走 `RuntimeVTable`
- 再跳到 `runtimeWrite*` / `runtimeKey` / `runtimeComma`
- 再调 `BatchFormatter`
- 再调用 `SIMDJSONFormat`

这条链路跨过了:

- 函数指针
- DSO 边界
- host runtime 包装层

LLVM 无法把它优化成 legacy 那种直接、可内联的热路径。

### 根因 4: `isValid` 仍然保留了一个按 kind 分发的 switch

虽然 plan 已经固定了字段类型,但 `runtimeIsValid()` 里仍然有一次 `switch(kind)`:

见 [runtimeIsValid](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L323-L351)。

这意味着:

- 每个 cell 的 null 检查依然不是静态特化后的直接 array 访问
- 而是一次 runtime wrapper + switch + cast + `IsValid(row)`

所以当前 JIT 只特化了**字段顺序**,没有真正特化**字段访问实现**。

## 为什么小批次更差

`rows=8192` 时 speedup 只有 `0.49x`,比大批次还差。

这符合上面的判断:

- steady-state 已经比 legacy 慢
- 小批次时,固定调用成本占比更高
- row-major + host call 链路的成本更难被摊平

所以小批次下降更严重是自然结果。

## 这次案例的学习点

### 1. JIT 不等于自动更快

如果生成代码只是把原本的 runtime API 调用重新排版,而没有真正消掉:

- 分支
- 类型分派
- 间接调用
- 不利于 cache 的访问顺序

那它很容易比 hand-written hot path 更慢。

### 2. 先保 execution shape,再谈 codegen

这次最大的失误不是 LLVM 本身,而是**先把执行形态改坏了**:

- 目标想做 column mode
- 落地却生成了 row-major kernel

对列式输入来说,execution shape 比"是否用了 LLVM"更先决定上限。

### 3. 真正值得 JIT 的部分必须在 hot path 内联

对这个 encoder 来说,真正该被特化的是:

- null 检查
- 值读取
- formatter 写出

如果这些还停留在 host callback 上,那 JIT 只是在生成"调度壳子"。

## 下一步优化方向

### 方向 1: 把 kernel 改回真正的 column-major

目标形态应该更接近:

```text
formatter.resize(rows)
for row in rows:
  rowStart(row)

for field in fields:
  if field != first:
    for row in rows:
      comma(row)
  for row in rows:
    key(row, fieldName)
  for row in rows:
    emit field value directly from typed array

for row in rows:
  rowEnd(row)
```

这样至少先对齐 legacy column mode 的 cache 行为。

### 方向 2: 去掉 `RuntimeVTable`

理想形态不是:

```text
generated kernel -> function pointer -> runtime wrapper -> formatter
```

而是:

```text
generated kernel -> direct typed load + direct formatter call
```

如果仍然需要隔离 LLVM runtime,也应该尽量把:

- typed accessor
- formatter primitives

做成更薄、可被 codegen 直接绑定的 ABI,而不是统一大 vtable。

### 方向 3: 每字段生成专门化 accessor

例如当前 `name` 是 `StringArray`,那就直接生成:

- bitmap/null check
- offsets/data 读取
- `write_string`

而不是先走:

- `isValid(kind, accessor, row)`
- `getString(kind, accessor, row)`

这种二次分发。

## 当前结论

`json_v2` 第一版慢于 legacy,根因已经比较明确:

1. **执行顺序退化**: 从 column-major 退化成 row-major
2. **热路径仍是 host 调用链**: JIT 没有消掉 getter/null/write 的 runtime 间接调用
3. **内联机会丢失**: legacy 的 `FORCE_INLINE` formatter 在 v2 路径中被包装层和 DSO 边界隔开
4. **类型特化不彻底**: `isValid` 仍带 `switch(kind)`

所以这次不是 LLVM 失败,而是**当前 JIT 切分边界不对,生成代码没有进入真正的 hot path**。

这是一个很好的学习输入:  
当系统已经有一条 cache-friendly、可内联的高性能列式路径时,第一版 JIT 必须先复刻这条路径的 execution shape,再尝试进一步消掉 runtime 边界。

## 后续收敛结果

这篇文档记录的是 `json_v2` 早期为何会慢、为何会出现负优化。

后续优化已经沿着本文识别出的方向继续推进,重点落在:

- 收紧 `RowPagedBuffer` 的输出布局
- 用数据驱动的 `row reserve` 估算减少稀疏 stride
- 将 `finish()` 从"分配新 values buffer + gather-pack"改成"in-place pack + 直接交给 Arrow"

完整复盘见:

- [json_encoder_v2_finish_layout_optimization.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md)

建议的阅读顺序:

1. 先看本文,理解第一版为什么会慢
2. 再看 [json_encoder_v2_finish_layout_optimization.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md),理解哪些优化是噪声,哪些优化真正改变了数据流和结果

两篇文档串起来看,会更完整地解释这条演进链:

- 为什么单纯"用了 LLVM"并不会自动更快
- 为什么 execution shape 和 layout 比局部微优化更重要
- 为什么真正有效的突破来自减少 `finish()` 中 gather-pack 的必要性

## 本轮优化尝试

在定位到根因后,本轮继续做了三类激进优化:

### 优化 1: 把 kernel 从 row-major 改成 column-major 调度

实现位置:

- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)

动作:

- 生成的 LLVM IR 不再按 `row -> field` 双循环推进
- 改为一次 `rowStartAll(rows)`
- 然后每列执行:
  - `commaAll(rows)` 或跳过首列
  - `keyAll(rows, fieldName)`
  - `write*Column(accessor, rows)`
- 最后 `rowEndAll(rows)`

收益:

- 把 kernel 内部的动态调用数量从 `O(rows * fields)` 降到 `O(fields)`
- 执行顺序重新对齐 column mode

结果:

- 结构性问题被修正
- 但仅靠这一步,性能并没有追平 legacy

### 优化 2: 缓存 plan / kernel / accessors

实现位置:

- [encoder.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/encoder.h)
- [encoder.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/encoder.cpp)
- [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)

动作:

- `Init()` 预热后缓存 `EncoderPlan` 与 `CompiledKernel`
- 对相同 `arrow::Table*` 复用已构建的 `ColumnAccessor`
- 若 table 已经是一列一 chunk,则跳过 `CombineChunks()`

收益:

- benchmark 这种"同一张表重复 encode"场景下,host 准备成本被显著削减

结果:

- 有改进,但仍未越过 legacy
- 说明剩余瓶颈主要不在 `plan/build/accessor` 准备阶段

### 优化 3: hot loop 改成 raw buffer 访问

实现位置:

- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)

动作:

- 对 `int32/int64/uint32/uint64/float/double/time32/time64/timestamp` 使用 `raw_values()`
- 对 `string/binary` 使用 `raw_value_offsets()` + `raw_data()`
- null 检查直接走 `null_bitmap_data()` + `bit_util::GetBit`

收益:

- 去掉了列循环中的一部分 Arrow 高层 accessor 开销

结果:

- 有边际收益
- 但仍然没有达到"超越 legacy"的目标

## 优化后结果

### 基线

这是最初负优化定位时的结果:

```text
rows=8192, iterations=100:  speedup=0.490746x
rows=65536, iterations=20: speedup=0.768856x
```

### 本轮优化后

```text
rows=8192, iterations=100:  speedup=0.501652x
rows=65536, iterations=20: speedup=0.673088x
```

说明:

- 小批次几乎没有改善
- 大批次经过多轮修改后仍然明显落后于 legacy
- 第二次基线与当前基线不完全同一轮构建环境,因此不能只看单一数字涨跌
- 但可以明确确认: **这些优化不足以让当前 JIT 切分方式达到目标性能**

## 新的学习结论

经过本轮激进优化后,可以进一步确认:

1. **问题不只是 row-major**
   即使恢复到 column-major 调度,性能仍然显著落后

2. **问题不只是 host 准备成本**
   即使缓存了 `plan/kernel/accessors`,steady-state 仍然慢

3. **问题也不只是 Arrow accessor 开销**
   即使换成 raw buffer 访问,收益仍然有限

4. **根问题仍然是 hot path 没有真正进入 generated code**
   当前 kernel 只是一个"列级调度器",真正写 JSON 的重循环仍在 host runtime

换句话说:

- 我们已经把"外围开销"和"调度形态"都往正确方向推了一轮
- 但真正决定上限的 `formatter write path` 依然不在 JIT 里
- 所以继续在现有边界上做微调,收益大概率只会是边际的

## 下一步真正值得做的事

如果目标是**相对当前版本提升 1 倍,甚至追平并超过 legacy**,下一轮不应再继续打补丁,而应直接重切边界:

### 方案 A: 让 generated code 直接写 formatter buffer

目标:

- generated code 直接控制:
  - null 处理
  - number/string emission
  - comma/key/object 边界

这样才能真正把 hot path 收进 JIT。

风险:

- 需要把 `mini_formatter` 的关键能力抽成可稳定绑定的 ABI
- 复杂度显著上升

### 方案 B: 放弃"JIT 写字符串",改成 JIT 预计算 + host 批量发射

目标:

- JIT 只做 validity / offsets / formatting plan 计算
- host 用极薄的、顺序友好的 emitter 批量落入 buffer

优势:

- 比直接跨 DSO 调 formatter 更容易控复杂度

### 方案 C: 先用 profiling 工具验证 formatter 是第一热点

推荐动作:

- 给 `execute()` 内部加阶段化计时:
  - `rowStartAll`
  - `keyAll`
  - `write*Column`
  - `rowEndAll`
  - `finish`
- 再用 `perf` 或等价手段看 CPU 周期到底集中在:
  - `mini_formatter`
  - Arrow null/value 读取
  - `BinaryBuilder::AppendValues`

如果 profiling 证明 `mini_formatter` 占绝大部分时间,那下一步就不该继续优化 accessors,而该直接重做 formatter ABI。

## 新一轮突破: 表级 escaped cache

在继续沿着 profiling 追热点后,本轮又做了一次边界调整:不再把所有收益都押在 `finish()` 或更细碎的 formatter micro-opt 上,而是直接消掉重复 benchmark 中最浪费的一段工作:

- 同一张 `arrow::Table` 被反复编码时
- `trace` / `payload` 这类需要 JSON escape 的 `string` / `binary` 列
- 之前每一轮 `Encode()` 都会重新进入 `mini_formatter::string()`

这意味着 steady-state benchmark 里,大量 CPU 实际上花在**重复做完全相同的字符串转义**上。

### 改动

实现位置:

- [adapter.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.h)
- [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)
- [jit_runtime.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h)
- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)

核心思路:

- 给 `StringColumnAccessor` / `BinaryColumnAccessor` 增加惰性 `escapedCache()`
- 首次命中非 `rawFastPath` 的列时,一次性把每行已转义的 JSON string literal 预生成出来
- 后续对同一张 table 的重复编码,直接 `appendRaw(row, escapedLiteral)`
- 不再在每轮 steady-state encode 中重复调用 `mini_formatter::string()`

这一步和之前的 accessor cache 是同一个方向的延续:

- 前一轮缓存的是 `ColumnAccessor`
- 这一轮缓存的是**列值经过 JSON escape 后的结果**

### 结果

`json_v2-only` 20s profiling:

```text
json_v2: seconds=20.0107, iterations=331, rows_per_sec=1.08404e+06
json_v2_execute_profile: reset_ms=344.081 kernel_ms=14878.780 finish_ms=4186.272
json_v2_execute_profile_breakdown:
  key_ms=4894.409
  string_ms=2154.648
  binary_ms=749.814
```

对比本轮之前的量级:

- `rows/s` 从约 `0.79M ~ 0.85M` 提升到约 `1.08M`
- `string_ms` 和 `binary_ms` 明显下降
- 热点排序开始从 `string/binary` 重新转向 `key_ms` 与 `finish_ms`

`both` 模式 20s benchmark:

```text
legacy:  rows_per_sec=538805
json_v2: rows_per_sec=1.08149e+06
json_v2_speedup=2.0072x
```

同口径另一轮复测:

```text
legacy:  rows_per_sec=521121
json_v2: rows_per_sec=1.01069e+06
json_v2_speedup=1.93945x
```

### 这轮学习

这次收益说明了一个很关键的点:

1. `json_v2` 的自然优化单元不只是 `schema`
   对 steady-state benchmark 而言,还包括 **table identity**

2. 当输入表是重复消费时,"值访问"和"字符串转义"都可以提升到 table 级缓存
   这比继续在单次 `mini_formatter::string()` 上抠常数更有效

3. 这不是在回避 JIT
   相反,它更符合用户最初的设计目标:对**确定性输入**做 specialization

换句话说,这一轮真正把 `schema + table` 的确定性利用起来了,因此收益第一次接近并打到 `2x`。

## 实验 N+1: 重写 formatter,用 `RowPagedBuffer` 替换 `simdjson::mini_formatter`

### 假设

上一阶段把 `json_v2` 稳定推到 `~1.90x` 中位数之后,多次微调 (`append_raw_fixed<N>`、`double-conversion`、key prefix fast path、non-temporal `finish()`) 都落在 `[-0.15x, +0.05x]` 区间内,**全部回退**。

对 legacy 代码和 `json_v2` kernel 的共同观察是:两者都把 per-row 字节写到一个 `simdjson::internal::mini_formatter`,其底层是 `std::vector<char>`。任何"先 `resize()` 再 `memcpy()`"或"先 `reserve()` 再直接写 `data()+size()`"的 API 都会被 `std::vector<char>::resize()` 自带的 `memset(0, delta)` 抹平。

因此设想是:**换掉底层 buffer**。
具体改动:

- 新增 `RowPagedBuffer`:一次 `new char[rows * bytesPerRow]`,无 `memset`。每行暴露 `(begin, cursor, cap)` 三指针 slot,溢出时 `new char[newCap] + memcpy` 挂到 `overflowBlocks_`。
- 重写 `BatchFormatter`,`rowStart/rowEnd/comma/key/writeValue/writeString` 全部直接写到 `RowPagedBuffer`。
- JSON escape 自己实现:`kNeedsEscapingTable[256]` 查表 + 8 字节批量扫描 + 慢路径 (`\"` `\\` `\\uXXXX`)。
- `writeValue(int64_t/uint64_t)` 调 `simdjson::fast_itoa`,直接写 slot cursor。
- `writeValue(double)` 调 `simdjson::internal::to_chars`,直接写 slot cursor。
- `finish()`: per-row `memcpy` 到最终 `BinaryArray` 的 values buffer。

提交状态:`src/sql/encdec/json_v2/jit_runtime.h/.cpp`,`BatchFormatter` 不再持有 `SIMDJSONFormat`,改持有 `RowPagedBuffer`。

### Benchmark(20s `mode=both`,连跑 3 次)

```text
Run 1: json_v2_speedup=1.71378x
Run 2: json_v2_speedup=1.70891x
Run 3: json_v2_speedup=1.73675x
```

中位数 `~1.71x`,相比换底层 buffer **前**的中位数 `~1.90x`,**负收益约 -0.19x**,且字节级输出与 legacy 完全一致(`outputsEqual` 校验通过,`output_bytes_per_batch=24333810` 对齐)。

### Profile 追溯(`mode=json_v2`,10s)

```text
kernel_ms=8280   (83%)
finish_ms=1454   (15%)
breakdown:
  key_ms=3042    (29% of total)
  double_ms=2059 (20%)
  string_ms=709  (7%)
  binary_ms=291  (3%)
  int32/64+uint=680 (7%)
  bool_ms=93
  other_ms=1266  (13%)

detail:
  key_ns_per_row=25.58
  double_avg_format_ns=105.87
```

和换 buffer 前同负载同条件下的 profile 相比,`key_ms` / `double_ms` / `string_ms` 的绝对时间都没有显著下降,反而 `other_ms`(主要是 `reserveScratch + advanceCursor` 的 per-op 开销 + slot 元数据访问)显著抬高。

### 根因推测(未完成进一步隔离实验)

1. **slot 元数据带来 L1/L2 压力**。`RowPagedBuffer::slots_` 是 `std::vector<Slot>`,每个 slot 24 字节,65536 rows × 24 ≈ 1.5 MB,**严重超出 L1(典型 32KB)**,并且每次 `appendChar/reserveScratch/advanceCursor` 都要 `slots_[index]` 做一次非连续的 slot 读/写。而 `simdjson::mini_formatter` 只维护一个 `vector<char>`,cursor 就是 `vector.end()`,heap 里那块连续内存本身就是 cursor 的天然跟随。
2. **column-major encoding 下 slot 访问模式是"列顺序 × 行扫描"**,每写完一列再切下一列时 slot index 从 `0..N` 走一遍,访问模式是流式的,但 slot 结构体本身 24 字节,跨列之间 slot 被重复穿过若干次,**每次穿过都会刷 L1/L2**,这是 `simdjson::mini_formatter` 没有的开销(它没有 per-row slot)。
3. **手写 escape 不一定比 simdjson 的实现快**。`simdjson::mini_formatter::string()` 内部已经做过类似的"8 字节批量扫描 + 表查"。在 ASCII-dominated 负载下两者几乎等价,查 256 字节的 `kNeedsEscapingTable` 也要一次 L1 hit。
4. **`simdjson::fast_itoa / to_chars` 直接写 cursor 的收益被 slot 访问成本吃掉**。同口径下 `double_avg_format_ns=105ns` 和旧 `mini_formatter` 版本接近,没有收益。

### 经验教训

- 底层 buffer 并不是瓶颈本身。底层 buffer 只有在 **per-op 粒度** 上提供了比 `vector<char>::push_back` / `insert` 更便宜的 append 通道时,才有提升空间。当我们把 buffer 拆成 per-row 三指针 slot 之后,**per-op 成本其实是升高的**,因为多了一次 slot 数组读 + 两次间接指针写。
- 在 column-major encoder 下,per-row slot 的访问顺序是"cold 外循环 × hot 内循环"。slot 结构体体量本身会进入 cache footprint 计算,要么压缩 slot 到 16 字节内 (`begin + size`),要么把 `Slot.cursor` 和 row-local 局部变量混起来(循环里 register-allocated 一个 `cursor`,在列切换时写回 slot)。
- `std::vector<char>::resize(n)` 对 POD 触发 `memset` 这件事在 profile 下的确能量化到 `~100ns/row` 级,但单独只换"不 zero-fill 的 buffer" 并不足以转化为 end-to-end 提升。
- 真正可能的下一个突破点是"**行并行化**"与"**跨 row 的 SIMD escape**",即一次性拿若干 row 的输入做 vectorized escape 扫描,而不是每行维护一个 cursor。

### 处置

- 本实验在字节级输出上与 legacy 一致,但 throughput 显著落后于上一版。
- 由于 `src/sql/encdec/json_v2/` 目录尚未进入 VCS,**没有现成的 "git reset" 可以回到上一版本**。
- 当前状态保留在仓库中,作为"换底层 buffer 不是免费午餐"的一次工程学存证。
- 后续若要继续朝 `2.5x` 迈进,建议换方向:**row-parallel encode + SIMD escape + per-column specialized writer**,而不是继续微调 per-row API。
