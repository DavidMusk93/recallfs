# Evidence Ledger

## 1. Provenance

| Field | Value |
| --- | --- |
| Target | `ssh d2` / host `n37-125-152` |
| Code commit | `ecda6780273a57adcde8e6c94754563f61671e64` |
| Kernel | Linux `5.15.198.bsk.1-amd64` |
| CPU | Intel Xeon Platinum 8457C, 2 sockets, 64 cores, no SMT |
| Native compiler | GCC 8.3.0 |
| Cross-check compiler | Clang 11.0.1 |
| FIL-C | 0.684, x86-64 |
| FIL-C archive SHA-256 | `eefb594bcbc1261a18dfa8b50041674635f53df2b5fe067915b5652adaed4e3f` |
| FIL-C container | `debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171` |

The d2 host glibc 2.28 cannot run the FIL-C compiler directly. FIL-C therefore
ran in the pinned Linux container on d2. Native tests and all performance
measurements ran directly on the d2 host.

Full environment: [`raw/environment.txt`](raw/environment.txt).
Review receipt and closure: [`review.md`](review.md).

## 2. Source And Binary Identity

| Artifact | SHA-256 |
| --- | --- |
| `src/rco.c` | `f496cac01206d510a1c2ae25a1d4d0ea262808a33e6da41d623a360d557fbf51` |
| `src/rco_context_x86_64.S` | `42b906658f44801887a1903288766fae96d19f0e5db8137a703b2e590bc955e5` |
| `bench/rco_bench.c` | `9de36e4eec8f819d3bbfe2cc7d70bf7a9c1365e93bc94f384f41564bd2c0dcbc` |
| `examples/l4_forwarder.c` | `c5e2aabbe0b5a2bf46b116442045a1bd543b5708af97caa2cb253b8459ef0e37` |
| `bench/l4_bench.sh` | `141ab559ffb4c72341aca760b1bce28e437bf23576998b7f82a3ac316c8b61cd` |
| optimized context benchmark | `a7d8c0c651f90a68144ae1e1c268f31c982746a0ed0036c1419192ebf1892cde` |
| optimized L4 forwarder | `61f526568e0f8234af40a0ca0acf212802dffcd24793c89f9e6f04df880bb8e7` |

Build flags:

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto
-fno-ipa-icf -Wall -Wextra -Werror -fno-omit-frame-pointer
-fcf-protection=branch
```

Raw record: [`raw/build-info.txt`](raw/build-info.txt).

## 3. Correctness Gates

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Proof-first native link | Expected undefined `rco_*` symbols | [`../exploration.md`](../exploration.md) |
| FIL-C lifecycle | 3 suites passed | [`raw/filc.txt`](raw/filc.txt) |
| FIL-C L4 source | Full C compile and CLI smoke passed | [`raw/filc.txt`](raw/filc.txt) |
| Native CTest | 4/4 targets passed | [`raw/native-tests.txt`](raw/native-tests.txt) |
| GCC/Clang optimization matrix | O0/O2/O3 and O3+LTO passed | [`raw/optimization-matrix.txt`](raw/optimization-matrix.txt) |
| ASan + UBSan | Concurrent L4 and forced drain passed | [`raw/sanitizers.txt`](raw/sanitizers.txt) |
| Clang static analyzer | No findings | [`raw/static-analysis.txt`](raw/static-analysis.txt) |
| ABI disassembly | Expected save/restore sequence present | [`raw/context-switch-disassembly.txt`](raw/context-switch-disassembly.txt) |
| Executable stack | `GNU_STACK` is `RW` | [`raw/gnu-stack.txt`](raw/gnu-stack.txt) |

FIL-C uses `tests/rco_context_filc.c`, whose switch function aborts if called.
It validates executed C allocation and lifecycle paths but does not claim to
validate assembly, stack alignment, native epoll timing, or performance.

## 4. Context Benchmark

Five independent runs, each taking the median of 11 samples with 1,000,000
iterations, pinned to CPU 0 and NUMA node 0:

| Operation | Median |
| --- | ---: |
| `rco_yield` | `35.335 ns` |
| noinline function call | `1.646 ns` |
| Linux `sched_yield` | `228.870 ns` |

`rco_yield` includes scheduler bookkeeping and two assembly transfers. Every
sample retained a nonzero observable sink. Raw samples:
[`raw/context-switch.csv`](raw/context-switch.csv).

## 5. L4 Benchmark

iperf3 configuration:

```text
proxy:  CPU 0, NUMA 0
server: CPU 1, NUMA 0
client: CPU 2, NUMA 0
flows:  4
time:   3 seconds measured after 1 second omit
runs:   5
```

| Path | Median | Range |
| --- | ---: | ---: |
| direct loopback | `74.476 Gbit/s` | `73.826-77.154` |
| one-core L4 proxy | `24.666 Gbit/s` | `24.517-24.900` |

The median proxy/direct ratio is `0.331`. The benchmark now rejects a sample
unless all four sender and receiver streams make progress. Full per-stream
CSV, iperf client/server JSON, forwarder counters, and environment are under
[`raw/l4/`](raw/l4/).

The benchmark is loopback, not a real-NIC result. It demonstrates sustained
TCP data movement and a reproducible comparison baseline, not line-rate or
tail-latency readiness.

## 6. PMU Limitation

The required hardware counters were requested with:

```text
perf stat -e cycles,instructions,branches,branch-misses,cache-misses
```

d2's KVM returned `<not supported>` for every event. The fallback evidence is
recorded CPU/NUMA topology, fixed affinity, source/binary digests, complete
flags, repeated wall-time samples, and observable sinks. See
[`raw/perf-stat.txt`](raw/perf-stat.txt).

The KVM exposes no cpufreq policy directories, `intel_pstate/no_turbo`, or
generic cpufreq boost control. Both context and L4 environment records state
that frequency policy and turbo state are unavailable instead of silently
omitting them.
