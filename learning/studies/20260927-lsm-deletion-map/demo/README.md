# LSM Deletion Map C Demo

This C11 project models the mechanism behind RocksDB read-path range tombstone
conversion. It is deliberately a bounded teaching model, not a storage engine:
records use `uint64_t` keys, arrays have explicit fixed capacities, and the
merge is implemented in-memory.

The model preserves the important contracts:

1. point tombstones remain the authoritative append-only history;
2. conversion consumes a complete, total-order snapshot view;
3. every derived range is half-open and stamped with the snapshot sequence;
4. an old snapshot cannot see the derived range;
5. a newer point write outranks the older range;
6. incomplete views and stale target generations reject conversion;
7. conversion failure is fail-open and affects performance only.

## Layout

| Path | Role |
| --- | --- |
| `include/deletion_map.h` | Bounded data model and public demo API |
| `src/deletion_map.c` | Snapshot merge, conversion, and range-aware scan |
| `src/main.c` | Narrated safe/unsafe examples |
| `tests/deletion_map_test.c` | Seven suites with a point-only independent oracle |

## FIL-C Correctness Gate

From the repository root:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260927-lsm-deletion-map/demo/include \
  learning/studies/20260927-lsm-deletion-map/demo/src/deletion_map.c \
  learning/studies/20260927-lsm-deletion-map/demo/tests/deletion_map_test.c \
  -o .tmp/deletion-map-test

.tmp/fil-c/bin/filrun .tmp/deletion-map-test
```

Expected:

```text
deletion-map correctness passed: 7 suites
```

Build and run the narrated example with the same toolchain:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O2 -g -DNDEBUG -Wall -Wextra -Werror \
  -I learning/studies/20260927-lsm-deletion-map/demo/include \
  learning/studies/20260927-lsm-deletion-map/demo/src/deletion_map.c \
  learning/studies/20260927-lsm-deletion-map/demo/src/main.c \
  -o .tmp/deletion-map-demo

.tmp/fil-c/bin/filrun .tmp/deletion-map-demo
```

## CMake

For an environment with CMake:

```bash
cmake \
  -S learning/studies/20260927-lsm-deletion-map/demo \
  -B .tmp/deletion-map-cmake \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$PWD/.tmp/fil-c/bin/filcc" \
  -DFIL_RUNNER="$PWD/.tmp/fil-c/bin/filrun" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build .tmp/deletion-map-cmake -j
ctest --test-dir .tmp/deletion-map-cmake --output-on-failure
```

The 2026-09-27 validation host did not have `cmake` installed, so the recorded
evidence uses the equivalent direct FIL-C and Apple Clang commands.

## What The Counters Mean

The demo prints abstract merged-iterator steps, not timing. For the safe input,
the point-only view has four visible states: deletes at `10`, `20`, and `30`,
then a put at `40`. The derived `[10,40)@30` range replaces the three delete
steps with one range step, so the model reports `4 -> 2`.

No benchmark is included. FIL-C execution proves only the exercised functional,
memory-safety, and undefined-behavior paths; it is not performance evidence.
