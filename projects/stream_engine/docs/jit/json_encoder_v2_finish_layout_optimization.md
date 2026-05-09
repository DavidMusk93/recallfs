# JSON Encoder V2 Finish/Layout Optimization Notes

## Goal

This note records the optimization path that moved `json_v2` from "finish is the dominant bottleneck"
to a state where the `both` benchmark reaches about `2.20x` speedup over legacy.

For the full optimization inventory, including later kernel-side work, retained optimizations,
reverted experiments, and the final `3x` stability validation, see:

- [json_encoder_v2_optimization_journal.md](file:///root/Documents/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md)

The focus is not generic JIT theory. It is the concrete lesson from this round:

- do not keep polishing `memcpy` in isolation
- change the output layout first
- then remove unnecessary gather-pack work in `finish()`

Relevant code:

- [jit_runtime.h](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h)
- [jit_runtime.cpp](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp)
- [json_encoder_benchmark.cpp](file:///root/Documents/stream_engine/src/test/arrow_encdec/json_encoder_benchmark.cpp)

## Starting Point

After the first `RowPagedBuffer` rewrite, `json_v2` had already escaped the `std::vector<char>` bottleneck,
but `finish()` was still expensive.

The observed profile shape was:

- `build_offsets_ms` was small
- `copy_values_ms` dominated `finish_ms`
- `overflow_rows=0` on the benchmark workload

That meant the real issue was not offset computation and not overflow fragmentation.
It was this:

- rows were written into a sparse per-row arena
- `finish()` still had to gather each row into a new contiguous Arrow values buffer

So the hot path was effectively:

```text
RowPagedBuffer sparse arena -> finish gather-pack -> Arrow BinaryArray values
```

## Failed Directions

Several local optimizations did not produce stable gains:

- hand-written small copy path
- prefetch-only variants
- block prefetch loop
- copy metadata staging

The reason is simple:

- they still preserved the same overall shape
- source remained sparse row slots
- destination was still a newly allocated contiguous values buffer

In other words, they changed the copy instruction mix, but not the dataflow.

## What Actually Worked

### 1. Make row reserve data-driven

The first step was to shrink the row stride itself.

Before this round, `RowPagedBuffer::reset()` used a fixed `bytesPerRow` default.
That made the arena sparse even when the actual average JSON row was much smaller.

The fix was to estimate per-row reserve from real batch data:

- schema punctuation and key overhead
- sampled formatted width for numeric columns
- `total_values_length()` based average for string/binary columns
- null-aware averaging

Implementation:

- [sampleFormattedAverageBytes](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L348-L374)
- [estimateRowReserveBytes](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L376-L534)
- [CompiledKernel::execute](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1965-L1977)

This changed the benchmark reserve from roughly `512` bytes per row to `448` bytes per row.

Why it matters:

- smaller stride improves source locality
- tighter arena reduces the distance between adjacent rows during `finish()`
- tighter reserve also increases the chance that `overflow_rows` stays at zero

### 2. Keep `overflow_rows == 0` as the fast-path contract

The benchmark showed that after better reserve estimation, the common workload stayed fully inside
the main arena:

- `overflow_rows=0`
- `overflow_bytes=0`

This was the key enabling condition.

It meant the main arena was not just a temporary write scratch space.
It could become the final values storage if we packed it in place.

Relevant buffer interface:

- [RowPagedBuffer](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.h#L20-L105)
- [reset](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1626-L1646)
- [growSlot](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1648-L1665)

### 3. Replace "allocate new values buffer + gather copy" with `inplace_pack`

This was the decisive optimization.

The new `finish()` fast path does the following when all rows are still in arena:

1. build offsets as before
2. skip `AllocateBuffer(totalBytes)` for values
3. compact rows inside the existing arena with `memmove`
4. wrap the packed arena into an Arrow-owned buffer
5. build `BinaryArray` directly from that packed arena

Implementation:

- default mode selection: [finishCopyMode](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L601-L625)
- Arrow buffer ownership wrapper: [OwnedArenaBuffer](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L677-L688)
- in-place pack path in `finish()`: [BatchFormatter::finish](file:///root/Documents/stream_engine/src/sql/encdec/json_v2/jit_runtime.cpp#L1782-L1955)

The important structural change is:

```text
before:
  sparse arena -> allocate values buffer -> gather copy -> BinaryArray

after:
  sparse arena -> in-place pack -> BinaryArray
```

This does not remove all movement.
Rows still need to be compacted.
But it removes:

- one `values` allocation
- one "copy into a different destination buffer" step
- one layer of ownership transfer complexity

### 4. Keep safe fallback for overflow cases

`inplace_pack` is only correct and worthwhile when rows remain in the arena.

So the implementation explicitly keeps fallback logic:

- if `overflow_rows > 0`, do not use `inplace_pack`
- fall back to the normal copy path

That keeps the fast path aggressive but makes correctness simple.

## Benchmark Result

### Before `inplace_pack`

`json_v2-only`, `10s`, `memcpy_prefetch`:

```text
rows_per_sec=819085
finish_ms=1529.482
alloc_values_ms=1.787
copy_values_ms=1503.453
```

### After `inplace_pack`

`json_v2-only`, `10s`, default mode:

```text
rows_per_sec=947047
finish_ms=409.762
alloc_values_ms=0.000
copy_values_ms=380.248
copy_mode=inplace_pack
```

### End-to-end result

`both`, `20s`:

```text
legacy rows_per_sec=583097
json_v2 rows_per_sec=1.28431e+06
json_v2_speedup=2.20256x
```

## Why This Round Worked

The main lesson is that the winning optimization changed the pipeline shape, not just the local instruction.

The sequence was:

1. replace `std::vector<char>` row formatter with `RowPagedBuffer`
2. measure `finish()` in detail
3. confirm the real hotspot is `copy_values`
4. confirm overflow is not the dominant factor on this workload
5. shrink row stride with data-driven reserve estimation
6. eliminate the extra values-buffer allocation and gather destination through `inplace_pack`

This is a good example of a more general rule:

- if a stage is bandwidth-bound, first ask whether the stage should exist in its current form
- only then consider micro-optimizing the implementation of that stage

## Practical Rules For Future Work

### Rule 1: Do not treat `finish()` as "just a copy"

`finish()` is really the last layout conversion stage.
If layout is wrong, `finish()` becomes expensive regardless of how clever the copy loop is.

### Rule 2: Preserve observability

The detailed `finish` profile was necessary to make the right decision.
Keep these metrics:

- `alloc_offsets_ms`
- `build_offsets_ms`
- `alloc_values_ms`
- `copy_values_ms`
- `arena_rows`
- `overflow_rows`
- `row_reserve_bytes`

Without them, it is too easy to chase the wrong optimization.

### Rule 3: Fast path should be conditional, not universal

`inplace_pack` is excellent when:

- reserve estimation is accurate
- overflow is absent or rare

It should remain a fast path with explicit fallback, not a blind assumption.

### Rule 4: Layout work beats instruction work

This round reinforced a strong priority order:

1. execution shape
2. memory layout
3. data movement stages
4. instruction-level tuning

That ordering matters more than whether a loop uses prefetch or hand-written copy intrinsics.

## Reproduction

Build:

```bash
env DISABLE_CAS=1 blade build //src/test:json_encoder_benchmark
```

Profile `json_v2` only:

```bash
env DISABLE_CAS=1 \
  JSON_ENCODER_BENCH_MODE=json_v2 \
  JSON_ENCODER_BENCH_MIN_SECONDS=10 \
  TIDE_JSON_V2_PROFILE=1 \
  ./build64_release/src/test/json_encoder_benchmark
```

End-to-end compare:

```bash
env DISABLE_CAS=1 \
  JSON_ENCODER_BENCH_MODE=both \
  JSON_ENCODER_BENCH_MIN_SECONDS=20 \
  ./build64_release/src/test/json_encoder_benchmark
```

## Bottom Line

The key breakthrough of this round was not a better formatter API and not a faster `memcpy`.

It was:

- estimate a tighter row layout
- keep writes inside one arena
- convert `finish()` from "copy into another buffer" into "pack current arena and hand it off"

That is the experience worth keeping for future encoder work.
