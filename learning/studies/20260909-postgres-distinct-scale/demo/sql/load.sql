\set ON_ERROR_STOP on

TRUNCATE workflow_status;
DROP INDEX IF EXISTS idx_workflow_status_partition_dequeue;

INSERT INTO workflow_status (id, queue_name, status, queue_partition_key)
SELECT
    g,
    'example_queue',
    'ENQUEUED',
    (g - 1) % :ndv
FROM generate_series(1, :total_rows) AS rows(g);

INSERT INTO workflow_status (id, queue_name, status, queue_partition_key)
VALUES
    (:total_rows + 1, 'other_queue', 'ENQUEUED', :ndv + 101),
    (:total_rows + 2, 'example_queue', 'PENDING', :ndv + 102),
    (:total_rows + 3, 'example_queue', 'COMPLETED', :ndv + 103),
    (:total_rows + 4, 'example_queue', 'ENQUEUED', NULL);

CREATE INDEX idx_workflow_status_partition_dequeue
    ON workflow_status (queue_name, status, queue_partition_key)
    WHERE status IN ('ENQUEUED', 'PENDING')
      AND queue_partition_key IS NOT NULL;

VACUUM (FREEZE, ANALYZE) workflow_status;
