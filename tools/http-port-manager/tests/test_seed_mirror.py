#!/usr/bin/env python3
"""Unit tests for dashboard mirror seeding / TCC grant helpers."""

from __future__ import annotations

import json
import socket
import sys
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import server  # noqa: E402


class SafePathTests(unittest.TestCase):
    def test_rejects_parent(self) -> None:
        with self.assertRaises(ValueError):
            server.safe_mirror_relpath("../etc/passwd")
        with self.assertRaises(ValueError):
            server.safe_mirror_relpath("a/../../b")

    def test_accepts_nested(self) -> None:
        self.assertEqual(server.safe_mirror_relpath("a/b/c.md"), Path("a/b/c.md"))

    def test_tcc_documents(self) -> None:
        docs = Path.home() / "Documents" / "foo"
        self.assertTrue(server.tcc_sensitive_path(docs))
        self.assertFalse(server.tcc_sensitive_path(Path("/tmp/foo")))


class SeedMirrorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.mirrors = self.tmp / "mirrors"
        self.mirrors.mkdir()
        self._old_mirrors = server.MIRRORS_DIR
        server.MIRRORS_DIR = self.mirrors

    def tearDown(self) -> None:
        server.MIRRORS_DIR = self._old_mirrors
        self._tmp.cleanup()

    def test_seed_writes_and_rejects_escape(self) -> None:
        w = server.StaticWorker(
            {
                "id": "demo",
                "name": "demo",
                "port": 18001,
                "bind": "127.0.0.1",
                "root": str(self.tmp / "missing-src"),
                "auto_start": False,
            }
        )
        out = w.seed_from_upload(
            {
                "reset": True,
                "files": [
                    {"path": "hello.txt", "text": "hi"},
                    {"path": "sub/n.txt", "text": "nested"},
                ],
            }
        )
        self.assertEqual(out["written"], 2)
        dest = self.mirrors / "demo"
        self.assertEqual((dest / "hello.txt").read_text(), "hi")
        self.assertEqual((dest / "sub/n.txt").read_text(), "nested")
        with self.assertRaises(ValueError):
            w.seed_from_upload(
                {"reset": False, "files": [{"path": "../x.txt", "text": "no"}]}
            )

    def test_http_seed_then_serve(self) -> None:
        cfg = self.tmp / "config.json"
        cfg.write_text(json.dumps({"control": {"host": "127.0.0.1", "port": 0}, "services": []}))
        registry = server.ServiceRegistry(cfg)
        lab = server.LabTelemetryStore(self.tmp / "lab")
        handler = server.make_control_handler(registry, lab)
        httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        httpd.daemon_threads = True
        t = threading.Thread(target=httpd.serve_forever, daemon=True)
        t.start()
        _host, port = httpd.server_address[:2]
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        worker_port = sock.getsockname()[1]
        sock.close()
        try:
            conn = HTTPConnection("127.0.0.1", port, timeout=5)
            body = json.dumps(
                {
                    "id": "grantme",
                    "name": "grantme",
                    "port": worker_port,
                    "bind": "127.0.0.1",
                    "root": str(Path.home() / "Documents" / "no-such-grant-dir"),
                    "auto_start": True,
                }
            ).encode()
            conn.request("POST", "/api/services", body, {"Content-Type": "application/json"})
            resp = conn.getresponse()
            payload = json.loads(resp.read().decode())
            self.assertEqual(resp.status, 201)
            self.assertTrue(payload.get("needs_grant"))

            seed = json.dumps(
                {
                    "reset": True,
                    "done": True,
                    "files": [{"path": "index.html", "text": "<h1>granted</h1>"}],
                }
            ).encode()
            conn.request(
                "POST",
                "/api/services/grantme/seed-mirror",
                seed,
                {"Content-Type": "application/json"},
            )
            seeded = conn.getresponse()
            seeded_body = json.loads(seeded.read().decode())
            self.assertEqual(seeded.status, 200, seeded_body)
            self.assertTrue(seeded_body.get("ok"))
            svc = seeded_body["service"]
            self.assertTrue(svc.get("healthy") or svc.get("running"))

            worker = HTTPConnection("127.0.0.1", worker_port, timeout=5)
            worker.request("GET", "/")
            page = worker.getresponse()
            html = page.read().decode()
            self.assertEqual(page.status, 200)
            self.assertIn("granted", html)
            worker.close()

            conn.request("DELETE", "/api/services/grantme")
            self.assertEqual(conn.getresponse().status, 200)
            conn.close()
        finally:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    unittest.main()
