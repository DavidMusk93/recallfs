# 从 `mmap` 到 `io_uring`：查询引擎 I/O、内存与并发工程学习报告

> 页面可见署名：Evan Chan，Conviva Engineering。页面 metadata 的 author
> 字段为 Melissa Pieroni；两者不一致，本文以可见署名为准。
>
> Part 1：[We Replaced mmap with io_uring in Our Rust Query Engine. It Got
> Slower.](https://www.conviva.ai/resource/we-replaced-mmap-with-io_uring-in-our-rust-query-engine-it-got-slower/)
> （发布于 2026-09-01，更新于 2026-09-02）
>
> Part 2：[Making io_uring Actually Fast: I/O Threads, Chunking, and the
> Memory Story Nobody Talks About](https://www.conviva.ai/resource/making-io_uring-actually-fast-i-o-threads-chunking-and-the-memory-story-nobody-talks-about/)
> （发布于 2026-09-02）
>
> 阅读日期：2026-09-07。文中标为“原文数据”的数字来自上述两篇文章；
> 标为“工程推导”或“校正”的内容结合 Linux 与 liburing 接口文档得出。

本文的 10 个 C 代码块已使用 FIL-C 0.684 编译，并通过 8 组运行时测试。该
harness 验证的是规划、状态机与策略逻辑，不执行真实内核 `io_uring` 提交、
`O_DIRECT` 设备 I/O 或 `madvise` 集成；这些边界仍需在目标 Linux 环境做
integration test。复验：

```bash
rustc --edition 2021 -D warnings tools/docs/verify_markdown_c.rs \
  -O -o .tmp/verify-markdown-c-bin
.tmp/verify-markdown-c-bin \
  --compiler .tmp/fil-c/bin/filcc \
  --runner .tmp/fil-c/bin/filrun \
  --harness tools/docs/tests/io_uring_query_engine_harness.c \
  --output-dir .tmp/io-uring-report-filc \
  docs/reports/io-uring-query-engine-engineering-notes.md
```

## 0. 结论先行

这两篇文章最重要的结论不是“`io_uring` 比 `mmap` 快”，而是：

> 从 `mmap` 切换到 `io_uring + O_DIRECT`，本质上是把 page cache、预读、
> 排队、公平性、buffer 生命周期和缺页成本的控制权，从内核移交给应用。
> 只有应用把这些职责都实现好，显式 I/O 才可能胜出。

Conviva 的迁移经历了三个阶段：

| 阶段 | 关键设计 | 11/14-day 或 cold query 结果 | 结论 |
| --- | --- | ---: | --- |
| `mmap` 基线 | host-wide page cache，按需缺页 | Linux cold query `13.6 s`；旧 11-day query `63 s` | 轻载快，重并发时 page-cache thrash 与锁竞争恶化尾延迟 |
| 首版 `io_uring` | 单 BMT 线程、约 40 个超大 SQE、Arrow 再复制 | `21.8 s`，比 `mmap` 慢约 `60%` | major fault 降低不等于端到端更快 |
| 最终架构 | 长驻 I/O 线程、分块 SQE、有界流水线、显式缓存、预触页 | 11-day query `49 -> 45 -> 37 s` | I/O、解码、内存准备和缓存必须共同优化 |

原文 production 对比中，旧 `mmap` 四 pod 基线的平均延迟为 `38.7 s`、
`p95 = 118 s`；单 pod、大缓存的 `io_uring` 为 `24.4 s`、
`p95 = 58 s`；native 为 `19.8 s`、`p95 = 48.3 s`。后者相对基线的
`p95` 降幅约为 `59%`。

对数据库和查询引擎开发，最值得带走的是以下九点：

1. **先证明瓶颈属于 page-cache 路径，再迁移。** `io_uring` 不是
   `mmap` 的无条件升级版。
2. **优化目标是端到端关键路径，而不是某一项计数器。** 首版将 major
   fault 从 `128,957` 降至 `3,647`，总耗时却从 `13.6 s` 升至
   `21.8 s`。
3. **请求形状决定设备并行度。** 少量数百 MiB 的大 SQE 与大量中等
   SQE，即使总字节相同，也可能让 RAID/NVMe 看到完全不同的并行机会。
4. **queue depth 必须有界且可解释。** “把所有 future 一次发出”不是
   调度策略；它没有表达内存上限、公平性和 backpressure。
5. **I/O 并发和 CPU 并行应拆开。** 单一 BMT 层同时做提交、等待、
   解码、缓存和查询 API，导致任何等待都能阻塞整条流水线。
6. **`O_DIRECT` 只是绕过 page cache，不会绕过虚拟内存。** DMA
   目标或后续 copy 的新匿名页仍可能产生大量 minor fault。
7. **预触页的价值是移动成本。** `MADV_POPULATE_WRITE` 没有消灭缺页
   工作，而是让它与前一批 I/O 重叠，移出 memcpy 的关键路径。
8. **替换 page cache 就必须补齐 cache。** 原文的小缓存配置平均延迟
   `87.8 s`，比 `mmap` 的 `38.7 s` 差一倍以上。
9. **每个“常识性解释”都要配区分性证据。** 时间线、CQE、in-flight、
   fault、off-CPU、cache hit 和设备利用率，比听起来合理的理论更可靠。

## 1. 场景、负载与问题边界

### 1.1 原文系统

Conviva 的事件和模式分析引擎使用 DataFusion、Arrow、Rust、Rayon 和
Tokio。原始事件编码后保存到云端，查询前复制到本地 NVMe。数据文件是
约 `3-5 GB` 的 Arrow IPC 文件，内存布局和磁盘布局接近，因此原先使用
`mmap` 做按需、近零拷贝访问。

典型一天查询：

- 约 8 个 batch 文件，每个文件一个大 batch；
- 每个 batch 约 `1.6 GB`；
- 查询读取 5-6 列，约 40 次列读取；
- 总数据量约 `13 GB/day`。

测试机器：

| 维度 | 原文配置 |
| --- | --- |
| CPU | 192 cores |
| RAM | 约 750 GB |
| 存储 A | 2 块 NVMe，LVM stripe，`fio` 上限约 `5.5 GB/s` |
| 存储 B | 32 块 NVMe，RAID-0，`fio` 上限约 `21 GB/s` |
| 调查内核 | Linux 5.15 |
| 生产内核 | Linux 6.x |

这个工作负载有三个决定性特征：

1. 单次查询读取量大，足以显著改变 host page cache。
2. 多个 pod 共享同一宿主机 page cache 和内核锁路径。
3. 列范围已知，应用具备显式预取和自建缓存的条件。

因此，本文结论优先适用于“大块、可预测读取 + 高并发 + 强缓存控制需求”的
分析型引擎，不应直接外推到 4 KiB 随机 OLTP、网络 I/O 或内存充足的单进程
扫描。

### 1.2 迁移不是 syscall 替换

`mmap` 路径把复杂度隐藏在内核：

```text
Query load
    |
    v
Virtual address access
    |
    v
Page fault and VMA lookup
    |
    v
Host-wide page cache
    |
    v
Read-ahead, eviction, and block I/O
```

`io_uring + O_DIRECT` 路径把这些职责显式化：

```text
Query planner
    |
    v
Fair scheduler and admission control
    |
    v
Aligned chunk planner
    |
    v
Bounded SQE window
    |
    v
NVMe or RAID
    |
    v
CQE validation and buffer ownership
    |
    v
Decode and application cache
```

`io_uring` 只提供提交队列、完成队列和一组异步操作。它不会替应用决定：

- 哪些列应该先读；
- 一次读多大；
- 同时允许多少字节在途；
- 哪个 query 先获得下一个 slot；
- 目标 buffer 何时预触页；
- cache 多大、淘汰谁；
- CQE 错误、短读、取消和关停如何传播。

## 2. `mmap` 为什么在重并发下失速

### 2.1 原文观察到的症状

真实并发升高后，原文记录了：

- pod 私有分配增长，留给共享 page cache 的内存减少；
- `p95` 从约 `30 s` 升至 `150 s+`；
- 增加 pod 后反而更慢；
- 14-day controlled benchmark 中，1 pod 最快时比 4 pods 快 `41%`，
  `p95` 快超过 `20%`；
- RSS 峰值达到 `734 GB`，约占机器内存 `98.91%`；
- major fault 出现 `571/s`、`1352/s`、`975/s`；
- minor fault 持续达到每秒 `1.25M-2.35M`；
- virtual address space 接近 `3 TB`；
- context switch 超过 `2M/s`，而 warm-cache run 约 `14K/s`。

`perf top` 的 cold run 中，`__filemap_add_folio` 占 `78%`，实际数据处理
只占 `4.96%`；warm run 中数据处理占 `45.08%`。off-CPU 分析还显示
futex `30.9%`、preempted `29.3%`、disk I/O `6.9%`。

这组证据支持的是：

> 在该机器、内核、文件布局和并发模型下，主要瓶颈已经从 NVMe 介质转移到
> page-cache refill、缺页处理、锁竞争和线程调度。

它不支持“`mmap` 普遍慢”。如果 working set 能稳定驻留、并发较低、
访问不可预测或应用不愿自建 cache，`mmap` 仍可能是更简单、更快的方案。

### 2.2 为什么增加 pod 会更差

pod 隔离了部分用户态状态，却没有为每个 pod 创建独立 page cache。四个 pod
仍竞争同一个宿主机资源：

```text
Pod A ----\
Pod B -----+--> Host page cache --> Reclaim and refault
Pod C -----+--> VMA and file-map synchronization
Pod D ----/
```

每个 pod 增加私有 heap 后，共享 cache 可用空间进一步收缩。正在使用的文件页
被淘汰，随后再次访问又触发 major fault，形成 refault storm。此时扩 pod
增加的是竞争者，不是存储带宽。

### 2.3 `fio` 上限为什么不能直接当应用上限

原文的 `fio` 配置是 4 processes、`iodepth=32`、4 MiB block、
`O_DIRECT`，32 盘 RAID-0 达到 `20.2 GiB/s`。压力运行中的 `mmap`
峰值只有 `3.44 GB/s`。

两者差距说明“硬件还有空间”，但不是应用理应得到 `21 GB/s` 的证明：

- `fio` 没有 Arrow decode、cache lookup 和 memcpy；
- 请求形状、文件分布、queue depth 与查询不同；
- warm/cold 状态可能不同；
- `fio` 的 aggregate throughput 不等于单查询 tail latency；
- CPU、NUMA、cgroup 和内存压力也可能限制应用。

正确用法是把 `fio` 当设备层 ceiling 和异常检测基线，再逐层量化损失。

## 3. 首版 `io_uring` 为什么更慢

### 3.1 结果看似矛盾，实际不矛盾

Linux cold benchmark：

| 指标 | `mmap` | 首版 `io_uring` | 变化 |
| --- | ---: | ---: | ---: |
| 总耗时 | `13.6 s` | `21.8 s` | `io_uring` 慢约 `60%` |
| materialize | `0`（成本延迟到缺页） | `17.4 s` | 显式成本出现 |
| pattern pass 1 | `10.0 s` | `17.5 s` | 下游也变慢 |
| major fault | `128,957` | `3,647` | 降低约 `97.2%` |
| minor fault | 约 `1M` | `8.6M` | 增加约 `8.6x` |

major fault 减少只说明绕开文件页 fault 生效，并不说明：

- 请求已充分利用 RAID；
- 用户态没有额外 copy；
- 目标匿名页已建立页表；
- I/O 与 CPU 已流水化；
- cache 命中率足够；
- 调度对多查询公平。

### 3.2 单层承担五种职责

首版 Batch Materialization Layer 同时负责：

1. 接收 `prefetch/materialize`；
2. 打开文件并提交 compio future；
3. 等待 completion；
4. 解码 Arrow；
5. 更新列 cache 并返回数据。

一次 inline `await` 就能阻塞后续提交。日志曾显示每约 10 次提交停顿
`2.5-2.6 s`，三个停顿累计约 `7.8 s`。把 `fd.open()` 移出提交循环后，
open 降至约 `5 ms`，但总查询时间几乎没变。这排除了“open 系统调用很慢”
这个诱人但错误的根因。

最佳实践不是猜某个 syscall，而是记录端到端时间线：

```text
request accepted
    |
    +--> file open start/end
    +--> SQE prepared/submitted
    +--> first CQE/last CQE
    +--> decode start/end
    +--> cache publish
    `--> consumer wakeup
```

任何 `2.5 s` 空洞都应能定位到相邻的两个事件，而不是从 `perf` 中最显眼的
函数反推故事。

### 3.3 少量超大 SQE 没有制造足够的设备并行

首版一次发出约 40 个列请求，但每列只对应一个 SQE，最大约 `650 MB`。
从 API 看有 40 个异步操作，从 RAID 看却未必得到足够多、可独立调度的
中等请求。

原文把它与 `fio` 的 128 个并发 4 MiB SQE 对比，并发现把列切成小块后，
单 ring 可以在 32 盘机器上达到约 `20 GB/s`。关键不是“SQE 数越多越好”，
而是同时满足：

```text
chunk >= amortize_submission_overhead
chunk <= expose_device_parallelism
inflight_bytes <= memory_budget
inflight_sqe <= ring_and_device_capacity
```

“一个大读等价于很多小读”不能从字节总量推出。文件系统、block layer、
RAID、NVMe queue 和控制器都可能在请求边界上表现不同。

### 3.4 Arrow copy 把 fault 从读侧搬到写侧

`Buffer::from_slice_ref` 会分配新内存并复制。若目标是尚未建立页表的匿名页，
memcpy 第一次写每个 4 KiB 页时会触发 minor fault。读取约 `13 GB` 时，
这足以产生数百万次 fault。

所以：

```text
mmap:
file-backed read fault -> page cache -> query

O_DIRECT first cut:
DMA buffer -> allocate Arrow buffer -> write fault during memcpy -> query
```

首版只是从 file-backed fault 迁移到了 anonymous write fault。`perf` 将时间
归到 memcpy，并不代表 DRAM copy 指令本身慢；必须结合 minor-fault 计数和
预触页实验区分。

## 4. 最终有效的架构

### 4.1 长驻、专用 I/O 线程

新设计模仿 `fio`：

- 一个长驻线程拥有 ring；
- 不在 I/O 层使用 future、waker 或 executor；
- 单线程提交 SQE、收割 CQE、补充下一批；
- I/O 只产出原始 DMA slice，不做 Arrow decode；
- 多 query 在 BMT 层 round-robin；
- CPU decode 交给有界的 blocking worker。

这里“非 async I/O 层”是指没有用户态 async runtime；内核 I/O 仍然是异步的。
专用线程不是普适答案。它适合单一、持续、高吞吐的数据管线；大量独立网络与
存储请求可能更适合成熟 async runtime。

### 4.2 分块与有界在途窗口

原文在单文件上扫描 `128 KiB` 到 `64 MiB`，带宽都约 `5.5 GiB/s`；
`512 KiB` 与 `4 MiB` 在最终 utility 中也几乎相同。应将其解释为：

> 在该设备、RAID、queue depth 和顺序读取模式达到饱和后，chunk size
> 不是主导变量。

这不意味着 chunk size 普遍无关。网络块设备、单盘、小 QD、压缩边界、
尾延迟目标和内存上限都可能改变最优值。生产实现应从中等值开始，用 sweep
验证，而不是把 `512 KiB` 或 `4 MiB` 当常量真理。

下面的 C 代码先把任意逻辑范围扩成满足 direct-I/O offset/length 对齐的
物理范围。避雷点是：不能把 page size、filesystem block size 或
`stx_blksize` 猜成 `O_DIRECT` 对齐；Linux 6.1+ 应优先查询
`STATX_DIOALIGN`，并检查返回 mask。

```c
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

struct direct_read_plan {
    uint64_t io_offset;
    uint64_t io_length;
    uint64_t payload_offset;
    uint64_t payload_length;
    uint64_t minimum_completion;
};

static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (out == NULL || UINT64_MAX - a < b)
        return false;
    *out = a + b;
    return true;
}

static bool round_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t remainder;

    if (alignment == 0 || out == NULL)
        return false;
    remainder = value % alignment;
    if (remainder == 0) {
        *out = value;
        return true;
    }
    return checked_add_u64(value, alignment - remainder, out);
}

static bool plan_direct_read(uint64_t offset, uint64_t length,
                             uint64_t file_size, uint64_t offset_alignment,
                             struct direct_read_plan *out)
{
    uint64_t logical_end;
    uint64_t rounded_end;

    if (out == NULL || offset_alignment == 0 || length == 0)
        return false;
    if (!checked_add_u64(offset, length, &logical_end) ||
        logical_end > file_size)
        return false;

    out->io_offset = offset - offset % offset_alignment;
    if (!round_up_u64(logical_end, offset_alignment, &rounded_end))
        return false;
    out->io_length = rounded_end - out->io_offset;
    out->payload_offset = offset - out->io_offset;
    out->payload_length = length;
    out->minimum_completion = logical_end - out->io_offset;
    return out->io_length <= UINT32_MAX;
}
```

`io_length` 是提交给内核的物理字节数，`minimum_completion` 是覆盖逻辑
payload 所需的最少字节数。前者可以跨过 EOF 的对齐边界，因此成功条件不是
“CQE 必须等于 `io_length`”，而是累计完成字节至少覆盖
`minimum_completion`。两者必须同时进入 completion state；若单次范围超过
SQE 的 32-bit `len`，必须继续切块。

分块器还必须与 queue-depth 窗口绑定，不能一次生成整个查询的所有 SQE：

```c
struct chunk_cursor {
    uint64_t next;
    uint64_t end;
    uint32_t chunk_bytes;
};

struct io_chunk {
    uint64_t offset;
    uint32_t length;
};

struct io_window {
    uint32_t limit;
    uint32_t in_flight;
};

static bool chunk_cursor_init(struct chunk_cursor *cursor,
                              uint64_t start, uint64_t length,
                              uint32_t chunk_bytes, uint32_t alignment)
{
    uint64_t end;

    if (cursor == NULL || alignment == 0 || chunk_bytes == 0 ||
        start % alignment != 0 || chunk_bytes % alignment != 0 ||
        !checked_add_u64(start, length, &end) || length == 0 ||
        length % alignment != 0)
        return false;

    cursor->next = start;
    cursor->end = end;
    cursor->chunk_bytes = chunk_bytes;
    return true;
}

static bool next_chunk(struct chunk_cursor *cursor, struct io_chunk *out)
{
    uint64_t remaining;
    uint64_t length;

    if (cursor == NULL || out == NULL || cursor->next == cursor->end)
        return false;
    remaining = cursor->end - cursor->next;
    length = remaining < cursor->chunk_bytes ?
        remaining : cursor->chunk_bytes;
    out->offset = cursor->next;
    out->length = (uint32_t)length;
    cursor->next += length;
    return true;
}

static bool reserve_io(struct io_window *window)
{
    if (window == NULL || window->limit == 0 ||
        window->in_flight >= window->limit)
        return false;
    window->in_flight++;
    return true;
}

static bool release_io(struct io_window *window)
{
    if (window == NULL || window->in_flight == 0)
        return false;
    window->in_flight--;
    return true;
}
```

最佳实践是同时限制 `in_flight_sqe` 与 `in_flight_bytes`。只限制任务数时，
40 个 4 KiB 请求与 40 个 650 MiB 请求具有完全不同的内存风险。

### 4.3 CQE 是状态机输入，不是“完成通知”

`io_uring` completion 默认不保证提交顺序。`cqe->res < 0` 是负 errno，
不是 `-1 + errno`；读操作也可能短读。buffer 不能在提交后、取消请求后或
future 被 drop 后立即复用，必须等拥有该 buffer 的请求到达终态。

下面用 `slot + generation` 编码 `user_data`，拒绝迟到 CQE 对已经复用的
slot 进行写后发布：

```c
enum slot_state {
    SLOT_FREE,
    SLOT_IN_FLIGHT,
    SLOT_COMPLETE,
    SLOT_FAILED
};

enum cqe_action {
    CQE_STALE,
    CQE_RESUBMIT,
    CQE_COMPLETE,
    CQE_FAILED
};

struct io_slot {
    uint32_t index;
    uint32_t generation;
    uint64_t submitted_bytes;
    uint64_t minimum_completion;
    uint64_t completed_bytes;
    uint32_t offset_alignment;
    uint32_t memory_alignment;
    int error_number;
    enum slot_state state;
};

static uint64_t make_user_data(uint32_t index, uint32_t generation)
{
    return ((uint64_t)generation << 32) | index;
}

static uint32_t user_data_index(uint64_t token)
{
    return (uint32_t)token;
}

static uint32_t user_data_generation(uint64_t token)
{
    return (uint32_t)(token >> 32);
}

static uint64_t begin_io(struct io_slot *slot, uint64_t submitted_bytes,
                         uint64_t minimum_completion,
                         uint32_t offset_alignment,
                         uint32_t memory_alignment)
{
    if (slot == NULL || slot->state != SLOT_FREE ||
        submitted_bytes == 0 || minimum_completion == 0 ||
        minimum_completion > submitted_bytes || offset_alignment == 0 ||
        memory_alignment == 0 ||
        submitted_bytes % offset_alignment != 0)
        return 0;
    slot->generation++;
    if (slot->generation == 0)
        slot->generation = 1;
    slot->submitted_bytes = submitted_bytes;
    slot->minimum_completion = minimum_completion;
    slot->completed_bytes = 0;
    slot->offset_alignment = offset_alignment;
    slot->memory_alignment = memory_alignment;
    slot->error_number = 0;
    slot->state = SLOT_IN_FLIGHT;
    return make_user_data(slot->index, slot->generation);
}

static enum cqe_action apply_read_cqe(struct io_slot *slot,
                                      uint64_t token, int32_t result)
{
    uint64_t completed;
    uint64_t remaining_physical;

    if (slot == NULL || slot->state != SLOT_IN_FLIGHT ||
        user_data_index(token) != slot->index ||
        user_data_generation(token) != slot->generation)
        return CQE_STALE;

    if (result < 0) {
        slot->error_number = -result;
        slot->state = SLOT_FAILED;
        return CQE_FAILED;
    }

    remaining_physical = slot->submitted_bytes - slot->completed_bytes;
    if (result == 0 || (uint64_t)result > remaining_physical) {
        slot->error_number = result == 0 ? EIO : EOVERFLOW;
        slot->state = SLOT_FAILED;
        return CQE_FAILED;
    }

    completed = slot->completed_bytes + (uint32_t)result;
    if (completed >= slot->minimum_completion) {
        slot->completed_bytes = completed;
        slot->state = SLOT_COMPLETE;
        return CQE_COMPLETE;
    }

    if ((uint32_t)result % slot->offset_alignment != 0 ||
        (uint32_t)result % slot->memory_alignment != 0) {
        slot->error_number = EIO;
        slot->state = SLOT_FAILED;
        return CQE_FAILED;
    }

    slot->completed_bytes = completed;
    return CQE_RESUBMIT;
}
```

真实实现还要把 file offset、buffer offset、剩余长度和 query/cancel token
放入 request context。只有不足 `minimum_completion` 且短读长度同时满足
direct-I/O offset alignment 与 memory alignment 时，推进后的 offset 和
buffer 才仍可直接重提物理余量。覆盖逻辑 payload 的 EOF-tail 短读直接成功；
不足且未对齐的短读必须失败，不能构造一个必然返回 `EINVAL` 的 `O_DIRECT`
重试。若请求已取消，也应先消费其最终 CQE，再归还 slot。

### 4.4 buffer arena 必须表达所有权

原文最终使用 round-robin `mmap`-backed arena，读取期间不再分配。一个
buffer 的合法生命周期应由状态机而不是注释维持：

```c
enum buffer_state {
    BUFFER_FREE,
    BUFFER_PREWARMING,
    BUFFER_READY_FOR_IO,
    BUFFER_IN_IO,
    BUFFER_DECODING,
    BUFFER_STATE_COUNT
};

static bool transition_buffer(enum buffer_state *state,
                              enum buffer_state next)
{
    bool valid;

    if (state == NULL)
        return false;

    valid =
        (*state == BUFFER_FREE && next == BUFFER_PREWARMING) ||
        (*state == BUFFER_PREWARMING && next == BUFFER_READY_FOR_IO) ||
        (*state == BUFFER_READY_FOR_IO && next == BUFFER_IN_IO) ||
        (*state == BUFFER_IN_IO && next == BUFFER_DECODING) ||
        (*state == BUFFER_DECODING && next == BUFFER_FREE);
    if (!valid)
        return false;
    *state = next;
    return true;
}

static bool buffer_may_be_reused(enum buffer_state state)
{
    return state == BUFFER_FREE;
}
```

这个模型刻意禁止：

- `BUFFER_IN_IO -> BUFFER_FREE`：取消提交不等于 DMA 已停止；
- `BUFFER_IN_IO -> BUFFER_PREWARMING`：同一内存不能边 DMA 边改页；
- `BUFFER_DECODING -> BUFFER_IN_IO`：消费者尚未释放时不能覆盖；
- 绕过 prewarm 直接提交：若选择了预触页策略，就必须保持其时序。

### 4.5 调度公平性属于正确性

单 query 跑满 ring 可以得到漂亮 throughput，却可能让其他 query 的
tail latency 饥饿。原文在 BMT 中按活跃 query round-robin。最小模型如下：

```c
#define MAX_QUERIES 16U

struct query_work {
    uint32_t pending_chunks;
};

struct fair_queue {
    struct query_work query[MAX_QUERIES];
    uint32_t count;
    uint32_t next_index;
};

static int pick_next_query(struct fair_queue *queue)
{
    uint32_t checked;

    if (queue == NULL || queue->count == 0 || queue->count > MAX_QUERIES)
        return -1;

    for (checked = 0; checked < queue->count; checked++) {
        uint32_t index = (queue->next_index + checked) % queue->count;
        if (queue->query[index].pending_chunks != 0) {
            queue->query[index].pending_chunks--;
            queue->next_index = (index + 1) % queue->count;
            return (int)index;
        }
    }
    return -1;
}
```

生产调度器通常还需要 deadline、权重、取消、tenant quota 和 scan cost，
但 round-robin 已经比“先来的大查询占满全部 SQE”更接近可控系统。

## 5. 内存策略才是另一半

### 5.1 `MADV_POPULATE_WRITE` 做了什么

原文的 100 MB microbenchmark：

| 配置 | prewarm | copy | copy throughput |
| --- | ---: | ---: | ---: |
| 4K pages，无 prewarm | `0` | `34.21 ms` | `2928 MB/s` |
| 4K pages，`MADV_POPULATE_WRITE` | `14.67 ms` | `9.95 ms` | `10068 MB/s` |
| HUGETLB，无 prewarm | `0` | `27.32 ms` | `3661 MB/s` |
| HUGETLB，`MADV_POPULATE_WRITE` | `10.27 ms` | `7.67 ms` | `13045 MB/s` |
| 4K prewarm + parallel copy | `14.00 ms` | `1.99 ms` | `51207 MB/s` |

4K pages 的总工作量约为：

```text
No prewarm:        34.21 ms
Prewarm then copy: 14.67 + 9.95 = 24.62 ms
```

主要收益不是 advice 调用凭空消除了工作，而是：

```text
Stage N disk read        ---------->
Stage N+1 page prewarm   ---------->
Stage N-1 decode/copy    ---------->

Critical path becomes max(stage costs), not sum(stage costs).
```

Linux 文档说明，`MADV_POPULATE_WRITE` 自 5.14 提供，对范围进行可写预填充；
地址需按页对齐，调用仍可能因内存、映射或权限问题失败。它必须被当成可观测、
可降级的步骤，不能忽略返回值。

```c
enum prewarm_result {
    PREWARM_OK,
    PREWARM_UNSUPPORTED,
    PREWARM_FAILED
};

typedef enum prewarm_result (*populate_write_operation)(
    void *address, size_t length, void *context);

static enum prewarm_result prewarm_for_write_with(
    void *address, size_t length, populate_write_operation operation,
    void *context)
{
    enum prewarm_result result;

    if (address == NULL || length == 0)
        return PREWARM_FAILED;
    if (operation == NULL)
        return PREWARM_UNSUPPORTED;

    result = operation(address, length, context);
    return result == PREWARM_OK || result == PREWARM_UNSUPPORTED ?
        result : PREWARM_FAILED;
}

#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
static enum prewarm_result invoke_madvise_populate_write(
    void *address, size_t length, void *context)
{
    (void)context;
    return madvise(address, length, MADV_POPULATE_WRITE) == 0 ?
        PREWARM_OK : PREWARM_FAILED;
}
#endif

static enum prewarm_result prewarm_for_write(void *address, size_t length)
{
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
    return prewarm_for_write_with(address, length,
                                  invoke_madvise_populate_write, NULL);
#else
    return prewarm_for_write_with(address, length, NULL, NULL);
#endif
}
```

注入点让 FIL-C 在任何宿主头文件上都能执行 success、failure 和 unsupported
策略分支；`invoke_madvise_populate_write()` 只是生产 adapter。harness 不把
fake operation 的通过解释为真实内核 `madvise` 已验证。

避雷：

- `MADV_SEQUENTIAL` 面向读取模式，不等于为匿名目标页建立可写页表；
- `MADV_COLLAPSE` 处理当前页状态，可能先 fault-in，不能替代前置预触页；
- prewarm 若与 copy 串行，墙钟收益会显著缩小；
- 预触页会提前兑现物理内存承诺，可能把延迟问题变成 OOM 问题。

### 5.2 HUGETLB、THP 与普通页不是同一保证

原文报告 HUGETLB 在其机器上额外改善约 `30-40%`。Linux 接口边界是：

| 机制 | 保证 | 主要代价 |
| --- | --- | --- |
| `MAP_HUGETLB` | 显式 HugeTLB page | 需预留、权限、对齐；不足时映射失败 |
| `MADV_HUGEPAGE` / THP | 给内核的 promotion 建议 | 不保证提升；内存压力下可能不发生 |
| `MADV_COLLAPSE` | best-effort 同步 collapse | 可能 fault-in、reclaim、compaction |
| 普通 4K + `MADV_POPULATE_WRITE` | 明确预填充可写页表 | TLB 覆盖较小，但部署最普遍 |

最佳实践是显式分级：

```text
Try HUGETLB arena
    |
    +-- success --> record huge-page metrics
    |
    `-- failure --> 4K arena + populate-write
                         |
                         `--> record fallback reason
```

不要静默 fallback，否则线上曲线变化时无法区分 workload 回归与 hugepage
供应失败。

### 5.3 缓存预算必须先算再分

原文最终使用过 `150 GB L1 / 200 GB L2 / 32 GB ring`，production 大缓存
配置则描述为 `200 GB L1 / 250 GB L2`。这些是特定 750 GB 机器的观测值，
不是推荐默认值。

缓存、ring、解码 scratch、query heap 和系统余量应进入同一预算：

```c
struct memory_budget {
    uint64_t physical_bytes;
    uint64_t process_base_bytes;
    uint64_t ring_bytes;
    uint64_t l1_bytes;
    uint64_t l2_bytes;
    uint64_t decode_bytes;
    uint64_t required_headroom_bytes;
};

static bool cache_budget_fits(const struct memory_budget *budget,
                              uint64_t *committed_bytes)
{
    uint64_t total = 0;
    size_t index;

    if (budget == NULL || committed_bytes == NULL ||
        budget->physical_bytes == 0)
        return false;

    const uint64_t parts[] = {
        budget->process_base_bytes,
        budget->ring_bytes,
        budget->l1_bytes,
        budget->l2_bytes,
        budget->decode_bytes,
        budget->required_headroom_bytes
    };

    for (index = 0; index < sizeof(parts) / sizeof(parts[0]); index++) {
        if (!checked_add_u64(total, parts[index], &total))
            return false;
    }
    *committed_bytes = total;
    return total <= budget->physical_bytes;
}

static double cache_hit_rate(uint64_t hits, uint64_t misses)
{
    uint64_t accesses;

    if (!checked_add_u64(hits, misses, &accesses) || accesses == 0)
        return 0.0;
    return (double)hits / (double)accesses;
}
```

仅仅“不 OOM”还不够。缓存大小必须同时由 reuse-distance 分布、并发查询
working set、NUMA topology 和目标 hit rate 驱动。原文部署后 L1 hit rate
稳定超过 `75%`，这是架构成立的重要证据。

## 6. `io_uring` 实现避雷清单

### 6.1 `O_DIRECT` 不是固定 4 KiB 对齐

Linux `open(2)` 明确指出，`O_DIRECT` 的内存地址、长度和文件 offset
限制可能随 filesystem 和 kernel 变化；不对齐可能返回 `EINVAL`，也可能
退回 buffered I/O。Linux 6.1 起，支持的 filesystem 可通过
`statx(..., STATX_DIOALIGN, ...)` 返回：

- `stx_dio_mem_align`：用户 buffer 对齐；
- `stx_dio_offset_align`：offset 与 segment length 对齐；
- `stx_dio_read_offset_align`：较新内核可单列读对齐。

必须检查 `stx_mask`，字段为零或未返回时再走 filesystem-specific fallback。
`stx_blksize` 只是 preferred I/O block size，不是 direct-I/O 合同。

此外：

- 不要并发混用 overlapping buffered I/O、direct I/O 和 `mmap`；
- `O_DIRECT` 不提供 `O_SYNC` 的持久化保证；
- 带 private mapping 的 direct I/O 不应与 `fork()` 并发；
- 文件尾部和未对齐逻辑列需要 bounce buffer 与 payload slice。

### 6.2 不要把 SQE 当成已完成请求

提交成功只代表内核消费了 SQE，不能复用其数据 buffer。可靠生命周期是：

```text
FREE -> PREWARM -> READY -> SUBMITTED -> CQE -> DECODE -> FREE
```

以下情况都不能提前复用：

- future 被 drop；
- query 被取消；
- timeout CQE 到达；
- cancel 请求已提交；
- ring 正在 shutdown。

取消与原请求可能各产生 CQE。安全关停必须停止 admission、提交 cancel、
持续 drain CQ，最后才释放注册文件、注册 buffer 和 ring。

### 6.3 不要假定完成顺序

同一批 SQE 的 completion 默认乱序。即使“先 write 再 fsync”按此顺序放入 SQ，
两者也可能并行。需要顺序时使用 linked SQE、drain，或先等待依赖 CQE 再提交。

对查询读取，乱序本来是优势；正确做法是让 `user_data` 标识
`query/batch/column/chunk/generation`，按完成粒度发布，而不是等待 40 列
全部完成后再统一处理。

### 6.4 短读和负 errno 必须是正常控制流

前文的 `apply_read_cqe()` 展示了三类结果：

| `cqe->res` | 含义 | 动作 |
| ---: | --- | --- |
| `< 0` | `-errno` | 记录请求级错误，不能读取全局 `errno` |
| `0` 且仍需数据 | EOF/异常短读 | 失败，避免无限 resubmit |
| `0 < res < minimum` 且满足 offset/memory alignment | 可续传短读 | 推进 offset/buffer 后重提物理余量 |
| `0 < res < minimum` 且任一未对齐 | 不可安全续传 | 失败；不得发出未对齐 `O_DIRECT` 重试 |
| `minimum <= res <= submitted` | 完整 payload 或 EOF-tail | 转入 decode/publish |

`user_data` 还要带 generation，防止旧 CQE 命中新一轮已复用的 slot。

### 6.5 ring 参数不是“全部打开”

- `SQPOLL` 可能减少提交 syscall，也会占用 CPU；原文中无可测收益。
- `IOPOLL` 要求设备/filesystem 支持 polling，并通常要求 `O_DIRECT`。
- registered buffers 可避免每次 I/O pin/map 和 page refcount 操作，但引入
  固定内存、注册更新和生命周期复杂度。
- opcode 与 feature 随 kernel 版本变化，应使用 probe，不要只判断
  `uname` 版本。
- CQ 容量、overflow、submit 返回数和 backpressure 都必须监控。

原则是先用最小 ring 达到正确、可观测的 pipeline，再逐项打开 feature，
每项都要有独立 A/B 证据。

## 7. 测量方法：从“快了”变成可归因

### 7.1 四层指标

| 层次 | 必测指标 | 回答的问题 |
| --- | --- | --- |
| Query | avg、p50、p95、p99、错误率、扫描行数/core | 用户结果是否改善 |
| Pipeline | admission wait、SQE in-flight、CQE latency、decode queue | 时间堵在哪一层 |
| Memory | major/minor fault、RSS、cache hit、arena reuse、NUMA miss | I/O 是否把成本转移到内存 |
| Device | bytes/s、IOPS、await、util、每盘分布 | 请求形状是否喂满设备 |

吞吐和降幅必须用明确分母：

```c
static double gib_per_second(uint64_t bytes, uint64_t elapsed_ns)
{
    const double gib = 1024.0 * 1024.0 * 1024.0;
    if (elapsed_ns == 0)
        return 0.0;
    return ((double)bytes / gib) / ((double)elapsed_ns / 1000000000.0);
}

static double reduction_fraction(double before, double after)
{
    if (before <= 0.0 || after < 0.0)
        return 0.0;
    return (before - after) / before;
}
```

按原文数字：

- 首版 major fault 降幅约 `97.17%`；
- 首版 latency 回退约 `60.29%`；
- native `p95` 相对旧 `mmap` 降幅约 `59.07%`；
- large-cache pod `p95` 相对旧 `mmap` 降幅实际约 `50.85%`。

最后两项揭示原文的一处表述混淆：表格中 `-59%` 对应 native `48.3 s`，
不是 large-cache pod 的 `58 s`。报告采用表格原始数字，不把二者合并。

### 7.2 对比实验必须锁定环境

“同代码”不等于“同实验”。至少记录：

```c
enum io_path_kind {
    IO_PATH_MMAP,
    IO_PATH_BUFFERED_READ,
    IO_PATH_URING_DIRECT
};

struct benchmark_identity {
    const char *query_mix_id;
    const char *dataset_layout_id;
    const char *host_storage_profile_id;
    enum io_path_kind io_path;
    uint32_t device_count;
    uint32_t query_concurrency;
    uint32_t pod_count;
    uint32_t queue_depth;
    uint64_t dataset_bytes;
    uint64_t cache_bytes;
    bool huge_tlb;
    bool native_process;
};

static bool stable_identity_is_present(const char *identity)
{
    return identity != NULL && identity[0] != '\0';
}

static bool same_workload(const struct benchmark_identity *a,
                          const struct benchmark_identity *b)
{
    if (a == NULL || b == NULL ||
        !stable_identity_is_present(a->query_mix_id) ||
        !stable_identity_is_present(b->query_mix_id) ||
        !stable_identity_is_present(a->dataset_layout_id) ||
        !stable_identity_is_present(b->dataset_layout_id))
        return false;
    return strcmp(a->query_mix_id, b->query_mix_id) == 0 &&
        strcmp(a->dataset_layout_id, b->dataset_layout_id) == 0 &&
        a->query_concurrency == b->query_concurrency &&
        a->dataset_bytes == b->dataset_bytes;
}

static bool isolates_io_path(const struct benchmark_identity *a,
                             const struct benchmark_identity *b)
{
    if (!same_workload(a, b))
        return false;
    if (!stable_identity_is_present(a->host_storage_profile_id) ||
        !stable_identity_is_present(b->host_storage_profile_id))
        return false;
    return strcmp(a->host_storage_profile_id,
                  b->host_storage_profile_id) == 0 &&
        a->device_count == b->device_count &&
        a->pod_count == b->pod_count &&
        a->queue_depth == b->queue_depth &&
        a->cache_bytes == b->cache_bytes &&
        a->huge_tlb == b->huge_tlb &&
        a->native_process == b->native_process &&
        a->io_path != b->io_path;
}
```

这些 ID 应来自版本化的 canonical 配置或内容摘要，而不是进程内指针、临时
路径或自由文本。`query_mix_id` 固定查询类型及权重，
`dataset_layout_id` 固定文件/列/分片布局，`host_storage_profile_id`
固定 CPU、kernel、filesystem、RAID/NVMe 与 NUMA 配置；聚合字节数相同不能
替代这些身份。`isolates_io_path()` 还要求 `io_path` treatment 不同；
枚举能区分 `mmap`、buffered read 和 `io_uring + O_DIRECT`，避免用一个
`direct_io` 布尔值把多种非 direct 路径混成同一组；两个完全相同的实验也
不构成 I/O path 对照。

原文 production 表格是“旧四 pod 架构”与“新单 pod/native 架构”的现实系统
比较，适合回答“新系统是否更好”，但不能单独回答“仅更换 I/O API 带来多少”。
后者需要 `isolates_io_path()` 意义上的控制变量实验。

### 7.3 warm-up 必须解释，不能丢弃

原文 utility 的前两 pass 只有约 `3.6/5.2 GB/s`，后续达到
`5.5/20 GB/s`。作者最终认为主要是 page initialization。更严谨的报告应
同时保留：

- first-touch cold；
- ring/thread warm；
- cache warm；
- steady state；
- recycled-buffer steady state。

不要笼统删除“前两次异常值”。如果生产会频繁重启、扩容或更换 arena，
cold cost 就是用户真实成本。

## 8. 上线门禁

### 8.1 决策矩阵

| 条件 | 倾向 `mmap` | 倾向 `io_uring + O_DIRECT` |
| --- | --- | --- |
| working set | 可稳定驻留 page cache | 明显大于可用 cache，频繁 refault |
| 访问模式 | 随机、难预测 | 大块、列范围可预测 |
| 多租户 | 竞争较弱 | host cache 互相污染严重 |
| 工程投入 | 需要简单可靠 | 能承担自建 cache、buffer、调度 |
| tail latency | page fault 可接受 | 需要显式 admission 和资源隔离 |
| 部署 | 通用容器环境 | 可控制 hugepage、NUMA、CPU pinning |

只有右列证据占主导时，迁移才合理。

### 8.2 分阶段门禁

1. **基线门禁**：相同 query mix 下采集 latency、fault、context switch、
   cache 与 device ceiling。
2. **I/O utility 门禁**：不接 Arrow，只验证 aligned chunks 能达到预期设备
   带宽，且 QD sweep 可解释。
3. **memory utility 门禁**：分别测 fresh、prewarmed、recycled、4K、
   THP/HUGETLB 和单/多线程 copy。
4. **pipeline 门禁**：验证 I/O、prewarm、decode 真正重叠，而非串行相加。
5. **cache 门禁**：用生产 reuse-distance 和并发 working set 决定容量。
6. **shadow/canary 门禁**：比较 p95/p99、错误率、OOM、fallback 和资源成本。
7. **回滚门禁**：保留 `mmap` 或 buffered path，按 feature probe 和运行时
   指标降级。

最小上线判定应同时约束性能和资源：

```c
struct rollout_sample {
    double p95_ms;
    double p99_ms;
    double error_rate;
    double cache_hit_rate;
    uint64_t rss_bytes;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t completed_samples;
    bool telemetry_complete;
};

static bool valid_rollout_sample(const struct rollout_sample *sample)
{
    if (sample == NULL || !sample->telemetry_complete ||
        sample->completed_samples == 0 ||
        !isfinite(sample->p95_ms) || !isfinite(sample->p99_ms) ||
        !isfinite(sample->error_rate) ||
        !isfinite(sample->cache_hit_rate))
        return false;
    return sample->p95_ms > 0.0 &&
        sample->p99_ms >= sample->p95_ms &&
        sample->error_rate >= 0.0 && sample->error_rate <= 1.0 &&
        sample->cache_hit_rate >= 0.0 &&
        sample->cache_hit_rate <= 1.0 &&
        sample->rss_bytes > 0;
}

static bool rollout_is_better(const struct rollout_sample *baseline,
                              const struct rollout_sample *candidate,
                              uint64_t rss_limit,
                              double required_hit_rate)
{
    if (!valid_rollout_sample(baseline) ||
        !valid_rollout_sample(candidate) || rss_limit == 0 ||
        !isfinite(required_hit_rate) ||
        required_hit_rate < 0.0 || required_hit_rate > 1.0)
        return false;
    return candidate->p95_ms < baseline->p95_ms &&
        candidate->p99_ms <= baseline->p99_ms &&
        candidate->error_rate <= baseline->error_rate &&
        candidate->cache_hit_rate >= required_hit_rate &&
        candidate->rss_bytes <= rss_limit;
}
```

采集器只有在所有字段都到齐时才能设置 `telemetry_complete`。判定先验证
telemetry domain 再做优劣比较：未完成采集、完成样本数为零、非有限
latency、乱序 percentile、越界 rate 或零 RSS 都视为缺失/损坏并 fail
closed。不能用零值代替采集失败，否则坏 telemetry 会被误判为性能提升。

这里没有把 fault 数量设为硬性越低越好，因为文章已经证明：fault 降低可能与
总耗时回退同时发生。fault 是解释指标，latency、错误率与资源上限才是结果门禁。

## 9. 推荐落地顺序

面向类似数据库/查询引擎，推荐按以下顺序推进：

1. **记录完整时间线。** 在改架构前给 open、submit、first/last CQE、
   decode、publish、consumer wakeup 打点。
2. **建立设备 ceiling。** 用真实文件、direct I/O、QD/chunk sweep，而非
   只测裸设备。
3. **做最小 I/O utility。** 单 ring、单 owner thread、无 async runtime、
   无 cache、无 decode。
4. **实现 direct-I/O capability probe。** 查询对齐，验证 filesystem 与
   opcode，保留可观测 fallback。
5. **实现有界 chunk scheduler。** 同时限制 SQE 数和 bytes，加入 query
   fairness。
6. **实现显式 buffer 状态机。** generation、取消、短读、错误、drain
   都经过 CQE。
7. **单独优化 memory path。** 测 first-touch；能重叠时使用
   `MADV_POPULATE_WRITE`；HUGETLB 作为显式能力，不作为假设。
8. **再接 Arrow/decode。** 先确认 copy 次数与 buffer ownership，再选择
   zero-copy 或 prewarmed copy。
9. **按 reuse-distance 配 cache。** 小 cache 可能比 page cache 更差。
10. **最后评估高级 ring feature。** fixed buffers、registered files、
    SQPOLL/IOPOLL 逐项 A/B。

## 10. 不能从两篇文章直接推出什么

1. **不能推出 `io_uring` 总比 `mmap` 快。** 文章恰好展示了相反的首版结果。
2. **不能推出 `mmap` 的 page cache 一定在 4 pods 下竞争。** 这取决于
   working set、内存限制、kernel、VMA 数量和访问重叠。
3. **不能推出 chunk size 永远不重要。** 原文只证明该设备在饱和区间内
   对 `128 KiB-64 MiB` 不敏感。
4. **不能推出 giant SQE 在所有 RAID 上都会串行。** 这是原文 workload
   的实验归因，需在目标 block stack 上用 trace/benchmark 复验。
5. **不能推出前两 pass 慢是固定的 ring warm-up。** 原文后续证据更接近
   destination page initialization。
6. **不能推出 zero-copy 一定优于 copy。** 破坏 Arrow ownership/invariant
   的手工 buffer 构造可能增加正确性成本；prewarmed copy 可能更稳健。
7. **不能推出 Tokio 普遍优于 Rayon。** 原文比较的是有界、嵌套协调型任务；
   Rayon 仍适合结构良好的 fork-join CPU 工作。
8. **不能推出 HUGETLB 在容器中不可用。** 它可配置，但需要 node reservation、
   pod request/limit、NUMA 与调度共同支持。
9. **不能把 production 表格当单变量 benchmark。** pod 数、cache、hugepage
   和 native/container 同时变化。

## 11. 一页检查表

| 阶段 | 必须回答 |
| --- | --- |
| 立项 | page-cache contention 是否有 fault/off-CPU/lock 证据？ |
| 对齐 | `STATX_DIOALIGN` 是否返回？fallback 是什么？ |
| 分块 | chunk/QD sweep 是否覆盖目标设备与真实文件？ |
| 排队 | SQE 数和在途字节是否同时有上限？ |
| 完成 | 负 errno、短读、乱序、取消和迟到 CQE 是否测试？ |
| 内存 | fresh/prewarm/reuse 的 fault 与 copy 时间是否分开？ |
| cache | 容量是否来自 reuse-distance，而非“剩余内存”？ |
| 并发 | query 间是否公平？decode 是否会耗尽 arena？ |
| 部署 | hugepage、NUMA、CPU pinning、cgroup 差异是否可观测？ |
| 验证 | cold/warm、单 query/并发、avg/p95/p99 是否齐全？ |
| 回滚 | feature probe 失败或 cache hit 不足时能否退回？ |

## 12. 参考资料

- [Conviva Part 1：We Replaced mmap with io_uring in Our Rust Query Engine. It Got Slower.](https://www.conviva.ai/resource/we-replaced-mmap-with-io_uring-in-our-rust-query-engine-it-got-slower/)
- [Conviva Part 2：Making io_uring Actually Fast](https://www.conviva.ai/resource/making-io_uring-actually-fast-i-o-threads-chunking-and-the-memory-story-nobody-talks-about/)
- [Linux `open(2)`：`O_DIRECT` 语义、对齐与混用限制](https://man7.org/linux/man-pages/man2/open.2.html)
- [Linux `statx(2)`：`STATX_DIOALIGN`](https://man7.org/linux/man-pages/man2/statx.2.html)
- [Linux `io_uring_setup(2)`：ring feature 与 polling 约束](https://man7.org/linux/man-pages/man2/io_uring_setup.2.html)
- [Linux `io_uring_enter(2)`：提交、完成与顺序语义](https://man7.org/linux/man-pages/man2/io_uring_enter.2.html)
- [liburing `io_uring_prep_read(3)`：CQE result 语义](https://man7.org/linux/man-pages/man3/io_uring_prep_read.3.html)
- [liburing `io_uring_register_buffers(3)`：fixed buffer 成本模型](https://man7.org/linux/man-pages/man3/io_uring_register_buffers.3.html)
- [liburing `io_uring_get_probe_ring(3)`：运行时 opcode probe](https://man7.org/linux/man-pages/man3/io_uring_get_probe_ring.3.html)
- [Linux `madvise(2)`：populate、THP 与 collapse](https://man7.org/linux/man-pages/man2/madvise.2.html)
- [Linux `mmap(2)`：`MAP_HUGETLB` 与 `MAP_POPULATE`](https://man7.org/linux/man-pages/man2/mmap.2.html)
