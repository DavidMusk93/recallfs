# Exploration Log

## 1. Decisions

| Step | Decision | Reason |
| --- | --- | --- |
| 1 | Reproduce in C11 | The mechanism is integer arithmetic and the repository requires C claims to pass FIL-C |
| 2 | Keep five scalar variants | Separates algebraic reordering from fixed-point and base-64 tricks |
| 3 | Exhaust every claimed restricted range | Boundary correctness is the main risk of reciprocal multiplication |
| 4 | Separate latency and throughput loops | A shorter dependency chain does not imply fewer total operations |
| 5 | Pin native runs to CPU 31 / NUMA 0 | Reduces scheduler and remote-memory variation |
| 6 | Preserve all three latency and throughput outputs through compiler barriers | Prevents the benchmark consumer from algebraically deleting or combining result fields |

## 2. Source Inspection

The article was opened successfully in a browser on 2026-09-08. Its
prerendered HTML was downloaded temporarily and hashed:

```text
a9f207b54dea3f16d8970fe341ce5b9d1d2bcbc169ee5088c385074ca72a7c6a
```

The linked repository was checked out at the exact article footer revision:

```text
25f6d9345f2d681a77315cf6eaeee25b3629af8c
Algorithms and benchmarks for time-of-day
```

Inspection of `time/algorithms/benjoffe.hpp`, `traditional.hpp`, `neri.hpp`,
`time_test.cpp`, and `time_bench.cpp` confirmed the formulas, claimed ranges,
exhaustive upstream checks, and separate latency/throughput modes.

## 3. Proof-First Correctness

The public header and exhaustive test were written before the implementation.
FIL-C reached the linker and failed on all five expected missing functions:

```text
undefined reference to `pizlonated_hms_traditional'
undefined reference to `pizlonated_hms_parallel_div'
undefined reference to `pizlonated_hms_parallel_fixed'
undefined reference to `pizlonated_hms_hi_low'
undefined reference to `pizlonated_hms_base64'
```

After implementation, the authoritative command was:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260908-fast-time-of-day/demo/include \
  learning/studies/20260908-fast-time-of-day/demo/src/fast_time.c \
  learning/studies/20260908-fast-time-of-day/demo/tests/fast_time_test.c \
  -o .tmp/fast-time-of-day-test-ndebug

.tmp/fil-c/bin/filrun .tmp/fast-time-of-day-test-ndebug
```

Observed:

```text
FIL-C correctness passed: all variants exhaustive over one day; fixed-point variants exhaustive over their claimed extended ranges; full-range division variants sampled at uint32 boundaries
```

The benchmark executable was also compiled and executed with FIL-C using
`--count 64 --rounds 2 --samples 2`. Those timings are discarded; the run only
verifies the benchmark's executed memory and arithmetic paths, including a
second outer-round iteration.

## 4. CMake Result

The FIL-C CMake build and CTest succeeded. CMake's IPO probe failed because the
host `/usr/local/bin/llvm-ar` is LLVM 15 while FIL-C emits LLVM 20 bitcode:

```text
llvm-ar: error: libfoo.a: Unknown attribute kind (86)
(Producer: 'LLVM20.1.8' Reader: 'LLVM 15.0.0git')
```

CMake correctly continued without IPO for the FIL-C build. This is a toolchain
composition issue, not an algorithm failure. The direct FIL-C command remains
the shortest correctness path.

## 5. Target-Machine Setup

`orthrus-cli demand fdbd:dc02:e:137::47` returned:

```text
err when kgetcred, exit status 1
```

Existing credentials still permitted direct access with:

```bash
ssh fdbd:dc02:e:137::47 -J j
```

Target facts:

| Item | Value |
| --- | --- |
| Host | `dc02-pe-t137-n047` |
| OS | Debian 12, Linux `5.15.152.bsk.15-amd64` |
| CPU | AMD EPYC 7Y83 64-Core Processor |
| Topology | 2 sockets, 128 physical cores, SMT2, 256 logical CPUs |
| NUMA | 4 nodes |
| Selected CPU | CPU 31, node 0, physical core 31 |
| SMT sibling | CPU 159 |
| Pre-run idle sample | CPU 31 was 100% idle over three one-second samples |
| Governor | `performance` |
| Boost | enabled |
| Reported frequency | 2450 MHz before/after run |
| Compiler | GCC 12.2.0 |
| Linker | GNU ld 2.40 |

The initial attempt to use `/usr/bin/time` failed because that binary is not
installed. Wall time was then recorded with `date +%s%N`.

## 6. GCC Portability Failure

The first target build failed under `-Werror`:

```text
error: ignoring '#pragma GCC novector' [-Werror=unknown-pragmas]
```

GCC 12 predates support for that pragma. The benchmark now uses
`optimize("no-tree-vectorize")` on GCC loop functions and retains Clang's loop
pragma on Clang.

## 7. Assembly-Guided Benchmark Fix

### Latency Codegen

The first optimized disassembly proved that no conversion loop was eliminated,
but revealed that the latency consumer allowed GCC to combine V3's minute and
second masks before XOR. The benchmark was measuring the requested XOR, not
necessarily materialization of three independent result fields.

A zero-instruction compiler barrier now takes all three fields as separate
read/write register operands. Latency mode uses the preserved fields to feed
the next input; throughput mode preserves them before packing each result.
Final `base64_latency` assembly contains separate operations for both fields:

```text
2 x imul
2 x lea
2 x and
```

The five latency loop symbols remain distinct because the build uses
`-fno-ipa-icf`. The final instruction summary is:

| Loop | `imul` | `idiv` | Characteristic result work |
| --- | ---: | ---: | --- |
| Traditional | 3 | 1 | serial hour remainder, then minute/second |
| V1 division | 4 | 0 | two independent reciprocal-divide chains |
| V1 fixed-point | 4 | 0 | explicit 32-bit reciprocal products |
| V2 hi/low | 4 | 0 | two quotient products plus two remainder products |
| V3 base-64 | 2 | 0 | two quotient products plus `lea` and masks |

Counts refer to one unrolled scalar conversion body in the relevant latency
loop, excluding loop-control instructions.

### Throughput Codegen

The final source was rebuilt on the same EPYC/GCC target after adding the
throughput field barriers. All five throughput symbols remained distinct.
Each contained two backward branches, one for the inner count loop and one for
the outer rounds loop, and none referenced an `xmm`, `ymm`, or `zmm` register.
Whole-function counts across the eight unrolled conversions were:

| Throughput symbol | `imul` | `idiv` |
| --- | ---: | ---: |
| Traditional | 24 | 8 |
| V1 division | 32 | 0 |
| V1 fixed-point | 32 | 0 |
| V2 hi/low | 32 | 0 |
| V3 base-64 | 16 | 0 |

### Rounds-Scaling Negative Check

As a negative check for rounds-loop collapse, two throughput-only runs used
`--count 8192 --samples 31`:

| Rounds | External wall time |
| ---: | ---: |
| 64 | `167640412 ns` |
| 256 | `665414030 ns` |

The `4x` rounds increase produced approximately `3.97x` wall time. Combined
with the two backward branches in every throughput symbol, this is inconsistent
with the compiler collapsing or hoisting the rounds loop in the final binary.

## 8. Benchmark Method

Native flags:

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG
-flto -fno-ipa-icf -Wall -Wextra -Werror
```

Execution:

```bash
numactl --physcpubind=31 --membind=0 \
  ./fast_time_benchmark --count 8192 --rounds 256 --samples 31
```

Three complete runs were taken. Each reported value is the median of 31
samples; the final study table uses the median of those three run medians.
Every sample performs `2,097,152` conversions. The whole ten-case benchmark
took `2057264300`, `2058797047`, and `2063500134 ns`, about `2.06` seconds per
externally timed run.

Only these run-level median summaries were retained. They are not the raw
31-sample logs and cannot be used to recompute within-run distributions.

## 9. Perf Boundary

Software events were available for the final throughput binary:

| Mode | Task clock | CPU utilized | Context switches | CPU migrations | Page faults |
| --- | ---: | ---: | ---: | ---: | ---: |
| Throughput, 7 samples | `149.60 ms` | `0.996` | 1 | 1 | 140 |

Hardware PMU events were unavailable:

```text
<not counted> cycles
<not counted> instructions
<not counted> branches
<not counted> branch-misses

Some events weren't counted. Try disabling the NMI watchdog.
```

The machine's `perf_event_paranoid` value was `2`, and this session did not
change the host-wide NMI watchdog. The study therefore does not report cycles
per conversion or IPC.

## 10. Final Digests

| Artifact | SHA-256 |
| --- | --- |
| `include/fast_time.h` | `60b55b6b973739be7ccb990b431b9cc92945e1233cb4afd33b0fbded79ad5840` |
| `src/fast_time.c` | `e2c5ee5a7afcdf5aeea8b150e78c456eb0b8b5b00a4280b0cdde14194fd42e4d` |
| `src/benchmark.c` | `e7ea5a69b9586dc9dbbbd83496c438951e17f27ac8376b1c1ac0770b7f55918a` |
| `tests/fast_time_test.c` | `80098f6e723d7a80636c246b7531982d916979aaea79a168a1df192fcc7b1846` |
| Native test binary | `d9cfc203e6aed5e96073ec0e9e67320725ebbae36a121ab00ba7ff543f06e476` |
| Native benchmark binary | `1f147862798d9fa7c5de65706976603e1d2c22a9ab86dd3dbc4c47321e33c74d` |
