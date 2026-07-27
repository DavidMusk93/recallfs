"""DuckDB Quack client for metrics master on d2."""

from __future__ import annotations

import os
import threading
from pathlib import Path
from typing import Any

import duckdb

DEFAULT_URI = os.environ.get("METRICS_QUACK_URI", "quack:10.37.125.152:9494")
DEFAULT_TOKEN_FILE = os.path.expanduser(
    os.environ.get(
        "METRICS_QUACK_TOKEN_FILE",
        "~/.config/metrics-console/quack.token",
    )
)


class QuackClient:
    """Thread-safe thin wrapper around quack_query (ATTACH multi-catalog is flaky)."""

    def __init__(
        self,
        uri: str = DEFAULT_URI,
        token_file: str | Path = DEFAULT_TOKEN_FILE,
        token: str | None = None,
    ) -> None:
        self.uri = uri
        self._token = (token or "").strip() or self._load_token(token_file)
        self._lock = threading.Lock()
        self._con = self._open()

    @staticmethod
    def _load_token(path: str | Path) -> str:
        p = Path(path).expanduser()
        if not p.is_file():
            raise FileNotFoundError(
                f"Quack token not found at {p}. "
                "Copy from d2:/root/data/duck/secrets/quack.token"
            )
        tok = p.read_text(encoding="utf-8").strip()
        if len(tok) < 4:
            raise ValueError("token too short")
        return tok

    def _open(self) -> duckdb.DuckDBPyConnection:
        con = duckdb.connect()
        ext = Path.home() / ".duckdb/extensions/v1.5.5"
        # Prefer LOAD after INSTALL; tolerate offline install miss if already present.
        try:
            con.execute("INSTALL quack FROM core")
        except Exception:
            try:
                con.execute("INSTALL quack FROM core_nightly")
            except Exception:
                pass
        con.execute("LOAD quack")
        # force plain HTTP for LAN
        return con

    def query(self, sql: str) -> list[tuple[Any, ...]]:
        with self._lock:
            return self._con.execute(
                "FROM quack_query(?, ?, token := ?, disable_ssl := true)",
                [self.uri, sql, self._token],
            ).fetchall()

    def query_dicts(self, sql: str) -> list[dict[str, Any]]:
        with self._lock:
            rel = self._con.execute(
                "FROM quack_query(?, ?, token := ?, disable_ssl := true)",
                [self.uri, sql, self._token],
            )
            cols = [d[0] for d in rel.description]
            rows = rel.fetchall()
        return [dict(zip(cols, row, strict=False)) for row in rows]

    def health(self) -> dict[str, Any]:
        import time

        t0 = time.perf_counter()
        rows = self.query("SELECT 1 AS ok")
        ms = (time.perf_counter() - t0) * 1000
        return {
            "ok": bool(rows),
            "latency_ms": round(ms, 2),
            "uri": self.uri,
            "probe": rows,
        }
