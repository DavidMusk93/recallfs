# Source

| Field | Value |
| --- | --- |
| URL | `https://veitner.bearblog.dev/radix-top-k/` |
| Access Date | `2026-06-11` |
| Archive | `learning/sources/20260611-radix-top-k.md` |
| Reference Implementation | `https://github.com/duckdb/duckdb` |
| Local Analysis Copy | `.tmp/duckdb` |
| Topic | Radix Top-K selection and DuckDB radix key ideas |

## DuckDB Files Inspected

| File | Relevant Idea |
| --- | --- |
| `.tmp/duckdb/src/include/duckdb/common/radix.hpp` | Encodes typed values into byte-comparable radix keys. |
| `.tmp/duckdb/src/include/duckdb/execution/index/art/art_key.hpp` | Uses `Radix::EncodeData` to build ART keys. |
| `.tmp/duckdb/src/execution/operator/order/physical_top_n.cpp` | Implements DuckDB TopN with sort keys and a bounded heap. |
| `.tmp/duckdb/src/common/radix_partitioning.cpp` | Uses radix bits to compute partition indexes from hash values. |

## Scope Note

DuckDB does not expose a standalone `radix top-k tree` implementation. The demo extracts the reusable ideas instead:

- byte-comparable key encoding from DuckDB radix utilities,
- radix/ART-style traversal over key prefixes,
- Top-K boundary pruning from the Radix Top-K article,
- validation against a full-sort baseline.
