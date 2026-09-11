---
doc_id: recallfs-evidence-bplus-tree-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-study-bplus-tree-v1
supersedes: []
verified_by:
  - BPT-RA-1
  - BPT-RA-2
  - BPT-RA-3
  - BPT-RA-4
  - BPT-RA-5
  - BPT-RA-6
---

# Evidence Ledger

## Result

All six reconciliation anchors passed for source commit
`fe17463d3421e65d3a84ddfa3ec145dddad049c9`.

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Native Debug | 7/7 CTest suites passed | [`native-debug.txt`](raw/native-debug.txt) |
| Native Release | 7/7 CTest suites passed | [`native-release.txt`](raw/native-release.txt) |
| FIL-C 0.684 | 7/7 suites passed | [`filc.txt`](raw/filc.txt) |
| ASan/UBSan | 7/7 suites passed | [`sanitizers.txt`](raw/sanitizers.txt) |
| Static analysis | Clang analyzer reported no bugs | [`static-analysis.txt`](raw/static-analysis.txt) |
| Format/install | Format, install, external C11 consumer, and C++17 headers passed | [`format-install.txt`](raw/format-install.txt) |
| Identity | Environment and every source/test digest recorded | [`environment.txt`](raw/environment.txt), [`source-sha256.txt`](raw/source-sha256.txt) |
| Evidence integrity | Every raw log digest verifies | [`SHA256SUMS`](raw/SHA256SUMS) |
| Review | 12 validated findings fixed; none remain actionable | [`review.md`](review.md) |

Leak detection was disabled for the sanitizer run because this macOS ASan
runtime does not provide a reliable LeakSanitizer path. FIL-C ownership checks
and explicit backend teardown tests remain active.

## Anchor Results

### BPT-RA-1: Model Equivalence

`bptree_model_test` ran 30,000 deterministic mixed put/delete/get operations
over a 2,048-key domain. It compared every point lookup and repeated complete
ordered scans against independent arrays, including `0` and `UINT64_MAX`.

Result: passed in Debug, Release, ASan/UBSan, and FIL-C.

### BPT-RA-2: Structural Mutation

`bptree_structure_test` built 4,096-key trees in ascending and descending
order with 512-byte pages. It validated during growth and deletion, collapsed
a height-three tree to one leaf and then empty, and proved freed pages were
reused without increasing the high-water count beyond the expected boundary.

Result: passed in all four toolchain modes.

### BPT-RA-3: Persistence

`bptree_persistence_test` inserted 1,200 keys, repeatedly closed and reopened
the file backend, replaced an existing value, deleted every third key, reopened
again, and compared every key/value result.

Result: passed in all four toolchain modes.

### BPT-RA-4: Crash Recovery

`bptree_crash_test` starts from a full 512-byte root leaf and inserts the 29th
key, forcing a leaf split, page extension, and new root. It crashes:

- after unpublished temporary WAL creation;
- after temporary WAL sync but before publication;
- after active WAL publication;
- after the first data-page write;
- after data-file sync;
- during replay after the first page;
- during replay after data-file sync.

Reopen yields the exact old state before publication and the exact committed
29-key state after publication. The same suite rejects every nonempty truncated
prefix of a valid WAL, rejects checksum corruption, bounds oversized record
counts before allocation, and reports a checksum-valid future version as
`BPT_UNSUPPORTED`.

Result: passed in all four toolchain modes.

### BPT-RA-5: Corruption And Ownership

The corruption suite independently edits serialized bytes and recomputes
CRC32C where needed. It rejects:

- bad file header;
- bad page checksum;
- out-of-range internal child with a valid checksum;
- leaf-link cycle with a valid checksum;
- malformed nonempty WAL.

The backend suite verifies mode `0600`, exclusive lock behavior, stale active
WAL preservation, unpublished temporary WAL cleanup, relative-path stability
after `chdir`, and descriptor closure across `exec`.

Result: passed in all four toolchain modes.

### BPT-RA-6: Toolchain And API

- Strict warnings: `-Wall -Wextra -Wpedantic -Werror -Wconversion
  -Wsign-conversion`.
- AppleClang: 21.0.0.
- FIL-C: 0.684, Clang 20.1.8 frontend.
- CMake: 4.0.3.
- Clang static analyzer: no findings.
- `clang-format --dry-run --Werror`: passed.
- Installed static libraries linked from an external C11 translation unit.
- Public headers compiled as C++17.

## Reproduction

Native:

```bash
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/bptree-build -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/bptree-build --parallel
ctest --test-dir .tmp/bptree-build --output-on-failure
```

Sanitizers:

```bash
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/bptree-sanitized \
  -DBPTREE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/bptree-sanitized --parallel
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir .tmp/bptree-sanitized --output-on-failure
```

FIL-C is invoked directly for each test translation unit with:

```text
.tmp/fil-c/bin/filcc -std=c11 -D_POSIX_C_SOURCE=200809L
  -DBPTREE_ENABLE_TEST_HOOKS=1
  -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
  -I<lib>/include -I<lib>/src -I<lib>/tests
  <all library sources> <test source> -o <binary>
.tmp/fil-c/bin/filrun <binary>
```

## Evidence Boundary

The evidence proves deterministic process-crash recovery and executed-path
memory/UB safety on the recorded macOS arm64 and FIL-C Linux environments. It
does not prove physical power-loss behavior, storage-controller cache
semantics, network filesystem ordering, concurrent access, or workload
performance. Those remain explicit non-goals or future qualification work.
