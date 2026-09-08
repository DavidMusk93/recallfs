# Allocator Provenance Demo

This C11 demo implements a page-backed bump arena with explicit zero
provenance. It is a correctness model, not a production allocator or a
performance benchmark.

## What It Proves

| Transition | Expected behavior |
| --- | --- |
| Fresh anonymous mapping -> zeroed allocation | Skip explicit clear |
| Dirty allocation -> arena reset -> zeroed allocation | Run explicit clear |
| Dirty allocation -> full successful reclaim -> zeroed allocation | Skip explicit clear |
| Reclaim while allocations are live | Reject and keep conservative state |
| Reclaim failure on a locked dirty mapping -> zeroed allocation | Run explicit clear |
| Invalid alignment or capacity overflow | Return failure without moving cursor |
| 1000 batch-lifetime rows | Use one backing mapping |

The demo only promotes `OS_RECLAIMED` after successful full-range
`MADV_DONTNEED` on its private anonymous mapping.

## FIL-C Build

From the repository root:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I learning/studies/20260907-allocator-provenance/demo/include \
  learning/studies/20260907-allocator-provenance/demo/src/allocator_demo.c \
  learning/studies/20260907-allocator-provenance/demo/tests/allocator_demo_test.c \
  -o .tmp/allocator-provenance-test

.tmp/fil-c/bin/filrun .tmp/allocator-provenance-test
```

Expected output:

```text
zeroing: explicit=128 elided=256
FIL-C allocator tests passed: 4 suites
```

## CMake Shape

`CMakeLists.txt` records the standard project topology for IDEs and Linux
builders. It can also drive FIL-C explicitly:

```bash
cmake \
  -S learning/studies/20260907-allocator-provenance/demo \
  -B .tmp/allocator-provenance-cmake \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$PWD/.tmp/fil-c/bin/filcc" \
  -DFIL_RUNNER="$PWD/.tmp/fil-c/bin/filrun"

cmake --build .tmp/allocator-provenance-cmake
ctest --test-dir .tmp/allocator-provenance-cmake --output-on-failure
```

The direct FIL-C command remains the shortest authoritative verification and
cannot silently select the host compiler.

Generated build trees and binaries belong under `.tmp/` or `demo/build/` and
must not be committed.
