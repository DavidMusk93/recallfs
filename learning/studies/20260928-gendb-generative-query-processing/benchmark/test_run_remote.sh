#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(mktemp -d)"
trap 'rm -rf "${ROOT}"' EXIT

mkdir -p "${ROOT}/bin" "${ROOT}/evidence/run"
printf '%s\n' '#!/bin/sh' 'exit 0' > "${ROOT}/bin/flock"
chmod +x "${ROOT}/bin/flock"
printf '%s\n' '{"status":"stale"}' > "${ROOT}/evidence/run/complete.json"
printf '%s\n' '{"status":"old-benchmark"}' \
    > "${ROOT}/evidence/run/benchmark-results.json"
printf '%s\n' '{"status":"old-ablation"}' \
    > "${ROOT}/evidence/run/ablation-results.json"

if PATH="${ROOT}/bin:${PATH}" PYTHON_BIN=/usr/bin/false \
    bash "${SCRIPT_DIR}/run_remote.sh" "${ROOT}" >/dev/null 2>&1; then
    echo "run_remote.sh unexpectedly accepted missing preparation evidence" >&2
    exit 1
fi

if [[ -e "${ROOT}/evidence/run/complete.json" ]]; then
    echo "failed preflight retained a stale completion marker" >&2
    exit 1
fi
test -s "${ROOT}/evidence/run/benchmark-results.json"
test -s "${ROOT}/evidence/run/ablation-results.json"

echo "run_remote stale-marker test passed"
