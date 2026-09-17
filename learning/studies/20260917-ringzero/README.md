---
doc_id: recallfs-study-ringzero-v2
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-agent-ready-docs-v1
  - recallfs-source-ringzero-study-v1
supersedes:
  - recallfs-study-ringzero-v1
verified_by:
  - RZ-RA-1
  - RZ-RA-2
  - RZ-RA-3
  - RZ-RA-4
  - RZ-RA-5
  - RZ-RA-6
  - RZ-RA-7
---

# RingZero、XDP 与 Maglev 深度学习

> 上游：<https://github.com/immanuwell/ringzero>
>
> 审计 revision：`9a56125f02fcbce55448482d442d99729573ab06`
>
> 本文严格区分源码事实、本机观察、论文作者报告和待验证推论。

## Contract

### Decision

RingZero 应被当作一个紧凑的 XDP L4 forwarding 教材，而不是可部署的负载
均衡器。它最值得迁移的机制是：控制面把可变 backend set 编译成固定大小的
Maglev table，数据面只做有界 parse、map lookup、rewrite 和 redirect。
RingZero 不是完整 kernel bypass；它把 fast path 前移到 kernel 内的 XDP
driver hook。对于 zero-drop 场景，XDP 仍可能显著减少 CPU 和增加容量余量，
但 `XDP_REDIRECT` 不提供可靠交付、buffering 或 backpressure，因而不能单独
证明或保证零丢包。

### Scope

- RingZero 的 XDP data plane、Zig control plane、pinned BPF maps、health
  check、Maglev builder、验证脚本和 load generator；
- kernel sockets、XDP、AF_XDP 与 DPDK 的边界和成本；
- zero-drop 的语义、收益条件和目标机验证方法；
- Maglev、Modulo、Ring、Rendezvous、Jump 的机制与实测对比；
- 从 demo 到 production L4 load balancer 之间缺失的 correctness、
  consistency、failure 和 observability contract。

### Non-goals

- 不把 d2 的单队列 veth/virtio 结果伪装成物理 NIC 或 line-rate 结果；
- 不因上游 README 的性能描述宣称已达到 line rate；
- 不把 selector microbenchmark 外推为完整 packet path 性能；
- 不修改或复制未授予 license 的 RingZero 源码；
- 不实现 production load balancer、BGP、IPIP/GUE、conntrack 或 control
  plane；
- 不比较所有 consistent hashing 变体或 weighted/bounded-load 扩展。

### Inputs And Outputs

| Boundary | Input | Output |
| --- | --- | --- |
| RingZero data plane | Ethernet frame、VIP/backend/Maglev maps | `XDP_PASS`、`XDP_DROP`、`XDP_TX` 或 redirect |
| RingZero control plane | CLI config、TCP health result | pinned map mutation、Maglev rebuild |
| Hash benchmark | stable `u64` keys、stable `u32` backend IDs | balance、churn、lookup/build time、main memory |
| Zero-drop qualification | sequenced packets、declared load/burst profile | independent receive ledger、drop attribution、CPU/latency/headroom |

### Interfaces And Ownership

- NIC driver owns RX descriptors until XDP receives an `xdp_buff`;
- RingZero's eBPF program borrows packet bytes for one invocation and returns an
  XDP action;
- pinned BPF maps own runtime configuration after the short-lived CLI exits;
- the Zig control plane owns health transitions and table reconstruction;
- a production publisher must own one immutable config generation and expose it
  atomically to packet processing;
- sender and receiver, not the load balancer's own counters, own the independent
  zero-drop oracle.

### Invariants

- a valid unfragmented 5-tuple maps deterministically only when every LB uses
  the same hash, stable backend identities, canonical member ordering, table
  size, and complete generation;
- a Maglev table with $M$ slots and $N$ backends gives each backend either
  $\lfloor M/N \rfloor$ or $\lceil M/N \rceil$ slots;
- table lookup is $O(1)$, but membership mutation is not free or atomic by
  implication;
- verifier acceptance establishes constrained memory/control-flow safety, not
  forwarding semantics, losslessness, fairness, or availability;
- zero observed loss is meaningful only with a defined ingress population and
  an independent end-to-end receive ledger;
- performance claims identify XDP mode, kernel, driver, NIC, queues, NUMA,
  packet size, flow cardinality, offered load, duration and revision.

### Failure Semantics

- RingZero returns `XDP_DROP` when a VIP has no table entry or its selected
  backend is missing/unhealthy; zero-drop is therefore not an unconditional
  property of this implementation;
- non-VIP and unsupported traffic generally returns `XDP_PASS`, transferring
  work to the normal stack;
- native attach may fall back to generic XDP in `auto` mode; successful attach
  is not proof of the performance path;
- individual BPF map updates are atomic, but a 4099-slot rebuild is not one
  atomic configuration transaction;
- AF_XDP/DPDK queue or buffer exhaustion can drop even though packet copies and
  system calls were removed;
- absent end-to-end retransmission, an L4 forwarder cannot repair a frame lost
  at NIC, RX ring, redirect, egress queue, fabric or backend.

### Worked Examples

**RZ-WE-1: steady-state VIP packet.**

An unfragmented IPv4/UDP packet targets a configured VIP. The XDP program finds
the VIP, hashes the 5-tuple, reads one Maglev slot, resolves a healthy backend,
patches destination IP and checksums, rewrites Ethernet addresses, updates
per-CPU stats and redirects. It avoids `sk_buff`, IP routing, netfilter and
socket delivery on that box, but still runs in kernel driver context.

**RZ-WE-2: healthy backend removed during a long-lived flow.**

The control plane clears the backend's healthy bit and then rewrites 4099 slots
one at a time. Before a slot is replaced, packets selecting the now-unhealthy
backend are dropped. After replacement, the same 5-tuple may select another
backend. Because RingZero has no connection table, the existing TCP flow is not
protected from movement. The original Maglev system used connection tracking
as the primary affinity mechanism; the lookup table was the fallback.

**RZ-WE-3: all traffic executes `XDP_PASS`.**

If a program only parses and passes every packet to the same kernel stack, it
adds eBPF work and bypasses nothing downstream. XDP's benefit appears when it
takes an early terminal action such as redirect/TX/drop, or supplies metadata
that removes more expensive later work.

**RZ-WE-4: zero observed drops at current load.**

A service already receives every packet at 2 Mpps. Native XDP may still be
valuable if it lowers cores/packet, p99 latency or power and raises overload
headroom. It has no demonstrated value if the bottleneck is backend service,
egress capacity or a large-packet bandwidth ceiling and those metrics do not
move.

**RZ-WE-5: non-initial IPv4 fragment.**

The source does not inspect `frag_off`. It can interpret fragment payload bytes
as a TCP/UDP header if enough bytes remain. A production policy must explicitly
drop, pass, reassemble elsewhere, or implement fragment affinity; accidental
parsing is not a valid policy.

### Reconciliation Anchors

| Anchor | Input or condition | Exact expected result | Verification |
| --- | --- | --- | --- |
| `RZ-RA-1` | NSDI'16 sample: 3 backends, 7 slots | `[1,0,1,0,2,2,0]`; after removing 1: `[0,0,0,0,2,2,2]` | CTest in `benchmark/` |
| `RZ-RA-2` | 32 backends, 4099 Maglev slots | backend slot counts differ by exactly 1 | CTest |
| `RZ-RA-3` | four IDs in forward/reverse order | canonical tables equal; raw ordered tables differ in exactly 8 slots | CTest |
| `RZ-RA-4` | 1M keys, 32 to 33 backends | Modulo add churn >95%; Maglev 2%-8%; Jump middle-remove 45%-55% | d2 `compare` |
| `RZ-RA-5` | upstream `zig test src/maglev.zig` | exit 1 at the invalid `slot < 4` assertion | `evidence/raw/d2-ringzero-build/upstream-maglev-test.txt` |
| `RZ-RA-6` | C safety/native matrix on d2 | FIL-C paths pass; Zig and GCC CTest both 7/7 pass | `benchmark/scripts/run_on_d2.sh` |
| `RZ-RA-7` | d2 veth/native-XDP, 10k paced UDP | 10k unique, zero missing/duplicate/invalid, XDP packets=10k, cleanup pass | `benchmark/scripts/run_on_d2.sh d2 <ringzero-root>` |

### Evidence And Unknowns

Observed evidence is indexed in [`evidence/README.md`](evidence/README.md).
`RZ-RA-1` through `RZ-RA-7` pass as documented. d2 proves Linux build,
verifier acceptance, native XDP on veth and zero observed loss for one bounded
10k-packet workload. Unknowns still include physical-NIC throughput, RSS queue
scaling, sustained high-load zero-loss envelope, p99/p999 latency, backend
return path, control-plane race behavior and upgrade recovery.

## 1. 结论先行

### 1.1 RingZero 真正展示了什么

RingZero 展示的是一个很好的 hot-path transformation：

```text
mutable backend set
        |
        v
control-plane Maglev build
        |
        v
flat immutable-looking table
        |
        v
one indexed lookup per packet
```

这个思想比项目本身更可复用：把昂贵、可变、带分支的决策移到低频控制面，
将运行时编译成紧凑、固定成本的数据结构。

但当前 map 不是一个真正 immutable generation：它被逐槽原地修改。机制方向
是对的，publication protocol 尚未完成。

### 1.2 三个问题的短答案

1. **绕过 kernel 的代价是什么？**
   代价不是某一次 syscall，而是把 kernel 原本拥有的语义、资源管理、隔离、
   工具和故障恢复责任转移给应用。越接近 DPDK，这笔责任转移越完整。
   RingZero/XDP 保留 kernel、driver、NAPI、eBPF verifier 和 BPF maps，因此
   代价明显小于全量 kernel bypass。
2. **eBPF 在 zero-drop 场景还有收益吗？**
   有，但收益应看 CPU/packet、tail latency、功耗和 overload headroom，不只
   看 drop counter。若所有包最终 `XDP_PASS`，通常只有额外开销；若 XDP 完成
   redirect/TX，仍跳过大量 stack work。XDP 不能提供 zero-drop guarantee。
3. **Maglev 相对其他 hash 的优缺点？**
   它用固定表换取极快、cache-friendly 的 $O(1)$ lookup 和近乎完美的 slot
   balance；代价是每 VIP 内存、重建与发布成本、略高于最小值的 churn、对稳定
   identity/order 的依赖，以及对 weighted capacity、heavy hitter 和 active
   connection preservation 不提供天然答案。

## 2. RingZero 数据面

### 2.1 Packet path

```text
+--------+    +------------+    +---------+    +--------------+
| NIC RX |--->| XDP parser |--->| VIP map |--->| 5-tuple hash |
+--------+    +------------+    +---------+    +------+-------+
                                                       |
                                                       v
+-----------------+    +------------------+    +-------------+    +--------------+
| egress redirect |<---| L2/L3/L4 rewrite |<---| backend map |<---| Maglev table |
+-----------------+    +------------------+    +-------------+    +--------------+
```

Revision-pinned path:
[`bpf/xdp_lb.c:129-234`](https://github.com/immanuwell/ringzero/blob/9a56125f02fcbce55448482d442d99729573ab06/bpf/xdp_lb.c#L129-L234).

成功的 exact-VIP path 至少执行：

1. `vip_map` lookup；
2. `maglev_map` lookup；
3. `backend_map` lookup；
4. per-VIP `stats_map` lookup；
5. global `stats_map` lookup。

因此 README 的“四次 map lookup”不是完整成功路径的精确计数。wildcard VIP
miss 后还会多一次 `vip_map` lookup。

### 2.2 Control plane

Zig CLI 负责：

- 加载与 attach eBPF object；
- pin `vip_map`、`backend_map`、`maglev_map`、`stats_map`；
- 添加 VIP/backend；
- TCP health check；
- 根据 healthy backend 重建每个 VIP 的 4099-slot table；
- 汇总 per-CPU stats。

进程退出后 map 与 XDP program 继续存在。这是清晰的 control/data plane
separation，但 pinned state 没有 schema version、migration 或 transactional
publication contract。

### 2.3 它不等于 Katran

RingZero 借用了 Katran 的结构，但不能把“相同方向”读成“相同语义”：

| Capability | RingZero revision | Katran documented model |
| --- | --- | --- |
| Fast path | XDP | XDP |
| Backend selection | Maglev table | modified weighted Maglev |
| Active-flow affinity | none | fixed-size LRU connection tracking |
| Forwarding | destination IP/MAC rewrite | IPIP/GUE-style encapsulation options |
| Scale-out ingress | none | BGP/ECMP integration |
| Return path | not defined | DSR topology contract |
| Health/config | basic TCP loop | production control-plane integration |

只改 destination IP 并不能独立定义 DSR。backend 若以 real IP 回复，client
看到的源地址会与 VIP 连接不符；若环境要求 backend 以 VIP 回复，就必须把
地址、routing、ARP 和 reverse-path policy 写进部署合同并做双向 E2E。

## 3. “Kernel Bypass” 应改问边界在哪里

### 3.1 四种边界

| Dimension | Kernel socket | RingZero native XDP | AF_XDP zero-copy | DPDK |
| --- | --- | --- | --- | --- |
| Data-plane execution | kernel network/transport stack + process | kernel driver hook | XDP steering + userspace packet app | userspace PMD |
| Packet memory | kernel `skb`/socket buffers | driver `xdp_buff` | registered UMEM ownership rings | userspace mempool/mbuf |
| NIC ownership | kernel | kernel | kernel | commonly userspace/VFIO |
| Transport semantics | kernel TCP/UDP | none in fast path | application | application/userspace stack |
| Idle behavior | interrupt/NAPI | interrupt/NAPI | configurable poll/wakeup | usually busy poll |
| Kernel tools | full | many retained, socket-path tools bypassed | partial | substantially separate |
| Isolation | normal kernel boundary | verifier + capabilities | kernel boundary plus shared-memory contract | IOMMU/VFIO/process design |

RingZero bypasses the **normal network stack**, not the kernel. XDP's design
motivation正是保留 kernel hardware ownership、security boundary 和管理接口，
同时在 `sk_buff` 分配前执行 bounded fast path。

### 3.2 全量 bypass 的成本账

| Cost | What moved out of the kernel |
| --- | --- |
| Protocol semantics | TCP retransmit/congestion/order、fragmentation、PMTU、ICMP、neighbor discovery |
| Policy | routing、netfilter、conntrack、qdisc/QoS、namespace/cgroup integration |
| Resource ownership | DMA buffers、hugepages、IOMMU mappings、queue lifecycle、NUMA locality |
| CPU/power | dedicated polling cores can stay at 100% when idle |
| Isolation/security | direct device access and a larger trusted userspace data plane |
| Operations | `ss`/iptables/tcpdump-style assumptions、metrics、debugging and upgrade tooling |
| Failure recovery | application crash can strand/blackhole queues; restart and drain become application semantics |
| Compatibility | NIC/driver/offload/virtualization matrix and deployment-specific tuning |

这笔成本不是“性能优化附带复杂一点”，而是 ownership transfer。需要先列出
哪些 kernel semantics 被删除，再证明 application 已重新提供必要子集。

### 3.3 XDP 的剩余成本

XDP 避免了大部分 ownership transfer，但仍有明确约束：

- program 必须满足 verifier 的 bounded execution 与 pointer checks；
- raw frame 上没有 socket、完整 route/conntrack 语义或自动 fragment policy；
- map/helper/driver feature 与 kernel version 构成兼容矩阵；
- kernel-space debug 依赖 tracepoint、bpftool、perf 等工具；
- native XDP 依赖 driver，generic XDP 已走到 `skb` 层，性能语义不同；
- redirect 没有通用 backpressure/QoS 保证；
- fast path 错误可在 line rate 放大成全局黑洞。

## 4. eBPF 在 Zero-Drop 场景是否有收益

### 4.1 先定义 zero-drop

“zero-drop”至少有三种不同含义：

1. **当前测试窗口零观察丢包**：有限样本事实；
2. **声明负载包络内零丢包 SLO**：packet size、flows、burst、duration、
   failover 都有边界；
3. **任意 overload 下不丢包**：没有无限 buffer 或端到端 backpressure 时
   不可能。

如果 $N$ 个独立、平稳 Bernoulli packet 全部成功，经验性的 95% upper bound
约为 $3/N$。它不覆盖相关 burst、queue stall、config race 或整机故障，所以
“跑了很多包没丢”仍不是可靠交付证明。

### 4.2 收益取决于 XDP action

| Path | Zero-drop 场景中的收益判断 |
| --- | --- |
| parse then `XDP_PASS` | 通常无 fast-path 收益，只增加 parse/JIT/helper 成本 |
| `XDP_REDIRECT`/`XDP_TX` forwarding | 仍可跳过 `skb`、routing、netfilter、socket；收益可能很大 |
| AF_XDP redirect | 复杂逻辑进 userspace 时可减少 copy/syscall；增加 UMEM/ring ownership |
| `XDP_DROP` attack traffic | 收益最大，但这不是“业务包 zero-drop”路径 |
| hardware offload | 可进一步省 host CPU，但功能、可观测性和 NIC portability 更受限 |

即使 baseline 已经 zero observed loss，XDP 仍可能带来：

- 更低 CPU/packet；
- 更少 cross-core/cache movement；
- 更高可承受 burst；
- 更低 p99/p999；
- 同等负载下更多业务 CPU；
- 不需要 DPDK 常驻 busy-poll core 的更好 idle efficiency。

若这些指标都不改善，或 bottleneck 在 backend/egress/fabric，eBPF 没有因为
“技术先进”而自动产生收益。

### 4.3 XDP 不是 lossless transport

`XDP_REDIRECT` 只表示当前程序选择 redirect。后续仍可能在下列位置失败：

```text
generator -> ingress wire -> NIC RX -> RX descriptors -> XDP
          -> redirect enqueue -> egress TX descriptors -> wire
          -> fabric -> backend NIC -> backend application
```

XDP 没有为这些层提供端到端 ACK、retransmission 或无限 backpressure。
CoNEXT'18 也明确把 QoS/rate transition 与 destination exhaustion 列为 XDP
缺口。真正要求可靠交付时，应由 TCP、QUIC、应用 ACK/retry 或上层复制协议
承担；XDP 负责提高可持续处理包络，而不是替代可靠性协议。

### 4.4 Zero-drop 验证门禁

目标验证至少使用三台独立角色或等价隔离：

```text
+-----------+       +----------------+       +------------+
| generator |------>| native-XDP DUT |------>| receiver   |
| seq IDs   |       | RingZero       |       | bitmap     |
+-----------+       +----------------+       +------------+
```

必须同时记录：

- generator attempted、successful submit 和 wire TX；
- DUT ingress packet、missed/no-buffer、per-queue RX；
- XDP action counts、`xdp_exception`、redirect success/error；
- DUT egress TX/drop/no-buffer；
- receiver unique sequence IDs、missing、duplicate、reordered；
- CPU topology/affinity、IRQ/RSS、NUMA、frequency policy；
- packet size、flow count、burst distribution、offered pps、duration；
- kernel、driver、firmware、NIC、BPF object/source digest 与 attach mode。

测试矩阵应覆盖：

- 64/128/512/1500-byte packets；
- single flow、many flows、heavy hitter；
- steady 70%/90% sustainable rate、line-rate attempt、microburst；
- 1/N RX queues and CPU scaling；
- backend add/down/up during long-lived flows；
- control-plane concurrent update、crash/restart；
- 至少一次长稳 soak。

只有 receiver ledger 与每层 counter reconciliation 同时闭合，才能说
“在该包络内观察到 zero drop”。RingZero 当前的 `floodgen` 忽略
`ENOBUFS`，backend sink 不含 sequence oracle，不能完成这项证明。

### 4.5 d2 实践结果

d2 上的验证先建立了 Linux execution path：

- 固定 Zig 0.16.0 编译 BPF object；
- kernel verifier 接受 `xdp_lb_prog`，生成 1694-byte JIT image；
- veth 上明确以 `XDP_FLAGS_DRV_MODE` attach，而不是 generic fallback；
- C sequence oracle 以单 backend 发送并接收 10,000 个 UDP packet；
- receiver 报告 10,000 unique、0 missing、0 duplicate、0 invalid；
- RingZero 报告 10,000 packets、0 dropped，清理探针通过。

这个请求 50 us inter-packet pacing 的受控实验验证了 rewrite、checksum、
redirect 和接收账本能闭合。sender 没有记录 elapsed time，所以不报告实际
pps。它没有验证高压包络。上游高压脚本的 sender 报告 2,407,680 packets
（约 1.204 Mpps），XDP 只计到 1,909,482，两个 polling sink 合计只报告
12,266。由于 sender 没有 sequence oracle，也没有逐层 drop counter，这个
结果不能解释为 RingZero 的精确 loss rate；它直接证明该脚本不能支持
zero-drop 结论。

d2 的 `eth0` 是单队列 `virtio_net`，hardware PMU 也不可用。因此当前证据
只覆盖 native veth correctness，不覆盖物理 NIC、RSS、多队列、NIC DMA、
cache/TLB 或 line rate。

## 5. Maglev Hashing

### 5.1 算法

对 prime table size $M$ 和 backend $i$：

$$
\mathrm{offset}_i = h_1(i) \bmod M
$$

$$
\mathrm{skip}_i = h_2(i) \bmod (M-1) + 1
$$

$$
\mathrm{permutation}_i[j]
= (\mathrm{offset}_i + j \cdot \mathrm{skip}_i) \bmod M
$$

每个 backend 轮流选择自己 permutation 中第一个空 slot，直到 table 填满。
运行时只做：

$$
\mathrm{backend}
= \mathrm{table}[h(\mathrm{flow}) \bmod M]
$$

轮流填充使每个 backend 获得
$\lfloor M/N \rfloor$ 或 $\lceil M/N \rceil$ slots。论文建议 $M>100N$；
RingZero 使用 $M=4099$、每 VIP 最多 32 backends，即 $M/N>128$。

### 5.2 它为什么适合 packet fast path

- lookup 是一次 modulo 加一次连续 array access；
- runtime 不扫描 backend，不做 binary search；
- table 可由低频 control plane 重建；
- slot 数几乎精确均衡；
- same membership、identity、order、hash 和 $M$ 可生成同一 table；
- BPF array map 天然适合 verifier 可接受的有界 lookup。

RingZero 每 VIP table payload 为：

$$
4099 \times 4 = 16396\ \mathrm{bytes}
$$

64 个 VIP 共 `1,049,344` bytes，不含 BPF map metadata。这个空间换取了
per-packet lookup 的固定成本。

### 5.3 代价与误区

1. **Maglev 不是 minimum-disruption guarantee。**
   原论文明确选择 balance 优先，容忍少量额外 table disruption，并依赖
   connection tracking 保护存量连接。
2. **表大小是三方 trade-off。**
   更大 $M$ 改善量化、balance 和 disruption，但增加每 VIP 内存、cache
   footprint、build 和 publish 成本。
3. **membership 必须 canonical。**
   backend name/ID、排序、hash seed、table size 任一不同，LB 可能得出不同
   结果。
4. **publish 必须 atomic。**
   正确构建一张表不代表逐槽覆盖 live table 时 packet 看到一致 generation。
5. **均匀 slot 不等于均匀 bytes/CPU。**
   flow hash 均匀时连接数近似均匀；elephant flow、请求成本差异和 backend
   capacity heterogeneity 仍会偏斜。
6. **weighted Maglev 不是基础算法免费获得。**
   原论文只说明可改变 backend turn frequency，未给出完整实现；需要独立
   weight、churn 和公平性合同。
7. **top-K/replication 不天然。**
   Rendezvous 可直接取 score 前 K；基础 Maglev 只给一个 backend。

### 5.4 d2 C 对比

固定 32 backends、1M stable keys；下表使用 Zig 0.16.0/Clang 21.1.0
ThinLTO 的五次轮换顺序 median-of-medians：

| Algorithm | Max/avg | Add churn | Remove-middle | Lookup ns | Build us | Main memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Modulo | 1.011 | 96.95% | 96.88% | 3.298 | 0.514 | 128 B |
| Ring, 256 vnodes/backend | 1.096 | 2.87% | 3.28% | 98.283 | 1138.465 | 128 KiB |
| Rendezvous | 1.010 | 3.01% | 3.11% | 117.616 | 0.536 | 128 B |
| Jump | 1.010 | 3.00% | 49.96% | 37.015 | 0.534 | 128 B |
| Maglev, 4099 slots | 1.007 | 4.76% | 5.20% | 3.294 | 66.704 | 16,396 B |

这些数字说明选择逻辑：

- **Modulo**：静态 membership 才合适；变化时几乎全量 remap。
- **Ring**：任意增删 churn 好，可用 vnodes/内存换 balance，lookup 是 binary
  search。
- **Rendezvous**：membership 语义简单、天然 top-K/weight，但基础 lookup
  扫描 $N$。
- **Jump**：dense append-only bucket 很强；任意中间删除需要额外稳定
  indirection，否则 renumbering 破坏一致性。
- **Maglev**：packet-rate lookup 最有优势，balance 最稳定；付出固定 table、
  rebuild/publish 和较高 churn。

GCC 8.3/LTO 独立构建产生完全相同的 balance、churn 和 checksum；其 Maglev
lookup/build medians 为 `3.290 ns`/`62.485 us`。Zig/GCC 的 4x lookup
workload wall time 分别是 1x 的 3.89/3.94 倍，两个 final binary 的反汇编和
checksum 也保留在 evidence 中。d2 KVM
不支持 hardware PMU，所以 selector 时间仍不能外推到 BPF packet path。
`lookup_ns` 还包含共同的 selector dispatch、循环和 checksum oracle；可迁移
的结论是复杂度、layout 和两套编译器一致的相对关系，不是某个纳秒数。

## 6. RingZero Code Audit

| Finding | Evidence type | Consequence |
| --- | --- | --- |
| No connection tracking | direct source observation | membership change can move active flow immediately |
| 4099 individual live updates | direct source observation | packets can see mixed old/new generation |
| BPF hash-map iteration is not sorted | direct source observation | table depends on unspecified iteration order |
| Backend hash identity is local numeric ID | direct source observation | independently allocated IDs can diverge across LB nodes |
| `frag_off` is not checked | direct source observation | non-initial fragments can be parsed as L4 headers |
| Destination IP is rewritten; return path absent | direct source observation | complete DSR/client 5-tuple semantics are unproven |
| UDP health always true | direct source observation | generic UDP failure detection is absent |
| Native attach silently falls back in `auto` | direct source observation | attach success does not prove native performance |
| Upstream Maglev test fails | local observation | current unit gate is red and does not test determinism |
| Flood generator lacks receive oracle | direct source observation | cannot establish loss, duplicates or reorder |
| No CO-RE portability | upstream declaration | rebuild required against local kernel BTF |
| No license | source-tree observation | reuse/distribution rights are not granted |

### 6.1 Correct publication design

一个 production 方向至少需要：

```text
backend config revision
        |
        v
canonical stable identities
        |
        v
build inactive generation
        |
        v
validate full table + digest
        |
        v
atomic active-generation switch
        |
        v
drain old flows / retire old generation
```

可用 BPF map-in-map 或双 generation 加单一 active index。无论选择哪种，
control plane 必须有 single-writer/revision compare-and-swap，防止 health
loop 与人工命令交错发布。active-flow 语义还需要 conntrack、backend draining
或明确接受 reset；atomic table swap 只解决 torn generation，不解决 flow
movement。

### 6.2 Fragment 和 malformed policy

production parser 应先定义而后实现：

- Ethernet VLAN/QinQ；
- IPv4 `ihl`、total length、fragment offset/MF；
- IPv6 与 extension headers；
- TCP/UDP header length；
- checksum/offload metadata；
- unsupported packet 是 drop、pass 还是 slow-path redirect。

“verifier 允许加载”只说明 pointer access 受约束，不说明这些 protocol choices
正确。

### 6.3 Build 与运行兼容性

d2 实践暴露出 source review 看不到的部署约束：

- Zig 0.16.0 需要 `-target bpfel-freestanding`；上游 `-target bpf` 不能直接
  生成该环境可用的 BPF object；
- Debian 10 的 libbpf development metadata 是 4.19，而运行库存在
  `libbpf.so.1.0.1`；构建必须显式固定 v1.0.1 headers 与 library path；
- 旧 `iproute2` 会对 Zig 生成的零大小 BTF `DATASEC` 发出警告，即使
  `bpftool` verifier/load 和 native veth attach 成功；
- 上游 Python probe 的 `bytes.hex(":")` 不兼容 Python 3.7；
- `zig build test` 返回 0 不代表独立 `zig test src/maglev.zig` 通过，后者
  实际在错误的 `slot < 4` 断言处失败。

production build 必须把 Zig、libbpf headers/runtime、kernel BTF、iproute2/
bpftool 和 probe runtime 作为一个版本矩阵管理，不能把“在开发机编译成功”
等同于目标机可装载。

## 7. 采用决策

### 7.1 选择 XDP

适合：

- L2-L4 bounded parsing；
- early drop/redirect/TX；
- 每包逻辑可以用小而稳定的 maps 表达；
- 希望与 kernel NIC ownership、interrupt/NAPI 和普通服务共存；
- 需要在 DPDK 之前获得大部分 stack-bypass 收益。

不适合直接承担：

- 任意复杂 transport；
- 大量动态 allocation；
- 需要 blocking/backpressure 的处理；
- 依赖完整 socket/cgroup/application identity 的 late-stage policy；
- 未定义 fragment、MTU、QoS 和 failure semantics 的“先加速再说”。

### 7.2 选择 Maglev

适合：

- 每秒 lookup 数远大于 membership change；
- backend IDs 稳定且全节点配置可 canonicalize；
- table memory 可接受；
- balance 比绝对最小 churn 更重要；
- 有 atomic publication 和 active-flow protection。

优先考虑其他算法：

- membership 静态：Modulo 最简单；
- arbitrary membership、低 churn、lookup 不极热：Ring；
- backend 数较少、需要 weighted/top-K：Rendezvous；
- dense append-only bucket、极低 state：Jump；
- 强 bounded-load 或 frequent-failure 语义：需要评估专门算法，不能仅凭
  “consistent hashing” 名称选择 Maglev。

## 8. 下一步

RingZero 的下一步不是增加更多 demo feature，而是先关闭语义缺口：

1. 定义完整 packet/fragment/return-path contract；
2. stable endpoint identity + canonical membership；
3. immutable generation build + atomic publish；
4. active-flow conntrack/drain semantics；
5. receive-side independent loss oracle；
6. physical-NIC native-XDP、RSS/NUMA 与 offered-load qualification；
7. fault injection、upgrade/schema migration 和 long soak；
8. 获得明确开源 license 后再讨论源码复用。

在完成这些之前，RingZero 是有价值的学习实现，但不是 production-ready
组件。
