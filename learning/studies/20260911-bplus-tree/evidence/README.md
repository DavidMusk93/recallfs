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
  - BPT-RA-7
---

# Evidence Ledger

## Result

All seven reconciliation anchors passed for the generic API and disk format at
source commit `5c936d1e70a293fe2e5acbf1f6d23016b49f43f8`.

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Native Debug | 8/8 CTest suites passed | [`native-debug.txt`](raw/native-debug.txt) |
| Native Release | 8/8 CTest suites passed | [`native-release.txt`](raw/native-release.txt) |
| FIL-C 0.684 | 8/8 suites passed | [`filc.txt`](raw/filc.txt) |
| ASan/UBSan | 8/8 CTest suites passed | [`sanitizers.txt`](raw/sanitizers.txt) |
| Static analysis | Clang analyzer reported no bugs | [`static-analysis.txt`](raw/static-analysis.txt) |
| Format/install | Format, install, tracked `find_package` consumer, and C++17 headers passed | [`format-install.txt`](raw/format-install.txt) |
| Identity | Environment and every source/test digest recorded | [`environment.txt`](raw/environment.txt), [`source-sha256.txt`](raw/source-sha256.txt) |
| Evidence integrity | Every raw log digest verifies | [`SHA256SUMS`](raw/SHA256SUMS) |
| Review | 4 generic-v2 findings fixed; no actionable findings remain | [`review.md`](review.md) |

Leak detection was disabled for the sanitizer run because this macOS ASan
runtime does not provide a reliable LeakSanitizer path. FIL-C ownership checks
and explicit backend teardown tests remain active.

## Anchor Results

### BPT-RA-1: Model Equivalence

`btree_model_test` runs 30,000 deterministic mixed put/delete/get operations
over a 2,048-key domain using an independent canonical big-endian codec. It
compares every point lookup and repeated complete ordered scans against
independent arrays, including `0` and `UINT64_MAX`.

Result: passed in Debug, Release, ASan/UBSan, and FIL-C.

### BPT-RA-2: Structural Mutation

`btree_structure_test` built 4,096-key trees in ascending and descending
order with 512-byte pages. It validated during growth and deletion, collapsed
a height-three tree to one leaf and then empty, and proved freed pages were
reused without increasing the high-water count beyond the expected boundary.

Result: passed in all four toolchain modes.

### BPT-RA-3: Persistence

`btree_persistence_test` inserted 1,200 keys, repeatedly closed and reopened
the file backend, replaced an existing value, deleted every third key, reopened
again, and compared every key/value result.

Result: passed in all four toolchain modes.

### BPT-RA-4: Crash Recovery

`btree_crash_test` starts from a full 512-byte root leaf and inserts the 29th
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
`BTREE_UNSUPPORTED`.

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
after `chdir`, final-component symlink rejection, and descriptor closure across
`exec`.

Result: passed in all four toolchain modes.

### BPT-RA-6: Toolchain And API

- Strict warnings: `-Wall -Wextra -Wpedantic -Werror -Wconversion
  -Wsign-conversion`.
- Native compiler: Zig 0.16.0 `zig cc`/`zig c++`, Clang 21.1.0 frontend.
- Zig macOS arm64 archive SHA-256:
  `b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489`.
- FIL-C: 0.684, Clang 20.1.8 frontend.
- CMake: 4.0.3.
- Clang static analyzer 16.0.6: `ccc-analyzer` binding asserted; no findings.
- `clang-format --dry-run --Werror`: passed.
- Installed static libraries linked from the tracked
  `tests/package_consumer` CMake project through `find_package(btree 1 CONFIG
  REQUIRED)`.
- Public headers compiled as C++17.

### BPT-RA-7: Generic Schema

`btree_generic_test` covers 13-byte keys, 21-byte values, embedded NUL bytes,
caller-buffer overwrite after put, file reopen, schema inspection, schema
mismatch, balanced odd-capacity right-edge split, comparator reentry rejection,
and a custom comparator over 2,048 shuffled keys through internal split and
delete paths.

Result: passed in all four toolchain modes.

## Reproduction

Native:

```bash
ZIG_CC="$PWD/.tmp/scripts/zig-cc"
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/btree-build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$ZIG_CC" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .tmp/btree-build --parallel --verbose
ctest --test-dir .tmp/btree-build --output-on-failure
```

Sanitizers:

```bash
ZIG_CC="$PWD/.tmp/scripts/zig-cc"
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/btree-sanitized \
  -DBTREE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$ZIG_CC" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .tmp/btree-sanitized --parallel --verbose
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir .tmp/btree-sanitized --output-on-failure
```

FIL-C is invoked directly for each test translation unit with:

```text
.tmp/fil-c/bin/filcc -std=c11 -D_POSIX_C_SOURCE=200809L
  -DBTREE_ENABLE_TEST_HOOKS=1
  -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
  -I<lib>/include -I<lib>/src -I<lib>/tests
  <all library sources> <test source> -o <binary>
.tmp/fil-c/bin/filrun <binary>
```

Installed-package consumer:

```bash
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/btree-install-build -DBTREE_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .tmp/btree-install-build --parallel
cmake --install .tmp/btree-install-build \
  --prefix "$PWD/.tmp/btree-install"
cmake -S \
  learning/studies/20260911-bplus-tree/lib/tests/package_consumer \
  -B .tmp/btree-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/.tmp/btree-install"
cmake --build .tmp/btree-consumer --parallel
.tmp/btree-consumer/btree_package_consumer
```

## Evidence Boundary

The evidence proves deterministic process-crash recovery and executed-path
memory/UB safety on the recorded macOS arm64 and FIL-C Linux environments. It
does not prove physical power-loss behavior, storage-controller cache
semantics, network filesystem ordering, concurrent access, or workload
performance. Those remain explicit non-goals or future qualification work.
