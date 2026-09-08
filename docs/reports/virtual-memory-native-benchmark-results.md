# 虚拟内存 C 实验：EPYC 目标机原生结果

> 目标机：`dc02-pe-t137-n047`（`fdbd:dc02:e:137::47`）
>
> 测试日期：2026-09-07 至 2026-09-08
>
> 本文是
> [《Virtual Memory From First Principles》开发者工程评析](virtual-memory-engineering-notes.md)
> 的目标机证据附件。概念、适用边界和 Linux 诊断流程见主报告。

## 0. 结论

| 要点 | 实验 | 中位结果 | 工程含义 |
| --- | --- | ---: | --- |
| reserve 不等于 resident | 1 GiB anonymous `mmap` | `4.761 us` | 建立 VMA 不等于物理页就绪 |
| first-touch 有真实成本 | 首次/再次逐 4 KiB 写 1 GiB | `193.459 / 3.124 ms` | 首次触页慢 `61.93x` |
| 权限由硬件执行 | read-only 页写入 | `SIGSEGV(11)` | 普通 load/store 没有 errno 路径 |
| 访问顺序改变成本 | 每页读 1 byte，顺序/随机 | `10.724 / 14.604 ns` | 随机访问慢 `1.36x` |
| Huge Page 不是万能加速 | random base/THP/HugeTLB | `14.604 / 14.115 / 14.150 ns` | hot loop 只改善约 `3%` |
| COW 推迟复制 | 512 MiB empty/write child | `4.561 / 153.961 ms` | 写路径增加 131072 minor fault |
| `mmap` 胜负取决于生命周期 | 2 GiB cold remap / warm persistent | `2.726 vs 3.360 / 19.954 vs 9.582 GiB/s` | cold remap 落后，warm persistent 领先 `2.08x` |
| 页表修改需要跨核同步 | 64 MiB `mprotect` | `166 -> 596 us` | 1 到 63 reader 成本约 `3.60x` |
| NUMA 成本被虚拟地址隐藏 | CPU node3/node2/node0 读 N3 | `22.925/20.275/13.056 GiB/s` | 跨 socket 下降 `43.05%` |

这些结果只适用于本文记录的机器、内核、编译器、数据规模和访问模式。
正式轮次原始向量和 source/binary digest 分别固化于
[`evidence/virtual-memory-native-20260907-final6.tsv`](evidence/virtual-memory-native-20260907-final6.tsv)
与
[`evidence/mmap-pread-tradeoff-20260908.tsv`](evidence/mmap-pread-tradeoff-20260908.tsv)。

## 1. 工具链边界

五个 C 程序和一个共用 header：

- [`virtual_memory_demo.c`](../../tools/docs/examples/virtual_memory_demo.c)
- [`virtual_memory_benchmark.c`](../../tools/docs/examples/virtual_memory_benchmark.c)
- [`virtual_memory_mprotect_benchmark.c`](../../tools/docs/examples/virtual_memory_mprotect_benchmark.c)
- [`virtual_memory_file_benchmark.c`](../../tools/docs/examples/virtual_memory_file_benchmark.c)
- [`mmap_pread_tradeoff_benchmark.c`](../../tools/docs/examples/mmap_pread_tradeoff_benchmark.c)
- [`virtual_memory_benchmark_support.h`](../../tools/docs/examples/virtual_memory_benchmark_support.h)

FIL-C 0.684 只负责已执行路径的功能正确性、内存安全和 UB 边界，不作为
性能基线：

```bash
.tmp/fil-c/bin/filcc -Itools/docs/examples \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_demo.c \
  -o .tmp/virtual-memory-demo
.tmp/fil-c/bin/filrun .tmp/virtual-memory-demo

.tmp/fil-c/bin/filcc -Itools/docs/examples \
  -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  tools/docs/examples/virtual_memory_benchmark.c \
  -o .tmp/virtual-memory-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-benchmark-filc selftest

.tmp/fil-c/bin/filcc -Itools/docs/examples \
  -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  tools/docs/examples/virtual_memory_mprotect_benchmark.c \
  -o .tmp/virtual-memory-mprotect-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-mprotect-benchmark-filc selftest

.tmp/fil-c/bin/filcc -Itools/docs/examples \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/virtual_memory_file_benchmark.c \
  -o .tmp/virtual-memory-file-benchmark-filc
.tmp/fil-c/bin/filrun .tmp/virtual-memory-file-benchmark-filc selftest
```

FIL-C selftest 覆盖：

- 页权限、anonymous first-touch、重复触页、COW、`MAP_SHARED`；
- sequential/random base walk；
- THP advice 分支；
- checksum 与 order permutation；
- COW parent isolation；
- `mprotect` reader participation；
- cold/warm `pread` 与 `mmap`；
- 负数、零和越界参数拒绝。

HugeTLB 依赖目标机预留池，使用目标原生构建验证；FIL-C VM 不提供等价池。

目标机原生构建：

```bash
flags='-std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto -Wall -Wextra -Werror'

gcc $flags -I. virtual_memory_demo.c -o virtual_memory_demo
gcc $flags -I. virtual_memory_benchmark.c -o virtual_memory_benchmark
gcc $flags -I. -pthread virtual_memory_mprotect_benchmark.c \
  -o virtual_memory_mprotect_benchmark
gcc $flags -I. \
  virtual_memory_file_benchmark.c -o virtual_memory_file_benchmark
```

## 2. 目标环境

| 项目 | 值 |
| --- | --- |
| Kernel | Linux `5.15.152.bsk.15-amd64` |
| CPU | 2 sockets, AMD EPYC 7Y83, 64 cores/socket, SMT2 |
| Logical CPU | 256 |
| NUMA | 4 nodes，每 node 约 512 GiB |
| RAM | 2.0 TiB；测试前 `MemAvailable` 约 1.8 TiB |
| Swap | 0 |
| Base page | 4 KiB |
| THP | `madvise` |
| HugeTLB | 2 MiB；测试时全机 1103 页空闲 |
| Governor | `performance` |
| Compiler | GCC 12.2.0 |
| Repeat | 每个正式 case 7 次 |

CPU/内存控制：

| 用途 | CPU | CPU node | Memory node | Distance |
| --- | ---: | ---: | ---: | ---: |
| local | 96 | 3 | 3 | 10 |
| same-socket neighbor | 64 | 2 | 3 | 12 |
| cross-socket remote | 1 | 0 | 3 | 32 |
| shootdown | coordinator 96，reader 97-127/224-255 | 3 | 3 | 10 |

walk、bandwidth 和 mprotect case 使用：

```bash
VM_BENCH_EXPECT_NODE=3 \
  numactl --physcpubind=<cpus> --membind=3 <command>
```

程序读取 `/proc/self/numa_maps`，若任何 resident page 不在 N3 则失败。
2 GiB NUMA case 均报告 `N3=524288`。fault 与 COW case 由外层
`numactl --membind=3` 约束，但不把该外层约束冒充程序内 placement 断言。

机器是共享生产节点。测试时整体约 `96-98% idle`，但仍有业务进程运行。结果
保留 7 次范围，不把最好一次当结论。未执行 OOM、swap 或内存压力注入。

## 3. 源码、二进制与反优化证据

final6 SHA-256：

| Artifact | SHA-256 |
| --- | --- |
| `virtual_memory_demo.c` | `5d2910727cafa1d8760cd798dc210ad396c282449d6a2442407c4659741e2a72` |
| `virtual_memory_benchmark.c` | `1a76bc1a7568db51150c341d0a3daa1ddb8da5213177e089d23c1739002c4cfd` |
| `virtual_memory_mprotect_benchmark.c` | `0e8977f94206dfc1144b5bd1fb52a4431f528d47a23e8b0ee83b4ffe29849bcf` |
| `virtual_memory_file_benchmark.c` | `ec9038e8ddeaa48214fe73b7612ad7729d2bcecc6631aded68161d49a5d2e9c8` |
| `virtual_memory_benchmark_support.h` | `1a788082817c648a1d98ad15069768eca5e4db705523f4bd9d009bcc370074f1` |
| `virtual_memory_demo` | `91f1acb368d43c676ed3cc543b3d326d21cbd7cf10423b5ad4a6bba96a34177d` |
| `virtual_memory_benchmark` | `b334ab9497fdcf715949bcf889701932dc10ca0263b17732ba8a810d8265c5bc` |
| `virtual_memory_mprotect_benchmark` | `2ba90e5a2355062fc54e0559f32d8c2f7ccf08ccb06283e710cbb3ef37352303` |
| `virtual_memory_file_benchmark` | `e0fd9fc5a5a70ab36263f2222c13ef3e24c3327a23af07a49169b03378f0c493` |

最终 LTO 二进制反汇编确认：

- `run_walk` 符号和运行时 page-indexed load loop 存在；
- `read_with_pread` 保留 `pread` 循环和每 64 byte 的 load；
- `read_with_mmap` 保留映射后每 64 byte 的 load；
- mprotect binary 保留权限切换、reader sweep 和 measurement epoch；
- `observation_sink` 存在；
- 每轮 checksum 与预期值比较，不只依赖 sink。

因此待测工作未被 DCE；sequential/random 使用同一 indexed loop，差别只在
identity permutation 与已验证的 shuffled permutation。

## 4. 计数器能力与限制

目标机通用 core PMU 事件 `cycles`、`instructions`、`dTLB-load-misses` 和
`cache-misses` 可被 `perf_event_open` 打开，但 `time_running=0`，结果为
`<not counted>`。短暂关闭 NMI watchdog 后仍相同，测试结束后 watchdog
恢复为 `1`。

没有伪造 dTLB miss。实际证据：

| 证据 | 用途 |
| --- | --- |
| `CLOCK_MONOTONIC` | 每轮 wall time |
| `getrusage`/perf software event | minor/major fault |
| `/proc/self/status` | `VmSize`、`VmRSS`、`VmPTE` |
| `/proc/self/smaps` | THP/HugeTLB 实际覆盖 |
| `/proc/self/numa_maps` | page placement |
| `/proc/self/io` | backing-device `read_bytes` |
| `tlb:tlb_flush` | 进程内 flush activity |
| `msr/aperf/`、`msr/mperf/`、`msr/tsc/` | 频率状态 |

正式 case 的 `APERF/MPERF` 约 `1.3265`，不同 case 没有系统性频率偏差。

## 5. 页权限

correctness demo 将 anonymous page 设为 `PROT_READ`，子进程执行 store。
程序要求 child 精确收到 `SIGSEGV`，并验证 parent byte 未变。恢复 read-write
后写入必须成功。

目标机：

```text
[2] Page permission enforcement
  read-only child write terminated by signal=11
```

这说明页权限是硬件访问契约，不是普通业务错误返回。JIT、guard page、
sandbox 和 allocator quarantine 必须按 signal/process boundary 设计。

## 6. Reserve、resident、PTE 与 first-touch

```bash
VM_BENCH_EXPECT_NODE=3 VM_BENCH_STRICT_NATIVE=1 \
perf stat -e msr/aperf/,msr/mperf/,msr/tsc/,\
page-faults,minor-faults,major-faults -- \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark fault 1024 7
```

| 指标 | 中位 | 范围 |
| --- | ---: | ---: |
| 1 GiB `mmap` reserve | `4.761 us` | `[2.084, 6.091]` |
| first-touch | `193.459 ms` | `[192.902, 201.350]` |
| second-touch | `3.124 ms` | `[3.097, 3.155]` |
| first-touch minor fault | 262144 | 每轮一致 |
| second-touch minor/major | 0/0 | 每轮一致 |
| first/second ratio | `61.93x` | 中位数比 |

典型稳定轮次：

| 时点 | `VmSize` | `VmRSS` | `VmPTE` |
| --- | ---: | ---: | ---: |
| mapping 前 | 2476 KiB | 约 1.5 MiB | 44 KiB |
| `mmap` 后 | 1051052 KiB | 约 1.5 MiB | 44 KiB |
| first-touch 后 | 1051052 KiB | 1049896 KiB | 2096 KiB |

262144 等于 1 GiB / 4 KiB。reserve 只建立地址范围；first-touch 承担 frame
allocation、zero-fill、PTE 安装与 exception handling。页表自身约增加 2 MiB。

## 7. 访问顺序与 Huge Page

1 GiB mapping 已 first-touch；每轮经同一 indexed loop 读取每个 4 KiB 页
1 byte，共 64 pass。identity order 表示 sequential，完整 permutation 表示
random；程序验证 permutation 无遗漏、无重复且超过一半位置发生变化。

| Mapping | Order | 覆盖证据 | 中位 ns/access | 范围 |
| --- | --- | --- | ---: | ---: |
| base | identity | `AnonHugePages=0` | 10.724 | `[10.630, 10.749]` |
| base | shuffled | `AnonHugePages=0` | 14.604 | `[14.585, 14.698]` |
| THP | shuffled | `AnonHugePages=1046528 KiB` | 14.115 | `[14.109, 14.194]` |
| HugeTLB | shuffled | 1 GiB `Private_Hugetlb` | 14.150 | `[14.128, 14.213]` |

random 比 sequential 慢 `1.36x`。差异同时包含 TLB/page-walk cache、cache
line prefetch 和 index-dependent address generation；core PMU 不可用，因此
不能把全部差异归因于 TLB。

相对 base random，THP 改善 `3.35%`，HugeTLB 改善 `3.11%`。进程初始化期
perf page fault 从 base 的 262662 降至 THP 的 1542、HugeTLB 的 1028。
Huge Page 在该 hot loop 中收益有限，但明显减少 fault 和页表粒度。

## 8. Copy-on-write

父进程先触碰 512 MiB/131072 个 base page。每轮比较 empty child 和逐页写
全部 512 MiB 的 child；`VM_BENCH_STRICT_NATIVE=1` 要求额外 fault 不少于
page count。

| 路径 | 中位 | 范围 | child minor fault |
| --- | ---: | ---: | ---: |
| empty child | `4.561 ms` | `[4.445, 5.339]` | 17 |
| write 512 MiB | `153.961 ms` | `[150.851, 160.878]` | 131089 |
| ratio/delta | `33.76x` | 中位数比 | 131072 |

COW 没有消灭复制，只是把复制和 131072 个 minor fault 推迟到首次写。

## 9. `mmap` 与 `pread`：生命周期会改变答案

### 9.1 原测试究竟测了什么

文件位于 node3 本地 NVMe `/data27`，大小 2 GiB。`pread` 使用 1 MiB user
buffer；旧 `mmap` 路径每轮执行
`mmap -> MADV_SEQUENTIAL -> scan -> munmap`。两个路径读取相同字节并验证
同一 checksum。文件通过临时文件写入、`fdatasync`、close、原子 rename 后
才可见。

cold 每轮先执行 `POSIX_FADV_DONTNEED`，并设置
`VM_BENCH_REQUIRE_IO=1`。程序要求 `/proc/self/io` 的 `read_bytes` 增加；
每轮两个 cold 路径均确认 2147483648 device bytes。warm 每轮为 0。

| Path | State | 中位 GiB/s | 范围 | fault 中位 |
| --- | --- | ---: | ---: | --- |
| `pread` | cold | 3.611 | `[3.562, 3.625]` | 0 major |
| `mmap` | cold | 2.671 | `[2.661, 2.685]` | 8193 major + 32768 minor |
| `pread` | warm | 11.805 | `[9.034, 11.886]` | 0 major |
| `mmap` | warm | 11.386 | `[11.336, 11.425]` | 0 major + 32768 minor |

cold `mmap` 比 cold `pread` 低 `26.03%`。`pread` 的设备等待在 syscall 内，
所以 0 major fault 不等于没有 I/O。cold `mmap` 把 I/O、VMA 查询和 PTE 安装
放入 fault-driven instruction path。

warm 结果中，page cache 已经命中，但 `mmap` 每轮仍有约 32768 个 minor
fault。这不是数据从 NVMe 读取，而是新 mapping 的 PTE 安装。旧 benchmark
回答的是：

> 如果每次操作都重新 map 2 GiB，mmap 是否优于复用 1 MiB buffer 的 pread？

答案在该机器上是否定的。它没有回答实践中更常见的“map once, access many”。

### 9.2 新实验固定三个生命周期

新增
[`mmap_pread_tradeoff_benchmark.c`](../../tools/docs/examples/mmap_pread_tradeoff_benchmark.c)
并区分：

| Mode | Setup | 每轮关键工作 |
| --- | --- | --- |
| `pread-reuse` | scan 预触并复用 1 MiB buffer；lookup 复用 1 byte slot | `pread` 循环 + page-cache-to-buffer copy |
| `mmap-remap` | 无长期 mapping | `mmap + fault/PTE + access + munmap` |
| `mmap-persistent` | map 一次并预热 | 直接访问已有 mapping，PTE 跨轮次保留 |

每轮均检查相同 checksum，并记录：

- setup 和 warmup 时间；
- wall time、minor/major fault、`read_bytes`；
- 逻辑 I/O 操作数；EINTR/short read 导致的实际 syscall 尝试由 perf 统计；
- perf 的 `pread64`、`mmap`、`munmap` tracepoint；
- MSR `APERF/MPERF/TSC` 与 context switch。

各 case 的 `APERF/MPERF` 位于 `1.3256-1.3316`，没有与结论同方向的系统性
频率偏差。LTO 反汇编确认 scan、random lookup 和 checksum loop 均保留。

lifecycle benchmark artifact：

| Artifact | SHA-256 |
| --- | --- |
| source | `bfc6a073f19bba3b0dc102f69c36de3ff50f566dbcf954b15759136ddf862889` |
| native binary | `713950d950f6f854ccb077081a3450a0c59422dd1343dccf93f701ef15282c54` |
| round vectors and perf totals | [`evidence/mmap-pread-tradeoff-20260908.tsv`](evidence/mmap-pread-tradeoff-20260908.tsv) |

### 9.3 顺序扫描：cold 与 steady state 是相反结论

2 GiB cold，每轮都确认 `read_bytes=2147483648`：

| Method | 中位 GiB/s | 范围 | Fault/round |
| --- | ---: | ---: | --- |
| `pread-reuse` | 3.360 | `[3.268, 3.406]` | 0 major |
| `mmap-remap` | 2.726 | `[2.717, 2.736]` | 8193 major + 32768 minor |

cold sequential 中，`mmap-remap` 吞吐低 `18.87%`。设备 I/O 主导时，
fault-driven 控制流没有省掉磁盘读取，反而增加了 fault/VMA/PTE 工作。

2 GiB warm steady state：

| Method | 中位 GiB/s | 范围 | Timed fault/round |
| --- | ---: | ---: | --- |
| `pread-reuse` | 9.582 | `[7.474, 9.585]` | median 0 |
| `mmap-remap` | 8.734 | `[8.674, 8.759]` | 32769 minor |
| `mmap-persistent` | 19.954 | `[19.729, 20.005]` | median 0 |

结论发生反转：

- remap `mmap` 仍比 `pread` 低约 `8.85%`；
- persistent `mmap` 是 `pread` 的 `2.08x`；
- persistent `mmap` 是 remap `mmap` 的 `2.28x`。

相同 page cache、相同 checksum、相同每 64 byte 采样工作下，差异来自两项：

1. persistent mapping 不再每轮重建约 32768 组 PTE/fault-around；
2. `pread` 仍需把 page-cache bytes copy 到用户 buffer。

1 MiB warm 范围也显示同方向：

| Method | 中位 GiB/s | 21 轮范围 |
| --- | ---: | ---: |
| `pread-reuse` | 12.488 | `[8.198, 13.256]` |
| `mmap-remap` | 18.503 | `[14.402, 19.324]` |
| `mmap-persistent` | 48.228 | `[25.363, 50.612]` |

小范围数字受 timer、cache 和 first-round jitter 影响更大，因此只用于说明
setup/amortization，不作为存储带宽结论。

### 9.4 随机 lookup：mmap 的典型优势场景

2 GiB page cache warm，随机访问每个 4 KiB page 的一个 byte，共 524288
probe：

| Method | 中位 ns/probe | 范围 | Timed work |
| --- | ---: | ---: | --- |
| `pread-reuse` | 446.290 | `[424.011, 607.865]` | 524288 `pread`/round |
| `mmap-remap` | 235.351 | `[225.919, 333.883]` | 32769 minor fault/round |
| `mmap-persistent` | 15.385 | `[15.317, 15.629]` | 0 syscall，median 0 fault |

persistent `mmap` 是 `pread` 的 `29.01x`，原因不是 NVMe 更快，而是 hot
lookup 已退化为普通 load；`pread` 每个 probe 仍跨一次 syscall boundary。
perf 总计也吻合：

- `pread-reuse`：3672068 次 `pread64`，等于 warmup 2048 +
  `7 * 524288`，再加进程启动噪声；
- `mmap-persistent`：只有进程启动和单次 mapping 的少量 mmap/munmap；
- persistent timed rounds 的 `read_bytes=0`、major fault=0。

512 MiB cold random，只访问 32768 个离散 page，两个路径都实际读取
134217728 bytes：

| Method | 中位 ns/probe | 范围 | I/O path |
| --- | ---: | ---: | --- |
| `pread-reuse` | 65527.030 | `[65243.305, 66751.391]` | 32768 syscall/round |
| `mmap-remap` | 66311.393 | `[66164.776, 66931.476]` | 32768 major fault/round |

差异只有约 `1.20%`。当每个 lookup 都要等一次 4 KiB device I/O 时，storage
latency 淹没了 syscall 与普通 load 的差别。

### 9.5 Warmup 与 break-even

两个 steady-state 结论都显式记录了从 cold 开始的 warmup：

| Case | `pread` warmup | persistent `mmap` warmup | 每轮节省 |
| --- | ---: | ---: | ---: |
| 2 GiB sequential | 621.888 ms | 697.785 ms | 108.489 ms |
| 2 GiB random lookup | 573.130 ms | 661.368 ms | 225.918 ms |

break-even 近似：

```text
N = (mmap_setup + mmap_warmup - pread_setup - pread_warmup)
    / (pread_round - mmap_round)
```

在这两组数据中，额外 mmap warmup 都在一次 steady-state operation 内摊平：

- sequential `N ~= 0.70`；
- random lookup `N ~= 0.39`。

这不表示所有 mmap 都能“一次回本”。若从 cold 状态直接按随机顺序 fault，
readahead 会失效，startup 可能很差。正式 persistent lookup 先以
`MADV_SEQUENTIAL` 顺序预触页，再切换 `MADV_RANDOM`；这是实践中的主动
初始化策略，不是 mmap 自动提供的免费收益。

### 9.6 实践选择指南

```text
Same logical work?
        |
        v
One-shot cold sequential -------- yes -------> pread first
        |
        no
        v
Long-lived mapping + reuse ------- yes -------> mmap first
        |
        no
        v
Fine-grained random lookup ------- yes -------> mmap or app cache
        |
        no
        v
Need cancellation, QD, isolation -----------> explicit I/O
```

| 条件 | 优先 `mmap` | 优先 `pread` |
| --- | --- | --- |
| 生命周期 | 同一文件长期映射、重复访问 | one-shot、文件快速轮换 |
| 访问粒度 | 小字段、随机 lookup、pointer-native | 大块顺序 streaming |
| cache state | working set 可驻留或可预热 | cold scan 占主导 |
| 控制需求 | 接受内核 readahead/reclaim | 需要 QD、取消、优先级、backpressure |
| 错误模型 | 能处理 `SIGBUS`/truncate contract | 希望 errno/short-read |
| 地址空间 | 64-bit、mapping 数受控 | 32-bit、超大文件、VMA churn 风险 |
| 并发隔离 | 共享 host page cache 是收益 | page-cache 互相污染，需要应用 cache |

实践中选 `mmap` 时：

1. **mapping 要长寿命。** 不要在 query/row/request 热路径反复
   `mmap/munmap`。
2. **预热要匹配未来访问。** 顺序预热后随机读，或用
   `madvise/readahead` 显式表达。
3. **区分 page-cache warm 与 PTE warm。** page 在 RAM 中不代表当前进程
   已有 PTE。
4. **版本化文件生命周期。** 禁止并发 truncate，或把 `SIGBUS` 纳入恢复
   边界。
5. **测量 mapping churn。** 记录 minor fault、VMA 数、TLB flush，而不只看
   device throughput。

实践中选 `pread` 时：

1. 复用 aligned buffer，不把 allocator 成本混入 I/O；
2. 用足够大的 request 让 syscall/copy 可摊销；
3. 显式使用 readahead、batch、异步 I/O 和 backpressure；
4. 若应用已有 buffer pool，避免再依赖隐式 host-wide cache 策略；
5. 对 cold/one-shot workload 单独测，不用 steady-state 数字代替。

### 9.7 复现命令

```bash
.tmp/fil-c/bin/filcc -Itools/docs/examples \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/mmap_pread_tradeoff_benchmark.c \
  -o .tmp/mmap-pread-tradeoff-filc
.tmp/fil-c/bin/filrun .tmp/mmap-pread-tradeoff-filc selftest
```

目标机原生构建：

```bash
gcc -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror -I. \
  mmap_pread_tradeoff_benchmark.c \
  -o mmap_pread_tradeoff_benchmark

gcc -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror -I. \
  virtual_memory_file_benchmark.c \
  -o virtual_memory_file_benchmark

mkdir -p /data27/.tmp
./virtual_memory_file_benchmark \
  prepare /data27/.tmp/mmap-pread-2g.bin 2048
```

正式 CLI 默认校验 cache state：cold scan 的 `read_bytes` 至少覆盖全部文件，
cold lookup 至少覆盖全部 probe page，warm case 则必须为零。FIL-C selftest
不把虚拟机的 `/proc/self/io` 当作设备证据。典型 perf 命令：

```bash
perf stat -e \
msr/aperf/,msr/mperf/,msr/tsc/,task-clock,context-switches,\
page-faults,minor-faults,major-faults,\
syscalls:sys_enter_pread64,syscalls:sys_enter_mmap,\
syscalls:sys_enter_munmap -- \
  numactl --physcpubind=96 --membind=3 \
  ./mmap_pread_tradeoff_benchmark \
  scan mmap-persistent warm \
  /data27/.tmp/mmap-pread-2g.bin 2147483648 7
```

正式测试文件已删除，目标机 NMI watchdog 保持为 `1`。

## 10. TLB flush 与 `mprotect`

64 MiB mapping 上，主线程独占 coordinator CPU 96；reader 只使用剩余
node3 CPUs。每个 reader 在计时前完成 warm sweep，计时期间至少完成一次
sweep；结果记录 `min_sweeps`。

| Reader | transition 中位 | 范围 | `tlb_flush` 中位 |
| ---: | ---: | ---: | ---: |
| 1 | `165.598 us` | `[165.043, 165.831]` | 636 |
| 8 | `435.839 us` | `[433.629, 442.964]` | 2135 |
| 32 | `632.338 us` | `[564.237, 656.074]` | 5268 |
| 63 | `596.467 us` | `[583.110, 695.236]` | 18778 |

63 readers 相对 1 reader 的中位成本约 `3.60x`。32 与 63 并非单调，说明
flush 策略、并行确认和调度存在阈值；不能拟合简单线性公式。tracepoint 数也
不等于 IPI 数，但 wall time 与 flush activity 都证明页表修改不是本地变量写。

## 11. NUMA locality

2 GiB mapping 全部由程序验证位于 node3，单线程顺序读取全部 `uint64_t`：

| CPU | CPU node | Memory node | Distance | 中位 GiB/s | 范围 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 96 | 3 | 3 | 10 | 22.925 | `[22.855, 22.958]` |
| 64 | 2 | 3 | 12 | 20.275 | `[19.435, 20.709]` |
| 1 | 0 | 3 | 32 | 13.056 | `[12.747, 13.130]` |

相对 local，同 socket neighbor 下降 `11.56%`，跨 socket 下降 `43.05%`；
local 是 cross-socket remote 的 `1.76x`。三组 `APERF/MPERF` 约相同，
差异不是频率或 page migration 造成。

## 12. 未在生产节点执行的破坏性实验

| 未执行项 | 原因 | 合适环境 |
| --- | --- | --- |
| swap-in/out | 机器无 swap | 隔离 VM |
| direct reclaim/thrashing | 会影响同机业务 | 独立 cgroup/host |
| OOM/overcommit failure | 可能杀死无关进程 | 隔离 namespace/VM |
| kernel/MGLRU A/B | 目标 kernel 不可切换 | 两套受控 kernel |
| persistence crash test | 可见性不等于 durability | 专用文件系统 host |
| precise dTLB miss | core PMU `time_running=0` | 修复 PMU 或专用 host |

## 13. 复现命令

所有正式 memory commands 设定 `VM_BENCH_EXPECT_NODE=3`：

```bash
VM_BENCH_EXPECT_NODE=3 VM_BENCH_STRICT_NATIVE=1 \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark fault 1024 7

VM_BENCH_EXPECT_NODE=3 \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark walk random base 1024 64 7

VM_BENCH_EXPECT_NODE=3 VM_BENCH_STRICT_NATIVE=1 \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_benchmark cow 512 7

VM_BENCH_EXPECT_NODE=3 \
  numactl --physcpubind=96-127,224-255 --membind=3 \
  ./virtual_memory_mprotect_benchmark mprotect 64 63 100
```

文件 cold command 设定 `VM_BENCH_REQUIRE_IO=1`：

```bash
./virtual_memory_file_benchmark \
  prepare /data27/.tmp/virtual-memory.bin 2048

VM_BENCH_REQUIRE_IO=1 \
  numactl --physcpubind=96 --membind=3 \
  ./virtual_memory_file_benchmark \
  read mmap cold /data27/.tmp/virtual-memory.bin 7
```

实际正式命令由可用的 MSR/software/tracepoint perf events 包裹。2 GiB 测试
文件在完成后删除，NMI watchdog 保持为 `1`。
