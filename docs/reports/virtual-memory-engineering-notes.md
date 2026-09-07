# 《Virtual Memory From First Principles》开发者工程评析

> 原文：Abhinav Upadhyay, *Virtual Memory From First Principles*，
> 2026-05-11。
>
> 原文链接：
> [https://blog.codingconfessions.com/p/virtual-memory](https://blog.codingconfessions.com/p/virtual-memory)
>
> 阅读日期：2026-09-07。本文以原文建立主线，再用 Linux 官方文档和
> man-pages 校正实现边界。文中的“工程判断”不是原文结论。

配套 C demo：
[`tools/docs/examples/virtual_memory_demo.c`](../../tools/docs/examples/virtual_memory_demo.c)。
该 demo 已使用 FIL-C 0.684 编译并运行通过，覆盖按需分页、缺页计数、
`fork` 后的写时复制和 `MAP_SHARED` 可见性。复验：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_demo.c \
  -o .tmp/virtual-memory-demo
.tmp/fil-c/bin/filrun .tmp/virtual-memory-demo
```

## 0. 结论先行

这篇文章对开发最有价值的地方，不是记住 x86-64 页表的四级名称，而是建立
以下因果链：

```text
Application access pattern
          |
          v
Virtual mapping and permissions
          |
          v
Page fault and residency
          |
          v
TLB and cache behavior
          |
          v
Reclaim and NUMA placement
          |
          v
Latency, throughput, and failure mode
```

它能帮助开发者纠正五个常见误判：

1. **申请成功不等于物理内存已经就绪。** `malloc` 或匿名 `mmap` 通常先建立
   可用地址范围，首次触页才分配和建立映射。
2. **RSS、虚拟地址空间和业务 working set 不是同一个量。** 只看 VSZ 或
   容器 memory usage，无法解释内存为什么慢、为什么被 OOM。
3. **数据在 RAM 中不等于访问代价相同。** TLB miss、cache miss、页表遍历
   和 NUMA remote access 都可能成为瓶颈。
4. **`mmap` 不是无成本 zero-copy。** 它省掉 page-cache 到用户 buffer 的
   copy，却引入 fault、PTE、TLB、shootdown 和 `SIGBUS` 等成本与失败模型。
5. **页大小和内存布局是算法参数。** 对大 working set，结构体 padding、
   pointer chasing、页数和 TLB reach 会共同决定吞吐。

对数据库、查询引擎、缓存、runtime、allocator 和高并发服务，本文可以作为
一张“从业务症状回溯到 VM 层”的地图；但它是教学模型，不应直接替代目标
机器上的 kernel、CPU、cgroup、NUMA 与 workload 证据。

## 1. 原文建立了什么模型

### 1.1 地址空间不是物理内存

进程看到的是独立虚拟地址空间。VMA 和页表分别表达两类状态：

| 层次 | 回答的问题 | 典型对象 |
| --- | --- | --- |
| VMA | 这个地址范围是否有效、权限是什么、由什么对象支持 | `vm_area_struct` |
| Page table | 某个虚拟页此刻映射到哪个物理 frame | PGD/P4D/PUD/PMD/PTE |
| TLB | 最近使用的地址翻译是否已经在 CPU 中缓存 | per-CPU translation cache |
| Physical page | 数据此刻是否驻留、位于哪个 NUMA node | page/folio/frame |
| Backing store | 不驻留时能从哪里恢复 | file、swap 或 zero-fill |

因此，同一个指针至少有四种独立属性：

- 地址是否落在合法 VMA；
- 当前权限是否允许 read/write/execute；
- PTE 是否 present；
- backing data 是否需要 I/O 才能恢复。

这正是“合法地址也会 fault”“fault 不一定是 bug”“present=0 不一定是
未分配”的根源。

### 1.2 页表是稀疏索引

如果给 128 TiB 用户地址空间中的每个 4 KiB 页都预留一个 8-byte PTE，
平坦页表本身就需要 256 GiB。多级页表通过只为实际使用的地址范围创建下层
表，把成本从“整个地址空间”约束到“已使用地址空间的稀疏形状”。

在 Linux 通用模型中，软件层次是：

```text
PGD -> P4D -> PUD -> PMD -> PTE -> page frame
```

具体架构可以折叠某些层级。常见四级 x86-64 会折叠 `P4D`；启用五级页表的
x86-64 则不会。开发代码通常不应该依赖某一台机器的固定层数。

### 1.3 page fault 是机制，不是错误分类

page fault 表示硬件无法按当前页表状态直接完成访问。内核随后根据 VMA、
权限、PTE 和 backing type 决定结果：

| 场景 | fault 原因 | 典型处理 |
| --- | --- | --- |
| 匿名页首次写入 | 合法 VMA，尚无私有物理页 | 分配并清零页，安装 PTE |
| `fork` 后首次写入 | COW 页暂时只读 | 复制或独占原页，恢复写权限 |
| file mapping 已在 page cache | 尚未安装当前进程 PTE | 直接映射 resident page |
| file/swap 数据不在 RAM | 需要 backing-store I/O | 读入后安装 PTE |
| 权限不匹配 | 例如写只读映射 | 信号或特定内核处理路径 |
| 地址不属于 VMA | 非法地址 | 通常向进程发送 `SIGSEGV` |

`minor` 和 `major` 的分界是**处理 fault 是否需要 I/O**，不是“匿名还是文件”
或“第一次还是再次访问”。

## 2. 对开发最直接的参考点

### 2.1 先区分 reserve、commit、resident 和 working set

内存问题排查中必须把下面四个量分开：

| 概念 | 含义 | 常见观测 |
| --- | --- | --- |
| Reserved virtual range | 进程拥有的地址范围 | `/proc/<pid>/maps`、`VmSize` |
| Committed promise | 内核允许未来兑现的内存承诺 | `Committed_AS`、`CommitLimit` |
| Resident memory | 当前实际驻留 RAM 的页 | RSS、PSS、`smaps` |
| Hot working set | 时间窗口内真正反复使用的页 | fault、refault、PSI、采样器 |

工程价值在于避免错误容量判断：

- 看到 100 GiB VMA，不应直接认定进程占了 100 GiB RAM；
- `malloc` 成功不代表以后首次触页一定成功，overcommit 下仍可能 OOM；
- RSS 高不一定都是私有成本，PSS 才能按比例分摊共享页；
- page cache 占满 RAM 不一定危险，持续 refault、swap 和 PSI stall 才更接近
  业务受损证据。

**工程判断：** 服务容量模型至少应包含：

```text
private_hot
+ shared_proportional
+ page_tables
+ kernel_or_runtime_overhead
+ reclaim_headroom
<= cgroup_or_host_limit
```

只按对象 payload 或 allocator requested bytes 估算，会遗漏页表、碎片、
page cache、线程栈和运行时保留区。

### 2.2 数据布局会同时改变 cache 与 TLB 成本

原文把顺序数组和随机 hash/pointer access 对比起来，这一点很适合指导实际
数据结构选择。

对 `N` 个对象，粗略页数为：

```text
pages ~= ceil(N * object_stride / page_size)
```

`object_stride` 包含字段、alignment 和 tail padding。因此结构体从 24 bytes
压缩到 16 bytes，不只是节省三分之一内存，还可能同时减少：

- cache line 数；
- 4 KiB page 数；
- TLB entry 需求；
- page-table walk；
- reclaim 扫描与 refault 风险；
- 跨 NUMA node 的实际流量。

可操作建议：

1. 热循环优先使用连续数组、分区数组或 SoA，不默认使用 pointer-rich graph。
2. 按访问频率拆 hot/cold 字段，不只按业务概念组织对象。
3. 用实际 stride 和 page footprint 评估结构体重排，不能只看 `sizeof` 降幅。
4. benchmark 同时记录 cache miss、TLB walk、fault 和 wall time，避免把不同
   层次的退化混为一谈。

### 2.3 `mmap` 与 `read` 是不同成本模型

原文正确指出：buffered `read` 通常从 page cache copy 到用户 buffer，而
file-backed `mmap` 可以让 PTE 直接指向 page-cache page。真正的选择不是
“copy 或 zero-copy”这么简单：

| 维度 | `read` / `pread` | file-backed `mmap` |
| --- | --- | --- |
| 工作触发 | 显式 syscall 和返回值 | 隐式 page fault |
| 用户态副本 | 通常有 | 通常无额外用户 buffer copy |
| 错误模型 | errno、short read | fault、`SIGBUS`、异步 writeback |
| 预取控制 | `readahead`、`posix_fadvise`、请求大小 | fault-around、`madvise` |
| 地址翻译 | 用户 buffer 的既有映射 | 文件页需要额外 PTE/TLB 覆盖 |
| 生命周期 | buffer ownership 明确 | 文件长度与 mapping 生命周期耦合 |
| 并发共享 | 需自行设计 | 同一 page-cache page 可被多进程映射 |

适合 `mmap` 的常见场景：

- 重复随机读取；
- 文件格式天然支持 pointer/offset navigation；
- 多进程需要共享同一文件页；
- working set 能被 page cache 稳定承载。

更倾向显式 I/O 的场景：

- 单次顺序流式扫描；
- 需要显式控制 queue depth、取消、backpressure；
- 文件可能被并发截断；
- 应用已有自己的 cache 与 eviction；
- 需要把 I/O 错误留在普通控制流中。

**关键边界：** `MAP_SHARED` 的“其他进程可见”和“掉电后持久化”是两回事。
需要持久化协议时，必须明确 `msync(MS_SYNC)`、`fsync`、文件系统和设备保证，
不能把一次普通 store 当成 durability barrier。

### 2.4 `fork` 的成本取决于后续写入

COW 让 `fork` 避免立刻复制全部物理页，但仍有以下成本：

- 复制页表和进程元数据；
- 把私有可写映射置于 COW 状态；
- 父子后续写入触发 protection fault；
- 写入导致真实 page copy 和 RSS 增长；
- 大进程、多线程和 huge page 可能放大 pause。

因此“`fork` 很快”应改写成：

> `fork` 避免立即复制全部 payload，但创建和后续写入成本仍随地址空间、
> 页表规模及 dirty pattern 变化。

这对 pre-fork server、snapshot、checkpoint 和 GC runtime 很关键。需要测量
的是 fork latency、子进程生命周期内 COW fault、额外 RSS 和 tail latency，
而不是只测 `fork()` syscall 返回时间。

### 2.5 映射生命周期会制造跨核同步

`munmap`、`mprotect`、page migration 和某些 COW 路径会修改页表。其他 CPU
可能仍缓存旧翻译，因此内核需要执行 TLB invalidation；在多核系统上，这可能
涉及 IPI 和同步等待。

对 allocator、JIT、GC 和高并发 runtime 的建议：

- 不在 hot path 高频创建、改权和销毁小 mapping；
- 大 region 内做用户态 suballocation；
- 缓存已释放 span，批量归还 OS；
- 把 `mprotect` 当同步操作测量，不当作廉价状态位；
- 观察 shootdown 时结合 CPU 数和 process CPU affinity。

这也解释了为什么“及时 `munmap` 降 RSS”不一定是吞吐最优策略：它降低驻留
或地址占用的同时，可能增加 VMA 操作、页表修改和跨核 invalidation。

### 2.6 Huge Page 是容量换翻译效率

Huge Page 的主要长期收益是扩大 TLB reach，并缩短某些 page walk；代价是
更粗粒度的分配、清零、回收和内部碎片。

| 机制 | 优点 | 代价与不确定性 |
| --- | --- | --- |
| THP | 无需固定池，可自动 promotion/demotion | 命中不保证；可能触发 reclaim/compaction |
| Multi-size THP | fault 粒度和延迟更平滑 | 架构、内核版本和配置相关 |
| HugeTLB | 容量和页大小显式、覆盖更可预测 | 预留内存不能作普通页使用；不可 swap |

工程上不能只检查 `/sys/kernel/mm/transparent_hugepage/enabled`。还要确认：

- 目标 VMA 的 `AnonHugePages`、`KernelPageSize`、`MMUPageSize`；
- THP fault、collapse、split 和 compaction 指标；
- tail latency 是否因同步 compaction 恶化；
- cgroup 和 NUMA node 是否真的有可用大页；
- 节省的 TLB walk 是否超过额外内存与初始化成本。

### 2.7 NUMA 把“第一次触碰”变成部署决策

匿名页通常在首次 fault 时按当时 CPU 和有效 memory policy 选择 NUMA node。
如果主线程串行初始化整个 buffer，再交给跨 socket worker，可能把数据集中放到
单个 node。

可操作顺序：

1. 明确 thread-to-data ownership。
2. 固定或至少观测 worker 的 CPU placement。
3. 由最终消费者并行 first-touch 自己的数据分区。
4. 用 `/proc/<pid>/numa_maps` 和 `numastat -p` 验证物理分布。
5. 共享只读数据再比较 local placement、interleave 和 replication。
6. 把 automatic NUMA balancing 当反应式优化，不能替代初始布局验证。

注意：`mbind` 的效果受 mapping 类型、cpuset 和已 fault 页影响。对普通
`MAP_SHARED` file mapping，VMA policy 并不等同于匿名页 policy；不能把
“first-touch”笼统套到所有内存。

## 3. 建议采用的诊断顺序

遇到“内存大、访问慢、偶发卡顿”时，不要从 `malloc` 或 OOM 日志单点猜测。
按下列层次逐步缩小：

```text
Symptom
   |
   v
VMA shape and permissions
   |
   v
RSS, PSS, dirty, and swap
   |
   v
Minor and major faults
   |
   v
Reclaim and PSI stalls
   |
   v
TLB walk and cache misses
   |
   v
NUMA page and CPU placement
```

### 3.1 地址空间

```bash
cat /proc/<pid>/maps
pmap -x <pid>
```

先确认 mapping 数、权限、文件来源和异常巨大 VMA。VMA 大不等于 RSS 大。

### 3.2 驻留与共享

```bash
cat /proc/<pid>/smaps_rollup
cat /proc/<pid>/smaps
```

重点看 `Rss`、`Pss`、`Private_Dirty`、`Shared_Clean`、`Swap`、
`AnonHugePages`、`KernelPageSize` 和 `MMUPageSize`。做多进程容量规划时，
不能把每个进程 RSS 简单相加。

### 3.3 fault 与 I/O

```bash
perf stat -e page-faults,major-faults ./your-program
```

解释原则：

- minor fault 多：可能是正常 first-touch、COW 或 page cache 已命中；
- major fault 多：处理 fault 发生了 I/O，需继续区分 swap 与 file mapping；
- major fault 少但程序仍慢：检查 TLB walk、cache miss、NUMA、锁和调度；
- 只看 fault 总数不能推出性能结论，必须与时间线和 latency 对齐。

### 3.4 系统和 cgroup 压力

```bash
vmstat 1
cat /proc/pressure/memory
cat /sys/fs/cgroup/memory.pressure
```

`vmstat` 的 `si/so` 显示 swap 活动；PSI `some` 表示至少有任务因内存受阻，
`full` 表示所有 non-idle task 同时受阻。容器中优先看对应 cgroup，而不只看
宿主机全局值。

### 3.5 翻译成本

```bash
perf list | grep -i tlb
perf stat -e dTLB-load-misses,dTLB-store-misses ./your-program
```

事件名和语义随 CPU 变化。先用 `perf list` 确认可用事件；如果硬件提供
page-walk cycle/completion 事件，应优先结合它们解释 miss 成本。

### 3.6 NUMA

```bash
numactl --hardware
numastat -p <pid>
cat /proc/<pid>/numa_maps
```

必须同时核对线程在哪些 CPU 上运行、页在哪些 node 上，以及 cpuset/mempolicy。
单看 `numastat` 中 remote 数量，不能排除 IRQ、锁竞争或频率差异。

## 4. 原文需要限定或校正的地方

原文作为心智模型很优秀，但以下细节不宜直接写进生产设计约束：

| 原文简化 | 更精确的工程表述 |
| --- | --- |
| x86-64 使用 48-bit、四级页表 | 这是常见模式，不是通用常量；Linux 通用模型已有五级，架构可折叠层级，x86-64 也可启用 LA57 |
| 用户一半、内核一半且内核映射在每个进程 | 地址布局依架构和内核配置变化；KPTI 等机制也会改变用户态运行时可见页表 |
| `malloc` 通常通过 `brk` 向上增长 heap | allocator 可混用 arena、`brk`、匿名 `mmap`；large allocation、thread arena 和实现阈值都不同 |
| 匿名页首次访问都会分配零页 | 首次 read 可能共享只读 zero page，首次 write 才获得私有页；THP 也会改变粒度 |
| file mapping 首次 fault 是 major fault | 只有需要 I/O 才是 major；页已在 page cache 时，安装 PTE 可以是 minor fault |
| present=0 可概括所有缺页状态 | 非 present entry 的软件编码、file fault 信息和 swap entry 路径不同，不能把硬件位解释成统一数据模型 |
| `mlock` 就是 DMA pin，固定物理地址 | `mlock` 保证页常驻、不进 swap；Linux driver 的 DMA pin 使用 `pin_user_pages*`/FOLL_PIN，并有额外生命周期与 writeback 约束 |
| 每个 core 都有完全独立 TLB | 实际 TLB 层次和共享关系由微架构决定；工程上应说 per-CPU translation state，需要正确失效 |
| 经典 active/inactive LRU 描述当前 reclaim | 这是可用简化；现代内核可能启用 MGLRU，且行为受版本、配置、memcg 和 workload 影响 |
| remote NUMA latency 是固定倍数 | 1.5-3x 只能作量级示意；平台拓扑、缓存命中、带宽拥塞和访问类型都会改变结果 |

其中最值得警惕的是 `mlock == DMA pin`。用户态 `mlock` 的公开契约是让页面
保持 resident；内核文档明确把 DMA page pin 归入 `pin_user_pages*` 和
`FOLL_PIN/FOLL_LONGTERM` 的独立约束。两者不能在设计评审中互换。

## 5. C demo 说明

### 5.1 Demo 目标

demo 不尝试在虚拟机或共享开发机上给出稳定纳秒级 benchmark。它验证四个
可以由程序语义和内核计数器直接观察的事实：

1. 地址可以拆成 virtual page number 与 page offset。
2. 匿名 `mmap` 后，逐页首次写入产生一批 minor fault，并提高 RSS。
3. `fork` 后子进程写 `MAP_PRIVATE` 页面，不会修改父进程看到的值。
4. 子进程写 `MAP_SHARED` 文件映射后，父进程和文件都能看到新值。

执行流程：

```text
Anonymous mmap
      |
      v
Read VmSize, VmRSS, VmPTE
      |
      v
Write one byte per page
      |
      v
Read minor and major faults
      |
      +----------------------+
      |                      |
      v                      v
fork + private map     fork + shared file
      |                      |
      v                      v
Parent unchanged       Parent sees child write
```

### 5.2 输出如何解读

一次 FIL-C 运行中，4 MiB 匿名 mapping 的首次逐页写入观察到 1024 个
minor fault、0 个 major fault；第二次写入观察到 0 个 fault。RSS 在首次
写入后增长约 4 MiB。COW 阶段让子进程逐页写 256 个私有页，并要求至少
出现 256 个 minor fault；实际计数还可能包含 FIL-C runtime 的额外 fault。
父进程的 256 个页必须全部保持原值。

这些数字不是测试断言：

- scheduler、runtime、stdio 和 `/proc` 读取都可能贡献 fault；
- THP 会改变 fault 粒度，所以 demo 使用 `MADV_NOHUGEPAGE`；
- FIL-C runtime 自身预留较大的地址空间，`VmSize` 绝对值及 mapping 前后差值
  不能外推到普通 glibc 程序；
- `ru_minflt`/`ru_majflt` 是进程累计值，demo 只比较阶段差值。

常驻检查不依赖 `assert`，即使定义 `NDEBUG` 也不会失效。它验证：

- first-touch 的 minor fault 为正且多于重复触页；
- first-touch 后每页数据仍符合写入值；
- 256 个 COW 写至少产生 256 个 minor fault，且父进程每页仍保持原值；
- shared mapping 的父进程视图与 file byte 一致。

### 5.3 为什么没有在 demo 中使用 `mincore`

`mincore` 能返回 mapping 每页的 resident snapshot，是很好的原生 Linux
补充观测手段；但 FIL-C 0.684 当前 libc 会拒绝该 syscall。仓库规则不允许
静默换成系统 Clang，因此 demo 保留 FIL-C 可执行路径，改用
`/proc/self/status` 和 `getrusage`。

这也说明工具限制应成为证据的一部分：编译器能接受函数声明，不代表其 runtime
允许对应 syscall。

### 5.4 Demo 没有证明什么

- 没有证明真实 x86-64 页表层数；FIL-C 当前运行在 Linux/ARM64 VM。
- 没有证明 TLB miss latency；需要目标 CPU 的 `perf` 事件。
- 没有证明 THP 或 HugeTLB 生效；需要检查 `smaps` 和 kernel counters。
- 没有制造 major fault；这需要受控冷 file cache 或 swap 环境。
- 没有比较 `mmap` 与 `read` 性能；这必须使用真实文件、访问模式和并发。
- 没有测试 NUMA；单 node VM 无法代表多 socket production topology。
- 没有调用 `msync` 或 `fsync`；共享值可见性不构成掉电持久性证明。

## 6. 从文章到代码评审

评审内存密集型代码时，可以直接问以下问题：

| 类别 | 必须回答 |
| --- | --- |
| Address space | mapping 数量、权限和生命周期是否有上限？ |
| Allocation | reserve、first-touch 和 RSS 峰值分别发生在哪个阶段？ |
| Layout | 每次业务操作触碰多少 cache line 和 page？ |
| Fault | fault 是 zero-fill、COW、page-cache hit、file I/O 还是 swap-in？ |
| File I/O | `mmap` 的 truncation、`SIGBUS`、writeback 和 durability 如何处理？ |
| Concurrency | `fork`、`munmap`、`mprotect` 是否在 latency-sensitive path？ |
| Huge page | 需要 THP 的 opportunistic 行为，还是 HugeTLB 的确定性？ |
| NUMA | 谁 first-touch，谁消费，线程是否会跨 node 迁移？ |
| Capacity | PSS、page table、cache、allocator 和 headroom 是否都计入？ |
| Evidence | 指标能否区分 translation、residency、reclaim 和 topology？ |

## 7. 推荐的落地实验

文章读完后，最有价值的后续不是继续背术语，而是建立目标 workload 的证据：

1. **Working-set sweep**：固定算法，逐步增大数据量，跨越 LLC、TLB reach、
   RAM 和 cgroup limit，记录拐点。
2. **Layout A/B**：AoS、字段重排、SoA 使用相同数据和访问序列，对比 bytes、
   cache miss、TLB walk 和 wall time。
3. **First-touch A/B**：单线程初始化与 worker-local 并行初始化，对比
   `numa_maps`、remote access 和 p99。
4. **Page-size A/B**：base page、THP madvise、HugeTLB 分开测，记录实际
   page size，不以配置值代替生效证据。
5. **Mapping churn A/B**：频繁 `mmap/munmap` 与 arena reuse 对比 syscall、
   shootdown、RSS 和 tail latency。
6. **File path A/B**：`pread`、`mmap`、direct I/O 在 cold/warm、顺序/随机、
   单进程/多进程下分别验证。
7. **Pressure test**：在受控 cgroup 内逐步收紧 memory，观察 reclaim、PSI、
   major fault、swap 和 OOM 行为。

每个实验必须同时保留 workload identity：CPU、kernel、page size、THP、
NUMA、cgroup、文件系统、数据量、访问分布和并发数。否则结果不可复现，也
无法解释为什么另一台机器得出相反结论。

## 8. 总体评价

| 维度 | 评价 |
| --- | --- |
| 心智模型 | 很强；把多个 VM 机制串成连续因果链 |
| 开发实用性 | 很高；尤其适合数据库、runtime、缓存和高并发服务 |
| Linux 观测 | 很实用；`maps`、`smaps`、fault、PSI、TLB、NUMA 顺序合理 |
| 架构精度 | 需要读者主动限定 x86-64、页表层级和 microarchitecture |
| reclaim 精度 | 教学级；不能代替目标 kernel 的实现与配置核对 |
| 性能结论 | 是假设生成器，不是 benchmark 结论 |
| 安全边界 | W^X、COW、zero-fill 主线正确；`mlock` 与 DMA pin 需要校正 |

最终建议是：把这篇文章当作**问题分层框架**，不要当作“某个 syscall 或页大小
一定更快”的处方。它真正提升开发质量的方式，是让设计、压测和故障分析都能
回答同一个问题：

> 当前代价发生在地址空间、驻留、翻译、回收还是物理拓扑哪一层，现有证据
> 能否排除其他层？

## 9. 参考资料

- [原文：Virtual Memory From First Principles](https://blog.codingconfessions.com/p/virtual-memory)
- [Linux Page Tables](https://docs.kernel.org/mm/page_tables.html)
- [Linux Transparent Hugepage Support](https://docs.kernel.org/admin-guide/mm/transhuge.html)
- [Linux HugeTLB Pages](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html)
- [Linux NUMA Memory Policy](https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html)
- [Linux PSI](https://docs.kernel.org/accounting/psi.html)
- [`mmap(2)`](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [`mlock(2)`](https://man7.org/linux/man-pages/man2/mlock.2.html)
- [`mincore(2)`](https://man7.org/linux/man-pages/man2/mincore.2.html)
- [`getrusage(2)`](https://man7.org/linux/man-pages/man2/getrusage.2.html)
- [`proc_pid_smaps(5)`](https://man7.org/linux/man-pages/man5/proc_pid_smaps.5.html)
- [Linux `pin_user_pages()`](https://docs.kernel.org/core-api/pin_user_pages.html)
