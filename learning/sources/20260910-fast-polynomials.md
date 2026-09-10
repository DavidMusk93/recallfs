---
doc_id: source-fast-polynomials-web-20260910
kind: source
status: snapshot
authority: upstream
applies_to:
  - learning/studies/20260910-fast-polynomials
depends_on: []
supersedes: []
verified_by:
  - learning/studies/20260910-fast-polynomials/source.md
---

# Fast Polynomial Evaluation - Source Snapshot

| Field | Value |
| --- | --- |
| Original URL | `https://thomasahle.com/fast-polynomials/` |
| Page title | Evaluating Polynomials Fast |
| Authors behind the linked paper | Thomas D. Ahle and Jakob B. T. Knudsen |
| Access date | 2026-09-10 |
| Local HTML SHA-256 | `7ad10cff6506f2c8616ac87dd236321fba12d23ca11fd701ec247ec843639367` |
| Upstream repository revision | `04c73a79a47f6107ed20ae1d4792e3e24ca6bc6f` |
| Paper | arXiv:2609.06022 v1, submitted 2026-09-05 |

## Page Claim

The page states that Horner's rule evaluates a degree-$n$ polynomial with $n$
multiplications, or $n-1$ when the polynomial is monic. After one-time
preprocessing of the coefficients, the linked work evaluates any monic
polynomial with

$$
\left\lfloor \frac{n}{2} \right\rfloor + 1
$$

multiplications, and a general non-monic polynomial with one additional
multiplication.

The page presents this as an interactive compiler:

1. enter a polynomial;
2. select $\mathbb{Q}$, $\mathbb{R}$, $\mathbb{C}$, a Mersenne prime field, or
   a binary extension field;
3. compare the paper's chain with Horner, Estrin, Rabin-Winograd,
   Knuth-Eve, and Pan;
4. inspect the mathematical chain, generated C, or circuit graph.

The named applications are function approximation, hashing, cryptography, and
coding theory.

## Captured Degree-9 Example

The desktop page opened on the reverse Bessel polynomial

$$
\begin{aligned}
P(x)={}&x^9+45x^8+990x^7+13860x^6+135135x^5\\
      &+945945x^4+4729725x^3+16216200x^2\\
      &+34459425x+34459425.
\end{aligned}
$$

The generated chain was:

```text
y0 = (x + 11) * x
y1 = (x + y0 - 43/2) * (-x + y0 + 265/2)
y2 = (x + y0 + y1 + 51215/4) * (-x - y0 + y1 + 5019/4)
y3 = (y0 + y1 + 69067/4) * (-y0 + y1 + 5391/4)
y4 = (x + y2 + 32741929) * x
P  = y3 + y4 + 56100843
```

The page counted 5 multiplications, 20 additions/subtractions, and
multiplicative depth 4. Its comparison rows for the same polynomial reported:

| Method | Multiplications | Additions | Multiplicative depth | Exact preprocessing |
| --- | ---: | ---: | ---: | --- |
| This paper | 5 | 20 | 4 | yes |
| Horner | 8 | 9 | 8 | yes |
| Estrin | 11, including 4 scalar | 9 | 4 | yes |
| Rabin-Winograd | 8, including 1 scalar | 12 | 4 | yes |
| Knuth-Eve | 5 | 9 | 5 | numeric |
| Pan | 5 | 14 | 4 | numeric |

## Archive Limitation

The page is an interactive client-side compiler. The local HTML download is
only its shell; generated chains, comparison rows, and C output depend on
JavaScript state. This archive therefore preserves the browser-observed claims
and the exact degree-9 output rather than committing a non-functional copy of
the site. The downloaded HTML and full upstream checkout remain under `.tmp/`
and are not tracked.
