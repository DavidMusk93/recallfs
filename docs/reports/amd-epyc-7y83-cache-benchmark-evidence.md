# AMD EPYC 7Y83 Cache Benchmark Evidence

> Target: `dc02-pe-t137-n047` (`fdbd:dc02:e:137::47`)
>
> Measurement date: 2026-09-07
>
> Companion source:
> [`tools/docs/examples/cache_hierarchy_demo.c`](../../tools/docs/examples/cache_hierarchy_demo.c)
>
> Explainer:
> [`docs/explainers/why-cpus-have-multiple-cache-levels.html`](../explainers/why-cpus-have-multiple-cache-levels.html)

## 0. Conclusions

The target exposes three cache-capacity transitions that agree with its sysfs
topology:

| Boundary | Topology | Observed dependent-load transition |
| --- | --- | --- |
| L1D | 32 KiB per core | 32 KiB: 1.95 ns; 64 KiB: 4.38 ns |
| L2 | 512 KiB per core | 512 KiB: 6.41 ns; 1 MiB: 14.33 ns |
| L3 | 32 MiB per 8-core CCD | 32 MiB: 43.23 ns; 64 MiB: 81.89 ns |

The 256 MiB dependent-load median was `113.49 ns` with memory on local NUMA
node 0 and `306.41 ns` with memory on remote-socket node 2. The remote path was
about 2.7 times slower.

These are end-to-end dependent-load costs, not pure cache or DRAM latency.
They include address generation, TLB behavior, page-table walks, interconnect,
memory-controller queues, frequency variation, and operating-system noise.

## 1. Target identity

| Property | Observed value |
| --- | --- |
| OS | Debian 12 (bookworm) |
| Kernel | `5.15.152.bsk.15-amd64` |
| Architecture | `x86_64` |
| CPU | AMD EPYC 7Y83 64-Core Processor |
| CPU family/model/stepping | `25/1/1` |
| Sockets | 2 |
| Physical cores | 128 |
| Logical CPUs | 256 |
| NUMA nodes | 4 |
| Frequency policy | `performance`, boost enabled |
| Base page size | 4096 bytes |
| THP policy | `madvise` |

The machine was not configured with isolated CPUs. A three-second `mpstat`
sample showed CPU 24 about 99% idle before the benchmark, but this does not
eliminate interrupts or later scheduling noise.

## 2. Cache and NUMA topology

`lscpu -C` and `/sys/devices/system/cpu/cpu0/cache/index*` reported:

| Cache | Per instance | Associativity | Line | Shared CPUs for CPU 0 |
| --- | ---: | ---: | ---: | --- |
| L1D | 32 KiB | 8-way | 64 B | `0,128` |
| L1I | 32 KiB | 8-way | 64 B | `0,128` |
| L2 unified | 512 KiB | 8-way | 64 B | `0,128` |
| L3 unified | 32 MiB | 16-way | 64 B | `0-7,128-135` |

The complete machine contains 128 L1/L2 instances and 16 L3 instances.
`lscpu` therefore reports an aggregate 512 MiB L3, but one thread does not
have uniform access to one monolithic 512 MiB cache.

CPU 24 belongs to NUMA node 0 and shares L3 with
`24-31,152-159`. The measured NUMA distance matrix was:

```text
node   0   1   2   3
  0:  10  12  32  32
  1:  12  10  32  32
  2:  32  32  10  12
  3:  32  32  12  10
```

## 3. Source and compiler identity

FIL-C 0.684 compiled and ran the correctness modes before native measurement.
The target benchmark used GCC 12.2.0:

```text
-std=c11 -O3 -march=native -mtune=native -flto -DNDEBUG
-Wall -Wextra -Werror
```

On this machine, GCC resolved `-march=native -mtune=native` to `znver3`.

Final digests:

```text
e624ff2937163c5a9c1be976855041a6c6f21e50958a1bc3465d109ecfb96f65  cache_hierarchy_demo.c
5e137dca3638ad3b626812f966c400daa62fe3eb99881676e23b1c62d4001a0e  .tmp/cache-hierarchy-native
```

The source was transferred to a fresh target directory and its SHA-256 was
compared before compiling.

An optimized negative-control build changed both reduction returns from
`return sum;` to `return sum + 1;` and used the same native flags. Its
`locality` mode exited with status 1 and printed:

```text
validation failed: row-major and column-major checksums differ
```

This confirms that `-DNDEBUG` does not remove the benchmark's always-on result
oracle.

## 4. Commands

Correctness:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  tools/docs/examples/cache_hierarchy_demo.c \
  -o .tmp/cache-hierarchy-demo
.tmp/fil-c/bin/filrun .tmp/cache-hierarchy-demo check
.tmp/fil-c/bin/filrun .tmp/cache-hierarchy-demo all
```

Source transfer and identity check:

```bash
REMOTE=fdbd:dc02:e:137::47
REMOTE_DIR=/tmp/recallfs-cache-bench-20260907
SOURCE=tools/docs/examples/cache_hierarchy_demo.c
LOCAL_SHA=$(shasum -a 256 "$SOURCE" | cut -d ' ' -f 1)
ssh -J j "$REMOTE" "install -d -m 700 '$REMOTE_DIR/.tmp'"
scp -o ProxyJump=j "$SOURCE" "[$REMOTE]:$REMOTE_DIR/cache_hierarchy_demo.c"
REMOTE_SHA=$(ssh -J j "$REMOTE" \
  "sha256sum '$REMOTE_DIR/cache_hierarchy_demo.c' | cut -d ' ' -f 1")
test "$LOCAL_SHA" = "$REMOTE_SHA"
```

Native build on the target:

```bash
mkdir -p .tmp
gcc -std=c11 -O3 -march=native -mtune=native -flto -DNDEBUG \
  -Wall -Wextra -Werror cache_hierarchy_demo.c \
  -o .tmp/cache-hierarchy-native
sha256sum cache_hierarchy_demo.c .tmp/cache-hierarchy-native
```

Local and remote memory runs:

```bash
./.tmp/cache-hierarchy-native check
numactl --physcpubind=24 --membind=0 \
  ./.tmp/cache-hierarchy-native all
numactl --physcpubind=24 --membind=2 \
  ./.tmp/cache-hierarchy-native latency
```

Core PMU events were run one at a time:

```bash
for event in \
  ls_dc_accesses \
  l1_data_cache_fills_all \
  l2_cache_accesses_from_dc_misses \
  l2_cache_misses_from_dc_misses
do
  perf stat -a -C 24 -r 5 -e "$event" -- \
    numactl --physcpubind=24 --membind=0 \
    ./.tmp/cache-hierarchy-native locality
done
```

L3 uncore events were collected separately:

```bash
perf stat -a -C 24 -r 5 \
  -e amd_l3/event=0x4,umask=0xff/,amd_l3/event=0x4,umask=0x1/ -- \
  numactl --physcpubind=24 --membind=0 \
  ./.tmp/cache-hierarchy-native locality
```

## 5. Dependent-load results

Each cell is `median [min, max]` in nanoseconds per dependent load over seven
rounds.

| Working set | CPU 24 + node 0 local | CPU 24 + node 2 remote |
| ---: | ---: | ---: |
| 4 KiB | 1.87 [1.87, 1.89] | 1.88 [1.87, 1.88] |
| 8 KiB | 1.87 [1.87, 1.89] | 1.87 [1.87, 1.88] |
| 16 KiB | 1.87 [1.87, 1.88] | 1.88 [1.87, 1.90] |
| 32 KiB | 1.96 [1.94, 1.98] | 1.96 [1.93, 1.97] |
| 64 KiB | 4.38 [4.38, 4.39] | 4.38 [4.37, 4.45] |
| 128 KiB | 4.38 [4.37, 4.39] | 4.38 [4.38, 4.40] |
| 256 KiB | 4.41 [4.39, 4.42] | 4.42 [4.40, 4.47] |
| 512 KiB | 6.41 [6.36, 6.52] | 9.04 [9.01, 9.08] |
| 1 MiB | 14.33 [14.28, 14.33] | 14.16 [14.12, 14.52] |
| 2 MiB | 16.10 [16.08, 16.29] | 16.28 [16.21, 17.59] |
| 4 MiB | 16.99 [16.92, 17.72] | 17.57 [17.09, 17.66] |
| 8 MiB | 17.66 [17.63, 17.82] | 17.66 [17.59, 17.92] |
| 16 MiB | 21.50 [21.43, 21.63] | 21.70 [21.57, 22.48] |
| 32 MiB | 43.23 [41.85, 46.20] | 100.22 [94.77, 120.12] |
| 64 MiB | 81.89 [80.96, 83.61] | 242.26 [236.60, 246.25] |
| 128 MiB | 105.06 [104.74, 106.12] | 296.99 [288.35, 299.64] |
| 256 MiB | 113.49 [113.35, 113.54] | 306.41 [288.88, 310.47] |

The large spread at 32 MiB is expected near the nominal L3 capacity boundary:
replacement, set conflicts, TLB behavior, page mapping, and background work
all affect the mixed hit/miss region.

## 6. Row-major versus column-major

For a 2048 by 2048 matrix of `uint64_t` values (32 MiB):

```text
row-major:     1.650 ms median [1.629, 1.974]
column-major: 28.444 ms median [28.363, 28.838]
median ratio: 17.24x
```

The benchmark uses volatile reads to prevent GCC from legally interchanging or
eliminating the intended access order. This is a benchmark control, not a
production optimization recommendation.

## 7. PMU evidence

Core events, each measured in a separate five-run command:

| Event | Mean | Variation | Running time |
| --- | ---: | ---: | ---: |
| `ls_dc_accesses` | 74,945,870 | +/- 0.88% | 71.40% |
| `l1_data_cache_fills_all` | 50,022,862 | +/- 0.36% | 71.09% |
| `l2_cache_accesses_from_dc_misses` | 51,066,213 | +/- 0.35% | 70.97% |
| `l2_cache_misses_from_dc_misses` | 21,412,191 | +/- 0.35% | 71.11% |

L3 uncore events:

| Event | Mean | Variation |
| --- | ---: | ---: |
| `amd_l3/event=0x4,umask=0xff/` | 50,892,834 | +/- 0.36% |
| `amd_l3/event=0x4,umask=0x1/` | 4,233,126 | +/- 3.96% |

An idle `sleep 0.25` baseline still recorded about 896 thousand
`ls_dc_accesses`, 63 thousand L1 fills, 72 thousand L2 accesses, and 23
thousand L2 misses. The system-wide counts include kernel or unrelated work.
The unfiltered `amd_l3` PMU covers the shared CCD rather than only the
benchmark process.

These counters are supplementary evidence only. They must not be mixed across
runs to compute an exact hit rate.

## 8. Raw artifacts

The canonical output files are committed under
[`docs/reports/evidence/amd-epyc-7y83-cache-20260907/`](evidence/amd-epyc-7y83-cache-20260907/):

| File | SHA-256 |
| --- | --- |
| `cache-local-final.txt` | `da67f3917137e005de0c5e6182aba4cf904bfa57975f38b31eaaa0e0d00e25db` |
| `cache-remote-final.txt` | `394aa81ff415044c93c9ac1f32232276e737cb4bb8a9042fd14c7506e1234691` |
| `pmu-core-final2.txt` | `feeec5e40f377a2618f6caefc3de438d1cc630201cc3eea2f0174c90d8073602` |
| `pmu-l3-final2.txt` | `a5383e2ce04437b57aa805760f215e9043c3de8cda8eb95d3bff87513102cbe9` |
| `pmu-idle-final2.txt` | `8979b453a379e71aa9abc153c224e0b19b5407e091b9436051419af3308018f7` |
| `topology-final.txt` | `8db8d3b748116f0e62532378d6d0ba71bb12d5d89bf71e7980c05ed9440c4fe2` |

## 9. Evidence limits

- The benchmark CPU was pinned but not isolated.
- Frequency boost remained enabled, so nanoseconds are more comparable than
  inferred cycle counts.
- The pointer chase used 4 KiB base pages. It measures cache, DTLB, page-walk,
  interconnect, and memory effects together.
- One deterministic shuffled cycle was reused for seven timing rounds at each
  size. The results do not characterize all virtual or physical mappings.
- The uncore L3 PMU was not filtered by `coreid` or `threadmask`.
- The measured values characterize this machine and run, not all Zen 3 or
  EPYC 7003 processors.
