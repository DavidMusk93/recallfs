---
name: "fixed-point-optimization"
description: "Finds and validates fixed-point opportunities in hot numeric code. Invoke for scaling, division/modulo, DSP/quantization, range or modular mapping, decimal integers, or codegen tuning."
---

# Fixed-Point Optimization

Use this skill to turn a measured numeric hot path into an integer transform
with explicit scale, range, error, overflow, and performance contracts.

Do not start from a magic multiplier. Start from the workload and semantics.

## 1. Required Outcome

Produce:

1. the measured hot path and baseline;
2. the fixed-point opportunity class;
3. the exact input domain and output semantics;
4. the derived transform and proof obligations;
5. correctness evidence across the claimed domain;
6. optimized code-generation evidence;
7. target-machine latency, throughput, and resource evidence;
8. rejected alternatives and rollback conditions.

## 2. Trigger

Invoke this skill when code or a profile shows:

- repeated integer division or modulo by one divisor;
- repeated conversion between counters, time units, samples, coordinates, or
  rates;
- floating-point multiply/add in a bounded hot loop;
- DSP, control, interpolation, or graphics work on bounded values;
- quantized or integer-only inference;
- mapping an integer into buckets, shards, tiles, or ranges;
- repeated modular reduction;
- decimal scaled values where deterministic representation matters;
- missing SIMD integer division while vector multiply/high-shift is available;
- a compiler-generated division sequence that remains on the critical path.

The presence of `/`, `%`, `float`, or a constant alone is not a trigger. Require
hot-path or domain evidence.

## 3. Do Not Conflate the Mechanisms

| Mechanism | General form | Main purpose |
| --- | --- | --- |
| Q-format representation | `real ~= integer / 2^F` | Store and compute bounded fractional values |
| Multiply-shift transform | `y ~= scale_round(x, M, S, mode) + offset` | Unit conversion, scaling, affine transform |
| Multiply-high/low extraction | `product = x * reciprocal` | Quotient, remainder progress, range position |
| Divider descriptor | multiplier + add flag + shift | Repeated division by a runtime divisor |
| Modular reduction | Barrett/Montgomery-style transform | Repeated arithmetic under one modulus |
| Decimal scaled integer | `stored = parse_decimal(source, D, mode)` | Exact decimal semantics and determinism |

Time-of-Day combines several mechanisms. It does not make every fixed-point
problem a reciprocal-division problem.

## 4. Workflow

```text
+---------------------------+
| profile the real consumer |
+-------------+-------------+
              |
              v
+---------------------------+
| classify numeric pattern  |
| Q / scale / div / range   |
+-------------+-------------+
              |
              v
+---------------------------+
| define semantic contract  |
| range / error / rounding  |
+-------------+-------------+
              |
              v
+---------------------------+
| derive integer transform  |
| prefer compiler/library   |
+-------------+-------------+
              |
              v
+---------------------------+
| prove and test domain     |
| width / overflow / bias   |
+-------------+-------------+
              |
              v
+---------------------------+
| inspect optimized codegen |
+-------------+-------------+
              |
              v
+---------------------------+
| benchmark target workload |
+-------------+-------------+
              |
        no    |    yes
       +------+------+
       |             |
       v             v
+-------------+  +----------------+
| reject/rework|  | retain + guard |
+-------------+  +----------------+
```

### Step 1: Establish the baseline

Record:

- exact caller and hot loop;
- input distribution and valid domain;
- divisor/scale lifetime: compile-time, setup-time, per batch, or per element;
- scalar versus vector workload;
- dependency-chain latency versus independent throughput;
- current compiler, flags, ISA, and generated instructions;
- product metric: throughput, p50/p99 latency, CPU, energy, memory bandwidth, or
  determinism.

If the path is not measured or cannot be tied to a product metric, stop. A
fixed-point rewrite is not justified.

### Step 2: Classify the opportunity

Read [`references/scenario-map.md`](references/scenario-map.md). Select one
primary class:

| Signal | Primary class |
| --- | --- |
| Bounded fractional state repeatedly added/multiplied | Q-format |
| Stable rational conversion between units | Multiply-shift |
| Constant quotient/remainder | Compile-time reciprocal division |
| Runtime divisor reused across many numerators | Divider descriptor |
| Uniform integer mapped to `[0, range)` | Multiply-high range map |
| Convolution/filter/control with bounded tensors | Quantized/Q-format MAC |
| Repeated same-modulus arithmetic | Modular reduction |
| Exact decimal quantity | Decimal scaled integer |

Do not combine classes until each one has a separate semantic contract.

### Step 3: Run the rejection gate

Reject or defer when:

- the path is cold;
- the current compiler already emits an equivalent or better instruction
  sequence;
- the divisor or scale changes too often to amortize precomputation;
- target floating-point or vector hardware wins on the real workload;
- the input range is unknown or cannot be enforced;
- rounding mode and overflow behavior are unspecified;
- required accuracy exceeds the available fixed-point resolution;
- a maintenance-critical public API would expose unexplained magic constants;
- strict unbiased, bit-exact, or constant-time behavior has not been proven;
- the optimization only improves a microbenchmark while regressing p99,
  memory bandwidth, energy, or production throughput.

### Step 4: Define the numeric contract

Before implementation, write:

| Field | Required statement |
| --- | --- |
| Input domain | inclusive bounds, signedness, distribution |
| Mathematical result | exact formula before approximation |
| Representation | scale, zero point, fraction bits, unit |
| Rounding | truncation, floor, nearest-even, ties-away, stochastic |
| Overflow | checked, widen, saturate, wrap, trap |
| Error | absolute/relative/ULP bound and accumulation budget |
| Output domain | inclusive bounds and sentinel behavior |
| Portability | word size, endianness if serialized, shift semantics |
| Security | constant-time and side-channel requirements |

Read [`references/proof-contract.md`](references/proof-contract.md) and fill
every applicable proof obligation.

### Step 5: Prefer an existing implementation owner

Use this order:

1. **Compiler-generated constant division.** Write clear `/` or `%`, compile
   with production flags, and inspect codegen.
2. **Established library or framework.**
   - Runtime repeated integer division: `libdivide` or an equivalent proven
     divider descriptor.
   - DSP/control: CMSIS-DSP or the target platform's fixed-point library.
   - ML quantization: the framework's quantization/calibration pipeline.
   - Cryptography: a reviewed constant-time big-integer/modular library.
3. **Local helper with generated constants.** Keep the generator and proof
   inputs near the helper.
4. **Hand-derived special case.** Use only when a narrower domain or unusual
   consumer permits a measurably shorter sequence that the compiler/library
   cannot express.

Never paste a magic constant without its divisor/scale, word width, rounding
rule, valid domain, generator/proof, and tests.

### Step 6: Prove and test

Required categories:

- zero, one, minimum, maximum;
- transition points around every quotient/rounding boundary;
- exact halfway cases for the selected rounding mode;
- largest positive and negative intermediate;
- first value outside a restricted domain;
- signed zero and negative operands when signed arithmetic exists;
- saturation/trap/wrap behavior;
- exhaustive testing when the domain is tractable;
- property tests and a trusted wide/reference implementation otherwise;
- uniformity/bias test for range mapping;
- compiler-independent expected outputs.

For C, compile and run correctness tests with the repository FIL-C toolchain
before any native performance build. FIL-C evidence covers exercised paths; it
does not prove mathematical completeness or performance.

For restricted reciprocal division, include at least one known failure outside
the valid range. For generic runtime division, test a divisor that cannot use a
plain multiply-high sequence.

### Step 7: Inspect optimized code

Build with the exact production compiler, target, and optimization flags.
Record:

- source and binary digest;
- function symbol or loop boundaries;
- multiply, high-half multiply, shift, add, branch, divide, and conversion
  instructions;
- vector width and lane count;
- whether loops were merged, exchanged, hoisted, vectorized, or deleted;
- whether the output contract forced all fields/results to materialize.

Do not claim an optimization from source shape alone. Constant division is
commonly strength-reduced automatically.

### Step 8: Benchmark the target workload

Read [`references/benchmark-contract.md`](references/benchmark-contract.md).
At minimum:

- run baseline and candidate on the target machine;
- use native production flags;
- pin CPU/NUMA where applicable;
- separate serial latency from independent throughput;
- retain result validation and an observable sink under `-DNDEBUG`;
- record repeated samples and variation;
- cross-check wall time with hardware counters when available;
- state why PMU evidence is unavailable when it is unavailable;
- inspect final codegen or run a negative scaling test to exclude DCE/hoisting;
- compare product metrics, not only ns/op.

### Step 9: Make the decision

Keep the change only when:

- correctness and error contracts hold;
- the target binary contains the intended work;
- the measured result is stable and material;
- precomputation, conversion, and wider-intermediate costs are included;
- call-site complexity and maintenance cost are acceptable;
- the input contract can be enforced;
- rollback is clear.

Otherwise keep the baseline and record the failed hypothesis.

## 5. Important Counterexamples

### Plain multiply-high is not generic division

For unsigned 32-bit division by 7:

```text
d = 7
m = ceil(2^32 / 7) = 613566757
x = 3724842645

high32(x * m) = 532120378
floor(x / 7)  = 532120377
```

General reciprocal division may need a different multiplier width, an add
correction, and a post-shift. Treat them as one generated descriptor.

### Q15 does not automatically overflow in C

On common CMSIS targets, `q15_t` is `int16_t` and integer promotion evaluates
`q15_t * q15_t` in 32-bit `int`; every Q15 product fits. The contract is:

- Q15 product: retain at least 32 bits;
- Q31 product: retain at least 64 bits;
- never narrow before rounding/rescale;
- prove the accumulator width separately from one product.

### Multiply-high is not automatically unbiased

Mapping a uniform `w`-bit integer with `high(x * range)` distributes the finite
source domain as evenly as the mapping permits, but exact uniformity is
impossible when `range` does not divide `2^w`. Strict unbiased random generation
normally needs a rejection step.

## 6. Deliverable

Use this report structure:

1. conclusion;
2. measured hotspot;
3. opportunity class;
4. current and proposed math;
5. range/error/rounding/overflow contract;
6. selected implementation owner;
7. correctness evidence;
8. code-generation evidence;
9. target benchmark;
10. rejected alternatives;
11. rollout and rollback gates.

For a reusable template, read
[`examples/opportunity-review.md`](examples/opportunity-review.md).

## 7. Stop Conditions

Stop and keep the baseline when:

- no profile proves the path matters;
- no trusted reference implementation exists;
- exhaustive/property tests cannot distinguish the candidate from the
  mathematical contract;
- the proof depends on implementation-defined signed shifts without a target
  restriction;
- intermediate width or accumulator growth cannot be bounded;
- range guards cost more than the proposed savings;
- the measured hot symbol or critical path still contains the avoidable
  expensive operation;
- benchmark gains disappear under realistic batching or input distribution;
- maintenance requires unexplained constants or duplicated semantics;
- constant-time requirements cannot be preserved.

## 8. Completion Checklist

- [ ] Hot path and baseline are measured.
- [ ] Opportunity class is named.
- [ ] Scale, range, rounding, overflow, and error are explicit.
- [ ] Compiler/library ownership was checked before hand-rolling.
- [ ] Constants are generated or derived, not unexplained literals.
- [ ] Boundaries and first-invalid inputs are tested.
- [ ] C correctness passed FIL-C before native benchmarking.
- [ ] Optimized assembly or equivalent codegen evidence is retained.
- [ ] Latency and throughput are separated when both matter.
- [ ] DCE/hoisting/vectorization behavior is proven.
- [ ] Target topology, compiler, flags, digests, and repeats are recorded.
- [ ] Product metrics and rollback conditions are stated.
- [ ] `tests/numeric-contracts.mjs` passes when skill formulas change.

## 9. Source Provenance

Derived from:

- `learning/studies/20260908-fast-time-of-day/README.md`;
- `learning/studies/20260908-fast-time-of-day/evidence/`;
- Linux clocksource timekeeping;
- CMSIS-DSP fixed-point types;
- TensorFlow Lite int8 quantization specification;
- Lemire, Kaser, and Kurz on direct remainder computation;
- libdivide runtime division.

The Time-of-Day target measurements are an example, not a transferable
performance claim for the other application domains.
