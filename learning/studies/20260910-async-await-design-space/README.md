---
doc_id: recallfs-study-async-await-design-space-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-async-await-design-space
depends_on:
  - learning/studies/20260910-async-await-design-space/source.md
  - learning/studies/20260910-async-await-design-space/demo/README.md
supersedes: []
verified_by:
  - learning/studies/20260910-async-await-design-space/evidence/README.md
  - learning/studies/20260910-async-await-design-space/demo/tests/mini_async_test.c
  - learning/studies/20260910-async-await-design-space/demo/tests/rust_reference.rs
---

# Async/Await 设计空间与 Rust Future

> 主文：Gray、Krishnamurthi、Crichton，
> [A Design Space Exploration of Async/Await](../../sources/20260910-async-await-design-space-v1.pdf)，
> OOPSLA 2026，arXiv `2608.20677v1`。
>
> 本文区分论文结论、Rust/Tokio API 合同、本地可执行观察和工程推论。

## Contract

### Decision

**可以用 C demo 表达 Rust async 的核心 operational model，但不能用它证明 Rust
的类型安全与完整 runtime 语义。**

C 适合把编译器隐藏的对象显式化：

- `async fn` 返回的惰性状态机；
- `Future::poll` 的 `Ready` / `Pending` 协议；
- `Context` / `Waker` 驱动的重新调度；
- `.await` 在子 future 已就绪时不一定挂起；
- drop future 即放弃 continuation 的 unaware cancellation；
- handle drop 不等于 task cancellation。

C 不能等价表达或证明：

- `Pin<&mut Self>` 的地址稳定性；
- borrow checker 对跨 `.await` 引用生命周期的证明；
- `Send + 'static` 等任务迁移约束；
- `Waker` 的线程安全、happens-before 和 wake coalescing 合同；
- Tokio/Smol 的真实 reactor、并发调度、panic 和 shutdown 行为。

因此，本 study 的 C demo 是一个**可执行语义剖面图**，不是 Rust async runtime
的重实现，也不是论文 Redex 模型的替代品。

### Scope

- 提炼论文的九个 design dimensions；
- 将它们映射到 Rust language、`Future` 和 Tokio/Smol runtime；
- 解释 `async fn`、`.await`、`spawn`、`Waker`、`Pin` 和 cancellation 的关系；
- 用 C11 显式状态机重现五个 Rust 核心观察；
- 用一个无第三方依赖的 Rust 程序作为独立行为 oracle。

### Non-goals

- 不复刻论文覆盖七个 runtime 的完整 Redex semantics；
- 不把单线程教学 executor 用作生产 runtime；
- 不讨论 reactor 的 epoll/kqueue/io_uring 实现和吞吐性能；
- 不声称所有 Rust executor 都采用 Tokio 的 task lifetime 语义；
- 不用 C 的手工约定冒充 Rust 编译器提供的内存安全。

### Inputs And Outputs

输入：

- 论文 arXiv v1；
- 论文作者页面与 artifact v1.3 元数据；
- Rust `Future`、`Waker`、`Pin` 官方文档；
- Tokio `spawn`、`JoinHandle`、`select!` 文档。

输出：

- 本文的设计空间和 Rust 映射；
- [`demo/`](demo/) 中的 C11 executor 与显式 future；
- [`rust_reference.rs`](demo/tests/rust_reference.rs) 行为 oracle；
- [`evidence/`](evidence/) 中的验证结果。

### Interfaces And Ownership

```text
async fn call
    |
    v
compiler-generated Future state
    |
    | poll(Pin<&mut Self>, Context)
    v
Ready(output) or Pending + stored Waker
    |
    | wake
    v
executor ready queue
    |
    v
poll again
```

- Rust compiler owns source-to-state-machine lowering.
- A `Future` owns suspended local state, but does not schedule itself.
- An executor owns task scheduling.
- A reactor or another producer invokes the stored `Waker`.
- A runtime decides task extent, handle strength, shutdown and failure policy.

### Invariants

1. Constructing a Rust async future executes none of its body.
2. A future that returns `Pending` arranges a later wake before progress is
   expected.
3. An executor polls a woken task at least once, but may coalesce wakeups.
4. A completed future is not polled again.
5. A future remains at a stable address while a poll implementation relies on
   self-references.
6. Dropping an incomplete future destroys suspended state without resuming the
   async body with a cancellation exception.
7. Tokio task lifetime is not the same as future lifetime: dropping a
   `JoinHandle` detaches rather than cancels its task.

### Failure Semantics

- Queue overflow makes the demo return failure; it never silently loses a wake.
- A test checks exact trace, poll count and lifecycle flags in optimized builds.
- C callers can violate address and lifetime contracts; Rust safe code cannot.
- Cancellation cleanup in the model is synchronous only.
- The full seven-runtime paper artifact was inspected but not executed locally.

### Worked Examples

Lazy construction:

```text
construct Future -> trace ""
poll Future      -> trace "A", Ready
```

Wake-driven progress:

```text
spawn            -> ready queue contains task, trace ""
poll #1          -> trace "A", store Waker, Pending
signal twice     -> one queued task
poll #2          -> trace "AB", Ready
```

Unaware cancellation:

```text
poll parent      -> poll child -> trace "A", Pending
abort parent     -> no cancellation exception enters future body
executor         -> drop parent -> drop child -> trace "AD"
```

`D` is synchronous destructor cleanup. It does not represent async cleanup.

### Reconciliation Anchors

| Anchor | Exact expected result | Verification |
| --- | --- | --- |
| `AA-LAZY-1` | construct is inert; final trace `A`; one poll | `mini_async_test` |
| `AA-SUSPEND-1` | ready child yields trace `AB` in one parent poll | `mini_async_test` |
| `AA-WAKE-1` | two wakes coalesce; final trace `AB`; two polls | `mini_async_test` |
| `AA-HANDLE-DROP-1` | drop handle 后 task 在有效 storage 内仍到达 `AB` | `mini_async_test` |
| `AA-CANCEL-1` | abort does not repoll body; drop produces `AD`; one poll | C and Rust tests |

### Evidence And Unknowns

- The C anchors pass FIL-C 0.684, Zig 0.16.0, Apple Clang with
  ASan/UBSan, and CMake/CTest.
- Rust 1.97.0 independently produces `A`, `AB`, and `AD` for the three
  language-level anchors.
- The paper reports differential fuzzing against seven runtime
  implementations. This study did not rerun its 2.2 GB container.
- Scheduler fairness, multi-threaded wake races and real I/O are outside this
  demo and remain untested here.

## 1. 论文的核心贡献

论文把 async/await 归入 **straight-line asynchrony**：程序表面保持接近同步代码
的顺序书写方式，但实际执行顺序由 coroutine、task 和 runtime 共同决定。

它最重要的结论不是“哪个语言设计最好”，而是：

> 相同的 `async`、`await`、`spawn` 词汇并不构成相同的语义合同。

论文用三个很小的程序比较 C#、JavaScript、Swift、Python+Asyncio、
Python+Trio、Rust+Tokio 和 Rust+Smol。七个系统没有任何两个给出完全相同的
三元输出。这说明迁移 async 心智模型时，只看表面语法会系统性地产生误判。

论文的证据由三层构成：

1. 对语言规范、RFC、runtime 文档和社区问题的人工分析；
2. 基于 delimited continuation 的统一 operational semantics；
3. 可执行 Redex 模型和 differential fuzzer，将模型输出与真实 runtime 比较。

作者 artifact 报告对每个 runtime 生成 50 个程序，每个程序运行 50 次，并检查
真实输出属于模型允许的输出集合。该证据支持“模型覆盖这些观察到的行为”，但不
证明设计优劣、性能或所有程序上的完备等价。

## 2. 九维设计空间

| 生命周期 | Dimension | 问题 | 论文中的选择 |
| --- | --- | --- | --- |
| Start | Eagerness | 调用 async function 时是否立即执行 | lazy / eager / semi-eager |
| Start | Suspension | 每个 `await` 是否保证让出控制权 | static / dynamic |
| End | Extent | task 最长能活到哪里 | indefinite / dynamic |
| End | Reference Strength | runtime 对 task 的引用是否维持其生命 | strong / weak |
| End | Destruction | extent 结束时如何处理 task | awaited / cancelled / terminated |
| End | Propagation | 未 await task 的失败如何传播 | destructed / never |
| Cancel | Awareness | 被取消的 task 是否有机会观察取消 | unaware / aware |
| Cancel | Direction | 取消如何沿依赖图传播 | top-down / bottom-up / simultaneous |
| Cancel | Persistence | 捕获取消后，取消状态是否持续 | transient / persistent |

这九个维度不是 API 名称列表，而是可观察行为的分解。一个系统必须用组合来描述。
例如，“structured concurrency”至少牵涉 `Extent`、`Destruction` 和
`Propagation`，不能由一个 `TaskGroup` 类型名自动推出。

## 3. Rust 必须分层理解

### 3.1 `Future` 不是 `Task`

Rust `async fn` 调用返回 `impl Future<Output = T>`。标准库给出的核心接口是：

```rust
trait Future {
    type Output;

    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Self::Output>;
}
```

这个值是 lazy 的。只有 executor 主动调用 `poll`，函数体才开始执行。

`Task` 则是 runtime 管理的一次 future 执行实例。以 Tokio 为例，
`tokio::spawn(future)` 将 future 注册到 runtime 并返回 `JoinHandle`。Tokio
保证 `spawn` 不会在调用栈内同步 poll 新 task，但 task 随后可以并发执行。

因此必须分开两句话：

- 调用 `async fn` 是 lazy construction；
- `tokio::spawn` 是把 future 交给 runtime，task 会在后台开始运行。

### 3.2 `.await` 是嵌套 poll，不等于线程切换

一个近似展开如下：

```text
parent.poll()
    |
    +--> child.poll()
            |
            +--> Ready(value)   -> parent continues in same poll
            |
            `--> Pending        -> parent returns Pending
                                  child retains parent's Waker
```

这解释了论文的 `dynamic suspension`。如果 child 已经 `Ready`，`.await` 后的
代码可以在同一次 parent poll 中继续执行。`.await` 不是公平性点，也不是
线程切换保证。需要强制让出时，应使用 runtime 明确提供的 yield primitive。

### 3.3 `Waker` 只表示“值得再 poll”

`Waker::wake` 不携带结果，也不直接恢复某个栈帧。它通知 executor：

> 这个 task 现在可能取得进展，请至少再 poll 一次。

多个 wake 可以合并为一次 poll；一次 poll 也仍可能返回 `Pending`。因此正确
future 必须把 readiness 保存在共享状态中，不能把一次 wake 当成一次不可丢失的
消息。C demo 的 `queued` bit 正是这个 coalescing 行为。

### 3.4 `Pin` 保护的是地址合同

编译器生成的 async state machine 会保存跨 `.await` 的局部变量，也可能形成
指向自身字段的引用。第一次 poll 后，这类 future 可能进入 address-sensitive
状态。

`Pin<&mut Self>` 的作用不是“把对象放到 heap”，而是约束安全代码不能再移动
pointee。C demo 仅依靠调用方把 state 留在固定地址；它展示布局依赖，却没有
类型系统保证。

### 3.5 Drop 是 Rust cancellation 的核心

对一个未完成 future 而言，取消通常意味着以后不再 poll 并最终 drop：

```text
Pending Future
    |
    | cancel / losing select branch / owner drop
    v
drop suspended state
    |
    +--> synchronous Drop cleanup
    `--> async body is not resumed with CancellationError
```

这就是论文所说的 unaware cancellation。future body 没有机会 `catch`
取消；它只能依赖字段的 `Drop`。这带来两个边界：

1. 同步 RAII cleanup 很可靠；
2. 需要 `.await` 的 cleanup 不能直接放进 `Drop`。

Tokio 对 cancellation safety 的实用定义是：丢弃未完成 future 后重新创建它，
应当像 no-op 一样不丢失进度或数据。`read` 常可满足，而 `read_exact`、
`write_all`、公平队列中的 `Mutex::lock` 等可能不满足。`select!` 丢弃失败分支
时，API 是否 cancellation-safe 因而成为 correctness contract。

## 4. Rust 在九维空间中的位置

“Rust 的 async 语义”不是单一行。语言定义 future，runtime 决定 task。

| Dimension | Rust language / core Future | Tokio | Smol | C demo |
| --- | --- | --- | --- | --- |
| Eagerness | lazy | `spawn` 后调度 task | `spawn` 后调度 task | construction inert |
| Suspension | dynamic | dynamic | dynamic | ready child 在同一 poll 完成 |
| Extent | 未定义 task | indefinite | indefinite，handle drop 呈现 dynamic-like 效果 | 只展示 queue lifetime |
| Reference Strength | 未定义 task | strong | weak-like ownership | handle 与 task control 分离；storage 仍由 caller 持有 |
| Destruction | drop future | shutdown 时 drop outstanding tasks | handle drop 默认 cancel | abort 后 drop |
| Propagation | `Output` 可含 `Result` | 未 await 则结果丢失 | 未 await 则结果丢失 | 未建模 error channel |
| Awareness | unaware drop | unaware abort/drop | unaware drop | body 不接收 cancel signal |
| Direction | owned nested futures top-down drop | detached spawned task 不自动形成 lexical child | handle ownership参与传播 | parent drop 显式 drop child |
| Persistence | 对 unaware cancellation 不适用 | 不适用 | 不适用 | 不适用 |

两个限定尤其重要：

1. 论文把 Tokio、Smol 的 task extent 都归为 indefinite，但 Smol 在 handle
   drop 时取消 task，因此常表现为 dynamic-like lifetime；这不是 lexical
   structured concurrency。
2. “Rust top-down cancellation”适用于 ownership/dependency graph。仅仅在
   Tokio task 内 `spawn` 另一个 task，不会让父 task 的取消自动取消 detached
   child；需要 `JoinSet`、cancellation token 或其他显式 scope policy。

## 5. C demo 如何对应 Rust

| C 元素 | Rust 对应物 | 语义 |
| --- | --- | --- |
| `ma_future.state` | compiler-generated future fields | 保存跨 suspension 的 locals 和 program counter |
| `ma_future_vtable.poll` | `Future::poll` | 向前运行直到 `Ready` 或 `Pending` |
| `ma_poll` | `Poll<T>` | 完成或暂不可推进 |
| `ma_context.waker` | `Context::waker` | 请求 executor 以后再次 poll |
| `ma_executor.queue` | executor ready queue | 保存可运行 task |
| `ma_join_handle_drop` | dropped Tokio `JoinHandle` | handle 消失不向 task 发出 cancel |
| `ma_future_drop` | future `Drop` chain | 销毁 continuation 和嵌套 future |

Demo 有五个场景：

| 场景 | 输出 | 证明的最小机制 |
| --- | --- | --- |
| `lazy` | `A`, 1 poll | construction 不执行 body |
| `dynamic-await` | `AB`, 1 poll | ready child 不强制 suspension |
| `wake` | `AB`, 2 polls | `Pending` 后由 Waker 重新入队；重复 wake 合并 |
| `handle-drop` | `AB`, 2 polls | handle drop 不会隐式请求 cancel |
| `cancel` | `AD`, 1 poll | abort 后不再进入 body；drop chain 做同步 cleanup |

其中 `wake` 是理解 Rust async 的核心。future 不是“等待线程”，而是一个被
executor 反复推进的状态值：

```text
                  +----------------------+
                  |                      |
                  v                      |
ready queue -> poll Future -> Pending -> store Waker
                  |
                  `------------> Ready -> drop frame

event source -> wake -> enqueue task
```

## 6. C 能表达什么，不能表达什么

| 主题 | C demo | 结论 |
| --- | --- | --- |
| State machine lowering | 可以，状态字段和 poll 分支完全显式 | 适合教学和调试 |
| Lazy evaluation | 可以，用 constructor 与 first poll 分离 | 与 Rust 行为一致 |
| Dynamic suspension | 可以，parent 直接 poll child | 与 Rust 行为一致 |
| Wake protocol | 可以，function pointer + context | 机制一致，线程安全未覆盖 |
| Task ownership | 部分可以，handle 与 task control 分离 | caller 仍持有 task/state storage，不能证明跨 scope lifetime |
| Unaware cancellation | 可以，停止 poll 后执行 drop callback | 与核心机制一致 |
| Pin | 只能靠约定固定地址 | 不能证明 memory safety |
| Borrow across await | 只能手工管理 pointer lifetime | 不能替代 borrow checker |
| `Send + 'static` | 无静态约束 | 不能证明跨线程迁移安全 |
| Async cleanup | 未提供 | 对应 Rust `Drop` 的同步限制 |
| Reactor and I/O | 未提供 | 不支持 runtime 性能结论 |
| Formal equivalence | 未证明 | 不能替代论文 Redex artifact |

所以答案不是简单的“C 可以”或“不可以”，而是：

> **C 可以精确解释控制流和生命周期机制；Rust 的价值则在于把最危险的地址、
> 所有权和线程约束提升为可检查合同。**

## 7. 对 Rust async 设计的工程判断

### 7.1 Async API 必须声明 cancellation safety

函数签名 `async fn f(...) -> T` 没有表达“在任意 `.await` 处 drop 后是否可安全
重试”。对于协议解析、分段写、事务更新和公平队列，这个属性直接决定能否放入
`select!` race。

建议把 cancellation safety 视为与 panic safety 类似的 API 合同，至少说明：

- 哪些状态在每个 `.await` 前已经提交；
- drop 时哪些资源由 RAII 恢复；
- 重建 future 是否重复、丢失或乱序；
- 是否需要单独 task + handle，而不是直接 race future；
- async cleanup 的 deadline 和失败策略。

### 7.2 Structured concurrency 是 runtime policy

Rust 的 ownership 很适合表达结构化生命周期，但 `async fn` 本身不自动建立
task tree。`tokio::spawn` + 丢弃 `JoinHandle` 明确产生 detached task。

如果系统要求“scope 退出前所有 child 已完成或取消”，必须选择并强制一种 scope
abstraction，不能仅凭语言是 Rust 就假定该性质成立。

### 7.3 `.await` 不是公平性保证

dynamic suspension 允许 ready future 连续执行。CPU-heavy loop 即使语法上有
`.await`，也可能长期不让出。如果公平性是系统 invariant，需要显式预算、
`yield_now` 或 runtime 层 cooperative scheduling。

### 7.4 Runtime 是语义依赖，不只是性能依赖

Tokio 与 Smol 在同一种语言、同一种 `Future` trait 上，对 handle drop 和 task
lifetime 给出不同可观察结果。依赖 runtime-specific `spawn` 的 library，不仅
耦合性能和 I/O driver，也耦合 extent、destruction 和 shutdown semantics。

## 8. 论文未回答的问题

1. 九个维度来源于现有系统分析，不是穷尽性证明。
2. differential fuzzer 检查真实输出属于模型输出集合，不证明模型不会允许过多
   行为。
3. 论文不评估各设计点的 bug rate、开发者理解度或真实 workload 性能。
4. task graph 的 ownership、shared dependency 和 detached task 在大型系统中
   如何组合，仍需要具体 runtime 合同。
5. async drop、effect typing、cancellation-safe trait 等方向尚未由本 taxonomy
   给出最终答案。

## 9. 运行

```bash
cmake -S demo -B ../../../.tmp/async-await-design-space/build -G Ninja
cmake --build ../../../.tmp/async-await-design-space/build
ctest --test-dir ../../../.tmp/async-await-design-space/build \
  --output-on-failure
../../../.tmp/async-await-design-space/build/async_design_demo
```

Rust oracle：

```bash
rustc --edition 2024 -D warnings demo/tests/rust_reference.rs \
  -o ../../../.tmp/async-await-design-space/rust_reference
../../../.tmp/async-await-design-space/rust_reference
```

完整 FIL-C、Zig 和 sanitizer 命令及结果见
[`evidence/README.md`](evidence/README.md)。

## 10. 推荐阅读顺序

1. 先运行 demo，看五个 exact traces。
2. 阅读论文 Table 1 和 Section 3，建立九维词汇。
3. 阅读本文第 3 节，把 `Future` 与 `Task` 分开。
4. 对照 [`async_examples.c`](demo/src/async_examples.c) 阅读显式状态机。
5. 阅读 [`rust_reference.rs`](demo/tests/rust_reference.rs)，确认语言级行为一致。
6. 最后阅读论文 Section 4 的 Redex rules，理解 taxonomy 如何进入 formal model。
