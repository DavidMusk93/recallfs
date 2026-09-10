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
---

# Stackful Coroutine 与 L4 Forwarder Study

> 主文：Huiba Li 等，[Stackful Coroutine Made Fast](papers/stackful-coroutine-made-fast.pdf)。
>
> 目标环境：`ssh d2`，Linux x86-64，Intel Xeon Platinum 8457C。
>
> 本文区分作者报告值、本地复现值和未验证推论。

## Contract

### Decision

在 Linux x86-64 上，以遵守完整 System V AMD64 callee-saved contract 的
stackful coroutine runtime 作为可复用执行层；以同语义的非 coroutine
`epoll` state machine 作为 A/B baseline。L4 数据面是否采用 coroutine 不由
微基准决定，而由真实 workload 下的吞吐、资源上界和控制流复杂度共同决定。

### Scope

- `learning/studies/20260910-stackful-coroutine/demo/include/rco.h` 的单线程
  cooperative coroutine API；
- `learning/studies/20260910-stackful-coroutine/demo/examples/l4_forwarder.c`
  的 coroutine L4 forwarder；
- `learning/studies/20260910-stackful-coroutine/demo/examples/l4_forwarder_epoll.c`
  的 non-coroutine A/B baseline；
- d2 上的 FIL-C、native、sanitizer、ABI 与 benchmark evidence；
- 文献中 CACS、stackful/stackless 和 cooperative task model 的适用边界。

### Non-goals

- 不实现论文依赖 compiler liveness 的 CACS 或 `preserve_none`；
- 不实现多线程 scheduler、work stealing、preemption 或 stack migration；
- 不实现 TLS/`errno` 虚拟化、blocking syscall hook、dynamic stack；
- 不把 loopback throughput 外推为真实 NIC line rate 或 p99 latency；
- 不以 L4 benchmark 决定所有服务都应采用 coroutine。

### Inputs And Outputs

| Boundary | Input | Output |
| --- | --- | --- |
| Runtime | task entry、argument、stack size、FD wait、deadline | task completion、ready event 或负 `errno` |
| Coroutine forwarder | client TCP byte stream、numeric upstream address | 保序的双向 TCP byte stream |
| Epoll baseline | 与 coroutine forwarder 完全相同的 CLI 和流量 | 同样的 byte stream 与 shutdown counters |
| A/B harness | 两个 binary、run count、duration、CPU/NUMA placement | aggregate throughput、per-stream progress、VM/CPU process metrics |

### Interfaces And Ownership

- runtime 创建线程拥有 runtime、task、stack cache、timer heap 和 FD watch；
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

### Failure Semantics

- 初始化或 event infrastructure 失败导致进程非零退出；
- 单连接 connect、reset 或 I/O error 只关闭该连接并计数；
- 第一次 termination signal 停止 accept 并进入 grace drain；
- deadline 或第二次 signal 强制关闭剩余连接；
- allocation/FD cap 拒绝新连接，不破坏已有连接；
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

### Reconciliation Anchors

| Anchor | Input or condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `CORO-RA-1` | Native runtime build | 12 runtime suites、guard page 和 accept failure test 通过 | `ctest --test-dir .tmp/rco-ab-build --output-on-failure` |
| `CORO-RA-2` | 同一 EOF-delayed backend 与 32 concurrent clients | 两个 forwarder 各返回精确 8,720,449 bytes，完成 78 次 half-close | `learning/studies/20260910-stackful-coroutine/demo/tests/l4_e2e.py <binary>` |
| `CORO-RA-3` | 5 runs x 4 iperf streams | direct/coroutine/epoll 共 60 个 sender/receiver stream 全部非零 | `learning/studies/20260910-stackful-coroutine/demo/bench/l4_bench.sh` |
| `CORO-RA-4` | 1,000,000 yield iterations | checksum、task count、active count 和 exact switch count 一致 | `rco_bench 1000000 11` |
| `CORO-RA-5` | 文档 frontmatter DAG | doc ID 唯一、依赖存在、图无环 | manual DAG validation + local-link check |
| `CORO-RA-6` | 固定 Zig 0.16.0 | `zig cc` 的 O0/O2/O3 与 ASan/UBSan 路径通过 | `learning/studies/20260910-stackful-coroutine/evidence/raw/ab/zig-validation.txt` |
| `CORO-RA-7` | client FD 同时可读写，两个方向各有 4 KiB work | 两个方向都转发 4 KiB；同一 byte 不在 recv/send 重复扣 budget | `epoll_l4_state_machine` |

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
- L4 forwarder 对每条连接使用两个单向 pump coroutine，支持 nonblocking
  connect、partial I/O、backpressure、TCP half-close、peer reset、连接上限和
  `signalfd` graceful shutdown；
- non-coroutine baseline 用显式 connection/direction state machine 实现同样
  的 socket、buffer、timeout、half-close 和 shutdown contract；
- C 代码先通过 FIL-C；真实切栈再通过 GCC/Zig `zig cc`、LTO、
  ASan/UBSan、ABI
  canary、guard-page fault 和真实 TCP E2E；
- d2 上 `rco_yield` 的 5 次 run-level median 中位数为
  `35.141 ns/operation`。一次 operation 包含 scheduler 工作和两次底层
  context switch，不能与论文的 CACS 单次切换数字直接比较；
- 单 proxy core、4 条 loopback TCP 流下，coroutine median 为
  `24.904 Gbit/s`，epoll state machine median 为 `25.007 Gbit/s`，两者
  没有可分辨的吞吐差异；direct loopback median 为 `74.093 Gbit/s`。

这里没有复现论文的 CACS。CACS 依赖 compiler 在每个调用点掌握 register
liveness，并配合 `preserve_none` calling convention。普通 C11 与独立汇编
只能安全地实现完整 ABI 保存集。擅自少保存寄存器会把 benchmark 技巧变成
correctness bug。

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
| Runtime | queue、timer、cancel、pipe/epoll、stack cache tests | 12 个 runtime suites；完整 CTest 7/7 通过 |
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
| 谁决定何时切换 | cooperative，只有显式 yield/wait |
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
| Coroutine L4 application | 884 | 28 |
| Non-coroutine epoll application | 1,375 | 53 |
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

目标 code/benchmark commit：`463126cb7ab3e40df6f214803695dd200a4f0ef4`。

| Gate | Result |
| --- | --- |
| Proof-first | API test 首次链接因缺少 `rco_*` symbols 按预期失败 |
| FIL-C 0.684 | 3 个 lifecycle suites；两条 L4 C 路径和 focused state-machine test 通过 |
| GCC/Zig matrix | GCC native benchmark；Zig 0.16.0 `zig cc` O0/O2/O3 全通过 |
| Native CTest | runtime、guard page、focused tests、双实现 E2E 共 7/7 targets 通过 |
| ASan + UBSan | 两种 forwarder 的 32 并发、half-close、forced drain 全通过 |
| Executable stack | `GNU_STACK` 为 `RW`，不是 `RWE` |

FIL-C 不执行自定义 stack-switch assembly，因此不能证明 register save、stack
alignment、CET 或真实 epoll 时序。对应缺口由 native ABI canary、FP control
隔离、guard fault、反汇编和 socket E2E 覆盖。

## 9. d2 性能结果

### 9.1 Context benchmark

构建参数：

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto
-fno-ipa-icf -Wall -Wextra -Werror -fno-omit-frame-pointer
-fcf-protection=branch
```

每个 run 内部取 11 个 sample 的 median，再对 5 个 run 取中位数：

| Operation | Run-level medians, ns | Median |
| --- | --- | ---: |
| `rco_yield` | 35.141, 35.406, 35.122, 35.092, 35.340 | `35.141` |
| noinline function call | 1.638, 1.643, 1.648, 1.636, 1.639 | `1.639` |
| Linux `sched_yield` | 229.449, 227.847, 227.739, 227.739, 228.330 | `227.847` |

`rco_yield` 是完整 scheduler operation，含两次 assembly transfer、queue
操作、状态更新和每 64 次 dispatch 一次的 I/O fairness poll。它比本机
`sched_yield` 低约 84.6%，但约为测试中 noinline function call 的 21.4 倍。
论文报告的约 1.52 ns CACS yield 使用
不同实现、compiler 和 benchmark，不能与这里做同口径结论。

### 9.2 L4 throughput

配置：iperf server 固定 CPU 1，proxy 固定 CPU 0，client 固定 CPU 2；三者
都在 NUMA node 0。每次 4 条并行流、3 秒测量、1 秒 omit。

| Path | Gbit/s samples | Median |
| --- | --- | ---: |
| direct loopback | 73.710, 73.836, 75.391, 74.598, 74.093 | `74.093` |
| coroutine forwarder | 24.471, 24.848, 24.904, 25.058, 25.023 | `24.904` |
| epoll state machine | 23.946, 25.037, 25.007, 25.167, 24.852 | `25.007` |

coroutine/epoll 的 ratio-of-medians 为 `0.996`，按轮配对 ratio 的中位数为
`0.996`、均值为 `1.003`，且 5 轮中有 3 轮 epoll 更快。因此结论是吞吐
**持平**，不是 epoll 快 0.4%。60 个 sender/receiver stream 全部有非零
进展，单次 run 内各流吞吐接近。

进程级资源中位数：

| Metric | Coroutine | Epoll state machine | Interpretation |
| --- | ---: | ---: | --- |
| `VmPeak` | 4,720 KiB | 2,948 KiB | coroutine 为每个 pump 保留独立 virtual stack |
| `VmHWM` | 2,016 KiB | 1,760 KiB | demand paging 后实际 RSS 差距较小 |
| user ticks | 3 | 3 | 采样粒度不足，不能判断优劣 |
| system ticks | 398 | 397 | 基本相同，主要成本在 kernel TCP 数据搬运 |

这个结果证明两个 demo 都能持续执行真实 TCP 数据面，并不证明跨机 line rate，
也不包含 p99 latency。loopback 结果受 kernel copy、KVM 和共享宿主噪声影响；
生产评估仍需增加运行时长和真实 NIC。

d2 的 `perf` 对 `cycles`、`instructions`、`branches`、
`branch-misses`、`cache-misses` 全部返回 `<not supported>`。因此本报告只把
固定拓扑与 wall time 作为性能证据，不用 VM 或 FIL-C 指令计数替代 PMU。
该 KVM 也没有暴露 cpufreq policy、governor 或 turbo control；原始环境明确
记录为 unavailable，而不是推测宿主频率策略。

## 10. 适用边界

在以下合同内，可以把 `rco` 当作 production-ready building block：

- Linux x86-64 System V ABI；
- 单 OS thread、cooperative scheduling；
- coroutine 不跨线程迁移；
- 所有可能阻塞的网络路径使用 nonblocking FD 与 `rco_wait_fd()`；
- 被 watch 的 FD 通过 `rco_close_fd()` 关闭；
- 固定 stack 大小在真实调用链上经过压力测试；
- CET shadow stack 未启用。

它不是通用线程替代品，当前不提供：

- preemption、work stealing 或多核 scheduler；
- dynamic/segmented stack；
- coroutine-local `errno`、TLS、signal mask 或 locale；
- blocking DNS、文件 I/O 和第三方阻塞调用的透明 hook；
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

后续若追求论文级 CACS，正确路径是扩展 compiler calling convention 并在
IR、最终 binary、ABI canary 和目标机 benchmark 四层共同验证，而不是继续
缩短当前汇编保存列表。
