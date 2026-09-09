# PostgreSQL DISTINCT scaling demo

This Docker experiment compares two equivalent ways to enumerate active queue
partitions:

- ordinary `SELECT DISTINCT`, which consumes every matching index tuple;
- a recursive CTE that repeatedly seeks the next larger partition key.

The default matrix runs PostgreSQL 17.10 and 18.4. It varies both:

- total matching rows `N` while holding the number of distinct keys `D=10`;
- `D` while holding `N=1,000,000`.

This matters because the recursive query is attractive only when `D` is much
smaller than `N`.

The default image references are digest-pinned to the artifacts recorded in
`../evidence/image-digests.txt`. Set `PG_VERSIONS` to space-separated
`postgres` image suffixes and use a noncanonical `EVIDENCE_DIR` when testing
newer images.

## Requirements

- a running Docker daemon;
- `docker compose` or `docker-compose`;
- `jq`.

## Run

```bash
./scripts/run.sh
```

The run writes:

- `../evidence/benchmark.csv`: one `pgbench` run of seven transactions per case;
- `../evidence/correctness.csv`: symmetric set-difference result per data shape;
- `../evidence/plan-metrics.csv`: execution time, index rows, heap fetches,
  buffer counts, and plan node types;
- `../evidence/plans/*.json`: one full `EXPLAIN ANALYZE` plan per case;
- `../evidence/environment.txt`: host, Docker, PostgreSQL, and session
  settings;
- `../evidence/image-digests.txt`: immutable digests resolved from image tags;
- `../evidence/pgbench.log`: unmodified `pgbench` summaries.

The script uses a per-process Compose project, removes its database volume after
each PostgreSQL version, and publishes evidence only after the complete matrix
passes. A lock prevents concurrent writers from targeting the same evidence
directory.

## Smaller smoke run

```bash
PG_VERSIONS='18.4-bookworm' \
DEPTH_ROWS_PER_PARTITION='10 100' \
CARDINALITIES='100' \
CARDINALITY_TOTAL_ROWS=1000 \
BENCH_RUNS=1 \
EVIDENCE_DIR=/tmp/postgres-distinct-smoke \
./scripts/run.sh
```

## Measurement boundary

Both queries are wrapped in `count(*)`, so client result transfer does not
dominate high-cardinality cases. The session disables sequential scan,
parallel gather, and JIT to isolate index access behavior. Every load is
followed by `VACUUM (FREEZE, ANALYZE)`, allowing a true index-only scan with
zero heap fetches.

These are comparative measurements inside a Linux VM on macOS. They establish
algorithmic shape and plan behavior, not production latency or throughput.
