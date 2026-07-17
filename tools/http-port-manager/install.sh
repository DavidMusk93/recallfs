#!/usr/bin/env bash
# Install / reinstall http-port-manager as a macOS LaunchAgent.
#
# Copies sources out of ~/Documents (TCC blocks launchd from reading Documents)
# into ~/Library/Application Support/http-port-manager.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="${HTTP_PORT_MANAGER_HOME:-$HOME/Library/Application Support/http-port-manager}"
LABEL="com.user.http-port-manager"
PLIST_SRC="$SRC_DIR/launchd/${LABEL}.plist"
PLIST_DST="$HOME/Library/LaunchAgents/${LABEL}.plist"
LOG_DIR="${HTTP_PORT_MANAGER_LOG_DIR:-$HOME/Library/Logs/http-port-manager}"
PYTHON_BIN="${HTTP_PORT_MANAGER_PYTHON:-/usr/bin/python3}"

mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR" "$INSTALL_DIR/static" "$INSTALL_DIR/launchd"

# Sync code. Preserve runtime state (config + lab_telemetry sessions).
# NEVER rsync-delete lab_telemetry — coach history lives there.
rsync -a --delete \
  --exclude 'config.json' \
  --exclude 'lab_telemetry/' \
  --exclude 'mirrors/' \
  --exclude '__pycache__' \
  --exclude '*.pyc' \
  --exclude '.DS_Store' \
  "$SRC_DIR/" "$INSTALL_DIR/"

if [[ ! -f "$INSTALL_DIR/config.json" ]]; then
  cp "$SRC_DIR/config.json" "$INSTALL_DIR/config.json"
else
  # merge new default services keys without wiping user-added services
  "$PYTHON_BIN" - <<'PY' "$SRC_DIR/config.json" "$INSTALL_DIR/config.json"
import json, sys
from pathlib import Path
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
a, b = json.loads(src.read_text()), json.loads(dst.read_text())
b.setdefault("control", a.get("control", {}))
by_id = {s["id"]: s for s in b.get("services") or []}
for s in a.get("services") or []:
    cur = by_id.get(s["id"], {})
    cur = {**s, **{k: v for k, v in cur.items() if k not in ("force_mirror",) or True}}
    # prefer keeping user root/port/bind/name; refresh force_mirror default from template
    for k in ("port", "bind", "root", "name", "auto_start"):
        if k in by_id.get(s["id"], {}):
            cur[k] = by_id[s["id"]][k]
    cur["force_mirror"] = s.get("force_mirror", cur.get("force_mirror", False))
    by_id[s["id"]] = cur
b["services"] = list(by_id.values())
dst.write_text(json.dumps(b, indent=2, ensure_ascii=False) + "\n")
PY
fi

SERVER_PY="$INSTALL_DIR/server.py"
CONFIG_JSON="$INSTALL_DIR/config.json"

chmod +x "$SERVER_PY" "$INSTALL_DIR/install.sh" "$INSTALL_DIR/sync.sh" 2>/dev/null || true
chmod +x "$SRC_DIR/sync.sh" 2>/dev/null || true

# Seed mirrors from Terminal context (has Documents TCC access).
echo "seeding mirrors..."
HTTP_PORT_MANAGER_HOME="$INSTALL_DIR" HTTP_PORT_MANAGER_CONFIG="$CONFIG_JSON" \
  bash "$SRC_DIR/sync.sh" || bash "$INSTALL_DIR/sync.sh"

# Best-effort: free control + service ports listed in config
if command -v lsof >/dev/null 2>&1; then
  while read -r port; do
    [[ -z "$port" || "$port" == "None" ]] && continue
    pids=$(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null || true)
    if [[ -n "${pids:-}" ]]; then
      echo "stopping listeners on :$port -> $pids"
      # shellcheck disable=SC2086
      kill $pids 2>/dev/null || true
    fi
  done < <("$PYTHON_BIN" - <<'PY' "$CONFIG_JSON"
import json, sys
from pathlib import Path
cfg = json.loads(Path(sys.argv[1]).read_text())
print(cfg.get("control", {}).get("port", 9090))
for s in cfg.get("services") or []:
    print(s.get("port", ""))
PY
)
  sleep 0.4
fi

tmp="$(mktemp)"
sed \
  -e "s|__SERVER_PY__|${SERVER_PY}|g" \
  -e "s|__CONFIG_JSON__|${CONFIG_JSON}|g" \
  -e "s|__APP_DIR__|${INSTALL_DIR}|g" \
  -e "s|__LOG_DIR__|${LOG_DIR}|g" \
  -e "s|/usr/bin/python3|${PYTHON_BIN}|g" \
  "$PLIST_SRC" >"$tmp"

# Unload existing
if launchctl print "gui/$(id -u)/$LABEL" >/dev/null 2>&1; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
fi
launchctl unload "$PLIST_DST" 2>/dev/null || true

cp "$tmp" "$PLIST_DST"
rm -f "$tmp"
chmod 644 "$PLIST_DST"

launchctl bootstrap "gui/$(id -u)" "$PLIST_DST" 2>/dev/null || launchctl load "$PLIST_DST"
launchctl enable "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl kickstart -k "gui/$(id -u)/$LABEL" 2>/dev/null || launchctl start "$LABEL" || true

CONTROL_PORT="$("$PYTHON_BIN" -c "import json;from pathlib import Path;print(json.loads(Path(r'$CONFIG_JSON').read_text())['control']['port'])")"

echo "installed LaunchAgent: $PLIST_DST"
echo "runtime dir: $INSTALL_DIR"
echo "logs: $LOG_DIR"
echo "dashboard: http://127.0.0.1:${CONTROL_PORT}/"

for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  if curl -fsS "http://127.0.0.1:${CONTROL_PORT}/api/healthz" >/dev/null 2>&1; then
    echo "healthz: ok"
    curl -fsS "http://127.0.0.1:${CONTROL_PORT}/api/services" | "$PYTHON_BIN" -m json.tool | head -60
    exit 0
  fi
  sleep 0.5
done

echo "warning: healthz not ready; last stderr:" >&2
tail -n 30 "$LOG_DIR/http-port-manager.err.log" 2>/dev/null || true
exit 1
