#!/usr/bin/env bash
# Smoke-check Algorithms Lab prerequisites.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="${HOME}/.cargo/bin:${PATH}"

ok=0
fail=0
check() {
  local name="$1"; shift
  if "$@"; then
    echo "[ok]   $name"
    ok=$((ok + 1))
  else
    echo "[fail] $name"
    fail=$((fail + 1))
  fi
}

echo "Algorithms Lab smoke-check"
echo "root: $ROOT"
echo

check "rustc" rustc --version
check "cargo" cargo --version
check "progress.md" test -f "${ROOT}/progress.md"
check "WORKFLOW.md" test -f "${ROOT}/WORKFLOW.md"
check "leetcode skill" test -f "${ROOT}/../../.grok/skills/leetcode/SKILL.md"
check "xiaohei skill" test -f "${HOME}/.grok/skills/ian-xiaohei-illustrations/SKILL.md"
check "network-accel" test -f "${HOME}/.grok/skills/network-accel/SKILL.md"
check "templates" test -f "${ROOT}/templates/learn.html"
check "new-problem.sh" test -x "${ROOT}/scripts/new-problem.sh"

if [[ -f "${ROOT}/Cargo.toml" ]]; then
  if grep -q 'problems/' "${ROOT}/Cargo.toml"; then
    check "cargo metadata" cargo metadata --manifest-path "${ROOT}/Cargo.toml" --no-deps --format-version 1 >/dev/null
  else
    echo "[skip] cargo metadata (no workspace members yet)"
  fi
fi

echo
echo "summary: ok=${ok} fail=${fail}"
[[ "$fail" -eq 0 ]]
