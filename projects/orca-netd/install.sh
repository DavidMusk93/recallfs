#!/usr/bin/env bash
# Install the Orca netd glue and hand the listener to Tunnel Manager.
# Runtime files live outside ~/Documents: a LaunchAgent-supervised manager
# cannot read that tree under macOS TCC. Pairing tokens are never copied.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="${ORCA_NETD_HOME:-$HOME/.local/share/orca-netd}"
NODE="${ORCA_NETD_NODE:-/opt/homebrew/bin/node}"
if [[ ! -x "$NODE" ]]; then
  NODE="$(command -v node)"
fi

mkdir -p "$RUNTIME"
cp "$SRC/forward.js" "$SRC/socks-cgnat-preload.js" "$RUNTIME/"
chmod 644 "$RUNTIME/forward.js" "$RUNTIME/socks-cgnat-preload.js"

ORCA_NETD_HOME="$RUNTIME" ORCA_NETD_NODE="$NODE" python3 "$SRC/scripts/write-config.py"
ORCA_NETD_HOME="$RUNTIME" ORCA_NETD_NODE="$NODE" python3 "$SRC/scripts/register-tunnel.py"

# Tunnel Manager is the supervisor. Drop the old LaunchAgent so it cannot
# race the manager back onto :16768 after a stop.
UID_NUM="$(id -u)"
launchctl bootout "gui/${UID_NUM}/orca-netd-forward" >/dev/null 2>&1 || true

if curl -fsS -m 5 -X POST "http://127.0.0.1:9020/api/reload" >/tmp/orca-netd-reload.json; then
  curl -fsS -m 20 -X POST "http://127.0.0.1:9020/api/tunnel/orca-netd/start" >/tmp/orca-netd-start.json
  python3 - << 'PY'
import json
reload = json.load(open("/tmp/orca-netd-reload.json"))
start = json.load(open("/tmp/orca-netd-start.json"))
print("reload started", reload.get("started"), "unchanged", len(reload.get("unchanged") or []))
print("start", start.get("ok"), start.get("message") or start.get("error"))
PY
else
  echo "tunnel manager :9020 did not accept reload; listener was not started" >&2
  exit 1
fi

if ! grep -a -q 'orca-netd/socks-cgnat-preload.js' "/Applications/Orca.app/Contents/Resources/app.asar"; then
  echo "Orca app.asar is missing the preload require. Re-run scripts/patch-asar.js after quitting Orca." >&2
  exit 1
fi

echo "installed -> $RUNTIME"
echo "status: curl -sS http://127.0.0.1:9020/api/tunnel/orca-netd"
