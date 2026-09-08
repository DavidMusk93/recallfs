# Fixed-Point Proof Contract

Every fixed-point change must connect the mathematical function to the actual
integer program over a declared finite domain.

## 1. Representation

Declare one form.

### Binary Q-format

```text
decoded(I) = I / 2^F
resolution = 2^-F
```

State:

- signed or unsigned;
- storage width;
- fraction bits `F`;
- valid encoded and decoded ranges;
- whether the most-negative signed value is allowed;
- whether values are normalized, saturated, or wrapped.

### Affine quantization

```text
decoded(q) = (q - zero_point) * scale
encoded(r) = clamp(round(r / scale) + zero_point)
```

State whether scale is per-tensor, per-channel/axis, or per-value.

### Rational multiply-shift

```text
y = round_mode((x * M) / 2^S) + affine_offset
```

State how `M`, `S`, and `affine_offset` are derived and whether the transform is
exact or approximate.

When rounding is implemented with an integer adjustment, use a separate form:

```text
y = floor_div(x * M + rounding_adjustment(x), 2^S) + affine_offset
```

Derive `rounding_adjustment` for the selected signed/tie semantics. Do not add a
rounding bias and then apply `round_mode` again.

### Decimal scaled integer

Exact decimal semantics require an exact decimal source:

```text
stored = parse_decimal_to_scaled_integer(text_or_decimal, D, rounding_mode)
```

State:

- accepted source: decimal text, decimal type, or exact integer/rational;
- behavior when the source has more than `D` fractional digits;
- signed rounding;
- scale/schema version;
- overflow and invalid syntax.

Treat a binary floating-point source as approximate unless a proven correctly
rounded conversion is used. For example, binary64 `1.005 * 100` is below the
exact decimal halfway value and can round to 100 instead of 101.

## 2. Rounding

Name the exact mode:

| Mode | Positive behavior | Negative behavior |
| --- | --- | --- |
| Truncate toward zero | discard fraction | move toward zero |
| Floor | down | more negative |
| Ceil | up | less negative |
| Nearest, ties away | nearest | symmetric magnitude increase on ties |
| Nearest-even | nearest | tie chooses even result |
| Stochastic | probability by fraction | requires RNG contract |

Do not implement signed rounding by adding a positive bias and shifting unless
the negative case has been derived. A right shift of a negative signed integer
also needs a language and target contract. In C/C++, left-shifting a negative
signed value is not an implementation technique; use checked widened
multiplication by `2^F` or an explicitly proven unsigned-magnitude transform.

For round-to-nearest Q-format with exact representable input and no clipping,
the initial quantization error is at most half one LSB:

```text
absolute_error <= 2^(-F-1)
```

This bound does not automatically survive repeated feedback, nonlinear
operations, clipping, or requantization.

## 3. Intermediate Width

For a signed `w`-bit input, a full product generally needs `2w` bits.

Common cases:

| Inputs | Product to retain | Typical accumulator |
| --- | --- | --- |
| Q7 x Q7 | 16-bit | 32-bit |
| Q15 x Q15 | 32-bit Q30 | 32/64-bit |
| Q31 x Q31 | 64-bit Q62 | 64-bit or wider analysis |
| u32 x u32 reciprocal | 64-bit | high/low 32-bit halves |
| u64 x u64 reciprocal | 128-bit or mul-high primitive | target-dependent |

In C, Q15 operands usually undergo integer promotion to 32-bit `int`; the
product fits. The bug appears when code narrows before rescale or when the
accumulator cannot hold the sum. Do not claim that Q15 multiplication itself
necessarily overflows.

Prove:

```text
max_abs_product = max_abs_a * max_abs_b
max_abs_sum = term_count * max_abs_product + max_abs_bias
```

Include signed asymmetry: `abs(INT_MIN)` is not representable in the same
signed type.

## 4. Scale Algebra

### Addition and subtraction

Operands must have the same scale:

```text
QF(a) + QF(b) -> QF(result)
```

If scales differ, align explicitly and account for rounding.

### Multiplication

```text
QF(a) * QF(b) -> Q(2F)(product)
QF(result) = round(product / 2^F)
```

### Division

To retain `F` fractional bits:

```text
QF(result) = round((widen(a) * 2^F) / b)
```

Prove that the widened multiplication cannot overflow, bound `F`, define
divide-by-zero behavior, and prove that the final quotient is representable.

### Multiply-accumulate

Accumulate at product scale, then round once when possible. Requantizing every
term can create avoidable cumulative error.

## 5. Reciprocal Division

For unsigned `x` and positive `d`, start from:

```text
q = floor(x / d)
r = x - q*d
```

For signed operands, first name the language contract. C, C++, and Rust integer
division truncate toward zero:

```text
q = trunc(x / d)
r = x - q*d
sign(r) follows x when r != 0
```

Define negative divisors, divide by zero, and the minimum-signed-value divided
by `-1` overflow case before deriving a signed descriptor.

A reciprocal method may use:

```text
q = (x * M) >> S
```

or:

```text
q = ((high(x * M) + correction(x)) >> post_shift)
```

The exact descriptor depends on word width, signedness, divisor, and algorithm.
A plain `M=ceil(2^w/d)` high-half multiply is not generic.

Counterexample:

```text
w = 32
d = 7
M = ceil(2^32 / 7) = 613566757
x = 3724842645

high32(x*M) = 532120378
floor(x/7)  = 532120377
```

Required proof or test:

- exact for every value in the claimed domain;
- or bounded approximation with an explicit correction;
- first failing value outside a restricted domain;
- quotient and remainder identity;
- signed numerator/divisor semantics when applicable.

If the divisor is compile-time constant, inspect compiler codegen before
replacing `/` or `%`.

## 6. Direct Remainder

When using the low fractional portion of a reciprocal product:

- state the reciprocal width;
- state whether the method computes quotient, remainder, divisibility, or all;
- prove the precision bound;
- retain the wide product;
- test all remainder transition points.

Do not infer a low-half remainder formula from the quotient formula without the
specific derivation.

## 7. Range Mapping and Bias

For unsigned `w`-bit `x`, an exact `2w`-bit product, and an integer
`range` satisfying `1 <= range <= 2^w - 1`:

```text
bucket = high_w(x * range)
```

maps the full source domain into `[0, range)`. When `range` does not divide
`2^w`, exact equal bucket counts are impossible without rejecting some source
values.

Reject `range == 0`. If an API can represent `range == 2^w`, handle it as a
separate identity-width case; values larger than the source cardinality cannot
make every bucket reachable.

State which contract applies:

- bounded imbalance acceptable;
- statistically tested distribution;
- strict unbiased output with rejection;
- deterministic partitioning only, not random generation.

Test:

- minimum and maximum source values;
- zero, one, and maximum representable range;
- every bucket reachable;
- bucket counts over a tractable full domain or representative sample;
- rejection threshold and retry behavior.

## 8. Saturation, Wrap, and Trap

Choose one behavior per operation:

| Behavior | Use |
| --- | --- |
| Widen | preserve exact intermediate |
| Saturate | DSP/control/audio where clipping is part of contract |
| Checked/trap | correctness-critical value should never exceed range |
| Wrap | modular arithmetic only when intentional |

Do not mix saturating inputs with wrapping accumulators unless the mathematical
contract requires it.

## 9. Error Accumulation

For a pipeline, track error stage by stage:

```text
input quantization
  + coefficient quantization
  + multiplication rounding
  + accumulation
  + requantization
  + clipping
```

Use worst-case bounds when safety depends on them. Use measured/statistical
error only when the product contract is statistical and the dataset is
representative.

Feedback systems require stability analysis; a one-operation LSB bound is not
enough.

## 10. Constant-Time and Security

For secret-dependent arithmetic:

- no secret-dependent branch;
- no secret-dependent memory access;
- no variable-time hardware divide unless explicitly accepted;
- use reviewed modular arithmetic implementations;
- test with target-specific side-channel tooling;
- do not translate an approximate business formula into cryptographic code.

## 11. Correctness Evidence

### Exhaustive

Use when the full domain is tractable. Report the exact inclusive domain.

### Property testing

Compare against a trusted wide or arbitrary-precision reference:

```text
candidate(x) == reference(x)
```

or:

```text
abs(candidate(x) - reference(x)) <= declared_error
```

Generate values around:

- powers of two;
- quotient transitions `k*d - 1`, `k*d`, `k*d + 1`;
- rounding half points;
- saturation thresholds;
- minimum/maximum signed values;
- first invalid range values.

### Differential testing

Compare:

- baseline integer/float implementation;
- compiler-generated division;
- library implementation;
- candidate fixed-point path.

Avoid using the candidate's own constants or rounding helper in the reference.

## 12. Proof Record

Record:

| Field | Value |
| --- | --- |
| Source formula | |
| Encoded formula | |
| Word widths | |
| Scale/zero point | |
| Input domain | |
| Output domain | |
| Rounding | |
| Overflow behavior | |
| Error bound | |
| Constant generator | |
| Exhaustive/property evidence | |
| First invalid value | |
| Portability assumptions | |
