---
name: "compile-runtime-representation"
description: "Designs mutable build data as compact runtime data. Invoke for build-once/read-many Rust caches, plans, indexes, or memory-layout optimization."
---

# Compile Runtime Representation

## 1. 结论

当一个数据结构具有“生成阶段频繁修改，生成后长期只读或极少修改”的生命周期
时，不要让同一种表示同时服务两个阶段：

- **Builder/IR representation**：服务生成、校验、去重和修改，优先表达能力。
- **Runtime representation**：服务高频读取和执行，优先紧凑、连续和不可变。
- **Compile boundary**：消费 Builder，验证不变量，再产出 Runtime
  representation。

这里的 compile 是“数据编译阶段”，不一定发生在 `rustc` 编译期。配置加载、
SQL prepare、缓存 entry 插入和索引 bulk load 都可能在程序运行时执行这一步。

```text
+---------------+     validate      +---------------+
| Builder / IR  | ----------------> | Runtime data  |
| mutable, rich |   compact/freeze  | compact, hot  |
+---------------+                   +---------------+
                                             |
                                             | repeated reads
                                             v
                                      +---------------+
                                      | Execution     |
                                      +---------------+
```

核心原则：

> 数据结构不是领域对象唯一且永恒的形状，而是特定生命周期阶段的执行表示。

## 2. 何时调用本 Skill

遇到以下信号时使用：

- 对象 build once、read many；
- 构建完成后很少调用 `push`、`insert`、`remove` 或扩容；
- 同类对象数量很大，每个对象浪费几 bytes 都会被基数放大；
- hot path 出现多次 heap allocation、pointer chasing 或 serialization；
- 当前类型保留了运行期不需要的 capacity、owner、hash index 或编辑信息；
- SQL plan、配置、规则、缓存、索引、协议消息可在发布前预计算。

不要机械套用：

| 场景 | 结论 |
| --- | --- |
| 运行期仍频繁增删 | 保留可变表示，或拆分 immutable core 与 mutable delta |
| 随机更新是主要路径 | packed bytes 可能使更新成本过高 |
| 对象数量很少 | 转换复杂度可能不值得 |
| 只构建一次也只读一次 | compile/copy 成本可能无法摊销 |
| 需要稳定 FFI/磁盘 ABI | 使用显式编码或 `repr(C)` 协议，不依赖默认 Rust layout |

## 3. 标准工作流

### 3.1 先记录生命周期

回答这些问题：

1. 对象在什么阶段创建和修改？
2. 发布后还会修改哪些字段，频率是多少？
3. 运行期主要是顺序扫描、随机查找还是局部更新？
4. 同时存活多少个对象？
5. 一次 compile/copy 可以被多少次读取摊销？

不能证明“发布后稳定”，就不能直接冻结整个对象。

### 3.2 分离 Builder 与 Runtime 类型

Builder 可以使用：

- `Vec<T>`、`String`；
- `HashMap`/`HashSet`；
- 冗余名称和反向索引；
- 便于报错的 source span；
- 便于增量编辑的树和 enum。

Runtime representation 可以使用：

- `Box<[T]>`、`Box<str>`；
- 一个连续 payload + checked offsets；
- bitflags 和经过证明的 narrow integer；
- 预计算 lookup table、bytecode 或 physical plan；
- common-value elision；
- hot/cold field split。

不要为了减小 root struct 就把所有字段都 `Box`。独立 allocation 会增加
allocator overhead 和 pointer chasing。目标是缩小完整对象图并改善访问局部性。

### 3.3 让 compile 成为唯一发布入口

转换函数应消费 Builder：

```rust
impl Builder {
    pub fn compile(self) -> Result<RuntimeData, CompileError> {
        // validate -> canonicalize -> compact -> freeze
    }
}
```

消费 `self` 有三个作用：

- 转换后不能再从旧 Builder 修改同一份逻辑数据；
- 可以移动内部 buffer，减少无意义 clone；
- 类型签名直接标出生命周期阶段切换。

Runtime 类型的字段保持 private，只提供读取 API。不要暴露能重新获得可变
container 的 escape hatch。

### 3.4 在边界验证不变量

compile 至少负责：

- checked integer conversion；
- offset 单调且不越界；
- key 唯一性；
- canonical ordering；
- 默认值和 override 的一致性；
- 编码版本与 byte order；
- 所有运行期读取需要的 metadata 已生成。

运行期不应重复执行 Builder 已经能完成的昂贵校验。

### 3.5 拆分真正需要变化的状态

“很少修改”不等于“完全没有运行期状态”。将稳定数据和动态状态分开：

```rust
use std::sync::atomic::AtomicU64;

struct CompiledRuleSet {
    bytecode: Box<[u8]>,
    rule_offsets: Box<[u32]>,
}

struct RuleRuntimeState {
    hit_counts: Box<[AtomicU64]>,
}
```

这样 bytecode 和 offsets 保持只读，计数器可以独立更新；不会为了增加一次命中
计数而复制或解包整个 compiled representation。

## 4. Rust 参考实现

下面把易编辑的字符串列表编译为一个 UTF-8 blob 和一组 end offsets。
Builder 保留 `String` 和增长能力；Runtime 表示只保留连续 bytes 和定长索引。

```rust
#[derive(Debug, Default)]
pub struct NameTableBuilder {
    names: Vec<String>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum CompileError {
    TooManyBytes,
}

#[derive(Debug)]
pub struct NameTable {
    bytes: Box<[u8]>,
    ends: Box<[u32]>,
}

impl NameTableBuilder {
    pub fn push(&mut self, name: impl Into<String>) {
        self.names.push(name.into());
    }

    pub fn compile(self) -> Result<NameTable, CompileError> {
        // Builder 可以自由增长。compile 时先计算一次总长度，以减少构建
        // runtime blob 期间的 realloc。
        let total_bytes = self.names.iter().try_fold(0_usize, |total, name| {
            total
                .checked_add(name.len())
                .ok_or(CompileError::TooManyBytes)
        })?;
        u32::try_from(total_bytes).map_err(|_| CompileError::TooManyBytes)?;

        let mut bytes = Vec::with_capacity(total_bytes);
        let mut ends = Vec::with_capacity(self.names.len());

        for name in self.names {
            // String 已保证 UTF-8。把所有名字放进一个连续 allocation，
            // 避免运行期为每个名字追逐一个独立指针。
            bytes.extend_from_slice(name.as_bytes());

            // Runtime 使用 u32 offset；转换必须检查，不能使用截断式 `as`。
            let end =
                u32::try_from(bytes.len()).map_err(|_| CompileError::TooManyBytes)?;
            ends.push(end);
        }

        Ok(NameTable {
            bytes: bytes.into_boxed_slice(),
            ends: ends.into_boxed_slice(),
        })
    }
}

impl NameTable {
    pub fn len(&self) -> usize {
        self.ends.len()
    }

    pub fn is_empty(&self) -> bool {
        self.ends.is_empty()
    }

    pub fn get(&self, index: usize) -> Option<&str> {
        let end = usize::try_from(*self.ends.get(index)?).ok()?;
        let start = if index == 0 {
            0
        } else {
            usize::try_from(self.ends[index - 1]).ok()?
        };

        // bytes 只可能来自 String，且字段 private，因此 UTF-8 不变量由
        // compile boundary 保证。仍使用安全 API，避免让维护者承担 unsafe
        // 不变量。
        std::str::from_utf8(self.bytes.get(start..end)?).ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compiles_mutable_names_into_read_only_table() {
        let mut builder = NameTableBuilder::default();
        builder.push("alpha");
        builder.push("beta");
        builder.push("");

        let table = builder.compile().unwrap();

        assert_eq!(table.len(), 3);
        assert_eq!(table.get(0), Some("alpha"));
        assert_eq!(table.get(1), Some("beta"));
        assert_eq!(table.get(2), Some(""));
        assert_eq!(table.get(3), None);
    }
}
```

这段转换减少了每个 `String` 的 pointer/length/capacity metadata 和独立 heap
allocation，但引入了 offset table。是否收益取决于：

- name 数量和长度分布；
- allocator size class；
- 查找频率；
- 是否需要按 name 独立修改；
- compile 成本能被多少次读取摊销。

必须测量，不能只根据 `size_of::<NameTable>()` 下结论。

## 5. 常见转换模式

| Builder/IR | Runtime representation | 适用场景 |
| --- | --- | --- |
| `Vec<T>` | `Box<[T]>` | 长度冻结、连续遍历 |
| `String` | `Box<str>` | 文本冻结 |
| `Vec<String>` | byte blob + offsets | 大量短字符串 |
| 多个 section `Vec<T>` | 一个 payload + section offsets | 同类型分区数据 |
| 大 enum | hot inline + cold boxed | variant 尺寸和频率高度偏斜 |
| parsed record tree | versioned packed bytes | 读取以遍历/转发为主 |
| logical plan | physical plan/bytecode | 执行前可完成选择和预计算 |
| mutable config | validated lookup tables | 配置加载后高频查询 |

## 6. 失败模式

### 6.1 只看 root struct

`size_of::<T>()` 不包含 heap object graph。必须同时测 allocation count、
requested/usable bytes、RSS 和 CPU cache miss。

### 6.2 把 boxing 当成压缩终态

boxing 可减小 large enum，却可能增加：

- 每个对象一次 allocation；
- allocator bin rounding；
- pointer chasing；
- cache/TLB miss。

它可能是合理的过渡方案，但不自动优于一个连续 packed representation。

### 6.3 用 `#[repr(packed)]` 强压尺寸

这可能产生 unaligned access，并把安全访问变复杂。磁盘和网络格式应显式编码；
内存布局优化优先采用字段分组、hot/cold split 和连续存储。

### 6.4 冻结后仍暴露修改入口

如果 Runtime 类型仍返回 `&mut Vec<T>`，阶段分离只是命名。让字段 private，
只提供只读 slice/iterator；变化部分放到独立 sidecar。

### 6.5 忽略 compile 成本和更新路径

运行期收益必须覆盖：

```text
compile cost + copy cost + update amplification
```

如果数据频繁变化，应考虑：

- immutable base + mutable delta；
- copy-on-write；
- generation swap；
- 分块重编译；
- 继续使用原可变结构。

## 7. 验证要求

每次采用此模式都要保留 before/after：

| 类别 | 指标 |
| --- | --- |
| 生命周期 | build 次数、read 次数、update 次数 |
| Layout | root size、payload bytes、padding |
| Allocator | allocations/object、requested、usable、resident |
| CPU | throughput、p50/p99、cycles、cache misses |
| 转换 | compile latency、temporary peak memory |
| 生产 | steady-state RSS、命中率、下游 I/O |

测试至少覆盖：

- 空输入；
- 最大合法 offset；
- overflow；
- 重复值和默认值；
- malformed persisted bytes；
- Builder 被消费后无法继续修改；
- Runtime lookup 与 Builder 语义一致。

## 8. 完成标准

- Builder 与 Runtime 类型职责不同；
- compile 是唯一发布边界；
- 所有压缩 offset 使用 checked conversion；
- Runtime API 不暴露无用修改能力；
- 动态状态已拆到 sidecar；
- 没有依赖默认 Rust layout 作为持久格式；
- before/after 同时覆盖内存和性能；
- 生产收益在 steady state 下验证。
