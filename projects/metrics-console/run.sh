#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

export METRICS_QUACK_URI="${METRICS_QUACK_URI:-quack:10.37.125.152:9494}"
export METRICS_QUACK_TOKEN_FILE="${METRICS_QUACK_TOKEN_FILE:-$HOME/.config/metrics-console/quack.token}"
export METRICS_CONSOLE_HOST="${METRICS_CONSOLE_HOST:-127.0.0.1}"
export METRICS_CONSOLE_PORT="${METRICS_CONSOLE_PORT:-9496}"
export PYTHONPATH="$ROOT/src${PYTHONPATH:+:$PYTHONPATH}"

if [[ ! -f "$METRICS_QUACK_TOKEN_FILE" ]]; then
  echo "missing token: $METRICS_QUACK_TOKEN_FILE" >&2
  echo "ssh d2 'cat /root/data/duck/secrets/quack.token' > $METRICS_QUACK_TOKEN_FILE && chmod 600 $METRICS_QUACK_TOKEN_FILE" >&2
  exit 1
fi

if [[ ! -d .venv ]]; then
  uv venv .venv --python 3.13
  # shellcheck disable=SC1091
  source .venv/bin/activate
  uv pip install duckdb fastapi 'uvicorn[standard]' pydantic-settings pytz
else
  # shellcheck disable=SC1091
  source .venv/bin/activate
fi

echo "Metrics Console → http://${METRICS_CONSOLE_HOST}:${METRICS_CONSOLE_PORT}/"
echo "Quack URI: $METRICS_QUACK_URI"
exec python -m metrics_console.app
