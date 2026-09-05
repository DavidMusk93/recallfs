# Cloudflare 1.1.1.1 DNS 缓存内存优化：Rust 工程启示

> 原文：Sebastiaan Neuteboom, *How we saved 100 terabytes of memory by
> optimizing 1.1.1.1's DNS cache*, Cloudflare Blog, 2026-08-27.
>
> 用户提供的原链接：
> [https://blog.cloudflare.com/dns-cache-memory-optimization-1111/?utm_campaign=cf_blog&utm_content=20260827&utm_medium=organic_social&utm_source=twitter](https://blog.cloudflare.com/dns-cache-memory-optimization-1111/?utm_campaign=cf_blog&utm_content=20260827&utm_medium=organic_social&utm_source=twitter)
>
> Canonical URL：
> [https://blog.cloudflare.com/dns-cache-memory-optimization-1111/](https://blog.cloudflare.com/dns-cache-memory-optimization-1111/)
>
> 阅读日期：2026-09-05。本文中的 Cloudflare 数据均来自上述原文；
> 标为“工程推导”的内容是面向 Rust 和数据库内核开发的进一步抽象。

## 0. 结论先行

这篇文章最值得学习的不是某个 `Box` 技巧，而是一套完整的方法：

> 先根据对象的生命周期收紧类型能力，再根据真实数据分布改变内存表示，
> 最后同时用分配器、微基准和生产 RSS 验证结果。

Cloudflare 的 Big Pineapple 在任意时刻保存超过 2500 亿条 DNS 缓存记录。
在这个基数上，每条记录浪费 `1 byte`，全网就会浪费超过 `250 GB`。他们用
五个连续改动把 benchmark 中的单条净内存占用从 `953 bytes` 降至
`420 bytes`，并在生产环境释放约 `100 TB` 内存。更关键的是，内存减少没有
以速度为代价：插入吞吐提高 `43%`，查询延迟降低 `19%`。

对 Rust 开发最重要的八点启示是：

1. **类型应表达对象当前阶段，而不是历史上最强的能力。** 构建期需要
   `Vec<T>`，发布后只读就应冻结为 `Box<[T]>`；`String` 同理可冻结为
   `Box<str>`。
2. **先消灭 allocation，再压缩 field。** 三个独立列表合并为一个连续列表，
   不只减少结构体字段，还减少 heap allocation 和 pointer chasing。
3. **让常见值便宜，让罕见值付费。** owner 通常等于 cache key，就用
   `None` 表示默认值；只有例外值才分配。
4. **Rust enum 的成本由最大 variant 决定。** 数据高度偏斜时，直接 enum
   可能让最常见的小 variant 为最罕见的大 variant 支付 padding。
5. **boxing 是过渡方案，不一定是终态。** 它缩小了 enum，却引入 allocator
   size class 浪费、额外分配和 cache miss。最终的连续字节表示同时解决三者。
6. **最优内存格式通常是“语义结构 + 编码字节”的混合。** 完全缓存 DNS
   response wire image 会损害 DNSSEC 等动态行为；只缓存 record data 的
   wire bytes 才是正确边界。
7. **一次看似多余的 `memcpy` 可能换来全局更优。** 复用 scratch `Vec<u8>`
   完成编码，再按最终长度请求一个 `Box<[u8]>`，比长期保留过量 capacity
   或每条 record 单独分配更好。
8. **极致优化必须是 evidence-driven。** `size_of`、分配次数、live bytes、
   allocator size class、CPU cache miss、吞吐、尾延迟和生产 RSS 缺一不可。

整个优化路径可以概括为：

```text
Mutable builder
      |
      v
Freeze capacity
      |
      v
Merge allocations
      |
      v
Elide common values
      |
      v
Split hot and cold variants
      |
      v
Pack canonical bytes
      |
      v
Measure allocator + CPU + RSS
```

## 1. 原文建立了怎样的证据链

### 1.1 规模决定优化优先级

原文给出的规模和结果如下：

| 指标 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| 单条净内存占用 | 953 bytes | 420 bytes | -56% |
| 单条分配字节 | 1.1 KB | 461 bytes | -58% |
| 插入吞吐 | 625,000 entries/s | 893,000 entries/s | +43% |
| 查询延迟 | 828 ns | 670 ns | -19% |
| 生产 p99 RSS | 9.3 GB | 5.3 GB | -43% |
| 生产 p90 RSS | 6.5 GB | 3.8 GB | -42% |
| 全网 steady-state working set | - | - | 约 -100 TB |

单条 benchmark 差值为 `533 bytes`。若机械乘以 2500 亿条，理论值约为
`133.25 TB`。生产观测约为 `100 TB` 并不矛盾，因为：

- benchmark 的记录分布只近似生产；
- 各数据中心的 occupancy 和 ECS 使用量不同；
- RSS 还包含缓存之外的进程数据、allocator arena 和 fragmentation；
- rollout 期间实例重启后会从 cold cache 重新增长。

这说明优化收益应分别报告：

```rust
#[derive(Debug, Clone, Copy)]
struct MemoryEvidence {
    object_bytes: usize,
    requested_heap_bytes: usize,
    allocation_count: usize,
    process_rss_bytes: u64,
}

fn fleet_upper_bound(bytes_saved_per_entry: u64, live_entries: u64) -> u128 {
    u128::from(bytes_saved_per_entry) * u128::from(live_entries)
}
```

`object_bytes * cardinality` 是立项上界，不是生产结论。生产结论必须来自
steady-state RSS 或 allocator resident/active memory。

### 1.2 benchmark 不是随便造数据

Cloudflare 使用近似生产流量的数据分布填满缓存：

- `56%` A；
- `25%` AAAA；
- `19%` TXT，代表其他非 A/AAAA 的变长类型；
- 每条 cache entry 含 `1..=4` 条 record；
- TXT 长度随机分布在 `64..=224 bytes`。

他们用自定义 allocator 包装 Rust `System` allocator，统计每条 entry 的
分配次数和大小，同时测量完整缓存路径的插入吞吐和查询延迟。rollout 后再用
生产实例的 p90、p98、p99 RSS 验证。

这里形成了三层证据：

| 层次 | 回答的问题 | 典型指标 |
| --- | --- | --- |
| Layout | 类型本身有多大 | `size_of`、alignment、padding |
| Allocator | 实际向 heap 申请多少 | calls、requested、size class、fragmentation |
| Process/Fleet | 全系统最终省了多少 | RSS、working set、percentile、总实例数 |

只做 `size_of::<T>()` 无法发现 heap capacity；只看 requested bytes 无法发现
jemalloc size class；只看 RSS 又无法归因到某个结构改动。

## 2. 五步优化到底做了什么

### 2.1 第一步：把可增长容器冻结

64 位环境中的 `Vec<T>` 通常携带三项 metadata：

```text
ptr | len | capacity
```

`Box<[T]>` 只需：

```text
ptr | len
```

DNS response 一旦进入缓存就不再修改，因此 `capacity` 永远不会被使用。
Cloudflare 将 8 个 `Vec`/`String` 字段改为 `Box<[T]>`/`Box<str>`：

- 每个字段减少一个 8-byte capacity；
- 单条 entry 的 inline metadata 减少 `64 bytes`；
- 同时不再保留 `Vec` 预留但未使用的 heap slots；
- 仅 64-byte metadata 一项，在 2500 亿条规模上就超过 `15 TB`。

Rust 中的正确模式不是禁止 `Vec`，而是区分 build phase 和 frozen phase：

```rust
#[derive(Debug)]
struct EntryBuilder<T> {
    records: Vec<T>,
}

#[derive(Debug)]
struct FrozenEntry<T> {
    records: Box<[T]>,
}

impl<T> EntryBuilder<T> {
    fn freeze(self) -> FrozenEntry<T> {
        FrozenEntry {
            records: self.records.into_boxed_slice(),
        }
    }
}
```

`into_boxed_slice()` 表达了“不再增长”的所有权语义。不过不能只凭 API 名称
断言 allocator 一定归还物理尾部空间。若要让最终 allocation request 与数据
长度一致，原文后面采用的 scratch buffer + 新 `Box<[u8]>` copy 更可控；
allocator 仍可能按 size class 向上取整。

### 2.2 第二步：合并列表，减少指针和分配

原结构把 answer、authority、additional 三个 section 分别放在三个列表中。
优化后把所有 record 放入一个连续 `Box<[T]>`，再保存 section 起点。

原文指出每个独立 `Box<[T]>` 具有 8-byte pointer 和 8-byte length。删除
两个列表并增加两个 `u16` offset，单条 entry 节省：

```text
2 * (8 + 8) - 2 * 2 = 28 bytes
```

一个安全的 Rust 抽象如下：

```rust
#[derive(Debug, Clone, Copy)]
enum Section {
    Answer,
    Authority,
    Additional,
}

#[derive(Debug)]
struct Sections<T> {
    records: Box<[T]>,
    authority_start: u16,
    additional_start: u16,
}

impl<T> Sections<T> {
    fn try_new(
        answer: Vec<T>,
        authority: Vec<T>,
        additional: Vec<T>,
    ) -> Result<Self, &'static str> {
        let total = answer
            .len()
            .checked_add(authority.len())
            .and_then(|n| n.checked_add(additional.len()))
            .ok_or("record count overflow")?;
        if total > usize::from(u16::MAX) {
            return Err("record count exceeds u16");
        }

        let authority_start =
            u16::try_from(answer.len()).map_err(|_| "authority offset overflow")?;
        let additional_start = u16::try_from(answer.len() + authority.len())
            .map_err(|_| "additional offset overflow")?;

        let mut records = Vec::with_capacity(total);
        records.extend(answer);
        records.extend(authority);
        records.extend(additional);

        Ok(Self {
            records: records.into_boxed_slice(),
            authority_start,
            additional_start,
        })
    }

    fn get(&self, section: Section) -> &[T] {
        let authority = usize::from(self.authority_start);
        let additional = usize::from(self.additional_start);

        match section {
            Section::Answer => &self.records[..authority],
            Section::Authority => &self.records[authority..additional],
            Section::Additional => &self.records[additional..],
        }
    }
}
```

这里的 `u16` 不是“为了省内存强行截断”，而是从 DNS section 的业务上界
推导出的 narrow integer。工程要求是：

- 构造入口必须检查 `usize -> u16`；
- offset 必须保持单调；
- 外部输入不能直接构造内部结构；
- 若真实 workload 可能超过上界，应升级类型，而不是 wrap/truncate。

原文还把多个 `bool` 合并成 bitflag。收益不只来自 bit 本身，还可能消掉
alignment padding。但 Rust 默认布局不是稳定 ABI，必须在目标编译器和目标
架构上重新测量。

### 2.3 第三步：默认值不存，只存例外

绝大多数 DNS record 的 owner 与查询域名相同。旧设计在每条 record 中重复
存储完整 owner；优化后：

- `None`：owner 等于 cache key 中的 qname；
- `Some(Box<Name>)`：只为 CNAME 链等不同 owner 保存完整值。

可用 Rust 表达为：

```rust
#[derive(Debug)]
struct CacheKey {
    qname: Box<str>,
}

#[derive(Debug)]
struct CachedRecord {
    owner_override: Option<Box<str>>,
    payload: Box<[u8]>,
}

impl CachedRecord {
    fn owner<'a>(&'a self, key: &'a CacheKey) -> &'a str {
        self.owner_override
            .as_deref()
            .unwrap_or(key.qname.as_ref())
    }
}

fn owner_override(owner: &str, key: &CacheKey) -> Option<Box<str>> {
    (owner != key.qname.as_ref()).then(|| Box::<str>::from(owner))
}
```

这是一种 common-value elision，也是一种语义压缩。它比通用字符串
interning 更直接，因为默认值已经稳定存在于 cache key。

代价也必须写进类型契约：

- record 不再 self-contained；
- 解码必须同时持有 cache key；
- key 与 value 的生命周期和一致性成为共同不变量；
- 导出、日志和调试工具不能脱离 key 单独解释 record。

### 2.4 第四步：处理 enum 的最大 variant 税

原文中的 `RecordData` enum 最大 variant 是 136-byte NAPTR。加上 tag 和
padding 后，整个 enum 为 `144 bytes`。但 A 只需要 4 bytes，AAAA 只需要
16 bytes，而二者占流量的 80% 以上。

直接 enum 等价于让每个 A/AAAA 都为罕见 NAPTR 预留空间。过渡优化是只把
大 variant 放到 heap：

```rust
#[derive(Debug)]
struct Naptr {
    order: u16,
    preference: u16,
    flags: Box<str>,
    services: Box<str>,
    regexp: Box<str>,
    replacement: Box<str>,
}

#[derive(Debug)]
enum RecordData {
    A([u8; 4]),
    Aaaa([u8; 16]),
    Txt(Box<[u8]>),
    Naptr(Box<Naptr>),
}
```

原文的实际类型中，boxing 后 enum 缩为 24 bytes，因此 A/AAAA 每条可节省
约 `120 bytes`。但不能把这个数字照搬到任意 enum；泛型、niche、alignment、
编译器版本和 target 都会影响结果。

应在自己的目标环境运行布局探针：

```rust
use std::mem::{align_of, size_of};

fn print_layout<T>(name: &str) {
    println!(
        "{name}: size={}, align={}",
        size_of::<T>(),
        align_of::<T>()
    );
}

fn main() {
    print_layout::<Vec<u8>>("Vec<u8>");
    print_layout::<Box<[u8]>>("Box<[u8]>");
    print_layout::<String>("String");
    print_layout::<Box<str>>("Box<str>");
    print_layout::<RecordData>("RecordData");
    print_layout::<Option<Box<Naptr>>>("Option<Box<Naptr>>");
}
```

不要用 `#[repr(packed)]` 作为通用省内存手段。它会制造 unaligned field，
让引用访问变得危险，也可能让 CPU 访问更贵。`#[repr(C)]` 的目标是稳定的
C-compatible layout，不是自动获得最小尺寸。磁盘/网络格式应显式编码，
而不是直接 dump Rust struct。

### 2.5 boxing 的两笔隐藏账

原文明确指出 boxing 不是免费午餐。

**第一笔是 allocator rounding。** Big Pineapple 生产环境使用 jemalloc。
例如：

- 申请 32 bytes 的 TXT 正好进入 32-byte bin；
- 申请 40 bytes 的 MX 会进入 48-byte bin，浪费 8 bytes。

因此：

```text
requested size != usable size != resident size
```

**第二笔是 locality。** 未 boxing 时，一组 enum 在同一连续 allocation；
boxing 后，每个 payload 可能散落在不同 heap region。查询一次 entry 需要
追逐多个 pointer，并加载更多 CPU cache line。

这解释了为什么“enum 变小”不等于“系统必然更快、更省”。优化对象必须是
完整对象图，而不是根结构体。

### 2.6 第五步：把 record data 编译成紧凑 wire bytes

最终方案不再缓存 parsed `RecordData` enum，而是把所有 record data 编码进
一个 `Box<[u8]>`：

```text
u16 length | encoded record | u16 length | encoded record | ...
```

它同时消除了：

- 每个 variant 按最大 variant 扩大的 inline 空间；
- boxing 大 variant 产生的每条 heap allocation；
- allocator size-class rounding；
- pointer chasing 和差 locality；
- lookup 时把已解析字段逐个重新序列化的工作。

多数 A、AAAA、TXT 和 DNSSEC record 可以直接从缓存 copy 到 response。
含 domain name 的 CNAME、NS、MX、SOA 仍需解析，以应用 DNS name
compression。原文报告，这一步让 lookup latency 再降低 `5%`。

Cloudflare 没有缓存完整 response wire image，原因是完整 response 仍受
client-specific 状态影响，例如：

- message ID；
- DNSSEC DO flag；
- 是否需要过滤 DNSSEC records；
- domain name compression 的具体输出。

这是一条很重要的边界：

> 缓存最稳定、最接近输出的 canonical fragment，而不是缓存包含动态语义的
> 完整最终产物。

## 3. 一个安全的 Rust 紧凑存储实现

下面的示例复现原文方案的关键性质：

- record 使用 2-byte big-endian length prefix；
- answer、authority、additional 共用一个 allocation；
- scratch `Vec<u8>` 跨构建复用；
- frozen entry 按最终数据长度请求 `Box<[u8]>`；
- section 使用 byte offset，构造时检查 `usize -> u32`；
- iterator 对 truncated/corrupted bytes 返回错误，不产生越界访问；
- 不使用 `unsafe`。

原文合并 parsed record 列表时使用 `u16` record offset。这里 packed bytes 的
section 边界是 byte offset，因此使用 `u32`，避免把“record 数量上界”和
“编码字节数上界”混为一谈。

```rust
use std::convert::TryFrom;

const LEN_PREFIX: usize = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Section {
    Answer,
    Authority,
    Additional,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackError {
    RecordTooLarge,
    EntryTooLarge,
    TruncatedLength,
    TruncatedRecord,
    InvalidSectionBoundary,
}

#[derive(Debug)]
pub struct PackedRecords {
    bytes: Box<[u8]>,
    authority_at: u32,
    additional_at: u32,
}

impl PackedRecords {
    pub fn section_bytes(&self, section: Section) -> &[u8] {
        let authority = self.authority_at as usize;
        let additional = self.additional_at as usize;

        match section {
            Section::Answer => &self.bytes[..authority],
            Section::Authority => &self.bytes[authority..additional],
            Section::Additional => &self.bytes[additional..],
        }
    }

    pub fn records(&self, section: Section) -> PackedIter<'_> {
        PackedIter {
            remaining: self.section_bytes(section),
            failed: false,
        }
    }

    pub fn validate(&self) -> Result<(), PackError> {
        let authority = self.authority_at as usize;
        let additional = self.additional_at as usize;
        if authority > additional || additional > self.bytes.len() {
            return Err(PackError::InvalidSectionBoundary);
        }

        for section in [
            Section::Answer,
            Section::Authority,
            Section::Additional,
        ] {
            for record in self.records(section) {
                record?;
            }
        }
        Ok(())
    }
}

pub struct PackedIter<'a> {
    remaining: &'a [u8],
    failed: bool,
}

impl<'a> Iterator for PackedIter<'a> {
    type Item = Result<&'a [u8], PackError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.failed || self.remaining.is_empty() {
            return None;
        }
        if self.remaining.len() < LEN_PREFIX {
            self.failed = true;
            return Some(Err(PackError::TruncatedLength));
        }

        let len = u16::from_be_bytes([
            self.remaining[0],
            self.remaining[1],
        ]) as usize;
        let end = match LEN_PREFIX.checked_add(len) {
            Some(end) if end <= self.remaining.len() => end,
            _ => {
                self.failed = true;
                return Some(Err(PackError::TruncatedRecord));
            }
        };

        let record = &self.remaining[LEN_PREFIX..end];
        self.remaining = &self.remaining[end..];
        Some(Ok(record))
    }
}

#[derive(Debug, Default)]
pub struct PackedBuilder {
    scratch: Vec<u8>,
}

impl PackedBuilder {
    pub fn build(
        &mut self,
        answer: &[&[u8]],
        authority: &[&[u8]],
        additional: &[&[u8]],
    ) -> Result<PackedRecords, PackError> {
        self.scratch.clear();

        self.append(answer)?;
        let authority_at = self.offset()?;

        self.append(authority)?;
        let additional_at = self.offset()?;

        self.append(additional)?;
        self.offset()?;

        // One final-length allocation request. Scratch keeps its capacity.
        let bytes = Box::<[u8]>::from(self.scratch.as_slice());
        let packed = PackedRecords {
            bytes,
            authority_at,
            additional_at,
        };
        packed.validate()?;
        Ok(packed)
    }

    fn append(&mut self, records: &[&[u8]]) -> Result<(), PackError> {
        for record in records {
            let len = u16::try_from(record.len())
                .map_err(|_| PackError::RecordTooLarge)?;
            self.scratch.extend_from_slice(&len.to_be_bytes());
            self.scratch.extend_from_slice(record);
        }
        Ok(())
    }

    fn offset(&self) -> Result<u32, PackError> {
        u32::try_from(self.scratch.len())
            .map_err(|_| PackError::EntryTooLarge)
    }
}

pub fn append_cached_record(
    output: &mut Vec<u8>,
    encoded_record: &[u8],
) {
    output.extend_from_slice(encoded_record);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn collect<'a>(
        records: impl Iterator<Item = Result<&'a [u8], PackError>>,
    ) -> Vec<&'a [u8]> {
        records.map(Result::unwrap).collect()
    }

    #[test]
    fn packs_three_sections_into_one_buffer() {
        let a: &'static [u8] = b"a";
        let aaaa: &'static [u8] = b"aaaa";
        let ns: &'static [u8] = b"ns";
        let txt: &'static [u8] = b"txt";

        let mut builder = PackedBuilder::default();
        let packed = builder
            .build(&[a, aaaa], &[ns], &[txt])
            .unwrap();

        assert_eq!(
            collect(packed.records(Section::Answer)),
            vec![a, aaaa]
        );
        assert_eq!(
            collect(packed.records(Section::Authority)),
            vec![ns]
        );
        assert_eq!(
            collect(packed.records(Section::Additional)),
            vec![txt]
        );
    }

    #[test]
    fn rejects_records_larger_than_u16() {
        let oversized = vec![0_u8; usize::from(u16::MAX) + 1];
        let mut builder = PackedBuilder::default();

        assert!(matches!(
            builder.build(&[oversized.as_slice()], &[], &[]),
            Err(PackError::RecordTooLarge)
        ));
    }
}
```

这个实现故意保留了一次 copy：

```text
parsed records -> reusable scratch -> final-length Box<[u8]>
```

如果直接把 scratch `Vec<u8>` 移交给 entry：

- builder 无法跨 entry 复用 capacity；
- entry 可能永久保留高水位 capacity；
- `shrink_to_fit()` 只是请求，allocator 不保证真正缩小 allocation；
- 大量 entry 会把一次偶发大记录的 capacity 放大为长期 working set。

原文报告，scratch + 单次按最终长度发起 allocation request 的改动，单独让
插入吞吐提高 `13%`。原因不是 copy 免费，而是它替换了每条 boxed record 的
独立 allocation，并让 allocator 和 cache locality 的总成本更低。

生产实现还应增加：

- cache format version；
- record type 和 flags；
- 对 byte order 的显式约定；
- fuzz/property tests；
- corrupt entry 的淘汰和监控；
- schema 迁移或进程升级时的兼容策略；
- 对恶意长度、总 entry 大小和 record 数量的硬上限。

## 4. 如何测量 Rust 对象的真实成本

### 4.1 先做 layout audit

每次涉及核心结构体布局的 PR，都应记录目标三元组、Rust 版本和以下数据：

```rust
use std::mem::{align_of, size_of, size_of_val};

fn audit<T>(name: &str, value: &T) {
    println!(
        "{name}: static_size={}, dynamic_size={}, align={}",
        size_of::<T>(),
        size_of_val(value),
        align_of::<T>()
    );
}
```

注意：

- `size_of::<Vec<T>>()` 不包含 heap allocation；
- `size_of_val(&*boxed_slice)` 只给 slice payload 大小，不包含 allocator
  rounding；
- `Box<[T]>` 和 `Box<str>` 是 fat pointer，其当前大小应实测；
- `Option<Box<T>>` 常可利用 null pointer optimization，但 ABI/FFI 约束仍需
  查 Rust 保证，不能靠“看起来一样大”设计磁盘格式；
- struct padding 需要看整体，而不是简单相加 field size。

### 4.2 再计数 allocator

原文 benchmark 用自定义 allocator 包装 `System`。最小计数器可以从
`GlobalAlloc` 开始：

```rust
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering};

struct CountingAlloc;

static LIVE_BYTES: AtomicU64 = AtomicU64::new(0);
static ALLOC_CALLS: AtomicU64 = AtomicU64::new(0);

unsafe impl GlobalAlloc for CountingAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let ptr = unsafe { System.alloc(layout) };
        if !ptr.is_null() {
            LIVE_BYTES.fetch_add(layout.size() as u64, Ordering::Relaxed);
            ALLOC_CALLS.fetch_add(1, Ordering::Relaxed);
        }
        ptr
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        LIVE_BYTES.fetch_sub(layout.size() as u64, Ordering::Relaxed);
        unsafe { System.dealloc(ptr, layout) };
    }
}

#[global_allocator]
static GLOBAL: CountingAlloc = CountingAlloc;
```

这段代码只统计 requested live bytes，不能代表：

- jemalloc 实际 usable size；
- arena retained bytes；
- dirty/muzzy pages；
- fragmentation；
- OS RSS/PSS；
- 多线程 benchmark 中其他任务的分配。

因此应把 allocator 计数放在隔离进程中，并在生产 allocator 上再测一次。
若生产使用 jemalloc，还应采集其 size class、allocated、active、resident、
retained 和 profiling 数据。

### 4.3 最后验证 CPU 与生产行为

建议最少保留以下对照矩阵：

| 维度 | 必测项 |
| --- | --- |
| 数据 | 生产 type/size/count 分布；ECS 与非 ECS 分层 |
| 内存 | entry bytes、alloc calls、requested、usable、RSS |
| CPU | insert/s、lookup ns、p50/p99、cycles、cache misses |
| 负载 | cold fill、steady state、eviction、mixed read/write |
| 版本 | baseline 与每个独立优化步骤 |
| 生产 | canary、分位数 plateau、rollout 前后同口径 |

不要只测“构造一个对象”。原文测完整 insert 和 lookup flow，因而能发现：

- 构造时的 allocation 数量；
- lookup 时的 pointer chasing；
- wire bytes 直接 copy 避免的 serialization；
- 内存减少对 CPU cache locality 的正收益。

## 5. 可以抽象出的 Rust 开发技能

这些不是零散语法，而是可复用的工程能力单元。

### Skill 1：Lifecycle-driven type design

**触发条件：** 对象先构建、后长期只读。

**能力：**

- 用 `Vec<T>`/`String` 完成 build；
- 在 publish boundary 转成 `Box<[T]>`/`Box<str>`；
- 让类型系统禁止发布后的扩容；
- 评估并发共享时 `Box`、`Arc<[T]>` 和 arena 的不同成本。

**验收：** frozen 类型没有无效 capacity，变更 API 无法绕过冻结边界。

### Skill 2：Rust layout forensics

**触发条件：** 高基数对象、hot struct、large enum。

**能力：**

- 使用 `size_of`、`align_of`、`size_of_val`；
- 识别 enum max-variant tax、struct padding 和 niche optimization；
- 比较 target/compiler/build profile；
- 区分 Rust layout、C ABI 和显式 wire format。

**验收：** PR 中给出 before/after layout 表，而不是只凭字段算术。

### Skill 3：Cardinality-bounded representation

**触发条件：** index、count、offset 的业务上界远小于机器字长。

**能力：**

- 从协议/业务不变量证明 `u16`、`u32` 是否足够；
- 在构造边界执行 checked conversion；
- 用 monotonic offset 重建多个逻辑 section；
- 对 overflow 和 malformed input 做测试。

**验收：** 不存在 `as u16` 静默截断；上界变化会显式失败。

### Skill 4：Common-case compression

**触发条件：** 某字段大多数时候等于 key、默认值或可推导值。

**能力：**

- 用 `Option` 表示 override；
- 从稳定 context 恢复默认值；
- 只让 rare case 承担 allocation；
- 写清对象不再 self-contained 后的生命周期和一致性约束。

**验收：** 用生产分布证明 common case 占比，并测 allocation 命中率。

### Skill 5：Skew-aware enum design

**触发条件：** enum variant 尺寸差异大，且流量高度偏斜。

**能力：**

- 统计 variant frequency 与真实 allocated size；
- inline small/hot variants，box large/cold variants；
- 评估 allocator bin 和 locality；
- 判断 boxing 是终态还是迁移到 packed format 的过渡态。

**验收：** 同时报告 root enum size、完整对象图 bytes、alloc calls 和 lookup
cache misses。

### Skill 6：Packed immutable format

**触发条件：** 对象发布后只读、数量小、遍历多于随机访问。

**能力：**

- 设计 length-prefixed 或 offset-table 编码；
- 将多个对象合并为一个 allocation；
- 编写 checked iterator/parser；
- 对格式做 versioning、fuzzing 和 corruption handling；
- 选择“parsed structure”和“canonical bytes”的正确边界。

**验收：** parser 无 unchecked offset；随机坏输入不会 panic 或越界。

### Skill 7：Scratch-buffer engineering

**触发条件：** 高频构建变长对象，最终对象不应保留 builder 的多余 capacity。

**能力：**

- 跨操作复用 scratch `Vec<u8>`；
- 用 `clear()` 保留 builder capacity；
- 完成后按最终长度请求一次分配并 copy 到 `Box<[u8]>`；
- 设置异常大输入后的 capacity retention 策略。

**验收：** steady-state 下 builder 很少 reallocate；entry 不保留 scratch
高水位 capacity。

### Skill 8：Allocator-aware benchmarking

**触发条件：** 优化涉及 boxing、容器、字符串或对象图。

**能力：**

- 用 `GlobalAlloc` 或生产 allocator profiler 计数；
- 区分 requested、usable、active、resident、RSS；
- 识别 size-class rounding 和 fragmentation；
- 将 microbenchmark 与 production percentile 对齐。

**验收：** memory、throughput、latency 三者同时无回退，生产 plateau 可复现。

## 6. 对数据库内核开发的直接启发

以下属于工程推导，不是 Cloudflare 原文结论。

### 6.1 Buffer pool descriptor

buffer frame metadata 往往是百万级常驻对象。每 frame 减少 8 bytes，在
1 亿 frame 上就是 800 MB。应检查：

- 不再变化的 variable-length metadata 是否仍用 `Vec`/`String`；
- 多个小数组是否可合并为一个 allocation + offset；
- 多个 `bool` 是否造成 padding；
- 冷字段是否把 hot descriptor 撑大到更多 cache line。

这里尤其适合做 hot/cold split：pin count、state、page id 留在 hot struct，
诊断文本、rare error 和扩展 metadata 放入 cold allocation。

### 6.2 Page/tuple metadata 的默认值省略

owner elision 可类比：

- tuple 的 table/schema id 可从 page 或 segment 推导；
- index entry 的 prefix 可从 page fence key 推导；
- WAL record 的 transaction/LSN context 可由 block header 继承；
- LSM block 中重复 key prefix 可在 block restart point 恢复。

关键不是“能省就省”，而是默认值必须来自同一原子发布单元。若 context 和
payload 能独立更新，就会产生无法解释的组合状态。

### 6.3 WAL、SST block 和 plan cache 的 compiled representation

packed record data 对数据库最直接的类比是：

- WAL decode 后若只用于转发/复制，可保留 validated bytes；
- SST block cache 可缓存接近 on-disk 的 block，加上少量解析索引；
- query plan cache 可把稳定表达式编译成紧凑 opcode/constant arena；
- protocol response cache 可缓存稳定 fragment，动态 patch request id、
  snapshot、权限或 compression。

设计原则是：

```text
Stable semantic core + explicit dynamic context -> final output
```

不要缓存包含 snapshot、transaction visibility、权限或 client option 的完整
最终字节，除非这些因素已经进入 cache key。

### 6.4 一次 copy 与多次 allocation 的取舍

数据库开发中经常把 `memcpy` 视为绝对成本，但这篇文章给出了更完整的成本式：

```text
Total cost =
    copied bytes
  + allocation calls
  + allocator rounding
  + pointer chasing
  + cache misses
  + retained capacity
  + serialization work
```

对小对象批量打包，一次线性 copy 往往比 N 次 allocation 和 N 次随机访问
便宜。page packing、WAL group commit buffer、SST block builder 和 network
batch 都应按总成本测量。

### 6.5 节省内存最终会改变系统容量曲线

Cloudflare 计划把释放的内存重新投入更大的 cache capacity，从而提高命中率、
减少 upstream query。这对数据库也成立：

- 同样 RSS 下扩大 buffer pool/block cache；
- 减少 cache miss 带来的 SSD read 和 tail latency；
- 降低 compaction/read amplification 的间接压力；
- 在不增加服务器数量的前提下提高 working-set coverage。

因此内存优化的最终业务指标不只是 `GB saved`，还包括：

```text
cache hit rate -> downstream I/O -> tail latency -> fleet capacity
```

## 7. 不能从原文直接推出什么

1. **不能假设每个项目都能省 56%。** 原文收益依赖 2500 亿级对象、A/AAAA
   超过 80%、record 数量较少、entry 发布后不可变等具体条件。
2. **不能假设 boxing 总是坏。** 对 rare large variant，它可能是简单有效的
   中间方案；只有完整对象图和 hot-path 数据才能决定。
3. **不能假设 packed bytes 总是优于 parsed object。** 随机访问频繁、修改
   频繁或每次都必须完整解析时，编码格式可能增加 CPU。
4. **不能把 benchmark allocator 数字直接等同于生产 jemalloc RSS。**
   原文因此额外做了 production rollout 测量。
5. **不能依赖 Rust 默认内存布局作为持久格式。** 编译器和 target 可改变
   padding/layout；持久化必须显式编码并版本化。
6. **不能只比较平均延迟。** 原文给出了单项 lookup benchmark 和生产内存
   percentile，但数据库落地还需关注 p99/p999、NUMA 和并发 contention。
7. **不能忽略 rollout 的 cold-cache 影响。** 原文图中的初始下降来自实例
   重启后的空缓存，稳定 plateau 才代表 steady state。

## 8. 建议的落地顺序

### Phase 0：建立基线

- 固化生产 type/size/count 分布；
- 记录 Rust version、target、allocator；
- 同时采集 object layout、alloc calls、usable bytes、RSS、吞吐和尾延迟；
- 把每个高基数对象换算为 fleet amplification。

### Phase 1：低风险冻结

- `Vec<T> -> Box<[T]>`；
- `String -> Box<str>`；
- builder/frozen type 分离；
- 每项独立 benchmark、commit 和 rollout。

### Phase 2：减少对象图

- 合并多个列表；
- 使用 checked narrow offset；
- bitflag 合并布尔状态；
- 默认值/重复 key elision。

### Phase 3：处理数据偏斜

- 统计 enum variant size 和 frequency；
- 先试 hot-inline/cold-box；
- 用生产 allocator 验证 size class；
- 测 cache miss，避免只看 root `size_of`。

### Phase 4：引入 packed format

- 明确 canonical fragment 边界；
- 定义 version、byte order、length、section；
- 用安全 iterator 和 fuzz test 验证 parser；
- scratch 复用，最终按数据长度发起 allocation request；
- 对动态字段保留结构化表示。

### Phase 5：生产验证

- 小比例 canary；
- 等待 cache 达到 steady state；
- 比较同 occupancy、同流量结构下的 p90/p98/p99；
- 检查内存收益是否转化为命中率、I/O 和 tail latency 改善；
- 确认回滚时不存在 cache format 兼容问题。

## 9. 最终检查表

- [ ] 高基数对象的每个 byte 是否都乘过真实 cardinality？
- [ ] 对象 publish 后是否仍保留无用的 grow capability？
- [ ] 是否存在多个可合并的 `Vec`、`Box<[T]>` 或小 allocation？
- [ ] `u16`/`u32` 是否来自已证明上界，并使用 checked conversion？
- [ ] 是否有可从 key/context 推导的重复字段？
- [ ] enum 最大 variant 是否由 rare case 主导？
- [ ] boxing 后是否测过 allocator bin、allocation count 和 cache miss？
- [ ] 是否能缓存 canonical fragment，而不是完整动态 response？
- [ ] scratch buffer 是否复用，frozen entry 是否仍保留多余 capacity？
- [ ] parser 是否处理 overflow、truncation、corruption 和 version？
- [ ] 是否同时测过 layout、allocator、CPU 和生产 RSS？
- [ ] rollout 是否排除了 cold-cache dip，并等待 steady-state plateau？

## 10. 总结

Cloudflare 的五步优化，本质上完成了三次抽象升级：

1. 从“Rust 容器怎么省 8 bytes”升级到“生命周期决定类型能力”；
2. 从“结构体怎么变小”升级到“完整对象图如何减少 allocation 和 cache miss”；
3. 从“内存 benchmark 变好”升级到“生产 RSS、吞吐和延迟共同验证”。

它给数据库和系统 Rust 开发的终态启示是：

> 对海量只读对象，先用可变结构完成构建，再冻结、去重、合并并编码成一个
> 经过校验的连续表示；让 common case 无分配，让 dynamic semantics 留在
> context；最后用生产 allocator 和 steady-state workload 证明收益。

这不是牺牲可维护性换取几个 byte。边界检查、类型冻结、显式格式和分层测量
恰恰使优化后的系统比依赖隐式布局的版本更容易验证。

## 参考资料

- [Cloudflare 原文：How we saved 100 terabytes of memory by optimizing 1.1.1.1's DNS cache](https://blog.cloudflare.com/dns-cache-memory-optimization-1111/?utm_campaign=cf_blog&utm_content=20260827&utm_medium=organic_social&utm_source=twitter)
- [Rust `GlobalAlloc`](https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html)
- [Rust `System` allocator](https://doc.rust-lang.org/std/alloc/struct.System.html)
- [Rust type layout reference](https://doc.rust-lang.org/reference/type-layout.html)
- [Rustonomicon: alternative representations](https://doc.rust-lang.org/nomicon/other-reprs.html)
- [bitflags crate](https://docs.rs/bitflags/latest/bitflags/)
- [jemalloc](https://jemalloc.net/)
- [RFC 1035, domain name compression](https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.4)
