# Go Allocator Zeroing Article Archive

| Field | Value |
| --- | --- |
| URL | `https://mp.weixin.qq.com/s/Zvbr8Q1Z4KxEMktISqE1Zw` |
| Title | Go 内存分配器探秘：一次提升内存分配效率 26% 的优化实践 |
| Author | Lance Yang |
| Publisher | eBPF Talk |
| Published | 2025-11-03 08:10 |
| Access Date | 2026-09-07 |
| Upstream Change | `golang/go@27937289dc9fccf1f5513475145799087f39b964` |

## 1. Archived Summary

文章分析 Go runtime 的 `mheap -> mcentral -> mcache -> mspan` 分层分配路径，
重点解释一次因内存来源信息未传递完整而发生的冗余清零。旧实现只知道某个
span 是否可能来自已使用的 Go heap，无法区分“仅在 heap 内复用的 dirty
pages”和“已经完整归还 OS、再次访问时由 OS 提供零页的 pages”，因此对两者
都设置 `needzero`。

上游改动把 page allocator 在本次 allocation transaction 中已经计算出的
`scav` 字节数传给 `initSpan`。它是 allocator metadata 中“该 range 被标记
为 scavenged 的字节数”，不是 `madvise` 事后返回的 success byte count。当且
仅当整个 span 都已 scavenged，且当前平台与 runtime 配置的 `sysUnused`
契约允许把它视为 zero-filled 时，runtime 才跳过软件 `memclr`。

## 2. Core Claims

| Claim | Status |
| --- | --- |
| Fresh private anonymous mappings are zero-filled | Linux contract; `MAP_UNINITIALIZED` 等显式例外除外 |
| Dirty in-process reuse must be initialized before zeroed use | Required |
| Fully scavenged private anonymous pages can skip explicit clear | Conditional on full-range metadata and platform/release-mode contract |
| Partial reclaim is enough to skip the whole clear | False; the Go change requires `scav == span_size` |
| Replacing an allocator alone explains the optimization | False; the key is preserving provenance across layers |
| The reported allocation speedup is 5-26% | Clear-cost microbenchmark, not commit A/B or end-to-end workload |

## 3. Source Boundary

This archive preserves metadata, claims, and the mechanism rather than copying
the complete article. The study validates the state machine with an independent
C implementation and checks the platform boundary against the Go commit and the
Linux `madvise(2)` contract.

Two article simplifications are intentionally not retained:

- `make([]byte, size)` may be stack-allocated or optimized away; the described
  mechanism applies when execution reaches the heap allocator.
- On Linux, Go can use `MADV_FREE` with `GODEBUG=madvdontneed=0`; that mode still
  requires software clearing. The optimization applies to the default
  `MADV_DONTNEED` path, not to every Linux configuration.
