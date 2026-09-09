# Sources

## Primary Source

- Peter Kraft, “Postgres SELECT DISTINCT Does Not Scale,” DBOS,
  2026-08-10:
  https://www.dbos.dev/blog/postgres-select-distinct-does-not-scale
- Accessed: 2026-09-09
- Local structured archive:
  `../../sources/20260909-postgres-select-distinct-does-not-scale.md`
- Raw HTML SHA-256:
  `0b986ec69aef49cfc5dfbc4eff4ff297c47c72c9acafe06051ca443e312325b7`

The page rendered successfully in a browser. Its SQL, plan, and benchmark
figures are images rather than selectable code blocks. The local archive
transcribes the key SQL and summarizes the claims; raw Webflow HTML and
third-party images are not committed.

## PostgreSQL Sources

- PostgreSQL wiki, “Loose indexscan,” revision accessed 2026-09-09:
  https://wiki.postgresql.org/wiki/Loose_indexscan
- PostgreSQL 18 documentation, “Multicolumn Indexes”:
  https://www.postgresql.org/docs/18/indexes-multicolumn.html
- PostgreSQL 18 documentation, “WITH Queries”:
  https://www.postgresql.org/docs/18/queries-with.html
- PostgreSQL 18 documentation, “Statistics Used by the Planner”:
  https://www.postgresql.org/docs/18/planner-stats.html
- PostgreSQL CommitFest, “Index Skip Scan”:
  https://commitfest.postgresql.org/patch/1741/

The PostgreSQL wiki explicitly distinguishes loose index scan, which returns
grouping values, from PostgreSQL 18 skip scan, which uses missing leading-column
values to satisfy predicates on later index columns.

## Distributed Database Sources

- Citus 14, “Run SQL queries on Citus distributed tables”:
  https://learn.microsoft.com/en-us/postgresql/citus/reference-sql?view=citus-14
- OceanBase, “How We Approach Improving Distributed Query Performance”:
  https://oceanbase.github.io/docs/blogs/tech/refine-performance
- OceanBase, “Generate a distributed plan”:
  https://en.oceanbase.com/docs/common-oceanbase-database-10000000001105915
- YugabyteDB, “How to Select the First Row of Each Set of Grouped Rows Using
  GROUP BY”:
  https://www.yugabyte.com/blog/select-first-row-group-by-postgresql/

These sources support three separate deductions:

1. colocating an aggregation key with the distribution key can eliminate a
   repartition stage;
2. local partial aggregation can reduce network volume before global
   aggregation;
3. an index-aware distinct operator should execute near storage, because
   coordinator-driven one-key-at-a-time RPCs erase the benefit of skipping
   duplicate runs.
