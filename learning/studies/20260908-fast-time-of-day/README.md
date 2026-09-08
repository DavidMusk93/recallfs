# Fast Time-of-Day Study

> 原文：[A faster way to convert a timestamp -> Hour, Min, Sec](https://www.benjoffe.com/fast-time-of-day)
>
> 阅读日期：2026-09-08。本文用 C、FIL-C、目标机反汇编和原生 benchmark
> 验证核心机制，不把原文数字当作本机结论。

## 1. 结论先行

最值得迁移的优化不是 magic number，而是 **先重写数据依赖图**：

```c
uint32_t total_minutes = time / 60;
uint32_t hour = time / 3600;
uint32_t second = time - total_minutes * 60;
uint32_t minute = total_minutes - hour * 60;
```

这就是 V1。它在完整 `uint32_t` 输入域都成立，可读性好，也让 CPU 同时启动
两次常量除法。目标机 AMD EPYC 7Y83 / GCC 12.2 上，它相对传统写法：

- 串行依赖延迟从 `7.180` 降到 `3.745 ns/value`，下降 `47.8%`；
- 八路独立标量吞吐从 `2.510` 降到 `2.072 ns/value`，下降 `17.5%`。

因此，文章最稳健的结论是：

> 运算数量不是唯一成本。单次请求受最长依赖链约束；批量吞吐取决于总操作数、
> 执行端口、寄存器压力和编译器调度。先缩短依赖链，再决定是否值得引入定点
> 算术。

V2/V3 进一步利用定点乘法。它们对日内秒数正确，但不是完整 `uint32_t`
除法的通用替代。当前目标机上 V3 最快，延迟和吞吐分别比传统写法低
`56.5%` 和 `30.0%`；这与原文部分平台的吞吐排序不同，不能外推到其他 CPU
或其他 consumer。

## 2. 问题边界

输入不是 Unix timestamp，而是已经归一化到一天内的无符号秒数：

```text
input:  day_second in [0, 86399]
output: hour in [0, 23], minute in [0, 59], second in [0, 59]
```

以下工作不在本算法内：

- 把 Unix timestamp 拆成日期与日内秒数；
- 时区、夏令时和 calendar conversion；
- 表示 `23:59:60` 的闰秒；
- 负时间或 signed division；
- 亚秒字段和 SIMD。

## 3. 第一性原理：重写商和余数

令：

```text
t = 60q + s       where q = floor(t / 60), s in [0, 59]
q = 60h + m       where h = floor(q / 60), m in [0, 59]
```

因为 `floor(floor(t / 60) / 60) = floor(t / 3600)`，所以：

```text
h = floor(t / 3600)
s = t - 60q
m = q - 60h
```

传统顺序计算形成一条长链：

```text
+-------+    +------+    +-----------+    +--------+
| input |--->| hour |--->| hour rem  |--->| minute |---> second
+-------+    +------+    +-----------+    +--------+
```

V1 把同一恒等式写成两条可重叠的链：

```text
                  +--------------+-----> second
              +-->| total minute |
              |   +--------------+-----> minute
+-------+-----+
| input |     |
+-------+-----+
              |   +--------------+-----> minute
              +-->| hour         |
                  +--------------+
```

真正的优化对象是 critical path。源码中的先后顺序不保证 CPU 指令串行，但
数据依赖保证后继不能早于前驱完成；V1 消除了不必要的前驱关系。

## 4. 定点除法为什么有范围

对常量除数 `d`，文章使用：

```text
M = floor(2^32 / d) + 1
q = floor(x * M / 2^32)
```

`M / 2^32` 略大于 `1/d`。只要累计向上误差尚未跨过下一个整数边界，`q`
就等于 `floor(x/d)`；输入继续增大后，近似会提前进位。因此 range 是算法
contract，不是注释性质的建议。

本 study 穷举确认：

| Variant | 最后正确输入 | 第一个失配输入 |
| --- | ---: | ---: |
| V1 fixed-point | `2257198` | `2257199` |
| V2 hi/low | `2255818` | `2255819` |
| V3 base-64 | `2257198` | `2257199` |

这些边界都远大于 `86399`，所以适合“日内秒数”契约；若 API 接受任意
`uint32_t` duration，则只有传统算法和 V1 division 可直接使用。

## 5. V2：低位不是废料

对于宽乘积 `p = x * M`：

- 高 32 位近似商；
- 低 32 位表示向下一个整数商推进的定点小数部分。

V2 对 hour multiplier 的低位乘 60 并取高位，得到 minute；对 minute
multiplier 的低位做同样操作，得到 second。这样四次乘法组成两条并行链，
不需要显式 remainder。

这种解释比背诵常量重要：low half 只有在 multiplier、rounding direction
和输入范围共同成立时才具有这个语义。

## 6. V3：把 60 补成 64

令 `x = qD + r`，其中 `q = floor(x/D)` 且 `0 <= r < D`。对任意正整数
`c`：

```text
x + cq = q(D + c) + r
```

因此：

```text
x mod D = (x + c * floor(x / D)) mod (D + c)
```

取 `D=60, c=4`，右侧 modulus 变成 64，可用 `& 63`：

```c
second = (time + 4 * total_minutes) & 63;
minute = (total_minutes + 4 * hour) & 63;
```

目标机反汇编显示 V3 latency loop 的每次转换保留了 2 个 `imul`、2 个
`lea` 和 2 个 `and`，没有 `idiv`。这条技巧依赖非负整数和已验证 range；
不要把 signed `%` 的语义混进来。

## 7. 实测

目标：`dc02-pe-t137-n047`，AMD EPYC 7Y83，2 sockets、128 cores、
SMT2、4 NUMA nodes。进程固定在 NUMA 0 的 CPU 31，governor 为
`performance`，boost 开启。编译器为 GCC 12.2.0：

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG
-flto -fno-ipa-icf -Wall -Wextra -Werror
```

下表取三次独立运行各自 median 的中位数；每次包含 31 个 sample，每个
sample 为 `8192 * 256` 次转换。这些是 run-level median 汇总，不是 31 个
sample 的原始日志。

| Algorithm | Latency ns/value | vs traditional | Throughput ns/value | vs traditional |
| --- | ---: | ---: | ---: | ---: |
| Traditional | `7.180` | baseline | `2.510` | baseline |
| V1 division | `3.745` | `-47.8%` | `2.072` | `-17.5%` |
| V1 fixed-point | `3.747` | `-47.8%` | `1.875` | `-25.3%` |
| V2 hi/low | `3.746` | `-47.8%` | `1.844` | `-26.5%` |
| V3 base-64 | `3.122` | `-56.5%` | `1.757` | `-30.0%` |

这组数字证明“同一机器、同一 consumer、同一 compiler”下依赖重排有效，
不证明 V3 在真实日期库或其他机器上总是最快。特别是：

- benchmark 为了保留三个输出，latency 和 throughput loop 都对每个字段
  使用 compiler barrier；latency 再把三者注入下一轮；
- throughput loop 使用八条独立 checksum chain，并明确禁用自动向量化；
  目标机上的五个 throughput symbol 都有 count 与 rounds 两条 backward
  branch，且没有 `xmm`/`ymm`/`zmm` 寄存器引用；
- 传统 loop 在 GCC 12 生成了一个 `idiv`，而 V1 被 strength-reduce 为
  reciprocal multiply；编译器选择显著放大了差距；
- PMU hardware events 返回 `<not counted>`，因此没有 cycles/instructions
  交叉证据，只有 wall time、软件 perf events 和反汇编。

完整环境、hash、run-level median 汇总、throughput codegen 证明和限制见
[`evidence/target-benchmark.md`](evidence/target-benchmark.md)。

## 8. 工程选择

| 场景 | 建议 |
| --- | --- |
| 普通日期库，尚无 profile | 保持现状，不为纳秒级代码增加维护成本 |
| 标量单次延迟敏感 | 先采用 V1 division，收益来自结构且 full-range |
| 批量标量转换热点 | 同时 benchmark V1 fixed、V2、V3 |
| 输入可能超出一天 | 用 V1 division，或在 API 边界强制并测试 range |
| SIMD 数据库算子 | 单独检查 auto-vectorization 和目标 ISA，不沿用标量排名 |
| signed 输入 | 先定义负值语义；不能直接照搬 unsigned mask 技巧 |

实践时必须把“函数公式”和“调用者如何消费结果”一起 benchmark。若测试只
消费 `hour ^ minute ^ second`，优化器可能合并两个 mask，测到的就不再是
物化完整返回值的成本；本 study 的第一次反汇编检查正好发现并修复了这个
问题。

## 9. Demo

| Item | Value |
| --- | --- |
| Path | `learning/studies/20260908-fast-time-of-day/demo/` |
| Language | C11 |
| Correctness | FIL-C 0.684; all variants exhaustive over one day; fixed-point variants exhaustive over their claimed extended ranges; full-range division variants sampled at uint32 boundaries |
| Native target | AMD EPYC 7Y83, GCC 12.2.0 |
| Build and run | See [`demo/README.md`](demo/README.md) |
| Exploration | See [`exploration.md`](exploration.md) |

## 10. Next Steps

1. 在真实调用点 profile，而不是直接替换公共日期 API。
2. 若采用任一定点版本（V1 fixed-point、V2、V3），把 `[0, 86399]`
   变成可执行的入口断言或类型边界。
3. 在生产 compiler 升级后重跑 codegen 与 benchmark；常量除法优化会变化。
4. 若真实 workload 是 columnar batch，增加 AVX2/AVX-512 专项版本并验证
   vectorization report，而不是用本次 scalar 结果推断。
