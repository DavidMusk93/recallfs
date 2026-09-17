---
doc_id: recallfs-runbook-ringzero-hashing-benchmark-v1
kind: runbook
status: active
authority: implementation
applies_to:
  - learning/studies/20260917-ringzero/benchmark
depends_on:
  - recallfs-study-ringzero-v1
supersedes: []
verified_by:
  - RZ-RA-1
  - RZ-RA-2
  - RZ-RA-3
---

# Consistent Hashing Benchmark

该 benchmark 比较 Modulo、Ring、Rendezvous、Jump 和 Maglev 在同一组
`u64` key 与 `u32` backend ID 上的行为。它回答四个问题：

1. 稳态分布是否均匀；
2. 尾部新增一个 backend 时多少 key 改变；
3. 删除中间 backend 时多少 key 改变；
4. selector-only lookup、构建时间和主要数据结构的内存成本。

它不测 XDP packet rate，也不代表 NIC、BPF map 或完整 L4 load balancer 的
性能。

## Run

```bash
cargo test --manifest-path learning/studies/20260917-ringzero/benchmark/Cargo.toml
cargo run --release \
  --manifest-path learning/studies/20260917-ringzero/benchmark/Cargo.toml \
  --bin compare -- \
  --keys 1000000 \
  --lookup-keys 500000 \
  --rounds 7 \
  --backends 32 \
  --table-size 4099 \
  --vnodes 256
```

输出为 CSV。`lookup_ns` 是预先 hash 后的 selector 时间，因此不包含共同的
key hash 成本；`checksum` 是防止 dead-code elimination 的可观察 sink。
构建与 lookup 报告中位数。

## Semantics

| Algorithm | Membership model | Lookup | Main state |
| --- | --- | --- | --- |
| Modulo | dense ordered list | $O(1)$ | backend list |
| Ring | arbitrary stable IDs | $O(\log(VN))$ | sorted virtual nodes |
| Rendezvous | arbitrary stable IDs | $O(N)$ | backend list |
| Jump | dense append-only bucket numbering | expected $O(\log N)$ | backend list |
| Maglev | arbitrary stable IDs, canonical member order | $O(1)$ | fixed lookup table |

`Jump` 对尾部增加 bucket 有良好一致性；把任意 backend 的有序列表压紧后删除
中间项，会改变后续 bucket index 的含义。因此 benchmark 同时报告 add 与
remove-middle，避免只展示有利场景。

`Maglev::new` 会排序并去重 backend ID。测试还保留
`build_maglev_table_in_order`，用于证明未经 canonicalization 的输入顺序会
改变大量 table slot。

## Reconciliation Anchors

| Anchor | Input | Exact expected result | Command |
| --- | --- | --- | --- |
| `RZ-RA-1` | NSDI'16 的 3 backend、7 slot 样例 | table 为 `[1,0,1,0,2,2,0]`；移除 backend 1 后为 `[0,0,0,0,2,2,2]` | `cargo test` |
| `RZ-RA-2` | 32 backends、4099 slots | 每个 backend 的 slot 数只相差 1 | `cargo test` |
| `RZ-RA-3` | `[1,2,3,4]` 与逆序输入、4099 slots | canonical builder 相同；raw ordered builder 恰有 8 个 slot 不同 | `cargo test` |

## Limits

- 本地时间只约束当前 Apple M5 Pro、Rust 1.97.0 和当前 revision；
- `memory_bytes` 统计主要 heap payload，不含 allocator 和对象头开销；
- 没有实现 weighted variants、bounded-load variants 或 replica top-K；
- sample distribution 不能替代生产 flow-size skew、heavy hitter 和 backend
  capacity 模型；
- 任何 packet-per-second 结论必须在目标 Linux、目标 NIC、native XDP 模式
  下另测。
