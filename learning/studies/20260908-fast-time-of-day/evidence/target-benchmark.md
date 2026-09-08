# Target Benchmark Evidence

## Environment

```text
host=dc02-pe-t137-n047
os=Debian GNU/Linux 12 (bookworm)
kernel=5.15.152.bsk.15-amd64
cpu=AMD EPYC 7Y83 64-Core Processor
sockets=2
physical_cores=128
threads_per_core=2
logical_cpus=256
numa_nodes=4
pinned_cpu=31
pinned_numa_node=0
smt_sibling=159
scaling_governor=performance
frequency_boost=enabled
reported_cpu31_frequency_khz=2450000
compiler=gcc (Debian 12.2.0-14) 12.2.0
linker=GNU ld 2.40
```

The selected CPU was 100% idle across three one-second `mpstat` samples before
the run. The SMT sibling was not reserved, so unrelated sibling activity
remains a source of noise.

## Build

```bash
gcc \
  -std=c11 -O3 -march=native -mtune=native -DNDEBUG \
  -flto -fno-ipa-icf -Wall -Wextra -Werror \
  -I include src/fast_time.c tests/fast_time_test.c \
  -o fast_time_test

gcc \
  -std=c11 -O3 -march=native -mtune=native -DNDEBUG \
  -flto -fno-ipa-icf -Wall -Wextra -Werror \
  -I include src/fast_time.c src/benchmark.c \
  -o fast_time_benchmark
```

Native correctness result:

```text
FIL-C correctness passed: all variants exhaustive over one day; fixed-point variants exhaustive over their claimed extended ranges; full-range division variants sampled at uint32 boundaries
```

The message names the authoritative FIL-C suite; this native rerun used the
same test binary sources after the FIL-C gate.

The benchmark was rebuilt on this same target after adding scalar field
barriers to the throughput loops. All measurements, assembly checks, and
binary digests below refer to that final build.

## Three Complete Runs

Command:

```bash
numactl --physcpubind=31 --membind=0 \
  ./fast_time_benchmark --count 8192 --rounds 256 --samples 31
```

Each cell is `median_ns_per_value` from one complete run:

| Mode | Algorithm | Run 1 | Run 2 | Run 3 | Median |
| --- | --- | ---: | ---: | ---: | ---: |
| latency | traditional | `7.179671` | `7.178449` | `7.180975` | `7.179671` |
| latency | parallel_div | `3.745218` | `3.747396` | `3.744716` | `3.745218` |
| latency | parallel_fixed | `3.745271` | `3.748448` | `3.746809` | `3.746809` |
| latency | hi_low | `3.743556` | `3.747363` | `3.745567` | `3.745567` |
| latency | base64 | `3.122350` | `3.122795` | `3.120650` | `3.122350` |
| throughput | traditional | `2.511780` | `2.509658` | `2.510447` | `2.510447` |
| throughput | parallel_div | `2.069368` | `2.071848` | `2.071963` | `2.071848` |
| throughput | parallel_fixed | `1.874125` | `1.875875` | `1.874953` | `1.874953` |
| throughput | hi_low | `1.843808` | `1.845213` | `1.844138` | `1.844138` |
| throughput | base64 | `1.758145` | `1.757099` | `1.757218` | `1.757218` |

The retained values are run-level median summaries. The 31 raw sample values
and complete stdout/stderr logs were not retained, so this evidence cannot be
used to recompute or inspect the within-run sample distributions.

Relative reductions computed from the final medians are:

| Algorithm | Latency reduction | Throughput reduction |
| --- | ---: | ---: |
| parallel_div | `47.8%` | `17.5%` |
| parallel_fixed | `47.8%` | `25.3%` |
| hi_low | `47.8%` | `26.5%` |
| base64 | `56.5%` | `30.0%` |

Three externally timed complete runs took:

```text
run_1_wall_ns=2057264300
run_2_wall_ns=2058797047
run_3_wall_ns=2063500134
```

All runs produced the same nonzero sink:

```text
sink=14494414171720221588
```

## Optimized Assembly Check

### Latency Codegen

`nm` showed ten distinct no-inline loop symbols. For the latency symbols,
`objdump -d -Mintel` showed:

```text
traditional_latency  imul=3 idiv=1
parallel_div_latency imul=4 idiv=0
parallel_fixed_latency imul=4 idiv=0
hi_low_latency       imul=4 idiv=0
base64_latency       imul=2 idiv=0 lea=2 and=2
```

The V3 inner body includes the expected operations:

```asm
imul   r11,rax,0x4444445
imul   rax,rax,0x123457
shr    r11,0x20
shr    rax,0x20
lea    edx,[rdx+r11*4]
lea    r10d,[r11+rax*4]
and    edx,0x3f
and    r10d,0x3f
```

This proves the latency conversion work remains inside the timed loop and the
two base-64 result fields are materialized separately.

### Throughput Codegen

All five throughput symbols were also distinct. Each contained two backward
branches: the inner count loop and the outer rounds loop. None referenced an
`xmm`, `ymm`, or `zmm` register. Whole-function arithmetic instruction counts
across the eight unrolled conversions were:

| Throughput symbol | `imul` | `idiv` |
| --- | ---: | ---: |
| traditional | 24 | 8 |
| parallel_div | 32 | 0 |
| parallel_fixed | 32 | 0 |
| hi_low | 32 | 0 |
| base64 | 16 | 0 |

Together, the distinct symbols, scalar register set, conversion instruction
counts, and nested backward branches show that the final target binary retains
all eight scalar conversions inside the count loop and the count loop inside
the rounds loop.

## Throughput Rounds-Scaling Negative Check

Two mode-only runs used `--count 8192 --samples 31 --mode throughput`:

| Rounds | External wall time |
| ---: | ---: |
| 64 | `167640412 ns` |
| 256 | `665414030 ns` |

Increasing rounds by `4x` increased wall time by approximately `3.97x`. This
scaling is inconsistent with the compiler hoisting or collapsing the rounds
loop in the inspected final binary.

## Perf Cross-Check

Command shape:

```bash
perf stat -e task-clock,context-switches,cpu-migrations,page-faults -- \
  numactl --physcpubind=31 --membind=0 \
  ./fast_time_benchmark --count 8192 --rounds 256 --samples 7 \
  --mode throughput
```

| Mode | Task clock | CPU utilized | Context switches | CPU migrations | Page faults |
| --- | ---: | ---: | ---: | ---: | ---: |
| throughput | `149.60 ms` | `0.996` | 1 | 1 | 140 |

Hardware counters could not be scheduled:

```text
<not counted> cycles
<not counted> instructions
<not counted> branches
<not counted> branch-misses

Some events weren't counted. Try disabling the NMI watchdog.
```

No PMU-derived claim is made. The evidence degrades explicitly to pinned
topology, wall time, software perf events, and optimized assembly.

## Digests

```text
60b55b6b973739be7ccb990b431b9cc92945e1233cb4afd33b0fbded79ad5840  include/fast_time.h
e2c5ee5a7afcdf5aeea8b150e78c456eb0b8b5b00a4280b0cdde14194fd42e4d  src/fast_time.c
e7ea5a69b9586dc9dbbbd83496c438951e17f27ac8376b1c1ac0770b7f55918a  src/benchmark.c
80098f6e723d7a80636c246b7531982d916979aaea79a168a1df192fcc7b1846  tests/fast_time_test.c
d9cfc203e6aed5e96073ec0e9e67320725ebbae36a121ab00ba7ff543f06e476  fast_time_test
1f147862798d9fa7c5de65706976603e1d2c22a9ab86dd3dbc4c47321e33c74d  fast_time_benchmark
```
