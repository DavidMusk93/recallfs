# Verus Study: Provably Correct Rust

## 1. 结论

**Verus 不是一个普通 Rust 库，而是一套 Rust 程序验证工具链。** 它让开发者在
Rust 源码旁写出前置条件、后置条件、不变量和证明提示，然后静态检查可执行实现是否对
所有满足前置条件的输入都满足规格。

它解决的问题位于 Rust 类型系统之上：

- Rust 主要保证类型、所有权、借用、内存安全和数据竞争安全。
- 测试证明若干被执行样本没有暴露问题。
- Verus 证明实现满足你写下的数学契约，而不是只检查有限样本。

Verus 的核心价值是把高代价系统 bug 的设计约束变成 CI 可重复检查的证明义务。它最适合
数据库内核、存储格式、持久化日志、页表、分配器、parser/serializer、自定义并发原语等
“范围小、规格稳定、错误代价高”的核心模块，不适合一开始就验证整个快速变化的应用。

最重要的限制同样明确：**Verus 只能证明规格写出的东西。错误或过弱的规格会得到没有业务
价值的正确证明。**

## 2. 项目定位

| 问题 | Verus 的答案 |
| --- | --- |
| 它是什么 | 面向 Rust 的 SMT-based automated program verifier |
| 输入 | Rust 子集 + Rust-like 规格与证明 |
| 输出 | 验证成功，或定位到源码的证明义务失败 |
| 证明范围 | 满足 `requires` 的所有输入和所有被建模执行 |
| 运行时成本 | `spec`/`proof` ghost code 编译时擦除，无证明逻辑运行时开销 |
| 自动化核心 | Rust 类型/借用检查 + verification conditions + Z3 |
| 主要差异 | 用 Rust 本身表达规格/证明，并用线性 ghost state 处理资源权限 |
| 当前成熟度 | 活跃开发；只支持 Rust 子集，文档和功能仍在演进 |

Amazon Science 文章的核心观点不是“Rust 不安全”，而是：

> Rust 的静态保证很强，但“内存安全”不等于“算法和系统行为符合设计意图”。

例如，Rust 可以在越界时阻止未定义内存访问，却不会自动证明二分查找永不走到越界路径；
它能防止普通数据竞争，却不会自动证明一个自定义锁协议保持业务不变量；它也不会证明
日志恢复状态机在断电后总能回到合法状态。

## 3. 工作原理

Verus 把程序分成三种 mode：

| Mode | 作用 | 是否进入最终二进制 | 关键约束 |
| --- | --- | --- | --- |
| `exec` | 普通可执行 Rust | 是 | Rust 类型、所有权和借用规则 |
| `spec` | 描述数学模型和目标性质 | 否 | 纯、确定、必须终止 |
| `proof` | 给求解器的证明结构和引理 | 否 | 必须终止，受线性和借用检查 |

编译与验证路径可以概括为：

```text
+---------------------------+
| Rust-like Verus source    |
| exec + spec + proof       |
+-------------+-------------+
              |
              v
+---------------------------+
| rustc front end           |
| parse + types + borrowing |
+-------------+-------------+
              |
              v
+---------------------------+
| Verus IR / proof duties   |
| contracts + invariants    |
+-------------+-------------+
              |
        +-----+-----+
        |           |
        v           v
+---------------+  +-------------------+
| AIR / SMT / Z3|  | erase ghost code  |
| prove or fail |  | rustc / LLVM      |
+---------------+  +---------+---------+
                            |
                            v
                  +-------------------+
                  | executable binary |
                  +-------------------+
```

### 3.1 `requires` 与 `ensures`

下面是 demo 中最终通过验证的 storage extent 计算：

```rust
fn extent_end(start_block: u64, block_count: u64, capacity: u64) -> (end: u64)
    requires
        start_block <= capacity,
        block_count <= capacity - start_block,
    ensures
        end == start_block + block_count,
        start_block <= end <= capacity,
{
    start_block + block_count
}
```

这可以直接理解为 Hoare triple：

```text
{ start <= capacity && count <= capacity - start }
    end = start + count
{ end == start + count && start <= end <= capacity }
```

- 调用者负责证明 `requires`。
- 函数实现负责证明 `ensures`。
- `capacity - start_block` 的写法同时避免了检查条件自身发生加法溢出。
- `ensures` 既描述业务语义，也把结果限制在合法空间内。

Verus 按函数做 modular verification。验证调用者时，只使用被调用函数公开的契约；验证函数
本体时，可以假设前置条件成立。这比每次把整个调用图交给求解器更容易扩展。

### 3.2 为什么 Rust ownership 对证明有价值

传统程序验证很容易被可变内存和别名关系拖垮。Verus 复用 Rust 的所有权与借用检查先处理
大量 aliasing 问题，再用 linear ghost permission 描述“谁有权读取、修改或转移哪个资源”。

这种 ghost permission：

- 在证明期间受 Rust 线性/借用规则约束；
- 可跟踪指针、原子变量、锁保护状态等资源的逻辑所有权；
- 编译时擦除，不增加运行时对象和分支。

这就是 Verus 能从纯算法进一步延伸到 raw pointer、interior mutability 和并发状态机的
关键设计，而不是简单地“把 Rust 代码翻译给 Z3”。

## 4. 可运行 Demo

路径：`learning/studies/20260907-verus/demo/`

macOS arm64 一键准备和执行：

```bash
learning/studies/20260907-verus/demo/setup-macos-arm64.sh
learning/studies/20260907-verus/demo/run.sh
```

安装脚本会：

1. 下载固定版本 Verus 到 `.tmp/`。
2. 用 GitHub release metadata 中的 SHA-256 校验 406 MiB archive。
3. 安装该版本要求的 Rust toolchain。
4. 下载并校验 Verus 当前源码指定的 Z3 4.16.0。
5. 保持二进制、依赖和构建输出都在 gitignored `.tmp/`/`build/`。

本地实测环境：

| Component | Value |
| --- | --- |
| Host | macOS 26.6.2, arm64 |
| Verus | `0.2026.09.06.8dea4a2` |
| Verus commit | `8dea4a2196ebf99449fe2f141a2fb30acae3f17c` |
| Rust | `1.98.0-aarch64-apple-darwin` |
| Z3 | `4.16.0` |

### 4.1 五级证据

| Stage | 输入 | 实际结果 | 说明 |
| --- | --- | --- | --- |
| 1 | 普通 Rust + 3 个典型 test cases | `1 passed` | 有限样本都正确 |
| 2 | 同一 `u64` 加法，无前置边界 | `possible arithmetic underflow/overflow` | Verus 找到未覆盖输入空间 |
| 3 | 错误实现永远返回 0，规格只要求 `end <= capacity` | `2 verified, 0 errors` | 弱规格会证明错误业务实现 |
| 4 | off-by-one 实现 + 精确后置条件 | `postcondition not satisfied` | 强规格拒绝语义 bug |
| 5 | 正确实现 + 完整边界契约 | `2 verified, 0 errors` | 证明、编译、运行全部成功 |

完整输出：`learning/studies/20260907-verus/evidence/run.log`

最终 stage 的本机观测：

```text
total-time: 219 ms
verification results:: 2 verified, 0 errors
verified executable exited successfully
```

这个结果证明 demo 的反馈循环是交互式的，但不是大型 Verus 项目的通用性能基准。

## 5. Verus 的实际价值

### 5.1 比测试多证明了什么

| 能力 | Rust compiler | Unit/property tests | Miri/sanitizers | Verus |
| --- | --- | --- | --- | --- |
| 类型与借用规则 | 强 | 依赖编译器 | 依赖编译器 | 复用 rustc |
| 具体执行中的 bug | 部分 | 强 | 强 | 不是主要目标 |
| 所有输入上的函数契约 | 否 | 否，仍是有限执行 | 否 | 是 |
| 循环/数据结构不变量 | 否 | 间接 | 间接 | 是 |
| 自定义并发协议 | 不证明业务正确性 | 难穷举 interleaving | 可发现部分问题 | 可建模并证明 |
| 运行时开销 | 无 | 测试时有 | 检查时高 | proof/spec 擦除 |
| 主要失败模式 | 类型系统表达力边界 | 覆盖不足 | 未执行路径 | 错误规格或错误模型 |

结论不是“有 Verus 就不要测试”。三者职责不同：

- 编译器守住语言级安全底线。
- 测试验证真实集成、I/O、依赖和运行时行为。
- Verus 对最关键的抽象模型和实现关系给出全称证明。

### 5.2 对数据库/存储内核最有价值的切入点

| 候选模块 | 可证明性质 |
| --- | --- |
| Page/extent arithmetic | 不溢出、不越界、不重叠、空间守恒 |
| WAL/redo/undo state machine | 合法状态迁移、replay 幂等、恢复不变量 |
| Persistent log metadata | crash 后前缀合法、checksum/epoch 关系 |
| Parser/serializer | round-trip、边界安全、拒绝非法编码 |
| Allocator/free list | ownership 唯一、分配块不重叠、回收守恒 |
| Custom lock/atomic primitive | 线性化点、资源 invariant、权限转移 |
| Page-table/index traversal | 查找结果与抽象 map 一致、访问范围合法 |

官方项目清单已经出现 persistent-memory log、page-table management、concurrent memory
allocator、verified parser/serializer、microkernel 和 Kubernetes controller。这说明它不是
只适合 LeetCode 风格纯函数，但也意味着真正的系统证明通常需要专门的抽象模型和 proof
engineering。

### 5.3 AI agent 带来的增益

Amazon 文章特别指出，快速自动化反馈也适合 AI agent 迭代证明。这个判断合理，但责任边界
必须反过来设计：

- 人负责批准顶层规格和可信边界。
- Agent 可以生成 loop invariant、lemma、trigger hint 和证明修复。
- Verus/Z3 检查 agent 产出的证明是否成立。
- 不能让同一个 agent 同时弱化规格再宣称“证明通过”。

因此，AI 降低的是 proof construction 成本，不是 specification review 成本。

## 6. 可信边界与限制

“Provably correct” 必须展开成完整句子：

> 在给定规格、环境模型和工具链假设下，Verus 证明所验证的 Rust 子集实现满足相应契约。

信任链如下：

```text
+------------------------+
| human-approved spec    |
+-----------+------------+
            |
            v
+------------------------+
| Verus + vstd models    |
+-----------+------------+
            |
            v
+------------------------+
| Z3 proof search        |
+-----------+------------+
            |
            v
+------------------------+
| rustc + LLVM + runtime |
+-----------+------------+
            |
            v
+------------------------+
| hardware behavior      |
+------------------------+
```

需要重点审计：

- `assume`、`admit`、`#[verifier::external_body]` 和外部函数规格；
- 顶层业务规格是否完整，是否允许 trivial implementation；
- 标准库、FFI、硬件、崩溃模型是否忠实；
- Verus/Rust/Z3 版本是否固定；
- proof timeout、trigger 和 solver 行为是否在升级后稳定。

官方 README 明确标注项目仍在 active development，部分功能或文档可能缺失。官方 overview
也明确列出当前非目标：支持全部 Rust 特性/库、验证 Verus 自身、验证 Rust/LLVM compiler。

## 7. 采用建议

不要以“全库形式化”为起点。建议按下面顺序推进：

1. 选择一个 100 到 500 行、纯度高、错误代价高的核心。
2. 先写独立 abstract spec，并由另一位工程师审查是否排除了 trivial implementation。
3. 将 I/O、syscall、FFI 和复杂依赖放在窄 external boundary 后面。
4. 用 `--no-cheating` 验证纯 proof core，并在 CI 单独统计所有可信逃生口。
5. 固定 Verus、Rust、Z3 版本和 release SHA-256。
6. 保留普通 unit/integration/fault-injection tests，验证模型外的真实系统行为。
7. 只有当第一个模块的 proof maintenance 成本可接受，再扩展到相邻状态机。

对数据库内核，优先级建议是：

```text
page arithmetic
      |
      v
serialization / on-disk format
      |
      v
recovery state machine
      |
      v
allocator or concurrency primitive
      |
      v
larger subsystem refinement
```

## 8. 报告资产

| Item | Path |
| --- | --- |
| 本报告 | `learning/studies/20260907-verus/README.md` |
| 单文件可视化讲解 | `learning/studies/20260907-verus/report.html` |
| 来源与版本证据 | `learning/studies/20260907-verus/source.md` |
| 调研过程 | `learning/studies/20260907-verus/exploration.md` |
| Amazon 文章归档摘要 | `learning/sources/20260831-verus-amazon-science.md` |
| 可运行 demo | `learning/studies/20260907-verus/demo/` |
| 实际运行日志 | `learning/studies/20260907-verus/evidence/run.log` |

## 9. 最终判断

Verus 的价值不是“证明 Rust 没 bug”，而是**把高价值模块的设计意图提升为机器可检查的接口**。
它能把测试很难穷举的整数边界、循环不变量、数据结构关系和并发资源协议变成每次变更都必须
重新满足的证明义务。

对数据库和存储系统，这是值得试点的技术，但采用单位应是“关键不变量明确的小型内核”，
不是整个产品代码库。规格评审质量决定证明价值，工具链固定与可信边界审计决定工程可信度。
