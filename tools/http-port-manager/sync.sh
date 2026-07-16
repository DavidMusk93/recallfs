#!/usr/bin/env bash
# Refresh mirrors from service roots (must run in Terminal / TCC-capable context).
# LaunchAgents cannot read ~/Documents; this script can.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="${HTTP_PORT_MANAGER_HOME:-$HOME/Library/Application Support/http-port-manager}"
CONFIG_JSON="${HTTP_PORT_MANAGER_CONFIG:-$INSTALL_DIR/config.json}"
MIRROR_DIR="$INSTALL_DIR/mirrors"
PYTHON_BIN="${HTTP_PORT_MANAGER_PYTHON:-/usr/bin/python3}"

if [[ ! -f "$CONFIG_JSON" ]]; then
  # fall back to repo config before first install
  CONFIG_JSON="$SRC_DIR/config.json"
fi

mkdir -p "$MIRROR_DIR"

"$PYTHON_BIN" - <<'PY' "$CONFIG_JSON" "$MIRROR_DIR"
import json, subprocess, sys, shutil
from pathlib import Path

cfg_path = Path(sys.argv[1])
mirror_dir = Path(sys.argv[2])
cfg = json.loads(cfg_path.read_text())
excludes = [".git", "target", ".tmp", "__pycache__", ".DS_Store", "*.pyc"]

for s in cfg.get("services") or []:
    sid = s["id"]
    root = Path(s["root"]).expanduser()
    dest = mirror_dir / sid
    dest.mkdir(parents=True, exist_ok=True)
    if not root.is_dir():
        print(f"[skip] {sid}: root missing {root}")
        continue
    if shutil.which("rsync"):
        cmd = ["rsync", "-a", "--delete"]
        for ex in excludes:
            cmd += ["--exclude", ex]
        cmd += [str(root) + "/", str(dest) + "/"]
        print(f"[rsync] {sid}: {root} -> {dest}")
        subprocess.run(cmd, check=True)
    else:
        print(f"[copy] {sid}: {root} -> {dest}")
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(root, dest)
    n = sum(1 for _ in dest.rglob("*"))
    print(f"  ok files≈{n}")
print("done")
PY

# optional: nudge manager if up
CONTROL_PORT="$("$PYTHON_BIN" -c "import json;from pathlib import Path;print(json.loads(Path(r'$CONFIG_JSON').read_text()).get('control',{}).get('port',9090))")"
if curl -fsS "http://127.0.0.1:${CONTROL_PORT}/api/healthz" >/dev/null 2>&1; then
  # restart services so handlers re-resolve (usually unnecessary for static files)
  ids="$("$PYTHON_BIN" -c "import json;from pathlib import Path;print(' '.join(s['id'] for s in json.loads(Path(r'$CONFIG_JSON').read_text()).get('services',[])))")"
  for id in $ids; do
    curl -fsS -X POST "http://127.0.0.1:${CONTROL_PORT}/api/services/${id}/restart" >/dev/null 2>&1 || true
  done
  echo "manager notified (restart)"
fi
