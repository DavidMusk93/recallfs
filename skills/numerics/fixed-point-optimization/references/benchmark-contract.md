# Fixed-Point Benchmark Contract

Fixed-point optimization is target- and consumer-dependent. A faster arithmetic
primitive can lose after conversion, range checks, wider accumulators, or
memory traffic are included.

## 1. Measurement Question

Choose the primary question:

| Question | Benchmark shape |
| --- | --- |
| One result feeds the next | Serial dependency latency |
| Many independent values | Scalar throughput with independent chains |
| Batch/vector workload | SIMD/vector throughput |
| Representation change | End-to-end conversion + compute + output |
| Model quantization | Accuracy, model bytes, bandwidth, kernel latency |
| Embedded/DSP | cycles, energy, saturation/error quality |

Do not combine latency and throughput into one number.

## 2. Baseline

Keep the clearest correct implementation:

- ordinary `/` or `%`;
- floating-point reference;
- framework/library baseline;
- existing production representation.

Build baseline and candidate with identical:

- compiler and version;
- optimization, target, LTO, and assertion flags;
- input data and distribution;
- batch size and concurrency;
- affinity and NUMA placement;
- output materialization.

## 3. Correctness Gate

Before native measurement:

1. run exhaustive or property/differential tests;
2. test all domain boundaries and first-invalid values;
3. test rounding, saturation, and overflow behavior;
4. verify range guards;
5. for C, compile and execute through FIL-C;
6. retain always-on result validation under `-DNDEBUG`.

FIL-C timing is never performance evidence.

## 4. Anti-Optimization Contract

The benchmark must make the result observable without distorting the compared
critical path.

Use:

- separate no-inline benchmark loop symbols;
- independent accumulators for throughput;
- a dependency carry for latency;
- compiler barriers when all output fields must materialize;
- a volatile or externally observable final sink;
- runtime counts/rounds;
- deterministic generated inputs;
- a validation pass outside the timed region.

Reject:

- repeated identical work whose intermediate rounds can be removed;
- outputs that consume only one field when the API returns several;
- a single checksum dependency in a throughput benchmark;
- auto-vectorization when claiming scalar throughput;
- disabled vectorization when claiming production vector performance.

## 5. Negative Checks

Use at least one:

- optimized disassembly proves the intended instructions and loop backedges;
- multiplying runtime rounds by `N` multiplies wall time by approximately `N`;
- removing the observable sink causes the work to disappear, proving the sink
  matters;
- a deliberately incorrect candidate trips the always-on validator;
- vectorization report matches the intended scalar/vector mode.

Inspect the final binary, not an intermediate object built with different
flags.

## 6. Target Build

For native C benchmarks on the target machine, start with:

```text
-O3 -march=native -mtune=native -DNDEBUG
```

Enable LTO only when supported and stable. Record the complete flags rather than
writing "release build".

Record:

- host and OS/kernel;
- CPU model, sockets, cores, SMT, NUMA;
- pinned CPU and sibling;
- frequency governor and boost state;
- compiler/linker versions;
- source and binary digests;
- sample count, rounds, warmup, and workload size.

## 7. Counter Evidence

Cross-check wall time with:

- task clock;
- cycles and instructions;
- IPC;
- branches and misses;
- cache/TLB misses when relevant;
- context switches and CPU migrations;
- page faults;
- energy counters for embedded/mobile workloads when available.

If PMU counters cannot be scheduled:

- record the exact reason;
- do not report cycles, IPC, or inferred port pressure as measured;
- retain topology, wall time, software counters, and disassembly.

## 8. Representation Costs

Include costs outside the arithmetic kernel when the proposed representation
changes:

- encode/decode;
- calibration;
- coefficient generation;
- divider descriptor generation;
- input range checks;
- saturation/clipping;
- wider intermediate/accumulator traffic;
- format conversion at API boundaries;
- compile/setup latency;
- model/cache footprint.

Precomputation only wins after enough reuse. Report the break-even count.

## 9. Workload Distribution

Record:

- minimum/maximum and histogram;
- positive/negative mix;
- zeros, powers of two, and boundary frequency;
- divisor or scale distribution;
- batch sizes;
- alignment;
- scalar/vector tail fraction;
- cache residency;
- real production sample source.

Magic-division instruction counts can vary by divisor. Quantization error can
vary by channel. Average-only inputs hide these effects.

## 10. Compare

Report:

| Metric | Baseline | Candidate | Delta | Variation |
| --- | ---: | ---: | ---: | ---: |
| latency/value | | | | |
| throughput/value | | | | |
| instructions/value | | | | |
| cycles/value | | | | |
| bytes/value | | | | |
| setup/compile cost | | | | |
| max/mean error | | | | |
| product metric | | | | |

Do not subtract a noisy harness baseline from nanosecond operations unless the
subtraction method is validated.

## 11. Decision Rules

Accept when:

- correctness and error gates pass;
- the final binary contains the intended transform;
- gains exceed run-to-run noise;
- production metrics improve;
- setup and conversion costs amortize;
- no unacceptable accuracy, overflow, or maintenance regression occurs.

Reject when:

- compiler baseline is equal or faster;
- gains exist only in isolated arithmetic;
- range checks erase the gain;
- vector tails dominate;
- p99 or energy regresses;
- accuracy/calibration budget fails;
- codegen depends on undocumented compiler behavior;
- the improvement is smaller than system noise.

## 12. Evidence Template

```text
Target:
Compiler:
Flags:
Source digest:
Binary digest:
CPU/NUMA pin:
Frequency policy:
Inputs:
Samples/rounds:
Correctness gate:
Codegen proof:
PMU availability:

Latency:
Throughput:
Product metric:
Error metric:
Setup/break-even:

Decision:
Rollback:
Residual risk:
```
