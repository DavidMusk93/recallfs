-- Metrics Quack schema v1
-- Applied on master.duckdb; category DBs ATTACHed as cat_e2ed / cat_orchestrator / cat_ops

-- Meta on master
CREATE SCHEMA IF NOT EXISTS meta;
CREATE TABLE IF NOT EXISTS meta.schema_migrations (
    version VARCHAR PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS meta.categories (
    name VARCHAR PRIMARY KEY,
    db_path VARCHAR NOT NULL,
    description VARCHAR,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ---------- cat_e2ed ----------
CREATE TABLE IF NOT EXISTS cat_e2ed.events (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    source VARCHAR NOT NULL DEFAULT 'e2ed',
    level VARCHAR NOT NULL DEFAULT 'info',
    event VARCHAR NOT NULL,
    service VARCHAR,
    pid INTEGER,
    attrs JSON,
    trace_id VARCHAR,
    msg VARCHAR
);

CREATE TABLE IF NOT EXISTS cat_e2ed.service_lifecycle (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    service VARCHAR NOT NULL,
    action VARCHAR NOT NULL, -- start|stop|restart|adopt|ensure|roll
    ok BOOLEAN NOT NULL,
    pid INTEGER,
    pgid INTEGER,
    restarts UBIGINT,
    duration_ms DOUBLE,
    error VARCHAR,
    attrs JSON
);

CREATE TABLE IF NOT EXISTS cat_e2ed.health_checks (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    service VARCHAR NOT NULL,
    url VARCHAR,
    ok BOOLEAN NOT NULL,
    latency_ms DOUBLE,
    error VARCHAR
);

CREATE TABLE IF NOT EXISTS cat_e2ed.daemon_ticks (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    poll_interval_secs INTEGER,
    services_total INTEGER,
    services_alive INTEGER,
    services_unhealthy INTEGER,
    tick_error VARCHAR,
    attrs JSON
);

-- ---------- cat_orchestrator ----------
CREATE TABLE IF NOT EXISTS cat_orchestrator.events (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    source VARCHAR NOT NULL DEFAULT 'orchestrator',
    level VARCHAR NOT NULL DEFAULT 'info',
    event VARCHAR NOT NULL,
    component VARCHAR,
    run_id VARCHAR,
    attrs JSON,
    trace_id VARCHAR,
    msg VARCHAR
);

CREATE TABLE IF NOT EXISTS cat_orchestrator.api_requests (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    method VARCHAR NOT NULL,
    path VARCHAR NOT NULL,
    status INTEGER,
    duration_ms DOUBLE,
    client VARCHAR,
    error VARCHAR,
    attrs JSON
);

CREATE TABLE IF NOT EXISTS cat_orchestrator.component_ops (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    component VARCHAR NOT NULL,
    action VARCHAR NOT NULL, -- start|stop|restart|topology_start|topology_stop
    ok BOOLEAN NOT NULL,
    duration_ms DOUBLE,
    error VARCHAR,
    attrs JSON
);

CREATE TABLE IF NOT EXISTS cat_orchestrator.scenario_steps (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    run_id VARCHAR NOT NULL,
    scenario_id VARCHAR,
    step_id VARCHAR NOT NULL,
    status VARCHAR NOT NULL, -- running|ok|failed|skipped
    attempt INTEGER,
    duration_ms DOUBLE,
    error VARCHAR,
    attrs JSON
);

CREATE TABLE IF NOT EXISTS cat_orchestrator.job_events (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    job_id VARCHAR NOT NULL,
    action VARCHAR NOT NULL, -- list|cancel|cleanup|submit
    ok BOOLEAN NOT NULL,
    duration_ms DOUBLE,
    error VARCHAR,
    attrs JSON
);

-- ---------- cat_ops (cross-cutting ops / system) ----------
CREATE TABLE IF NOT EXISTS cat_ops.events (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    instance VARCHAR,
    source VARCHAR NOT NULL,
    level VARCHAR NOT NULL DEFAULT 'info',
    event VARCHAR NOT NULL,
    attrs JSON,
    msg VARCHAR
);

CREATE TABLE IF NOT EXISTS cat_ops.write_batches (
    ts TIMESTAMPTZ NOT NULL,
    event_date DATE NOT NULL,
    host VARCHAR,
    source VARCHAR,
    category VARCHAR,
    table_name VARCHAR,
    rows INTEGER,
    ok BOOLEAN,
    duration_ms DOUBLE,
    error VARCHAR
);

INSERT INTO meta.schema_migrations VALUES ('001_init', now())
ON CONFLICT DO NOTHING;

INSERT INTO meta.categories VALUES
    ('e2ed', '/root/data/duck/db/cat_e2ed.duckdb', 'e2ed daemon lifecycle', now()),
    ('orchestrator', '/root/data/duck/db/cat_orchestrator.duckdb', 'e2e-orchestrator API and scenarios', now()),
    ('ops', '/root/data/duck/db/cat_ops.duckdb', 'cross-cutting ops metrics', now())
ON CONFLICT DO NOTHING;
