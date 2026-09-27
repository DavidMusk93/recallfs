---
doc_id: recallfs-study-lock-free-ring-buffer-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260927-lock-free-ring-buffer
depends_on:
  - recallfs-agent-ready-docs-v1
  - recallfs-source-lock-free-ring-buffer-study-v1
supersedes: []
verified_by:
  - SPSC-RA-1
  - SPSC-RA-2
  - SPSC-RA-3
  - SPSC-RA-4
  - SPSC-RA-5
---

# Lock-Free SPSC Ring Buffer 深度学习

> 原文：<https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/>
>
> 上游代码：`CppPlayground@74ddbf92cd96d9bdb31a554cb5024fde4bb751bc`
>
> 本地复现：C11、FIL-C 0.684、Zig 0.16.0、Apple M1 Pro

## Contract

### Decision

文章真正有效的核心不是“lock-free 必然比 mutex 快 25 倍”，而是三个逐层收窄
的机制：

1. 用 SPSC ownership 消除对同一索引的多写竞争；
2. 用 release/acquire 只建立 payload 发布与槽位回收所需的 happens-before；
3. 缓存对端索引，把跨核读取从“每次操作”降为“本地观察到可能满/空时”。

本机复现支持第 3 点：在正确 128-byte 隔离下，cached 相比
acquire/release 的中位吞吐提高 19.3%。本机没有复现文章的总体排序：
mutex 仍比 cached 高 65.5%，acquire/release 也没有胜过 seq_cst。结论必须绑定
ISA、runtime、线程放置和队列占用分布，不能搬运 305M ops/s。

### Scope

- 固定容量、保留一个空槽的 `uint64_t` SPSC FIFO；
- mutex、seq_cst、acquire/release、peer-index cache 四种实现；
- 内存序、cache-line 布局、ownership、进度与 benchmark 语义；
- FIL-C、ASan/UBSan、TSan、AArch64 codegen 和本机 wall-time 证据。

### Non-Goals

- 不声称 production ready；
- 不支持 MPSC、SPMC 或 MPMC；
- 不提供 blocking wait、overwrite、batch API 或 variable-sized payload；
- 不把 FIL-C 或 sanitizer 时间当作性能；
- 不把未固定 CPU、没有 PMU 的 macOS 结果外推到作者的 Intel 主机；
- 不实现文章脚注中的 power-of-two mask，因为实际热路径没有 `%` 证据。

### Ownership And Invariants

```text
Producer endpoint                   Consumer endpoint
  owns cached_tail                    owns cached_head
  writes payload                      reads payload
  release-stores head                 release-stores tail
          |                                  ^
          v                                  |
       shared head ---- acquire load ---- consumer
       shared tail ---- acquire load ---- producer
```

- 只有 producer 写 `head`，只有 consumer 写 `tail`。
- producer 在 release-store `head` 前写完 payload；consumer acquire-load
  观察到新 `head` 后才能读 payload。
- consumer 在 release-store `tail` 前读完 payload；producer acquire-load
  观察到新 `tail` 后才能覆盖该槽。
- 自己写的索引可 relaxed-load，因为不存在第二个 writer。
- cached peer index 只能比真实进度旧。它可能导致保守的 false full/empty，
  但到达缓存边界时必须 acquire-refresh 后再次检查。
- `head == tail` 表示空；`next(head) == tail` 表示满；实际容量为 `N - 1`。
- ring、producer endpoint、consumer endpoint 都不得在并发运行时搬移、重绑或
  销毁。

### Failure Semantics

| Situation | Behavior |
| --- | --- |
| Capacity `< 2` | 创建失败 |
| Allocation size overflow | 创建失败 |
| Allocation or mutex initialization failure | 创建失败，不泄漏 |
| Atomic `size_t` not lock-free | atomic ring 创建失败 |
| Full | `try_push` 返回 `false`，不覆盖 |
| Empty | `try_pop` 返回 `false`，不修改队列 |
| More than one producer/consumer | Contract violation; behavior unsupported |

### Worked Examples

**SPSC-WE-1: stale consumer index.** Producer 缓存的 `tail=7`，consumer 已推进到
`tail=12`。只要 `next(head) != 7`，producer 仍在上次确认的安全区间内；到达 7
时刷新真实 tail。旧值最多提前报告 full，不会越过未消费槽。

**SPSC-WE-2: publication.** Producer 先写 `slots[head]`，再 release-store 新
`head`。Consumer 的 acquire-load 读到该值后，payload write happens-before
payload read。把两侧都改成 relaxed 会失去这条跨线程顺序。

**SPSC-WE-3: 64-byte padding on M1 Pro.** 本机 cache line 是 128 bytes。
当编译配置为 64 时，运行时检查在 9/9 轮中都确认 head/tail 同线；配置为 128
时 9/9 均分线。类型成员写着 `alignas(64)` 并不等于避免了 false sharing。

### Reconciliation Anchors

| ID | Claim | Result | Evidence |
| --- | --- | --- | --- |
| `SPSC-RA-1` | 四种实现保持 FIFO、满/空与 wrap 语义 | 14/14 suites pass | [`evidence/verification.txt`](evidence/verification.txt) |
| `SPSC-RA-2` | C 执行路径先经过 FIL-C | FIL-C 0.684 test + benchmark self-check pass | [`evidence/verification.txt`](evidence/verification.txt) |
| `SPSC-RA-3` | 并发路径无动态 sanitizer 报告 | ASan/UBSan + TSan pass | [`evidence/verification.txt`](evidence/verification.txt) |
| `SPSC-RA-4` | 最终 ARM64 binary 实现预期内存序 | `ldar`/`ldapr`/`stlr` 与 conditional peer load 可见 | [`evidence/codegen.txt`](evidence/codegen.txt) |
| `SPSC-RA-5` | 本机性能结论有原始样本 | 100M x 5 主实验；9 对 alignment A/B | [`evidence/`](evidence/README.md) |

## 1. 文章优化了什么

| 阶段 | 删除的成本 | 新增约束 | 文章报告 |
| --- | --- | --- | ---: |
| Mutex -> seq_cst atomic | lock acquire/release 与阻塞路径 | 严格 SPSC；原子必须 lock-free | 12M -> 35M |
| seq_cst -> acquire/release | 不必要的全序约束 | 精确维护 payload/slot happens-before | 35M -> 108M |
| acquire/release -> cached peer | 每次操作都读对端索引 | 缓存只归本端线程，边界时 refresh | 108M -> 305M |

V3 同时引入了 atomics 和 cache-line alignment，所以文章并未独立量化
“去锁”和“消除 false sharing”各自贡献。V5 又同时改变共享读取频率和对象布局。
这些数字是 end-to-end 组合结果，不是单变量成本表。

## 2. 为什么内存序成立

Producer 的 publication edge：

```text
write slots[h]
    |
    v
head.store(next, release)
    |
    | synchronizes-with
    v
head.load(acquire) == next
    |
    v
read slots[h]
```

Consumer 的 reclamation edge 对称：先读槽，再 release-store `tail`；producer
acquire-load 新 `tail` 后才复用槽。这里 acquire 不只是“看到一个数字”，还要
阻止 payload read 或 overwrite 穿过 ownership handoff。

seq_cst 比这两条有向边更强，但“更强”不等于在每个 ISA 上都多一条昂贵指令。
本机 ARM64 codegen 中，两者的 publication 都是 `stlr`；差异主要是 owner load
从 `ldar` 变为 `ldr`。因此文章在 Intel 上的 3 倍增益不能作为跨架构规律。

## 3. 本机复现结果

### 3.1 与文章同规模

100,000 个物理槽、100,000,000 次传输、5 个轮换样本、128-byte 隔离：

| Variant | 本机中位 transfers/s | 相邻阶段变化 | 文章报告 |
| --- | ---: | ---: | ---: |
| mutex | 30.895M | baseline | 12M ops/s |
| seq_cst | 16.447M | -46.76% | 35M ops/s |
| acquire/release | 15.651M | -4.84% | 108M ops/s |
| cached | 18.670M | +19.29% | 305M ops/s |

原文的 `SetItemsProcessed(num_iters)` 把一对 push+pop 记为一个 item，所以本文
统一写 `transfers/s`；若按成功 API 调用数计，本机 cached 为 37.340M calls/s。

本机 mutex 获胜不等于 lock-free 设计无效。这个 workload 下 atomic consumer
经常追上 producer：五轮 acquire/release 合计约 61.0 亿次 empty retry，
cached 降到约 31.0 亿次；mutex 则可能通过 Darwin 的实现和调度形成更粗粒度的
运行批次。这些只是解释候选，缺少 CPU pinning 和 PMU，不能升级为根因。

### 3.2 Cache-Line A/B

9 轮交替先后顺序，每轮 10,000,000 次传输：

| Variant | 128-byte separated | 64-byte shared | 128 vs 64 |
| --- | ---: | ---: | ---: |
| mutex | 30.626M | 30.832M | -0.67% |
| seq_cst | 16.070M | 18.567M | -13.45% |
| acquire/release | 15.582M | 16.488M | -5.50% |
| cached | 19.041M | 18.786M | +1.35% |

正确分线在这一轮仅让 cached 中位数提高 1.35%，远小于前一轮观察到的 22.7%，
且没有让其他版本更快。seq_cst 每次本来就要同时读两个共享索引；把它们放在一
条线可能减少 footprint，同时增加 invalidation。如此高的轮间漂移说明：没有
固定 CPU 和 PMU 证据时，不能从 wall time 判定 false sharing 的实际成本。

## 4. 容易踩的坑

| Pitfall | Consequence | Required response |
| --- | --- | --- |
| 把 SPSC API 用成 MPSC/MPMC | 同一索引多写，cached 字段数据竞争 | 类型/API 显式区分 endpoint；升级算法而非补 fence |
| 认为 `atomic<T>` 天生 lock-free | 平台可能落到内部锁 | 初始化时检查 `atomic_is_lock_free` |
| 两侧全部 relaxed | payload publication/reclamation 无 happens-before | peer load acquire，index publish release |
| cache line 写死 64 | M1 等 128-byte 机器仍 false-share | 按目标编译并验证实际地址 |
| 只对齐成员，不对齐对象分配 | 动态对象基址可能不满足过度对齐 | 使用 aligned allocation，并验证 offset/address |
| 忽略相邻 allocation | slot 首尾仍可能与邻接对象共享 cache line | production allocator 需 guard padding |
| 忘记空槽 | `N` 个槽只能放 `N-1` 项 | API 暴露 usable capacity |
| `N=0/1` | 零容量或永远 full，甚至越界 | 创建时拒绝 |
| 把 try-loop 称为 wait-free | full/empty 时完成依赖对端 | 区分 bounded try operation 与 retry policy |
| 忙等不加约束 | 吃满核心、放大功耗和调度不公平 | 明确 spin budget、pause/yield/park 策略 |
| 泛型对象只做赋值 | 构造、析构、异常、ownership 不清 | 明确 payload lifetime；C 版先限制标量 |
| `noexcept` 包住可抛赋值 | 异常触发 terminate | 让异常规格依赖 `T` 或限制类型 |
| 只测大 ring/顺序整数 | 隐藏 cache residency、burst 与 backpressure | 做容量、payload、占用和 burst 矩阵 |
| 只报平均 ops/s | 看不到抖动、重试和尾延迟 | 保留 raw samples、retries、p99/occupancy |
| 把 transfer 当两个 op 或反之 | 吞吐口径相差 2 倍 | 同时报 transfers/s 与 API calls/s |
| Google Benchmark 默认计时未声明 | 手工 worker 的 CPU/wall 口径可能误读 | 显式 wall/manual time 并保存 raw output |
| 用 `-ffast-math` 装饰整数测试 | 不增益，还污染真实泛型构建语义 | 删除无关 flag |
| CPU ID 只看数字 | 可能同 SMT core、跨 NUMA 或绑到 E-core | 记录 topology、siblings、NUMA、频率 |
| 看到 `%` 就改 bitmask | 容量契约改变，编译器可能已优化 | 先 profile/codegen；本文实际是 wrap branch |

## 5. 什么时候值得用

适合：

- producer/consumer 数量在接口层可证明为 1:1；
- 固定上限和显式 backpressure 可接受；
- 元素 ownership 可在 push/pop 边界转移；
- 线程长期驻留且队列通常能批量前进；
- 目标机实测证明 coherence/lock 是瓶颈。

不适合：

- producer/consumer 数量动态；
- 不能丢、不能忙等、又没有阻塞/唤醒协议；
- 队列需要在线扩容；
- payload 有复杂析构、异常或跨语言 ownership；
- 端到端瓶颈在 I/O、调度、序列化或 NUMA，而非 queue。

## 6. 产物与边界

- [`demo/include/spsc_ring.h`](demo/include/spsc_ring.h)：可消费的 C11 API；
- [`demo/src/spsc_ring.c`](demo/src/spsc_ring.c)：四种同步实现；
- [`demo/tests/spsc_ring_test.c`](demo/tests/spsc_ring_test.c)：独立 FIFO oracle；
- [`demo/src/benchmark.c`](demo/src/benchmark.c)：wall-time benchmark；
- [`demo/verify.sh`](demo/verify.sh)：FIL-C-first 验证入口；
- [`evidence/README.md`](evidence/README.md)：环境、结果、codegen 与限制；
- [`exploration.md`](exploration.md)：失败路径与决策过程。

当前产物是机制验证与 benchmark harness，不是 production queue。进入生产前至少还
需要 endpoint misuse 防护、payload lifetime API、batch operations、blocking
策略、目标 Linux 拓扑上的 PMU/`perf c2c`、故障注入和真实 workload 回归门禁。
