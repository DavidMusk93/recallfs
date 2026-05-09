# JSON Encoder V2 全量优化手段总复盘

## 目标

这篇文档把 `json_v2` 这一轮优化过程中**所有做过的手段**集中整理出来,包括:

- 最后保留的优化
- 已验证但回退的负优化
- 只作为观测手段保留的 profiling/bench 工具
- 每一步的判断依据

本文使用中文描述,并用 ASCII graph 表达优化推进流程。

相关代码:

- [jit_runtime.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h)
- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)
- [adapter.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.h)
- [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)
- [json_encoder_benchmark.cpp](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp)
- [json_encoder_bench_stability.sh](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh)

## 一图看全程

```text
起点
  |
  v
第一版 JIT 负优化
  |
  +--> 修正 execution shape
  |      |
  |      +--> column-major 调度
  |      +--> plan/kernel/accessor cache
  |      +--> raw buffer access
  |      |
  |      `--> 仍然不够快
  |
  +--> 表级 escaped cache
  |      |
  |      `--> speedup ~ 2.0x
  |
  +--> 定位 finish() 瓶颈
  |      |
  |      +--> 细粒度 finish profile
  |      +--> memcpy/prefetch/small-copy 多实验
  |      +--> 发现 copy_values 是主热点
  |      |
  |      `--> 继续追 layout/dataflow
  |
  +--> 收紧输出布局
  |      |
  |      +--> row_reserve 数据驱动估算
  |      +--> overflow_rows 保持为 0
  |      +--> inplace_pack 替换新 values buffer + gather-pack
  |      |
  |      `--> speedup ~ 2.15x ~ 2.20x
  |
  +--> 转攻 kernel_ms
  |      |
  |      +--> string/binary fused prefix 试验
  |      |      `--> 负优化,回退
  |      |
  |      +--> float/double formatted cache
  |      |      `--> 显著正收益,突破 2.5x
  |      |
  |      +--> 非字符串列 prefix + value 融合
  |      |      `--> key_ms 明显下降,逼近/突破 3x
  |      |
  |      `--> rawFastPath string 定位 + 微优化
  |             |
  |             +--> 单次 reserve 占比统计
  |             +--> 长度分桶
  |             +--> <8B / 8-15B payload 小拷贝 fast path
  |             `--> 多个更激进 copy 版本全部回退
  |
  `--> 收尾
         |
         +--> 稳定性脚本
         +--> 7 轮 both 20s 看中位数
         `--> 当前基线中位数稳定 >= 3x
```

## 当前结论

截至本轮收尾,最终应该保留的核心优化是:

- column-major JIT 调度修正
- plan/kernel/accessor cache
- 表级 escaped cache
- finish 细粒度 profile
- row reserve 数据驱动估算
- `overflow_rows == 0` 前提下的 `inplace_pack`
- `float/double` formatted cache
- 非字符串列 `prefix + value` 融合
- `rawFastPath string` 单次 reserve 路径
- `writePrefixedQuotedRawString()` 对 `<8B` 和 `8-15B` payload 的小拷贝 fast path
- 稳定性验收脚本

当前不应保留的方向是:

- 针对字段前缀的更激进常量专门化
- `string/binary` fused prefix 大改
- `8-15B` 的重叠双 8B copy
- `8-15B` 的纯标量逐字节 copy
- `8-15B` 的 `uint64_t` unaligned load/store 版本
- 单纯围绕 `memcpy` 做越来越复杂的 prefetch/block 变体

## 阶段 0: 第一版为什么会慢

这部分已经有单独文档:

- [json_encoder_v2_negative_optimization_case.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md)

这里只保留一句总结:

- 第一版的主要问题不是 LLVM 本身,而是 **execution shape 不对 + hot path 还在 host runtime 里**

ASCII 图:

```text
想要的形态:
  generated code -> 真正的列式热路径

第一版实际形态:
  generated code -> RuntimeVTable -> runtime wrapper -> formatter
```

### 这一阶段做过并保留的修正

1. 把 kernel 从 row-major 改回 column-major 调度
2. 缓存 `plan/kernel/accessor`
3. 列访问尽量走 raw buffer

### 结果

- 这些动作是必要修正
- 但它们本身还不够把 `json_v2` 推到目标性能

## 阶段 1: 表级 escaped cache

### 背景

当 benchmark 重复编码同一张 `arrow::Table` 时,许多字符串列每轮都在重复做完全一样的 JSON escape。

### 做法

- 给 `StringColumnAccessor` / `BinaryColumnAccessor` 加 `escapedCache()`
- 首次遇到 escape 路径时生成整列表级 cache
- 后续 steady-state 直接 `appendRaw()`

相关代码:

- [adapter.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.h)
- [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)
- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)

### 结果

- `json_v2` 首次稳定逼近并打到 `~2.0x`
- 说明这条路真正利用了 `schema + table` 的确定性

### 结论

- 这是一次**保留**的优化
- 它证明 steady-state 下,table identity 是很重要的 specialization 维度

## 阶段 2: finish() 诊断与 dataflow 重构

这部分已有单文档:

- [json_encoder_v2_finish_layout_optimization.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md)

这里补充成“总账视角”。

### 2.1 先做观测,不是先改 copy

增加的观测字段包括:

- `finishAllocOffsetsNs`
- `finishBuildOffsetsNs`
- `finishAllocValuesNs`
- `finishCopyValuesNs`
- `finishRows`
- `finishTotalBytes`
- `finishMaxRowBytes`
- `arena_rows`
- `overflow_rows`

结论:

- `build_offsets_ms` 不是主问题
- `copy_values_ms` 才是主问题
- benchmark 下 `overflow_rows=0`

### 2.2 做过但没有保留的 copy 微优化

做过的实验包括:

- `memcpy`
- `memcpy_prefetch`
- `memcpy_prefetch_meta`
- `memcpy_prefetch_block`
- `inline_small`

结果判断:

- `memcpy_prefetch` 偶尔略好,但不够稳定到值得单独作为最终突破
- `inline_small` 明显负优化
- `memcpy_prefetch_block` 短跑偶尔好看,20s 不稳定
- 这些都没有改变数据流,只是改变 copy 指令形态

ASCII 图:

```text
失败原因:

  sparse arena
      |
      v
  仍然要 gather-pack 到新 values buffer
      |
      v
  只是换了一种 copy loop

=> dataflow 没变,收益上限很低
```

### 2.3 真正保留的 finish 级优化

#### 优化 A: row reserve 数据驱动估算

做法:

- 用数值列格式化采样
- 用 `total_values_length()` 估算字符串/二进制列平均长度
- 把 `null_count` 也纳入估算
- 让 `row_reserve_bytes` 从固定值收紧到更贴近实际

结果:

- 减少 arena 稀疏度
- 提高 `overflow_rows=0` 的概率

#### 优化 B: `inplace_pack`

做法:

- 当 `overflow_rows == 0` 时
- 不再 `AllocateBuffer(totalBytes)` 新建 values buffer
- 直接在原 arena 中 `memmove` 压紧
- 用 `OwnedArenaBuffer` 把 arena 直接交给 Arrow

结果:

- `finish_ms` 从 `~1529ms` 降到 `~410ms`
- `copy_values_ms` 从 `~1503ms` 降到 `~380ms`
- 端到端 speedup 提升到 `~2.15x ~ 2.20x`

### 结论

- `finish()` 这轮真正的突破不是 copy 指令级技巧
- 而是 **layout + dataflow 重构**

## 阶段 3: kernel_ms 再突破

当 `finish()` 被压下去之后,主矛盾转移到:

- `key_ms`
- `string_ms`
- `double_ms`

ASCII 图:

```text
阶段切换:

before:
  finish_ms >>> kernel_ms

after inplace_pack:
  kernel_ms > finish_ms

=> 继续抠 finish 不再是主线
=> 必须转攻 kernel 内部热点
```

## 阶段 4: string/binary fused path 的失败实验

### 做法

- 尝试把字段前缀和 `string/binary` 写值合并
- 让 `keyAll + writeStringLikeColumn` 变成更 fused 的写法

### 发生的问题

- 中间还踩到过一次 `%RuntimeVTable` ABI 槽位不一致
- 修好 ABI 后,bench 仍然是负优化

### 结论

- **回退**
- 说明 `string/binary` 这里不是“融合越多越快”
- 尤其 escape/quoted 路径复杂,容易把热点从 `key_ms` 搬到 `string_ms`

## 阶段 5: float/double formatted cache

### 做法

- 新增 `FormattedValueCache`
- `FloatColumnAccessor` / `DoubleColumnAccessor` 懒加载缓存格式化结果
- steady-state 下 `runtimeWriteFloatColumnImpl()` / `runtimeWriteDoubleColumnImpl()` 直接 `appendRaw(cache.view(row))`

相关代码:

- [adapter.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.h)
- [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)
- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)

### 结果

- `double_format_ms` 基本从 steady-state 热路径里消失
- `both 20s` 连跑曾达到:
  - `2.64804x`
  - `2.93354x`

### 结论

- **保留**
- 这是冲破 `2.5x` 的关键一步

## 阶段 6: 非字符串列 prefix + value 融合

### 做法

- 给 `BatchFormatter` 增加按 row 直接写 raw bytes 的接口
- 对 `bool/int/uint/float/double/time/timestamp` 这些非字符串列
- 把“写字段前缀 + 写值/null”改成单趟循环

### 结果

- `key_ms` 大幅下降
- `json_v2-only` 提升约 `11%`
- `both 20s` 达到:
  - `2.95906x`
  - `2.96206x`

### 结论

- **保留**
- 这是把 `key_ms` 从显性热点里压下去的重要动作

## 阶段 7: rawFastPath string 的观测与微优化

这一段是最典型的“先观测,再小步实验,按 bench 裁决”。

### 7.1 先加 profile 计数

增加的统计:

- `raw_single_reserve_calls`
- `raw_single_reserve_rows`
- `raw_single_reserve_bytes`
- 命中占比

结论:

- 这条单次 reserve 路径覆盖了约 `75%` 的 `string` 调用
- 不是边角路径,而是主路径

### 7.2 再做长度分桶

分桶结果:

- `<8B`
- `8-15B`
- `16-31B`
- `32-63B`
- `64B+`

实际数据特征:

- 全部命中都落在 `<16B`
- 主力集中在 `8-15B`
- `16B+` 基本为 0

结论:

- 这条 fast path 本质上是**超短字符串路径**

ASCII 图:

```text
rawFastPath string
  |
  +--> 75% 调用命中单次 reserve
  |
  `--> 命中 payload 长度:
         <8B    : 约 37%
         8-15B  : 约 63%
         >=16B  : 约 0%
```

### 7.3 保留的微优化

#### 优化: `<8B` 和 `8-15B` payload 小拷贝 fast path

做法:

- 在 `writePrefixedQuotedRawString()` 中
- 保留单次 `reserveScratch()`
- 对 payload:
  - `<8B` 走 `copyLt8`
  - `8-15B` 走 `copy8To15`
  - 其余走 `memcpy`

结果:

- `both 20s` 达到:
  - `3.12571x`
  - `3.17582x`

结论:

- **保留**
- 这是对 workload 高度对症的一次小优化

### 7.4 做过但回退的 string 路径实验

#### 实验 A: rawFastPath string fused 版本

做法:

- 单趟循环同时写字段前缀和 raw string
- 避开 escape cache 路径

结果:

- `key_ms` 几乎被吃掉
- 但 `string_ms` 反而抬升
- 长跑波动大,不稳定

结论:

- **回退**

#### 实验 B: `writeQuotedJsonString()` no-escape reserve 优化

做法:

- 先扫描是否真的需要 escape
- 如果不需要,就只按 `len + 2` reserve
- 不再无脑按 `2 + len * 6`

结果:

- `json_v2-only` 看起来更快
- 但 `both 20s` 波动大,一轮强一轮弱

结论:

- 不把它作为“已经验证稳定收益”的成果
- 价值主要在于说明 reserve 的精细化值得观察,但不是当前最终结论

## 阶段 8: 针对字段前缀与 8-15B copy 的失败实验

这一段实验集中证明了一件事:

- **不是所有“更激进”“更底层”的 copy 形态都会赢**

### 8.1 字段前缀常量专门化

做法:

- 针对 `literalSize == 10` 等常见长度
- 做 `8+2`、`8+4` 等固定拆分

结果:

- `json_v2-only` 变差
- `both 20s` 也回落

结论:

- **回退**

### 8.2 `8-15B` 重叠双 8B copy

做法:

- 前 8B `memcpy`
- 后 8B 再 `memcpy`
- 用重叠覆盖掉中间区域

结果:

- bench 不如 `8B + tail(<8B)`

结论:

- **回退**

### 8.3 `8-15B` 纯标量逐字节 copy

做法:

- `switch(8..15)` 逐字节赋值
- 完全不走 `memcpy`

结果:

- 指令体积膨胀
- `json_v2-only` 与 `both 20s` 都不占优

结论:

- **回退**

### 8.4 `8-15B` 的 `uint64_t` unaligned load/store

做法:

- 通过 `__builtin_memcpy` 做定义良好的 unaligned `u64/u32/u16` load/store
- 目标是少于逐字节,同时绕开通用 `memcpy`

结果:

- `json_v2-only` 明显掉速
- `both 20s` 的 speedup 摇摆很大,主要是 legacy 波动,不能说明它更好

结论:

- **回退**

### 这一组实验的统一结论

ASCII 图:

```text
针对 8-15B:

  更激进 copy 形态
      |
      +--> 重叠双8B          -> 回退
      +--> 纯标量逐字节      -> 回退
      +--> u64/u32/u16 版    -> 回退
      |
      `--> 8B memcpy + tail  -> 当前最优保留
```

这组结果说明:

- `memcpy(constant)` 往往已经被编译器优化得很好
- 手写“更底层”版本很容易增加指令体积、寄存器压力和分支成本

## 阶段 9: 稳定性收尾

当端到端已经逼近或越过 `3x` 后,继续冒险改热路径的收益开始可疑。

因此最后做的不是再继续硬改 encoder,而是把**稳定性验收标准**固化下来。

### 做法

新增脚本:

- [json_encoder_bench_stability.sh](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh)

脚本能力:

- 自动补 `LD_LIBRARY_PATH`
- 默认 `repeat=7`
- 默认跑 `both`
- 提取每轮 `legacy rows_per_sec`
- 提取每轮 `json_v2 rows_per_sec`
- 提取每轮 `json_v2_speedup`
- 统计 `median/min`
- 当 `median < 3.0` 时直接失败

### 结果

7 轮 `both 20s`:

- 未绑核:
  - `median=3.14091x`
  - `min=2.87602x`
- `taskset=0-15`:
  - `median=3.05485x`
  - `min=2.94457x`

### 结论

- 如果“稳定 3x”的定义是**中位数 >= 3x**,当前已经达标
- 如果定义升级成“最差一轮也必须 >= 3x”,那主要矛盾更像是**环境噪声**,不再是 copy 技巧

## 最终保留清单

```text
[保留]
  |
  +-- column-major JIT 调度修正
  +-- plan/kernel/accessor cache
  +-- 表级 escaped cache
  +-- finish 细粒度 profiling
  +-- row reserve 数据驱动估算
  +-- inplace_pack
  +-- float/double formatted cache
  +-- 非字符串列 prefix + value 融合
  +-- rawFastPath string 单次 reserve 路径
  +-- rawFastPath string <8B / 8-15B payload 小拷贝
  `-- 稳定性验收脚本
```

## 最终回退清单

```text
[回退]
  |
  +-- 单纯 copy loop 微调:
  |     +-- inline_small
  |     +-- memcpy_prefetch_block
  |     +-- memcpy_prefetch_meta
  |     `-- 其他只改 copy 但不改 dataflow 的分支
  |
  +-- string/binary fused prefix 大改
  +-- 字段前缀常量专门化
  +-- 8-15B 重叠双8B copy
  +-- 8-15B 纯标量逐字节 copy
  `-- 8-15B u64 unaligned load/store
```

## 方法论总结

这轮优化最后沉淀出来的优先级顺序是:

1. 先修 execution shape
2. 再修 memory layout
3. 再减少 data movement stage
4. 最后才是指令级 micro-opt

换成更直白的话:

- 先问“这一步还该不该存在”
- 再问“这一步能不能写得更快”

ASCII 图:

```text
优化优先级:

  execution shape
        |
        v
  memory layout
        |
        v
  dataflow / stage elimination
        |
        v
  instruction-level tuning
```

如果未来还要继续往上冲,优先关注的仍然应该是:

- `string_ms`
- 剩余 `key_ms`
- 更高层的 batching / 并行化 / emitter 边界

而不是再次陷入“越来越复杂的 copy 形态”。

