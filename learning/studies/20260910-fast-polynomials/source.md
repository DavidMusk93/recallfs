---
doc_id: study-fast-polynomials-sources-20260910
kind: source-index
status: active
authority: evidentiary
applies_to:
  - learning/studies/20260910-fast-polynomials
depends_on:
  - learning/sources/20260910-fast-polynomials.md
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/evidence/README.md
---

# Sources

## Primary Sources

| Source | Revision or date | Role |
| --- | --- | --- |
| `https://thomasahle.com/fast-polynomials/` | accessed 2026-09-10 | Interactive explanation, comparison, and C generator |
| `https://arxiv.org/abs/2609.06022` | v1, 2026-09-05 | Theorems, construction, lower bounds, experiments, and numerical analysis |
| `https://github.com/thomasahle/fast-polynomials` | `04c73a79a47f6107ed20ae1d4792e3e24ca6bc6f` | Reference compiler, generated-C tests, Lean proofs, and benchmark sources |
| `learning/sources/20260910-fast-polynomials.md` | local snapshot | Browser-observed page claims and degree-9 chain |

## Integrity

| Artifact | SHA-256 |
| --- | --- |
| Downloaded page HTML | `7ad10cff6506f2c8616ac87dd236321fba12d23ca11fd701ec247ec843639367` |
| arXiv PDF | `fa5db152b8d5f7c73c544624cd2986255c6bf3850f1deb45854c6cd786e6784e` |
| Upstream `website/js/cgen.js` | `ea72099fc34fcc20850b06540e4b24372c682da5021e8c1aaf758668165c4e39` |
| Upstream `tools/polychain.py` | `b67891ed23cdf0f3c7f37aeecd48d9411c408abe6ad131af443f88f0c5d5fc7e` |
| Upstream `sections/experiments.tex` | `fed53379cf9786a3347bc140c8bc7eb7887fad7028d1e5f5ce0c90359111c14f` |

## Claims Used

The study relies on these distinctions from the paper:

- For a monic degree-$n$ polynomial over characteristic zero or characteristic
  $p>n$, the construction uses $\lfloor n/2\rfloor+1$ multiplications.
- The preprocessing is rational and everywhere defined for that theorem.
- The construction has $O(\log n)$ multiplicative depth but may use more
  additions than Horner.
- Exact algebraic equivalence does not imply floating-point stability.
- The strongest application evidence is for exact finite-field arithmetic and
  parameter-as-data hashing, not arbitrary prescribed floating coefficients.
- The characteristic-2 construction is separate; the general all-degree upper
  bound remains open.

## Scope and Limitations

- The C demo independently reproduces the generated binary64 degree-9 chain.
- This study does not independently rerun the upstream GF($2^{64}$), Mersenne,
  hash-table, sketch, or filter benchmarks. Their numbers are labeled as
  upstream evidence.
- The local target-machine benchmark measures binary64 evaluation only.
- The arXiv PDF and repository moved quickly around the v1 publication date.
  Exact claims in this study are tied to the v1 PDF and repository commit above,
  rather than to an unspecified future website revision.
