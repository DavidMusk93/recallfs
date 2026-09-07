# Exploration Log

## 1. Decisions

| Step | Decision | Reason |
| --- | --- | --- |
| 1 | Model zero provenance, not the Go allocator hierarchy | Provenance is the transferable mechanism |
| 2 | Use a private anonymous page arena | It exposes the Linux reclaim contract directly |
| 3 | Keep reclaim full-range and require `cursor == 0` | Partial/live reclaim cannot justify whole-range zero state |
| 4 | Count explicit and elided zero bytes | Semantic evidence is deterministic; timing is environment-sensitive |
| 5 | Test with FIL-C 0.684 in Linux/ARM64 | Repository policy forbids host Clang fallback |

## 2. Source Corrections

| Source shorthand | Verified boundary |
| --- | --- |
| `make([]byte, n)` enters the heap allocator | Escape analysis may stack-allocate or eliminate it |
| Linux reclaimed memory is zero | True for the default Go `MADV_DONTNEED` path and relevant private anonymous mappings |
| `scav` is syscall success bytes | It is page allocator metadata returned by the allocation transaction |
| Internal reuse is definitely dirty | It is not provably zero and must be treated as potentially dirty |
| 5-26% is the change's speedup | It is clear-cost microbenchmark overhead, not commit A/B workload speedup |

## 3. FIL-C Environment

| Component | Value |
| --- | --- |
| Host | macOS 26.6.2, ARM64 |
| Guest | Ubuntu 25.10, ARM64, Lima 2.2.0 |
| FIL-C | 0.684, clang 20.1.8 |
| FIL-C SHA-256 | `564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666` |
| Isolation root | `.tmp/fil-c/` |

The guest needed `binutils` and `patchelf`; no host C compiler was used.

## 4. Test-First Evidence

The test contract was compiled before the implementation. FIL-C reached the
linker and failed on the expected missing symbols:

```text
undefined reference to `pizlonated_page_arena_init'
undefined reference to `pizlonated_page_arena_alloc'
undefined reference to `pizlonated_page_arena_reset'
undefined reference to `pizlonated_page_arena_reclaim'
undefined reference to `pizlonated_page_arena_destroy'
```

After implementing the arena, the first runtime execution exposed an incorrect
test checksum:

```text
Assertion failed: checksum == 624030
filc panic: user thwarted themselves.
```

The correct sum is `627291`; the expectation was fixed without changing the
allocator behavior.

## 5. Final Verification

Command:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I learning/studies/20260907-allocator-provenance/demo/include \
  learning/studies/20260907-allocator-provenance/demo/src/allocator_demo.c \
  learning/studies/20260907-allocator-provenance/demo/tests/allocator_demo_test.c \
  -o .tmp/allocator-provenance-test

.tmp/fil-c/bin/filrun .tmp/allocator-provenance-test
```

Observed output:

```text
zeroing: explicit=128 elided=256
FIL-C allocator tests passed: 3 suites
```

The test demonstrates that fresh and fully reclaimed zeroed requests elide
`256` bytes of explicit clearing, while dirty reuse clears `128` bytes. This is
not a throughput benchmark and is not comparable to the article's percentages.
