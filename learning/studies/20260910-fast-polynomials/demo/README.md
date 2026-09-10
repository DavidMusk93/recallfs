---
doc_id: study-fast-polynomials-demo-20260910
kind: implementation-guide
status: active
authority: executable
applies_to:
  - learning/studies/20260910-fast-polynomials/demo
depends_on:
  - learning/studies/20260910-fast-polynomials/source.md
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c
  - learning/studies/20260910-fast-polynomials/evidence/README.md
---

# Fast Polynomial Evaluation C Demo

This C11 project compares Horner evaluation with the five-product chain emitted
for the degree-9 reverse Bessel polynomial by the upstream compiler.

It also includes a deliberate numerical counterexample: the exact rational
five-product chain for $x^9$ loses relative accuracy near zero because nonzero
intermediates must cancel to recover zero low-order coefficients.

## Layout

| Path | Role |
| --- | --- |
| `include/fast_polynomial.h` | Public evaluator declarations and operation-count anchors |
| `src/fast_polynomial.c` | Horner and preprocessed-chain implementations |
| `tests/fast_polynomial_test.c` | Exact values, bounded sweep, and cancellation negative test |
| `src/benchmark.c` | Dependent-latency and eight-way scalar-throughput harness |

## FIL-C Correctness

From the repository root:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260910-fast-polynomials/demo/include \
  learning/studies/20260910-fast-polynomials/demo/src/fast_polynomial.c \
  learning/studies/20260910-fast-polynomials/demo/tests/fast_polynomial_test.c \
  -lm -o .tmp/fast-polynomial-test

.tmp/fil-c/bin/filrun .tmp/fast-polynomial-test
```

Expected shape:

```text
correctness passed: 8 exact anchors, 8193-point sweep;
max relative error horner=<bounded> chain=<bounded>;
x^9 cancellation relative error=1.000e+00
```

The final observed values are recorded in `../evidence/README.md`.

## CMake Form

```bash
cmake \
  -S learning/studies/20260910-fast-polynomials/demo \
  -B .tmp/fast-polynomials-filc-cmake \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$PWD/.tmp/fil-c/bin/filcc" \
  -DFIL_RUNNER="$PWD/.tmp/fil-c/bin/filrun" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build .tmp/fast-polynomials-filc-cmake -j
ctest --test-dir .tmp/fast-polynomials-filc-cmake --output-on-failure
```

## Native Benchmark

FIL-C timings are not performance evidence. After correctness passes, build
natively on the target machine:

```bash
gcc \
  -std=c11 -O3 -march=native -mtune=native -DNDEBUG \
  -flto -fno-ipa-icf -ffp-contract=off \
  -Wall -Wextra -Werror \
  -I include src/fast_polynomial.c src/benchmark.c -lm \
  -o fast_polynomial_benchmark_strict

numactl --physcpubind=31 --membind=0 \
  ./fast_polynomial_benchmark_strict 2000000 31
```

Repeat with `-ffp-contract=fast` because FMA changes both code generation and
the result. The benchmark alternates method order, validates the eight inputs
before timing, keeps outputs through a volatile sink, and uses direct calls so
LTO can inline the evaluator into each loop.

The default arguments are one million rounds and 31 samples. `samples` must be
odd and at most 101.

## What the Demo Does Not Prove

- It does not validate the general coefficient decoder.
- It does not benchmark finite-field multiplication.
- It does not establish a portable speedup.
- Its $[-2,2]$ error result applies only to the selected reverse Bessel
  polynomial and binary64 implementation.
- The $x^9$ negative test demonstrates a failure mode, not a universal error
  magnitude for every preprocessed polynomial.
