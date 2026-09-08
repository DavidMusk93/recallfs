# Fixed-Point Opportunity Review

## Conclusion

State one of:

- keep the baseline;
- defer pending measurement;
- prototype one candidate;
- accept the measured candidate;
- reject the candidate.

Name the measured reason.

When deferred, state:

- missing evidence;
- next measurement or proof;
- owner;
- stop condition for further work.

## Hotspot

| Field | Evidence |
| --- | --- |
| Caller/loop | |
| Product metric | |
| Baseline cost | |
| Input distribution | |
| Divisor/scale lifetime | |
| Scalar/vector shape | |

## Opportunity

| Candidate | Class | Why it may help | Rejection risk |
| --- | --- | --- | --- |
| | Q / multiply-shift / divider / range / reduction | | |

## Numeric Contract

| Field | Value |
| --- | --- |
| Mathematical formula | |
| Input/output domains | |
| Representation and scale | |
| Rounding | |
| Overflow | |
| Error bound | |
| Signedness | |
| Portability | |

## Selected Implementation Owner

Record the compiler, library/framework, generated descriptor, or local helper.
Explain why higher-priority owners were insufficient.

## Correctness Evidence

- exhaustive/property domain;
- boundary and first-invalid inputs;
- trusted reference;
- FIL-C result for C;
- distribution/bias checks when relevant.

## Code Generation

- compiler and flags;
- relevant final-binary disassembly;
- divide/multiply/high-half/shift/add/branch instructions;
- scalar/vector mode;
- DCE/hoisting/loop proof.

## Benchmark

### Reproduction Identity

| Field | Value |
| --- | --- |
| Host / OS / kernel | |
| CPU / sockets / cores / SMT / NUMA | |
| CPU and NUMA pin | |
| Frequency governor / boost | |
| Compiler / linker | |
| Complete flags | |
| Source digest | |
| Binary digest | |
| Inputs / workload source | |
| Warmup / samples / rounds | |
| PMU availability or failure reason | |

| Metric | Baseline | Candidate | Delta | Variation |
| --- | ---: | ---: | ---: | ---: |
| latency/value | | | | |
| throughput/value | | | | |
| instructions/value | | | | |
| cycles/value | | | | |
| setup/descriptor cost | | | | |
| break-even reuse count | | | | |
| product metric | | | | |
| error | | | | |

Include setup and conversion cost.

## Rejected Alternatives

| Alternative | Reason |
| --- | --- |
| | |

## Decision and Rollback

State:

- accepted scope;
- enforced range/scale guard;
- rollout metric;
- rollback threshold;
- residual risk.
