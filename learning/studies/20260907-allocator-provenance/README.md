# 从 Go `memclr` 优化抽象 Allocator Engineering

> 原文：[Go 内存分配器探秘：一次提升内存分配效率 26% 的优化实践](https://mp.weixin.qq.com/s/Zvbr8Q1Z4KxEMktISqE1Zw)
>
> 阅读日期：2026-09-07。原文 benchmark 数据只作为来源记录；本文用 C +
> FIL-C 验证可迁移的 correctness contract，不宣称复现 Go 的性能结果。

## 1. 结论先行

这篇 Go 实践可以抽象成一个独立技能，但技能不应叫“更换 allocator”，而应叫
**Allocator Engineering**：

> 先识别 allocation 的 owner、lifetime、size、access、concurrency 和
> topology，再减少 allocation count、选择 domain allocator，并把
> initialization provenance 一直传到最终决策点；最后分别验证 correctness、
> allocator 指标、CPU 成本和生产 RSS。

原文最重要的迁移价值是“不要丢失下层已经知道的事实”。Go allocation path
已经知道一个 span 有多少 bytes 被 scavenged，但旧 `initSpan` 没拿到这条
信息，只能保守地再次 `memclr`。上游改动传入 allocation transaction 已经
计算出的 `scav` bytes，避免事后重扫 metadata；仅在**整个 span** 都被标记为
scavenged，且平台/release mode 保证后续访问得到零页时跳过软件清零。

对 C 和 Rust，实践优先级如下：

1. **先减少 allocation count，再换 allocator。** 合并对象、复用 buffer、
   batch lifetime、arena/slab 通常比替换 `malloc` 后端更直接。
2. **把 initialization 变成显式 contract。** 区分 uninitialized、dirty
   reused、fresh zeroed、explicitly zeroed 和 OS-reclaimed zeroed。
3. **只凭可证明 provenance 跳过清零。** “上次看到是零”不是证据；
   successful full-range reclaim 或本轮完整覆盖才是证据。
4. **allocator 必须匹配 lifetime。** Arena 适合同生共死；slab 适合同尺寸；
   general allocator 适合任意 lifetime；page allocator 适合大块区域。
5. **Rust 不会自动消除 allocator 成本。** Ownership 防止很多 lifetime
   错误，但 `Box`、`Vec`、`String`、`Arc` 和 collection 仍可能产生大量
   allocation、reallocation、fragmentation 和 cache miss。
6. **RSS 不是 layout，layout 也不是 allocator。** 必须同时测 requested、
   usable、live、active/resident、RSS/PSS、CPU、page fault 和 tail latency。

## 2. 原文机制及其边界

### 2.1 Go 的分层结构

原文描述的 `mcache -> mcentral -> mheap` 是典型的分层 allocator：

| Go component | 通用角色 | C allocator 中的近似概念 |
| --- | --- | --- |
| `mcache` | processor-local fast path | thread cache / per-CPU cache |
| `mcentral` | size-class refill | central bin / slab depot |
| `mheap` | global page ownership | arena/extent/page heap |
| `mspan` | page-run allocation unit | slab/span/extent |

这些名称不能机械映射到 jemalloc、mimalloc 或自研 allocator 的具体实现，但
共同目标相同：把频繁的小对象分配留在局部 fast path，把昂贵的 page
allocation 和全局同步摊薄。

### 2.2 真正的优化是 provenance propagation

```text
+--------------------------+
| page allocation result   |
| bytes + reclaim evidence |
+------------+-------------+
             |
             v
+--------------------------+
| span / arena metadata    |
| preserve zero provenance |
+------------+-------------+
             |
             v
+--------------------------+
| zeroed allocation request|
+------------+-------------+
             |
             +---- dirty or partial ---->+------------------+
             |                           | explicit memset  |
             |                           +------------------+
             |
             +---- proven zero --------->+------------------+
                                         | skip duplicate   |
                                         | initialization   |
                                         +------------------+
```

内存至少有四种不同状态：

| State | 可否假设为零 | 原因 |
| --- | --- | --- |
| Fresh private anonymous mapping | 是 | OS zero-fill-on-demand contract |
| In-process dirty reuse | 否 | 仍可能保留旧对象内容 |
| Fully reclaimed private anonymous pages | 有条件地是 | Linux `MADV_DONTNEED` 成功后的映射语义 |
| Partially reclaimed range | 否 | 未归还部分仍可能为 dirty |

Linux `madvise(2)` 的保证必须写完整：对 **private anonymous mapping** 成功
执行 `MADV_DONTNEED` 后，后续访问得到 zero-fill-on-demand pages。file-backed、
shared mapping 会按 backing store 重新填充，不具备同一个“全零”结论。
`madvise` 也不是 POSIX 通用语义，跨平台代码必须保留 platform gate。
Linux 的 `MADV_FREE` 也不能提供立即归零的同等保证；Go 设置
`GODEBUG=madvdontneed=0` 时会走这条 lazy discard 路径，并继续软件清零。

原文中 `scav == npages * pageSize` 的全量判断非常关键。若只有部分 range
成功 reclaim，却把整个 span 标为 zeroed，就会把性能优化变成数据泄漏或
未初始化读取。

### 2.3 原文 benchmark 如何解读

原文报告 `memclr` 对 8 bytes 到 32 KiB allocation 约增加 `5.5%-26.7%`
耗时。它来自 `AllocOnly` 与 `Alloc+Clear` 的清零成本 microbenchmark，不是
commit 前后 A/B，更不是 end-to-end workload。这个数据只能证明其测试环境中
的重复清零值得优化，不能直接推出：

- C 换成 jemalloc/mimalloc 会得到相同比例；
- Rust 使用 `MaybeUninit` 会得到相同比例；
- 跳过清零一定降低生产 RSS；
- `MADV_DONTNEED` 一定比保留 hot pages 更快。

跳过 `memset` 减少的是 write bandwidth 和 cache pollution；重新访问已
reclaimed pages 又会支付 minor page fault、page allocation 和可能的 NUMA
placement 成本。最终取舍取决于 reuse distance 和 memory pressure。

## 3. C：先设计 allocator contract

### 3.1 不要先做进程级 `malloc` 替换

C 中更稳健的第一步是把 allocator 作为 domain dependency 传递：

```c
enum allocation_init {
    ALLOC_UNINITIALIZED,
    ALLOC_ZEROED,
};

enum zero_provenance {
    ZERO_PROVENANCE_UNKNOWN,
    ZERO_PROVENANCE_FRESH_ANONYMOUS,
    ZERO_PROVENANCE_EXPLICIT,
    ZERO_PROVENANCE_OS_RECLAIMED,
};

struct arena_allocation {
    unsigned char *ptr;
    size_t size;
    enum zero_provenance zero_source;
};
```

这个接口把两个经常混在一起的问题拆开：

- storage 从哪里来、由谁释放；
- caller 是否要求返回 zeroed bytes。

生产 API 未必需要把 `zero_source` 暴露给业务代码，但 allocator 内部必须
保留等价状态，测试和 metrics 也应能观察它。

### 3.2 只在 contract 要求时清零

本 study 的核心决策是：

```c
bool proven_zero =
    start >= old_reuse_limit &&
    (arena->clean_source == ZERO_PROVENANCE_FRESH_ANONYMOUS ||
     arena->clean_source == ZERO_PROVENANCE_OS_RECLAIMED);

if (init == ALLOC_ZEROED) {
    if (proven_zero) {
        arena->stats.elided_zero_bytes += size;
    } else {
        memset(result.ptr, 0, size);
        result.zero_source = ZERO_PROVENANCE_EXPLICIT;
        arena->stats.explicit_zero_bytes += size;
    }
}
```

这里的 `old_reuse_limit` 是上一轮曾暴露给 caller、因此可能已变脏的最高
边界。Arena `reset` 只重置 cursor，不清除这个边界。只有成功 reclaim
整个 mapping 后，才能把边界归零并将 clean source 改为
`ZERO_PROVENANCE_OS_RECLAIMED`。

若 caller 会完整覆盖对象，就请求 `ALLOC_UNINITIALIZED`，不要先清零再覆盖：

```c
struct arena_allocation allocation =
    page_arena_alloc(&arena, sizeof(struct row), _Alignof(struct row),
                     ALLOC_UNINITIALIZED);
if (allocation.ptr == NULL) {
    return false;
}

struct row *row = (struct row *)allocation.ptr;
row->key = key;
row->length = length;
row->kind = kind;
```

这段模式的 invariant 是：对象在被任何 reader 观察前，所有有语义的 field
都必须完成初始化。若结构包含 padding，不应把逐 field 初始化误称为
“整个 object representation 都已初始化”。

### 3.3 `MADV_DONTNEED` 必须是事务式状态转换

```c
if (madvise(arena->base, arena->capacity, MADV_DONTNEED) != 0) {
    return false;
}

arena->cursor = 0;
arena->reuse_limit = 0;
arena->clean_source = ZERO_PROVENANCE_OS_RECLAIMED;
```

顺序不可颠倒：先观察 syscall success，再发布新 provenance。失败时保留
dirty 状态，继续显式清零。调用者还必须证明：

- mapping 是 private anonymous；
- address 和 length 满足 page alignment；
- reclaim 覆盖整个 allocator 要豁免清零的 range；
- 没有 live object 或并发 reader/writer 仍引用该 range；
- 不会在 syscall success 与状态发布之间写回旧数据。

### 3.4 Arena 的收益和代价

Arena 用一次 backing mapping 支撑大量 logical allocations：

```text
+---------------------------+
| mmap one page region      |
+-------------+-------------+
              |
              v
+---------------------------+
| aligned bump allocations  |
| no per-object free        |
+-------------+-------------+
              |
              v
+---------------------------+
| reset whole lifetime      |
+-------------+-------------+
              |
              +---- reuse hot pages ----> dirty provenance
              |
              `---- reclaim all pages --> OS-zero provenance
```

它适合 request、query、parser、batch、compaction step 等同生共死对象。它不
适合任意对象独立释放，也不自动执行 destructor。Arena 可能因为 long-lived
outlier 延长整批内存 lifetime，并提高 peak RSS。

### 3.5 C allocator 选择矩阵

| Workload | 首选 | 主要收益 | 主要风险 |
| --- | --- | --- | --- |
| 任意 size/lifetime | libc/jemalloc/mimalloc | 通用、成熟 | metadata、fragmentation、contention |
| 同生共死小对象 | bump arena | 少量 backing allocations、批量释放 | lifetime 延长、无单对象 free |
| 大量同尺寸对象 | slab/pool | 固定 size class、复用稳定 | stale data、remote free、idle slabs |
| 大块 page-aligned region | `mmap` page arena | page control、reclaim | syscall/page fault/THP/NUMA |
| Thread-local hot path | per-thread cache/arena | 降低 lock contention | 跨线程 free、内存滞留 |

无论选择哪一种，都应遵守：

- allocation size 和 alignment 计算先做 overflow check；
- 明确 zero-size allocation 语义；
- allocation failure 不覆盖旧 owner；
- allocation 和 deallocation 必须使用同一 allocator family；
- `realloc` 使用临时指针，失败时保留原 allocation；
- security erasure 使用 `explicit_bzero` 等不会被优化器删除的 primitive，
  不把普通 `memset` 当成 secret wipe；
- `calloc` 可能利用 allocator/OS 的 zero-page 优化，不应未经测量地替换成
  `malloc + memset`。

## 4. Rust：ownership 之外仍要管理 allocation

### 4.1 Stable Rust 优先级

| 目标 | Stable 做法 | 注意点 |
| --- | --- | --- |
| 避免反复增长 | `Vec::with_capacity` / `reserve` | `reserve` 参数是 additional，不是目标 capacity |
| 可失败预留 | `Vec::try_reserve` / `try_reserve_exact` | 成功申请后仍可能受 Linux overcommit/OOM 影响 |
| 复用 scratch | `clear()` 后保留 capacity | 不要在热循环反复 `shrink_to_fit()` |
| 冻结只读数据 | `Vec::into_boxed_slice()` | `len == capacity` 时保证无需移动；否则可能 shrink |
| 借用而非复制 | `&[T]`、`&str`、必要时 `Cow` | 先确认 owner lifetime |
| 安全地延迟初始化 | `MaybeUninit`、`spare_capacity_mut()` | 只有完整初始化后才能 `set_len`/`assume_init` |
| 全局 allocator | `GlobalAlloc` + `#[global_allocator]` | binary 级决策；禁止 unwind 和递归 allocation |

标准库的 per-container `Allocator`、`Vec<T, A>` 和大量 `*_in` API 截至
Rust 1.98.1 仍是 nightly `allocator_api`。稳定代码若需要 domain arena，
通常使用成熟 crate、自定义集合，或把 allocation 隐藏在明确的 subsystem
边界，不应假装 nightly API 已稳定。

### 4.2 `MaybeUninit` 不是通用性能开关

Rust 的 initialized value 必须立即满足类型 validity。引用不能为 null，
`NonZero*` 不能为 zero，enum discriminant 也必须有效。因此：

- 不要用 `mem::zeroed()` 创建任意 `T`；
- 不要对 `MaybeUninit<T>` 在未完整初始化时调用 `assume_init()`；
- panic/error path 必须 drop 已初始化元素，或明确接受 leak；
- `set_len` 只能在 `0..new_len` 都已成为有效 `T` 后执行；
- 仅在 profiling 证明初始化是热点时才承担这段 `unsafe` proof。

Go 优化中“OS 已经把 bytes 变为零”也不能直接推出 Rust 的任意 `T` 已合法
初始化。零页只提供 byte-level fact；能否形成 value 仍由 `T` 的 validity
决定。

### 4.3 Arena crate 也有 destructor contract

`bumpalo` 适合同生共死的小对象，但默认释放 arena 时不逐对象执行 `Drop`。
把 `Vec`、文件、锁、mmap owner 等资源直接放入 arena，可能泄漏其二级资源。
`typed_arena` 会在 arena 销毁时 drop 元素，但只管理同一种类型。选择 crate
前先写出：

```text
object lifetime
      |
      +---- trivial drop ----> bump allocation is plausible
      |
      `---- owns resources --> require explicit drop strategy
```

### 4.4 FFI allocator ownership

不要让 C `free` Rust `Box`/`Vec` 的 allocation，也不要通常用 C `malloc`
pointer 构造 `Vec::from_raw_parts`。跨 FFI 应传 `ptr + len` view，并由原
allocator 所在一侧提供对应 destroy function。Rust `Vec::from_raw_parts`
要求 size、capacity、alignment 和 allocator identity 全部匹配。

## 5. 六维 allocator 决策模型

在写优化前，先回答六组分布：

| Dimension | 问题 | 常见决策 |
| --- | --- | --- |
| Value | 哪些 variant/value 最常见？ | common-value elision、hot/cold split |
| Size | size class 与长尾如何？ | slab、segregated arena、large mmap |
| Lifetime | 谁一起创建和销毁？ | arena、pool、ownership boundary |
| Access | 顺序、随机、只写一次还是反复读？ | contiguous layout、offset、prefetch |
| Concurrency | 谁分配、谁释放、跨线程比例？ | per-thread cache、remote-free queue |
| Topology | allocation 和访问位于哪个 node/core？ | NUMA placement、sharding、pinning |

Go 文章主要提供了 lifetime、initialization state 和 page topology 的信息。
迁移到数据库内核时，还要补 size distribution、跨线程 free 和 NUMA evidence。

## 6. 验证分层

```text
+---------------------------+
| semantic invariants       |
| zero, alignment, lifetime |
+-------------+-------------+
              |
              v
+---------------------------+
| allocator metrics         |
| calls, bytes, size class  |
+-------------+-------------+
              |
              v
+---------------------------+
| CPU and VM metrics        |
| cycles, cache, page fault |
+-------------+-------------+
              |
              v
+---------------------------+
| production outcomes       |
| p99 latency, RSS/PSS      |
+---------------------------+
```

建议至少记录：

| Layer | Metrics |
| --- | --- |
| API | allocation/reallocation count、requested bytes、failure count |
| Allocator | usable/live/active/resident、bin、fragmentation、purge |
| CPU | cycles、instructions、cache/TLB miss、lock contention |
| VM | minor/major fault、anonymous RSS、THP、NUMA placement |
| Product | throughput、p95/p99 latency、steady-state RSS/PSS |

自制 counting allocator 只能用于观测。Rust optimizer 可以消除或 stack-promote
源码中看似存在的 allocation；allocator call count 不能成为 correctness
条件。jemalloc/mimalloc 也只是改变 cache、bin、arena 和 purge 策略，不会
自动消除业务发出的逻辑 allocations。

## 7. FIL-C Demo

| Item | Value |
| --- | --- |
| Path | `learning/studies/20260907-allocator-provenance/demo/` |
| Language | C11 |
| Backing allocator | private anonymous `mmap` region |
| Logical allocator | aligned bump arena |
| Verification | FIL-C 0.684, Linux/ARM64 |
| Behavior checked | fresh/dirty/reclaimed provenance、zeroing、alignment、failure、batch allocation |

构建并运行：

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I learning/studies/20260907-allocator-provenance/demo/include \
  learning/studies/20260907-allocator-provenance/demo/src/allocator_demo.c \
  learning/studies/20260907-allocator-provenance/demo/tests/allocator_demo_test.c \
  -o .tmp/allocator-provenance-test

.tmp/fil-c/bin/filrun .tmp/allocator-provenance-test
```

FIL-C 验证 memory safety 和 executable behavior；它不证明：

- provenance state machine 在所有并发 interleaving 下正确；
- 该 arena 比生产 allocator 更快；
- `MADV_DONTNEED` 在其他 OS/mapping type 上有相同语义；
- reported zero-elision 等价于 RSS 降低。

## 8. 落地顺序

1. 用 production-like workload 记录 allocation stack、size、lifetime 和
   thread ownership。
2. 删除不必要的 owner 和 allocation，合并连续数据。
3. 对稳定对象冻结 capacity；对 batch lifetime 引入 arena/slab。
4. 把 zero/uninitialized contract 写进 API，不让 caller 猜。
5. 只有下层提供可验证 provenance 时才跳过 initialization。
6. 用 sanitization/FIL-C/Miri 检查 safety，用 profiler 检查成本。
7. 在真实 allocator、线程数、NUMA 和稳定态流量下比较。
8. 小流量 rollout；同时观察 tail latency、fault、allocator resident 和 RSS。

## 9. 反模式

| 反模式 | 为什么错 |
| --- | --- |
| “换 mimalloc 就会省内存” | workload、size class、cache 和 purge 决定结果 |
| “`mmap` 返回过零，所以以后都不用清零” | 一旦写入并在进程内复用，内容就是 dirty |
| “调用过 `madvise` 就是 zeroed” | 必须检查 success、mapping type 和完整 range |
| “`MaybeUninit` 等于 C uninitialized object” | Rust value validity 仍必须满足 |
| “`shrink_to_fit` 会立即降低 RSS” | 只是一项 shrink request，allocator/OS 行为未保证 |
| “arena 没有 leak” | arena 自身可释放，但对象拥有的二级资源可能未 drop |
| “microbenchmark 变快即可上线” | page fault、NUMA、contention 和 steady-state RSS 未覆盖 |

## 10. Checklist

- [ ] 每个 allocation 的 owner 和 deallocator 是否唯一且匹配？
- [ ] 是否先测 allocation count，而不是先替换 allocator？
- [ ] size、lifetime、access、concurrency、topology 是否来自真实分布？
- [ ] zeroed request 与 uninitialized request 是否在 API 上可区分？
- [ ] 跳过清零是否有 fresh/full-reclaim/full-overwrite 证据？
- [ ] partial reclaim 和 syscall failure 是否保持 dirty state？
- [ ] Arena reset 后的所有旧 pointer 是否禁止继续使用？
- [ ] Rust `unsafe` 是否逐条证明 initialization、layout 和 panic cleanup？
- [ ] 是否区分 requested、usable、resident 和 RSS？
- [ ] 是否同时检查 throughput、tail latency、cache miss 和 page fault？
- [ ] C 示例是否由 FIL-C 编译运行，而非系统 Clang？

## 11. References

- [Go change: avoid zeroing scavenged memory](https://github.com/golang/go/commit/27937289dc9fccf1f5513475145799087f39b964)
- [Linux `madvise(2)`](https://man7.org/linux/man-pages/man2/madvise.2.html)
- [Rust `Vec`](https://doc.rust-lang.org/std/vec/struct.Vec.html)
- [Rust `Box`](https://doc.rust-lang.org/std/boxed/struct.Box.html)
- [Rust `MaybeUninit`](https://doc.rust-lang.org/std/mem/union.MaybeUninit.html)
- [Rust `GlobalAlloc`](https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html)
- [Rust nightly `Allocator`](https://doc.rust-lang.org/std/alloc/trait.Allocator.html)
- [bumpalo](https://docs.rs/bumpalo/latest/bumpalo/)
- [typed-arena](https://docs.rs/typed-arena/latest/typed_arena/)
- [jemalloc](https://jemalloc.net/)
- [mimalloc](https://microsoft.github.io/mimalloc/)
- [FIL-C](https://fil-c.org/)
