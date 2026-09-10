# Sources

Access date for every source: 2026-09-10.

## Archived Papers

| Role | Title | Status | Source | Local file |
| --- | --- | --- | --- | --- |
| Primary | Stackful Coroutine Made Fast | Rejected ASPLOS'24 author manuscript | https://photonlibos.github.io/blog-20241014/Stackful_Coroutine_Made_Fast.pdf | `papers/stackful-coroutine-made-fast.pdf` |
| Primary supplement | Appendix of Stackful Coroutine Made Fast | Author appendix; submission number conflicts with main PDF | https://photonlibos.github.io/blog-20241014/appendix.pdf | `papers/stackful-coroutine-made-fast-appendix.pdf` |
| Semantics | Revisiting Coroutines | ACM TOPLAS 31(2), 2009; DOI `10.1145/1462166.1462167` | https://www.cs.tufts.edu/~nr/cs257/archive/roberto-ierusalimschy/revisiting-coroutines.pdf | `papers/revisiting-coroutines.pdf` |
| Server model | Cooperative Task Management without Manual Stack Management | USENIX ATC 2002 | https://www.usenix.org/publications/library/proceedings/usenix02/full_papers/adyahowell/adyahowell.pdf | `papers/cooperative-task-management.pdf` |
| Fiber critique | Fibers under the Magnifying Glass, P1364R0 | C++ WG21 paper, 2018 | https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1364r0.pdf | `papers/fibers-under-the-magnifying-glass-p1364r0.pdf` |
| Fiber response | Response to "Fibers under the Magnifying Glass", P0866R0 | C++ WG21 paper, 2019 | https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p0866r0.pdf | `papers/response-to-fibers-p0866r0.pdf` |
| Fiber reply | Response to Response to "Fibers under the Magnifying Glass", P1520R0 | C++ WG21 paper, 2019 | https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1520r0.pdf | `papers/response-to-response-p1520r0.pdf` |
| Network data plane | mTCP: A Highly Scalable User-level TCP Stack for Multicore Systems | USENIX NSDI 2014 | https://www.usenix.org/system/files/conference/nsdi14/nsdi14-paper-jeong.pdf | `papers/mtcp-nsdi14.pdf` |

## Referenced but Not Archived

| Title | Reason | Canonical reference |
| --- | --- | --- |
| Compiler Support for Lightweight Context Switching | The open author-copy page was available, but its PDF endpoint could not be retrieved without a forbidden response. No substitute copy was used. | DOI `10.1145/2400682.2400695`; https://figshare.com/articles/journal_contribution/Compiler_support_for_lightweight_context_switching/19854355 |
| libfringe | This is source code, not a paper. The primary manuscript identifies it as prior implementation of the key CACS idea. | https://github.com/emberian/libfringe |
| PhotonLibOS | This is the production codebase discussed by the primary manuscript. | https://github.com/alibaba/PhotonLibOS |

## Source Notes

- The main manuscript identifies itself as `Submission #17`; the appendix
  metadata says `Submission #46`.
- P0866R0 contains a `P0886R0` footer typo on extracted pages.
- Source benchmark claims are kept separate from results reproduced on `ssh d2`.
- The local demo uses kernel TCP and `epoll`; mTCP is background for batching
  and data-plane limits, not an implementation dependency.
