# Fixed-Point Opportunity Map

Use this map to distinguish a real optimization opportunity from a numeric code
smell.

## 1. Discovery Signals

### Profile signals

- integer divide/remainder is visible in top-down, PMU, or sampled profiles;
- a unit conversion appears in a high-frequency trace;
- float/int conversion dominates a bounded numeric loop;
- vectorization stops at integer division;
- memory bandwidth limits a float tensor workload;
- a hardware counter or sensor scale is applied per event;
- a setup-time divisor or scale is reused across a large batch.

### Code signals

- `/ constant` or `% constant` inside a hot loop;
- `/ divisor` where `divisor` is loop-invariant but runtime-known;
- repeated `ticks * numerator / denominator`;
- fixed coefficients in filters, interpolation, transforms, or control;
- repeated `floor(x * scale)`, `round(x * scale)`, or unit normalization;
- hash/shard mapping through `% bucket_count`;
- integer tensors with scale and zero point;
- repeated modular reduction under one modulus.

Code signals without profile evidence are candidates for measurement, not
permission to rewrite.

## 2. Application Matrix

| Domain | Typical shape | Value mechanism | Evidence required |
| --- | --- | --- | --- |
| Clock/timebase conversion | `counter * mult >> shift` | Avoid repeated general division | Wrap interval, drift/error bound, kernel/driver call rate |
| Unit conversion | rational multiply-shift | Hoist fixed ratio and rounding | Unit contract, max value, accumulated error |
| Compile-time division | `/ const`, `% const` | Compiler reciprocal transform | Production assembly; no manual rewrite if equivalent |
| Runtime repeated division | divider descriptor | Amortize multiplier/add/shift generation | Batch size, divisor reuse count, libdivide benchmark |
| DSP/filter/control | Q7/Q15/Q31 MAC | Integer DSP/SIMD and deterministic scale | Signal range, headroom, saturation, SNR/error budget |
| Graphics/interpolation | subpixel fixed coordinates | Stable resolution and vectorizable affine math | Coordinate range, zoom, clipping, raster determinism |
| ML quantization | `(q-z)*scale` | Reduce model size/bandwidth; integer accelerator | Calibration data, accuracy metrics, per-axis scale |
| Bucket/range mapping | high-half multiply | Avoid scalar/vector modulo in mapping | Distribution, bias tolerance, rejection requirement |
| Modular arithmetic | Barrett/Montgomery | Reuse modulus-specific precomputation | Proof, constant-time audit, big-integer tests |
| Decimal quantities | integer with `10^D` scale | Exact decimal and reproducibility | Unit/version schema, overflow, rounding policy |

## 3. Opportunity Classes

### Q-format representation

Good fit:

- bounded values;
- stable fractional resolution;
- frequent add/multiply;
- target has integer MAC/DSP support;
- deterministic integer semantics matter.

Bad fit:

- dynamic range spans many orders of magnitude;
- frequent division by changing values;
- error budget is tight and input-dependent;
- hardware floating point is already faster;
- conversion at API boundaries dominates.

### Multiply-shift unit conversion

Good fit:

- ratio is constant or setup-time;
- conversion executes per sample/event;
- error can be bounded over the valid domain;
- a widened product is cheap.

Examples:

- cycles to nanoseconds;
- samples to time;
- pixels to fixed subpixels;
- sensor raw units to engineering units;
- normalized integer coordinates.

### Constant or repeated division

Compile-time divisor:

- write clear division first;
- inspect the optimized binary;
- hand-roll only if a narrower domain creates a better proven sequence.

Runtime divisor:

- precompute only when reused;
- use a generated divider descriptor;
- include setup cost in the benchmark;
- prefer libdivide or an equivalent proven implementation.

### Multiply-high range mapping

Good fit:

- source is a full-width unsigned integer;
- target range satisfies the proven multiply-high preconditions, including
  `1 <= range <= 2^w - 1` for the ordinary `w`-bit form;
- slight finite-domain imbalance is acceptable, or rejection is added;
- vector multiply-high is available.

Bad fit:

- zero or wider-than-source-cardinality range without a separate contract;
- strict unbiased output with no rejection;
- signed range semantics are unclear;
- source distribution is not uniform and quality depends on it;
- security requires constant-time behavior not yet reviewed.

### Quantized tensor arithmetic

Good fit:

- memory bandwidth or accelerator support dominates;
- calibration data represents production;
- operator quantization contracts are available;
- accuracy loss has a product budget.

Bad fit:

- sensitive regression output;
- calibration distribution is unknown;
- fallback float operators break the integer pipeline;
- scale conversion overhead dominates small batches.

### Modular reduction

Good fit:

- one modulus is reused;
- a reviewed implementation exists;
- exact and constant-time contracts are explicit.

Do not derive cryptographic reduction from an application-level approximate
reciprocal. Use a reviewed library.

## 4. False Positives

| Observation | Why it is not enough |
| --- | --- |
| Source contains `/ 60` | Compiler may already use reciprocal multiply |
| Source contains `% n` | Modulo may not be hot; mapping semantics may differ |
| Float appears in a loop | FPU/SIMD may be the fastest path |
| Values are bounded | Resolution and accumulated error may still fail |
| MCU has no FPU | Library Q-format and saturation semantics still need proof |
| Integer output matches samples | Samples do not prove quotient boundaries |
| Smaller type reduces memory | Requantization and accuracy may dominate |
| Multiply-high removes divide | It may be off by one or biased |
| Decimal values use integers | This may be semantic correctness, not speed |

## 5. Cross-Language Notes

### C and C++

- Integer promotion may widen Q7/Q15 products, but storing early can narrow
  them again.
- Signed right shift semantics and overflow assumptions must match the target
  language/compiler contract.
- Use explicit wide types for products and accumulators.
- Validate C through FIL-C before native performance builds in RecallFS.

### Rust

- Use `u64`/`u128` widening and checked conversions.
- Choose `checked_*`, `saturating_*`, `wrapping_*`, or ordinary arithmetic
  intentionally.
- Do not hide release-mode wrapping assumptions behind debug-only panics.
- Inspect LLVM codegen; clear division may already compile optimally.

### JVM/managed runtimes

- JIT specialization can change after warmup.
- Include warmup, deoptimization, and vector API behavior.
- Avoid comparing cold JIT division with warmed fixed-point code.

### GPU/accelerator

- Throughput may be bandwidth- or occupancy-bound rather than ALU-bound.
- Quantized speedup depends on native integer kernels and avoiding fallback
  operators.
- Include host/device conversion and transfer costs.

## 6. Production References

- Linux clocksource timekeeping:
  <https://www.kernel.org/doc/Documentation/timers/timekeeping.rst>
- CMSIS-DSP fixed-point types:
  <https://github.com/ARM-software/CMSIS-DSP/blob/main/Include/arm_math_types.h>
- TensorFlow Lite int8 quantization:
  <https://github.com/tensorflow/tensorflow/blob/master/tensorflow/lite/g3doc/performance/quantization_spec.md>
- Faster Remainder by Direct Computation:
  <https://lemire.me/en/publication/arxiv190201961/>
- libdivide:
  <https://libdivide.com/>
