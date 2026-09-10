---
doc_id: study-fast-polynomials-exploration-20260910
kind: exploration-log
status: complete
authority: historical
applies_to:
  - learning/studies/20260910-fast-polynomials
depends_on:
  - learning/studies/20260910-fast-polynomials/source.md
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/evidence/README.md
---

# Exploration Log

## 1. Research Path

| Step | Action | Result |
| --- | --- | --- |
| 1 | Opened the interactive page in a browser | Confirmed it is a chain compiler rather than a prose-only article |
| 2 | Captured its degree-9 $\theta_9$ math and C views | Obtained the exact five-product schedule and method comparison |
| 3 | Read arXiv:2609.06022 v1 | Separated the theorem, application measurements, and floating-point warning |
| 4 | Inspected upstream revision `04c73a7` | Confirmed compiler, C generator, tests, Lean proofs, and benchmark locations |
| 5 | Re-ran `tools/poly_schedule.py ... --check` | The reference compiler reproduced the page's degree-9 chain with five multiplications |
| 6 | Implemented a minimal C11 comparison | Preserved Horner, the generated chain, a long-double oracle, and a cancellation counterexample |
| 7 | Ran FIL-C before native compilation | Correctness, memory safety, and executed UB checks passed |
| 8 | Ran Zig and sanitizer checks | O0/O2/O3 and ASan/UBSan passed |
| 9 | Benchmarked on the target EPYC | Found opposite latency and throughput outcomes; FMA favored Horner strongly |
| 10 | Audited assembly and loop scaling | Confirmed intended work remained and no loop collapse was observed |

## 2. Key Interpretive Decisions

### Multiplication count is a model, not elapsed time

The paper counts field multiplications as the primary expensive operation.
That model is strong for carry-less or modular multiplication, where reduction
dominates. It is incomplete for scalar binary64, where additions, register
pressure, FMA formation, and independent issue width can dominate.

The demo therefore measures two contracts separately:

- dependent latency, where multiplicative depth should matter;
- eight-way scalar throughput, where total instruction pressure should matter.

### Use one favorable and one adversarial polynomial

The reverse Bessel polynomial $\theta_9$ is the page's default example and has
dyadic preprocessed constants that are exactly representable in binary64.
It is a useful implementation example.

The polynomial $x^9$ exposes the numerical boundary. Its five-product rational
chain contains nonzero constants whose contributions cancel in exact
arithmetic. At $x=2^{-10}$, the C chain rounds to zero while ordinary repeated
multiplication yields exactly $2^{-90}$.

### Keep upstream and local evidence separate

The paper's GF($2^{64}$) and application benchmarks were not rerun here. Their
speedups are useful evidence about the intended domain, but they are quoted as
upstream results. The only local performance conclusion is for the binary64
degree-9 demo on the named EPYC/GCC target.

## 3. Failed or Corrected Attempts

### Stale browser element reference

The first attempt to click the C tab used a stale accessibility-tree reference
and failed. A fresh snapshot plus a DOM text lookup selected the tab, after
which the generated C was extracted successfully.

### Browser lock unavailable

The browser tool did not expose `browser_lock`. The browser-use contract allows
continuing without a lock when that function is unavailable.

### SCP with an IPv6 destination

`scp` through the jump host closed the connection because the raw IPv6 target
was ambiguous to its destination parser. Streaming a tar archive through the
already-working SSH command transferred the demo without changing its files.

### Incorrect hand-transcribed dyadic constant

The first $x^9$ negative-test implementation transcribed
$38917/16384$ as `2.37579345703125`; the correct exact binary64 value is
`2.37530517578125`. FIL-C produced an implausibly huge residual, which exposed
the mistake before native benchmarking. After correction, the intended
relative error was exactly 1.0 at $x=2^{-10}$.

### Target `/usr/bin/time` absent

The target does not install `/usr/bin/time`. The rounds-scaling negative check
used `date +%s%N` around the pinned process instead.

### PMU unavailable

`perf stat` returned `<not counted>` for cycles, instructions, branches, and
branch misses and suggested disabling the NMI watchdog. No privileged setting
was changed. The performance evidence therefore uses pinned wall time,
topology, source/binary digests, optimized disassembly, and a 4x rounds-scaling
check.

## 4. What Changed the Initial Expectation

The naive expectation was that 5 multiplications should beat Horner's 8. The
target data refined that into three cases:

1. Without FMA and with one dependent result chain, lower multiplicative depth
   wins.
2. With eight independent evaluations, Horner's compact instruction stream
   wins despite more multiplications.
3. With FMA contraction, Horner maps almost perfectly to dependent FMA
   instructions and wins even the latency case.

This is the practical lesson of the study: optimize the generated machine DAG
under the application's actual arithmetic semantics, not a source-level
operation count in isolation.
