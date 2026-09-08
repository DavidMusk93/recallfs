# FIL-C Evidence

## Toolchain

```text
clang version 20.1.8
Fil-C 0.684
target=aarch64-unknown-linux-gnu
```

## Proof-First Failure

The test and API declarations were created before the implementation. The
initial link failed with the expected missing symbols:

```text
undefined reference to `pizlonated_hms_traditional'
undefined reference to `pizlonated_hms_parallel_div'
undefined reference to `pizlonated_hms_parallel_fixed'
undefined reference to `pizlonated_hms_hi_low'
undefined reference to `pizlonated_hms_base64'
exit_status=1
```

## Correctness Command

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

## Executed Benchmark Path

The benchmark executable was separately built and run with FIL-C:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260908-fast-time-of-day/demo/include \
  learning/studies/20260908-fast-time-of-day/demo/src/fast_time.c \
  learning/studies/20260908-fast-time-of-day/demo/src/benchmark.c \
  -o .tmp/fast-time-of-day-benchmark-filc

.tmp/fil-c/bin/filrun .tmp/fast-time-of-day-benchmark-filc \
  --count 64 --rounds 2 --samples 2
```

The run validated all generated inputs, exercised both benchmark modes for all
five algorithms, and produced a nonzero observable sink:

```text
sink=9980056299585968252
```

FIL-C timing values are intentionally not retained as performance evidence.
