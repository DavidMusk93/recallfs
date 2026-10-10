#!/usr/bin/env python3
"""Upsert the orca-netd service into Tunnel Manager's tunnels.json.

Does not print or store pairing tokens. Destination stays in config.env.
"""

from __future__ import annotations

import json
import os
from pathlib import Path


def entry(home: Path) -> dict:
    runtime = Path(os.environ.get("ORCA_NETD_HOME") or home / ".local/share/orca-netd")
    node = os.environ.get("ORCA_NETD_NODE") or "/opt/homebrew/bin/node"
    return {
        "name": "orca-netd",
        "port": 16768,
        "host": "127.0.0.1",
        "purpose": "Orca CGNAT 转发：本机 :16768 经 netd SOCKS 到远端 runtime",
        "kind": "service",
        "group": "service",
        "accent": "sky",
        "cwd": str(runtime),
        "cmd": [node, str(runtime / "forward.js")],
        "env": {
            "ORCA_NETD_STATE_DIR": str(runtime),
            "ORCA_NETD_CONFIG": str(runtime / "config.env"),
        },
        "match": "orca-netd/forward.js",
        "auto_start": True,
        "stop_on_exit": False,
        "start_timeout": 15,
    }


def main() -> None:
    home = Path.home()
    path = Path(
        os.environ.get("SSH_SOCKS_TUNNELS")
        or home / "Library/Application Support/ssh-socks/tunnels.json"
    )
    data = json.loads(path.read_text())
    if isinstance(data, list):
        items = data
        wrapper = None
    elif isinstance(data, dict) and isinstance(data.get("tunnels"), list):
        items = data["tunnels"]
        wrapper = data
    else:
        raise SystemExit(f"unrecognized tunnels file: {path}")
    fresh = entry(home)
    for index, item in enumerate(items):
        if isinstance(item, dict) and item.get("name") == "orca-netd":
            items[index] = fresh
            break
    else:
        items.append(fresh)
    text = json.dumps(wrapper if wrapper is not None else items, indent=2, ensure_ascii=False) + "\n"
    path.write_text(text)
    print(f"upserted orca-netd -> {path}")


if __name__ == "__main__":
    main()
