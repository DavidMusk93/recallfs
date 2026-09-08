# Fast Time-of-Day C Demo

This C11 project compares five ways to convert an unsigned count of seconds
into `{hour, minute, second}`. The optimized variants assume the input is within
their documented range; every variant is valid for the intended day-second
domain `[0, 86399]`.

## Algorithms

| Name | Purpose |
| --- | --- |
| `traditional` | Sequential division/remainder baseline |
| `parallel_div` | V1 dependency-chain reordering with ordinary division |
| `parallel_fixed` | V1 with 32-bit fixed-point reciprocals |
| `hi_low` | V2 fixed-point quotient and fractional low bits |
| `base64` | V3 modulo-60 to modulo-64 transformation |

## Correctness with FIL-C

From the repository root:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I learning/studies/20260908-fast-time-of-day/demo/include \
  learning/studies/20260908-fast-time-of-day/demo/src/fast_time.c \
  learning/studies/20260908-fast-time-of-day/demo/tests/fast_time_test.c \
  -o .tmp/fast-time-of-day-test

.tmp/fil-c/bin/filrun .tmp/fast-time-of-day-test
```

Expected output:

```text
FIL-C correctness passed: 5 variants, exhaustive declared ranges
```

The test checks every value in `[0, 86399]`, every value in each restricted
variant's larger claimed range, selected full-`uint32_t` values for both
division variants, and the first expected fixed-point mismatch.

The CMake form keeps the same FIL-C boundary explicit:

```bash
cmake \
  -S learning/studies/20260908-fast-time-of-day/demo \
  -B .tmp/fast-time-of-day-cmake \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$PWD/.tmp/fil-c/bin/filcc" \
  -DFIL_RUNNER="$PWD/.tmp/fil-c/bin/filrun" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build .tmp/fast-time-of-day-cmake -j
ctest --test-dir .tmp/fast-time-of-day-cmake --output-on-failure
```

## Native Benchmark

FIL-C timings are not performance evidence. Build the benchmark natively on
the target CPU after the FIL-C correctness gate:

```bash
cc \
  -std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto \
  -Wall -Wextra -Werror \
  -I learning/studies/20260908-fast-time-of-day/demo/include \
  learning/studies/20260908-fast-time-of-day/demo/src/fast_time.c \
  learning/studies/20260908-fast-time-of-day/demo/src/benchmark.c \
  -o .tmp/fast-time-of-day-benchmark

.tmp/fast-time-of-day-benchmark
```

The benchmark validates every generated input before timing. Latency mode
feeds all three result fields into the next input. Throughput mode uses eight
independent checksum chains and disables auto-vectorization. A volatile sink
keeps final results observable even with `-DNDEBUG`.

Run it on an otherwise idle machine, pin it to one physical CPU where the
operating system permits that, and record the compiler, flags, topology,
frequency policy, source digest, binary digest, repeats, wall time, and
available hardware counters. Inspect optimized assembly to ensure the timed
loops still contain the intended conversion work.
