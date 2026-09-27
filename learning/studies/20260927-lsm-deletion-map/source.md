# Sources And Revision

## Primary Article

| Field | Value |
| --- | --- |
| Title | Range Tombstone Conversion: Faster Scans Over Long Runs of Deletes |
| Author | Josh Kang |
| Published | 2026-06-22 |
| Accessed | 2026-09-27 |
| URL | <https://rocksdb.org/blog/2026/06/22/range-tombstone-conversion.html> |
| Local archive | [`../../sources/20260927-rocksdb-range-tombstone-conversion.md`](../../sources/20260927-rocksdb-range-tombstone-conversion.md) |
| HTML SHA-256 | `ed7eab3c0f70ae6861d8cd0c94c8a04e16bfcd52a7faa57d7f6fe413ede8ca31` |

The rendered article was read in a browser and independently fetched as
Markdown. The local archive normalizes the technical text and preserves links
to the two upstream diagrams instead of copying the image assets.

## Implementation Evidence

The article points to
[RocksDB PR #14448](https://github.com/facebook/rocksdb/pull/14448). The PR API
reported head commit `3e5a85b2e5f879f1f59af0acd284fd5e2ff3f380` and a patch
SHA-256 of
`1ed0b7cbe19dab6cd1bb35b3b7fde9bf7dbb3903fd5a515898c51c06256d619a`.

The safety analysis also inspected RocksDB `main` at
`4052fccde9d9533fa9c57749b63ba3cda2c9b134`, whose `version.h` identifies
11.12.0. This later revision is intentional: the post says the initial PR had
follow-up fixes, and the current option contract makes those guards explicit.

Relevant source:

- [`AdvancedColumnFamilyOptions::min_tombstones_for_range_conversion`](https://github.com/facebook/rocksdb/blob/4052fccde9d9533fa9c57749b63ba3cda2c9b134/include/rocksdb/advanced_options.h#L1408-L1451)
- [`DBIter` completeness gate](https://github.com/facebook/rocksdb/blob/4052fccde9d9533fa9c57749b63ba3cda2c9b134/db/db_iter.cc#L119-L130)
- [`DBIter::MaybeInsertRangeTombstone`](https://github.com/facebook/rocksdb/blob/4052fccde9d9533fa9c57749b63ba3cda2c9b134/db/db_iter.cc#L1731-L1798)
- [`MemTable::AddLogicallyRedundantRangeTombstone`](https://github.com/facebook/rocksdb/blob/4052fccde9d9533fa9c57749b63ba3cda2c9b134/db/memtable.cc#L968-L1022)

Raw source SHA-256 values:

| Path | SHA-256 |
| --- | --- |
| `include/rocksdb/advanced_options.h` | `ed929a1d030a7ae70b6ac61f9ab94a767db1e24aa1e331e1b4c9113cdc5e0fc8` |
| `db/db_iter.cc` | `0b984c601194bcd7216a7b0fa546f6f8506c838e53a35f7abc75dadab8b1cdd2` |
| `db/memtable.cc` | `31ad9914206bf049dd3fc31d6d69d44d199571855e2ef3f8d7d9b64ef5a1afd6` |

The article's benchmark numbers were recorded as upstream evidence only. This
study does not reproduce a RocksDB `db_bench` run and makes no local throughput
claim.
