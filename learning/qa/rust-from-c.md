# 从 C 的视角理解 Rust

## 0. 结论先行

C 和 Rust 不应被理解为“旧语言与新语言”的替换关系。对系统开发者更有价值的
关系是：

> C 建立机器、内存、ABI 和成本模型；Rust 把其中一部分依赖人工纪律的不变量，
> 提升为编译器可检查的程序结构。

从 C 学 Rust，最有效的方式不是重新背一套语法，而是反复问：

1. 这段 C 代码依赖了什么隐含约定？
2. 约定由谁维护，什么时候可能失效？
3. Rust 用 ownership、borrowing、enum、trait 或 type state 表达了哪条约定？
4. 编译器证明了什么，仍有哪些语义和性能问题必须由人证明？
5. 学到的表达方式如何反过来改善 C 设计？

推荐形成双向循环：

```text
+-------------+     expose contract     +----------------+
| C mechanism | ----------------------> | Rust invariant |
+-------------+                         +----------------+
       ^                                         |
       |          recover cost model             |
       +-----------------------------------------+
```

Rust 的精髓不是“没有指针”或“绝对安全”，而是以下五件事：

1. **Ownership**：资源释放责任进入值的语义。
2. **Borrowing**：alias 和 mutation 的时间关系进入函数签名。
3. **Algebraic data types**：状态和错误进入类型，减少非法组合。
4. **Traits + generics**：接口约束与实现分离，同时保留静态优化选择。
5. **Safe/unsafe boundary**：无法静态证明的底层操作被集中到显式边界。

C 的精髓也不能丢：

- object representation；
- pointer arithmetic；
- allocator 和 syscall 成本；
- cache、TLB、NUMA；
- ABI、alignment、padding；
- 原子操作和 memory ordering；
- 每一个抽象最终执行了什么 load/store/branch/allocation。

## 1. 一张 C 到 Rust 的语义地图

| C 概念/惯例 | Rust 表达 | 关键差别 |
| --- | --- | --- |
| `malloc` + 唯一 owner | `Box<T>`、`Vec<T>`、`String` | owner 离开作用域自动 `Drop` |
| `free()` | `Drop` | 编译器插入确定性释放，不是 GC |
| pointer + length | `&[T]`、`&mut [T]` | slice 把边界和借用关系一起传递 |
| nullable pointer | `Option<&T>`、`Option<Box<T>>` | nullability 显式进入类型 |
| status + out parameter | `Result<T, E>` | 成功值和错误值是一个 tagged union |
| tag + `union` | `enum` + `match` | variant 与 payload 绑定，匹配需覆盖状态 |
| `const T *` | `&T` | `&T` 还携带有效性、生命周期和 alias 规则 |
| `T *restrict` | `&mut T` | `restrict` 是程序员承诺；`&mut` 受借用规则约束 |
| cleanup label | RAII/`Drop` | 提前返回也会按作用域释放 |
| function pointer + context | trait object/closure | Rust 同时表达调用接口和环境所有权 |
| macro/template-like 泛化 | generics + trait bounds | monomorphization 或显式动态分派 |
| `_Atomic T` | `Atomic*` | ordering 仍需人正确选择 |
| mutex + protected fields | `Mutex<T>`/`RwLock<T>` | lock 与受保护数据绑定 |
| thread-safety 注释 | `Send`/`Sync` | 很多跨线程约束由类型系统检查 |
| opaque handle | newtype/私有字段 | 模块边界可禁止错误构造 |
| init/use/destroy state | type-state 或 enum state machine | 非法状态转换可变成编译错误 |
| flexible build structure | Builder/IR | 服务编辑、校验和错误报告 |
| compact execution structure | frozen runtime representation | 服务高频读取、局部性和低分配 |

这张表是学习入口，不是等价关系。尤其不能写成：

```text
const pointer == shared reference
restrict pointer == mutable reference
malloc == Box
```

它们只在某些职责上相似，Rust 类型通常还包含更强的有效性和生命周期约束。

## 2. Ownership：把 `free` 的责任变成值语义

### 2.1 C 中真正困难的不是 `free()` 本身

C 开发者通常已经知道什么时候调用 `free()`。困难在于长期维护这些问题：

- 当前 pointer 是 owner 还是 borrower？
- shallow copy 后谁负责释放？
- error path 是否漏掉 cleanup？
- callback 或异步任务是否延长了生命周期？
- 释放后其他 alias 是否仍然存在？
- `realloc` 后旧 pointer 是否还被保存？

成熟 C 项目会用命名、注释、init/destroy、引用计数和 `goto cleanup` 建立协议。
Rust ownership 的价值，是把其中一部分协议变为编译器必须验证的规则。

### 2.2 Move 不是 `memcpy` 的同义词

C 结构体赋值通常是 byte-wise member copy。若结构体含 owning pointer，复制后
会出现两个“看起来都像 owner”的值：

```text
owner A ----+
            +----> allocation
owner B ----+
```

Rust 中普通 move 可能仍在机器层复制几个 machine words，但语言语义会让源值
不可再使用：

```compile_fail
fn main() {
    let bytes = vec![1_u8, 2, 3];
    let moved = bytes;
    println!("{}", bytes.len()); // error[E0382]: borrow of moved value
    println!("{}", moved.len());
}
```

核心不是“有没有复制 pointer bits”，而是“释放责任只能沿 move 转移一次”。

### 2.3 `Copy` 应理解为“复制后仍然各自有效”

整数、纯值坐标等可以实现 `Copy`；`Vec<T>`、`String`、`Box<T>` 不能默认
`Copy`，因为复制其 bits 会复制 owner 身份。

从 C 视角判断一个 Rust 类型是否应 `Copy`：

> 如果对该值做一次普通结构体赋值，两个副本都能独立、正确地使用和销毁吗？

若答案是否定的，就不应实现 `Copy`。

## 3. Borrowing：把 Pointer Contract 放进签名

### 3.1 Reference 不是“更方便的裸指针”

合法的 Rust reference 至少意味着：

- non-null；
- properly aligned；
- 指向对其类型有效的值；
- 在声明的 lifetime 内保持有效；
- `&mut T` 在其活跃借用范围内具有独占访问语义；
- `&T` 不允许普通可变 alias，interior mutability 需显式类型表达。

C 的 `T *` 本身不携带这些保证。它可能为 null、dangling、misaligned，
也无法从类型判断 owner、borrower 或可访问长度。

### 3.2 `const T *` 不等于 `&T`

`const T *p` 只表示不能通过 `p` 修改 `*p`。另一个 alias 仍可能修改同一对象：

```text
const view ----+
               +----> object
mutable ptr ---+
```

`&T` 的 shared borrow 语义更强：正常 safe Rust 不能在该 shared borrow 活跃时
通过普通 alias 修改同一值。`UnsafeCell`、`Cell`、`RefCell`、`Mutex` 和原子
类型显式表示允许的 interior mutability。

### 3.3 `restrict` 不等于 `&mut`

`restrict` 向 C 编译器承诺特定访问期间不存在冲突 alias；违反承诺会产生
undefined behavior，但编译器通常不会替程序员完整验证。

`&mut T` 同样为优化提供强 alias 信息，但它还是语言级借用关系：

- 同一活跃范围不能再创建冲突 borrow；
- lifetime 限制引用不能逃逸到 owner 之后；
- safe API 的调用者无需用注释猜测约定。

下面的冲突借用不会通过编译：

```compile_fail
fn main() {
    let mut values = vec![1, 2, 3];
    let first = &mut values[0];
    let also_first = &values[0];
    *first += 1;
    println!("{also_first}");
}
```

### 3.4 Slice 是值得带回 C 的设计习惯

Rust 的 `&[T]` 可以从 C 视角理解为：

```text
borrowed pointer + element count + lifetime contract
```

即使在 C 中，也应尽量让 pointer 与 length：

- 同时作为参数传递；
- 同时验证；
- 同时存储在一个 view struct 中；
- 使用 element count，而不是在 byte count 和 element count 之间含混。

## 4. ADT：把 Tag 与 Payload 绑定

### 4.1 `Result<T, E>` 是受检查的 tagged union

C 常见错误接口：

```text
int parse(input, output, error_buffer)
```

调用者必须记住：

- `0` 是否成功；
- 失败时 `output` 是否初始化；
- 错误码和 `errno` 哪个有效；
- 哪些错误可以重试。

Rust：

```text
Result<SuccessValue, ParseError>
```

让成功值只存在于 `Ok`，错误信息只存在于 `Err`。调用者必须 `match`、传播
`?`，或显式选择丢弃错误。

### 4.2 Rust enum 不只是 C enum

C `enum` 通常只是整数集合；要携带不同 payload，需要手写 tag + union，并
维护 tag 与 active member 的一致性。

Rust enum 把二者绑定：

```rust
#[allow(dead_code)]
enum IoState {
    Idle,
    Reading { offset: u64, len: usize },
    Failed(std::io::Error),
}
```

`match` 默认要求覆盖全部 variant。增加新状态时，未处理位置会出现编译错误。

### 4.3 Type State 适合表达单向生命周期

对于 `Building -> Ready -> Retired` 这样的状态，使用不同类型可以让非法操作
根本不存在：

```text
Building --finish(self)--> Ready --retire(self)--> Retired
```

这也对应此前的数据编译经验：Builder/IR 与 Runtime representation 不应被迫
使用同一个类型。

## 5. Traits 与 Generics：理解抽象的成本

### 5.1 Trait 先看作接口约束

trait 表达“一个类型能提供哪些行为”：

- generic `T: Trait` 通常使用静态分派；
- `dyn Trait` 使用动态分派，类似 data pointer + vtable；
- associated type 表达实现选择的相关类型；
- blanket impl 能组合能力，但也会影响 coherence 和错误信息。

从 C 视角可作如下类比：

| Rust | C 中的近似机制 |
| --- | --- |
| `T: Trait` | macro/code generation + static function choice |
| `&dyn Trait` | interface pointer + vtable |
| closure | function pointer + captured context |
| newtype impl | opaque handle + namespaced functions |

类比只用于理解调用模型。Rust trait 还参与类型检查、生命周期和自动 trait 推导。

### 5.2 “Zero-cost” 不是“没有成本”

它更接近：

> 不使用某项能力时不支付成本；使用静态抽象时，有机会生成与手写专用代码相当
> 的机器代码。

仍需关注：

- monomorphization 造成 binary size 增长；
- `dyn Trait` 的间接调用；
- iterator 是否真的被优化；
- bounds check 是否被消除；
- panic/unwind 配置；
- allocation 和 cache locality。

最终仍应看 benchmark、profile、layout 和必要时的 assembly。

## 6. `unsafe`：把证明义务集中起来

Rust 不是没有裸指针。它提供：

- `*const T`、`*mut T`；
- `unsafe fn`；
- `unsafe trait`；
- `union`；
- FFI；
- allocator 和底层 intrinsic。

`unsafe` 的意义不是“这里不安全，后果自负”，而是：

> 编译器无法证明的若干规则，由这一小段代码的作者证明；safe caller 仍应得到
> 完整安全保证。

一个高质量 unsafe boundary 应做到：

1. `unsafe` block 尽量小；
2. 注释列出必须成立的 safety invariant；
3. raw pointer 尽快转换为受约束的 safe abstraction；
4. 对 length、alignment、initialization、alias、lifetime 分别检查；
5. 用 Miri、sanitizer、fuzzing 和边界测试补充静态检查；
6. 不把未经验证的 raw handle 暴露给普通业务代码。

C 代码也可采用同样的思想：把危险 pointer arithmetic、ownership transfer 和
syscall edge case 集中到少量模块，而不是让整个代码库共享隐含约定。

## 7. 并发：Rust 检查“能不能共享”，不证明算法一定正确

Rust 的 `Send`/`Sync` 能阻止许多明显错误：

- `Rc<T>` 不能直接跨线程 move；
- 非线程安全 interior mutability 不能无保护共享；
- `Mutex<T>` 把 lock 与数据绑定；
- scoped borrow 防止线程持有过期 stack reference。

例如下面代码不会通过编译：

```compile_fail
use std::rc::Rc;
use std::thread;

fn main() {
    let value = Rc::new(42);
    thread::spawn(move || println!("{value}"));
}
```

但 Rust 不会自动证明：

- lock ordering 无死锁；
- atomic ordering 正确；
- condition variable 无 lost wakeup；
- bounded queue 有正确 backpressure；
- 并发状态机满足业务一致性；
- cache line 没有 false sharing。

C 的 memory model、atomics 和 cache coherence 知识仍然直接适用于 Rust。

## 8. 一组可运行的 C/Rust 对照

下面两段程序表达同一组行为：

- 构造 4-byte buffer；
- 借用 buffer 计算 checksum；
- 转移 owner；
- 解析十进制整数并显式返回错误；
- 输出 `checksum=10 parsed=42 len=4`。

### 8.1 C：协议由 API 和纪律维护

```c
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * OwnedBuffer 的约定：
 * 1. data != NULL 时，这个 struct 是 allocation 的唯一 owner。
 * 2. 调用 owned_buffer_destroy() 后恢复为空状态。
 * 3. 禁止普通结构体复制；owner 转移必须调用 owned_buffer_move()。
 *
 * C 类型系统不会自动验证第 3 条，因此 API、命名、review 和测试共同维护它。
 */
struct OwnedBuffer {
    uint8_t *data;
    size_t len;
};

static bool owned_buffer_init(struct OwnedBuffer *out, size_t len)
{
    /*
     * 先写入可安全 destroy 的空状态。这样调用者在后续失败路径中仍可统一
     * cleanup，不需要猜测 out 是否部分初始化。
     */
    *out = (struct OwnedBuffer){0};

    if (len == 0)
        return true;

    uint8_t *data = malloc(len);
    if (data == NULL)
        return false;

    out->data = data;
    out->len = len;
    return true;
}

static void owned_buffer_destroy(struct OwnedBuffer *buffer)
{
    /*
     * free(NULL) 合法。释放后清空字段，使重复 cleanup 在本例中无害，也让
     * 调试时更容易发现“对象已经不再拥有资源”。
     */
    free(buffer->data);
    *buffer = (struct OwnedBuffer){0};
}

static struct OwnedBuffer owned_buffer_move(struct OwnedBuffer *source)
{
    /*
     * 机器层仍复制 pointer 和 length，但随后清空 source，人工模拟 Rust move。
     * 如果调用者绕过本函数直接 `a = b`，类型系统无法阻止 double free。
     */
    struct OwnedBuffer destination = *source;
    *source = (struct OwnedBuffer){0};
    return destination;
}

static uint64_t checksum_borrowed(const uint8_t *data, size_t len)
{
    /*
     * data 是 borrower，不负责 free。pointer 与 length 必须来自同一对象；
     * C 签名无法表达 data 在整个循环期间有效，也无法表达是否存在写 alias。
     */
    uint64_t total = 0;
    for (size_t i = 0; i < len; ++i)
        total += data[i];
    return total;
}

enum ParseTag {
    PARSE_OK,
    PARSE_EMPTY,
    PARSE_INVALID,
    PARSE_OVERFLOW,
};

struct ParseResult {
    enum ParseTag tag;
    uint64_t value;
};

static struct ParseResult parse_u64(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return (struct ParseResult){.tag = PARSE_EMPTY};

    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return (struct ParseResult){.tag = PARSE_INVALID};

        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return (struct ParseResult){.tag = PARSE_OVERFLOW};
        value = value * 10 + digit;
    }

    return (struct ParseResult){
        .tag = PARSE_OK,
        .value = value,
    };
}

int main(void)
{
    struct OwnedBuffer building;
    if (!owned_buffer_init(&building, 4))
        return 1;

    building.data[0] = 1;
    building.data[1] = 2;
    building.data[2] = 3;
    building.data[3] = 4;

    uint64_t checksum =
        checksum_borrowed(building.data, building.len);

    /*
     * ready 成为唯一 owner；building 被清空。这个状态转移是项目协议，
     * 不是 C 编译器强制规则。
     */
    struct OwnedBuffer ready = owned_buffer_move(&building);
    assert(building.data == NULL && building.len == 0);

    struct ParseResult parsed = parse_u64("42");
    if (parsed.tag != PARSE_OK) {
        owned_buffer_destroy(&ready);
        return 2;
    }

    size_t len = ready.len;
    printf(
        "checksum=%llu parsed=%llu len=%zu\n",
        (unsigned long long)checksum,
        (unsigned long long)parsed.value,
        len
    );

    owned_buffer_destroy(&ready);
    return 0;
}
```

### 8.2 Rust：把协议编码进类型

```rust
#[derive(Debug)]
struct BuildingBuffer {
    data: Vec<u8>,
}

#[derive(Debug)]
struct ReadyBuffer {
    data: Box<[u8]>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ParseError {
    Empty,
    InvalidDigit,
    Overflow,
}

impl BuildingBuffer {
    fn with_len(len: usize) -> Self {
        Self { data: vec![0; len] }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        // &mut [u8] 表示调用者暂时独占借用 payload，但不取得 owner 身份。
        &mut self.data
    }

    fn finish(self) -> ReadyBuffer {
        // self 被消费：状态只能从 Building 转移到 Ready。
        // ReadyBuffer 不暴露 mutable API，也不携带可增长的 Vec capacity。
        ReadyBuffer {
            data: self.data.into_boxed_slice(),
        }
    }
}

impl ReadyBuffer {
    fn as_slice(&self) -> &[u8] {
        // shared borrow 只在调用期间有效，不转移 buffer 的释放责任。
        &self.data
    }

    fn len(&self) -> usize {
        self.data.len()
    }
}

fn checksum_borrowed(data: &[u8]) -> u64 {
    // Slice 同时携带 pointer 和 length；迭代不会访问边界之外。
    data.iter().map(|&byte| u64::from(byte)).sum()
}

fn parse_u64(text: &str) -> Result<u64, ParseError> {
    if text.is_empty() {
        return Err(ParseError::Empty);
    }

    let mut value = 0_u64;
    for byte in text.bytes() {
        if !byte.is_ascii_digit() {
            return Err(ParseError::InvalidDigit);
        }

        let digit = u64::from(byte - b'0');
        value = value
            .checked_mul(10)
            .and_then(|current| current.checked_add(digit))
            .ok_or(ParseError::Overflow)?;
    }

    Ok(value)
}

fn main() -> Result<(), ParseError> {
    let mut building = BuildingBuffer::with_len(4);
    building.as_mut_slice().copy_from_slice(&[1, 2, 3, 4]);

    // finish(self) move 走 building；后续代码无法再访问 building。
    let ready = building.finish();
    let checksum = checksum_borrowed(ready.as_slice());

    // `?` 在 Err 时提前返回；ready 仍会自动 Drop。
    let parsed = parse_u64("42")?;

    println!("checksum={checksum} parsed={parsed} len={}", ready.len());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_reports_each_error_class() {
        assert_eq!(parse_u64(""), Err(ParseError::Empty));
        assert_eq!(parse_u64("4x"), Err(ParseError::InvalidDigit));
        assert_eq!(parse_u64("18446744073709551616"), Err(ParseError::Overflow));
    }

    #[test]
    fn building_state_compiles_into_ready_state() {
        let mut building = BuildingBuffer::with_len(3);
        building.as_mut_slice().copy_from_slice(&[4, 5, 6]);

        let ready = building.finish();

        assert_eq!(checksum_borrowed(ready.as_slice()), 15);
        assert_eq!(ready.len(), 3);
    }
}
```

### 8.3 同一行为中，证明责任去了哪里

| 责任 | C 版本 | Rust 版本 |
| --- | --- | --- |
| allocation 失败 | 显式检查 `malloc` | 默认全局 allocator 失败策略；可失败分配需专门 API |
| 唯一 owner | 注释 + `owned_buffer_move` | move semantics |
| borrower 不释放 | 命名和约定 | `&[u8]` |
| pointer/length 配对 | 调用者保证 | slice 类型 |
| 构建到只读状态 | 清空 source 的项目协议 | `finish(self) -> ReadyBuffer` |
| cleanup | 每条路径调用 destroy | `Drop` |
| parse 状态与值一致 | 手写 tag + struct | `Result<u64, ParseError>` |
| overflow | 手写算术检查 | `checked_mul`/`checked_add` |
| 错误传播 | 分支 + cleanup | `?` + automatic Drop |

Rust 没有替你决定 allocation failure policy、数据格式、算法或性能目标。它减少的
是“协议写对了但某条路径忘记遵守”的空间。

## 9. 最容易从 C 误学的 Rust 概念

### 9.1 `String` 不是 `char *`

- Rust `String` 拥有 UTF-8 bytes，包含 pointer/length/capacity；
- `&str` 是 borrowed UTF-8 slice，不要求 NUL 结尾；
- Rust `char` 是 Unicode scalar value，不是单 byte；
- 二进制和协议字段优先使用 `[u8]`/`Vec<u8>`；
- C FFI 使用 `CString`/`CStr` 明确处理 NUL。

### 9.2 Reference 不能临时指向任意 bytes

C 常通过 cast 解释 raw bytes。Rust reference 必须立即满足 alignment、
initialization、valid value 和 alias 规则。若原始 bytes 尚未满足这些条件，
先保持为 `[u8]`，使用显式 decode；不要为了“方便访问字段”过早制造 `&T`。

### 9.3 Rust 默认 layout 不是持久化格式

普通 `repr(Rust)` struct/enum 的 padding 和布局不能作为稳定 ABI。规则：

- FFI struct 使用经过审查的 `#[repr(C)]`；
- 协议/磁盘格式逐字段编码；
- enum 跨 FFI 使用显式 tag + payload contract；
- 不跨 FFI 传 trait object、Rust `String`、默认 enum；
- allocator 必须由创建对象的一侧负责对应释放；
- panic 不应越过不允许 unwind 的 C ABI 边界。

### 9.4 Integer Overflow 必须表达意图

不要把 debug/release 的默认差异当业务语义。明确选择：

- `checked_*`：失败是数据/输入错误；
- `wrapping_*`：算法定义为模运算；
- `saturating_*`：达到边界后饱和；
- `overflowing_*`：同时需要值与 overflow flag。

这也值得带回 C：无符号 wrap 虽有定义，也不代表业务允许；有符号 overflow
更不能依赖。

### 9.5 `clone()` 不是解决 Borrow Checker 的默认答案

频繁 clone 往往是在掩盖：

- owner 边界不清；
- 数据结构生命周期混在一起；
- API 返回了过长 borrow；
- 应传 index/handle，却传了整个对象；
- 应拆分 immutable core 和 mutable state。

先重新画 ownership graph，再决定 clone 是否是有意的成本。

## 10. Rust 如何反过来加强 C

### 10.1 给每个资源写 Owner Ledger

每类资源明确记录：

| 项目 | 问题 |
| --- | --- |
| Create | 谁创建，失败时 out parameter 是什么状态？ |
| Own | 哪个对象负责 destroy？ |
| Borrow | 哪些 API 只借用，借用能持续多久？ |
| Move | ownership 如何转移，source 如何失效？ |
| Share | 是否引用计数，计数是否原子？ |
| Destroy | 是否允许重复调用，顺序约束是什么？ |

这相当于在 C API 设计阶段手动执行一次 borrow-check thinking。

### 10.2 统一可清理的空状态

C init 函数先把 out parameter 设置为可安全 destroy 的空状态，失败路径使用
单一 cleanup：

```text
zero state -> partial init -> ready -> destroyed zero state
```

避免每个 error branch 猜测初始化进度。

### 10.3 用 Tagged Union 表达状态

不要让多个 bool 形成隐含状态空间：

```text
is_ready + is_failed + has_value
```

改成单一 tag，并只允许与该 tag 对应的 payload。Rust enum 会自然推动这种
设计，C 中也可用 `enum + union + constructor/accessor` 实现。

### 10.4 把 Borrow View 变成 Struct

对于反复出现的 pointer/length：

```c
struct byte_view {
    const uint8_t *data;
    size_t len;
};
```

这仍不具备 Rust lifetime 检查，但能减少参数错配，并为统一校验建立边界。

### 10.5 把 Unsafe 集中到模块边界

即使使用 FIL-C 提供运行期 memory-safety enforcement，ownership、状态机、
业务边界和并发语义仍需设计。推荐分层：

```text
+-------------------+
| Checked public API|
+---------+---------+
          |
          v
+-------------------+
| Narrow raw layer  |
+---------+---------+
          |
          v
+-------------------+
| OS / device / ABI |
+-------------------+
```

FIL-C 与 Rust 不是重复关系：

- FIL-C 保留 C 的表达和 ABI 习惯，在运行期强化 memory safety；
- Rust 在编译期用 ownership/borrowing 限制 safe code；
- 两者都不能代替算法不变量、协议正确性和性能验证。

## 11. 为什么 Rust 相对 Agent-friendly

这里的“Agent-friendly”不是指 agent 写 Rust 一定正确，而是其工程反馈回路更
适合自动验证。

### 11.1 编译器是高密度反馈接口

borrow checker 和类型检查器会给出：

- 错误类别；
- 精确 source span；
- 冲突 borrow/move 的位置；
- 缺失 trait bound；
- 非穷尽 match；
- 很多情况下的修改建议。

Agent 可以根据结构化错误迭代，而不是等待随机崩溃才发现 owner 协议被破坏。

### 11.2 工具链高度统一

典型仓库有稳定入口：

```bash
cargo fmt --check
cargo check --all-targets
cargo clippy --all-targets -- -D warnings
cargo test
cargo doc --no-deps
```

`Cargo.toml`、crate/module、测试和文档约定减少了“先猜项目如何构建”的空间。

### 11.3 类型携带更多任务上下文

`Option`、`Result`、newtype、enum state 和 trait bound 让很多约束可从代码
直接恢复。对 agent 和人类都更容易：

- 找到所有错误分支；
- 判断 nullability；
- 看出 ownership transfer；
- 枚举状态；
- 生成边界测试。

### 11.4 Safe Rust 缩小验证面

当绝大多数代码是 safe Rust 时，memory-safety review 可以集中检查：

- `unsafe` block；
- FFI；
- raw pointer；
- custom allocator；
- interior mutability；
- concurrency ordering。

这降低了审查范围，但不降低这些边界的质量要求。

### 11.5 仍然必须防范的 Agent 失败

- 为绕过 borrow error 无意义 `clone()`；
- 滥用 `Arc<Mutex<_>>` 把设计问题变成 runtime contention；
- 写出编译正确但复杂度错误的 iterator chain；
- 在 `unsafe` 注释中“宣称”不变量而未证明；
- 依赖 crate 而未审计维护性和供应链；
- 只跑 `cargo check`，未做测试、Miri、fuzz 或 benchmark；
- 改变 layout/serialization 却没有兼容性测试。

所以 Rust 适合 agent 的原因是“更容易闭环验证”，不是“更容易一次生成正确”。

## 12. 推荐学习路线：每一步同时练 C 与 Rust

### 12.1 Value、Pointer、Owner

**C 侧加强：**

- object lifetime；
- stack/heap/static storage duration；
- init/destroy/move convention；
- pointer + length；
- `const`、`restrict` 的真实语义。

**Rust 侧学习：**

- move、`Copy`、`Clone`；
- `Box`、`Vec`、`String`；
- `&T`、`&mut T`、slice；
- lexical scope 与 `Drop`。

**练习：** 实现 owned byte buffer、borrowed view、显式 ownership transfer。

### 12.2 Error 与 State

**C 侧加强：**

- status/out parameter；
- tagged union；
- partial initialization；
- `goto cleanup`；
- overflow checking。

**Rust 侧学习：**

- `Option`、`Result`；
- `match`；
- `?`；
- enum payload；
- type-state。

**练习：** 写一个 length-prefixed record parser，同时实现 C tagged result 和
Rust `Result<Record, Error>`，对同一 malformed corpus 测试。

### 12.3 Layout 与 Allocation

**C 侧加强：**

- `sizeof`、`_Alignof`；
- padding；
- flexible array member；
- allocator size class；
- cache line、TLB、pointer chasing。

**Rust 侧学习：**

- `size_of`、`align_of`；
- `Vec<T>` 与 `Box<[T]>`；
- enum max-variant cost；
- arena/index；
- hot/cold split；
- packed immutable representation。

**练习：** 把 mutable record builder 编译成连续 runtime buffer，比较 allocation
count、RSS 和 lookup latency。

### 12.4 Interface 与 Polymorphism

**C 侧加强：**

- opaque handle；
- function pointer；
- vtable；
- header/API ownership annotation；
- ABI versioning。

**Rust 侧学习：**

- module/privacy；
- newtype；
- trait；
- generic；
- `dyn Trait`；
- associated type。

**练习：** 为同一 storage backend 分别写 C vtable 和 Rust trait，比较 static
dispatch 与 dynamic dispatch。

### 12.5 Concurrency

**C 侧加强：**

- pthread mutex/condition variable；
- `_Atomic`；
- acquire/release；
- cache coherence 和 false sharing。

**Rust 侧学习：**

- `Send`/`Sync`；
- `Arc`；
- `Mutex`/`RwLock`；
- channels；
- atomic ordering；
- scoped threads。

**练习：** 实现 bounded queue，明确 ownership transfer、shutdown state 和
backpressure；先验证正确性，再测 contention。

### 12.6 Unsafe、FFI 与内核边界

**C 侧加强：**

- ABI；
- alignment；
- syscall contract；
- mmap/device memory；
- signal/callback lifetime。

**Rust 侧学习：**

- raw pointers；
- `NonNull<T>`；
- `MaybeUninit<T>`；
- `Pin`；
- `UnsafeCell`；
- `extern "C"`；
- safety invariant。

**练习：** 用 C ABI 暴露一个最小 Rust library，owner 一侧创建并释放；为每个
unsafe block 写可逐项审查的 safety proof。

## 13. 每个主题的学习模板

以后学习一个 Rust 概念时，按下面顺序记录：

| 步骤 | 要回答的问题 |
| --- | --- |
| 1. C mechanism | C 中用什么 pointer/layout/API 完成？ |
| 2. C contract | 哪些规则只存在于注释和 review？ |
| 3. Failure | 违反后是 compile error、runtime error 还是 UB？ |
| 4. Rust type | Rust 用什么类型表达？ |
| 5. Compiler proof | 编译器具体排除了哪些状态？ |
| 6. Residual risk | 哪些语义、并发、性能问题仍由人负责？ |
| 7. Cost | allocation、branch、layout、dispatch 有何变化？ |
| 8. Backport | 这个思路如何改善 C API？ |
| 9. Evidence | FIL-C、rustc、test、Miri、perf 的结果是什么？ |

标准验证流程：

```text
+-------------+
| C baseline  |
+------+------+
       |
       | expose manual contract
       v
+-------------+
| Rust model  |
+------+------+
       |
       | compile and test both
       v
+-------------+
| Measure cost|
+------+------+
       |
       | retain transferable rule
       v
+-------------+
| Apply to C  |
+-------------+
```

## 14. 本文示例验证方式

C 示例必须使用仓库 FIL-C 工具链：

```bash
rustc --edition 2021 -D warnings tools/docs/verify_markdown_c.rs \
  -O -o .tmp/verify-markdown-c

.tmp/verify-markdown-c \
  --compiler .tmp/fil-c/bin/filcc \
  --output-dir .tmp/verify-rust-from-c \
  learning/qa/rust-from-c.md

.tmp/fil-c/bin/filrun \
  .tmp/verify-rust-from-c/rust-from-c.filc-test
```

Rust 示例：

```bash
rustc --edition=2024 --test .tmp/rust-from-c.rs \
  -o .tmp/rust-from-c-test
.tmp/rust-from-c-test

rustc --edition=2024 .tmp/rust-from-c.rs \
  -o .tmp/rust-from-c
.tmp/rust-from-c
```

预期两个程序均输出：

```text
checksum=10 parsed=42 len=4
```

## 15. 最终检查表

- [ ] 我能指出每个 allocation 的唯一 owner 吗？
- [ ] 我能区分 owner、shared borrower 和 mutable borrower 吗？
- [ ] 我是否把 pointer 与 length 作为一个逻辑对象处理？
- [ ] 我能解释 `const T *` 与 `&T` 为什么不等价吗？
- [ ] 我能解释 `restrict` 与 `&mut T` 为什么不等价吗？
- [ ] 错误状态是否与成功 payload 绑定？
- [ ] 状态转换能否消费旧状态，避免回退到非法状态？
- [ ] 我是否知道 abstraction 产生的 allocation、branch 和 indirection？
- [ ] `unsafe`/FFI 的 safety invariant 是否逐条可审查？
- [ ] Rust 示例是否通过 `fmt/check/clippy/test` 或对应单文件验证？
- [ ] C 示例是否通过 FIL-C，而不是静默使用系统 Clang？
- [ ] 我是否把 Rust 学到的 owner/state/boundary 思维带回了 C API？

## 16. 后续建议

接下来的学习不应从“Rust 语法大全”开始，而应按真实系统问题推进：

1. ownership 与 allocator；
2. slice 与 parser；
3. enum 与状态机；
4. Builder/IR 到 runtime representation；
5. arena、cache locality 与 pointer chasing；
6. trait/vtable 与 ABI；
7. atomics、locks 与 `Send`/`Sync`；
8. unsafe、FFI、mmap 和存储设备接口。

每个主题都写一对 C/Rust 实现，使用相同输入和不变量，分别通过 FIL-C 和
Rust 工具链验证。这样学习到的不是两套孤立语法，而是一套能在两种语言之间
迁移的系统设计能力。

## 17. 参考资料

- [The Rust Programming Language](https://doc.rust-lang.org/book/)
- [The Rust Reference](https://doc.rust-lang.org/reference/)
- [The Rustonomicon](https://doc.rust-lang.org/nomicon/)
- [Rust std](https://doc.rust-lang.org/std/)
- [Rust API Guidelines](https://rust-lang.github.io/api-guidelines/)
- [FIL-C](https://fil-c.org/)
- [Compile Runtime Representation Skill](../../skills/rust/compile-runtime-representation/SKILL.md)
- [Pointer Chasing 答疑](pointer-chasing.md)
