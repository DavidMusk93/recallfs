# `io_uring` 的“逃逸”I/O：进程退出后的请求生命周期与工程边界

> 原文：Evgenii Ivanov, [Is There I/O After Death? What Happens to
> io_uring When a Process Dies](https://blog.ydb.tech/is-there-i-o-after-death-what-happens-to-io-uring-when-a-process-dies-92c65354873f?postPublishedType=repub)
>
> 页面 metadata 发布时间：2026-09-06。阅读日期：2026-09-07。
>
> 原文实验使用 Linux 6.6.79。本文同时核对了 Linux 6.6 源码和
> 2026-09-07 的主线 `7.3-rc2`，commit
> [`df2908090cda`](https://github.com/torvalds/linux/commit/df2908090cda368b01ff43709f51890076c56157)。
> 当前主线仍保留普通任务退出只等待 tracked requests、SQPOLL 退出等待全部
> requests 的结构。本文没有在 macOS 本机复跑破坏性块设备实验。

## 0. 结论先行

文章展示的现象不是“死进程继续运行”，而是：

> 进程在用户态和进程表中的生命周期已经结束，但此前已交给内核或设备的
> 异步请求仍拥有独立的内核对象生命周期，因此可能在 `waitpid()` 返回后
> 才完成写入。

对“异常还是 feature”最准确的回答是：

| 层次 | 判断 |
| --- | --- |
| 请求持有 `struct file`、用户页和 ring context 等引用 | **设计所需的 feature**。否则 fd 关闭、fd 编号复用或地址空间退出会造成悬空引用或写错对象。 |
| 已下发到设备、无法取消的 I/O 继续完成 | **正常的异步 I/O 语义**。取消是 best effort，不能撤销已经发生的设备副作用。 |
| 普通 `io_uring` 让部分请求在 `waitpid()` 后完成 | **有意允许的退出策略，但不是应用可依赖的 UAPI 保证**。它在退出及时性与同步 drain 之间选择了前者。 |
| 应用把 `waitpid()` 当成存储 I/O barrier | **应用协议错误**。`waitpid()` 只报告子进程状态，不承诺异步 I/O 已取消、完成或持久化。 |
| 依赖“进程死后请求一定成功”实现 fire-and-forget | **错误用法**。请求可能成功、失败或被取消，且已没有可靠的用户态 CQE 消费者处理结果。 |

“逃逸”适合描述现象，不适合描述契约。工程上应把它理解成
**process lifetime 与 request lifetime 解耦**，而不是一项
“post-mortem I/O”功能。

这个机制有三类实际价值：

1. 保证异步请求引用的文件和内存不会在请求完成前失效。
2. 避免普通进程退出被所有不可取消的设备 I/O 无限拖住。
3. 可以把请求保留的 open file description 与 `flock()` 结合，构造本机
   failover 的 drain barrier；YDB 正在这样使用。

第三项是可利用的工程技巧，但不是分布式 fencing，也不是数据持久化保证。

## 1. 原文究竟证明了什么

### 1.1 实验方法

子进程持续保持约 32K 个 4 KiB direct write 在途，在 1,024 个已知位置循环
写入 `writer_id + generation`。父进程执行：

```text
start writer
    |
    v
keep about 32K direct writes in flight
    |
    v
send SIGKILL
    |
    v
waitpid returns
    |
    +--> snapshot A: read 1,024 blocks
    |
    +--> snapshot B: read the same blocks again
             |
             v
     compare writer_id and generation
```

如果两次快照之间同一位置的 generation 增长，就能证明该 writer 的请求在
`waitpid()` 返回后仍修改了存储。只看“块是否来自该 writer”不够，因为它
无法区分写入是在 `waitpid()` 之前还是之后完成的。

原文用两种方式扩大竞态窗口：

- `dm-delay`：writer 经 device-mapper 写延迟映射，父进程绕过映射直接读取
  backing partition；写延迟 3 秒而读不延迟。
- 原生 NVMe 压力：用只读 `fio` 把设备和 blk-mq 队列压深，不引入第二个
  writer，避免把别的写误判为旧进程写入。

### 1.2 结果

| 后端 | 3 秒 `dm-delay` 下的 `waitpid()` | `waitpid()` 返回后是否观察到旧写入 | 原文判断 |
| --- | ---: | --- | --- |
| 普通 `io_uring` | 约 `2.951 ms` | 是，第二次快照的 1,024 个位置全部变化 | 退出不构成 drain barrier |
| `io_uring` + `SQPOLL` | 约 `2149.286 ms` | 未观察到 | 剩余等待落在退出路径 |
| Linux native AIO | 约 `2246.371 ms` | 未观察到 | 最后一个 `mm` 释放时等待 AIO |

在原生 NVMe 压力下，普通 `io_uring` 的一次实验中：

- `waitpid()` 约 `8.028 ms` 返回；
- 第一次和第二次快照分别耗时约 `252.735 ms`、`177.544 ms`；
- 两次快照之间有 `627/1024` 个位置的 generation 增长。

这排除了“只有人工 `dm-delay` 才会发生”的解释。

### 1.3 证据能支持和不能支持的结论

它直接支持：

- Linux 6.6.79 的普通、非 SQPOLL `io_uring` direct block write 可以在
  writer 被 kill、reap 后完成；
- 请求仍持有 writer 使用的 open file description；
- 在该路径上，`waitpid()` 不是 I/O completion barrier。

它不能直接支持：

- 所有内核版本、文件系统、设备和 opcode 都有相同时序；
- SQPOLL 或 native AIO 永远保证 `waitpid()` 是 barrier；
- I/O completion 等于掉电后仍可恢复的 durability；
- 网络 I/O、buffered write、DAX、远端文件系统具有相同语义。

## 2. 为什么进程没了，I/O 还能继续

### 2.1 `waitpid()` 等待的是 task state，不是 storage state

[`waitpid(2)`](https://man7.org/linux/man-pages/man2/wait.2.html) 的契约是等待
子进程发生终止、停止或继续等状态变化，并取得终止状态。它没有声明：

- 子进程提交的所有异步请求都已完成；
- 所有请求都已被成功取消；
- 所有文件引用都已释放；
- 所有写入都已通过设备 volatile cache；
- 所有数据都已满足数据库恢复协议。

即使不使用 `io_uring`，Linux 的同步阻塞 I/O 也可能持有 open file
description 引用并在另一个线程执行 `close()` 后完成；`close(2)` 的文档明确
记录了这一点。`io_uring` 把同一类生命周期分离扩展到了显式异步请求。

### 2.2 fd、open file description 和 request 不是一个生命周期

需要区分以下对象：

| 对象 | 生命周期结束意味着什么 | 不意味着什么 |
| --- | --- | --- |
| fd number | 进程 fd table 中的槽位可复用 | 原 open file description 已无其他引用 |
| `struct file` / open file description | 最后一个引用释放后执行 `__fput()` | 数据已经满足掉电持久性 |
| `io_kiocb` request | 对应异步操作已完成或取消并释放资源 | 进程仍存在 |
| task / process | 用户代码不再执行，父进程可取得退出状态 | 所有独立内核请求已结束 |
| block request / device command | 块层或设备已报告完成 | 无 volatile cache，或事务已提交 |

对 direct I/O，内核会在请求期间 pin 住作为 DMA buffer 的用户页。Linux
[`pin_user_pages()` 文档](https://docs.kernel.org/core-api/pin_user_pages.html#case-1-direct-io-dio)
把 DIO 明确列为短期 `FOLL_PIN` 场景。因此 task 的虚拟地址空间退出并不会
让设备面对已经释放的物理页。

普通退出路径可概括为：

```text
userspace task                    submitted request
      |                                  |
      | SIGKILL                          | owns kernel references
      v                                  v
   do_exit()                      file + pages + ring ctx
      |                                  |
      v                                  v
io_uring_files_cancel()           block layer / device
      |                                  |
      | waits only tracked requests      | may finish later
      v                                  v
task becomes reapable             release request references
      |                                  |
      v                                  v
 waitpid() returns                 final __fput() may run
```

这里没有用户代码在进程退出后继续执行。后半段由块层、设备完成回调和
`io_uring` 的异步 ring teardown work 推进。

### 2.3 普通退出为什么没有等待所有请求

Linux 6.6 的调用关系是：

```text
do_exit
  |
  v
io_uring_files_cancel
  |
  v
__io_uring_cancel(false)
  |
  v
io_uring_cancel_generic(cancel_all = false)
  |
  v
wait for inflight_tracked, not total inflight
```

Linux 6.6 的
[`io_uring_files_cancel()`](https://github.com/torvalds/linux/blob/v6.6/include/linux/io_uring.h#L67-L74)
传入 `false`。在
[`io_uring_cancel_generic()`](https://github.com/torvalds/linux/blob/v6.6/io_uring/io_uring.c#L3333-L3426)
中，`cancel_all == false` 时读取的是 `inflight_tracked`，不是全部
`inflight`。

截至主线 `7.3-rc2`，这个结构仍然存在：

- [`io_uring_files_cancel()` 仍调用 `__io_uring_cancel(false)`](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/include/linux/io_uring.h#L17-L25)；
- [`cancel_all == false` 仍只用 `inflight_tracked` 决定是否等待](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/cancel.c#L571-L665)；
- [`REQ_F_INFLIGHT` 的源码注释](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/io_uring.c#L334-L345)
  说明 tracked request 用于“文件取消必须找到它”或“请求依赖 `mm` 存活”
  的特殊情形。

普通 raw-block direct write 在请求已拥有所需引用、也不需要 task 的 `mm`
继续存活后，不必属于上述 tracked 集合。task 退出因此不等待它，但 ring
本身也不会被直接释放：

- ring fd release 调用 `io_ring_ctx_wait_and_kill()`；
- 它停止接收新引用，并把 `io_ring_exit_work()` 放到 workqueue；
- exit work 取消可取消请求，并等待 ring refs 最终归零后释放 context。

源码见主线
[`io_ring_exit_work()` 与 `io_ring_ctx_wait_and_kill()`](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/io_uring.c#L2309-L2440)。
这解释了为何 task 可以先被 reap，而请求仍能安全收尾。

### 2.4 为什么 SQPOLL 和 native AIO 表现不同

SQPOLL 有独立内核 submission thread。该线程退出时调用
[`io_uring_cancel_generic(true, sqd)`](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/sqpoll.c#L409-L421)，
`cancel_all == true` 会按 total inflight 等待。原文中的剩余设备延迟因此进入
SQPOLL teardown，间接延长了父进程观察到的 `waitpid()`。

Linux native AIO 绑定于 `mm`。Linux 6.6 的
[`exit_aio()`](https://github.com/torvalds/linux/blob/v6.6/fs/aio.c#L880-L928)
注释和实现都明确表示：最后一个 `mm` 用户消失时，等待该 context 的全部
I/O。显式 [`io_destroy(2)`](https://man7.org/linux/man-pages/man2/io_destroy.2.html)
也会尝试取消所有请求，并等待不能取消的请求完成。

这两个对照说明退出时序是各子系统 teardown policy 的结果，不是
`waitpid()` 提供的统一保证。尤其不能因为某次 SQPOLL 实验表现为 barrier，
就把 `IORING_SETUP_SQPOLL` 当成正确性开关。

## 3. 它为什么不能简单叫 bug 或 feature

### 3.1 作为内核资源模型，它是 feature

异步提交完成后，请求必须拥有执行所需资源的引用。这样才能保证：

- 用户关闭 fd 后，旧请求仍指向原文件，而不是碰巧复用该数字的新文件；
- task 退出后，DMA buffer 不会立即成为可重用内存；
- completion 与 ring teardown 可以在其他执行上下文安全收尾；
- 已经到达设备、无法取消的请求不需要伪装成“已撤销”。

这类引用所有权是异步内核接口正确性的基础。

### 3.2 作为 `waitpid()` 后的可见时序，它不是稳定 feature

Linux UAPI 没有承诺以下任何一项：

- 普通 `io_uring` 请求一定越过 `waitpid()`；
- 哪些 opcode、文件类型或内核版本会越过；
- 退出时是取消还是自然完成；
- SQPOLL 一定在 task reap 前 drain；
- 完成顺序可用来实现事务提交。

因此，应用不能把当前源码细节升级为自己的持久性协议。内核可以在不破坏
`waitpid()` 或 `io_uring` 公共契约的前提下调整取消和 teardown 时序。

### 3.3 什么情况才应认定为内核异常

下列情况与文章现象不同，可能是真 bug：

- 请求完成后仍永久泄漏 file/page/ring 引用；
- teardown 永久卡死，且不是设备本身不返回；
- 请求使用已释放内存，出现 UAF、数据泄露或写错对象；
- completion 违反对应 opcode 已声明的结果语义；
- cancel 与 completion 竞态导致请求既未完成也未释放；
- 文件系统或驱动违反其自身 completion / durability 契约。

“请求比 task 活得久”本身不足以证明 bug；资源泄漏、错误对象访问或违反
UAPI 才是。

## 4. 在实践中有什么用

### 4.1 真正有价值的是资源所有权，不是“死后执行”

请求持有 `struct file` 引用解决了 fd reuse race。假设应用提交 fd `17` 的
写入后立刻关闭它，另一个线程又把 fd `17` 分配给完全不同的文件。如果请求
只保存整数 `17`，后果会是灾难性的；持有原 open file description 则让请求
始终作用于提交时的对象。

同理，pin 住 DIO buffer 让 DMA 生命周期独立于用户虚拟地址映射。这个能力
也支持共享 ring、worker thread 更替和异步 completion，但应用仍必须为
每个请求保留明确的 completion owner。

### 4.2 普通退出可以更及时

某些设备 I/O 已进入不可取消阶段。若每个普通 task 退出都必须同步等待全部
请求，`SIGKILL`、容器回收和故障切换可能被慢设备或故障设备拖住。普通
`io_uring` 的策略让 task 生命周期与 ring/request 清理分离，由异步 teardown
完成收尾。

这是 liveness 优势，也是 correctness 风险转移：内核保证对象不会被提前
释放，但应用必须自己保证新旧 writer 不会重叠破坏数据。

### 4.3 `flock()` 可以把 file reference 变成 drain barrier

原文和 YDB 使用了一个很有价值的派生能力：

1. writer 独立 `open()` 数据文件或块设备；
2. 在这个 open file description 上取得 `LOCK_EX`；
3. 所有 `io_uring` 请求都使用这个 fd；
4. failover 后，新进程再次独立 `open()` 同一对象；
5. 新进程等待自己取得 `LOCK_EX`，再读盘、恢复或写入。

请求持有 writer 的 `struct file`，而 `flock()` 锁属于该 open file
description。Linux 在最后一个 file reference 的
[`__fput()` 中执行 `locks_remove_file()`](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/fs/file_table.c#L484-L505)。
因此，旧请求尚未释放最终引用时，新进程仍拿不到冲突锁。

```text
old writer                      replacement
    |                               |
    v                               |
open data object                    |
    |                               |
    v                               |
flock(LOCK_EX)                      |
    |                               |
    v                               |
submit I/O using same file          |
    |                               |
    X SIGKILL                       |
    |                               |
    +--> retained request refs      |
             |                      v
             |                 separate open()
             |                      |
             |                 flock(LOCK_EX)
             |                      |
             v                      | blocks
       requests drain               |
             |                      |
             v                      v
       final __fput() ----------> lock acquired
                                    |
                                    v
                              recovery and writes
```

YDB 的
[`LockFile()`](https://github.com/ydb-platform/ydb/blob/94a017b78fe62317b7d6d28471db7e296fcc8959/ydb/library/pdisk_io/aio_linux.cpp#L297-L324)
源码注释明确记录了这一用途：前一进程即使已被 `waitpid()` 回收，仍可能因
非 SQPOLL `io_uring` I/O 持锁，直到 I/O 完成或取消。

### 4.4 这个技巧的硬边界

`flock` barrier 只有满足以下条件才成立：

- lock 必须位于被请求引用的**同一个 open file description** 上；
- replacement 必须使用单独的 `open()`，不能只继承或 `dup()` writer 的 fd；
- 所有本机 writer 都必须遵守 advisory lock 协议；
- 不得有无关的继承 fd、fixed-file table 或其他长期引用让锁永久不释放；
- 必须在目标内核、文件系统或块设备路径上做故障注入验证；
- NFS、SMB 等远端文件系统的 `flock` 传播和强制性语义另有差异。

尤其不能使用一个独立 lock file，再让请求写另一个 data file。I/O 请求不会
持有 lock file 的 `struct file`，进程退出时锁会过早释放，失去 drain
效果。

`flock` 还是 advisory lock。拥有权限但不调用 `flock()` 的 writer 可以直接
绕过它。

## 5. 正确的关停与故障切换协议

### 5.1 可控关停：显式 quiesce、cancel、drain、flush

正常升级或进程主动退出时，应使用应用层状态机：

```text
RUNNING
   |
   | reject new submissions
   v
QUIESCING
   |
   | submit cancellation for cancelable work
   v
DRAINING
   |
   | consume every original CQE; pending_count becomes zero
   v
FLUSHING
   |
   | execute the storage engine durability protocol
   v
RELEASED
   |
   | notify supervisor that takeover is safe
   v
EXIT
```

关键点：

- 应用自己维护准确的 `pending_count`，不能只看 SQ/CQ 当前是否为空；
- `IORING_OP_ASYNC_CANCEL` 和 synchronous cancel 仍是 best effort；
- cancel request 的 CQE 不代替原 request 的终态 CQE；
- 已下发硬件的 disk I/O 通常不可取消，必须等待其自然完成；
- drain 只证明请求终态已知，不自动证明数据已经 durable；
- 需要持久性时，在正确写入顺序之后执行 `IORING_OP_FSYNC`、
  `fdatasync()`、FUA 或存储引擎已有的 flush protocol。

[`io_uring_cancelation(7)`](https://man7.org/linux/man-pages/man7/io_uring_cancelation.7.html)
明确说明关闭普通 fd 不会自动取消 pending request，因为请求拥有自己的
file reference；它也说明已经提交到硬件的操作通常不能取消。

### 5.2 不可控退出：用独立 barrier 阻止新旧 writer 重叠

`SIGKILL`、OOM kill 或崩溃无法执行用户态 drain。此时 supervisor 的
正确顺序是：

```text
fence old process from new submissions
    |
    v
send termination signal
    |
    v
waitpid or pidfd observation
    |
    v
acquire I/O-lifetime barrier
    |
    v
validate storage state
    |
    v
run recovery
    |
    v
enable replacement writes
```

对文章限定的“单机、单 writer、同一 raw device/local file”场景，
同一 data fd 上的 `flock` 可以承担 I/O-lifetime barrier。它应当位于
`waitpid()` 之后、任何 recovery read 或 replacement write 之前。

更稳妥的设计还应：

- 给每个进程 incarnation 分配单调递增 epoch；
- 在页、WAL record 或 metadata 中记录 writer epoch；
- takeover 后扫描并拒绝不可能的旧 epoch；
- 在 barrier 前不复用旧 writer 可能仍会覆盖的 LBA；
- 把 `lock_wait_seconds`、旧 epoch 写入和 recovery 校验失败变成告警。

epoch 只能检测 stale write，不能自动阻止它覆盖新数据。真正的防重叠仍要靠
barrier、fencing 或不重叠的物理写入布局。

### 5.3 跨主机 failover 必须使用真正的 fencing

本机 `flock` 不能证明另一台机器已经停止写共享盘。跨主机场景需要根据设备
与部署方式选择：

- distributed lease，并确保 lease token 进入每次写入或提交协议；
- SCSI persistent reservation；
- NVMe reservation；
- 存储服务提供的条件写、generation 或 fencing token；
- STONITH / power fencing；
- 只追加新 epoch 区域，最后原子发布新 root/checkpoint。

进程死亡、容器停止、节点失联和 writer 被存储端拒绝，是四个不同事件。
HA 协议必须把最后一项作为安全边界。

## 6. 不要把这些概念混为一个 barrier

| 观察或操作 | 保证 | 不保证 |
| --- | --- | --- |
| `kill -9` 成功 | signal 已发送或目标不存在 | task 已退出、I/O 已停止 |
| `waitpid()` 返回 | 子进程已进入可回收状态并取得退出信息 | `io_uring` 已 drain |
| 取得文章所述 `flock` | 旧 open file description 的最终引用已释放 | 跨主机 writer 已停止、数据已 durable |
| 收齐 CQE | 每个被跟踪请求已有终态 | device volatile cache 已刷盘 |
| `fsync()` / 正确 flush protocol 成功 | 对应接口承诺范围内的数据已同步到设备 | 数据库事务依赖关系自动正确 |
| distributed fencing 成功 | 旧 owner 后续写入会被共享存储拒绝 | 先前已接受写入已经 durable |

工程协议通常需要其中多个 barrier，不能寻找一个 syscall 同时替代所有层。

## 7. 场景决策表

| 场景 | 建议 |
| --- | --- |
| 正常服务退出或滚动升级 | 停止提交，显式 cancel，收齐原请求 CQE，执行 durability protocol，再通知 supervisor。 |
| 单机数据库进程被 `SIGKILL` 后快速重启 | `waitpid()` 之后再取得与 data I/O 同一 open file description 关联的 `flock`，随后 recovery。 |
| 多进程可能同时写同一 local file | 所有 writer 强制遵守同一个 lock protocol，并用独立 `open()` 竞争。 |
| 共享 SAN/NVMe-oF 上的跨节点切换 | 使用设备级 reservation 或带 fencing token 的分布式协议；不要依赖本机 `flock`。 |
| 希望任务在进程退出后可靠完成 | 把任务交给独立 durable worker/queue；不要把 `io_uring` teardown 当任务系统。 |
| 需要掉电持久性 | 在 drain 之外加入 flush/FUA/WAL-checkpoint 顺序，并验证硬件写缓存策略。 |
| 只是为了获得退出 barrier 考虑启用 SQPOLL | 不建议。性能模式不应成为 correctness contract。 |

## 8. 如何验证自己的生产路径

文章的
[`io_uring_from_grave`](https://github.com/eivanov89/toys/tree/master/cpp/io_uring_from_grave)
是一个好的实验模板，但它会覆盖 raw blocks，必须只在可销毁测试设备上运行。

最低验证矩阵应覆盖：

| 维度 | 建议取值 |
| --- | --- |
| 内核 | 生产版本、升级候选版本 |
| ring mode | ordinary、SQPOLL（若生产使用）、IOPOLL（若生产使用） |
| I/O path | raw block、实际文件系统、实际 mount options |
| write mode | buffered、`O_DIRECT`、`O_DSYNC`/FUA（按生产配置） |
| queue condition | idle、目标 QD、压力与 timeout/fault injection |
| termination | graceful、`SIGTERM` 超时、`SIGKILL`、OOM/cgroup kill |
| fd mode | normal fd、registered fixed file |
| recovery | snapshot、WAL replay、checkpoint、replacement write |

测试必须记录单一 monotonic timeline：

```text
last submission
signal sent
waitpid returned
first lock attempt
lock acquired
first recovery read
second verification read
replacement write enabled
durability barrier completed
```

推荐观测项：

- submitted、completed、canceled、failed 和 unknown request 数；
- cancel CQE 与 original CQE 的结果；
- `waitpid()` latency；
- `flock` 首次 `EWOULDBLOCK` 及最终等待时间；
- writer ID、epoch、LBA 和 generation；
- block-layer issue/complete trace；
- device flush/FUA 数和延迟；
- takeover 前后是否存在旧 epoch 写入。

没有故障注入时，“从未见过问题”通常只说明设备太快，竞态窗口不够大。

## 9. 对数据库内核的最终建议

1. 把 `waitpid()` 从所有“旧 writer 已停止 I/O”的证明链中移除。
2. 把每个 SQE 视为拥有独立生命周期的内核事务，直到 original CQE 到达。
3. 正常退出实现显式 quiesce/drain，不依赖关闭 ring 的隐式行为。
4. `SIGKILL` 路径必须有进程外 barrier；单机场景可评估 YDB 的同-fd
   `flock` 协议。
5. 将 I/O completion barrier、durability barrier 和 writer fencing
   分别建模、分别监控。
6. 不把 SQPOLL 或 native AIO 当前的退出时序当成长期正确性契约。
7. 内核升级验证必须包含进程退出时仍有高 QD 写入的测试，而不只测 steady
   state 性能。

一句话总结：

> `io_uring` 的“逃逸”不是让死进程获得超能力，而是内核让已提交请求按其
> 自己的引用生命周期安全收尾。它对内核是必要机制，对应用则是必须显式
> fencing 的并发事实。

## 参考资料

- [YDB 原文](https://blog.ydb.tech/is-there-i-o-after-death-what-happens-to-io-uring-when-a-process-dies-92c65354873f?postPublishedType=repub)
- [原文实验代码与完整结果](https://github.com/eivanov89/toys/tree/master/cpp/io_uring_from_grave)
- [Linux 6.6 `io_uring` cancellation path](https://github.com/torvalds/linux/blob/v6.6/io_uring/io_uring.c#L3280-L3426)
- [Linux 7.3-rc2 `io_uring` cancellation path](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/cancel.c#L515-L665)
- [Linux 7.3-rc2 ring asynchronous teardown](https://github.com/torvalds/linux/blob/df2908090cda368b01ff43709f51890076c56157/io_uring/io_uring.c#L2309-L2440)
- [Linux 6.6 native AIO `exit_aio()`](https://github.com/torvalds/linux/blob/v6.6/fs/aio.c#L880-L928)
- [`io_uring_cancelation(7)`](https://man7.org/linux/man-pages/man7/io_uring_cancelation.7.html)
- [`waitpid(2)`](https://man7.org/linux/man-pages/man2/wait.2.html)
- [`flock(2)`](https://man7.org/linux/man-pages/man2/flock.2.html)
- [`close(2)`](https://man7.org/linux/man-pages/man2/close.2.html)
- [`fsync(2)`](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [Linux `pin_user_pages()` documentation](https://docs.kernel.org/core-api/pin_user_pages.html)
