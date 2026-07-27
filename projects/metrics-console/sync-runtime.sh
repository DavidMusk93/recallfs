#!/usr/bin/env bash
# Sync metrics-console into Application Support so launchd Tunnel Manager
# can start it (macOS TCC blocks LaunchAgents from reading ~/Documents).
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
DST="${METRICS_CONSOLE_RUNTIME:-$HOME/Library/Application Support/metrics-console}"
mkdir -p "$DST"
rsync -a --delete \
  --exclude '.venv' \
  --exclude '__pycache__' \
  --exclude '*.pyc' \
  --exclude '.DS_Store' \
  "$SRC/" "$DST/app/"

if [[ ! -x "$DST/.venv/bin/python" ]]; then
  export UV_PYTHON_INSTALL_DIR="${UV_PYTHON_INSTALL_DIR:-$HOME/.local/share/uv/python}"
  uv venv "$DST/.venv" --python 3.13
  # shellcheck disable=SC1091
  source "$DST/.venv/bin/activate"
  uv pip install duckdb fastapi 'uvicorn[standard]' pydantic-settings pytz
fi

cat > "$DST/run.sh" << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$ROOT/app/src${PYTHONPATH:+:$PYTHONPATH}"
export METRICS_QUACK_URI="${METRICS_QUACK_URI:-quack:10.37.125.152:9494}"
export METRICS_QUACK_TOKEN_FILE="${METRICS_QUACK_TOKEN_FILE:-$HOME/.config/metrics-console/quack.token}"
export METRICS_CONSOLE_HOST="${METRICS_CONSOLE_HOST:-127.0.0.1}"
export METRICS_CONSOLE_PORT="${METRICS_CONSOLE_PORT:-9496}"
export PYTHONUNBUFFERED=1
exec "$ROOT/.venv/bin/python" -m metrics_console.app
EOF
chmod +x "$DST/run.sh"
echo "synced -> $DST"
echo "manage: curl -X POST http://127.0.0.1:9020/api/tunnel/metrics-console/restart"
