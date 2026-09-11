---
doc_id: recallfs-study-stackful-coroutine-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260910-stackful-coroutine/demo
depends_on:
  - recallfs-agent-ready-docs-v1
  - learning/studies/20260910-stackful-coroutine/source.md
supersedes: []
verified_by:
  - CORO-RA-1
  - CORO-RA-2
  - CORO-RA-3
  - CORO-RA-4
  - CORO-RA-5
  - CORO-RA-6
  - CORO-RA-7
  - CORO-RA-8
  - CORO-RA-9
  - CORO-RA-10
  - CORO-RA-11
  - CORO-RA-12
---

# Stackful Coroutine 与 L4 Forwarder Study

> 主文：Huiba Li 等，[Stackful Coroutine Made Fast](papers/stackful-coroutine-made-fast.pdf)。
>
> 目标环境：`ssh d2`，Linux x86-64，Intel Xeon Platinum 8457C。
>
> 本文区分作者报告值、本地复现值和未验证推论。

## Contract

### Decision

在 Linux x86-64 上，保留完整 System V AMD64 backend 作为默认生产路径，
并以 Zig 0.16.0/Clang 21.1.0 构建实验 CACS 与
CACS+显式 `preserve_none` backend；以同语义的非 coroutine `epoll` state
machine 作为应用级 A/B baseline。是否采用 coroutine 不由单一吞吐指标决定，
而由真实 workload 下的 CPU、内存、cache、资源上界和控制流复杂度共同决定。

### Scope

- `learning/studies/20260910-stackful-coroutine/demo/include/rco.h` 的单线程
  cooperative coroutine API；
- `learning/studies/20260910-stackful-coroutine/demo/examples/l4_forwarder.c`
  的 coroutine L4 forwarder；
- `learning/studies/20260910-stackful-coroutine/demo/examples/l4_forwarder_epoll.c`
  的 non-coroutine A/B baseline；
- compiler-visible CACS switch 与显式 `preserve_none` suspension ABI；
- coroutine-local `errno`、runtime-scoped sparse TLS，以及 opt-in signal
  mask/locale；
- timer-requested safe-point preemption；
- 每 worker 一个 runtime 的多核 pool、启动前 work stealing 和 worker
  affinity；
- 最高 16,384 tasks、256 KiB touched stack 的 CPU/内存 scaling matrix；
- d2 上的 FIL-C、native、sanitizer、ABI 与 benchmark evidence；
- 文献中 CACS、stackful/stackless 和 cooperative task model 的适用边界。

### Non-goals

- 不实现论文的 automatic preserve-none propagation（APN）或 in-stack
  generator；
- 不把实验 CACS backend 作为默认安装 ABI；
- 不实现任意 PC 异步切栈的 hard preemption；
- 不迁移已启动 coroutine；work stealing 仅适用于尚未分配 stack 的 job；
- 不实现 blocking syscall hook 或 dynamic/segmented stack；
- 不把现有单 runtime L4 forwarder 自动升级为多核 listener；
- 不把 loopback throughput 外推为真实 NIC line rate 或 p99 latency；
- 不以 L4 benchmark 决定所有服务都应采用 coroutine。

### Inputs And Outputs

| Boundary | Input | Output |
| --- | --- | --- |
| Runtime | task entry、argument、stack size、FD wait、deadline | task completion、ready event 或负 `errno` |
| Worker pool | task spec、affinity、shutdown mode | exact-once completion、pre-start steal、统计与 bounded join |
| Coroutine forwarder | client TCP byte stream、numeric upstream address | 保序的双向 TCP byte stream |
| Epoll baseline | 与 coroutine forwarder 完全相同的 CLI 和流量 | 同样的 byte stream 与 shutdown counters |
| A/B harness | 两个 binary、run count、duration、CPU/NUMA placement | aggregate throughput、per-stream progress、VM/CPU process metrics |

### Interfaces And Ownership

- runtime 创建线程拥有 runtime、task、stack cache、timer heap 和 FD watch；
- pool 每个 worker 线程独占一个 runtime、epoll、timer 和已启动 coroutine；
- pool mutex 管理未启动 job、handle generation、取消和 aggregate stats；
- task finalizer 在 scheduler stack 上 exactly once；
- coroutine forwarder 的两个 pump 各持有一个 connection reference；
- epoll baseline 的 event loop 独占 connection list、timer heap 和 FD table；
- 两种 forwarder 的最后一个 owner 都执行双 FD close 和 connection free；
- benchmark 只读取已生成 binary，不修改 tracked source。

### Invariants

- 单线程内最多一个 coroutine 为 `RUNNING`；
- ready task 不能无限饿死 epoll waiter；
- 每个方向最多持有一个 `buffer_size` payload；
- FIN 只在对应方向 buffer 排空后传播；
- stale `fd + generation` event 不命中新对象；
- 两个 forwarding direction 各有独立的 1 MiB dispatch budget，且每个
  payload byte 只在成功 `send()` 后计入一次；
- 每条 benchmark stream 的 sender/receiver bytes 和 bitrate 都必须大于 0；
- FIL-C、sanitizer 和 benchmark 结果不能互相替代。
- 三种 backend 的 `rco_context` 均保持 64 bytes，避免 layout 混入 CACS
  CPU/cache 对比。
- preemption signal handler 只设置 pending；实际切换只发生在
  `rco_preempt_point()` 或 `rco_preempt_enable()`；
- zero local-state flags 默认隔离 `errno`；signal mask 与 locale 必须显式启用；
- TLS key 属于 runtime，task value 稀疏分配，退出时最多执行 4 轮 destructor；
- 只有 `QUEUED` job 可被偷取；`RESERVED` 以后 worker/TID 不变；
- job entry 非零结果计入 `failed/first_job_error`，不转化为 pool 基础设施错误。

### Failure Semantics

- 初始化或 event infrastructure 失败导致进程非零退出；
- 单连接 connect、reset 或 I/O error 只关闭该连接并计数；
- 第一次 termination signal 停止 accept 并进入 grace drain；
- deadline 或第二次 signal 强制关闭剩余连接；
- allocation/FD cap 拒绝新连接，不破坏已有连接；
- pool worker 内调用 blocking wait/join 返回 `-EDEADLK`；
- failed pool start 在返回前回收已创建 worker，之后允许 destroy；
- reserved preemption signal 不能被 task-local mask 阻塞；
- 无法取得 PMU 或 CPU frequency policy 时显式记录 unavailable。

### Worked Examples

**CORO-WE-1: half-close 后返回响应。**

输入为 256 KiB request。backend 必须先读到 EOF，之后才发送完整 response。
client 发送后执行 `shutdown(SHUT_WR)`。两个 forwarder 都先把 request buffer
排空，再向 upstream 传播 FIN；reverse direction 继续接收并返回精确的
262,144 bytes。该例证明不能把一侧 EOF 误实现成双向 close。

**CORO-WE-2: write side backpressure。**

当 destination `send()` 返回 `EAGAIN` 时，coroutine 版本保留普通 C
`send_all()` call stack 并 suspend；epoll baseline 必须显式保存
`offset/length`，关闭 source read interest，并在 `EPOLLOUT` 后恢复。两者的
最大未发送 payload 都是一个 direction buffer。

**CORO-WE-3: runnable 与 I/O 同时存在。**

一个 task 连续 `rco_yield()`，另一个 task 等待已可读 pipe。scheduler 最多
执行 64 次 READY dispatch 后以 timeout 0 poll epoll，pipe waiter 必须在
busy task 完成 1024 次 yield 前恢复。

**CORO-WE-4: timer request 不在任意指令切栈。**

worker 的 thread-CPU timer 发送 reserved RT signal。handler 只设置
`preempt_pending`；CPU loop 到达 `rco_preempt_point()` 后才按普通 suspension
路径切回 scheduler。没有 safe point 的 loop 不会被强切。

**CORO-WE-5: 启动前 steal，启动后固定。**

4-worker pool 中把 1,024 个 job 以 `PREFER worker 0` 提交。空闲 worker
可以偷取 `QUEUED` job；job 一旦进入 `RESERVED` 并获得 runtime stack，后续
yield、sleep、FD wait 和 preempt point 都保持同一 worker 与 TID。

**CORO-WE-6: task-local state。**

两个 task 可同时持有不同 `errno`、TLS value、signal mask 和 locale。
TLS destructor 在 task state 下最多执行 4 轮；之后恢复 root state，再执行
用户 finalizer。未启用 signal-mask/locale flag 时不支付对应切换成本。

### Reconciliation Anchors

| Anchor | Input or condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `CORO-RA-1` | Native runtime build | Zig O3 37/37、GCC SysV 13/13、ASan/UBSan 34/34 | `cacs_scale_evidence.sh` |
| `CORO-RA-2` | 同一 EOF-delayed backend 与 32 concurrent clients | 两个 forwarder 各返回精确 8,720,449 bytes，完成 78 次 half-close | `learning/studies/20260910-stackful-coroutine/demo/tests/l4_e2e.py <binary>` |
| `CORO-RA-3` | 5 runs x 5 modes x 4 iperf streams | 共 100 个 sender/receiver stream 全部非零 | `learning/studies/20260910-stackful-coroutine/demo/bench/l4_bench.sh` |
| `CORO-RA-4` | 1,000,000 yield iterations | checksum、task count、active count 和 exact switch count 一致 | `rco_bench 1000000 11` |
| `CORO-RA-5` | 文档 frontmatter DAG | doc ID 唯一、依赖存在、图无环 | manual DAG validation + local-link check |
| `CORO-RA-6` | 固定 Zig 0.16.0 | `zig cc` 的 O0/O2/O3 与 ASan/UBSan 路径通过 | `learning/studies/20260910-stackful-coroutine/evidence/raw/ab/zig-validation.txt` |
| `CORO-RA-7` | client FD 同时可读写，两个方向各有 4 KiB work | 两个方向都转发 4 KiB；同一 byte 不在 recv/send 重复扣 budget | `epoll_l4_state_machine` |
| `CORO-RA-8` | Zig CACS build | O0/O2/O3、ABI/FP、guard page、codegen 与两版 L4 E2E 通过 | `RCO_BUILD_CACS=ON` CTest |
| `CORO-RA-9` | 10 cases x 3 modes x 5 runs | 150 samples；checksum、switch count、stack sentinel、task lifecycle 全部一致 | `rco_high_concurrency_bench.py` |
| `CORO-RA-10` | 3 backend local-state suites | `errno`、sparse TLS/destructor、signal mask、locale 与 NONE path 一致 | `rco_local_state_*` |
| `CORO-RA-11` | 3 backend preemption suites | 无 safe point 不强切；safe point、公平性、disable/enable 与资源恢复通过 | `rco_preempt_*` |
| `CORO-RA-12` | 1/2/4 workers x 3 backend x preemption on/off | 90 samples exact-once、steal、zero migration；2w >=1.50x、4w >=2.50x | `rco_pool_bench_*` |

### Evidence And Unknowns

已观察证据位于
[`learning/studies/20260910-stackful-coroutine/evidence/`](evidence/)。
当前未知项是跨机真实 NIC 的
throughput/p99、长稳 RSS、生产 stack high-water 和 overload SLO。A/B 的
5 次样本只能支持“两个 forwarding model 在此 workload 下没有可分辨的吞吐
优势”，不能支持普遍性能排序。

## 1. 结论先行

本 study 交付了一个在明确平台合同内可用于生产验证的 C11 stackful
coroutine library，以及一个真实可运行的 TCP L4 forwarder：

- `rco` 使用 System V AMD64 ABI、独立 `mmap` stack、上下双 guard page、
  FIFO ready queue、`epoll`、timer min-heap、取消和 bounded stack cache；
- runtime 默认隔离 coroutine `errno`，提供 sparse task TLS，并可选隔离
  signal mask 与 locale；
- deferred preemption 由 per-thread CPU timer 请求，在显式 safe point
  切换，适用于 SysV、CACS 和 CACS+PN；
- `rco_pool` 以每 worker 一个 runtime 扩展到多核，只偷取未启动 job，
  支持 affinity、并发 submit/cancel、drain/cancel shutdown 和 bounded join；
- L4 forwarder 对每条连接使用两个单向 pump coroutine，支持 nonblocking
  connect、partial I/O、backpressure、TCP half-close、peer reset、连接上限和
  `signalfd` graceful shutdown；
- non-coroutine baseline 用显式 connection/direction state machine 实现同样
  的 socket、buffer、timeout、half-close 和 shutdown contract；
- C 代码先通过 FIL-C；真实切栈再通过 GCC/Zig `zig cc`、LTO、
  ASan/UBSan、ABI
  canary、guard-page fault 和真实 TCP E2E；
- 同一 Zig O3 binary contract 下，启用默认 `errno` isolation 后，CACS
  把 `rco_yield` 从 `51.964 ns` 降到 `27.728 ns`（`-46.6%`）；
  CACS+PN 为 `27.562 ns`；
- CPU-bound pool 的 2-worker paired median speedup 为 `1.879-1.891x`，
  4-worker 为 `3.104-3.145x`，并且 90/90 样本均为 zero migration；
- 单 proxy core、4 条 loopback TCP 流下，SysV/CACS/CACS+PN/epoll median
  为 `25.215/25.436/25.099/25.324 Gbit/s`，没有可分辨的吞吐优势；
  direct loopback median 为 `73.716 Gbit/s`。

默认 backend 仍使用完整 ABI 保存集。实验 CACS backend 用 inline asm
clobber 把 register liveness 暴露给 compiler，只显式保存 `rsp/rbp` 与 FP
control；第三个 backend 再给 suspension API 添加 Clang `preserve_none`。
这部分复现了论文的 symmetric CACS 核心，但没有复现 APN 或 in-stack
generator。

主文也不是正式 ASPLOS'24 论文。作者页面明确说明稿件被拒，原因是 CACS
此前已在 libfringe 中实现。其结果应视为 author manuscript claim，而不是
经过 ASPLOS 出版流程的结论。

## 2. 证据地图

| 层级 | 证据 | 结论 |
| --- | --- | --- |
| 文献 | 主稿、appendix、coroutine 语义、C++ fibers 争论、USENIX task model、mTCP | 已归档 8 份 PDF，并记录 SHA-256 |
| C safety | FIL-C 0.684 | 生命周期、配置、stack mapping 和 CLI 路径通过 |
| ABI | GCC/Zig O0/O2/O3、register canary、FP control test、反汇编 | SysV callee-saved 与 FP control state 保持 |
| VM | 上下 guard page 子进程测试 | stack overflow 以 `SIGSEGV` fail-fast |
| Runtime | queue、timer、cancel、local state、preemption、pool、pipe/epoll | Zig O3 37/37；ASan/UBSan 34/34 |
| Multicore | 3 backend x 500 pool stress + 90 scaling samples | exact-once、pre-start steal、worker coverage、zero migration 全通过 |
| Network | EOF-delayed TCP backend + 32 concurrent clients | 两种 forwarder 均为 8,720,449 bytes、78 次 half-close |
| Shutdown | 保持连接跨越 grace deadline | 100 ms 到期后强制取消并退出 |
| Sanitizer | ASan + UBSan + fiber switch hooks | 完整 L4 E2E 通过 |
| Performance | CPU/NUMA pinning + wall time + iperf3 | direct/coroutine/epoll A/B 见第 9 节 |

原始日志在
[`learning/studies/20260910-stackful-coroutine/evidence/raw/`](evidence/raw/)，
汇总与限制见
[`learning/studies/20260910-stackful-coroutine/evidence/README.md`](evidence/README.md)。

## 3. 论文到底证明了什么

### 3.1 两个正交维度

`Revisiting Coroutines` 区分了 symmetric/asymmetric 与
stackful/stackless。`Cooperative Task Management without Manual Stack
Management` 又指出 task scheduling 和 stack management 是两个不同轴。

因此下面三件事不能混为一谈：

| 问题 | 本实现选择 |
| --- | --- |
| 谁决定何时切换 | 显式 yield/wait，或 timer request 后到达 safe point |
| continuation 放在哪里 | 每个 coroutine 的独立 native stack |
| I/O 谁驱动 | 单线程 level-triggered `epoll` |

stackful 的核心价值不是“更像线程”这个标签，而是任意深度普通 C call chain
都能在 I/O 点 suspend，恢复时无需把每一层改写为 state machine。

### 3.2 CACS 的优化对象

传统 context switch 把切换点当作普通 ABI call：

```text
caller code
    |
    v
save all callee-saved state
    |
    v
indirect transfer
    |
    v
restore all callee-saved state
```

CACS 让 compiler 在调用点决定哪些值真正 live，并允许 transfer path
inline。它同时攻击三类成本：

1. 不必要的 register save/restore；
2. 公共间接分支带来的 branch prediction 压力；
3. 通用切换代码和 metadata 的 instruction/data cache 压力。

论文还讨论 `preserve_none` calling convention 和 in-stack generator。
这些都不是只改一段 assembly 就能安全获得的优化。没有 compiler contract
时，inline asm clobber list 不能替代跨优化阶段的 register-liveness 证明。

### 3.3 本地基线

本实现把 `rco_context_switch(from, to)` 作为普通 C call。根据 System V
AMD64 ABI，它保存：

| State | Reason |
| --- | --- |
| `rsp` | 切换 stack |
| `rbx`, `rbp`, `r12-r15` | ABI callee-saved GPR |
| MXCSR | 保持 SIMD floating-point control |
| x87 control word | 保持 x87 rounding/exception control |
| return address | 保留在各自 stack，由 `ret` 恢复 |

caller-saved GPR 和 vector registers 不需要由 switch 保存，因为 compiler
必须把跨普通函数调用仍然 live 的值 spill。汇编显式执行 `cld`，并用
`endbr64` 支持 IBT 入口。CET shadow stack 尚未支持；runtime 探测到启用
状态时返回 `-ENOTSUP`。

### 3.4 三种可执行 backend

| Backend | Switch contract | Status |
| --- | --- | --- |
| `sysv` | 独立汇编保存完整 callee-saved GPR 与 FP control | 默认、可安装 |
| `cacs` | inline switch 声明所有 GPR/XMM/x87/flags/memory clobber，由 compiler spill live values | 实验 |
| `cacs-preserve-none` | CACS + suspension API 使用显式 `preserve_none` | 实验，ABI 固定到 Zig 0.16.0 |

三个 backend 使用相同 64-byte context layout、guarded stack、scheduler、
tests 和 benchmark source。CACS build 强制 `-mno-red-zone`，否则 inline
switch 中保存 resume address 会覆盖 compiler-owned red-zone 数据。
`preserve_none` 的 public compile definition 必须传播到所有 caller；混用
普通声明和 preserve-none library 会形成静默 ABI 错配，因此实验 archive
不进入默认 install target。

## 4. Runtime 结构

```text
+----------------------+
| application task     |
+----------+-----------+
           |
           | yield / wait_fd / sleep
           v
+----------------------+
| rco scheduler        |
| FIFO ready queue     |
| timer min-heap       |
+----------+-----------+
           |
           | no ready task
           v
+----------------------+
| epoll_wait           |
| fd + generation      |
+----------+-----------+
           |
           | wake once
           v
+----------------------+
| READY coroutine      |
+----------------------+
```

状态机为：

```text
NEW -> READY -> RUNNING -> READY
                  |
                  +-> WAIT_IO ----+
                  +-> WAIT_TIMER -+-> READY
                  |
                  +-> DONE
                  +-> CANCELLED
```

关键 invariant：

- 同一时刻只有一个 `RUNNING` coroutine；
- coroutine 最多位于一个 ready queue 和一个 timer heap slot；
- 同一个 FD 最多有一个 read waiter 和一个 write waiter；
- timeout、I/O、cancel 统一走一次性 wake；
- `epoll_event.data.u64` 编码 `fd + generation`，FD reuse 后的旧事件不会命中
  新 waiter；
- 最多执行 64 次 READY dispatch 就进行一次 nonblocking epoll poll，持续
  runnable 的 coroutine 不能无限饿死 I/O waiter；
- `EPOLLONESHOT` registration 在普通 readiness 后保留，下一次 wait 用
  `EPOLL_CTL_MOD` rearm；cancel、timeout 和 close 才执行删除；
- 已启动的 coroutine 被 cancel 后必须恢复到取消点执行清理；只有从未启动
  的 task 可以不进入用户函数直接回收；
- task finalizer 无论正常完成、已启动取消还是未启动取消都 exactly once；
- task stack 只能在 scheduler stack 上回收。

### 4.1 Local state 与 deferred preemption

zero `local_state_flags` 默认启用 coroutine-local `errno`。TLS key 归属
runtime，value 以 32-slot sparse chunk 延迟分配；signal mask 与 locale
隔离是 opt-in。TLS destructor 在 task state 下执行，最多 4 轮；用户
finalizer 在 TLS 销毁且 root state 恢复后执行。

preemption 使用 `CLOCK_THREAD_CPUTIME_ID` timer 和 reserved RT signal：

```text
thread CPU timer -> signal handler sets pending
                 -> rco_preempt_point()
                 -> normal suspension path
```

handler 不直接修改 stack/context。没有 safe point 的 C loop 不会被强切；
这是兼容 SysV 与 CACS register-liveness contract 的必要边界。

### 4.2 Multicore pool

`rco_pool` 在每个 worker 上创建独立 owner-only runtime。submitter 只把
`QUEUED` job 放入 worker deque；空闲 worker 可偷取非
`RCO_AFFINITY_REQUIRE` job。claim 后 job 立即进入 `RESERVED`，此后永久绑定
worker、TID、epoll 和 timer owner。

pool 支持并发 submit/cancel、ANY/PREFER/REQUIRE affinity、eventfd wakeup、
generation job handle、DRAIN/CANCEL shutdown 和 bounded join。finalizer
exactly once。job entry 的非零结果不终止 pool，而记录到
`stats.failed/first_job_error`；pool wait/join 只返回 infrastructure/lifecycle
错误。

ready queue 为 $O(1)$，timer insert/remove 为 $O(\log N)$。FD watch table
按 256 项 chunk 延迟分配：仍以两级直接索引提供 $O(1)$ 查找，但只为实际
出现的 FD range 分配 watch metadata。

## 5. Stack 分配与生命周期

每个 stack 是一个 private anonymous mapping：

```text
low address
+------------+--------------------------+------------+
| PROT_NONE  | usable PROT_RW           | PROT_NONE  |
| guard page | downward-growing stack   | guard page |
+------------+--------------------------+------------+
high address
```

新 task 冷路径执行一次 `mmap` 和一次 `mprotect`。task 完成并切回 scheduler
后，runtime 对 usable range 执行 `MADV_DONTNEED`；成功后才放入 bounded
cache，失败则 `munmap`。复用保留 guard pages，不在运行 stack 上释放自身。

| Workload dimension | 当前合同 |
| --- | --- |
| Value | stack 内容不要求清零后提供给用户；cache 前主动 reclaim |
| Size | 默认 128 KiB usable，加两个 system page guard |
| Lifetime | 完全隶属于一个 task，terminal 后由 scheduler 回收 |
| Access | downward stack，页面按需 fault，热调用链局部访问 |
| Concurrency | 单 owner thread，无 remote free |
| Topology | first-touch；benchmark 固定到 NUMA 0 |

这张表是实现合同，不是生产 workload histogram。接入真实服务后仍需记录
stack high-water mark、stack-size distribution、minor faults、RSS/PSS 和
cache hit rate。

## 6. L4 转发模型

```text
client socket                     upstream socket
     |                                  |
     |  pump A: recv -> bounded buffer  |
     +--------------------------------->|
     |                                  |
     |  pump B: bounded buffer <- recv  |
     |<---------------------------------+
```

每条已连接会话有两个 pump coroutine，共享一个 connection owner：

- 每个方向只有一个 `buffer_size` buffer；
- `send` 未排空前不再 `recv`，因此慢消费者会把 backpressure 传回 TCP；
- `recv == 0` 只在对应目的端执行 `shutdown(SHUT_WR)`；
- 另一个方向继续运行，直到自己的 EOF 或 error；
- 最后一个 pump 负责 exactly-once close 和 connection free；
- `MSG_NOSIGNAL` 防止 peer close 杀死进程；
- `EPOLLERR/HUP/RDHUP` 只触发重试，最终语义来自 `recv`、`send` 或
  `SO_ERROR`；
- 每转发 1 MiB 主动 yield，避免始终 ready 的大流独占 scheduler。

连接对象和两个 buffer 使用一次 allocation。payload buffer 的硬上界为：

$$
M_{buffer} = 2 \times C_{max} \times B
$$

默认 $C_{max}=256$、$B=64\,KiB$，所以最大 payload buffer 空间为
$32\,MiB$。两个 coroutine stack 的 usable virtual space 上界约为
$64\,MiB$，但 private anonymous pages 按需进入 RSS。

### 6.1 A/B：同一协议，两种控制流

non-coroutine 版本不调用 `rco`。它在一个 `epoll` loop 中显式保存：

- upstream connect state 和 deadline heap slot；
- 两个方向的 `offset/length/source_eof/destination_shutdown`；
- client/upstream interest mask、one-shot armed state 和 FD generation；
- 以 256-entry chunk 延迟分配的 FD watch table；
- 每个 event 后应继续 read、write、shutdown 还是 rearm。

coroutine 版本把这些 continuation state 放在 C call stack 上：

```text
coroutine path                 event-loop path
--------------                 ---------------
recv                           state.read_ready
send_all                       state.offset/length
rco_wait_fd                    epoll interest/rearm
return from wait               explicit dispatch branch
normal local variables         persistent connection fields
```

两种实现都使用一个 connection allocation 和两个固定 buffer。代码规模是：

| Surface | Lines | Function definitions |
| --- | ---: | ---: |
| Coroutine L4 application | 929 | 31 |
| Non-coroutine epoll application | 1,463 | 58 |
| Reusable `rco` runtime | 1,244 | 29 |

这组数字只描述当前实现，不是通用复杂度度量。它说明：

- 如果已有多个网络服务共享 `rco`，单个业务的 control-flow surface 更小；
- 如果只实现这一个简单 forwarder，event-loop 版本没有 coroutine runtime
  和 ABI assembly 依赖，总代码与部署组件更少；
- coroutine 的主要价值是 nested suspend、普通局部变量和 reusable blocking
  style API，不是这次 benchmark 中的吞吐提升；
- event-loop 的主要价值是更低 virtual-memory footprint、无自定义 stack/ABI
  风险，以及更直接的 event batch 控制。

因此本次 A/B 的工程结论是：

> 在 kernel TCP copy 主导的单核 L4 workload 中，两种模型吞吐持平。选择
> coroutine 的理由是复杂调用链的可组合性和业务代码可读性；选择显式
> state machine 的理由是更小运行时边界和更低 per-connection VM 成本。

## 7. Shutdown 与失败语义

启动时阻塞 `SIGINT`/`SIGTERM`，由 `signalfd` coroutine 接收：

1. 第一次信号关闭 listener，不再接收新连接；
2. 现有连接继续传播 half-close；
3. 最后一条连接结束时立即停止 runtime；
4. grace deadline 到期或收到第二次信号时，取消所有 waiter 并强制关闭。

初始化失败是进程级错误。单连接 connect/I/O/reset 只终止该连接并计数。
`close()` 不重试；被 runtime watch 的 FD 必须经 `rco_close_fd()` 关闭，以
先失效 generation、唤醒 waiter，再完成一次 close。

## 8. Correctness 结果

最终 v0.2 benchmark evidence source commit：
`d3f48b96964d39760ec75b4e5a2ce7227b9b7947`。该 revision 包含 local
state、deferred preemption、multicore pool、review fixes、backend identity、
parent signal cleanup、subreaper 和 tracked evidence driver。

| Gate | Result |
| --- | --- |
| Proof-first | API test 首次链接因缺少 `rco_*` symbols 按预期失败 |
| FIL-C 0.684 | 5 个 lifecycle/config/pool suites；high-concurrency parser/bounds 通过 |
| GCC/Zig matrix | GCC SysV 13/13；Zig O0/O2 focused 28/28，O3 37/37 |
| ASan + UBSan | runtime、local state、preemption、pool、高并发与四版 L4 共 34/34 |
| Pool stress | SysV/CACS/CACS+PN 各 500/500，合计 1,500 次 |
| Parent signal cleanup | 19/19 focused harness tests；launch-window signal、stopped leader 与忽略 TERM 的 descendant 均被回收 |
| Executable stack | `GNU_STACK` 为 `RW`，不是 `RWE` |
| Evidence manifest | 139/139 payload path 与 SHA-256 通过 |

FIL-C 不执行自定义 stack-switch assembly，因此不能证明 register save、stack
alignment、CET 或真实 epoll 时序。对应缺口由 native ABI canary、FP control
隔离、guard fault、反汇编和 socket E2E 覆盖。

## 9. d2 性能结果

### 9.1 Context benchmark

构建参数：

```text
-O3 -DNDEBUG -flto -march=native -mtune=native
-Wall -Wextra -Werror -fno-omit-frame-pointer -mno-red-zone
-fcf-protection=branch
```

每个 backend 的每个 run 内部取 11 个 sample 的 median，再对 5 个
position-rotated run 取中位数：

| Backend | Run-level `rco_yield` medians, ns | Median | vs SysV |
| --- | --- | ---: | ---: |
| SysV | 51.964, 51.580, 51.521, 52.099, 52.140 | `51.964` | `1.000` |
| CACS | 27.339, 27.728, 27.170, 27.765, 27.895 | `27.728` | `0.534` |
| CACS + `preserve_none` | 27.243, 27.562, 27.632, 27.812, 27.141 | `27.562` | `0.530` |

`rco_yield` 是完整 scheduler operation，含两次 assembly transfer、queue
操作、默认 `errno` local-state 切换、状态更新和每 64 次 dispatch 一次的
I/O fairness poll。CACS 相对 SysV 减少 `46.6%`；显式 `preserve_none`
没有进一步降低该 workload 的中位数。
同轮 noinline function call 与 Linux `sched_yield` 的 SysV 中位数分别为
`1.639 ns` 和 `228.580 ns`。论文报告的约 1.52 ns 使用更激进的
in-stack generator/APN 和不同 benchmark，不能与这里做同口径结论。

### 9.2 L4 throughput

配置：iperf server 固定 CPU 1，proxy 固定 CPU 0，client 固定 CPU 2；三者
都在 NUMA node 0。每次 4 条并行流、3 秒测量、1 秒 omit。每轮把五种 mode
轮转到不同顺序位置，实际顺序记录在 `run-order.csv`。

| Path | Gbit/s samples | Median |
| --- | --- | ---: |
| direct loopback | 73.716, 73.486, 71.984, 74.947, 74.141 | `73.716` |
| coroutine SysV | 25.831, 24.628, 24.988, 25.796, 25.215 | `25.215` |
| CACS | 25.554, 25.057, 25.436, 25.159, 25.617 | `25.436` |
| CACS + `preserve_none` | 25.191, 24.894, 24.876, 25.099, 25.802 | `25.099` |
| epoll state machine | 25.459, 24.734, 25.083, 25.407, 25.324 | `25.324` |

CACS/SysV paired ratio 的中位数为 `1.015959`，CACS+PN/SysV 为
`0.995533`。100 个 sender/receiver stream 全部有非零进展。结论仍是四种
forwarder 吞吐持平，而不是某个 backend 稳定领先。

下表保留早期三路 A/B 的进程级资源中位数；最终五路 run 主要用于 backend
吞吐回归，两组数据不可按 run 配对：

| Metric | Coroutine | Epoll state machine | Interpretation |
| --- | ---: | ---: | --- |
| `VmPeak` | 4,724 KiB | 2,948 KiB | coroutine 为每个 pump 保留独立 virtual stack |
| `VmHWM` | 1,940 KiB | 1,828 KiB | demand paging 后实际 RSS 差距较小 |
| user ticks | 2 | 2 | 采样粒度不足，不能判断优劣 |
| system ticks | 398 | 397 | 基本相同，主要成本在 kernel TCP 数据搬运 |

这个结果证明两个 demo 都能持续执行真实 TCP 数据面，并不证明跨机 line rate，
也不包含 p99 latency。loopback 结果受 kernel copy、KVM 和共享宿主噪声影响；
生产评估仍需增加运行时长和真实 NIC。

benchmark 时 soft/hard `RLIMIT_NOFILE` 分别为 1,024/1,048,576。focused
startup test 把 soft limit 从 1,024 提到 1,048,576，epoll baseline 的
`VmSize` 增长为 0 KiB，证明 lazy watch chunks 消除了 limit-driven dense
allocation 对 VM 对比的混淆。

### 9.3 High-concurrency CPU 与 memory

该 benchmark 不创建 socket。每个 task 在自己的 guarded stack 上按页写入
sentinel，全部 task 到达 residency barrier 后进程 `SIGSTOP`，parent 读取
`/proc/<pid>/status`、`smaps_rollup` 和 fault counters，再恢复并执行精确
yield 数。150 个样本覆盖 10 个 case、3 个 backend、5 次轮转。

| Case | SysV CPU ns/measured-yield | CACS | CACS+PN | CACS/SysV |
| --- | ---: | ---: | ---: | ---: |
| 256 tasks, 4 KiB touched | 62.98 | 34.26 | 44.83 | `0.544` |
| 1,024 tasks, 4 KiB touched | 140.52 | 67.92 | 103.11 | `0.483` |
| 4,096 tasks, 4 KiB touched | 175.97 | 89.62 | 119.53 | `0.509` |
| 16,384 tasks, 4 KiB touched | 229.00 | 115.86 | 195.06 | `0.506` |
| 1,024 tasks, 128 KiB touched | 623.76 | 456.06 | 495.79 | `0.731` |
| 1,024 tasks, 256 KiB touched | 1,413.30 | 1,252.80 | 1,295.36 | `0.886` |
| 1,024 tasks, 1,024 yields/task | 136.21 | 64.85 | 97.61 | `0.476` |

结论：

- 高频 suspend/resume 时，默认 `errno` isolation 下 CACS 把
  CPU/measured-yield 降低约 `46%-52%`；
- task 数增加时收益没有消失，16,384 tasks 下仍降低约 `49%`；
- 每次 yield 扫描的 stack working set 从 32 KiB 增到 256 KiB 时，收益从
  约 `45%` 缩小到 `11%`，说明 memory work 会逐步淹没 switch 优化；
- 三种 backend 保持相同 64-byte context；16,384 tasks 的 PSS 分别为
  201,238/201,238/201,238 KiB，CACS 没有可分辨的额外常驻内存成本；
- CACS+PN 在该扩展 runtime 上没有稳定优于 plain CACS，不能把
  preserve-none 标注为额外收益。

同一批 CACS binaries 在五路 L4 benchmark 中分别得到 direct `73.716`、
SysV `25.215`、CACS `25.436`、CACS+PN `25.099`、epoll
`25.324 Gbit/s`。CACS/SysV 与 CACS+PN/SysV 的 paired ratio 中位数分别为
`1.015959` 和 `0.995533`，说明 `rco_yield` 的 `46.6%` 降幅没有
穿透 kernel-TCP-dominated 数据面；这不是 CACS 无效，而是该 workload 的
switch 占比太低。

d2 的 `perf` 对 `cycles`、`instructions`、`branches`、
`branch-misses`、`cache-misses`、L1D 和 dTLB events 全部返回
`<not supported>`。因此 CPU 结论来自 wall time 与 `getrusage` 的一致结果；
cache/TLB 仍是未知项，不用 VM 或 FIL-C 指令计数替代 PMU。
该 KVM 也没有暴露 cpufreq policy、governor 或 turbo control；原始环境明确
记录为 unavailable，而不是推测宿主频率策略。

### 9.4 Multicore pool scaling

pool benchmark 把 1,024 个 CPU job 以 `PREFER worker 0` 提交，每个 job
执行 65,536 次确定性 integer step，并每 64 次迭代调用 preemption safe
point。5 轮同时轮转 backend、worker count 与 preemption 顺序。

| Backend | Preemption | 1 worker jobs/s | 2 worker speedup | 4 worker speedup |
| --- | --- | ---: | ---: | ---: |
| SysV | off | 7,402 | `1.889x` | `3.117x` |
| CACS | off | 7,421 | `1.891x` | `3.135x` |
| CACS+PN | off | 7,421 | `1.885x` | `3.104x` |
| SysV | 1 ms | 7,275 | `1.880x` | `3.110x` |
| CACS | 1 ms | 7,226 | `1.883x` | `3.145x` |
| CACS+PN | 1 ms | 7,271 | `1.878x` | `3.141x` |

每个 cell 都通过 2-worker `>=1.50x`、4-worker `>=2.50x` gate。90 个样本
同时验证 deterministic checksum、exact-once execution/finalizer、全部 worker
参与、2/4 worker 时发生启动前 steal、started coroutine migration 为 0，
以及 wake coalescing。另有三 backend 各 500 次 pool suite 压力。

该 benchmark 的 PMU probe 同样返回 unavailable，因此这些数字只支持此 d2
拓扑上的 wall/process CPU scaling，不支持 cache/TLB/branch 原因归因。

## 10. 适用边界

在以下合同内，可以把默认 SysV `rco` 当作 production-ready building
block；两个 CACS backend 仍是 benchmark-only experimental targets：

- Linux x86-64 System V ABI；
- 单个 `rco_runtime` 仍是 owner-thread scheduler；多核由 `rco_pool` 组合；
- timer-requested preemption 只在显式 safe point 切换；
- pool 只偷取未启动 job；started coroutine 不跨 worker/TID 迁移；
- 默认隔离 `errno`，TLS runtime-scoped；signal mask/locale 为 opt-in；
- 所有可能阻塞的网络路径使用 nonblocking FD 与 `rco_wait_fd()`；
- 被 watch 的 FD 通过 `rco_close_fd()` 关闭；
- 固定 stack 大小在真实调用链上经过压力测试；
- CET shadow stack 未启用。

它不是通用线程替代品，当前不提供：

- arbitrary-PC hard preemption；
- 已启动 coroutine 的 stack migration；
- dynamic/segmented stack；
- blocking DNS、文件 I/O 和第三方阻塞调用的透明 hook；
- 多 worker `SO_REUSEPORT` L4 listener；当前 demo 仍是单 runtime forwarder；
- CET shadow-stack switching；
- Windows、AArch64 或其他 ABI backend；
- 跨机 NIC、p99 latency、长期 RSS 和 overload SLO 证明。

## 11. 运行

完整命令和参数见
[`learning/studies/20260910-stackful-coroutine/demo/README.md`](demo/README.md)。

```bash
cmake -S learning/studies/20260910-stackful-coroutine/demo \
  -B .tmp/rco-build -DCMAKE_BUILD_TYPE=Release
cmake --build .tmp/rco-build -j
ctest --test-dir .tmp/rco-build --output-on-failure
```

后续 L4 扩展应让每个 pool worker 独占 `SO_REUSEPORT` listener、epoll 和
connection lifetime，连接启动后不迁移。若继续逼近论文完整方案，则实现
automatic preserve-none propagation 和 in-stack generator，并继续在 IR、
最终 binary、ABI canary 和目标机 PMU benchmark 四层共同验证。
