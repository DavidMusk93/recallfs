#!/usr/bin/env python3
"""HTTP Port Manager — multi-port static servers + Web UI + SSE metrics.

Single-process supervisor for directory static HTTP services.
Control plane default: 0.0.0.0:9090

macOS note: LaunchAgents cannot read ~/Documents (TCC). When a service root is
unreadable, content is rsync-mirrored into APP_DIR/mirrors/<id>/ for serving.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import traceback
import uuid
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

APP_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = APP_DIR / "config.json"
STATIC_DIR = APP_DIR / "static"
MIRRORS_DIR = APP_DIR / "mirrors"
LAB_TELEMETRY_DIR = APP_DIR / "lab_telemetry"
ID_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$")
RSYNC_EXCLUDES = [".git", "target", ".tmp", "__pycache__", ".DS_Store", "*.pyc"]


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def port_listening(host: str, port: int, timeout: float = 0.15) -> bool:
    probe_host = "127.0.0.1" if host in ("0.0.0.0", "::", "") else host
    try:
        with socket.create_connection((probe_host, port), timeout=timeout):
            return True
    except OSError:
        return False


def path_readable(path: Path) -> bool:
    try:
        if not path.is_dir():
            return False
        next(path.iterdir(), None)
        # try reading one file if present
        for p in path.iterdir():
            if p.is_file():
                with p.open("rb") as f:
                    f.read(1)
                break
        return True
    except OSError:
        return False


def mirror_root(source: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if shutil.which("rsync"):
        cmd = ["rsync", "-a", "--delete"]
        for ex in RSYNC_EXCLUDES:
            cmd.extend(["--exclude", ex])
        cmd.extend([str(source) + "/", str(dest) + "/"])
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return
    # fallback: shutil copytree
    if dest.exists():
        shutil.rmtree(dest)
    def ignore(dirpath: str, names: list[str]) -> set[str]:
        skipped = set()
        base = Path(dirpath).name
        for n in names:
            if n in {".git", "target", ".tmp", "__pycache__", ".DS_Store"}:
                skipped.add(n)
            if n.endswith(".pyc"):
                skipped.add(n)
        return skipped
    shutil.copytree(source, dest, ignore=ignore)


class Metrics:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.requests_total = 0
        self.bytes_sent = 0
        self.errors_total = 0
        self.status_2xx = 0
        self.status_3xx = 0
        self.status_4xx = 0
        self.status_5xx = 0
        self.last_request_at: str | None = None
        self.last_path: str | None = None
        self.last_status: int | None = None
        self.last_error: str | None = None
        self.note: str | None = None
        self.last_sync_at: str | None = None
        self.mirrored = False
        self.serve_root: str | None = None

    def record(self, path: str, status: int, nbytes: int) -> None:
        with self._lock:
            self.requests_total += 1
            self.bytes_sent += max(0, nbytes)
            self.last_request_at = utc_now_iso()
            self.last_path = path
            self.last_status = status
            if 200 <= status < 300:
                self.status_2xx += 1
            elif 300 <= status < 400:
                self.status_3xx += 1
            elif 400 <= status < 500:
                self.status_4xx += 1
                self.errors_total += 1
            else:
                self.status_5xx += 1
                self.errors_total += 1

    def set_error(self, msg: str) -> None:
        with self._lock:
            self.last_error = msg
            self.errors_total += 1

    def set_serve_info(self, serve_root: str, mirrored: bool, synced: bool = False) -> None:
        with self._lock:
            self.serve_root = serve_root
            self.mirrored = mirrored
            if synced:
                self.last_sync_at = utc_now_iso()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "requests_total": self.requests_total,
                "bytes_sent": self.bytes_sent,
                "errors_total": self.errors_total,
                "status_2xx": self.status_2xx,
                "status_3xx": self.status_3xx,
                "status_4xx": self.status_4xx,
                "status_5xx": self.status_5xx,
                "last_request_at": self.last_request_at,
                "last_path": self.last_path,
                "last_status": self.last_status,
                "last_error": self.last_error,
                "note": self.note,
                "mirrored": self.mirrored,
                "serve_root": self.serve_root,
                "last_sync_at": self.last_sync_at,
            }


class MetricsHandler(SimpleHTTPRequestHandler):
    metrics: Metrics
    service_id: str

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def copyfile(self, source, outputfile) -> None:  # type: ignore[no-untyped-def]
        try:
            super().copyfile(source, outputfile)
        except BrokenPipeError:
            pass

    def send_response(self, code: int, message: str | None = None) -> None:
        super().send_response(code, message)
        self._resp_code = code

    def send_header(self, keyword: str, value: str) -> None:
        super().send_header(keyword, value)
        if keyword.lower() == "content-length":
            try:
                self._resp_len = int(value)
            except ValueError:
                pass

    def finish(self) -> None:
        try:
            super().finish()
        finally:
            code = getattr(self, "_resp_code", 0)
            nbytes = getattr(self, "_resp_len", 0)
            path = getattr(self, "path", "")
            if code:
                try:
                    self.metrics.record(path, code, nbytes)
                except Exception:
                    pass


class StaticWorker:
    def __init__(self, cfg: dict[str, Any]) -> None:
        self.cfg = dict(cfg)
        self.metrics = Metrics()
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._sync_thread: threading.Thread | None = None
        self._sync_stop = threading.Event()
        self._started_at: float | None = None
        self._status = "stopped"
        self._lock = threading.Lock()
        self._serve_root: Path | None = None
        self._mirrored = False

    @property
    def id(self) -> str:
        return self.cfg["id"]

    def update_cfg(self, cfg: dict[str, Any]) -> None:
        with self._lock:
            self.cfg = dict(cfg)

    def resolve_serve_root(self, do_sync: bool = True) -> Path:
        """Pick a readable serve path.

        LaunchAgents cannot read ~/Documents (TCC). Prefer direct root when
        readable; otherwise use APP_DIR/mirrors/<id> seeded by install.sh/sync.sh.
        """
        source = Path(self.cfg["root"]).expanduser()
        try:
            source = source.resolve()
        except OSError:
            pass
        dest = MIRRORS_DIR / self.id
        force_mirror = bool(self.cfg.get("force_mirror", False))

        if not force_mirror and path_readable(source):
            self._mirrored = False
            self._serve_root = source
            self.metrics.set_serve_info(str(source), False)
            return source

        if do_sync and path_readable(source):
            try:
                mirror_root(source, dest)
            except Exception as e:  # noqa: BLE001
                self.metrics.set_error(f"mirror failed: {e}")

        # Use existing mirror (seeded by install/sync from Terminal)
        if dest.is_dir() and path_readable(dest):
            self._mirrored = True
            self._serve_root = dest
            self.metrics.set_serve_info(str(dest), True, synced=do_sync)
            if not path_readable(source):
                with self.metrics._lock:
                    self.metrics.note = (
                        "macOS TCC: LaunchAgent cannot read source; serving mirror. "
                        "Refresh: tools/http-port-manager/sync.sh"
                    )
            return dest

        raise PermissionError(
            f"cannot serve root {source}. LaunchAgent lacks TCC access and mirror "
            f"missing at {dest}. From Terminal run: tools/http-port-manager/sync.sh "
            f"or grant Full Disk Access to {sys.executable}."
        )

    def sync(self) -> dict[str, Any]:
        source = Path(self.cfg["root"]).expanduser()
        try:
            source = source.resolve()
        except OSError:
            pass
        dest = MIRRORS_DIR / self.id
        if path_readable(source):
            force_mirror = bool(self.cfg.get("force_mirror", False))
            if not force_mirror:
                self._mirrored = False
                self._serve_root = source
                self.metrics.set_serve_info(str(source), False, synced=True)
                return {
                    "mirrored": False,
                    "serve_root": str(source),
                    "note": "direct root readable",
                }
            mirror_root(source, dest)
            self._mirrored = True
            self._serve_root = dest
            self.metrics.set_serve_info(str(dest), True, synced=True)
            return {"mirrored": True, "serve_root": str(dest), "source": str(source)}

        # Under LaunchAgent, Documents is unreadable — keep current mirror.
        if dest.is_dir() and path_readable(dest):
            self._mirrored = True
            self._serve_root = dest
            self.metrics.set_serve_info(str(dest), True)
            raise PermissionError(
                "source not readable from this process (macOS TCC). "
                "Run tools/http-port-manager/sync.sh in Terminal to refresh the mirror."
            )
        raise PermissionError(
            f"source not readable and no mirror at {dest}. Run sync.sh from Terminal."
        )

    def _start_sync_loop(self) -> None:
        # No auto-pull from Documents under launchd (TCC). Mirror is refreshed
        # by sync.sh / install.sh from a TCC-capable Terminal session.
        return

    def _stop_sync_loop(self) -> None:
        self._sync_stop.set()
        t = self._sync_thread
        self._sync_thread = None
        if t and t.is_alive():
            t.join(timeout=1.0)

    def start(self) -> None:
        with self._lock:
            if self._status == "running" and self._thread and self._thread.is_alive():
                return
            self._status = "starting"
            bind = self.cfg.get("bind") or "0.0.0.0"
            port = int(self.cfg["port"])
            try:
                serve_root = self.resolve_serve_root(do_sync=True)
            except Exception as e:  # noqa: BLE001
                self._status = "error"
                self.metrics.set_error(str(e))
                raise

            class BoundHandler(MetricsHandler):
                metrics = self.metrics
                service_id = self.id

                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__(*args, directory=str(serve_root), **kwargs)

            try:
                server = ThreadingHTTPServer((bind, port), BoundHandler)
            except OSError as e:
                self._status = "error"
                self.metrics.set_error(str(e))
                raise
            server.daemon_threads = True
            self._server = server
            self._started_at = time.time()

            def run() -> None:
                try:
                    server.serve_forever(poll_interval=0.3)
                except Exception as e:  # noqa: BLE001
                    self.metrics.set_error(str(e))
                    with self._lock:
                        self._status = "error"
                finally:
                    with self._lock:
                        if self._status != "error":
                            self._status = "stopped"

            t = threading.Thread(target=run, name=f"static-{self.id}", daemon=True)
            self._thread = t
            t.start()
            self._status = "running"
            if self._mirrored:
                self._start_sync_loop()

    def stop(self, join_timeout: float = 2.0) -> None:
        self._stop_sync_loop()
        with self._lock:
            server = self._server
            self._server = None
            self._started_at = None
            self._status = "stopped"
        if server is not None:
            try:
                server.shutdown()
            except Exception:
                pass
            try:
                server.server_close()
            except Exception:
                pass
        t = self._thread
        if t and t.is_alive():
            t.join(timeout=join_timeout)
        self._thread = None

    def restart(self) -> None:
        self.stop()
        self.start()

    def status_dict(self) -> dict[str, Any]:
        with self._lock:
            cfg = dict(self.cfg)
            status = self._status
            started = self._started_at
            thread_alive = bool(self._thread and self._thread.is_alive())
            server_open = self._server is not None
            serve_root = str(self._serve_root) if self._serve_root else None
            mirrored = self._mirrored
        bind = cfg.get("bind") or "0.0.0.0"
        port = int(cfg["port"])
        listening = port_listening(bind, port) if status == "running" else False
        if status == "running" and not (thread_alive and server_open):
            status = "error"
        uptime = int(time.time() - started) if started and status == "running" else 0
        m = self.metrics.snapshot()
        return {
            "id": cfg["id"],
            "name": cfg.get("name") or cfg["id"],
            "port": port,
            "bind": bind,
            "root": cfg["root"],
            "serve_root": serve_root or m.get("serve_root"),
            "mirrored": mirrored or bool(m.get("mirrored")),
            "auto_start": bool(cfg.get("auto_start", True)),
            "sync_interval_sec": float(cfg.get("sync_interval_sec") or 5),
            "status": status,
            "running": status == "running" and thread_alive,
            "listening": listening,
            "healthy": status == "running" and listening,
            "pid": os.getpid() if status == "running" else None,
            "uptime_sec": uptime,
            "started_at": (
                datetime.fromtimestamp(started, tz=timezone.utc).isoformat()
                if started
                else None
            ),
            "metrics": m,
            "url": f"http://{('127.0.0.1' if bind == '0.0.0.0' else bind)}:{port}/",
        }


class ServiceRegistry:
    def __init__(self, config_path: Path) -> None:
        self.config_path = config_path
        self._lock = threading.RLock()
        self.control = {"host": "0.0.0.0", "port": 9090}
        self.workers: dict[str, StaticWorker] = {}
        self.load()

    def load(self) -> None:
        path = self.config_path
        if not path.exists():
            self.save()
            return
        data = json.loads(path.read_text(encoding="utf-8"))
        with self._lock:
            self.control = data.get("control") or self.control
            services = data.get("services") or []
            want_ids = {s["id"] for s in services if "id" in s}
            for wid in list(self.workers):
                if wid not in want_ids:
                    self.workers[wid].stop()
                    del self.workers[wid]
            for s in services:
                sid = s["id"]
                if sid in self.workers:
                    self.workers[sid].update_cfg(s)
                else:
                    self.workers[sid] = StaticWorker(s)

    def save(self) -> None:
        with self._lock:
            payload = {
                "control": self.control,
                "services": [w.cfg for w in self.workers.values()],
            }
        tmp = self.config_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        tmp.replace(self.config_path)

    def list_status(self) -> list[dict[str, Any]]:
        with self._lock:
            workers = list(self.workers.values())
        return [w.status_dict() for w in sorted(workers, key=lambda x: (x.cfg.get("port", 0), x.id))]

    def get(self, sid: str) -> StaticWorker | None:
        with self._lock:
            return self.workers.get(sid)

    def auto_start_all(self) -> None:
        with self._lock:
            items = list(self.workers.values())
        for w in items:
            if w.cfg.get("auto_start", True):
                try:
                    w.start()
                except Exception as e:  # noqa: BLE001
                    w.metrics.set_error(f"auto_start failed: {e}")

    def create(self, body: dict[str, Any]) -> dict[str, Any]:
        sid = (body.get("id") or "").strip()
        if not sid:
            sid = "svc-" + uuid.uuid4().hex[:8]
        if not ID_RE.match(sid):
            raise ValueError("invalid id; use [A-Za-z0-9._-] up to 64 chars")
        port = int(body["port"])
        if not (1 <= port <= 65535):
            raise ValueError("port out of range")
        root = str(Path(body["root"]).expanduser())
        # resolve when possible; if TCC blocks resolve, keep expanded path
        try:
            root = str(Path(root).resolve())
        except OSError:
            pass
        bind = body.get("bind") or "0.0.0.0"
        name = body.get("name") or sid
        auto_start = bool(body.get("auto_start", True))
        cfg = {
            "id": sid,
            "name": name,
            "port": port,
            "bind": bind,
            "root": root,
            "auto_start": auto_start,
            "sync_interval_sec": float(body.get("sync_interval_sec") or 5),
        }
        with self._lock:
            if sid in self.workers:
                raise ValueError(f"service already exists: {sid}")
            for w in self.workers.values():
                if int(w.cfg["port"]) == port and (w.cfg.get("bind") or "0.0.0.0") == bind:
                    raise ValueError(f"port {port} already used by {w.id}")
            worker = StaticWorker(cfg)
            self.workers[sid] = worker
        self.save()
        if auto_start:
            worker.start()
        return worker.status_dict()

    def update(self, sid: str, body: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            worker = self.workers.get(sid)
            if not worker:
                raise KeyError(sid)
            cfg = dict(worker.cfg)
            for key in ("name", "bind", "root", "auto_start", "port", "sync_interval_sec"):
                if key in body:
                    cfg[key] = body[key]
            cfg["port"] = int(cfg["port"])
            try:
                cfg["root"] = str(Path(cfg["root"]).expanduser().resolve())
            except OSError:
                cfg["root"] = str(Path(cfg["root"]).expanduser())
            for w in self.workers.values():
                if w.id == sid:
                    continue
                if int(w.cfg["port"]) == cfg["port"] and (w.cfg.get("bind") or "0.0.0.0") == (
                    cfg.get("bind") or "0.0.0.0"
                ):
                    raise ValueError(f"port {cfg['port']} already used by {w.id}")
            was_running = worker.status_dict()["running"]
            worker.update_cfg(cfg)
        self.save()
        if was_running:
            worker.restart()
        return worker.status_dict()

    def delete(self, sid: str) -> None:
        with self._lock:
            worker = self.workers.pop(sid, None)
        if not worker:
            raise KeyError(sid)
        worker.stop()
        self.save()

    def signature(self) -> str:
        rows = self.list_status()
        slim = []
        for r in rows:
            slim.append(
                {
                    "id": r["id"],
                    "status": r["status"],
                    "healthy": r["healthy"],
                    "listening": r["listening"],
                    "running": r["running"],
                    "port": r["port"],
                    "bind": r["bind"],
                    "root": r["root"],
                    "serve_root": r.get("serve_root"),
                    "mirrored": r.get("mirrored"),
                    "uptime_sec": r["uptime_sec"],
                    "metrics": r["metrics"],
                }
            )
        return json.dumps(slim, sort_keys=True, separators=(",", ":"))


class LabTelemetryStore:
    """Persist Algorithms Lab learning events for AI coach.

    Layout:
      lab_telemetry/events.jsonl
      lab_telemetry/sessions/<sessionId>.json
      lab_telemetry/latest_by_problem/<problemId>.json
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.sessions_dir = root / "sessions"
        self.by_problem_dir = root / "latest_by_problem"
        self.events_path = root / "events.jsonl"
        self._lock = threading.Lock()
        self.root.mkdir(parents=True, exist_ok=True)
        self.sessions_dir.mkdir(parents=True, exist_ok=True)
        self.by_problem_dir.mkdir(parents=True, exist_ok=True)

    def ingest(self, body: dict[str, Any]) -> dict[str, Any]:
        session_id = str(body.get("sessionId") or body.get("session_id") or "").strip()
        if not session_id:
            session_id = "s_" + uuid.uuid4().hex[:12]
        problem_id = body.get("problemId", body.get("problem_id"))
        slug = body.get("slug") or ""
        summary = body.get("summary") if isinstance(body.get("summary"), dict) else None
        events = body.get("events") if isinstance(body.get("events"), list) else []
        # cap batch size
        if len(events) > 200:
            events = events[-200:]

        rec = {
            "sessionId": session_id,
            "problemId": problem_id,
            "slug": slug,
            "titleZh": body.get("titleZh") or body.get("title_zh") or "",
            "receivedAt": utc_now_iso(),
            "clientStartedAt": body.get("startedAt"),
            "eventCount": len(events),
            "events": events,
            "summary": summary,
            "kind": body.get("kind") or "batch",
        }

        with self._lock:
            with self.events_path.open("a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")

            sess_path = self.sessions_dir / f"{session_id}.json"
            prev: dict[str, Any] = {}
            if sess_path.exists():
                try:
                    prev = json.loads(sess_path.read_text(encoding="utf-8"))
                except Exception:
                    prev = {}
            merged_events = (prev.get("events") or []) + events
            if len(merged_events) > 2000:
                merged_events = merged_events[-2000:]
            session_doc = {
                "sessionId": session_id,
                "problemId": problem_id if problem_id is not None else prev.get("problemId"),
                "slug": slug or prev.get("slug") or "",
                "titleZh": rec["titleZh"] or prev.get("titleZh") or "",
                "updatedAt": utc_now_iso(),
                "startedAt": prev.get("startedAt") or body.get("startedAt") or utc_now_iso(),
                "events": merged_events,
                "summary": summary or prev.get("summary"),
                "batches": int(prev.get("batches") or 0) + 1,
            }
            sess_path.write_text(
                json.dumps(session_doc, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )

            if problem_id is not None and str(problem_id) != "":
                safe = re.sub(r"[^0-9A-Za-z._-]", "_", str(problem_id))
                (self.by_problem_dir / f"{safe}.json").write_text(
                    json.dumps(session_doc, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8",
                )

        return {
            "ok": True,
            "sessionId": session_id,
            "storedEvents": len(events),
            "totalEvents": len(session_doc["events"]),
        }

    def list_sessions(self, limit: int = 20) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        with self._lock:
            paths = sorted(
                self.sessions_dir.glob("*.json"),
                key=lambda p: p.stat().st_mtime,
                reverse=True,
            )
            for p in paths[: max(1, min(limit, 100))]:
                try:
                    d = json.loads(p.read_text(encoding="utf-8"))
                except Exception:
                    continue
                rows.append(
                    {
                        "sessionId": d.get("sessionId"),
                        "problemId": d.get("problemId"),
                        "slug": d.get("slug"),
                        "titleZh": d.get("titleZh"),
                        "updatedAt": d.get("updatedAt"),
                        "eventCount": len(d.get("events") or []),
                        "passed": bool((d.get("summary") or {}).get("quiz", {}).get("passed")),
                        "elapsedHuman": (d.get("summary") or {}).get("elapsedHuman"),
                    }
                )
        return rows

    def get_session(self, session_id: str) -> dict[str, Any] | None:
        path = self.sessions_dir / f"{session_id}.json"
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return None

    def latest_for_problem(self, problem_id: str | int) -> dict[str, Any] | None:
        safe = re.sub(r"[^0-9A-Za-z._-]", "_", str(problem_id))
        path = self.by_problem_dir / f"{safe}.json"
        if path.is_file():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except Exception:
                return None
        # fallback scan
        with self._lock:
            best = None
            best_m = 0.0
            for p in self.sessions_dir.glob("*.json"):
                try:
                    d = json.loads(p.read_text(encoding="utf-8"))
                except Exception:
                    continue
                if str(d.get("problemId")) != str(problem_id):
                    continue
                m = p.stat().st_mtime
                if m >= best_m:
                    best_m = m
                    best = d
            return best

    def coach_brief(
        self,
        problem_id: str | int | None = None,
        session_id: str | None = None,
    ) -> dict[str, Any]:
        doc = None
        if session_id:
            doc = self.get_session(session_id)
        elif problem_id is not None:
            doc = self.latest_for_problem(problem_id)
        else:
            sessions = self.list_sessions(limit=1)
            if sessions:
                doc = self.get_session(sessions[0]["sessionId"])

        if not doc:
            return {
                "ok": False,
                "error": "no session",
                "hint": "User has not opened learn.html with telemetry yet.",
            }

        summary = doc.get("summary") or {}
        interest = summary.get("interest") or []
        confusion = summary.get("confusion") or []
        quiz = summary.get("quiz") or {}

        # Derive coach talking points if summary thin
        if not interest and doc.get("events"):
            dwell: dict[str, int] = {}
            for ev in doc["events"]:
                if ev.get("type") == "section_leave":
                    sec = (ev.get("payload") or {}).get("section") or "unknown"
                    dwell[sec] = dwell.get(sec, 0) + int(
                        (ev.get("payload") or {}).get("dwellMs") or 0
                    )
            interest = [
                {"section": k, "dwellMs": v}
                for k, v in sorted(dwell.items(), key=lambda x: -x[1])[:5]
            ]

        talking_points = []
        human = summary.get("humanInsights") or []
        if human:
            talking_points.append("人话路径：" + "；".join(str(h) for h in human[:4]))
        if confusion:
            talking_points.append(
                "优先澄清卡点："
                + ", ".join(
                    str(
                        c.get("label")
                        or c.get("section")
                        or c.get("signal")
                        or c
                    )
                    for c in confusion[:4]
                )
            )
        if interest:
            top = interest[0]
            talking_points.append(
                f"用户停留最多：{top.get('label') or top.get('section')}"
                f"（约 {int((top.get('dwellMs') or 0)/1000)}s）"
            )
        if quiz.get("passed"):
            talking_points.append(
                f"理解测已通过 {quiz.get('lastScore') or quiz.get('firstScore')}，可进入 Rust"
            )
        elif quiz.get("submits"):
            talking_points.append(
                f"理解测未过：{quiz.get('lastScore')}，已提交 {quiz.get('submits')} 次"
            )
        else:
            talking_points.append("尚未提交理解测：先引导读图解/场景，勿贴完整 AC 代码")

        talking_points.append(
            "术语保持英文：HashMap / complement / carry / two-pointers / dummy head"
        )

        return {
            "ok": True,
            "schema": "lab.coach_brief.v1",
            "sessionId": doc.get("sessionId"),
            "problemId": doc.get("problemId"),
            "slug": doc.get("slug"),
            "titleZh": doc.get("titleZh"),
            "updatedAt": doc.get("updatedAt"),
            "elapsedHuman": summary.get("elapsedHuman"),
            "interest": interest,
            "confusion": confusion,
            "quiz": quiz,
            "talkingPoints": talking_points,
            "summary": summary,
            "eventCount": len(doc.get("events") or []),
            "coachPrompt": (
                "你是 Algorithms Lab coach。根据下列学习行为摘要调整讲解："
                "先处理 confusion，再强化 interest 区概念；"
                "闸门未过禁止贴完整 AC 代码；专业术语保持英文。\n\n"
                + json.dumps(
                    {
                        "problemId": doc.get("problemId"),
                        "slug": doc.get("slug"),
                        "interest": interest,
                        "confusion": confusion,
                        "quiz": quiz,
                        "talkingPoints": talking_points,
                    },
                    ensure_ascii=False,
                    indent=2,
                )
            ),
        }


class ControlHandler(SimpleHTTPRequestHandler):
    registry: ServiceRegistry
    static_dir: Path
    lab_store: LabTelemetryStore

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,PATCH,DELETE,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def _send_json(self, code: int, obj: Any) -> None:
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-cache")
        self._cors()
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(raw)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        q = parse_qs(parsed.query)

        if path == "/api/healthz":
            self._send_json(200, {"ok": True, "pid": os.getpid(), "time": utc_now_iso()})
            return

        if path == "/api/services":
            self._send_json(200, {"services": self.registry.list_status(), "time": utc_now_iso()})
            return

        if path.startswith("/api/services/") and path.count("/") == 3:
            sid = path.rsplit("/", 1)[-1]
            w = self.registry.get(sid)
            if not w:
                self._send_json(404, {"error": "not found"})
                return
            self._send_json(200, w.status_dict())
            return

        if path == "/api/lab/sessions":
            limit = int((q.get("limit") or ["20"])[0] or 20)
            self._send_json(200, {"sessions": self.lab_store.list_sessions(limit=limit)})
            return

        if path == "/api/lab/session":
            sid = (q.get("sessionId") or q.get("id") or [""])[0]
            doc = self.lab_store.get_session(sid) if sid else None
            if not doc:
                self._send_json(404, {"error": "not found"})
                return
            self._send_json(200, doc)
            return

        if path == "/api/lab/coach":
            pid = (q.get("problemId") or q.get("id") or [None])[0]
            sid = (q.get("sessionId") or [None])[0]
            self._send_json(
                200,
                self.lab_store.coach_brief(problem_id=pid, session_id=sid),
            )
            return

        if path == "/api/events":
            self._sse()
            return

        if path in ("/", "/index.html"):
            self._serve_file(self.static_dir / "index.html", "text/html; charset=utf-8")
            return
        if path.startswith("/static/"):
            rel = path[len("/static/") :]
            if ".." in rel or rel.startswith("/"):
                self.send_error(HTTPStatus.FORBIDDEN)
                return
            fpath = (self.static_dir / rel).resolve()
            if not str(fpath).startswith(str(self.static_dir.resolve())):
                self.send_error(HTTPStatus.FORBIDDEN)
                return
            if not fpath.is_file():
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            ctype = "text/css" if fpath.suffix == ".css" else (
                "application/javascript" if fpath.suffix == ".js" else "application/octet-stream"
            )
            self._serve_file(fpath, ctype)
            return

        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        try:
            body = self._read_json()
        except json.JSONDecodeError:
            self._send_json(400, {"error": "invalid json"})
            return

        try:
            if path == "/api/lab/events":
                result = self.lab_store.ingest(body)
                self._send_json(202, result)
                return

            if path == "/api/services":
                st = self.registry.create(body)
                self._send_json(201, st)
                return

            m = re.fullmatch(r"/api/services/([^/]+)/(start|stop|restart|sync)", path)
            if m:
                sid, action = m.group(1), m.group(2)
                w = self.registry.get(sid)
                if not w:
                    self._send_json(404, {"error": "not found"})
                    return
                if action == "start":
                    w.start()
                    self._send_json(200, w.status_dict())
                elif action == "stop":
                    w.stop()
                    self._send_json(200, w.status_dict())
                elif action == "restart":
                    w.restart()
                    self._send_json(200, w.status_dict())
                else:
                    result = w.sync()
                    self._send_json(200, {"ok": True, **result, "service": w.status_dict()})
                return
        except FileNotFoundError as e:
            self._send_json(400, {"error": str(e)})
            return
        except PermissionError as e:
            self._send_json(403, {"error": str(e)})
            return
        except ValueError as e:
            self._send_json(400, {"error": str(e)})
            return
        except OSError as e:
            self._send_json(409, {"error": str(e)})
            return
        except Exception as e:  # noqa: BLE001
            self._send_json(500, {"error": str(e), "trace": traceback.format_exc()})
            return

        self._send_json(404, {"error": "not found"})

    def do_PUT(self) -> None:  # noqa: N802
        self.do_PATCH()

    def do_PATCH(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        m = re.fullmatch(r"/api/services/([^/]+)", path)
        if not m:
            self._send_json(404, {"error": "not found"})
            return
        sid = m.group(1)
        try:
            body = self._read_json()
            st = self.registry.update(sid, body)
            self._send_json(200, st)
        except KeyError:
            self._send_json(404, {"error": "not found"})
        except ValueError as e:
            self._send_json(400, {"error": str(e)})
        except OSError as e:
            self._send_json(409, {"error": str(e)})
        except Exception as e:  # noqa: BLE001
            self._send_json(500, {"error": str(e)})

    def do_DELETE(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        m = re.fullmatch(r"/api/services/([^/]+)", path)
        if not m:
            self._send_json(404, {"error": "not found"})
            return
        sid = m.group(1)
        try:
            self.registry.delete(sid)
            self._send_json(200, {"ok": True, "id": sid})
        except KeyError:
            self._send_json(404, {"error": "not found"})
        except Exception as e:  # noqa: BLE001
            self._send_json(500, {"error": str(e)})

    def _serve_file(self, path: Path, content_type: str) -> None:
        if not path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def _sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        last_sig = ""
        try:
            while True:
                services = self.registry.list_status()
                sig = self.registry.signature()
                if sig != last_sig:
                    last_sig = sig
                    payload = {
                        "time": utc_now_iso(),
                        "pid": os.getpid(),
                        "services": services,
                    }
                    data = json.dumps(payload, ensure_ascii=False)
                    self.wfile.write(f"event: update\ndata: {data}\n\n".encode("utf-8"))
                    self.wfile.flush()
                else:
                    self.wfile.write(b": heartbeat\n\n")
                    self.wfile.flush()
                time.sleep(1.0)
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception:
            return


def make_control_handler(
    registry: ServiceRegistry, lab_store: LabTelemetryStore
) -> type[ControlHandler]:
    class Bound(ControlHandler):
        pass

    Bound.registry = registry
    Bound.static_dir = STATIC_DIR
    Bound.lab_store = lab_store
    return Bound


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="HTTP Port Manager")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="path to config.json")
    parser.add_argument("--host", default=None, help="override control host")
    parser.add_argument("--port", type=int, default=None, help="override control port")
    args = parser.parse_args(argv)

    config_path = Path(args.config).expanduser().resolve()
    MIRRORS_DIR.mkdir(parents=True, exist_ok=True)
    LAB_TELEMETRY_DIR.mkdir(parents=True, exist_ok=True)
    registry = ServiceRegistry(config_path)
    lab_store = LabTelemetryStore(LAB_TELEMETRY_DIR)
    if args.host:
        registry.control["host"] = args.host
    if args.port is not None:
        registry.control["port"] = int(args.port)
    registry.save()

    registry.auto_start_all()

    host = registry.control.get("host") or "0.0.0.0"
    port = int(registry.control.get("port") or 9090)
    handler = make_control_handler(registry, lab_store)
    server = ThreadingHTTPServer((host, port), handler)
    server.daemon_threads = True

    def shutdown_workers() -> None:
        for w in list(registry.workers.values()):
            try:
                w.stop()
            except Exception:
                pass

    print(
        f"http-port-manager listening on http://{host}:{port}/  config={config_path}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        print("\nshutting down...", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        shutdown_workers()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
