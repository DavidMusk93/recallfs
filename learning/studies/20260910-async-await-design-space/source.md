---
doc_id: recallfs-source-async-await-design-space-v1
kind: source-index
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-async-await-design-space
depends_on:
  - learning/sources/20260910-async-await-design-space-v1.pdf
supersedes: []
verified_by:
  - learning/studies/20260910-async-await-design-space/evidence/raw/paper.txt
  - learning/studies/20260910-async-await-design-space/evidence/raw/doc-dag.txt
---

# Sources

Access date: 2026-09-10.

## Primary Paper

| Field | Value |
| --- | --- |
| Title | A Design Space Exploration of Async/Await |
| Authors | Gavin Gray, Shriram Krishnamurthi, Will Crichton |
| Venue | OOPSLA 2026 |
| Version | arXiv `2608.20677v1`, submitted 2026-08-21 |
| Subjects | Programming Languages (`cs.PL`) |
| arXiv | <https://arxiv.org/abs/2608.20677> |
| Publication DOI | <https://doi.org/10.1145/3839519> |
| Local PDF | [`learning/sources/20260910-async-await-design-space-v1.pdf`](../../sources/20260910-async-await-design-space-v1.pdf) |
| Pages | 28 |
| Size | 963,903 bytes |
| SHA-256 | `cd9156997914274ae0411d63496c6c5f29316e9cc9eee5d1dd06f3ffff6b5606` |
| License | CC BY 4.0 |

The PDF was downloaded directly from
`https://arxiv.org/pdf/2608.20677`. `pdfinfo` identifies the title, authors,
28 pages, PDF 1.7, no encryption, and no JavaScript.

## Author Summary

- Cognitive Engineering Lab paper page:
  <https://cel.cs.brown.edu/paper/a-design-space-exploration-of-aa/>
- Author blog post, published 2026-09-08:
  <https://cel.cs.brown.edu/blog/design-space-async-await/>

The blog provides an interactive overview of the paper's nine dimensions and
the output differences across seven runtimes. Claims in the study are grounded
in the archived paper rather than the blog presentation.

## Paper Artifact

| Field | Value |
| --- | --- |
| Version | `v1.3` |
| Zenodo | <https://doi.org/10.5281/zenodo.21765917> |
| Source repository | <https://github.com/gavinleroy/async-await> |
| Contents | Redex models, language examples, differential fuzzer |
| Reported evaluation | 50 programs x 50 runs for each of seven runtimes |

The artifact metadata and public repository were inspected. The 2.2 GB
multi-runtime image was not downloaded or run for this study: it requires
8-16 GB of VM memory and a 45-60 minute full evaluation. The local C demo is
an explanatory model of the Rust slice, not a reproduction of the paper's
seven-runtime Redex artifact.

## Rust References

- `Future` contract:
  <https://doc.rust-lang.org/std/future/trait.Future.html>
- `Waker` contract:
  <https://doc.rust-lang.org/std/task/struct.Waker.html>
- `Pin` and address-sensitive async state machines:
  <https://doc.rust-lang.org/std/pin/index.html>
- Async/await RFC 2394:
  <https://rust-lang.github.io/rfcs/2394-async_await.html>
- Tokio `spawn`:
  <https://docs.rs/tokio/latest/tokio/task/fn.spawn.html>
- Tokio `JoinHandle`:
  <https://docs.rs/tokio/latest/tokio/task/struct.JoinHandle.html>
- Tokio `select!` cancellation safety:
  <https://docs.rs/tokio/latest/tokio/macro.select.html>

## Version Boundary

The paper evaluates Rust 1.92.0, Tokio 1.50.0, and Smol 2.0.2. The local Rust
reference was compiled with `rustc 1.97.0 (2d8144b78 2026-07-07)`. Its three
language-level observations agree with the paper, but this does not silently
upgrade the paper's runtime-version claims.
