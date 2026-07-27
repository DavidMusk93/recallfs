"""FastAPI app: metrics analysis API + Apple-style UI.

Performance model:
  - /api/snapshot?range=  full window load (parallel Quack)
  - /api/delta?range=&since=  only rows newer than since (cheap poll)
  - KPI counts are window-scoped (not full-table COUNT)
"""

from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .quack import QuackClient

STATIC_DIR = Path(__file__).resolve().parent / "static"

_MUTATION = re.compile(
    r"\b(insert|update|delete|drop|alter|create|attach|detach|copy|export|pragma|call|load|install)\b",
    re.I,
)

# UI range → DuckDB INTERVAL + chart bucket
RANGES: dict[str, dict[str, str]] = {
    "15m": {"interval": "INTERVAL 15 MINUTE", "bucket": "minute", "label": "15 分钟"},
    "1h": {"interval": "INTERVAL 1 HOUR", "bucket": "minute", "label": "1 小时"},
    "6h": {"interval": "INTERVAL 6 HOUR", "bucket": "minute", "label": "6 小时"},
    "24h": {"interval": "INTERVAL 24 HOUR", "bucket": "minute", "label": "24 小时"},
    "7d": {"interval": "INTERVAL 7 DAY", "bucket": "hour", "label": "7 天"},
}

app = FastAPI(title="Metrics Console", version="0.2.0")
_client: QuackClient | None = None


def get_client() -> QuackClient:
    global _client
    if _client is None:
        _client = QuackClient()
    return _client


def _range_spec(range_key: str) -> dict[str, str]:
    key = (range_key or "1h").strip().lower()
    if key not in RANGES:
        raise HTTPException(
            status_code=400,
            detail=f"invalid range; use one of {', '.join(RANGES)}",
        )
    return RANGES[key]


def _esc_ts_literal(ts: str) -> str:
    """Sanitize client-provided timestamp for SQL literal."""
    s = (ts or "").strip()
    if not re.fullmatch(r"[0-9T:\.\+\- Z]{8,40}", s):
        raise HTTPException(status_code=400, detail="invalid since timestamp")
    return s.replace("'", "")


class SqlBody(BaseModel):
    sql: str = Field(..., min_length=1, max_length=20000)
    limit: int = Field(200, ge=1, le=2000)


@app.get("/api/health")
def api_health() -> dict[str, Any]:
    try:
        return get_client().health()
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=503, detail=str(exc)) from exc


@app.get("/api/ranges")
def api_ranges() -> dict[str, Any]:
    return {
        "ranges": [
            {"id": k, "label": v["label"], "bucket": v["bucket"]}
            for k, v in RANGES.items()
        ]
    }


def _window_sql(interval: str) -> str:
    return f"cast(ts AS TIMESTAMP) > cast(now() AS TIMESTAMP) - {interval}"


def _build_snapshot(range_key: str) -> dict[str, Any]:
    spec = _range_spec(range_key)
    interval = spec["interval"]
    bucket = spec["bucket"]
    win = _window_sql(interval)
    c = get_client()
    t0 = time.perf_counter()

    count_jobs = [
        (
            "orch_api",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.api_requests WHERE {win}"
            )[0][0],
        ),
        (
            "orch_events",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.events WHERE {win}"
            )[0][0],
        ),
        (
            "orch_components",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.component_ops WHERE {win}"
            )[0][0],
        ),
        (
            "orch_scenarios",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.scenario_steps WHERE {win}"
            )[0][0],
        ),
        (
            "e2ed_lifecycle",
            lambda: c.query(
                f"SELECT count(*) FROM cat_e2ed.service_lifecycle WHERE {win}"
            )[0][0],
        ),
        (
            "e2ed_ticks",
            lambda: c.query(
                f"SELECT count(*) FROM cat_e2ed.daemon_ticks WHERE {win}"
            )[0][0],
        ),
        (
            "e2ed_events",
            lambda: c.query(f"SELECT count(*) FROM cat_e2ed.events WHERE {win}")[0][0],
        ),
        (
            "ops_events",
            lambda: c.query(f"SELECT count(*) FROM cat_ops.events WHERE {win}")[0][0],
        ),
    ]

    feed_jobs = [
        (
            "api",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, method, path, status, duration_ms
                FROM cat_orchestrator.api_requests
                WHERE {win}
                ORDER BY ts DESC
                LIMIT 40
                """
            ),
        ),
        (
            "lifecycle",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, service, action, ok, pid, error
                FROM cat_e2ed.service_lifecycle
                WHERE {win}
                ORDER BY ts DESC
                LIMIT 30
                """
            ),
        ),
        (
            "ticks",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, services_total, services_alive,
                       services_unhealthy, poll_interval_secs
                FROM cat_e2ed.daemon_ticks
                WHERE {win}
                ORDER BY ts DESC
                LIMIT 30
                """
            ),
        ),
        (
            "components",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, *
                FROM cat_orchestrator.component_ops
                WHERE {win}
                ORDER BY ts DESC
                LIMIT 30
                """
            ),
        ),
        (
            "latency",
            lambda: c.query_dicts(
                f"""
                SELECT
                  date_trunc('{bucket}', cast(ts AS TIMESTAMP)) AS bucket,
                  count(*) AS n,
                  avg(duration_ms) AS avg_ms,
                  max(duration_ms) AS max_ms,
                  quantile_cont(duration_ms, 0.95) AS p95_ms
                FROM cat_orchestrator.api_requests
                WHERE {win}
                GROUP BY 1
                ORDER BY 1
                """
            ),
        ),
        (
            "paths",
            lambda: c.query_dicts(
                f"""
                SELECT path, count(*) AS n,
                       avg(duration_ms) AS avg_ms,
                       max(duration_ms) AS max_ms,
                       quantile_cont(duration_ms, 0.95) AS p95_ms
                FROM cat_orchestrator.api_requests
                WHERE {win}
                GROUP BY path
                ORDER BY n DESC
                LIMIT 25
                """
            ),
        ),
    ]

    counts = c.map(count_jobs)
    feeds = c.map(feed_jobs)

    # max ts for client cursor
    max_ts = None
    for key in ("api", "lifecycle", "ticks", "components"):
        rows = feeds.get(key)
        if isinstance(rows, list) and rows:
            for r in rows:
                ts = r.get("ts")
                if ts and (max_ts is None or str(ts) > str(max_ts)):
                    max_ts = ts

    ms = round((time.perf_counter() - t0) * 1000, 1)
    return {
        "mode": "snapshot",
        "range": range_key,
        "range_label": spec["label"],
        "bucket": bucket,
        "cursor": max_ts,
        "query_ms": ms,
        "tables": counts,
        "recent": {
            "api": feeds.get("api", []),
            "lifecycle": feeds.get("lifecycle", []),
            "ticks": feeds.get("ticks", []),
            "components": feeds.get("components", []),
        },
        "latency": feeds.get("latency", []),
        "paths": feeds.get("paths", []),
        "server_time": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }


def _build_delta(range_key: str, since: str) -> dict[str, Any]:
    """Cheap incremental feed: only rows with ts > since, plus re-bucketed latency tip."""
    spec = _range_spec(range_key)
    interval = spec["interval"]
    bucket = spec["bucket"]
    win = _window_sql(interval)
    since_lit = _esc_ts_literal(since)
    since_pred = f"cast(ts AS TIMESTAMP) > cast('{since_lit}' AS TIMESTAMP)"
    c = get_client()
    t0 = time.perf_counter()

    feed_jobs = [
        (
            "api",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, method, path, status, duration_ms
                FROM cat_orchestrator.api_requests
                WHERE {since_pred}
                ORDER BY ts DESC
                LIMIT 80
                """
            ),
        ),
        (
            "lifecycle",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, service, action, ok, pid, error
                FROM cat_e2ed.service_lifecycle
                WHERE {since_pred}
                ORDER BY ts DESC
                LIMIT 40
                """
            ),
        ),
        (
            "ticks",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, services_total, services_alive,
                       services_unhealthy, poll_interval_secs
                FROM cat_e2ed.daemon_ticks
                WHERE {since_pred}
                ORDER BY ts DESC
                LIMIT 40
                """
            ),
        ),
        (
            "components",
            lambda: c.query_dicts(
                f"""
                SELECT cast(ts AS VARCHAR) AS ts, *
                FROM cat_orchestrator.component_ops
                WHERE {since_pred}
                ORDER BY ts DESC
                LIMIT 40
                """
            ),
        ),
    ]
    # Window KPIs + series recomputed in parallel (small; simpler than partial merge)
    agg_jobs = [
        (
            "orch_api",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.api_requests WHERE {win}"
            )[0][0],
        ),
        (
            "e2ed_lifecycle",
            lambda: c.query(
                f"SELECT count(*) FROM cat_e2ed.service_lifecycle WHERE {win}"
            )[0][0],
        ),
        (
            "e2ed_ticks",
            lambda: c.query(
                f"SELECT count(*) FROM cat_e2ed.daemon_ticks WHERE {win}"
            )[0][0],
        ),
        (
            "orch_components",
            lambda: c.query(
                f"SELECT count(*) FROM cat_orchestrator.component_ops WHERE {win}"
            )[0][0],
        ),
        (
            "latency",
            lambda: c.query_dicts(
                f"""
                SELECT
                  date_trunc('{bucket}', cast(ts AS TIMESTAMP)) AS bucket,
                  count(*) AS n,
                  avg(duration_ms) AS avg_ms,
                  max(duration_ms) AS max_ms,
                  quantile_cont(duration_ms, 0.95) AS p95_ms
                FROM cat_orchestrator.api_requests
                WHERE {win}
                GROUP BY 1
                ORDER BY 1
                """
            ),
        ),
        (
            "paths",
            lambda: c.query_dicts(
                f"""
                SELECT path, count(*) AS n,
                       avg(duration_ms) AS avg_ms,
                       max(duration_ms) AS max_ms,
                       quantile_cont(duration_ms, 0.95) AS p95_ms
                FROM cat_orchestrator.api_requests
                WHERE {win}
                GROUP BY path
                ORDER BY n DESC
                LIMIT 25
                """
            ),
        ),
    ]

    feeds = c.map(feed_jobs)
    aggs = c.map(agg_jobs)

    max_ts = since_lit
    for key in ("api", "lifecycle", "ticks", "components"):
        rows = feeds.get(key)
        if isinstance(rows, list):
            for r in rows:
                ts = r.get("ts")
                if ts and str(ts) > str(max_ts):
                    max_ts = ts

    ms = round((time.perf_counter() - t0) * 1000, 1)
    return {
        "mode": "delta",
        "range": range_key,
        "range_label": spec["label"],
        "bucket": bucket,
        "since": since_lit,
        "cursor": max_ts,
        "query_ms": ms,
        "tables": {
            "orch_api": aggs.get("orch_api"),
            "e2ed_lifecycle": aggs.get("e2ed_lifecycle"),
            "e2ed_ticks": aggs.get("e2ed_ticks"),
            "orch_components": aggs.get("orch_components"),
        },
        "added": {
            "api": feeds.get("api", []),
            "lifecycle": feeds.get("lifecycle", []),
            "ticks": feeds.get("ticks", []),
            "components": feeds.get("components", []),
        },
        "latency": aggs.get("latency", []),
        "paths": aggs.get("paths", []),
        "server_time": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }


@app.get("/api/snapshot")
def api_snapshot(
    range: str = Query("1h", alias="range"),
) -> dict[str, Any]:
    return _build_snapshot(range)


@app.get("/api/delta")
def api_delta(
    range: str = Query("1h", alias="range"),
    since: str = Query(..., min_length=8, max_length=40),
) -> dict[str, Any]:
    return _build_delta(range, since)


@app.get("/api/overview")
def api_overview(
    range: str = Query("1h", alias="range"),
    since: str | None = Query(None),
) -> dict[str, Any]:
    """Back-compat: full snapshot, or delta when since is set."""
    if since:
        return _build_delta(range, since)
    return _build_snapshot(range)


@app.get("/api/tables")
def api_tables() -> dict[str, Any]:
    c = get_client()
    rows = c.query_dicts(
        """
        SELECT database, schema, name AS table_name, column_names, column_types
        FROM (SHOW ALL TABLES)
        WHERE database LIKE 'cat_%' OR schema = 'meta'
        ORDER BY database, schema, name
        """
    )
    return {"tables": rows}


@app.get("/api/table/{category}/{table}")
def api_table_rows(
    category: str,
    table: str,
    limit: int = Query(100, ge=1, le=1000),
    range: str = Query("24h", alias="range"),
) -> dict[str, Any]:
    if not re.fullmatch(r"[a-zA-Z0-9_]+", category) or not re.fullmatch(
        r"[a-zA-Z0-9_]+", table
    ):
        raise HTTPException(status_code=400, detail="invalid identifier")
    if category == "meta":
        base = f"meta.{table}"
    else:
        base = f"cat_{category}.{table}"
    try:
        spec = _range_spec(range)
        win = _window_sql(spec["interval"])
        rows = get_client().query_dicts(
            f"SELECT * FROM {base} WHERE {win} ORDER BY ts DESC LIMIT {limit}"
        )
    except Exception:
        try:
            rows = get_client().query_dicts(f"SELECT * FROM {base} LIMIT {limit}")
        except Exception as exc2:  # noqa: BLE001
            raise HTTPException(status_code=400, detail=str(exc2)) from exc2
    return {"category": category, "table": table, "rows": rows, "range": range}


@app.post("/api/sql")
def api_sql(body: SqlBody) -> dict[str, Any]:
    sql = body.sql.strip().rstrip(";")
    if _MUTATION.search(sql):
        raise HTTPException(
            status_code=400,
            detail="mutation SQL blocked (read-only console)",
        )
    limited = sql
    if re.match(r"(?is)^\s*(with\b|select\b|from\b|show\b|describe\b|summarize\b)", sql):
        if not re.search(r"\blimit\b", sql, re.I):
            limited = f"SELECT * FROM ({sql}) AS _q LIMIT {body.limit}"
    try:
        rows = get_client().query_dicts(limited)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    cols = list(rows[0].keys()) if rows else []
    return {"columns": cols, "rows": rows, "count": len(rows)}


@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


if STATIC_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def main() -> None:
    import uvicorn

    host = os.environ.get("METRICS_CONSOLE_HOST", "127.0.0.1")
    port = int(os.environ.get("METRICS_CONSOLE_PORT", "9496"))
    uvicorn.run(
        "metrics_console.app:app",
        host=host,
        port=port,
        reload=False,
        factory=False,
    )


if __name__ == "__main__":
    main()
