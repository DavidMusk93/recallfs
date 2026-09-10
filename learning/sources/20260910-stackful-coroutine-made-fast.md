# Stackful Coroutine Made Fast

| Field | Value |
| --- | --- |
| Authors | Huiba Li, Rui Du, Sinan Lin, Windsor Hsu |
| Organization | Alibaba Cloud |
| Source | https://photonlibos.github.io/blog-20241014/stackful-coroutine-made-fast.html |
| PDF | https://photonlibos.github.io/blog-20241014/Stackful_Coroutine_Made_Fast.pdf |
| Appendix | https://photonlibos.github.io/blog-20241014/appendix.pdf |
| Accessed | 2026-09-10 |
| Publication status | Rejected ASPLOS'24 submission; not a published ASPLOS paper |
| Local archive | `learning/studies/20260910-stackful-coroutine/papers/` |

## Source Status

The authors explicitly state that the manuscript was submitted to ASPLOS'24
but rejected because its key proposal, context-aware context switching (CACS),
had already appeared in libfringe. The study must therefore describe it as an
author manuscript, not as an ASPLOS publication.

The downloaded artifacts contain two metadata inconsistencies:

- the main manuscript says `Submission #17 for ASPLOS'24`;
- the appendix title says `Appendix of Submission #46`.

These labels are preserved as source facts. No attempt is made to reconcile
them without an authoritative correction from the authors.

## Abstract-Level Claim

The manuscript argues that stackful coroutines are not intrinsically slow.
Its CACS design moves register-liveness knowledge to the compiler and switch
call site so that a switch saves only values needed by the suspended caller,
allows the branch to be inlined, and improves branch prediction and cache
behavior. The implementation also proposes a `preserve_none` calling
convention.

The strongest reported result is approximately 1.52 ns, or 3.34 cycles, for a
yield operation on the authors' Xeon test system. That number is a source
claim, not a locally reproduced result.

## Reproduction Boundary

Portable C11 plus a separately compiled assembly switch cannot safely infer
register liveness at every call site. The local implementation therefore uses
the complete System V AMD64 callee-saved contract as its production baseline.
It studies CACS but does not claim to reproduce CACS without the authors'
compiler extensions and calling convention.

Related papers, standards proposals, and the network data-plane reference are
listed with exact URLs and checksums in the study's `source.md` and
`papers/README.md`.
