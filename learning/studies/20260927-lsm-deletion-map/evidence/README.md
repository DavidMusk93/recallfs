# Verification Evidence

## Environment

```text
Date: 2026-09-27
Host: Darwin arm64, kernel 25.5.0
Native compiler: Apple clang 21.0.0
FIL-C compiler: clang 20.1.8, Fil-C 0.684
FIL-C target: aarch64-unknown-linux-gnu
CMake: unavailable
```

FIL-C archive identity is recorded by the repository-local toolchain:

```text
FIL-C 0.684 Linux ARM64
SHA-256 564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666
Guest Ubuntu 26.04 ARM64
```

## Source Digests

```text
ed53547db3ac2a08ca9be1696eb5d18ef04eeca27ebd76e158a247c069e8e16f  demo/include/deletion_map.h
658acf3df1889ebddb506322cc97ce8a46d10b885dfe33939d5043b0a92f7386  demo/src/deletion_map.c
7a5a2f6a5246fb39134feb1c870e27da61be00d13102e80f6966eee0949500b1  demo/src/main.c
fd0eb83b5b41e89515e2700742714179be53e862c749813095a43bd217f66e8d  demo/tests/deletion_map_test.c
```

These digests describe the source at the final recorded FIL-C run. Any source
change requires refreshing this ledger and rerunning the gates.

## FIL-C Test

Command:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260927-lsm-deletion-map/demo/include \
  learning/studies/20260927-lsm-deletion-map/demo/src/deletion_map.c \
  learning/studies/20260927-lsm-deletion-map/demo/tests/deletion_map_test.c \
  -o .tmp/deletion-map-test

.tmp/fil-c/bin/filrun .tmp/deletion-map-test
```

Observed:

```text
deletion-map correctness passed: 7 suites
```

The suites cover safe conversion, forward/reverse scans, incomplete-view and
stale-generation rejection, an expected-corruption negative case, snapshot
differential checks against a point-only oracle, threshold/tail behavior,
conflicting records, and atomic rejection on map-capacity exhaustion.

## FIL-C Demo

Observed:

```text
safe conversion: [10,40)@30
baseline=[40]
mapped=[40]
abstract steps: 4 -> 2 (3 point entries skipped)
partial-level conversion: incomplete view
partial-view baseline=[15,25,35,40]
naive range result=[40]
naive range verdict: CORRUPTION detected by oracle
snapshot@20=[10,20,30,40]
snapshot@35=[20,40]
```

## Native Cross-check

The same source and flags were compiled directly with Apple Clang 21.0.0.
The test emitted the same success line and the demo emitted the same output.

The Apple Clang test also passed with `-fsanitize=address,undefined` and
`detect_leaks=0`, and `clang --analyze` reported no diagnostics. The first
sanitizer invocation used `detect_leaks=1` and exited before the test because
that option is unsupported by this Apple AddressSanitizer runtime; the rerun
disabled only leak detection.

## Documentation DAG

Manual validation resolved:

```text
recallfs-study-lsm-deletion-map-v1
  -> recallfs-agent-ready-docs-v1
  -> recallfs-source-rocksdb-range-tombstone-conversion-20260622
  -> learning/studies/20260927-lsm-deletion-map/source.md
```

Repository-wide `doc_id` scanning found each referenced ID exactly once. The
source reference has no dependencies, and the exact-path dependency exists, so
the new subgraph has no missing edge, duplicate ID, or cycle.

## Boundary

FIL-C establishes executed-path functional, memory-safety, and
undefined-behavior evidence. It is not a benchmark or a complete proof. No
RocksDB build, crash injection, concurrency test, or target-machine performance
measurement was run for this study.
