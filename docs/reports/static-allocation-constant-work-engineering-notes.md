# 《Static Allocation, Constant Work》工程分析

> 原文：matklad, *Static Allocation, Constant Work*, 2026-09-02。
>
> 原文链接：
> [https://matklad.github.io/2026/09/02/static-allocation-constant-work.html](https://matklad.github.io/2026/09/02/static-allocation-constant-work.html)
>
> 相关背景：
> [Memory Safety's Hardest Problem](https://matklad.github.io/2026/07/20/memory-safety-hardest-problem.html) |
> [TigerStyle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)
>
> 阅读日期：2026-09-07。本文先还原原文观点，再给出标为“工程判断”的边界
> 与推导。原文是一篇设计随笔，没有提供 benchmark 数据。

## 0. 结论先行

**这不是一篇主要介绍新 allocator 或 allocator 实现技术的文章。**

它从对象池中的 use-after-free（UAF）切入，短暂讨论了
type-segregated allocator（按类型隔离的分配器）如何降低跨类型内存混淆，
随后把重点转向两条 TigerStyle 系统设计原则：

1. **Static Allocation**：启动时按明确上限一次性准备内存，初始化完成后
   不再动态分配、释放和重新分配。
2. **Constant Work**：让每轮处理覆盖固定容量，用 `reserved` 之类的中性状态
   表示空槽，使工作量尽量不随当前活跃对象数量变化。

文章讨论的是下面这条设计链：

```text
Capacity limit
      |
      v
Startup allocation
      |
      v
Runtime slot transitions
      |
      v
Bounded overload behavior
      |
      v
Predictable work per cycle
```

allocator 在其中有三个不同角色，不能混为一谈：

| 层次 | 原文中的做法 | 是否是主线 |
| --- | --- | --- |
| 通用 allocator | 启动时用 `gpa.alloc` 申请整块 backing storage | 否，只是初始化工具 |
| 对象池 / slot allocator | 在固定数组中复用 `Order` 槽位 | 是，但重点是容量和状态模型 |
| typed segregated allocator | 不同类型使用不同池，避免释放后的地址被另一类型复用 | 否，是 UAF hardening 支线 |

最准确的一句话概括是：

> 文章不是在问“怎样让每次 `malloc` 更快”，而是在问“能否让核心运行阶段根本
> 不需要 `malloc`，并让过载时的资源和 CPU 成本仍然可预测”。

## 1. 文章从什么问题出发

### 1.1 对象池中的 UAF

来信者描述了一个限价订单撮合引擎中的错误：

- 订单取消后，槽位被放回对象池；
- 但订单仍链接在 price level 中；
- 下一次分配复用了该槽位；
- 旧链接因此指向一个新的订单。

这仍然是 UAF，只是对象池改变了它的物理后果。

不使用类型隔离时，风险链可能是：

```text
Stale pointer
      |
      v
Freed storage
      |
      v
Reused by another type
      |
      v
Cross-type confusion
      |
      v
Potential control-flow corruption
```

如果池只存储同一种 `T`，旧指针通常会读写“另一个 `T`”，不会直接把某种
整数布局解释成另一类型的函数指针布局。因此：

- 跨类型 type confusion 的攻击面下降；
- 行为更容易复现和分析；
- 但 stale alias 仍可能修改另一个业务对象；
- 订单身份、金额、权限等同类型字段被串改，仍然可能造成严重后果。

所以“对象池让 UAF 更安全”只能理解为**缩小物理内存破坏的种类**，不能理解
为修复了对象生命周期错误。

### 1.2 为什么 inline enum 是例外

原文的上一篇文章给出了 tagged union 的困难情形：

1. union 当前存放 variant `A`；
2. 代码取得指向 `A` payload 的内部指针；
3. 原 union 被覆盖为 variant `B`；
4. 旧指针仍按 `A` 的类型解释现在属于 `B` 的字节。

即使外层对象仍是同一个静态类型，variant payload 之间仍会发生物理类型
混淆。按外层类型隔离对象池无法解决这个问题。

### 1.3 typed allocator 只是安全加固支线

原文由此提出一个带有实验性质的想法：如果分配接口接收类型 `T`，而不只是
运行时的 `size + alignment`，allocator 可以为每种类型维护独立池：

```text
alloc(Order)   -> Order pool
alloc(Account) -> Account pool
alloc(Node)    -> Node pool
```

这样 `Order` 释放的地址不会立刻成为 `Account`。代价是：

- 不同类型之间不能共享空闲空间，可能增加内存碎片；
- 类型种类很多而每种对象很少时，池的固定开销会放大；
- 同类型 UAF、double free、越界和 inline enum 问题仍然存在；
- C 的传统 `malloc(size)` 接口没有类型信息，Fil-C 不能直接采用这一方案。

**工程判断：** 这是 allocator hardening，不是内存安全证明，也不是文章后半段
“Static Allocation, Constant Work”的必要前提。

## 2. Static Allocation 到底是什么

### 2.1 它不是编译期静态存储

原文示例仍然在启动时调用通用 allocator：

```zig
const orders: []Order = try gpa.alloc(Order, cli_args.orders_max);
```

因此这里的 “static” 不表示：

- 必须是全局变量；
- 必须放在可执行文件的 `.bss`；
- 容量必须是编译期常量；
- 完全没有发生 heap allocation。

它表示的是**分配阶段被静态地限制在初始化期**：

```text
Init phase                    Runtime phase
--------------------------    -----------------------------
Read capacity N               No heap growth
Allocate N slots              No free and reallocate
Initialize slot states        Reuse slots by state change
Fail startup if unavailable   Reject work beyond capacity
```

原文的例子允许：

```text
order-engine --orders-max=1_000_000
```

程序启动时为一百万个订单准备空间。运行时第 `1_000_001` 个并发订单不会尝试
“再挤一个”，而是被明确拒绝。

### 2.2 真正关键的是 admission control

预分配本身只是手段。真正的系统语义是：

```text
accepted <= configured_capacity
```

超过容量后必须有确定行为，例如：

- 返回 overload / resource exhausted；
- 对上游施加 backpressure；
- 按业务优先级拒绝；
- 进入另一个有独立上限的降级通道。

如果没有这层 admission control，固定池耗尽后仍可能出现无限重试、请求排队
或其他无界状态，只是把 OOM 换成了 retry storm。

### 2.3 为什么比“有内存就继续分”更可靠

系统在容量边界上继续动态申请内存，可能触发：

- allocator 慢路径和锁竞争；
- arena / size class 碎片；
- page fault、reclaim 和 swap；
- 内核 OOM killer；
- 延迟在最高负载下突然恶化。

最危险的地方在于：为了多接收一个对象，系统可能失去已经服务的一百万个
对象，甚至连 supervisor 也被 OOM killer 终止。

静态分配将失败时间前移：

| 阶段 | 动态增长系统 | 启动期静态分配系统 |
| --- | --- | --- |
| 启动 | 通常容易成功 | 容量不可满足则明确失败 |
| 正常运行 | 按需增长 | 在已验证的内存预算内运行 |
| 达到上限 | 可能继续申请并拖垮进程/节点 | 按协议拒绝新增工作 |
| 扩容 | 常在故障后被迫处理 | 可在服务仍可用时进行 |

这不是“永不失败”，而是把失败从不可预测的资源崩溃，转化为显式、局部且可测
的容量拒绝。

### 2.4 预分配也不等于物理内存已就绪

**工程判断：** 在有虚拟内存的操作系统上，成功申请大块地址空间不一定表示
所有物理页已经提交。首次写入仍可能触发 demand paging；进程也可能受到
swap、NUMA placement 和 memory cgroup 的影响。

原文随后用 `@memset(orders, .reserved)` 初始化所有槽位，这通常会触碰整个
范围，把一部分首次缺页成本移到启动阶段。这是有价值的，但若系统需要更强
的延迟保证，还应显式验证：

- 初始化后 RSS 与 committed memory；
- page fault 是否已在启动阶段发生；
- NUMA first-touch 是否落在预期节点；
- memory cgroup 和系统 overcommit 配置；
- 运行期第三方库是否仍会隐式分配。

## 3. Constant Work 到底是什么

### 3.1 从“空闲集合”改为“中性状态”

常见对象池会额外维护 bitmap 或 free list：

```zig
const OrderPool = struct {
    orders: []Order,
    free: DynamicBitSet,
};
```

原文提出另一种表示：固定数组中永远有 `N` 个 `Order`，空槽也是合法状态：

```zig
const Order = struct {
    id: u128,
    price: u32,
    count: u32,
    tag: enum { bid, ask, reserved },
};
```

初始化后所有槽位都是 `reserved`。创建和删除订单不再表现为对象的物理诞生与
销毁，而是状态转换：

```text
reserved -> bid
reserved -> ask
bid      -> reserved
ask      -> reserved
```

这带来一个“订单数量守恒”的模型：

```text
reserved + bid + ask = capacity
```

程序可以围绕每个状态转换写 precondition、postcondition 和 pair assertions，
再通过 deterministic simulation testing（DST）探索状态组合。

### 3.2 “Constant” 不是算法复杂度 `O(1)`

假设：

- `N` 是配置的总槽位数；
- `A` 是当前活跃订单数，`0 <= A <= N`。

只遍历活跃索引的近似成本是：

```text
W_indexed ~= A * (index_lookup + active_processing) + index_maintenance
```

扫描整个固定数组的近似成本是：

```text
W_full ~= N * state_probe + A * active_processing
```

因此 full scan 对 `N` 仍然是 `O(N)`。原文所谓 Constant Work，更准确地说是：

> 当容量 `N` 已固定时，每轮扫描的访问形状和工作上界不再随 occupancy `A`
> 线性扩张；低负载和满负载都执行同一个有界框架。

如果 `active_processing` 比 `reserved` no-op 昂贵，实际 CPU 时间仍会随状态
分布变化。I/O、锁竞争、调度、cache miss 和中断也不会因为固定数组而消失。

所以文章中的“P100 latency stays flat regardless of load”应视为设计目标，而
不是由代码形状自动得到的证明。

### 3.3 为什么满负载时顺序扫描可能更快

原文对比了两种访问：

```zig
for (orders) |order| {
    process(order);
}
```

和：

```zig
for (orders_active) |order_index| {
    const order = orders[order_index];
    process(order);
}
```

满负载下，直接扫描连续数组可能具有以下优势：

- 没有 index load 和二次寻址；
- 地址连续，硬件 prefetch 更容易工作；
- 编译器更容易识别 vectorization 机会；
- 不需要维护独立 live set；
- 测试时天然覆盖最大容量路径。

但这些是**需要测量的假设**，不是普遍规律：

- `process()` 有复杂分支或副作用时，编译器未必能 vectorize；
- 活跃索引也可以保持紧凑、有序；
- `N` 很大而 `A/N` 很低时，全量扫描会浪费内存带宽；
- `reserved` 与活跃状态随机交错时，分支预测可能恶化；
- 并发写状态可能带来 false sharing 和同步成本。

### 3.4 它用平均效率换最坏情况可见性

Constant Work 的核心价值不是让低负载更快，而是让系统在开发、测试和上线初期
就持续支付接近容量上界的成本。

```text
Variable work:
low load -> looks fast -> peak load reveals worst case

Constant work:
low load -> pays bounded scan -> worst case visible early
```

这相当于主动放弃一部分低负载效率，以换取：

- 更稳定的运行时形状；
- 更容易推导的 CPU 上界；
- 更早暴露的容量不足；
- 更少的低负载 benchmark 假象。

## 4. 它和 allocator 的真实关系

可以把文章放进 allocator 优化层级中理解：

| 优先级 | 要解决的问题 | 本文对应做法 |
| ---: | --- | --- |
| 1 | 系统最多允许多少 live state | 明确 `orders_max` |
| 2 | 超限时怎样拒绝和反压 | 不再尝试追加分配 |
| 3 | 运行期能否消除逻辑 allocation | 对象变成固定槽位的状态转换 |
| 4 | 生命周期能否对应独立 domain | 所有订单属于同一启动期 pool |
| 5 | 表示能否连续、可扫描 | 固定 `[]Order` |
| 6 | allocator 本身是否需要替换 | 文章没有讨论，也通常不是第一步 |

这与“先减少逻辑分配次数、再让 allocation domain 对齐生命周期、最后才比较
malloc/jemalloc/mimalloc”的工程顺序一致。

因此，对“是不是一种分配器场景”的完整回答是：

| 问题 | 回答 |
| --- | --- |
| 是否涉及 allocator？ | 是，启动时分配 backing storage，并讨论了 typed allocator。 |
| 是否在介绍一个新 allocator？ | 否，没有给出 allocator 数据结构、API、并发或回收算法。 |
| 是否在介绍 object pool？ | 部分是，但将 pool 推进为系统级固定容量模型。 |
| 主旨是否是内存性能？ | 不止。首要目标是安全、过载行为和尾延迟可预测性。 |
| 最接近什么概念？ | Bounded resource design、admission control、preallocation 和 fixed-work scheduling。 |

## 5. 它解决不了什么

### 5.1 固定池不会自动消灭 stale reference

如果旧指针在槽位重新用于新订单后仍然可访问，逻辑 UAF 依然存在。
`reserved` 状态也不够，因为旧指针可能在槽位再次进入 `bid` 或 `ask` 后通过
表面上的状态检查。

常见补强方式是 generational handle：

```text
handle = slot_index + generation
```

每次复用槽位时递增 generation，访问时同时校验 index、generation 和 state。
它有效的前提是：

- 所有长期引用都使用 handle，不能绕过为裸指针；
- generation wraparound 被证明足够远或被显式处理；
- 并发读取与复用之间有清晰同步；
- 序列化、网络和持久化中的旧 handle 同样接受校验。

### 5.2 有界内存不等于有界系统

还必须逐项限制：

- 输入队列和重试队列；
- 单请求 fan-out；
- 在途 I/O 和 buffer；
- timer、future、callback 和日志；
- 每轮状态机迭代次数；
- 外部依赖的等待时间。

只固定 `Order` 数组，却允许请求排队无限增长，系统仍然不是 bounded。

### 5.3 固定工作不等于实时保证

要得到 hard real-time 或近实时保证，还需要控制：

- page fault 和 swap；
- allocator 之外的锁与调度；
- I/O completion 和设备尾延迟；
- cache coherence 和 NUMA；
- 中断、频率变化与系统噪声；
- 每个状态分支的 worst-case execution time。

文章提供的是让上界更容易建立的结构，不是完整的 WCET 证明。

## 6. 适用与不适用场景

### 6.1 很适合

| 场景 | 原因 |
| --- | --- |
| 撮合引擎、支付核心状态机 | 容量和拒绝语义可明确，尾延迟比低载效率重要 |
| 存储副本的 I/O request pool | 并发上限天然受 queue depth 和 buffer 数约束 |
| 网络 packet/ring 处理 | 固定 descriptor 与批处理模型成熟 |
| 嵌入式和安全关键系统 | 内存预算固定，需要证明资源上界 |
| 数据库 buffer pool | 容量本来就固定，可用显式 eviction/admission |
| 有界 actor/task runtime | mailbox、task 和在途操作都能施加上限 |

### 6.2 需要谨慎

| 场景 | 风险 |
| --- | --- |
| 上限未知且增长很快的业务数据 | 预估过小频繁拒绝，过大则长期浪费 |
| `A/N` 长期极低的大型稀疏集合 | full scan 消耗过多内存带宽 |
| 对象尺寸长尾明显 | 固定槽位可能造成严重内部碎片 |
| 多租户且容量动态变化 | 静态切分容易让空闲资源无法跨租户复用 |
| 主要成本是远程 I/O 或复杂计算 | 固定扫描无法稳定真正的主导延迟 |

实际系统常采用混合设计：

- 每个资源域设置硬上限；
- 启动时预分配 hot-path pool；
- 稀有大对象进入另一个有界 arena；
- 稠密时顺序扫描，稀疏时使用受控索引；
- 溢出时拒绝、降级或 spill，但每条路径都有独立预算。

## 7. 工程落地检查表

### 7.1 设计

- `N` 来自什么生产分布、业务承诺和 headroom？
- 超限是拒绝、排队、降级还是 spill？每种行为的上限是什么？
- 资源是进程级、线程级、租户级还是 NUMA node 级？
- 是否能将生命周期改写成显式状态机？
- 哪些引用必须改成 index + generation？

### 7.2 正确性

- 对每个状态转换检查 precondition 和 postcondition。
- 持续断言各状态计数之和等于总容量。
- 覆盖 double release、重复取消、旧 handle、generation wraparound。
- 在 DST 中注入满池、超限、并发复用和取消/完成竞态。
- 确认第三方依赖在初始化后不会偷偷增长 heap。

### 7.3 性能

至少比较以下 workload，而不是只测平均 occupancy：

| 负载 | 必须回答的问题 |
| --- | --- |
| `A/N = 0%` | full scan 的固定税是多少？ |
| `A/N = 1%` | 稀疏时是否浪费内存带宽？ |
| `A/N = 50%` | 分支预测与 cache 行为如何？ |
| `A/N = 100%` | 承诺容量下吞吐和尾延迟是否合格？ |
| burst to limit | admission 与 backpressure 是否形成重试风暴？ |
| long soak | 是否仍有隐藏分配、碎片和计数漂移？ |

指标至少包括：

- allocation calls、allocated/resident bytes；
- minor/major page faults；
- cycles、instructions、branch misses、LLC misses；
- memory bandwidth；
- p50、p99、p999 和测试窗口内 maximum；
- overload rejection latency 与系统有效吞吐。

“P100”只是有限样本中的最大值。更可靠的交付物是：

1. 明确的资源和循环上界；
2. 满容量 benchmark；
3. 长时间高压下的高分位与 maximum；
4. 对剩余 OS、I/O 和同步抖动的单独归因。

## 8. 最终判断

这篇文章把对象池从一个“减少 `malloc/free` 次数”的局部优化，上升为一种
系统架构：

```text
Fixed capacity
      +
Explicit state transitions
      +
Bounded admission
      +
Predictable scan shape
      =
Failure and cost become visible early
```

它最有价值的地方不是某个 allocator 技巧，而是**先设计资源上限和过载语义，
再设计内存布局与执行路径**。

可以采用它的原则，但不能机械采用“总是扫描全部槽位”这一实现。是否值得做
full scan，必须由容量 `N`、occupancy 分布、每状态处理成本、cache 行为和
满负载尾延迟共同决定。对象池、typed allocator 和 generational handle 是
互补工具，不能互相替代。
