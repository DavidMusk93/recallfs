# SQLite WAL-Reset Reproducer

This C11 program adapts Phil Eaton's natural WAL-reset reproducer into three
runtime modes. It compares SQLite versions without modifying SQLite source and
checks acknowledged writes independently from `PRAGMA integrity_check`.

| Mode | SQLite path | Purpose |
| --- | --- | --- |
| `unsafe` | WAL, write and PASSIVE checkpoint overlap | Expose the documented race |
| `serialized` | WAL, process-wide gate around writes and checkpoints | Validate an application-level mitigation |
| `rollback` | DELETE journal mode | Validate removal of the WAL precondition |

The default setup creates 65,536 rows containing 3,900-byte blobs so that a
large `mmap`/`munmap` widens the vulnerable interval. The unsafe result is
probabilistic. Exit code `1` means committed-write loss or failed integrity was
detected; exit code `0` means no loss was observed in that finite run.

## Prepare SQLite

From the repository root:

```bash
mkdir -p .tmp/sqlite-wal-reset-demo/vendor
curl -L --fail \
  https://www.sqlite.org/2026/sqlite-amalgamation-3510200.zip \
  -o .tmp/sqlite-amalgamation-3510200.zip
curl -L --fail \
  https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip \
  -o .tmp/sqlite-amalgamation-3510300.zip
unzip -q .tmp/sqlite-amalgamation-3510200.zip \
  -d .tmp/sqlite-wal-reset-demo/vendor
unzip -q .tmp/sqlite-amalgamation-3510300.zip \
  -d .tmp/sqlite-wal-reset-demo/vendor
```

Verify:

```text
6e2a845a493026bdbad0618b2b5a0cf48584faab47384480ed9f592d912f23ec  sqlite-amalgamation-3510200.zip
acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4  sqlite-amalgamation-3510300.zip
```

## FIL-C Correctness Gate

The 9 MB SQLite amalgamation exceeded the isolated compiler's memory at `-O1`
when both sources were passed in one invocation. Compile it separately at
`-O0`; do not replace this gate with host Clang:

```bash
.tmp/fil-c/bin/filcc \
  -std=c11 -O0 -g0 -DSQLITE_THREADSAFE=1 \
  -I .tmp/sqlite-wal-reset-demo/vendor/sqlite-amalgamation-3510200 \
  -c .tmp/sqlite-wal-reset-demo/vendor/sqlite-amalgamation-3510200/sqlite3.c \
  -o .tmp/sqlite-wal-reset-demo/sqlite3-filc-3512.o

.tmp/fil-c/bin/filcc \
  -std=c11 -O0 -g0 \
  -I .tmp/sqlite-wal-reset-demo/vendor/sqlite-amalgamation-3510200 \
  -c learning/studies/20260908-sqlite-wal-reset-bug/demo/src/wal_reset_repro.c \
  -o .tmp/sqlite-wal-reset-demo/wal-reset-repro-filc.o

.tmp/fil-c/bin/filcc \
  .tmp/sqlite-wal-reset-demo/wal-reset-repro-filc.o \
  .tmp/sqlite-wal-reset-demo/sqlite3-filc-3512.o \
  -pthread -ldl -lm \
  -o .tmp/sqlite-wal-reset-demo/wal-reset-repro-filc-3512

.tmp/fil-c/bin/filrun \
  .tmp/sqlite-wal-reset-demo/wal-reset-repro-filc-3512 \
  --db .tmp/sqlite-wal-reset-demo/filc.db \
  --mode serialized --attempts 3 --rows 512
```

The recorded run completed 6 committed writes with zero loss and no FIL-C
failure.

## Native A/B

Build each upstream version in a separate directory:

```bash
cmake \
  -S learning/studies/20260908-sqlite-wal-reset-bug/demo \
  -B .tmp/sqlite-wal-reset-demo/build-3512 \
  -DSQLITE_AMALGAMATION_DIR="$PWD/.tmp/sqlite-wal-reset-demo/vendor/sqlite-amalgamation-3510200" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .tmp/sqlite-wal-reset-demo/build-3512 -j

cmake \
  -S learning/studies/20260908-sqlite-wal-reset-bug/demo \
  -B .tmp/sqlite-wal-reset-demo/build-3513 \
  -DSQLITE_AMALGAMATION_DIR="$PWD/.tmp/sqlite-wal-reset-demo/vendor/sqlite-amalgamation-3510300" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .tmp/sqlite-wal-reset-demo/build-3513 -j
```

Run the vulnerable case:

```bash
.tmp/sqlite-wal-reset-demo/build-3512/wal-reset-repro \
  --db .tmp/sqlite-wal-reset-demo/unsafe-3512.db \
  --mode unsafe --attempts 400
```

Run the fixed and no-upgrade controls:

```bash
.tmp/sqlite-wal-reset-demo/build-3513/wal-reset-repro \
  --db .tmp/sqlite-wal-reset-demo/unsafe-3513.db \
  --mode unsafe --attempts 400

.tmp/sqlite-wal-reset-demo/build-3512/wal-reset-repro \
  --db .tmp/sqlite-wal-reset-demo/serialized-3512.db \
  --mode serialized --attempts 400

.tmp/sqlite-wal-reset-demo/build-3512/wal-reset-repro \
  --db .tmp/sqlite-wal-reset-demo/rollback-3512.db \
  --mode rollback --attempts 400
```

The `serialized` mutex is process-local. A real multi-process deployment needs
one writer/checkpoint owner or an inter-process gate observed by every writer
and checkpointer. Merely enabling SQLite serialized threading mode does not
serialize distinct connections into one database-wide critical section.
