---
doc_id: recallfs-source-rocksdb-range-tombstone-conversion-20260622
kind: reference
status: archived
authority: evidence
applies_to:
  - learning/studies/20260927-lsm-deletion-map
depends_on: []
supersedes: []
verified_by:
  - source URL and SHA-256 recorded below
---

# Range Tombstone Conversion: Faster Scans Over Long Runs of Deletes

Normalized archive of the RocksDB post by Josh Kang, published 2026-06-22 and
accessed 2026-09-27.

- Original:
  <https://rocksdb.org/blog/2026/06/22/range-tombstone-conversion.html>
- HTML SHA-256:
  `ed7eab3c0f70ae6861d8cd0c94c8a04e16bfcd52a7faa57d7f6fe413ede8ca31`
- Linked implementation:
  <https://github.com/facebook/rocksdb/pull/14448>
- Original diagrams:
  <https://rocksdb.org/static/images/range-tombstone-conversion/tombstone-basics.svg>
  and
  <https://rocksdb.org/static/images/range-tombstone-conversion/conversion-before-after.svg>

The page text was extracted from the rendered article. Navigation, generated
line-number labels, and site chrome were removed. The diagrams remain linked
rather than copied.

## Article Claims

RocksDB is an LSM-tree, so a delete appends a tombstone rather than erasing old
data in place. A point tombstone from `Delete` or `SingleDelete` shadows one
key. A range tombstone from `DeleteRange` shadows a half-open key range
`[start, end)`. Space is reclaimed only after compaction reaches a level where
no live snapshot still needs the shadowed state.

A scan must step over each point tombstone. A run of \(N\) point tombstones
therefore costs \(O(N)\) iterator steps on every scan. A range tombstone can
represent the same dead span with one entry and let the iterator seek to the
range end.

Existing mitigations are:

- `NewCompactOnDeletionCollectorFactory`, which schedules files with dense
  deletes for compaction;
- `memtable_op_scan_flush_trigger` and
  `memtable_avg_op_scan_flush_trigger`, which flush a memtable after expensive
  scans so deletion-triggered compaction can process it.

They are reactive and asynchronous, add write amplification, and cannot remove
tombstones still needed by a long-lived snapshot.

## Why Local Conversion Is Incorrect

A flush sees one memtable and a compaction sees only its selected inputs.
Point tombstones for keys `10`, `20`, and `30` can look contiguous in L0 while
live keys `15`, `25`, and `35` exist in L1. Replacing the L0 point deletes with
`[10, 40)` would delete those live keys. Conversion therefore requires a view
that can observe every live key in the interior of the proposed range.

## Read-path Conversion

An application iterator merges the mutable memtable, immutable memtables, and
all SST levels into one ordered stream at one snapshot sequence. When that
iterator sees a sufficiently long run of point tombstones with no visible live
key between them, RocksDB synthesizes
`[first_tombstone_key, next_live_key)` at the iterator's snapshot sequence and
inserts it into the active mutable memtable.

The original point tombstones remain authoritative. The inserted range is
logically redundant, changes no query result, and exists only to accelerate
future scans. It follows the normal memtable, flush, SST, and compaction
lifecycle. A failed or rejected conversion only loses an optimization.

The feature requires a globally complete iterator view. It disables itself for
`table_filter`, partial timestamp visibility, and prefix scans that are neither
total-order nor bounded by `prefix_same_as_start`.

## Configuration And Observability

The feature is documented as available in RocksDB 11.3.0 and later. The
dynamically changeable column-family option is:

```cpp
options.min_tombstones_for_range_conversion = 100;
```

The default is zero, which disables conversion. A higher threshold limits the
extra derived writes to longer runs with larger expected read benefit.

The article names two tickers:

- `rocksdb.read.path.range.tombstones.inserted`
- `rocksdb.read.path.range.tombstones.discarded`

## Published Benchmark

The article's setup fills and compacts one million keys, scatters runs of 100
point deletes, disables automatic compaction, and compares eight-thread
`seekrandom` scans with conversion disabled (`0`) and enabled (`8`).

| Workload | Conversion off | Conversion on | Reported speedup |
| --- | ---: | ---: | ---: |
| Forward scan | 2,685 ops/s | 266,733 ops/s | about 99x |
| Reverse scan | 519 ops/s | 191,119 ops/s | about 368x |

With no deletes, the article reports results within noise:

| Workload | Conversion off | Conversion on |
| --- | ---: | ---: |
| Forward scan, no deletes | 310,052 ops/s | 311,185 ops/s |
| Reverse scan, no deletes | 237,484 ops/s | 236,541 ops/s |

These are upstream measurements, not reproduced evidence in RecallFS.
