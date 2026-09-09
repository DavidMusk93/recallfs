WITH
expected(pk) AS MATERIALIZED (
    SELECT generate_series(0, :ndv - 1)::bigint
),
ordinary AS MATERIALIZED (
    SELECT pk FROM distinct_partitions
),
loose AS MATERIALIZED (
    SELECT pk FROM loose_partitions
),
differences(pk) AS (
    (SELECT pk FROM ordinary EXCEPT SELECT pk FROM loose)
    UNION ALL
    (SELECT pk FROM loose EXCEPT SELECT pk FROM ordinary)
),
ordinary_errors(pk) AS (
    (SELECT pk FROM expected EXCEPT SELECT pk FROM ordinary)
    UNION ALL
    (SELECT pk FROM ordinary EXCEPT SELECT pk FROM expected)
),
loose_errors(pk) AS (
    (SELECT pk FROM expected EXCEPT SELECT pk FROM loose)
    UNION ALL
    (SELECT pk FROM loose EXCEPT SELECT pk FROM expected)
)
SELECT
    (SELECT count(*) FROM loose) AS result_count,
    (SELECT count(*) FROM differences) AS symmetric_difference_count,
    (SELECT count(*) FROM ordinary_errors) AS ordinary_expected_difference_count,
    (SELECT count(*) FROM loose_errors) AS loose_expected_difference_count;
