---
doc_id: recallfs-runbook-ringzero-hashing-benchmark-v2
kind: runbook
status: active
authority: implementation
applies_to:
  - learning/studies/20260917-ringzero/benchmark
depends_on:
  - recallfs-study-ringzero-v2
supersedes:
  - recallfs-runbook-ringzero-hashing-benchmark-v1
verified_by:
  - RZ-RA-1
  - RZ-RA-2
  - RZ-RA-3
  - RZ-RA-4
  - RZ-RA-6
  - RZ-RA-7
---

# C Consistent Hashing Benchmark And XDP Probe

这个 C11 artifact 包含两部分：

- `ringzero_hashing`：Modulo、Ring、Rendezvous、Jump、Maglev 的统一 selector
  API；
- `ringzero_udp_sequence`：带独立 sequence bitmap 的 UDP sender/receiver，
  用于判断实际 packet path 的 missing、duplicate 和 invalid packet。

它不把 selector microbenchmark 当作完整 XDP 性能，也不把一次低速零丢包
观察外推为 line-rate guarantee。

## Layout

```text
benchmark/
  CMakeLists.txt
  include/hashing.h
  src/hashing.c
  src/compare.c
  tests/test_hashing.c
  tools/udp_sequence.c
  scripts/run_on_d2.sh
  scripts/validate_d2.sh
  scripts/validate_ringzero_netns.sh
```

## Build And Test

本机或普通 Linux：

```bash
cmake -S learning/studies/20260917-ringzero/benchmark \
  -B .tmp/ringzero-hashing-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .tmp/ringzero-hashing-build
ctest --test-dir .tmp/ringzero-hashing-build --output-on-failure
```

d2 C/FIL-C/benchmark 验证：

```bash
learning/studies/20260917-ringzero/benchmark/scripts/run_on_d2.sh d2
```

若 d2 已按 [`../evidence/README.md`](../evidence/README.md) 的 build evidence
准备好 pinned RingZero snapshot，可用同一入口追加 native-XDP E2E：

```bash
learning/studies/20260917-ringzero/benchmark/scripts/run_on_d2.sh \
  d2 /root/recallfs/.tmp/ringzero-upstream-validation
```

该入口把 source 同步到 d2 的隔离 `.tmp`，依次执行：

1. FIL-C 0.684 correctness、benchmark code path 和 UDP self-test；
2. 固定 Zig 0.16.0/Clang 21.1.0 的 Release + ThinLTO CMake build；
3. GCC 8.3.0 的独立 Release + LTO build；
4. 两套编译器各 5 次、轮换算法顺序、固定 CPU 16/NUMA 0 的 benchmark；
5. 两套编译器的跨编译器 semantic equality 和 1x/4x anti-DCE；
6. 两个 final binary 的 disassembly、source/tool/binary digest 和 perf evidence；
7. manifest 校验后事务式提升 evidence；
8. 提供 RingZero snapshot 时，验证 revision/binary digest 后执行 native-XDP。

FIL-C 时间不进入性能结论。

## API And Ownership

`include/hashing.h` 公开 opaque `rz_selector`：

- `rz_selector_create()` canonicalize backend IDs 并拥有其内部副本；
- duplicate ID 返回 `EEXIST`；
- 空集合、非法 kind、非 prime Maglev table 或零 vnodes 返回 `EINVAL`；
- allocation/size failure 返回 `ENOMEM`/`EOVERFLOW`；
- `rz_selector_destroy(NULL)` 是 no-op；
- selector 创建后 immutable，可并发只读；
- `rz_selector_maglev_table()` 返回 selector-owned view，生命周期不超过
  selector。

作为父 CMake project 的子目录使用时，可通过 `BUILD_TESTING=OFF`、
`RZ_BUILD_BENCHMARK=OFF` 和 `RZ_BUILD_TOOLS=OFF` 只构建
`ringzero::hashing`。

`rz_maglev_build_ordered()` 故意保留 caller order，用于复现“未 canonicalize
的 membership 会改变 table”这一故障。普通 caller 应使用
`rz_selector_create()`。

## Benchmark Contract

输出为 CSV。所有算法消费同一批预先 hash 的 `u64` keys：

| Metric | Meaning |
| --- | --- |
| `max_over_avg` / `min_over_avg` / `cv` | sampled key-count balance |
| `add_churn` | 32 到 33 backends 的 key remap 比例 |
| `remove_middle_churn` | 删除 backend 16 的 key remap 比例 |
| `lookup_ns` | 不含共同 key hash，包含 selector dispatch、循环与 checksum oracle 的 median |
| `build_us` | validated selector construction median |
| `memory_bytes` | selector 主要 heap payload |
| `checksum` | 每个 timed run 都必须匹配的结果 oracle |

`selector_fingerprint()` 在计时结束后消费新构建 selector，防止 compiler
删除或搬空 build work。volatile sink 使用加法并混入 sample index，不会像
旧 Rust 版那样在偶数轮 XOR 时归零。4x lookup workload 的 wall time 必须
至少达到 1x 的 2 倍。

## d2 Result

环境：Linux `5.15.198.bsk.1-amd64`、Intel Xeon Platinum 8457C、CPU 16、
NUMA node 0。下表是五个轮换顺序 run 的 median-of-medians：

| Compiler | Algorithm | Max/avg | Add churn | Remove-middle | Lookup ns | Build us |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Zig Clang 21.1 | Modulo | 1.011 | 96.95% | 96.88% | 3.298 | 0.514 |
| Zig Clang 21.1 | Ring | 1.096 | 2.87% | 3.28% | 98.283 | 1138.465 |
| Zig Clang 21.1 | Rendezvous | 1.010 | 3.01% | 3.11% | 117.616 | 0.536 |
| Zig Clang 21.1 | Jump | 1.010 | 3.00% | 49.96% | 37.015 | 0.534 |
| Zig Clang 21.1 | Maglev | 1.007 | 4.76% | 5.20% | 3.294 | 66.704 |
| GCC 8.3 | Modulo | 1.011 | 96.95% | 96.88% | 3.289 | 0.491 |
| GCC 8.3 | Ring | 1.096 | 2.87% | 3.28% | 92.804 | 1124.337 |
| GCC 8.3 | Rendezvous | 1.010 | 3.01% | 3.11% | 125.079 | 0.559 |
| GCC 8.3 | Jump | 1.010 | 3.00% | 49.96% | 35.259 | 0.525 |
| GCC 8.3 | Maglev | 1.007 | 4.76% | 5.20% | 3.290 | 62.485 |

两个 compiler 的所有 balance、churn 和 checksum 完全一致。hardware PMU 在
d2 KVM 中报告 `<not supported>`；证据降级为 pinned topology、wall/process
time、两编译器交叉验证、binary/source digest、反汇编和 scaling probe。

## RingZero Netns Probe

`validate_ringzero_netns.sh` 只操作带当前 PID 后缀的 `rzv*` veth/netns 和
`/sys/fs/bpf/ringzero-validation-*`。它配置单 backend，发送 10,000 个带序号
UDP packets，并要求：

```text
sender:   expected=10000 sent=10000
receiver: expected=10000 unique=10000 missing=0 duplicates=0 invalid=0
XDP:      packets=10000 dropped=0
cleanup:  pass
```

这是 native XDP/veth、请求 50 us inter-packet pacing 的 correctness
证据。sender 未测量实际 elapsed time，因此不报告 achieved pps；它也不是物理
NIC、multi-queue、RSS 或 line-rate 证据。

## Reconciliation Anchors

| Anchor | Input | Exact expected result | Command |
| --- | --- | --- | --- |
| `RZ-RA-1` | NSDI'16 的 3 backend、7 slot 样例 | `[1,0,1,0,2,2,0]`；移除 1 后 `[0,0,0,0,2,2,2]` | CTest |
| `RZ-RA-2` | 32 backends、4099 slots | 每个 backend 的 slot 数只相差 1 | CTest |
| `RZ-RA-3` | `[1,2,3,4]` 与逆序输入 | canonical table 相同；raw ordered table 恰有 8 slots 不同 | CTest |
| `RZ-RA-4` | 1M keys、32 到 33 backends | Modulo add churn >95%；Maglev 2%-8%；Jump middle-remove 45%-55% | `compare` |
| `RZ-RA-6` | d2 C safety/native matrix | FIL-C paths、Zig 7/7、GCC 7/7 pass | `run_on_d2.sh` |
| `RZ-RA-7` | d2 veth/native-XDP，10k sequenced UDP | 10k unique、0 missing/duplicate/invalid、XDP 10k、cleanup pass | `run_on_d2.sh d2 <ringzero-root>` |

## Limits

- `memory_bytes` 不含 allocator metadata 与 selector object；
- Ring balance 依赖 vnode count；
- Jump middle removal 反映 dense bucket renumbering；
- 未实现 weighted、bounded-load 或 replica top-K variants；
- d2 是单队列 `virtio_net` KVM；不能验证 physical NIC/RSS scaling；
- hardware PMU 不可用，不能报告 cycles、cache/TLB misses；
- sequence probe 的单次低速结果不证明高压或长稳 zero-drop。
