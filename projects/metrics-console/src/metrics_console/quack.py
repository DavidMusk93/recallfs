"""DuckDB Quack client for metrics master on d2.

Thread-local connections enable parallel quack_query round-trips.
"""

from __future__ import annotations

import os
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Callable, Iterable, TypeVar

import duckdb

DEFAULT_URI = os.environ.get("METRICS_QUACK_URI", "quack:10.37.125.152:9494")
DEFAULT_TOKEN_FILE = os.path.expanduser(
    os.environ.get(
        "METRICS_QUACK_TOKEN_FILE",
        "~/.config/metrics-console/quack.token",
    )
)

T = TypeVar("T")


class QuackClient:
    """Thread-safe Quack access via thread-local DuckDB connections."""

    def __init__(
        self,
        uri: str = DEFAULT_URI,
        token_file: str | Path = DEFAULT_TOKEN_FILE,
        token: str | None = None,
        max_workers: int = 8,
    ) -> None:
        self.uri = uri
        self._token = (token or "").strip() or self._load_token(token_file)
        self._local = threading.local()
        self._max_workers = max(1, int(max_workers))
        self._pool_lock = threading.Lock()
        self._pool: ThreadPoolExecutor | None = None

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
        try:
            con.execute("INSTALL quack FROM core")
        except Exception:
            try:
                con.execute("INSTALL quack FROM core_nightly")
            except Exception:
                pass
        con.execute("LOAD quack")
        return con

    def _conn(self) -> duckdb.DuckDBPyConnection:
        con = getattr(self._local, "con", None)
        if con is None:
            con = self._open()
            self._local.con = con
        return con

    def _executor(self) -> ThreadPoolExecutor:
        with self._pool_lock:
            if self._pool is None:
                self._pool = ThreadPoolExecutor(
                    max_workers=self._max_workers,
                    thread_name_prefix="quack",
                )
            return self._pool

    def query(self, sql: str) -> list[tuple[Any, ...]]:
        rel = self._conn().execute(
            "FROM quack_query(?, ?, token := ?, disable_ssl := true)",
            [self.uri, sql, self._token],
        )
        return rel.fetchall()

    def query_dicts(self, sql: str) -> list[dict[str, Any]]:
        rel = self._conn().execute(
            "FROM quack_query(?, ?, token := ?, disable_ssl := true)",
            [self.uri, sql, self._token],
        )
        cols = [d[0] for d in rel.description]
        rows = rel.fetchall()
        return [dict(zip(cols, row, strict=False)) for row in rows]

    def map(
        self,
        items: Iterable[tuple[str, Callable[[], T]]],
    ) -> dict[str, T | dict[str, str]]:
        """Run callables in parallel; return {key: result_or_{error}}."""
        jobs = list(items)
        if not jobs:
            return {}
        if len(jobs) == 1:
            key, fn = jobs[0]
            try:
                return {key: fn()}
            except Exception as exc:  # noqa: BLE001
                return {key: {"error": str(exc)}}

        out: dict[str, T | dict[str, str]] = {}
        futs = {self._executor().submit(fn): key for key, fn in jobs}
        for fut in as_completed(futs):
            key = futs[fut]
            try:
                out[key] = fut.result()
            except Exception as exc:  # noqa: BLE001
                out[key] = {"error": str(exc)}
        return out

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
