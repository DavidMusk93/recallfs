# Exploration Log

## 2026-09-27

### 1. Source capture

- Opened the rendered RocksDB article in a real browser.
- Independently fetched the page as Markdown.
- Recorded author, publication date, URL, diagram links, article HTML digest,
  published benchmark setup, and published results.
- Read PR #14448 and the later RocksDB `main` implementation at
  `4052fccde9d9533fa9c57749b63ba3cda2c9b134`.

The later source was necessary because the article explicitly mentions
follow-up bug fixes. The current implementation adds concrete completeness,
transaction, mutable-memtable, duplicate-range, and external-ingestion guards
that the article summarizes only as "numerous guards."

### 2. Mechanism selected

The requested artifact is a bounded C demo, not a production storage
component. The smallest complete mechanism is:

```text
append-only point history
          |
          v
snapshot merge over every source
          |
          v
ordered visible states + completeness proof
          |
          v
deleted run -> [start, next_live) @ snapshot
          |
          v
derived range map for later scans
```

The point history remains authoritative. The map is derived and may be dropped
without changing recovery results.

### 3. Rejected approaches

| Approach | Why rejected |
| --- | --- |
| Convert during one-level flush | Cannot see live keys in other levels and can delete valid data |
| Convert from compaction inputs alone | The selected files are not necessarily a complete snapshot view |
| Give the range the oldest or newest point-delete sequence | The proof was made at the iterator snapshot; using that snapshot is the direct visibility contract |
| Replace point tombstones | Makes the optimization authoritative and complicates crash recovery and old snapshots |
| Use an unversioned bitmap for LSM keys | Physical position is unstable across compaction and cannot express MVCC ordering |
| Convert a tail run without an owned end bound | A generic ordered key type has no universal successor or safe infinity encoding |

### 4. Demo design

`dm_materialize_snapshot` creates the full visible user-key stream from
append-only point records. `dm_convert_tombstone_runs` requires an explicit
`dm_visibility_proof` and matching source epoch before producing disjoint
half-open ranges. `dm_scan_view` applies range sequence ordering while
preserving newer point writes.

The implementation is intentionally bounded to 128 records, 128 visible keys,
and 32 ranges. Capacity exhaustion is explicit. It does not implement WAL,
manifest publication, physical compaction, background concurrency, or a
general byte-key comparator.

### 5. Proof-first failure

The first FIL-C release build used ordinary `assert(...)` around test calls.
`-DNDEBUG` removed the expressions, so Clang correctly reported every test
variable as unused. This exposed that the tests would not execute in a release
configuration.

The suite was changed to an always-evaluated `CHECK` macro that prints the
failed expression and exits. The tests are now valid with `-DNDEBUG`.

### 6. Build environment

```text
Host: Darwin arm64, kernel 25.5.0
FIL-C compiler: clang 20.1.8, Fil-C 0.684
FIL-C target: aarch64-unknown-linux-gnu
pyenv: 2.6.32
Python: 3.13.13
uv: 0.11.26
CMake: 4.4.3
Ninja: 1.13.2
Zig: 0.16.0
```

The repository-local FIL-C wrapper started its Ubuntu 26.04 ARM64 Lima guest
and successfully compiled and ran the tests.

The host initially lacked `pyenv`, CMake, Ninja, and Zig. `pyenv` 2.6.32 was
cloned from its official tag into `.tmp/tooling/pyenv`; it then built CPython
3.13.13 under `.tmp/tooling/pyenv-root`. `uv` created isolated CMake and Zig
environments from that interpreter. The first requested Python version,
3.13.14, was not in pyenv 2.6.32's definitions, so installation stopped before
mutation and was retried with the latest listed version, 3.13.13. Likewise,
the configured package index did not contain CMake 4.1.1; `uv --dry-run`
resolved CMake 4.4.3 and Ninja 1.13.2, which were then installed with exact
pins.

The first CMake/FIL-C build exposed a host/target archive mismatch:
`/usr/bin/ar` wrapped Linux ARM64 FIL-C objects in a form the FIL-C linker
could not resolve, producing undefined `pizlonated_*` symbols. Changing the
teaching library from `STATIC` to `OBJECT` removed the unnecessary archive
step. CMake configure, build, and CTest then passed with FIL-C.

Zig 0.16.0 `zig cc` replaced the earlier system-Clang cross-check. Direct,
CMake/CTest, and ASan/UBSan Zig runs all passed. The earlier Apple Clang
`detect_leaks=1` failure remains historical evidence from before the tool
policy correction, not a final verification path.

### 7. Observed output

```text
deletion-map correctness passed: 7 suites
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

No local RocksDB benchmark was run. The `4 -> 2` result is a pedagogical count
of abstract iterator states, not measured throughput.
