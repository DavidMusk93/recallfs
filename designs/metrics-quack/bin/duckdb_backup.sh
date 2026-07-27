#!/usr/bin/env bash
# Cold snapshot of metrics DuckDB files (HA-lite slave materialization).
set -euo pipefail
ROOT="${DUCK_ROOT:-/root/data/duck}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
DEST="${ROOT}/backup/${STAMP}"
mkdir -p "$DEST"

# Prefer online CHECKPOINT via local python if venv present
if [[ -x "${ROOT}/venv/bin/python" ]]; then
  "${ROOT}/venv/bin/python" - <<PY || true
import duckdb
from pathlib import Path
root = Path("${ROOT}")
con = duckdb.connect(str(root / "db" / "master.duckdb"), read_only=False)
try:
    con.execute("CHECKPOINT")
except Exception as e:
    print("checkpoint master:", e)
for p in (root / "db").glob("cat_*.duckdb"):
    try:
        c = duckdb.connect(str(p))
        c.execute("CHECKPOINT")
        c.close()
    except Exception as e:
        print("checkpoint", p, e)
con.close()
PY
fi

cp -a "${ROOT}/db/"*.duckdb "$DEST/" 2>/dev/null || true
# retain last 14 snapshots
ls -1dt "${ROOT}/backup"/*/ 2>/dev/null | tail -n +15 | xargs -r rm -rf
echo "backup ok: $DEST"
