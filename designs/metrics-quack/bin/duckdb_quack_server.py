#!/usr/bin/env python3
"""DuckDB Quack metrics master server.

Opens master.duckdb, ATTACHes category DBs, applies schema, serves Quack on
configured URI. Designed for systemd (Type=simple).
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import time
from pathlib import Path

import duckdb

LOG = logging.getLogger("duckdb-quack")

DEFAULT_ROOT = Path(os.environ.get("DUCK_ROOT", "/root/data/duck"))
CATEGORIES = ("e2ed", "orchestrator", "ops")


def setup_logging(log_path: Path | None) -> None:
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stdout)]
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        handlers.append(logging.FileHandler(log_path))
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=handlers,
    )


def read_token(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(f"token file missing: {path}")
    token = path.read_text(encoding="utf-8").strip()
    if len(token) < 4:
        raise SystemExit("token too short (min 4 chars)")
    return token


def ensure_category_dbs(con: duckdb.DuckDBPyConnection, db_dir: Path) -> None:
    for name in CATEGORIES:
        path = db_dir / f"cat_{name}.duckdb"
        # CREATE empty DB file if missing by connecting once
        if not path.exists():
            tmp = duckdb.connect(str(path))
            tmp.close()
        con.execute(
            f"ATTACH '{path}' AS cat_{name} (READ_WRITE)"
        )
        LOG.info("attached cat_%s -> %s", name, path)


def apply_schema(con: duckdb.DuckDBPyConnection, schema_path: Path) -> None:
    sql = schema_path.read_text(encoding="utf-8")
    con.execute(sql)
    LOG.info("applied schema %s", schema_path)


def main() -> int:
    p = argparse.ArgumentParser(description="DuckDB Quack metrics server")
    p.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    p.add_argument(
        "--uri",
        default=os.environ.get("QUACK_URI", "quack:0.0.0.0:9494"),
    )
    p.add_argument(
        "--token-file",
        type=Path,
        default=None,
        help="default: $ROOT/secrets/quack.token",
    )
    p.add_argument(
        "--schema",
        type=Path,
        default=None,
        help="default: $ROOT/schema/001_init.sql",
    )
    args = p.parse_args()

    root: Path = args.root
    db_dir = root / "db"
    logs_dir = root / "logs"
    token_file = args.token_file or (root / "secrets" / "quack.token")
    schema_path = args.schema or (root / "schema" / "001_init.sql")
    master_path = db_dir / "master.duckdb"

    setup_logging(logs_dir / "duckdb-quack.log")
    db_dir.mkdir(parents=True, exist_ok=True)
    (root / "backup").mkdir(parents=True, exist_ok=True)

    token = read_token(token_file)
    LOG.info("duckdb=%s master=%s uri=%s", duckdb.__version__, master_path, args.uri)

    con = duckdb.connect(str(master_path))
    # extensions
    try:
        con.execute("INSTALL quack FROM core")
    except Exception as exc:  # noqa: BLE001
        LOG.warning("INSTALL quack FROM core: %s; trying core_nightly", exc)
        con.execute("INSTALL quack FROM core_nightly")
    con.execute("LOAD quack")

    ensure_category_dbs(con, db_dir)
    if schema_path.is_file():
        apply_schema(con, schema_path)
    else:
        LOG.warning("schema file missing: %s", schema_path)

    con.execute(
        "CALL quack_identify(name => 'metrics-master', provider => 'd2-host', "
        "hostname => 'd2', region => 'local', meta => '{\"role\":\"metrics-master\"}')"
    )

    # bind all interfaces for docker containers on host
    result = con.execute(
        "CALL quack_serve(?, token := ?, allow_other_hostname := true)",
        [args.uri, token],
    ).fetchall()
    # never log the auth token
    safe = [(r[0], r[1], "<redacted>") if len(r) >= 3 else r for r in result]
    LOG.info("quack_serve result: %s", safe)

    stop = False

    def _handle(signum: int, _frame: object) -> None:
        nonlocal stop
        LOG.info("signal %s received, stopping", signum)
        stop = True

    signal.signal(signal.SIGTERM, _handle)
    signal.signal(signal.SIGINT, _handle)

    while not stop:
        time.sleep(1)

    try:
        con.execute("CALL quack_stop(?)", [args.uri])
    except Exception as exc:  # noqa: BLE001
        LOG.warning("quack_stop: %s", exc)
    try:
        con.execute("CHECKPOINT")
    except Exception as exc:  # noqa: BLE001
        LOG.warning("checkpoint: %s", exc)
    con.close()
    LOG.info("server exited cleanly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
