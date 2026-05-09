# JSON Encoder V2 基于 L1/L2 Working Set 的优化草案

本文只讨论一件事：`json_v2` 如果继续从 cache 利用率出发做性能优化，下一轮最值得投入的方向是什么。

结论先写在前面：

- 不建议继续把主要精力放在 `memcpy` / prefetch / 小拷贝指令形态上
- 更值得做的是 **working set 缩小**，也就是：
  - 引入 `row tile`
  - 压缩 `Slot` 热元数据
  - 让 arena fast path 的状态尽量停留在 L1/L2
- 当前已经落地并保留的两项优化是：
  - `row tile`，通过环境变量 `TIDE_JSON_V2_TILE_ROWS` 控制
  - `raw + null-free` string 路径的 `4-lane unroll`
- 默认 `tileRows` 不应拍脑袋，至少需要对齐目标机器的 `L1D/L2`
  - 通用保守默认（按 `L2=256KB` 估算）：`tileRows=384`
  - 当前机器实测（`L2=2MB`）：`tileRows=1024`
- 如果要给下一轮结构改造定主方向，建议目标是 **arena row 走 4B/row 的 `RowState`，overflow 单独 side table**

相关代码：

- `json_v2` 主运行时： [jit_runtime.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h) 、 [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)
- accessor / cache： [adapter.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.h) 、 [adapter.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/adapter.cpp)
- benchmark： [json_encoder_benchmark.cpp](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp)
- 现有优化复盘： [json_encoder_v2_optimization_journal.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md)
- `finish()` / layout 改造记录： [json_encoder_v2_finish_layout_optimization.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md)
- 负优化案例与 `slot` cache 压力讨论： [json_encoder_v2_negative_optimization_case.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md)

---

## 1. 当前状态回顾

这轮 `json_v2` 已经吃掉了几类最值钱的结构性收益：

- kernel 从错误的 `row-major` 调度修回 `column-major`
- 加入 `plan/kernel/accessor` cache
- 字符串 escape 走表级 cache
- `float/double` 走格式化 cache
- `finish()` 从 “新 values buffer + gather copy” 改成 `inplace_pack`

这些动作都不是“写更花的指令”，而是 **修执行形态和数据流**。

当前文档里已经明确说明这几点是保留优化，见 [json_encoder_v2_optimization_journal.md:L84-L107](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md#L84-L107)。

当前 kernel 的大体形态是：

```text
rowStartAll(rows)
for each field:
  write<Kind>Column(formatter, accessor, rows, ...)
rowEndAll(rows)
finish()
```

这个执行形态见 [llvm_development_tutorial_json_v2.md:L258-L265](file:///root/Documents/stream_engine/docs/jit/llvm_development_tutorial_json_v2.md#L258-L265)。

也就是说，`json_v2` 当前已经不是“完全没顾 cache”的状态。下一轮要挖的，不是第一性原理层面的方向错误，而是：

- 现有列式热路径里，哪些热元数据仍然太大
- 哪些工作集本可以留在 L1/L2，却被整批 `rows` 冲散
- arena fast path 是否还能进一步收紧为更小的热状态

### 1.1 当前已经落地的 cache / working-set 优化

这份文档最初是“优化草案”，但到当前阶段，已经有两项优化从 proposal 进入了保留实现：

1. `row tile`
2. `raw + null-free` string 快路径的 `4-lane unroll`

`row tile` 的代码入口是 [executionTileRows](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L337-L348) 和 [CompiledKernel::execute](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L2438-L2451)：

- 通过环境变量 `TIDE_JSON_V2_TILE_ROWS` 读取 tile 大小
- 未配置、配置为空、配置非法或 `<= 0` 时，退回整批 `rows`
- 配置有效时，按 `tileStart += tileRows` 分块执行
- `formatter.setRowBase(tileStart)` 用来保证 tile 模式下 row 映射正确，相关字段在 [BatchFormatter](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h#L108-L150)

`4-lane unroll` 的实现位于 [runtimeWriteStringColumn](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1433-L1524) 的 `arrayValue->null_count() == 0` 分支：

- 只针对 `raw` 或 `rawFastPath` 生效
- 只针对 `null-free` 子路径生效
- 以 `4 rows` 为一组展开，尾部保留原来的标量 loop
- 目的不是改 JSON 语义，而是减少小字符串 raw 写路径里的 loop/control 开销

这轮探索里其他几条线已经验证过但没有保留：

- `raw + nullable` 的 4-row valid-block fast path：无稳定收益，已回滚
- 直接绕开 `std::string_view` 临时对象：无稳定收益，已回滚
- 更激进的 `8-lane` 或手写完全展开：无稳定收益，已回滚

也就是说，当前文档里的“推荐方向”需要区分两类：

- 已落地并验证保留的：`tile`、`4-lane raw fast path`
- 仍处于后续方向储备的：`RowState`、`micro-tile`、新的 escape / SIMD 路线

---

## 2. 现在最像 cache 问题的点

现有文档里最重要的一条线索其实已经出现了。

在 [json_encoder_v2_negative_optimization_case.md:L678-L692](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md#L678-L692) 中，已经明确指出：

- `RowPagedBuffer::slots_` 是 `std::vector<Slot>`
- 每个 `Slot` 现在是 `begin/cursor/cap` 三指针，共 `24B`
- `appendChar()` / `reserveScratch()` / `advanceCursor()` 都会反复访问 `slots_[row]`
- 在 `column-major` 编码下，每写一列都要重新扫一遍 `0..rows`

当前 `Slot` 定义见 [jit_runtime.h:L21-L106](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h#L21-L106)。

这意味着两件事：

1. 问题不只是输出 value 的 copy
2. 问题还包括 **per-row 热元数据本身的 cache footprint**

以 benchmark 常见负载 `rows=65536` 为例，如果每行一个 `24B` slot，那么只算 `slots_` 就已经接近：

```text
65536 * 24B ~= 1.5 MB
```

这显然不可能停留在 L1，甚至会不断冲刷很多 CPU 的 L2。

而当前默认 benchmark 行数正是 `65536`，见 [json_encoder_benchmark.cpp:L470-L472](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp#L470-L472)。

---

## 3. 本草案的目标

本草案只讨论下面三个问题：

1. `row tile` 应该怎么定范围
2. `Slot` 应该怎么改布局
3. 实验顺序应该怎么排，才能尽快知道“值不值得继续投”

本草案**不**讨论：

- 新增 JSON 输出语义
- schema / options 对外接口变化
- escape 算法语义变更
- 跨线程并行执行模型

---

## 4. 一个简化的 L1/L2 Working Set 模型

为了给 tile 提建议，先建立一个够用的估算模型。

### 4.1 通用保守估算

在没有目标机器信息时，可以用典型 x86 的保守量级做粗估：

- `L1D = 32KB`
- `L2 = 256KB`
- `cache line = 64B`

### 4.2 当前机器实测

实际建议用 sysfs 读出来（避免误判 L2 大小）：

```bash
for i in /sys/devices/system/cpu/cpu0/cache/index*; do
  echo "$i level=$(cat $i/level) type=$(cat $i/type) size=$(cat $i/size)"
done
```

本机输出为：

```text
/sys/devices/system/cpu/cpu0/cache/index0 level=1 type=Data size=48K
/sys/devices/system/cpu/cpu0/cache/index1 level=1 type=Instruction size=32K
/sys/devices/system/cpu/cpu0/cache/index2 level=2 type=Unified size=2048K
/sys/devices/system/cpu/cpu0/cache/index3 level=3 type=Unified size=99840K
```

因此本机用于 tile 推导的关键参数是：

- `L1D = 48KB`
- `L2 = 2048KB`

当前实现里，对 tile 影响最大的热工作集主要有：

- arena: `tileRows * rowReserveBytes`
- slot 元数据: `tileRows * slotBytes`
- `finish()` offsets: `(tileRows + 1) * 4B`
- 某些阶段的 row 级临时元数据：大约 `4B ~ 16B / row`

其中：

- 当前 `rowReserveBytes` 在 benchmark 里已从约 `512` 收紧到约 `448`，见 [json_encoder_v2_finish_layout_optimization.md:L67-L93](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md#L67-L93)
- 当前 `slotBytes = 24`

所以可以先粗算：

```text
working_set(tile) ~= tileRows * 448B   // arena
                    + tileRows * 24B    // slots
                    + tileRows * 8~16B  // offsets / row metadata / 余量
```

换句话说，当前每行的“近似热 footprint”可以先按 `480B ~ 488B` 这个量级估。

---

## 5. Tile 范围建议

### 5.1 按当前布局估算

先用：

- `rowReserveBytes = 448`
- `slotBytes = 24`
- 额外元数据余量按 `12B/row`

粗算：

| `tileRows` | arena | slots | 其他元数据 | 总量级 |
|---|---:|---:|---:|---:|
| `256` | `112KB` | `6KB` | `3KB` | `121KB` |
| `384` | `168KB` | `9KB` | `4.5KB` | `181.5KB` |
| `512` | `224KB` | `12KB` | `6KB` | `242KB` |
| `768` | `336KB` | `18KB` | `9KB` | `363KB` |
| `1024` | `448KB` | `24KB` | `12KB` | `484KB` |

这个表不表示“所有数据都必须同时驻留在 L2”，它表达的是：

- tile 越大，arena 和 slot 元数据越容易跨出 L2 舒适区
- tile 越小，cache 更友好，但调度和 `finish()` 的固定成本更难摊薄

### 5.2 推荐区间

基于上面的估算，建议把推荐拆成两档（不要混用）：

#### 档 A：通用保守（按 `L2=256KB` 估算）

- 首轮 benchmark 区间：`256 / 384 / 512`
- 默认实验值：`384`
- 在确认目标 CPU 单核 L2 至少 `512KB` 之前，不建议把 `768` 以上作为默认值

理由：

1. `256` 较保守，几乎肯定对 `256KB L2` 友好
2. `384` 仍有余量，比 `256` 更能摊薄固定成本
3. `512` 已经非常贴近 `256KB L2` 的舒适边界，值得测，但不适合作为“盲选默认”

#### 档 B：当前机器（`L2=2MB`）

在本机 `L2=2048KB` 下，tile 可以显著放大，建议的首轮扫描改为：

- 首轮 benchmark 区间：`512 / 1024 / 1536 / 2048`
- 默认实验值：`1024`

理由：

1. 从 `working set ~= tileRows * ~480B` 的量级估算看，`tileRows=2048` 也仍在 `~1MB` 级别，留给其他工作集仍有余量
2. `1024` 通常更容易在“cache 友好”和“摊薄固定成本”之间取得平衡

### 5.3 当前实现里的 tile 配置方式

除了“推荐值”，还需要把当前代码里的真实配置方式写清楚。

当前实现不是编译期常量，而是运行时环境变量：

```bash
TIDE_JSON_V2_TILE_ROWS=1024
```

读取逻辑见 [jit_runtime.cpp:L337-L348](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L337-L348)，执行入口见 [jit_runtime.cpp:L2438-L2451](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L2438-L2451)。

当前语义是：

- 不设置 / 为空：默认启用 tiling，`tileRows=1024`
- 设置为正整数：按该值 tiling（单次 kernel 执行最多处理该数量的 rows）
- 设置为 `>= rows`：等价于“不分 tile”（因为最终会 `min(rows, tileRows)`）
- 非法值 / `<= 0`：静默退回默认 `tileRows=1024`

因此在当前机器上，文档里提到的“默认实验值 `1024`”不是抽象建议，而是已经可以直接这样跑：

```bash
TIDE_JSON_V2_TILE_ROWS=1024 ./build64_release/src/test/json_encoder_benchmark
```

### 5.4 如果 Slot 压缩成功

如果 arena fast path 最终能把每行热元数据压到 `4B` 量级，那么同样粗算下：

| `tileRows` | arena | fast-path metadata | 其他元数据 | 总量级 |
|---|---:|---:|---:|---:|
| `384` | `168KB` | `1.5KB` | `4.5KB` | `174KB` |
| `512` | `224KB` | `2KB` | `6KB` | `232KB` |
| `768` | `336KB` | `3KB` | `9KB` | `348KB` |

这时：

- `512` 会明显更像一个稳定默认值
- `768` 才开始成为 “看机器 L2 大小再决定” 的区间

所以 `tileRows` 的最终默认值，其实和 `Slot` 是否压缩成功是绑定的。

---

## 6. 推荐的分块执行形态

建议的方向不是彻底推翻当前列式调度，而是把“整批 `rows`”切成若干 `row tile`：

```text
for each row_tile:
  rowStartAll(tileRows)
  for each field:
    write<Kind>Column(formatter, accessor, tileRows, ...)
  rowEndAll(tileRows)
  finish(tileRows)
```

这样做的收益主要有三类：

- `slots` / row state 的反复扫描范围变小
- `finish()` 的 offsets/build/copy 工作集变小
- null bitmap、offsets、value range 等列级读写更容易在 tile 内保持局部性

### 6.1 建议保留一个更小的 micro-tile 选项

如果后面发现：

- `row tile` 已经让 L2 压力明显下降
- 但 `reserveScratch()` / `advanceCursor()` 仍然有显著的 L1 压力

那么可以在列 writer 内部再加一个更小的 `micro-tile`：

- 候选值：`128` 或 `256`

此时执行形态可变成：

```text
for each row_tile:
  rowStartAll(tileRows)
  for each field:
    for each micro_tile in row_tile:
      write field for micro_tile
  rowEndAll(tileRows)
  finish(tileRows)
```

这个 `micro-tile` 的目的不是再分配 arena，而是：

- 让 row-local cursor / row state / 某些 null 路径判断更容易留在 L1

建议把它放在第二阶段再考虑，不要和第一轮 `row tile` 改造一起上。

---

## 7. Slot 布局方案

下面把可选方案拆成三档，按“侵入性从低到高、收益从弱到强”排序。

### 7.1 方案 A：tile-local cursor staging

这是最保守的方案。

做法：

- 不改 `RowPagedBuffer` 的公开语义
- 在处理一个 tile 的某一列时，把这段 tile 的 `cursor` 提前拉到紧凑局部数组
- 列内循环只更新这段局部 cursor
- 列结束后再批量写回

优点：

- 改动面最小
- 适合快速判断 “slot 读写本身是不是主要瓶颈”

缺点：

- 只是减少一部分 pointer chasing
- 并没有解决 `Slot` 结构体本身偏大的问题

建议：

- 可作为低风险前置实验
- 不建议把它当最终方案

### 7.2 方案 B：16B 紧凑 Slot

一个比较自然的中间态是把三指针 AoS 改成 16B 的 offset 化结构，例如：

```cpp
struct CompactSlot {
    uint32_t beginOffset;
    uint32_t cursorOffset;
    uint32_t capOrSize;
    uint32_t flagsOrOverflowIndex;
};
```

优点：

- 不改变“每行一个 slot”的思维模型
- 把元数据从 `24B/row` 压到 `16B/row`
- 指针换 offset 后，更方便做 tile-local arena

缺点：

- 对 arena row 而言，`begin` / `cap` 其实仍然包含冗余
- 仍然保留了“每列反复全扫 slot”的基本访问形态

建议：

- 如果担心一次把结构改太大，可以把它作为过渡版本

### 7.3 方案 C：arena fast path 的 `RowState` + overflow side table

这是本草案最推荐的目标方案。

关键观察：

1. 当前 `finish()` 的快路径契约本来就依赖 `overflow_rows == 0`，见 [json_encoder_v2_finish_layout_optimization.md:L95-L159](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_finish_layout_optimization.md#L95-L159)
2. 当前 `rowReserveBytes` 最终还会被限制在 `<= 1024`，见 [jit_runtime.cpp:L527-L530](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L527-L530)
3. 对 arena row 而言，真正需要的热状态只有：
   - 这行当前写了多少字节
   - 是否溢出到 side buffer

所以 arena fast path 理论上可以改成：

```cpp
struct RowState {
    uint16_t usedBytes;
    uint16_t overflowIndex; // 0xFFFF 表示仍在 arena
};
```

arena row 的地址推导变成：

```text
rowBase = arenaBase + row * bytesPerRow
cursor  = rowBase + usedBytes[row]
cap     = rowBase + bytesPerRow
```

而只有 overflow row 才进入 side table：

```cpp
struct OverflowRow {
    char* begin;
    char* cursor;
    char* cap;
};
```

优点：

- arena fast path 的热元数据从 `24B/row` 降到 `4B/row`
- `finish()` 构 offsets 时可以直接读 `usedBytes`
- `begin` / `cap` 对 arena row 不再进入热路径数据结构

缺点：

- `growSlot()` / overflow 管理逻辑会更复杂
- 需要明确 fast path / overflow path 的边界

建议：

- 这是最值得作为最终目标的布局

---

## 8. 为什么我更推荐方案 C

因为当前 `json_v2` 的真正高频情况其实很明确：

- arena row 是主路径
- overflow row 是少数
- `inplace_pack` 的收益也建立在 “大多数甚至全部 row 都留在 arena” 这个事实上

如果主路径已经是：

- 行地址可由 `row index + bytesPerRow` 直接推导
- 行 cap 对所有 arena row 固定相同

那继续为每行维护 `begin/cursor/cap` 三个字段，其实是在为少数 overflow 情况让整个快路径付 metadata 成本。

从 cache 角度说，这不是一个理想的 trade-off。

更自然的设计应该是：

- arena fast path 只保留最小热状态
- overflow 变成 side structure

也就是：

```text
common case pays for common case
rare case pays for rare case
```

---

## 9. 建议的实验顺序

为了尽快回答“值不值得继续投”，建议不要一上来把 tile 和 Slot 一起大改。

### 9.1 Phase 1：只加 row tile

目标：

- 先验证 working set 收缩本身能不能稳定带来收益

建议：

- 新增环境变量，例如 `TIDE_JSON_V2_TILE_ROWS`
- benchmark 扫描区间建议按目标机器分两档：
  - 通用保守：`256 / 384 / 512 / 768`
  - 当前机器（`L2=2MB`）：`512 / 1024 / 1536 / 2048`
- 其他逻辑尽量不动

验收重点：

- `json_v2_speedup`
- `kernel_ms`
- `finish_ms`
- `other_ms`
- `overflow_rows`

### 9.2 Phase 2：把 arena row 改成 `RowState`

目标：

- 验证 slot 元数据缩小后，`reserveScratch + advanceCursor + slot access` 的成本是否明显下降

建议：

- 先不引入 `micro-tile`
- 先保证对外语义和当前输出完全一致

验收重点：

- `other_ms` 是否显著下降
- `finish_ms` 是否不反弹
- `overflow_rows` 是否仍接近 `0`

### 9.3 Phase 3：再决定是否引入 micro-tile

目标：

- 如果 `other_ms` 仍然偏高，再压 L1 working set

建议：

- 候选值只测 `128` / `256`
- 不要把这个阶段和 `RowState` 首次上线绑在一起

### 9.4 Phase 4：先做 raw fast path 小步实验，再决定是否碰 escape / SIMD

这一阶段到当前已经有一项保留收益：

- `raw + null-free` string 路径 `4-lane unroll`

原因不是“盲目微优化”，而是 benchmark 数据分布本身给了很强的提示。稳定性脚本 [json_encoder_bench_stability.sh](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh) 在当前数据集上打印的 profile 显示：

- `raw_fast_path_eligible_fields=3/5`
- `total_escape_free_pct=60.0942`
- `name / city / region` 三列都满足 `escape_free_pct=100`
- 这几列平均长度约 `7.75 ~ 7.89`，是非常典型的小字符串 raw 路径

在这种分布下，继续先看 `raw + null-free` 的 execution shape 比直接改 escape 算法更合理。

理由：

- 现有文档已说明，单纯手写 escape 不一定比 `simdjson` 快，见 [json_encoder_v2_negative_optimization_case.md:L684-L685](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md#L684-L685)
- 如果 tile + RowState 已经把 `other_ms` 大幅压下去，那么 `string_ms` 是否重新成为主热点，要用新 profile 再决定
- 当前已经验证过多条无收益路线并回滚，包括 nullable fast path、去掉 `string_view` 临时对象、`8-lane` 展开和手写完全展开

### 9.5 实验思路的出处

这轮实验不是凭空拍脑袋，主要有三条出处：

1. working set / tiling 视角来自用户最初讨论的文章：[Faster Timeseries Aggregations](https://www.opendata.dev/blog/faster-timeseries-aggregations)
2. 小拷贝 hot path 的实现参考来自代码内注释提到的 StarRocks 讨论：[StarRocks PR 13330](https://github.com/StarRocks/starrocks/pull/13330)
3. 具体该优先打哪条路径，则来自项目内 benchmark 数据分布和 profile，本轮使用的是 [json_encoder_bench_stability.sh](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh) 输出的数据集画像

因此这轮保留优化的逻辑链是：

```text
OpenData 的 tiling / working-set 视角
  -> 先落地 row tile
  -> 用 bench dataset profile 判断 raw fast path 命中面
  -> 在 null-free raw 小字符串路径上做最小 unroll
  -> 不稳定或无收益的实验立即回滚
```

---

## 10. 需要重点观察的 profile 指标

下一轮优化时，建议重点看这些指标：

- `kernel_ms`
- `finish_ms`
- `copy_values_ms`
- `string_ms`
- `double_ms`
- `other_ms`
- `overflow_rows`
- `rowReserveBytes`

判断逻辑建议是：

- `other_ms` 下降，说明 row-local 热元数据确实是问题
- `finish_ms` 不反弹，说明 tile 没把收尾成本放大过头
- `overflow_rows` 仍接近 `0`，说明当前 reserve 估算和 fast path 契约仍成立
- `string_ms` 占比上升，则说明 metadata 问题被削弱后，下一阶段才有必要看 SIMD escape

---

## 11. 推荐的默认选择

如果要给下一轮开发一个简单、明确、工程上可执行的起点，建议把默认值也按机器分档：

#### 档 A：通用保守（按 `L2=256KB` 估算）

- 默认 `tileRows = 384`
- benchmark 主测区间：`256 / 384 / 512`

#### 档 B：当前机器（`L2=2MB`）

- 默认 `tileRows = 1024`
- benchmark 主测区间：`512 / 1024 / 1536 / 2048`

两档共同的结构目标与顺序不变：

- 结构目标：`RowState(4B/row) + overflow side table`
- 实验顺序：先 tile，后 Slot，再决定要不要 `micro-tile`

这条路和现有复盘结论是一致的：

- 继续做 **shape / layout / working set** 优化
- 不再把主要精力放在 copy loop 微调上

### 11.1 当前机器上的稳定性结果

本轮已经按多轮脚本完成稳定性验证，命令是：

```bash
JSON_ENCODER_BENCH_REPEAT=7 \
JSON_ENCODER_BENCH_MIN_SECONDS=20 \
TIDE_JSON_V2_TILE_ROWS=1024 \
bash dev/json_encoder_bench_stability.sh
```

脚本实现见 [json_encoder_bench_stability.sh:L1-L139](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh#L1-L139)。

本轮结果：

- `speedup_min = 4.50885x`
- `speedup_median = 4.63439x`
- 阈值 `threshold_median = 3.0`
- 结果：`[OK] stability check passed`

对当前 Phase 4 判断更有价值的是数据集画像：

- `raw_fast_path_eligible_fields = 3/5`
- `total_escape_free_pct = 60.0942`
- `total_null_pct = 4.33014`

这也解释了为什么：

- `tileRows=1024` 在当前机器上是合理起点
- `raw + null-free` string fast path 值得继续挖
- `nullable` 分支不是当前 benchmark 的主战场

---

## 12. 一句话总结

`json_v2` 下一轮如果还想从 cache 利用率里拿到稳定收益，最合理的路线不是“把 copy 写得更花”，而是：

```text
整批 rows
  -> row tile
  -> arena fast path 最小热状态
  -> overflow 独立 side table
  -> 再视 profile 决定要不要 micro-tile / SIMD escape
```

从工程价值上看，这是一条比继续微调 `memcpy` 更像“下一跳”的方向。
