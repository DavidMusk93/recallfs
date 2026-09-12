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
  - RBT-RA-1
  - RBT-RA-2
  - RBT-RA-3
  - RBT-RA-4
  - RBT-RA-5
  - RBT-RA-6
  - RBT-RA-7
  - RBT-RA-8
---

# Evidence Ledger

## Result

The refreshed evidence is bound to source commit
`f41e0976977e5012cd4946fa2dca258e85aebd32`. `RBT_FORMAT_VERSION=1` is
the sole format, with `RBTR` schema magic and unified-column layout.

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Zig Debug | 8/8 CTest suites passed | [`native-debug.txt`](raw/native-debug.txt) |
| Zig Release | 8/8 CTest suites passed | [`native-release.txt`](raw/native-release.txt) |
| ASan/UBSan | 8/8 CTest suites passed | [`sanitizers.txt`](raw/sanitizers.txt) |
| FIL-C 0.684 | 8/8 standalone suites passed | [`filc.txt`](raw/filc.txt) |
| Static analysis | `ccc-analyzer` bound; no bugs found | [`static-analysis.txt`](raw/static-analysis.txt) |
| Format/install | format, install, typed `find_package` consumer, C++17 headers passed | [`format-install.txt`](raw/format-install.txt) |
| Identity | environment plus every source/test digest recorded | [`environment.txt`](raw/environment.txt), [`source-sha256.txt`](raw/source-sha256.txt) |
| Review | current stale-version finding fixed; no actionable findings remain | [`review.md`](review.md) |

Leak detection was disabled because this macOS ASan runtime does not provide a
reliable LeakSanitizer path. ASan/UBSan, FIL-C ownership checks, and explicit
backend teardown tests remained active.

## Anchor Results

### RBT-RA-1: Typed Schema And Canonical Keys

`rbt_schema_test` verifies package version 0.2.0, format version 1, one
schema-order `columns[]` array with KEY-filtered ordering, one `values[]`
record representation, complete owned and callback rows, the sole
`rbt_row_get` accessor, column IDs, all five types, nullable and descending
flags, maximum sizes, UTF-8 validation, composite canonical ordering,
input-copy behavior, callback lifetime, and schema-free reopen through
`rbt_open`. The current `RBTR` schema roundtrip and malformed schema
chain/size/count handling are covered by `rbt_schema_test` and
`rbt_corruption_test`.

Result: passed under Debug, Release, ASan/UBSan, and FIL-C.

### RBT-RA-2: Limits And Overflow

Compile-time and runtime checks enforce 64 total columns, 1 MiB per variable
field, and 4 MiB per row. `rbt_overflow_test` verifies inline-to-overflow
transition, byte-exact reads, independent per-column chains, shrink/delete/free
behavior, freelist reuse, and replacement of a row backed by 2,048 overflow
pages. Its interleaved schema places payload columns before, between, and after
KEY columns and verifies full physical-order reconstruction plus
physical-column-ordinal `AUX` identity through reopen, update, delete, and
reuse.

Result: passed in all four modes.

### RBT-RA-3: Exact Mutation Locality

`rbt_locality_test` wraps the memory provider and records every committed page
ID. Existing-row replacement, non-splitting insertion, and non-rebalancing
deletion each commit exactly page `0` metadata plus one leaf. Same-value put
commits nothing.

The same suite observes leaf split, internal split, borrow, merge, and root
collapse. Each commit includes metadata, has unique page IDs, stays below the
live tree size, and fits the height-derived path/sibling bound. Updating one
large value column excludes the unchanged column's overflow chain.

Result: passed in all four modes.

### RBT-RA-4: Structure And Reuse

`rbt_structure_test` grows multi-level typed trees, deletes through
redistribution and merge, collapses the root to one leaf and then empty, frees
overflow pages, and proves later inserts consume the freelist before extending
the page high-water mark.

Result: passed in all four modes.

### RBT-RA-5: Independent Typed Model

`rbt_model_test` executes 30,000 deterministic mixed get/put/delete/scan
operations over typed composite records and compares point results and ordered
scans against an independent in-memory model.

Result: passed in all four modes.

### RBT-RA-6: Backends And Page Sizes

`rbt_backend_test` runs both memory and file providers with page sizes 512,
4,096, and 65,536. The raw FIL-C log records:

| Page size | Rows | Height | Tree pages | Overflow pages |
| --- | ---: | ---: | ---: | ---: |
| 512 | 24 | 3 | 15 | 4 |
| 4,096 | 96 | 3 | 19 | 1 |
| 65,536 | 120 | 3 | 20 | 1 |

The suite also verifies one live tree lease per storage context, mode `0600`,
exclusive lock, dirfd-relative `O_NOFOLLOW` paths, `CLOEXEC`, definite-failure
rollback, `-EOWNERDEAD` poisoning, and close/reopen recovery.

Result: passed in all four modes.

### RBT-RA-7: Corruption And WAL

`rbt_corruption_test` independently mutates serialized bytes and recomputes
checksums where necessary. It rejects malformed schema chains and sizes,
out-of-bounds or overlapping slots, child/link corruption, overflow cycles,
wrong physical-column overflow identity/length, bad file headers, and malformed
WAL.

`rbt_crash_test` exercises full-page redo publication and replay crash points,
every nonempty truncated prefix of a valid WAL, checksum/version/size failures,
and a checksum-valid 4,096-record WAL. Reopen exposes the exact pre-publication
or committed state, never a mixed page set.

Result: passed in all four modes.

### RBT-RA-8: Qualification And Review

- Strict warnings: `-Wall -Wextra -Wpedantic -Werror -Wconversion
  -Wsign-conversion`.
- Native frontend: Zig 0.16.0 `zig cc`/`zig c++`, Clang 21.1.0.
- Zig archive SHA-256:
  `b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489`.
- FIL-C: 0.684, Clang 20.1.8 frontend.
- CMake: 4.0.3.
- Static analyzer: Homebrew Clang 16.0.6 `ccc-analyzer`; no bugs.
- Installed consumer:
  `find_package(rbt 0.2.0 EXACT CONFIG REQUIRED)` with `rbt::rbt`,
  `rbt::memory`, and `rbt::file`.
- Public headers: C++17 compilation passed.
- Review run `20260912-110259-0424385b`: one validated stale-version README
  finding fixed by this documentation update; rejected candidates did not
  establish further defects; no actionable findings remain.

## Raw SHA-256 Ledger

The tracked `raw/SHA256SUMS` contains:

| Raw file | SHA-256 |
| --- | --- |
| `environment.txt` | `45b0a6683972c249c3309e124c24e7f9876ca343f7cfe80a4340bead8c0cbd53` |
| `filc.txt` | `94bd50d3fcdda00e579c478cc0cccc0bbf9ea7580f662e6f4b88fd30b674629a` |
| `format-install.txt` | `2e38d39fefaea30099f7f36a90184c8fd94c236d1c0d0a013b818d9fa6807529` |
| `native-debug.txt` | `91cd6bf96d76346a600308ad9e981f7c9148939a3c435b9d68f12453cce7db09` |
| `native-release.txt` | `4ab17ee14ea54365632c9413d4293d71fd8bca7ce745e8a2f9a17523b2f39a7a` |
| `sanitizers.txt` | `f74d72f4905948e26ae7e1d8f7afbaa34447865b24c86453f7d0b16f189ef7d0` |
| `source-sha256.txt` | `ed6105976da47396c396391bbe40c26820e75de091a13526215ec36613436f78` |
| `static-analysis.txt` | `4e3b0f4bb57a7c7c97dc544f2b991255b5b257b64bfcac0a791dfe081e43934f` |

## Reproduction

Native:

```bash
ZIG_CC="$PWD/.tmp/scripts/zig-cc"
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/rbt-build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$ZIG_CC" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .tmp/rbt-build --parallel --verbose
ctest --test-dir .tmp/rbt-build --output-on-failure
```

Sanitizers:

```bash
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/rbt-sanitized -DRBT_ENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$ZIG_CC"
cmake --build .tmp/rbt-sanitized --parallel --verbose
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir .tmp/rbt-sanitized --output-on-failure
```

Installed package:

```bash
cmake -S learning/studies/20260911-bplus-tree/lib \
  -B .tmp/rbt-install-build -DRBT_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$ZIG_CC"
cmake --build .tmp/rbt-install-build --parallel
cmake --install .tmp/rbt-install-build --prefix "$PWD/.tmp/rbt-install"
cmake -S learning/studies/20260911-bplus-tree/lib/tests/package_consumer \
  -B .tmp/rbt-consumer -DCMAKE_PREFIX_PATH="$PWD/.tmp/rbt-install"
cmake --build .tmp/rbt-consumer --parallel
.tmp/rbt-consumer/rbt_package_consumer
```

## Evidence Boundary

These logs prove deterministic process-crash behavior and executed-path
memory/UB safety in the recorded macOS arm64 and FIL-C Linux environments.
They do not prove physical power-loss or controller-cache behavior, network
filesystem ordering, multi-thread or multi-process shared-tree access, online
backup, or performance. No benchmark result is claimed.
