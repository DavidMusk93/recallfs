# Source

| Field | Value |
| --- | --- |
| URL | `https://mp.weixin.qq.com/s/Zvbr8Q1Z4KxEMktISqE1Zw` |
| Title | Go 内存分配器探秘：一次提升内存分配效率 26% 的优化实践 |
| Author | Lance Yang |
| Publisher | eBPF Talk |
| Access Date | `2026-09-07` |
| Archive | `learning/sources/20260907-go-allocator-zeroing.md` |
| Go Change Date | `2025-10-31` |
| Released In | Go 1.26 |
| Topic | allocator provenance, zero initialization, arena allocation |

## 1. Primary References

| Reference | Usage |
| --- | --- |
| [Go commit `27937289`](https://github.com/golang/go/commit/27937289dc9fccf1f5513475145799087f39b964) | Verify the `scav` propagation and `needzero` condition |
| [Linux `madvise(2)`](https://man7.org/linux/man-pages/man2/madvise.2.html) | Bound `MADV_DONTNEED` zero-fill semantics |
| [POSIX `posix_madvise`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_madvise.html) | Distinguish portable advice from Linux semantic changes |
| [Rust `Vec`](https://doc.rust-lang.org/std/vec/struct.Vec.html) | Capacity, reserve, reuse, and `Box<[T]>` conversion |
| [Rust `GlobalAlloc`](https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html) | Stable process-global allocator contract |
| [Rust `Allocator`](https://doc.rust-lang.org/std/alloc/trait.Allocator.html) | Nightly per-container allocator boundary |
| [Rust `MaybeUninit`](https://doc.rust-lang.org/std/mem/union.MaybeUninit.html) | Initialization validity and unsafe boundary |
| [FIL-C](https://fil-c.org/) | C compilation and runtime memory-safety checks |

## 2. Scope Note

The demo does not copy Go runtime code and does not claim to reproduce the
article's benchmark. It independently models the transferable contract:

1. a private anonymous mapping is initially zero-filled;
2. an arena reset makes previously exposed bytes potentially dirty;
3. dirty bytes require explicit initialization for a zeroed request;
4. a successful full-range Linux `MADV_DONTNEED` restores zero-fill-on-demand;
5. the allocator may skip `memset` only when it carries that evidence to the
   allocation decision.

The Linux rule does not generalize to file-backed/shared mappings or arbitrary
operating systems. The Go implementation keeps a platform gate through
`needZeroAfterSysUnused()` for that reason.
