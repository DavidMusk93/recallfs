# Exploration Log

## 1. Research Questions

The article's benchmark answers one narrow question: what happens when a
PostgreSQL B-tree contains long runs of duplicate values and `DISTINCT` must
return only a few keys?

This study split that into four questions:

1. Does an index-only `DISTINCT` consume all `N` matching index tuples?
2. Does a recursive loose-index-scan emulation consume approximately `D`
   tuples, where `D` is the number of distinct keys?
3. Does PostgreSQL 18 multicolumn skip scan change this behavior?
4. At what point does the `D`-seek workaround become worse than one sequential
   pass over `N` tuples?

## 2. Source Inspection

The DBOS article rendered successfully on 2026-09-09. Its SQL, plan, and
benchmark are embedded as images. The images were inspected directly and the
two queries were transcribed into the demo. Raw HTML and images were kept under
`.tmp/`; their hashes are in `evidence/source-digests.txt`.

The article correctly separates PostgreSQL 18 skip scan from loose index scan.
The PostgreSQL documentation confirms that skip scan synthesizes equality
constraints for missing leading index columns so predicates on later columns
can skip irrelevant index ranges. The PostgreSQL wiki defines loose index scan
as jumping from one distinct leading-key value to the next. These are different
operator contracts.

## 3. Experimental Design

The Docker matrix uses official `postgres:17.10-bookworm` and
`postgres:18.4-bookworm` images.

The schema mirrors the article:

```text
index key = (queue_name, status, queue_partition_key)
predicate = queue_name = constant AND status = constant
result    = DISTINCT queue_partition_key
```

Two independent axes prevent a one-sided conclusion:

| Axis | Fixed | Varied | Purpose |
| --- | --- | --- | --- |
| Depth | `D=10` | `N=1K..10M` | Test whether work follows matching rows |
| Cardinality | `N=1M` | `D=100..100K` | Find the repeated-seek crossover |

Controls:

- `VACUUM (FREEZE, ANALYZE)` enables zero-heap-fetch index-only scans.
- `enable_seqscan=off` isolates the best available B-tree path.
- parallel gather and JIT are disabled to isolate access-method behavior.
- both queries are wrapped in `count(*)` to remove client result-transfer cost.
- every case checks that both queries return exactly the configured `D`.
- `pgbench` performs one warmup and seven measured transactions.
- a separate `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` captures operator work.

The benchmark is a Linux VM on macOS. It is evidence for algorithmic shape and
plan behavior, not a production latency claim.

## 4. Setup Failures and Fixes

The host initially had a Docker CLI but no running daemon. Colima 0.10.3 was
installed and started with 4 vCPUs, 6 GiB RAM, and a 50 GiB disk.

The existing Docker configuration referenced an uninstalled
`docker-credential-desktop`. The run used an empty config under `.tmp/` and the
Colima socket, avoiding changes to user credentials.

The existing `docker compose` plugin symlink also pointed into an absent
Docker Desktop installation. Homebrew `docker-compose` 5.5.1 was installed;
the runner supports both plugin and standalone forms.

PostgreSQL 18 changed the official image's data-directory layout. Mounting only
`/var/lib/postgresql/data` caused the container to reject the old layout. The
Compose file now mounts `/var/lib/postgresql`, which works for both major
versions and follows the image's upgrade-safe recommendation.

## 5. Mechanism Observed

For ordinary `DISTINCT`, plans were:

```text
Aggregate
  Unique
    Index Only Scan
```

The index scan emitted `N` rows and `Unique` reduced them to `D`. Heap fetches
were zero, so the linear behavior is not caused by table access.

For the recursive query, plans were:

```text
Aggregate
  Recursive Union
    first min() index seek
    repeated next-greater min() index seek
  CTE Scan
```

The scans emitted approximately one row per distinct key plus the terminating
miss. PostgreSQL 18's `Index Searches` metric reports one search for ordinary
`DISTINCT` and approximately `D+1` searches for the recursive query. This is
direct evidence that PostgreSQL 18 did not turn this query into a native loose
index scan.

## 6. Interpretation Boundary

The article's result should not be generalized to “all DISTINCT is
unscalable.” Hash distinct, sorted unique, parallel local distinct, partition
pruning, and colocated distributed aggregation are valid at scale. The precise
failure mode is:

> Low output cardinality does not reduce scan work when the access method can
> only iterate every matching tuple.

The recursive workaround replaces one sequential `O(N)` index walk with about
`D+1` B-tree searches. Its more accurate complexity is `O(D log N)`, with a
small cached-tree constant in this experiment. It is advantageous only while
`D` is sufficiently smaller than `N`.

In a distributed engine, a SQL-level recursive formulation can be actively
harmful if each iteration becomes a coordinator-to-tablet RPC. A native
storage-side iterator must batch the seeks and expose continuation state.
