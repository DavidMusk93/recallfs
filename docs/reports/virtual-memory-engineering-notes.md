# 《Virtual Memory From First Principles》开发者工程评析

> 原文：Abhinav Upadhyay, *Virtual Memory From First Principles*，
> 2026-05-11。
>
> 原文链接：
> [https://blog.codingconfessions.com/p/virtual-memory](https://blog.codingconfessions.com/p/virtual-memory)
>
> 阅读日期：2026-09-07。本文以原文建立主线，再用 Linux 官方文档和
> man-pages 校正实现边界。文中的“工程判断”不是原文结论。

配套 C 程序分成正确性和性能两层：

| 文件 | 角色 | 运行位置 |
| --- | --- | --- |
| [`virtual_memory_demo.c`](../../tools/docs/examples/virtual_memory_demo.c) | 权限、按需分页、COW、共享映射的常驻正确性检查 | FIL-C；目标 Linux 原生复验 |
| [`virtual_memory_benchmark.c`](../../tools/docs/examples/virtual_memory_benchmark.c) | fault、页大小、NUMA、COW、`mprotect` benchmark | FIL-C 小规模 selftest；目标机原生 benchmark |
| [`virtual_memory_file_benchmark.c`](../../tools/docs/examples/virtual_memory_file_benchmark.c) | cold/warm `pread` 与 `mmap` 文件读取 | FIL-C 小规模 selftest；目标机原生 benchmark |

FIL-C 0.684 只作为功能正确性、内存安全和未定义行为边界门禁，不作为运行时
性能基线。正确性复验：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_demo.c \
  -o .tmp/virtual-memory-demo
.tmp/fil-c/bin/filrun .tmp/virtual-memory-demo

.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  tools/docs/examples/virtual_memory_benchmark.c \
  -o .tmp/virtual-memory-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-benchmark-filc selftest

.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_file_benchmark.c \
  -o .tmp/virtual-memory-file-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-file-benchmark-filc selftest
```

性能数据来自目标机 `dc02-pe-t137-n047`（IPv6
`fdbd:dc02:e:137::47`）上的原生 GCC 12.2.0 构建，完整环境、命令、结果和
限制见第 6 节。

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

correctness demo 不尝试在 FIL-C VM 中给出性能结论。它验证五个
可以由程序语义和内核计数器直接观察的事实：

1. 地址可以拆成 virtual page number 与 page offset。
2. `mprotect(PROT_READ)` 后写入被硬件权限阻止；恢复写权限后可以再次写入。
3. 匿名 `mmap` 后，逐页首次写入产生一批 minor fault，并提高 RSS。
4. `fork` 后子进程写 `MAP_PRIVATE` 页面，不会修改父进程看到的值。
5. 子进程写 `MAP_SHARED` 文件映射后，父进程和文件都能看到新值。

执行流程：

```text
Anonymous mmap
      |
      v
Apply page permissions
      |
      v
Read VmSize, VmRSS, VmPTE
      |
      v
Write one byte per page
      |
      v
Read minor and major faults
      |                      |
      +----------------------+
      |                      |
      v                      v
fork + private map     fork + shared file
      |                      |
      v                      v
Parent unchanged       Parent sees child write
```

### 5.2 输出如何解读

一次 FIL-C 正确性运行中，read-only 页的子进程写入由信号 11 终止；4 MiB
匿名 mapping 的首次逐页写入观察到 1024 个
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
- read-only 写入必须由信号终止，恢复写权限后写入必须成功；
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

## 6. 目标机 C 实验：把机制变成可观察证据

### 6.1 结论先行

在 `fdbd:dc02:e:137::47` 上使用原生优化 C 程序后，原文中的主要开发参考点
可以落到以下实测：

| 要点 | C 实验 | 目标机中位结果 | 工程含义 |
| --- | --- | ---: | --- |
| reserve 不等于 resident | 1 GiB 匿名 `mmap`，先不触页 | reserve `3.709 us` | 建立 VMA 很便宜，不等于 RAM 已就绪 |
| first-touch 有真实成本 | 首次/再次逐 4 KiB 写同一 1 GiB | `207.801 / 3.222 ms` | 首次触页比重写慢 `64.49x` |
| 权限由硬件执行 | read-only 页中执行写入 | `SIGSEGV(11)` | `mprotect` 不是元数据提示，而是访问契约 |
| 顺序访问更友好 | 每页读 1 byte，顺序/随机遍历 1 GiB | `11.078 / 14.746 ns` | 随机页访问慢 `1.33x` |
| Huge Page 不是万能加速 | random base/THP/HugeTLB | `14.746 / 14.174 / 14.187 ns` | 本负载只改善约 `4%`，但显著减少初始化 fault |
| COW 把复制推迟到写入 | 512 MiB，空 child 与逐页写 child | `4.534 / 145.314 ms` | 写入路径触发 131072 个额外 minor fault |
| `mmap` 不是总比 `pread` 快 | 2 GiB 本地 NVMe cold/warm 扫描 | cold `2.805 / 3.548 GiB/s` | cold `mmap` 反而比 `pread` 低 `20.94%` |
| 页表修改有跨核成本 | 64 MiB 上反复 `mprotect` | `165 -> 4090 us` | 1 到 64 worker 时单次切换中位成本扩大 `24.81x` |
| NUMA 拓扑不能被指针隐藏 | CPU node3/node2/node0 读 node3 RAM | `23.015/20.187/13.110 GiB/s` | 跨 socket 带宽下降 `43.04%` |

这些是该机器、该内核、该编译器和该访问模式的证据。它们支持机制判断，
不能直接变成所有服务的固定常数。

### 6.2 目标机与控制变量

| 项目 | 值 |
| --- | --- |
| Host | `dc02-pe-t137-n047` |
| IPv6 | `fdbd:dc02:e:137::47` |
| Kernel | Linux `5.15.152.bsk.15-amd64` |
| CPU | 2 sockets, AMD EPYC 7Y83, 64 cores/socket, SMT2 |
| Logical CPU | 256 |
| NUMA | 4 nodes，每 node 约 512 GiB |
| RAM | 2.0 TiB；正式测试前 `MemAvailable` 约 1.8 TiB |
| Swap | 0 |
| Base page | 4 KiB |
| THP | `madvise` |
| HugeTLB | 2 MiB；正式测试时全机 1103 页空闲 |
| Frequency governor | `performance` |
| Compiler | GCC 12.2.0 |
| Native flags | `-O3 -march=native -mtune=native -DNDEBUG -flto` |
| Repeat count | 每个正式 case 7 次，报告中位数与 `[min, max]` |

CPU/内存绑定：

| 用途 | CPU | CPU node | Memory node | NUMA distance |
| --- | ---: | ---: | ---: | ---: |
| local | 96 | 3 | 3 | 10 |
| same-socket neighbor | 64 | 2 | 3 | 12 |
| cross-socket remote | 1 | 0 | 3 | 32 |
| shootdown | `96-127,224-255` | 3 | 3 | 10 |

所有 NUMA benchmark 都通过 `numactl --physcpubind=... --membind=3`
固定 CPU 和物理页来源，并用 `/proc/self/numa_maps` 验证实际页分布。正式
实验中 node3 的 2 GiB mapping 均显示 `N3=524288`，即 524288 个 4 KiB
页全部位于 node3。

机器是共享生产节点，不是隔离 benchmark server。测试时 host 总体约
`96-98% idle`，但仍有业务进程运行。因此：

- 报告 7 次范围，不隐藏 jitter；
- 使用每组中位数，不挑最好一次；
- 单次 mapping 最大 2 GiB，未接近 node3 可用内存；
- 不执行 swap、OOM、direct reclaim 或内存压力注入；
- 不修改 THP、HugeTLB、NUMA balancing 等持久配置。

### 6.3 正确性门禁与性能工具链

三个 C 文件先通过 FIL-C：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_demo.c \
  -o .tmp/virtual-memory-demo
.tmp/fil-c/bin/filrun .tmp/virtual-memory-demo

.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  tools/docs/examples/virtual_memory_benchmark.c \
  -o .tmp/virtual-memory-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-benchmark-filc selftest

.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_file_benchmark.c \
  -o .tmp/virtual-memory-file-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-file-benchmark-filc selftest
```

FIL-C 的输出只证明小规模路径满足程序检查，没有内存安全错误；其 VM、安全
插桩和耗时不参与下文任何性能比较。

目标机使用原生编译器：

```bash
gcc -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror \
  virtual_memory_demo.c -o virtual_memory_demo
numactl --physcpubind=96 --membind=3 ./virtual_memory_demo

gcc -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror -pthread \
  virtual_memory_benchmark.c -o virtual_memory_benchmark

gcc -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror \
  virtual_memory_file_benchmark.c -o virtual_memory_file_benchmark
```

上传到目标机的源码 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `virtual_memory_benchmark.c` | `ed5bbf41e50fc5b26d19b741e93819961fa2f35fc4af39f7e2dc3b946c3b495d` |
| `virtual_memory_file_benchmark.c` | `cf6a532b8416d664fcec9994563367ad761540dc447a9c2682c777325190fd5e` |

### 6.4 计数器边界：什么能测，什么不能测

目标机通用 core PMU 事件 `cycles`、`instructions`、`dTLB-load-misses` 和
`cache-misses` 都能被 `perf_event_open` 打开，但 `time_running=0`，结果为
`<not counted>`。即使短暂关闭 NMI watchdog，结果仍然如此；测试结束后
watchdog 已恢复为 `1`。

因此本文没有伪造 TLB miss 数字，也没有用 wall time 反推“精确 miss 次数”。
实际可用证据是：

| 证据 | 用途 |
| --- | --- |
| `CLOCK_MONOTONIC` wall time | 每轮延迟与吞吐 |
| `getrusage` / perf software fault | minor/major fault |
| `/proc/self/status` | `VmSize`、`VmRSS`、`VmPTE` |
| `/proc/self/smaps` | `AnonHugePages`、`Private_Hugetlb`、page size |
| `/proc/self/numa_maps` | 物理页所在 NUMA node |
| `tlb:tlb_flush` tracepoint | 页表权限变化时的 flush 活动 |
| `msr/aperf/`、`msr/mperf/`、`msr/tsc/` | 频率状态交叉检查 |

正式 case 的 `APERF/MPERF` 比值均约 `1.3265`。按 BIOS 报告的 2.4 GHz
基频估算，运行频率约 3.18 GHz；更重要的是各 case 比值一致，NUMA 和页大小
差异不是由某一组降频造成。

### 6.5 页权限：非法写入不是普通错误返回

`virtual_memory_demo.c::demonstrate_memory_protection()`：

1. 创建一页 anonymous read-write mapping；
2. 写入 `0x5a`；
3. 调用 `mprotect(PROT_READ)`；
4. 子进程再次写入；
5. 父进程验证 child 由信号终止，原值未变；
6. 恢复 `PROT_READ | PROT_WRITE` 并验证写入成功。

目标机原生结果：

```text
[2] Page permission enforcement
  read-only child write terminated by signal=11
```

这说明 PTE permission 是同步硬件契约。与 `read()` 返回 `EACCES` 不同，
普通 load/store 没有 errno 返回点；非法访问通常通过 `SIGSEGV` 离开正常
控制流。JIT、guard page、sandbox 和 allocator quarantine 都必须按信号和
进程生命周期设计，不能把 `mprotect` 当作普通业务校验。

### 6.6 reserve、resident、PTE 与 first-touch

实验：

```bash
perf stat -e msr/aperf/,msr/mperf/,msr/tsc/,\
page-faults,minor-faults,major-faults -- \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark fault 1024 7
```

程序每轮：

1. `mmap` 1 GiB base-page anonymous range；
2. 记录 `VmSize`、`VmRSS`、`VmPTE`；
3. 每 4 KiB 写 1 byte；
4. 再写一次相同页；
5. `munmap` 后重复。

| 指标 | 中位数 | 范围 |
| --- | ---: | ---: |
| reserve | `3.709 us` | `[2.524, 5.338]` |
| first-touch 1 GiB | `207.801 ms` | `[206.250, 216.932]` |
| second-touch 1 GiB | `3.222 ms` | `[3.194, 3.248]` |
| first-touch minor fault | 262144 | 每轮相同 |
| second-touch fault | 0 | 每轮相同 |
| first/second time ratio | `64.49x` | 由中位数计算 |

内存状态的典型稳定轮次：

| 时点 | `VmSize` | `VmRSS` | `VmPTE` |
| --- | ---: | ---: | ---: |
| mapping 前 | 2476 KiB | 约 1.5 MiB | 40 KiB |
| `mmap` 后、未触页 | 1051052 KiB | 约 1.5 MiB | 40 KiB |
| first-touch 后 | 1051052 KiB | 1049852 KiB | 2092 KiB |

结论：

- `mmap` 先增加 1 GiB `VmSize`，RSS 和 PTE 几乎不变；
- 262144 次 first-touch 恰好对应 1 GiB / 4 KiB；
- first-touch 同时承担 frame allocation、zero-fill、PTE 安装和 fault
  处理；
- PTE 从 44 KiB 增至约 2 MiB，说明 page table 本身也要计入容量；
- 再次写入没有 fault，只剩内存写路径。

所以 latency-sensitive 服务若在请求路径首次触碰大 arena，`malloc/mmap`
发生在启动期也不能消除 tail latency。需要在正确 NUMA node 上 pre-touch，
或显式把 first-touch 算入请求预算。

### 6.7 访问顺序、TLB footprint 与 Huge Page

实验每次创建 1 GiB mapping、完成 first-touch，然后每轮读取每个 4 KiB 页的
一个 byte，共 64 pass。所有正式测量轮次都没有 major fault，除首轮极少量
runtime noise 外 hot loop 为 0 fault。

| Mapping | Access | 实际页覆盖证据 | 中位 `ns/access` | 范围 |
| --- | --- | --- | ---: | ---: |
| base | sequential | 4 KiB，`AnonHugePages=0` | 11.078 | `[11.060, 11.096]` |
| base | random | 4 KiB，`AnonHugePages=0` | 14.746 | `[14.720, 14.784]` |
| THP | random | `AnonHugePages=1046528 KiB` | 14.174 | `[14.154, 14.238]` |
| HugeTLB | random | `Private_Hugetlb=1048576 KiB`，2 MiB MMU page | 14.187 | `[14.167, 14.253]` |

顺序 base page 比随机 base page 快 `1.33x`。这个差异同时包含：

- 顺序页号对 TLB 与 page-walk cache 更友好；
- 顺序 cache-line stream 更容易被硬件预取；
- 随机顺序破坏下一页预测。

由于 core PMU 的 dTLB 事件不可计数，不能把全部差异归因于 TLB。

Huge Page 结果同样要克制解释：

- THP 相对 base random 改善 `3.88%`；
- HugeTLB 相对 base random 改善 `3.79%`；
- THP 实际覆盖 1022 MiB，不是只检查 sysfs 后假设生效；
- HugeTLB 实际使用 512 个 2 MiB 页；
- 整个进程初始化期的 perf page fault 从 base 的 262529 降至 THP 的
  1409、HugeTLB 的 898。

Huge Page 对该 hot random-per-page loop 只有约 3% 收益，却显著减少 fault
和页表粒度。这说明它更像一个有适用条件的容量/翻译优化，不是“启用后必然
快很多”。若 workload 主要是 cache miss、内存带宽或计算瓶颈，减少 TLB
miss 也可能只带来小幅端到端变化。

### 6.8 COW：`fork` 快，不代表 child 写入也快

实验：

```bash
perf stat -e msr/aperf/,msr/mperf/,msr/tsc/,\
page-faults,minor-faults,major-faults -- \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark cow 512 7
```

父进程先触碰 512 MiB 的 131072 个 base page。每轮分别：

- fork 一个不写内存的 child；
- fork 另一个逐页写全部 512 MiB 的 child；
- 用 `wait4` 取得 child fault；
- 验证父进程每页仍保持原值。

| 路径 | 中位时间 | 范围 | child minor fault |
| --- | ---: | ---: | ---: |
| fork + empty child | `4.534 ms` | `[4.471, 4.902]` | 17 |
| fork + write 512 MiB | `145.314 ms` | `[144.705, 149.115]` | 131089 |
| COW 增量 | `32.05x` | 中位时间比 | 131072 |

131072 个额外 minor fault 恰好等于 512 MiB / 4 KiB。COW 没有消灭复制，
只是把它从 `fork` 移到首次写入。工程上应同时问：

- child 是否很快 `exec`，几乎不写旧地址空间；
- snapshot/checkpoint child 会 dirty 多少页；
- parent 是否在 child 存活时继续高频写；
- THP split、page copy 和 RSS 峰值是否落在 tail-latency 窗口。

### 6.9 `pread` 与 `mmap`：cold fault-driven I/O 可能更慢

文件实验使用 node3 本地 NVMe `nvme0n1` 上的 `/data27`，构造 2 GiB 文件。
两个路径都读取完整文件，并每 64 byte 采样一个 byte 计算相同 checksum。

- `pread`：1 MiB user buffer，显式循环读取；
- `mmap`：`MAP_PRIVATE + MADV_SEQUENTIAL`，直接 load；
- cold：每轮前 `posix_fadvise(POSIX_FADV_DONTNEED)`；
- warm：先完整 warm-up，再测 7 轮。

测试数据文件在实验后已删除。

`POSIX_FADV_DONTNEED` 是 eviction hint，不是强制 cache flush。这里把它与
每轮一致的 8193 个 `mmap` major fault、稳定的 cold 吞吐共同作为 cold
证据；不能仅凭调用成功就宣称 page cache 已清空。

| 路径 | Cache | 中位 GiB/s | 范围 | 每轮 fault 特征 |
| --- | --- | ---: | ---: | --- |
| `pread` | cold | 3.548 | `[3.508, 3.582]` | 0 major |
| `mmap` | cold | 2.805 | `[2.792, 2.828]` | 8193 major + 32768 minor |
| `pread` | warm | 11.300 | `[8.823, 11.403]` | 0 major |
| `mmap` | warm | 11.014 | `[10.991, 11.028]` | 0 major + 32769 minor |

cold `mmap` 吞吐比 cold `pread` 低 `20.94%`。这不是说 NVMe 变慢了，而是
I/O 通过 page fault 进入用户指令关键路径：

- 8193 个 major fault 负责触发磁盘读取和 fault-around/readahead；
- 32768 个 minor fault 负责其余 resident page 的 PTE 安装；
- 每次 fault 都包含用户态/内核态切换与 VMA/PTE 处理。

`pread` 的磁盘等待发生在 syscall 内，因此 `ru_majflt=0` 不代表没有 I/O；
major-fault 只统计 page-fault 路径。warm 时两者接近，`mmap` 仍需为每次新
mapping 安装 PTE，而 `pread` 支付 page-cache 到用户 buffer 的 copy。

因此只说“`mmap` 少一次 copy”不足以选型。要在真实 cold/warm 比例、文件复用、
并发截断风险、错误处理和 cache ownership 下比较。

### 6.10 TLB shootdown：页表修改会放大为跨核工作

`virtual_memory_benchmark.c::run_mprotect()`：

1. 创建并 first-touch 64 MiB base-page mapping；
2. 在固定 CPU 上启动 1/8/32/64 个 reader；
3. 每个 reader 持续访问分配给自己的页；
4. 主线程执行 100 轮 read-only/read-write，合计 200 次权限切换；
5. 通过 `tlb:tlb_flush` 统计进程内 flush 活动。

| Reader | 单次 transition 中位 | 范围 | `tlb_flush` 中位 |
| ---: | ---: | ---: | ---: |
| 1 | `164.818 us` | `[164.333, 288.841]` | 637 |
| 8 | `375.532 us` | `[346.841, 388.057]` | 2112 |
| 32 | `639.872 us` | `[545.027, 663.424]` | 5123 |
| 64 | `4089.599 us` | `[1736.877, 6423.415]` | 11960 |

64 reader 的中位成本是 1 reader 的 `24.81x`。tracepoint 数不是“IPI 数”或
“每个 mprotect 精确 flush 几次”的直接同义词；它还包含 range flush 的内部
拆分和线程生命周期相关活动。但 wall time 与 flush 活动都随参与 CPU 增长，
支持以下工程结论：

> 页表权限和映射修改不是本地变量写入。在多核进程中，它可能强制其他 CPU
> 丢弃 translation state，并让调用者等待同步完成。

所以 allocator 不应为每个小对象频繁 `mmap/munmap`，JIT 不应在 hot path
反复切换大范围 W/X，GC 和 runtime 应批量处理 mapping/protection 变化。

### 6.11 NUMA：同一个虚拟指针有不同物理成本

实验固定 2 GiB mapping 全部位于 node3，单线程顺序读取全部 `uint64_t`：

| CPU | CPU node | Memory node | Distance | 中位 GiB/s | 范围 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 96 | 3 | 3 | 10 | 23.015 | `[22.839, 23.085]` |
| 64 | 2 | 3 | 12 | 20.187 | `[19.719, 20.313]` |
| 1 | 0 | 3 | 32 | 13.110 | `[12.977, 13.181]` |

相对 local：

- 同 socket 邻近 node 下降 `12.29%`；
- 跨 socket node 下降 `43.04%`；
- local 带宽是 cross-socket remote 的 `1.76x`。

三组 `APERF/MPERF` 都约 `1.3265`，并且 `numa_maps` 都证明物理页仍在 N3，
所以差异不是数据迁移或频率变化造成。

这直接解释“相同线程逻辑、相同 pointer、不同 worker 速度”的一种常见根因。
正确流程不是先开 automatic NUMA balancing，而是：

1. 确认 CPU affinity；
2. 确认 first-touch 或 `mbind` 后的 page placement；
3. 按 thread-to-data ownership 分区；
4. 再决定 local、interleave 或 replication。

### 6.12 没有在目标机做的实验

以下原文要点没有在该生产节点强行制造：

| 未执行项 | 原因 | 正确验证环境 |
| --- | --- | --- |
| swap-in/swap-out | 机器没有 swap | 隔离 VM 或专用 benchmark host |
| direct reclaim/thrashing | 会干扰同机业务 | 独立 cgroup + 可控 memory.max |
| OOM/overcommit 失败 | 可能杀死无关进程 | 隔离 namespace/VM |
| MGLRU reclaim 对比 | 目标 kernel/config 未提供可确认开关 | 两套可控 kernel |
| 持久化语义 | `MAP_SHARED` 可见性不等于 durability | 专用文件系统 crash test |
| 精确 dTLB miss | core PMU 在该内核上不计数 | 修复 PMU 环境或专用 perf host |

这类实验不能因为“文章提到了”就在生产节点执行。文档中的未验证边界必须保留，
不能用机制常识冒充目标机证据。

### 6.13 可复现命令

典型内存/页大小命令：

```bash
numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark fault 1024 7

numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark walk random base 1024 64 7

numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark walk random thp 1024 64 7

numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark walk random hugetlb 1024 64 7
```

COW、NUMA 和 shootdown：

```bash
numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark cow 512 7

numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark bandwidth base 2048 7
numactl --physcpubind=64 --membind=3 \
  ./virtual_memory_benchmark bandwidth base 2048 7
numactl --physcpubind=1 --membind=3 \
  ./virtual_memory_benchmark bandwidth base 2048 7

numactl --physcpubind=96-127,224-255 --membind=3 \
  ./virtual_memory_benchmark mprotect 64 64 100
```

文件 I/O：

```bash
numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_file_benchmark prepare /data27/.tmp/vm.bin 2048

numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_file_benchmark read pread cold /data27/.tmp/vm.bin 7
numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_file_benchmark read mmap cold /data27/.tmp/vm.bin 7
```

目标机测试目录为 `/root/.tmp/recallfs-virtual-memory-20260907/`；2 GiB
benchmark data 已删除。所有正式命令都可以额外由 `perf stat` 包裹，使用
本机实际可计数的 MSR/software/tracepoint events。

## 7. 从文章到代码评审

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

## 8. 推荐的落地实验

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

## 9. 总体评价

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

## 10. 参考资料

- [正确性 C demo](../../tools/docs/examples/virtual_memory_demo.c)
- [内存与 NUMA C benchmark](../../tools/docs/examples/virtual_memory_benchmark.c)
- [文件映射 C benchmark](../../tools/docs/examples/virtual_memory_file_benchmark.c)
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
