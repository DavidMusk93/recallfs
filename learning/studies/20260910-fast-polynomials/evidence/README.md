---
doc_id: study-fast-polynomials-evidence-20260910
kind: runtime-evidence
status: verified
authority: observed
applies_to:
  - learning/studies/20260910-fast-polynomials/demo
depends_on:
  - learning/studies/20260910-fast-polynomials/source.md
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c
---

# Verification Evidence

## 1. Correctness Gate

FIL-C version:

```text
clang version 20.1.8
(Fil-C 0.684, revision 0f06037def80573162db5cfcda000af31006720e)
target: aarch64-unknown-linux-gnu
```

Command:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260910-fast-polynomials/demo/include \
  learning/studies/20260910-fast-polynomials/demo/src/fast_polynomial.c \
  learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c \
  -lm -o .tmp/fast-polynomial-test

.tmp/fil-c/bin/filrun .tmp/fast-polynomial-test
```

Observed:

```text
correctness passed: 8 exact anchors, 8193-point sweep;
max relative error horner=1.219e-15 chain=2.557e-15;
x^9 cancellation relative error=1.000e+00
```

The benchmark executable was also compiled and executed through FIL-C with
`64` rounds and `3` samples. Its timings are discarded; that run checks only
the benchmark's allocation, timer, validation, loop, and sink paths.

The FIL-C CMake build and its one CTest passed. CMake disabled IPO because the
host `/usr/local/bin/llvm-ar` is LLVM 15 and cannot read FIL-C's LLVM 20
bitcode. The direct FIL-C command is the authoritative correctness path.

## 2. Additional Compiler Checks

The C test passed with Zig 0.16.0 as the LLVM frontend at `-O0`, `-O2`, and
`-O3`. The `-O1 -fsanitize=address,undefined` build also passed.

Zig command shape:

```bash
zig cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I learning/studies/20260910-fast-polynomials/demo/include \
  learning/studies/20260910-fast-polynomials/demo/src/fast_polynomial.c \
  learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c \
  -lm -o .tmp/fast-polynomial-test-zig-O2
```

## 3. Reconciliation Anchors

| ID | Input | Expected | Probe |
| --- | --- | --- | --- |
| `FP-A1` | reverse Bessel $P_9$, $x=-16,-4,-1,0,1,2,4,16$ | both evaluators equal the eight exact integer values in the test | FIL-C test |
| `FP-A2` | reverse Bessel $P_9$, 8193 binary64 points in $[-2,2]$ | both evaluators have relative error at most $10^{-12}$ against long double Horner | FIL-C and Zig test |
| `FP-A3` | $P(x)=x^9$, $x=2^{-10}$ | ordinary multiplication gives exactly $2^{-90}$; the five-product chain has at least 50% relative error | FIL-C and Zig negative test |
| `FP-A4` | optimized strict-FP binary | evaluator bodies retain 8 versus 5 scalar multiplications | target `objdump` |
| `FP-A5` | benchmark rounds `500000` versus `2000000` | external wall time scales approximately 4x | target wall-clock negative check |

The exact integer expectations for `FP-A1` are:

```text
-16 -> -4109200111
-4  -> 399581
-1  -> 12310199
0   -> 34459425
1   -> 90960751
2   -> 226566107
4   -> 1191200869
16  -> 988079200561
```

## 4. Target Machine

| Item | Value |
| --- | --- |
| Host | `dc02-pe-t137-n047` |
| OS | Debian 12, Linux `5.15.152.bsk.15-amd64` |
| CPU | AMD EPYC 7Y83 |
| Topology | 2 sockets, 128 physical cores, SMT2, 256 logical CPUs |
| NUMA | 4 nodes |
| Selected CPU | CPU 31, NUMA node 0, physical core 31 |
| Idle sample after runs | 99.66% idle over three one-second samples |
| Governor | `performance` |
| Reported frequency | 2450 MHz |
| Compiler | GCC 12.2.0 |

`orthrus-cli demand fdbd:dc02:e:137::47` returned `err when kgetcred`, but
existing credentials allowed direct access through `ssh ... -J j`.

## 5. Benchmark Contract

Both binaries used:

```text
-std=c11 -O3 -march=native -mtune=native -DNDEBUG
-flto -fno-ipa-icf -Wall -Wextra -Werror
```

The strict build added `-ffp-contract=off`; the FMA build added
`-ffp-contract=fast`. Each run was pinned with:

```bash
numactl --physcpubind=31 --membind=0 \
  ./fast_polynomial_benchmark 2000000 31
```

Each displayed value is the median of 31 samples. The final table takes the
median of three complete runs. Latency mode feeds every result into the next
input. Throughput mode evaluates eight independent volatile inputs and keeps
eight accumulators. LTO inlined the evaluator into each direct loop; the
`no-tree-vectorize` function attribute kept this a scalar comparison. A
volatile sink keeps the outputs observable.

## 6. Target Results

| FP contraction | Mode | Horner ns/eval | Chain ns/eval | Chain relative to Horner |
| --- | --- | ---: | ---: | --- |
| off | dependent latency | 16.852 | 13.498 | 19.9% less time |
| off | 8-way scalar throughput | 2.837 | 3.684 | 29.9% more time |
| fast | dependent latency | 11.852 | 13.088 | 10.4% more time |
| fast | 8-way scalar throughput | 1.400 | 3.858 | 175.6% more time |

Three-run medians before aggregation:

```text
strict latency horner: 16.837, 16.856, 16.852
strict latency chain:  13.482, 13.503, 13.498
strict throughput horner: 2.835, 2.838, 2.837
strict throughput chain:  3.682, 3.687, 3.684

FMA latency horner: 11.864, 11.852, 11.847
FMA latency chain:  13.109, 13.088, 13.083
FMA throughput horner: 1.402, 1.400, 1.400
FMA throughput chain:  3.865, 3.858, 3.856
```

The strict binary's disassembly retained 8 multiply instructions per Horner
evaluation and 5 per preprocessed-chain evaluation. With FMA enabled, Horner's
eight multiply-add stages became dependent `vfmadd` instructions, while the
preprocessed chain retained substantially more standalone affine-form work.

The negative scaling check measured:

```text
500000 rounds  -> 208473188 ns
2000000 rounds -> 828300504 ns
ratio          -> 3.973x
```

This is inconsistent with elimination or hoisting of the timed loops.

`perf stat` could measure wall time but reported `<not counted>` for cycles,
instructions, branches, and branch misses, with an NMI-watchdog diagnostic.
No PMU conclusion is claimed.

## 7. Final Digests

| Artifact | SHA-256 |
| --- | --- |
| `include/fast_polynomial.h` | `f3a88b5a4df2f2c7e36be9dd4a598122c631f21dc0cbccf695be5851a9489f2e` |
| `src/fast_polynomial.c` | `722a538f21c5f1b3ca24b6e0c5cb955fece2a35fc0c947920c848ccfdae90ede` |
| `tests/fast_polynomial_test.c` | `90c70659093a2d55f2626f49c316dce426ed4f504b83921cc13a23fb0f11e9c0` |
| `src/benchmark.c` | `b2ed2187a0a704b6d404d0fc6341123cf316fd6c4746227de46dea4c5a318d48` |
| target strict benchmark | `4496dc3f4fdae3ba665a689506dd87a3fffac1aa810f15f1bc0e730ecda8c1f8` |
| target FMA benchmark | `bf917a56cd66c85136e70ef83947544e5969d9efe0b8c1ccbcbe6053487eeab4` |
