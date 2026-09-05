# Pointer Chasing 是什么意思

## 1. 结论

Pointer chasing 通常译为“指针追逐”。它不是泛指“代码里出现了指针”，而是：

> 下一次内存访问的地址，必须先读取当前对象中的 pointer/index 才能确定。

典型例子是链表：

```text
+--------+     +--------+     +--------+
| Node A | --> | Node B | --> | Node C |
+--------+     +--------+     +--------+
```

CPU 要访问 `Node B`，必须先读到 `Node A.next`；要访问 `Node C`，又必须先读到
`Node B.next`。如果节点散落在 heap 上，每一步都可能等待 cache/TLB/memory。
这些访存彼此依赖，难以并行，也难以被硬件 prefetcher 提前预测。

连续数组不同：

```text
+--------+--------+--------+--------+
| Item 0 | Item 1 | Item 2 | Item 3 |
+--------+--------+--------+--------+
```

`Item i` 的地址可直接计算为：

```text
base + i * item_size
```

CPU 不必读取 `Item i` 才知道 `Item i + 1` 在哪里。它可以提前加载后续 cache
line，编译器也更容易 vectorize/unroll 循环。

## 2. CPU 为什么不喜欢 Pointer Chasing

### 2.1 访存形成串行依赖

链表遍历的逻辑是：

```text
load current node
        |
        v
read next address
        |
        v
load next node
```

若当前节点不在 cache 中，CPU 必须等它返回，才能知道下一次 load 的地址。
即使 CPU 能同时处理多个 outstanding memory request，这条依赖链也限制了
memory-level parallelism。

### 2.2 Heap 节点可能没有空间局部性

每个 `Box<Node>` 通常是独立 allocation。allocator 不保证逻辑相邻的节点
位于相邻地址：

- 一条 cache line 可能只使用少量有效字段；
- 后继节点可能位于另一条 cache line；
- 更大数据集还可能访问不同 page，增加 TLB pressure；
- allocator metadata、size-class rounding 和 fragmentation 也会增加成本。

常见 CPU 的 cache line 是 64 bytes，但这不是 Rust 语言保证，也不应写进
数据格式协议。

### 2.3 Hardware prefetcher 难以推断

连续地址、固定 stride 通常容易预测。随机链表的下一个地址来自刚读出的
payload，prefetcher 很难提前知道。

软件 prefetch 也不是自动解法：为了 prefetch `next.next`，程序通常仍要先
取得 `next`，依赖链并未消失。只有同时遍历多条独立链或提前拥有一批地址时，
才可能隐藏部分 latency。

## 3. 三种 Rust 表示的差别

### 3.1 `Box` 链表：典型 Pointer Chasing

```rust
struct HeapNode {
    value: u64,
    next: Option<Box<HeapNode>>,
}

fn sum_boxed_chain(head: Option<&HeapNode>) -> u64 {
    let mut total = 0;
    let mut current = head;

    while let Some(node) = current {
        total += node.value;

        // 下一次访问哪个地址，只能在读到当前 node.next 后确定。
        // 如果后继节点不在 cache 中，下一轮就可能等待一次长延迟 load。
        current = node.next.as_deref();
    }

    total
}
```

这里每个 `HeapNode` 自身很小，但每个 `Box` 都可能带来独立 allocation。
根对象变小不代表完整对象图更紧凑。

### 3.2 Arena + index：仍有 Chasing，但 Locality 更好

```rust
const END: u32 = u32::MAX;

#[derive(Clone, Copy)]
struct ArenaNode {
    value: u64,
    next: u32,
}

fn sum_index_chain(nodes: &[ArenaNode], head: u32) -> Option<u64> {
    let mut total = 0;
    let mut index = head;

    while index != END {
        // get() 保留边界检查：损坏的 index 不会造成越界内存访问。
        let position = usize::try_from(index).ok()?;
        let node = nodes.get(position)?;
        total += node.value;

        // 这里仍然存在数据依赖：必须先读 node.next，才能确定下一个 index。
        // 但所有节点位于同一个 Vec 中，通常比独立 Box 具有更好的 locality，
        // 也省掉了逐节点 allocation。
        index = node.next;
    }

    Some(total)
}
```

arena 把“随机 heap pointer”变成“连续数组中的 index”。它通常改善：

- allocation 数量；
- cache/TLB locality；
- 序列化；
- handle 大小；
- 批量扫描能力。

但如果 `next` 顺序本身是随机的，地址仍依赖上一个节点，pointer chasing 的
依赖链只是变便宜了，并没有消失。

### 3.3 连续切片：顺序扫描时没有 Chasing

```rust
fn sum_contiguous(values: &[u64]) -> u64 {
    values.iter().copied().sum()
}
```

这里只需从 slice 的 base pointer 开始顺序读取。后续地址与数据内容无关，
硬件可以预取，编译器也有机会把多个元素合并处理。

## 4. 可直接运行的完整示例

下面的程序验证三种表示具有相同语义。它只演示数据布局和遍历依赖，不把一次
运行时间当成 benchmark 结论。

```rust
#[derive(Debug)]
struct HeapNode {
    value: u64,
    next: Option<Box<HeapNode>>,
}

fn build_boxed_chain(values: &[u64]) -> Option<Box<HeapNode>> {
    let mut head = None;

    // 从后向前构建，最终遍历顺序与 values 相同。
    // 每次 Box::new 通常产生一个独立 heap allocation。
    for &value in values.iter().rev() {
        head = Some(Box::new(HeapNode { value, next: head }));
    }

    head
}

fn sum_boxed_chain(head: Option<&HeapNode>) -> u64 {
    let mut total = 0;
    let mut current = head;

    while let Some(node) = current {
        total += node.value;

        // True pointer chasing:
        // 必须读出当前节点中的 heap pointer，才能访问下一个节点。
        current = node.next.as_deref();
    }

    total
}

const END: u32 = u32::MAX;

#[derive(Debug, Clone, Copy)]
struct ArenaNode {
    value: u64,
    next: u32,
}

fn build_index_chain(values: &[u64]) -> Option<(Box<[ArenaNode]>, u32)> {
    if values.is_empty() {
        return None;
    }

    // END 占用 u32::MAX，因此最大合法 index 是 u32::MAX - 1。
    if u32::try_from(values.len()).is_err() {
        return None;
    }

    let mut nodes = Vec::with_capacity(values.len());
    for (index, &value) in values.iter().enumerate() {
        let next = if index + 1 == values.len() {
            END
        } else {
            // 上面的总长度检查保证转换不会截断。
            u32::try_from(index + 1).ok()?
        };
        nodes.push(ArenaNode { value, next });
    }

    Some((nodes.into_boxed_slice(), 0))
}

fn sum_index_chain(nodes: &[ArenaNode], head: u32) -> Option<u64> {
    let mut total = 0;
    let mut index = head;

    while index != END {
        // Index chasing:
        // 下一个 index 仍依赖当前节点，但 payload 位于连续 allocation。
        let position = usize::try_from(index).ok()?;
        let node = nodes.get(position)?;
        total += node.value;
        index = node.next;
    }

    Some(total)
}

fn sum_contiguous(values: &[u64]) -> u64 {
    // 没有逐元素 next 字段。第 i 个元素的地址可由 base + i * size 计算。
    values.iter().copied().sum()
}

fn main() {
    let values = [10, 20, 30, 40, 50];
    let expected = 150;

    let boxed = build_boxed_chain(&values);
    assert_eq!(sum_boxed_chain(boxed.as_deref()), expected);

    let (arena, head) = build_index_chain(&values).unwrap();
    assert_eq!(sum_index_chain(&arena, head), Some(expected));

    assert_eq!(sum_contiguous(&values), expected);
}
```

运行：

```bash
rustc --edition=2024 -O pointer-chasing.rs
./pointer-chasing
```

程序无输出且退出码为 `0`，表示三种遍历结果一致。

## 5. 哪些情况算，哪些不算

| Rust 表示 | 是否属于 Pointer Chasing | 原因 |
| --- | --- | --- |
| `&T` 读取一次 | 通常不这样称呼 | 只有一次普通 indirection |
| `Box<T>` 读取一次 | 通常不这样称呼 | 单次 heap dereference，不是链 |
| `Vec<T>` 顺序遍历 | 否 | 一次 base pointer 后连续访问 |
| `Box<[T]>` 顺序遍历 | 否 | 与 slice 一样连续 |
| `Vec<Box<T>>` | 有额外 indirection | pointer 数组连续，pointee 可能分散；不一定形成严格串行链 |
| `LinkedList<T>` | 是 | `next` 地址依赖当前节点 |
| tree root-to-leaf | 是 | 下一个 child 由当前 node 决定 |
| hash bucket chain | 是 | collision chain 逐节点访问 |
| arena + random next index | 是，但通常更轻 | 仍是依赖链，节点更紧凑 |
| packed bytes 顺序 iterator | 通常否 | 下一个位置由当前 length 计算，数据整体连续 |

`Vec<Box<T>>` 值得单独区分。它会产生 N 次间接访问，但 pointer 本身连续，
CPU 可能同时发出多个相互独立的 pointee load；链表的下一个 pointer 则必须
等当前节点返回。两者都可能 locality 差，但后者的依赖更强。

## 6. 在 Cloudflare DNS 缓存案例中是什么意思

把 large enum variant 放进 `Box` 后，root enum 从 144 bytes 缩小，但每个
boxed payload：

1. 需要独立 allocation；
2. record 中只保存 pointer；
3. lookup 读取 payload 时必须再跳到另一片 heap；
4. 多条 record 的 payload 可能分散在不同 cache line/page。

这会增加 indirection 和 locality 成本。严格来说，一组 `Vec<Box<Record>>`
中的 pointer 可从连续 pointer array 预先读出，不一定像链表一样形成完全串行
的 `next -> next` 依赖；原文使用“poor memory locality”描述得更精确。

最终改为一个 `Box<[u8]>` 后：

- 所有 record data 位于一个 allocation；
- iterator 在连续 buffer 中前进；
- 多数 record 可直接复制到输出；
- 不再为每个 enum payload 单独追逐 heap pointer。

所以性能提升来自 allocation、cache locality 和 serialization 三项共同变化，
不能只归因于“少了一个 pointer”。

## 7. Rust 中常见的改写方式

### 7.1 Sequence/Queue

| 原表示 | 优先考虑 |
| --- | --- |
| `LinkedList<T>` | `Vec<T>` 或 `VecDeque<T>` |
| `Vec<Box<T>>` | `Vec<T>`，前提是元素可移动且尺寸可接受 |
| 多个只读 `Vec<T>` | 一个 `Box<[T]>` + section offsets |

Rust 标准库的 `LinkedList` 文档也明确提示，通常优先使用 `Vec` 或
`VecDeque`，因为它们利用连续内存并具有更少的 allocation。

### 7.2 Tree/Graph

树和图不能总是消除追逐，但可以改善布局：

- 用 arena/slab 统一分配节点；
- 用 compact index 替代 machine-word pointer；
- 按 BFS、DFS 或访问热度排列节点；
- 把 hot fields 放在连续数组，把 cold payload 单独存放；
- 批量处理多个独立 query，增加 memory-level parallelism。

### 7.3 Build Once, Read Many

当结构构建后稳定时：

- Builder 使用 `Vec`、`String`、`HashMap`；
- compile 阶段验证、排序、去重、计算 offset；
- Runtime 使用 `Box<[T]>`、packed bytes 或预计算 table；
- 真正变化的 counter/state 放在独立 sidecar。

这正是
[Compile Runtime Representation](../../skills/rust/compile-runtime-representation/SKILL.md)
Skill 处理的场景。

## 8. 如何确认瓶颈真的是 Pointer Chasing

先建立相同 workload 的 before/after，不要只看结构体大小。

### Linux

```bash
perf stat \
  -e cycles,instructions,cache-references,cache-misses,dTLB-loads,dTLB-load-misses \
  ./target/release/your-benchmark
```

重点看：

- 每次操作的 cycles 是否下降；
- cache miss 和 dTLB miss 是否下降；
- instructions 是否因编码/解码增加；
- throughput 与 p99 是否同时改善。

### macOS

使用 Instruments 的 Counters/Time Profiler，观察：

- CPU cycles；
- cache miss；
- stalled time；
- allocation count；
- hot loop 的调用栈。

### Rust Benchmark

- 使用 `criterion` 或项目既有 benchmark harness；
- 使用 `std::hint::black_box` 防止无关消除；
- 数据规模必须超过 cache，另设 fit-in-cache 对照；
- 固定元素数量、遍历顺序和 payload；
- 构建成本与查询成本分开测；
- 顺序链和随机链分开测；
- 至少报告波动区间，不用一次运行下结论。

若数据完整落在 L1/L2，pointer chasing 可能不明显；当 working set 超过 cache，
差异才会显著。因此小样本“没有变快”不能证明生产规模没有问题。

## 9. 常见误区

1. **“用了 `Box` 就一定慢。”** 错。一次稳定 indirection 可能长期在 cache
   中，成本很低；问题是高频、依赖、不可预测、分散的访问链。
2. **“换成 index 就消除了 Pointer Chasing。”** 错。随机 `next index`
   仍是依赖链，只是 arena 往往改善了 locality。
3. **“结构体更小，所以完整对象更省。”** 错。必须加上 heap allocation、
   allocator rounding 和 padding。
4. **“预取可以彻底解决。”** 错。严格依赖链限制了能提前获得的地址。
5. **“连续一定更快。”** 不总是。若更新需要搬移大量数据、随机插入频繁或
   只访问少量 cold payload，间接表示可能更合适。
6. **“一次 microbenchmark 就够。”** 错。必须验证生产数据分布、working
   set、allocator 和并发访问。

## 10. 一句话判断

看到循环时问：

> 下一次访问地址能否在当前数据返回前算出来？

- 能：通常可预取或并行，不是典型 pointer chasing。
- 不能：存在依赖式 pointer/index chasing，应继续检查 locality 和 cache miss。
