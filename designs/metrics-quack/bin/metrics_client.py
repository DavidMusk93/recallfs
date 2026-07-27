#!/usr/bin/env python3
"""Quack metrics client using quack_query (ATTACH multi-catalog is flaky in 1.5.5).

Usage:
  metrics_client.py health
  metrics_client.py flush --file /path/events.ndjson --category e2ed --table events
  metrics_client.py insert --category e2ed --table events --json '{"event":"tick"}'
  metrics_client.py sql --sql 'SELECT count(*) FROM cat_ops.events'
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any

import duckdb

DEFAULT_URI = os.environ.get("METRICS_QUACK_URI", "quack:172.17.0.1:9494")
DEFAULT_TOKEN_FILE = os.environ.get(
    "METRICS_QUACK_TOKEN_FILE",
    "/root/data/duck/secrets/quack.token",
)

# column order must match 001_init.sql
TABLE_COLUMNS: dict[tuple[str, str], list[str]] = {
    ("e2ed", "events"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "source",
        "level",
        "event",
        "service",
        "pid",
        "attrs",
        "trace_id",
        "msg",
    ],
    ("e2ed", "service_lifecycle"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "service",
        "action",
        "ok",
        "pid",
        "pgid",
        "restarts",
        "duration_ms",
        "error",
        "attrs",
    ],
    ("e2ed", "health_checks"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "service",
        "url",
        "ok",
        "latency_ms",
        "error",
    ],
    ("e2ed", "daemon_ticks"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "poll_interval_secs",
        "services_total",
        "services_alive",
        "services_unhealthy",
        "tick_error",
        "attrs",
    ],
    ("orchestrator", "events"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "source",
        "level",
        "event",
        "component",
        "run_id",
        "attrs",
        "trace_id",
        "msg",
    ],
    ("orchestrator", "api_requests"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "method",
        "path",
        "status",
        "duration_ms",
        "client",
        "error",
        "attrs",
    ],
    ("orchestrator", "component_ops"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "component",
        "action",
        "ok",
        "duration_ms",
        "error",
        "attrs",
    ],
    ("orchestrator", "scenario_steps"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "run_id",
        "scenario_id",
        "step_id",
        "status",
        "attempt",
        "duration_ms",
        "error",
        "attrs",
    ],
    ("orchestrator", "job_events"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "job_id",
        "action",
        "ok",
        "duration_ms",
        "error",
        "attrs",
    ],
    ("ops", "events"): [
        "ts",
        "event_date",
        "host",
        "instance",
        "source",
        "level",
        "event",
        "attrs",
        "msg",
    ],
    ("ops", "write_batches"): [
        "ts",
        "event_date",
        "host",
        "source",
        "category",
        "table_name",
        "rows",
        "ok",
        "duration_ms",
        "error",
    ],
}


def load_token(path: str | Path) -> str:
    env_tok = os.environ.get("METRICS_QUACK_TOKEN")
    if env_tok:
        return env_tok.strip()
    p = Path(path)
    candidates = [
        p,
        Path("/workspace/tide-dev-agent@tide-vs-ck/.metrics/quack.token"),
        Path("/workspace/.metrics/quack.token"),
        Path("/root/data/duck/secrets/quack.token"),
    ]
    for c in candidates:
        if c.is_file():
            return c.read_text(encoding="utf-8").strip()
    raise SystemExit(f"token not found (tried {candidates})")


def client_con() -> duckdb.DuckDBPyConnection:
    con = duckdb.connect()
    # Prefer already-installed extension path (offline / container-friendly).
    ext_candidates = [
        os.environ.get("DUCKDB_QUACK_EXTENSION", ""),
        "/root/.duckdb/extensions/v1.5.5/linux_amd64/quack.duckdb_extension",
        str(Path.home() / ".duckdb/extensions/v1.5.5/linux_amd64/quack.duckdb_extension"),
    ]
    loaded = False
    for path in ext_candidates:
        if path and Path(path).is_file():
            try:
                con.execute(f"LOAD '{path}'")
                loaded = True
                break
            except Exception:
                continue
    if not loaded:
        try:
            con.execute("INSTALL quack FROM core")
        except Exception:
            try:
                con.execute("INSTALL quack FROM core_nightly")
            except Exception:
                pass
        con.execute("LOAD quack")
    return con


def quack_query(
    con: duckdb.DuckDBPyConnection, uri: str, token: str, sql: str
) -> list[Any]:
    return con.execute(
        "FROM quack_query(?, ?, token := ?, disable_ssl := true)",
        [uri, sql, token],
    ).fetchall()


def enrich_row(row: dict[str, Any], category: str | None = None) -> dict[str, Any]:
    now = datetime.now(timezone.utc)
    if row.get("ts") is None:
        row["ts"] = now.isoformat()
    ts = row["ts"]
    if row.get("event_date") is None:
        try:
            if isinstance(ts, str):
                d = datetime.fromisoformat(ts.replace("Z", "+00:00")).date()
            else:
                d = date.today()
            row["event_date"] = d.isoformat()
        except Exception:
            row["event_date"] = date.today().isoformat()
    row.setdefault("host", os.environ.get("HOSTNAME") or os.uname().nodename)
    row.setdefault("instance", os.environ.get("METRICS_INSTANCE", "tide-vs-ck"))
    if category:
        row.setdefault("source", category if category != "ops" else "ops")
    row.setdefault("level", "info")
    return row


def sql_lit(v: Any) -> str:
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return str(v)
    if isinstance(v, (dict, list)):
        s = json.dumps(v, ensure_ascii=False).replace("'", "''")
        return f"'{s}'::JSON"
    s = str(v).replace("'", "''")
    return f"'{s}'"


def build_insert_sql(category: str, table: str, rows: list[dict[str, Any]]) -> str:
    key = (category, table)
    cols = TABLE_COLUMNS.get(key)
    if not cols:
        raise SystemExit(f"unknown table {category}.{table}")
    fq = f"cat_{category}.{table}"
    value_groups = []
    for row in rows:
        vals = [sql_lit(row.get(c)) for c in cols]
        value_groups.append("(" + ", ".join(vals) + ")")
    col_list = ", ".join(cols)
    return f"INSERT INTO {fq} ({col_list}) VALUES " + ", ".join(value_groups)


def insert_rows(
    con: duckdb.DuckDBPyConnection,
    uri: str,
    token: str,
    category: str,
    table: str,
    rows: list[dict[str, Any]],
) -> int:
    if not rows:
        return 0
    # batch in chunks to keep query size reasonable
    chunk = 50
    total = 0
    for i in range(0, len(rows), chunk):
        part = rows[i : i + chunk]
        sql = build_insert_sql(category, table, part)
        quack_query(con, uri, token, sql)
        total += len(part)
    return total


def cmd_health(args: argparse.Namespace) -> int:
    token = load_token(args.token_file)
    t0 = time.perf_counter()
    con = client_con()
    try:
        # avoid whoami() TIMESTAMPTZ -> pytz dependency on thin clients
        rows = quack_query(con, args.uri, token, "SELECT 1 AS ok")
        ms = (time.perf_counter() - t0) * 1000
        print(
            json.dumps(
                {"ok": True, "latency_ms": round(ms, 2), "probe": rows, "uri": args.uri},
                default=str,
            )
        )
        return 0
    except Exception as exc:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    finally:
        con.close()


def cmd_flush(args: argparse.Namespace) -> int:
    path = Path(args.file)
    if not path.is_file():
        print(json.dumps({"ok": True, "rows": 0, "reason": "missing"}))
        return 0
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(enrich_row(json.loads(line), args.category))
        except json.JSONDecodeError:
            continue
    if not rows:
        print(json.dumps({"ok": True, "rows": 0}))
        return 0
    token = load_token(args.token_file)
    t0 = time.perf_counter()
    con = client_con()
    try:
        n = insert_rows(con, args.uri, token, args.category, args.table, rows)
        ms = (time.perf_counter() - t0) * 1000
        path.rename(path.with_suffix(path.suffix + f".sent.{int(time.time())}"))
        print(json.dumps({"ok": True, "rows": n, "duration_ms": round(ms, 2)}))
        return 0
    except Exception as exc:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    finally:
        con.close()


def cmd_insert(args: argparse.Namespace) -> int:
    row = enrich_row(json.loads(args.json), args.category)
    token = load_token(args.token_file)
    con = client_con()
    try:
        n = insert_rows(con, args.uri, token, args.category, args.table, [row])
        print(json.dumps({"ok": True, "rows": n}))
        return 0
    except Exception as exc:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    finally:
        con.close()


def cmd_sql(args: argparse.Namespace) -> int:
    token = load_token(args.token_file)
    con = client_con()
    try:
        rows = quack_query(con, args.uri, token, args.sql)
        print(json.dumps({"ok": True, "rows": rows}, default=str))
        return 0
    except Exception as exc:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    finally:
        con.close()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--uri", default=DEFAULT_URI)
    p.add_argument("--token-file", default=DEFAULT_TOKEN_FILE)
    sub = p.add_subparsers(dest="cmd", required=True)

    h = sub.add_parser("health")
    h.set_defaults(func=cmd_health)

    f = sub.add_parser("flush")
    f.add_argument("--file", required=True)
    f.add_argument("--category", required=True, choices=["e2ed", "orchestrator", "ops"])
    f.add_argument("--table", required=True)
    f.set_defaults(func=cmd_flush)

    i = sub.add_parser("insert")
    i.add_argument("--category", required=True, choices=["e2ed", "orchestrator", "ops"])
    i.add_argument("--table", required=True)
    i.add_argument("--json", required=True)
    i.set_defaults(func=cmd_insert)

    s = sub.add_parser("sql")
    s.add_argument("--sql", required=True)
    s.set_defaults(func=cmd_sql)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
