\set ON_ERROR_STOP on

DROP VIEW IF EXISTS loose_partitions;
DROP VIEW IF EXISTS distinct_partitions;
DROP TABLE IF EXISTS workflow_status;

CREATE TABLE workflow_status (
    id bigint NOT NULL,
    queue_name text NOT NULL,
    status text NOT NULL,
    queue_partition_key bigint
);

CREATE VIEW distinct_partitions AS
SELECT DISTINCT queue_partition_key AS pk
FROM workflow_status
WHERE queue_name = 'example_queue'
  AND status = 'ENQUEUED'
  AND queue_partition_key IS NOT NULL;

CREATE VIEW loose_partitions AS
WITH RECURSIVE partitions(pk) AS (
    SELECT min(queue_partition_key)
    FROM workflow_status
    WHERE queue_name = 'example_queue'
      AND status = 'ENQUEUED'
      AND queue_partition_key IS NOT NULL

    UNION ALL

    SELECT (
        SELECT min(queue_partition_key)
        FROM workflow_status
        WHERE queue_name = 'example_queue'
          AND status = 'ENQUEUED'
          AND queue_partition_key > partitions.pk
    )
    FROM partitions
    WHERE partitions.pk IS NOT NULL
)
SELECT pk
FROM partitions
WHERE pk IS NOT NULL;
