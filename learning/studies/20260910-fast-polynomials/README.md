---
doc_id: study-fast-polynomials-20260910
kind: study
status: active
authority: explanatory
applies_to:
  - learning/studies/20260910-fast-polynomials
depends_on:
  - learning/studies/20260910-fast-polynomials/source.md
  - learning/studies/20260910-fast-polynomials/exploration.md
  - learning/studies/20260910-fast-polynomials/demo/README.md
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/evidence/README.md
  - learning/studies/20260910-fast-polynomials/report.html
---

# Fast Polynomial Evaluation 学习报告

> 原始页面：`https://thomasahle.com/fast-polynomials/`
>
> 论文：Thomas D. Ahle and Jakob B. T. Knudsen,
> *Fast Evaluation of Polynomials with Rational Preprocessing*,
> arXiv:2609.06022 v1, 2026。
>
> 阅读与复现日期：2026-09-10。

## 1. 结论先行

这篇文章展示的不是一条手写算式，而是一个 **polynomial
evaluation-chain compiler**：

1. 输入一个固定系数多项式；
2. 离线把 coefficients 变换成另一组参数；
3. 生成共享中间结果的 straight-line program；
4. 在线对许多 $x$ 求值。

它的主定理是：对 characteristic 0 或 characteristic $p>n$ 的域，任意
monic degree-$n$ 多项式经过 rational preprocessing 后，都能以

$$
\left\lfloor\frac{n}{2}\right\rfloor+1
$$

次乘法求值；一般的 non-monic 多项式再多一次乘法。相比之下，Horner 对
monic degree-$n$ 多项式需要 $n-1$ 次乘法。

这个结果在数学上很强，但工程结论必须更窄：

> **它首先是“减少昂贵 field multiplication”的方法，不是“让所有浮点
> 多项式都更快”的方法。**

最合适的真实场景是有限域 polynomial hashing、sketch/filter、coding 和
某些 proof-system kernels：乘法含 reduction，成本高；参数能预处理并被反复
使用；运算本身 exact。

对普通 `double` 多项式，结论可能相反。本 study 在 AMD EPYC 7Y83 上复现
degree-9 C evaluator：

| FP 模式 | 工作负载 | Horner | 五乘法链 | 结果 |
| --- | --- | ---: | ---: | --- |
| `-ffp-contract=off` | dependent latency | 16.852 ns/eval | 13.498 ns/eval | 链快 19.9% |
| `-ffp-contract=off` | 8-way scalar throughput | 2.837 ns/eval | 3.684 ns/eval | 链慢 29.9% |
| `-ffp-contract=fast` | dependent latency | 11.852 ns/eval | 13.088 ns/eval | 链慢 10.4% |
| `-ffp-contract=fast` | 8-way scalar throughput | 1.400 ns/eval | 3.858 ns/eval | 链慢 175.6% |

Horner 能直接收缩成连续 FMA；预处理链虽然乘法少，却需要更多加减法和更复杂
的数据流。**运算计数、critical path 和 throughput 是三个不同指标。**

## 2. 页面到底讲了什么

页面允许输入一个多项式，选择域和求值方法，然后查看：

- 数学形式的 evaluation chain；
- 可编译的 C；
- circuit graph；
- 与 Horner、Estrin、Rabin-Winograd、Knuth-Eve、Pan 的操作数对比。

默认桌面示例是 degree-9 reverse Bessel polynomial：

$$
\begin{aligned}
P(x)={}&x^9+45x^8+990x^7+13860x^6+135135x^5\\
      &+945945x^4+4729725x^3+16216200x^2\\
      &+34459425x+34459425.
\end{aligned}
$$

页面生成的 chain 使用 5 次乘法、20 次加减法，multiplicative depth 为 4。
同一多项式的 Horner 需要 8 次乘法、9 次加法，depth 为 8。

因此页面把论文中相当复杂的构造变成了实用工具：输入 coefficients，输出
可审计、可下载、可编译的 evaluation program。

## 3. 第一性原理：为什么一次乘法能承载两个系数

考虑 monic cubic：

$$
P(x)=x^3+c_2x^2+c_1x+c_0.
$$

先计算：

$$
H_2=x^2,
$$

再计算：

$$
Q_3=(x+\alpha_2)(H_2+\alpha_1)+\alpha_0.
$$

展开：

$$
Q_3=x^3+\alpha_2x^2+\alpha_1x+
(\alpha_1\alpha_2+\alpha_0).
$$

于是：

$$
\alpha_2=c_2,\qquad
\alpha_1=c_1,\qquad
\alpha_0=c_0-c_1c_2.
$$

`x*x` 和两个 affine forms 的乘积共 2 次乘法，却容纳了 3 个自由
coefficients。直觉上，一次

$$
(A+\alpha)(B+\beta)
$$

不只生成一个高次项，也同时把 $\alpha$、$\beta$ 注入不同 coefficient
positions。只要这些位置按 triangular order 可恢复，一次乘法就能引入大约
两个自由参数。

完整构造把这个思想递归化：

```text
+--------------------+
| target coefficients|
+----------+---------+
           |
           v
+--------------------+
| exact decoder      |
| coefficients -> key|
+----------+---------+
           |
           v
+--------------------+
| fixed circuit DAG  |
| shared intermediates|
+----------+---------+
           |
           v
+--------------------+
| evaluate many x    |
+--------------------+
```

它维护 compatible pairs 和可恢复的 coefficient windows，使用已知的 monic
power gadgets 移动系数块，并最终组合成

$$
P(x)=xT^{(1)}(x)+T^{(2)}(x).
$$

最后乘以 $x$ 的操作贡献了公式中的额外 `+1`。

## 4. 论文真正解决了什么

### 4.1 乘法上界

论文把 Rabin-Winograd 的

$$
\frac{n}{2}+O(\log n)
$$

进一步降到 $\lfloor n/2\rfloor+1$，消除了 logarithmic overhead。它同时给出：

- 最多
  $\min(2n,\frac54n+6\lceil\log_2n\rceil^2+1)$
  次加减法；
- odd degree 的 multiplicative height 至多
  $2\lceil\log_2n\rceil+4$；
- even degree 的 height 再多 1。

这说明它不是用一条更长的串行链换取较少乘法，而是同时保留了
$O(\log n)$ 乘法深度。

### 4.2 下界

Motzkin 的维数下界说明，monic degree-$n$ 多项式一般至少需要
$\lceil n/2\rceil$ 次乘法。因此 odd degree 时，该论文的乘法数达到最优。

even degree 更微妙。某些旧方法能在 generic inputs 上达到更低计数，但
preprocessing 会在一个 hypersurface 上无定义。论文证明 degree 6 如果要求
everywhere-defined rational decoding，3 次乘法不够，至少需要 4 次。这说明
“异常输入是否允许存在”会改变最优复杂度。

### 4.3 参数化比右逆更强

论文不只找到了“给定 coefficients 时能求出一组参数”的 right inverse。
构造中的 coefficient map 与 decoder 形成双向对应，即 polynomial
automorphism。对 hashing，这意味着：

- 均匀采样参数；
- 等价于均匀采样 monic polynomial coefficients；
- 可以继续使用 Vandermonde argument 证明 $k$-wise independence。

如果映射只是 many-to-one 或漏掉部分 polynomials，随机参数不再自动对应目标
hash family。

## 5. C 如何表达

网页生成的核心 C 可以直接写成：

```c
double fp_eval_p9_chain(double x) {
    double y = (x + 11.0) * x;
    double z = (x + y - 21.5) * (-x + y + 132.5);
    double t =
        (x + y + z + 12803.75) *
        (-x - y + z + 1254.75);
    double u =
        (y + z + 17266.75) *
        (-y + z + 1347.75);
    double v = (x + t + 32741929.0) * x;
    return u + v + 56100843.0;
}
```

对应 Horner：

```c
double fp_eval_p9_horner(double x) {
    double value = x + 45.0;
    value = value * x + 990.0;
    value = value * x + 13860.0;
    value = value * x + 135135.0;
    value = value * x + 945945.0;
    value = value * x + 4729725.0;
    value = value * x + 16216200.0;
    value = value * x + 34459425.0;
    return value * x + 34459425.0;
}
```

这里的 C 语法并不特殊。真正困难的部分在运行前：

- 从 coefficients 求出 chain parameters；
- 证明任意允许输入都能解码；
- 生成不会破坏 field semantics 的算术；
- 为 GF($2^k$) 选择 PCLMULQDQ/PMULL 和正确 reduction；
- 为 prime field 保证中间位宽与 modular reduction；
- 为 floating point 验证 error contract。

因此生产实现应当使用受测试的 generator，而不是人工抄写或调参。

## 6. 六个有趣现象

### 6.1 预处理把工作从热路径移到了冷路径

这与 FFT twiddle table、数据库 query plan、常量除法 magic number 类似。
coefficients 固定后，decoder 的成本可以摊到大量 evaluations 上。若
coefficients 每次都变且只求值一次，在线少几次乘法很可能抵不过预处理。

### 6.2 最小乘法数不等于最少时间

degree-9 页面示例：

| Method | Multiplications | Additions | Multiplicative depth |
| --- | ---: | ---: | ---: |
| Paper chain | 5 | 20 | 4 |
| Horner | 8 | 9 | 8 |
| Estrin | 11 | 9 | 4 |
| Rabin-Winograd | 8 | 12 | 4 |

单条 dependent evaluation 更关心 depth；批量 throughput 更关心总 uops、
执行端口、寄存器压力和是否能形成 FMA/SIMD。目标机结果正好展示了这两个方向
可以相反。

### 6.3 FMA 会改变赢家

Horner 的每一级天然是：

$$
y\leftarrow yx+c,
$$

即一条 FMA。开启 `-ffp-contract=fast` 后，目标 GCC 把它变成连续
`vfmadd`，把 strict-FP 下的串行劣势消掉。预处理链的输入端有大量
affine-form additions，不能同样完整地收缩。

所以不能只看论文 gate count，也不能只看 `-O3`；必须把 FP contraction
policy 当成 benchmark contract。

### 6.4 Exact algebra 与 floating-point stability 完全不同

对 $P(x)=x^9$，generator 给出的 5 次乘法链包含多个非零 dyadic constants。
它们在 exact rational arithmetic 中消掉全部低阶项，但在 binary64 中会发生
catastrophic cancellation。

本 study 的负向锚点是：

$$
x=2^{-10},\qquad P(x)=2^{-90}.
$$

普通 repeated multiplication 精确得到 $2^{-90}$；预处理链得到 0，相对误差
为 1。论文的更高 degree 实验显示 prescribed coefficients 模式会严重得多：
degree 31 的样本直接 overflow。

这不是实现 bug，而是表示条件数。论文定义 schedule amplification：

$$
A_C(P,x)=
\frac{M_C(|x|)}
{\sum_i |a_i||x|^i},
$$

其中 $M_C$ 把 chain 中所有常数取绝对值、减法换成加法。大量本应抵消的内部
项会让 $M_C$ 很大，而目标 polynomial 本身很小。

### 6.5 “coefficients 是数据”与“keys 是数据”是两种算法

- **Prescribed coefficients**：先给任意 $a_i$，再解码 $\alpha_i$。参数可能
  极大并严重抵消，不适合通用 binary64。
- **Prescribed keys**：直接采样 $\alpha_i$，polynomial 由它们诱导。这正是
  hashing 的自然语义，参数不需要从病态 inverse map 中产生。

论文实验中，第二种模式在 hashing-relevant degrees 上保持良好 floating
误差；有限域中更没有舍入误差。

### 6.6 field characteristic 会改变可行结构

主构造依赖 2 等小整数可逆，所以统一结论要求 characteristic 0 或
characteristic $p>n$。characteristic 2 中

$$
(a+b)^2=a^2+b^2
$$

使部分解码 pivot 消失，必须利用 Frobenius map 的特殊结构。当前工作只给出
有限 degree 的显式 circuits 和 decoders；不能把 characteristic-zero 代码
直接换一个整数类型就当作 GF($2^k$) 实现。

## 7. 对真实项目的收益

### 7.1 最可信的收益：有限域 hashing

在 GF($2^{64}$) 上，field multiplication 包含 carry-less multiply 和
reduction，明显比 XOR/add 更贵。论文对每次 $10^6$ 个 hashes 的 upstream
结果显示：

| k-wise degree | ARM Horner | ARM chain | x86 Horner | x86 chain |
| --- | ---: | ---: | ---: | ---: |
| 5 | 1895 us | 1434 us | 4816 us | 3090 us |
| 7 | 3225 us | 2241 us | 8535 us | 4880 us |
| 9 | 4869 us | 2641 us | 12817 us | 5952 us |

degree 9 相对 Horner 分别约快 `1.84x` 和 `2.15x`。这组结果与数学成本模型
一致，因为被减少的确实是昂贵操作。

### 7.2 端到端收益会被其他工作稀释

论文报告：

| Workload | ARM speedup vs Horner | x86 speedup vs Horner |
| --- | ---: | ---: |
| CountSketch update | 1.27x | 1.62x |
| Linear-probe insert | 1.14x | 1.53x |
| Linear-probe lookup | 1.11x | 1.47x |
| XOR-filter query | 1.10x | 1.76x |

这些数字说明 Amdahl's law：hash evaluation 变快后，memory access、probe、
counter update 和 filter construction 占比上升。XOR-filter build 在 ARM 上
甚至是 Rabin-Winograd 更快。

### 7.3 可以考虑的项目

| 项目形态 | 是否值得试 | 首要检查 |
| --- | --- | --- |
| $k$-wise independent hashing | 是 | independence proof、field kernel、key mapping |
| sketches / randomized load balancing | 是 | hash 在总 CPU 中的占比 |
| finite-field coding / erasure kernels | 是 | 固定 degree、批大小、reduction cost |
| STARK/secret-sharing evaluation kernel | 有条件 | chain 是否有该 field 的有效 decoder/certificate |
| 固定 libm approximation polynomial | 谨慎 | ULP/relative error、FMA、完整输入区间 |
| 任意 runtime coefficients 的 `double` | 通常否 | preprocessing stability 和 fallback |
| 一次性求值 | 通常否 | preprocessing 无法摊销 |

论文中的部分 prime-field benchmark 使用 search candidates，只验证 evaluation
速度，没有展示对应 field 上的 inverse。工程中不能把“同一 polynomial 算得
快”误写成“随机 key 仍保持目标 independence guarantee”。

## 8. 采用时的最小协议

```text
+---------------------+
| define semantics    |
| field, degree, data |
+----------+----------+
           |
           v
+---------------------+
| generate + verify   |
| decoder and chain   |
+----------+----------+
           |
           v
+---------------------+
| numerical/exact gate|
| range and error     |
+----------+----------+
           |
           v
+---------------------+
| inspect final binary|
| mul, FMA, SIMD, DCE |
+----------+----------+
           |
           v
+---------------------+
| benchmark real call |
| latency + throughput|
+---------------------+
```

具体要求：

1. 先写清 field、monic/non-monic、degree、coefficients/keys 谁是输入。
2. 固定 generator revision，并保存 coefficients、parameters 和 chain digest。
3. exact field 做 differential/exhaustive checks；floating point 做全 domain
   error sweep，并与高精度 oracle 比较。
4. 检查 final binary，不用源代码乘号数量代替 codegen。
5. 同时测单条 latency 与批量 throughput；使用真实调用方的数据布局。
6. 保留回退。浮点场景可在 preprocessing 时计算 amplification guard，超预算
   就退回 Horner/Estrin。

## 9. 本地复现

代码位于 `demo/`。权威正确性命令：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260910-fast-polynomials/demo/include \
  learning/studies/20260910-fast-polynomials/demo/src/fast_polynomial.c \
  learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c \
  -lm -o .tmp/fast-polynomial-test

.tmp/fil-c/bin/filrun .tmp/fast-polynomial-test
```

行为锚点：

| ID | Contract |
| --- | --- |
| `FP-A1` | 8 个 integer inputs 上，Horner 与 chain 都等于精确整数 |
| `FP-A2` | $\theta_9$ 在 $[-2,2]$ 的 8193 点上相对 long-double oracle 不超过 $10^{-12}$ |
| `FP-A3` | $x^9$ 在 $x=2^{-10}$ 暴露至少 50% 的 chain 相对误差 |
| `FP-A4` | strict-FP target binary 保留 8 对 5 次 scalar multiplication |
| `FP-A5` | benchmark rounds 放大 4 倍时，wall time 近似放大 4 倍 |

完整工具链、目标机、重复次数、digests 和 PMU 限制记录在
[`evidence/README.md`](evidence/README.md)。

## 10. 最终判断

这项工作的核心不是“发现了比 Horner 更聪明的括号展开”，而是证明了：

> 对完整的 monic polynomial family，可以设计一个可逆的 coefficient
> coordinate transform，让在线 evaluation circuit 用接近信息论下界的乘法数。

它最有工程价值的地方，是把 expensive multiplication 从约 $n$ 个压到约
$n/2$ 个，同时保留 logarithmic depth。它最危险的误读，是把这个 field-level
结论直接套到 `double` wall time 或 arbitrary-coefficient numerical stability。

对真实项目，默认决策应是：

- exact finite field + repeated evaluation：值得原型验证；
- scalar floating point + FMA：先保留 Horner，除非 target benchmark 和 error
  analysis 同时证明 chain 更好；
- arbitrary floating coefficients：没有 amplification guard 和 fallback 就不要
  采用。
