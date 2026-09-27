# Verification Evidence

## Environment

```text
Date: 2026-09-27
Host: Darwin arm64, kernel 25.5.0
FIL-C compiler: clang 20.1.8, Fil-C 0.684
FIL-C target: aarch64-unknown-linux-gnu
pyenv: 2.6.32, commit 5c924f986e2c6e1767e510e6990276dc2820ed84
Python: 3.13.13
uv: 0.11.26
CMake: 4.4.3
Ninja: 1.13.2
Zig: 0.16.0, LLVM-compatible C frontend reports Clang 21.1.0
```

FIL-C archive identity is recorded by the repository-local toolchain:

```text
FIL-C 0.684 Linux ARM64
SHA-256 564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666
Guest Ubuntu 26.04 ARM64
```

The Python and build tools are isolated under `.tmp/tooling/`. The fixed
CPython source archive SHA-256 is
`2ab91ff401783ccca64f75d10c882e957bdfd60e2bf5a72f8421793729b78a71`.
The `ziglang-0.16.0-py3-none-macosx_12_0_arm64.whl` index SHA-256 is
`b61e5413c49508d9d62e5dcea543e3af491594154d74a00ff52f84ed508260cc`;
the installed `zig` executable SHA-256 is
`e6cd688d25664983833aae272f501d4bceeae304875b8f1741209d15fd13a4ec`.

Provisioning commands:

```bash
git clone --depth 1 --branch v2.6.32 \
  https://github.com/pyenv/pyenv.git .tmp/tooling/pyenv

export PYENV_ROOT="$PWD/.tmp/tooling/pyenv-root"
export PATH="$PWD/.tmp/tooling/pyenv/bin:$PATH"
pyenv install -s 3.13.13
export PYENV_VERSION=3.13.13
PYTHON_BIN="$(pyenv which python)"

uv venv --python "$PYTHON_BIN" .tmp/tooling/cmake/venv
uv pip install --python .tmp/tooling/cmake/venv/bin/python \
  "cmake==4.4.3" "ninja==1.13.2"

uv venv --python "$PYTHON_BIN" .tmp/tooling/zig/venv
uv pip install --python .tmp/tooling/zig/venv/bin/python \
  "ziglang==0.16.0"
```

## Source Digests

```text
e758f50852b1169e652f0e0fedaba21245fd1c88fc2d9ab281635542ac4bda06  demo/CMakeLists.txt
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

## CMake And CTest

The first FIL-C CMake build used a static library. CMake selected the host
macOS `/usr/bin/ar`, which emitted a BSD/Mach-O-oriented archive around Linux
ARM64 FIL-C objects. The FIL-C linker then failed with unresolved
`pizlonated_*` symbols. The final graph uses a CMake `OBJECT` library, avoiding
the cross-target archive boundary.

After that correction, CMake 4.4.3 and Ninja 1.13.2 completed both builds:

```text
FIL-C: configure passed; build 5/5; CTest 1/1 passed
Zig cc: configure passed; build 5/5; CTest 1/1 passed
```

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

## Zig LLVM Frontend Cross-check

The same source and flags were compiled directly with Zig 0.16.0 `zig cc`.
The test and demo emitted the same output as FIL-C. The Zig build also passed
with `-fsanitize=address,undefined` and `detect_leaks=0`.

Before the Zig/tool-provisioning policy was added, an Apple Clang run was used
and `detect_leaks=1` was found unsupported by that runtime. It is historical
exploration, not the final LLVM frontend evidence.

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
