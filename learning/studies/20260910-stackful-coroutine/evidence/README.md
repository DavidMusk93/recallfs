# Evidence Ledger

## 1. Provenance

| Field | Value |
| --- | --- |
| Target | `ssh d2` / host `n37-125-152` |
| Code commit | `1a824544d6a487d32dec3949b0e90deb1dd8b602` |
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

## 2. Source And Binary Identity

| Artifact | SHA-256 |
| --- | --- |
| `src/rco.c` | `e73c0089cdffef4dafccd666a659ec7da3a202107d9127efd7d7593d2820ca9b` |
| `src/rco_context_x86_64.S` | `42b906658f44801887a1903288766fae96d19f0e5db8137a703b2e590bc955e5` |
| `bench/rco_bench.c` | `d12ffdfa9707d209e01f8c9afea49ea5e79ec3d6b784bd4b31a96a3287b1e6b2` |
| `examples/l4_forwarder.c` | `9d09062b14a242e77eeaec0aefb26ba7890e65ce9ad355fc1e1e9233ad1f3baa` |
| optimized context benchmark | `19ec079208299acfc31be1e58fe40d2f6bb18565228c9d08d9ba43bcefaf52be` |
| optimized L4 forwarder | `aaa3a1c0aa43c54a8d0329937a03abdce7d4bee7ba1f5c879ae0cc0a9d9311e1` |

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
| Native CTest | 3/3 targets passed | [`raw/native-tests.txt`](raw/native-tests.txt) |
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
| `rco_yield` | `33.952 ns` |
| noinline function call | `1.638 ns` |
| Linux `sched_yield` | `228.380 ns` |

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
| direct loopback | `75.328 Gbit/s` | `74.140-77.944` |
| one-core L4 proxy | `26.484 Gbit/s` | `24.887-27.077` |

The median proxy/direct ratio is `0.352`. Full iperf client/server JSON,
forwarder counters, environment, and CSV are under [`raw/l4/`](raw/l4/).

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
