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
`6d823594103237e372cfe844ac0cac12fb187e3d` and the sole clean-break format
`RBT_FORMAT_VERSION=1`.

| Gate | Result | Raw evidence |
| --- | --- | --- |
| Zig Debug | 8/8 CTest suites passed | [`native-debug.txt`](raw/native-debug.txt) |
| Zig Release | 8/8 CTest suites passed | [`native-release.txt`](raw/native-release.txt) |
| ASan/UBSan | 8/8 CTest suites passed | [`sanitizers.txt`](raw/sanitizers.txt) |
| FIL-C 0.684 | 8/8 standalone suites passed | [`filc.txt`](raw/filc.txt) |
| Static analysis | `ccc-analyzer` bound; no bugs found | [`static-analysis.txt`](raw/static-analysis.txt) |
| Format/install | format, install, typed `find_package` consumer, C++17 headers passed | [`format-install.txt`](raw/format-install.txt) |
| Identity | environment plus every source/test digest recorded | [`environment.txt`](raw/environment.txt), [`source-sha256.txt`](raw/source-sha256.txt) |
| Review | 14 validated findings closed; none actionable remain | [`review.md`](review.md) |

Leak detection was disabled because this macOS ASan runtime does not provide a
reliable LeakSanitizer path. ASan/UBSan, FIL-C ownership checks, and explicit
backend teardown tests remained active.

## Anchor Results

### RBT-RA-1: Typed Schema And Canonical Keys

`rbt_schema_test` verifies package version 0.1.0, format version 1, column IDs,
all five types, nullable and descending flags, maximum sizes, UTF-8 validation,
composite canonical ordering, input-copy behavior, row ownership, scan
callback lifetime, and schema-free reopen through `rbt_open`.

Result: passed under Debug, Release, ASan/UBSan, and FIL-C.

### RBT-RA-2: Limits And Overflow

Compile-time and runtime checks enforce 64 total columns, 1 MiB per variable
field, and 4 MiB per row. `rbt_overflow_test` verifies inline-to-overflow
transition, byte-exact reads, independent per-column chains, shrink/delete/free
behavior, freelist reuse, and replacement of a row backed by 2,048 overflow
pages.

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
wrong overflow column identity/length, bad file headers, and malformed WAL.

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
  `find_package(rbt 0.1 CONFIG REQUIRED)` with `rbt::rbt`,
  `rbt::memory`, and `rbt::file`.
- Public headers: C++17 compilation passed.
- Review run `20260911-221757-4574fb5e`: 14 validated findings closed;
  no actionable findings remain.

## Raw SHA-256 Ledger

The tracked `raw/SHA256SUMS` contains:

| Raw file | SHA-256 |
| --- | --- |
| `environment.txt` | `b037efb4b2ea999050f51b66adc825ead16bf1efee2bae1c0ab107ab47033e0a` |
| `filc.txt` | `1937e2e8c85fb10a16cb891f7003320a4aafd5577b73d5dddb51185eff200d55` |
| `format-install.txt` | `9a1f9af5a854c43b6662925b85dfc1b4b7974b1023430c395b10f2bb5751a367` |
| `native-debug.txt` | `eaa83c535ad3920dff9742d0583daf76dae3f972e69072c86a8481392e89d5ec` |
| `native-release.txt` | `671b7edd7f704104bad7bd07eecfb190369ec90525665d0957808dd3185c4f16` |
| `sanitizers.txt` | `e104de70cb00435839e1dd37d9bfc5ca1516c8060c4b66efd62cd995af5dfa4d` |
| `source-sha256.txt` | `65fab5af0210c61c27f1825a92833564490d11399b6f2f04102866403581ce27` |
| `static-analysis.txt` | `bccced06f7423df91f68b6679fa67c74673a040da770aa62d56186280571f3a2` |

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
