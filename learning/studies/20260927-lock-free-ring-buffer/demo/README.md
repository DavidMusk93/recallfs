# C11 SPSC Ring Buffer Reproduction

This project reproduces the article's four concurrent variants with a fixed
`uint64_t` payload:

| API family | Synchronization |
| --- | --- |
| `spsc_mutex_*` | One `pthread_mutex_t` |
| `spsc_seq_cst_*` | C11 sequentially consistent atomics |
| `spsc_acquire_release_*` | Relaxed owner loads, acquire peer loads, release stores |
| `spsc_cached_*` | Acquire/release plus endpoint-owned peer-index caches |

The ring reserves one slot. A requested capacity of 100,000 therefore stores at
most 99,999 values.

## Ownership Contract

- Atomic variants require exactly one producer and one consumer.
- One thread exclusively owns each `spsc_producer` or `spsc_consumer`.
- A successful push transfers the copied scalar value to the ring.
- A successful pop transfers it to the consumer.
- Destruction and endpoint binding require a quiescent ring.
- Cached stale indices may report full or empty conservatively. They must never
  permit overwrite or consumption of an unpublished slot.
- Creation rejects the atomic variants if C11 `atomic_size_t` is not lock-free.

## Verify

From the repository root:

```bash
learning/studies/20260927-lock-free-ring-buffer/demo/verify.sh
```

The script runs, in order:

1. FIL-C 0.684 correctness and benchmark-path self-check;
2. fixed Zig 0.16.0 builds at O0, O2, and native O3;
3. ASan/UBSan;
4. ThreadSanitizer;
5. a short native benchmark self-check.

On macOS, Zig compiles the TSan-instrumented objects and Apple Clang links only
the platform sanitizer runtime. Zig's bundled TSan runtime crashes during
startup on the recorded macOS 26.5.2 host.

Set `RUN_BENCHMARK=1` to run the article-shaped 100-million-transfer benchmark:

```bash
RUN_BENCHMARK=1 BENCHMARK_SAMPLES=5 \
  learning/studies/20260927-lock-free-ring-buffer/demo/verify.sh
```

Generated binaries remain under `.tmp/lock-free-ring-buffer/`.

## CMake

The equivalent project build is:

```bash
cmake -S learning/studies/20260927-lock-free-ring-buffer/demo \
  -B .tmp/lock-free-ring-buffer/cmake \
  -DCMAKE_C_COMPILER="$PWD/.tmp/zig/dist-macos-0.16.0/zig" \
  -DCMAKE_C_COMPILER_ARG1=cc \
  -DSPSC_CACHE_LINE_SIZE=128
cmake --build .tmp/lock-free-ring-buffer/cmake
ctest --test-dir .tmp/lock-free-ring-buffer/cmake --output-on-failure
```

The local host did not have CMake installed, so the retained evidence uses the
direct compiler commands from `verify.sh`.

## Benchmark Semantics

Defaults are 10,000,000 transfers, 100,000 physical slots, and 11 rotated
samples. One transfer is one successful push plus one successful pop. The CSV
reports both `transfers_per_second` and
`successful_api_calls_per_second = 2 * transfers_per_second`.

Every consumed value is checked against an independent increasing-sequence
oracle, the checksum remains observable under `-DNDEBUG`, and failed attempts
are counted. Allocation and thread creation are outside the timed interval.
The timer starts only after the consumer signals readiness.

On Apple platforms both threads request `QOS_CLASS_USER_INTERACTIVE`, but this
is not CPU pinning. On Linux the current harness is also unpinned. Do not use
these results as cross-machine latency or topology claims.

The cache-line size is a build contract:

```bash
-DSPSC_CACHE_LINE_SIZE=128
```

Use the target's destructive-interference size. The default 64-byte value is
not valid for every CPU; Apple M1 Pro reports 128 bytes.
