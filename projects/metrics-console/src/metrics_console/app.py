"""FastAPI app: metrics analysis API + static Apple-style UI."""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .quack import QuackClient

STATIC_DIR = Path(__file__).resolve().parent / "static"

# Read-only SQL guard: block obvious mutation keywords.
_MUTATION = re.compile(
    r"\b(insert|update|delete|drop|alter|create|attach|detach|copy|export|pragma|call|load|install)\b",
    re.I,
)

app = FastAPI(title="Metrics Console", version="0.1.0")
_client: QuackClient | None = None


def get_client() -> QuackClient:
    global _client
    if _client is None:
        _client = QuackClient()
    return _client


class SqlBody(BaseModel):
    sql: str = Field(..., min_length=1, max_length=20000)
    limit: int = Field(200, ge=1, le=2000)


@app.get("/api/health")
def api_health() -> dict[str, Any]:
    try:
        return get_client().health()
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=503, detail=str(exc)) from exc


@app.get("/api/overview")
def api_overview() -> dict[str, Any]:
    c = get_client()
    out: dict[str, Any] = {"tables": {}, "recent": {}}
    counts = [
        ("e2ed_events", "SELECT count(*) FROM cat_e2ed.events"),
        ("e2ed_lifecycle", "SELECT count(*) FROM cat_e2ed.service_lifecycle"),
        ("e2ed_ticks", "SELECT count(*) FROM cat_e2ed.daemon_ticks"),
        ("orch_events", "SELECT count(*) FROM cat_orchestrator.events"),
        ("orch_api", "SELECT count(*) FROM cat_orchestrator.api_requests"),
        ("orch_components", "SELECT count(*) FROM cat_orchestrator.component_ops"),
        ("orch_scenarios", "SELECT count(*) FROM cat_orchestrator.scenario_steps"),
        ("ops_events", "SELECT count(*) FROM cat_ops.events"),
    ]
    for key, sql in counts:
        try:
            out["tables"][key] = c.query(sql)[0][0]
        except Exception as exc:  # noqa: BLE001
            out["tables"][key] = {"error": str(exc)}

    try:
        out["recent"]["api"] = c.query_dicts(
            """
            SELECT cast(ts AS VARCHAR) AS ts, method, path, status, duration_ms
            FROM cat_orchestrator.api_requests
            ORDER BY ts DESC
            LIMIT 30
            """
        )
    except Exception as exc:  # noqa: BLE001
        out["recent"]["api"] = {"error": str(exc)}

    try:
        out["recent"]["lifecycle"] = c.query_dicts(
            """
            SELECT cast(ts AS VARCHAR) AS ts, service, action, ok, pid, error
            FROM cat_e2ed.service_lifecycle
            ORDER BY ts DESC
            LIMIT 20
            """
        )
    except Exception as exc:  # noqa: BLE001
        out["recent"]["lifecycle"] = {"error": str(exc)}

    try:
        out["recent"]["ticks"] = c.query_dicts(
            """
            SELECT cast(ts AS VARCHAR) AS ts, services_total, services_alive,
                   services_unhealthy, poll_interval_secs
            FROM cat_e2ed.daemon_ticks
            ORDER BY ts DESC
            LIMIT 20
            """
        )
    except Exception as exc:  # noqa: BLE001
        out["recent"]["ticks"] = {"error": str(exc)}

    try:
        # cast away TIMESTAMPTZ to avoid client-side pytz dependency on thin envs
        out["latency"] = c.query_dicts(
            """
            SELECT
              date_trunc('minute', cast(ts AS TIMESTAMP)) AS minute,
              count(*) AS n,
              avg(duration_ms) AS avg_ms,
              max(duration_ms) AS max_ms,
              quantile_cont(duration_ms, 0.95) AS p95_ms
            FROM cat_orchestrator.api_requests
            WHERE cast(ts AS TIMESTAMP) > cast(now() AS TIMESTAMP) - INTERVAL 6 HOUR
            GROUP BY 1
            ORDER BY 1
            """
        )
    except Exception as exc:  # noqa: BLE001
        out["latency"] = {"error": str(exc)}

    try:
        out["paths"] = c.query_dicts(
            """
            SELECT path, count(*) AS n,
                   avg(duration_ms) AS avg_ms,
                   max(duration_ms) AS max_ms
            FROM cat_orchestrator.api_requests
            WHERE cast(ts AS TIMESTAMP) > cast(now() AS TIMESTAMP) - INTERVAL 24 HOUR
            GROUP BY path
            ORDER BY n DESC
            LIMIT 20
            """
        )
    except Exception as exc:  # noqa: BLE001
        out["paths"] = {"error": str(exc)}

    return out


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
) -> dict[str, Any]:
    if not re.fullmatch(r"[a-zA-Z0-9_]+", category) or not re.fullmatch(
        r"[a-zA-Z0-9_]+", table
    ):
        raise HTTPException(status_code=400, detail="invalid identifier")
    # meta tables live on master; category tables are cat_<name>.<table>
    if category == "meta":
        base = f"meta.{table}"
    else:
        base = f"cat_{category}.{table}"
    try:
        rows = get_client().query_dicts(
            f"SELECT * FROM {base} ORDER BY ts DESC LIMIT {limit}"
        )
    except Exception:
        try:
            rows = get_client().query_dicts(f"SELECT * FROM {base} LIMIT {limit}")
        except Exception as exc2:  # noqa: BLE001
            raise HTTPException(status_code=400, detail=str(exc2)) from exc2
    return {"category": category, "table": table, "rows": rows}


@app.post("/api/sql")
def api_sql(body: SqlBody) -> dict[str, Any]:
    sql = body.sql.strip().rstrip(";")
    if _MUTATION.search(sql):
        raise HTTPException(
            status_code=400,
            detail="mutation SQL blocked (read-only console)",
        )
    # wrap with limit if pure select without limit
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
