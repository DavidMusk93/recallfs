# LLVM 开发教程(结合 json_v2 case)

本文不是泛泛的 LLVM 入门,而是围绕仓库里 `json_v2` 的真实实现,把“如何开发/调试/验证一段 LLVM JIT 代码生成链路”讲清楚。

适用对象:

- 需要改 `generateKernelIr()` 的同学
- 需要新增/调整 `RuntimeVTable` ABI 的同学
- 需要通过 `kernel.ll / kernel.bc / kernel.o / kernel.so / compile.log` 快速定位问题的同学
- 需要把一次优化做成“可复现、可回退、可验收”的同学

相关代码:

- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)
- [jit_runtime.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h)
- [json_encoder_benchmark.cpp](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp)

推荐配合阅读:

- 方案设计与背景: [json_encoder_llvm_jit_plan.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_llvm_jit_plan.md)
- 全量优化总账(含正负优化): [json_encoder_v2_optimization_journal.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md)

---

## 0. json_v2 的 LLVM “开发闭环”是什么

先给一个“开发闭环图”,后面每一节都围绕它展开。

```text
修改 C++ 代码(生成 IR/运行时接口)
  |
  v
生成 kernel.ll(文本 IR)
  |
  v
llvm-as -> kernel.bc(bitcode)
  |
  v
llc -> kernel.o(object)
  |
  v
g++ -shared -> kernel.so
  |
  v
dlopen + dlsym 得到入口函数指针
  |
  v
执行 kernel(fn(ctx, rows))
  |
  v
profile + benchmark 验收(10s 分项 + 20s 长跑)
```

仓库里这条链路是“离线编译 + 动态加载”模型,不是 ORC/LLJIT 在进程内做 JIT。
优点是隔离 LLVM 运行时,缺点是 ABI 边界更硬,调试手段要靠 dump + log。

---

## 1. 产物在哪里: plan/IR/bitcode/object/so/log

`json_encoder_benchmark` 跑起来会打印出当前 kernel 的产物目录,类似:

```text
json_v2_artifacts: dump_plan=/tmp/.../plan.json, dump_ir=/tmp/.../kernel.ll, dump_bc=/tmp/.../kernel.bc,
dump_obj=/tmp/.../kernel.o, dump_log=/tmp/.../compile.log
```

对应代码:

- JIT 编译与落盘: [JITRuntime::getOrCompile](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L2350-L2426)

目录结构固定:

```text
<dump_root>/<module_id>/
  plan.json
  kernel.ll
  kernel.bc
  kernel.o
  kernel.so
  compile.log
```

其中:

- `plan.json`: 计划(字段名、类型、raw 等信息),用于复现与比对
- `kernel.ll`: 生成的 LLVM IR(可读性最好)
- `kernel.bc`: `llvm-as` 的产物,确认 IR 可被 LLVM 接受
- `kernel.o`: `llc` 编译出的目标文件
- `kernel.so`: 用 g++ 把 object 链成共享库,供 `dlopen`
- `compile.log`: 编译链路每一步 stdout/stderr 的拼接日志

---

## 2. 工具链是什么: llvm-as/llvm-dis/llc/g++

编译子进程逻辑在:

- [spawnCompiler](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1657-L1721)

它的步骤是:

```text
llvm-as    kernel.ll -> kernel.bc
llvm-dis   kernel.bc -> stdout(写入 compile.log,方便快速目视)
llc        kernel.bc -> kernel.o      (-filetype=obj -relocation-model=pic)
g++        kernel.o  -> kernel.so     (-shared -fPIC -O2)
```

工具路径查找规则:

- 优先用环境变量覆盖:
  - `TIDE_JSON_V2_LLVM_AS`
  - `TIDE_JSON_V2_LLVM_DIS`
  - `TIDE_JSON_V2_LLC`
  - `CXX`
- 否则用仓库内默认路径: `cpp3rdlib/llvm/bin/*`

这意味着:

- 你想快速验证 IR 合法性时,最关键的是 `llvm-as` 那一步
- 你想知道 LLVM 实际优化后变成什么样时,看 `compile.log` 里的 `llvm-dis` 输出(它是从 bitcode 反汇编回来的)

---

## 3. IR 是怎么生成的: generateKernelIr()

json_v2 的 codegen 是“拼字符串生成 IR”,入口是:

- [generateKernelIr](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1456-L1655)

它做了几件很重要的事:

1. 定义目标平台

```text
target triple = "x86_64-pc-linux-gnu"
```

2. 定义 `%RuntimeVTable` 和 `%RuntimeExecutionContext` 的 LLVM 侧 ABI

```text
%RuntimeVTable = type { ... function pointers ... }
%RuntimeExecutionContext = type { i8**, i8*, %RuntimeVTable* }
```

3. 为每个字段生成 `@jsonFieldPrefix<i>` 常量(字段前缀字面量)

```text
@jsonFieldPrefix0 = constant [N x i8] c"..."
```

4. 生成 kernel 主函数:

```text
define void @<symbolName>(i8* %opaqueCtx, i64 %rows) {
  %ctx = bitcast ...
  %fields = load ...
  %formatter = load ...
  %vtable = load ...
  ...
  call void %writeInt64Column(..., %fieldPrefix0, prefix_len)
  ...
  call void %rowEndAll(...)
  ret void
}
```

核心思想是:

- codegen 只负责“按 plan 固定顺序调度”
- 具体写 JSON 的热路径还在 host 侧的 `BatchFormatter`
- 两者通过 `RuntimeVTable` 函数指针表连接

---

## 4. 运行时接口是什么: RuntimeVTable / RuntimeExecutionContext

### 4.1 RuntimeExecutionContext

它是 kernel 的唯一参数,在 C++ 侧是:

```text
struct RuntimeExecutionContext {
  const ColumnAccessorBase** accessors;
  void* formatter;
  const RuntimeVTable* vtable;
}
```

生成 IR 里用 `bitcast + gep + load` 把它拆出来,然后进行调用。

执行入口在:

- [CompiledKernel::execute](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L2310-L2343)

### 4.2 RuntimeVTable

`RuntimeVTable` 是 ABI “硬边界”,既存在于:

- C++ 的 `struct RuntimeVTable { ... }`
- IR 的 `%RuntimeVTable = type { ... }`

两边必须完全一致,否则会发生“取错函数指针槽位”的灾难性错误(可能 crash,也可能悄悄跑慢)。

仓库里实际定义位置:

- `struct RuntimeVTable` / `runtimeVTable` 常量: [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1061-L1454)
- IR 侧 `%RuntimeVTable` 字段序: [generateKernelIr](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1456-L1467)

---

## 5. 最常踩的坑: vtable/ABI 不一致

这是 json_v2 迭代中最危险、也最常见的坑。

症状:

- 跑起来直接 `SIGSEGV` (dlsym 成功,但 call 到了错误地址)
- 或者不 crash,但性能断崖式下降(实际调用的函数不是你想的那一个)

根因:

- C++ `RuntimeVTable` 新增/调整了函数指针
- 但 IR `%RuntimeVTable` 的 type 没同步
- 或者 `getelementptr` 的索引没同步

### 5.1 安全修改 checklist

当你要新增一个 runtime 函数给 kernel 调用时,必须同时改这四处:

1. C++ `struct RuntimeVTable` 新增字段
2. C++ `runtimeVTable = { ... }` 初始化新增函数指针
3. IR `%RuntimeVTable = type { ... }` 新增对应函数指针类型
4. IR 里 `getelementptr ... i32 <index>` 使用的槽位索引同步更新

ASCII 图:

```text
你改了什么?         你必须同步什么?

RuntimeVTable(C++)  <-->  %RuntimeVTable(IR)
      |                         |
      v                         v
runtimeVTable 实例        gep index + call signature
```

### 5.2 最快自检方式

1. 先看 `kernel.ll` 里 `%RuntimeVTable` 的 type
2. 再对照 `jit_runtime.cpp` 里 `struct RuntimeVTable`
3. 确认字段数量、顺序、每个函数的参数列表完全一致

---

## 6. 怎么改一个 kernel: 以“改调度形态”为例

json_v2 当前 kernel 是 column-major 调度,形态是:

```text
rowStartAll(rows)
for each field:
  call write<Kind>Column(formatter, accessor, rows, fieldPrefixPtr, fieldPrefixLen)
rowEndAll(rows)
```

如果你要改调度形态(例如插入某个 prepass),推荐原则:

- 先改 `generateKernelIr()` 输出的 IR 结构
- 不要先改 runtime wrapper
- 让 `llvm-as` 先过,再上 bench

一个安全的增量方式是:

```text
1) 在 IR 里先插入一个 no-op 的 call (或一个额外的 call 到已有函数)
2) 通过 compile.log 确认 IR 进到 bitcode 没失败
3) 跑 json_v2-only 10s 看 profile 是否符合预期
4) 跑 both 20s 长跑验收
```

---

## 7. 怎么调试: 从 compile.log 到定位问题

调试链路建议按优先级:

### 7.1 IR 语法/类型错误

看:

- `compile.log` 的 `llvm-as` 报错
- `kernel.ll` 的出错行附近

### 7.2 dlsym/dlopen 错误

看:

- `compile.log` 末尾是否 g++ 链接失败
- `kernel.so` 是否存在
- 错误会在 C++ 侧以 `json_v2 dlopen failed` / `json_v2 dlsym failed` 报出来

对应代码:

- [loadKernelFromSharedObject](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1723-L1763)

### 7.3 不 crash 但性能很怪

优先怀疑:

- vtable ABI mismatch
- 调用签名 mismatch(比如 i1/bool 传错)
- 你以为走 fast path,但条件没满足(例如 overflow_rows 不为 0)

手段:

- 开 `TIDE_JSON_V2_PROFILE=1`
- 看 `json_v2_execute_profile_breakdown` 与 `json_v2_finish_profile`

---

## 8. 性能开发方法: 10s 分项 + 20s 长跑 + 稳定性

### 8.1 快速定位(10s)

```bash
env TIDE_JSON_V2_PROFILE=1 \
  JSON_ENCODER_BENCH_MODE=json_v2 \
  JSON_ENCODER_BENCH_MIN_SECONDS=10 \
  ./build64_release/src/test/json_encoder_benchmark
```

读这几行最关键:

- `kernel_ms` / `finish_ms`
- `key_ms` / `string_ms` / `double_ms` 等 breakdown
- `copy_values_ms`、`overflow_rows`、`copy_mode=inplace_pack` 等 finish 指标

### 8.2 最终验收(20s)

```bash
env JSON_ENCODER_BENCH_MODE=both \
  JSON_ENCODER_BENCH_MIN_SECONDS=20 \
  ./build64_release/src/test/json_encoder_benchmark
```

### 8.3 稳定性验收(推荐)

仓库里有收尾脚本:

- [json_encoder_bench_stability.sh](file:///root/Documents/stream_engine/dev/json_encoder_bench_stability.sh)

例子:

```bash
JSON_BENCH_TASKSET=0-15 \
JSON_ENCODER_BENCH_MIN_SECONDS=20 \
JSON_ENCODER_BENCH_REPEAT=7 \
bash dev/json_encoder_bench_stability.sh
```

它会给出:

- `speedup_min`
- `speedup_median`

并用 `median >= 3.0` 作为“稳定 3x”门槛。

---

## 9. 本 case 的三条关键经验(LLVM 开发视角)

### 9.1 优化通常不是“把 IR 写得更花”

这次真正的大收益来自:

- 改变数据流/布局(例如 `inplace_pack`)
- 把重复工作挪到 table 级缓存(例如 `float/double formatted cache`)

而不是:

- 再写一个更复杂的 copy loop
- 或者再写一版“更底层”的 IR 细节

### 9.2 ABI 是生命线

只要你还在用 `RuntimeVTable` 这种函数指针 ABI:

- 任何槽位/签名不一致都会非常难排
- 所以要用 checklist 强约束自己

### 9.3 观测先于优化

这轮能收敛到稳定 3x,靠的是:

- 每次改动前后都有 profile 指标
- 失败实验快速回退,不留“半吊子分支”
- 最后用稳定性脚本把验收标准固化

---

## 10. 附录: 最小“新增一个 runtime 能力”的示例模板

假设你要新增一个 runtime 函数 `writeFooColumn(...)`:

```text
1) C++:
   - struct RuntimeVTable 增字段
   - runtimeVTable 初始化填函数指针

2) IR:
   - %RuntimeVTable type 增字段类型
   - generateKernelIr 用 gep 取出该槽位
   - call 的参数类型和顺序必须一致

3) 自检:
   - llvm-as 是否通过
   - bench 是否能跑通
   - profile 是否符合预期(调用次数/分项时间)
```

这就是 json_v2 里“能快速进化但不失控”的基本做法。

