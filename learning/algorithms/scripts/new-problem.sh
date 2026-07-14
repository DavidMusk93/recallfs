#!/usr/bin/env bash
# Scaffold a LeetCode problem directory + cargo workspace member.
# Usage:
#   bash learning/algorithms/scripts/new-problem.sh <id> <slug> "<title_zh>" <Easy|Medium|Hard> [title_en]
# Example:
#   bash learning/algorithms/scripts/new-problem.sh 1 two-sum "两数之和" Easy "Two Sum"

set -euo pipefail

die() { echo "error: $*" >&2; exit 2; }

[[ $# -ge 4 ]] || die "usage: new-problem.sh <id> <slug> \"<title_zh>\" <Easy|Medium|Hard> [title_en]"

ID_RAW="$1"
SLUG="$2"
TITLE_ZH="$3"
DIFFICULTY="$4"
TITLE_EN="${5:-$SLUG}"

[[ "$ID_RAW" =~ ^[0-9]+$ ]] || die "id must be a positive integer"
[[ "$SLUG" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]] || die "slug must be kebab-case"
case "$DIFFICULTY" in
  Easy|Medium|Hard) ;;
  *) die "difficulty must be Easy|Medium|Hard" ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TEMPLATES="${ROOT}/templates"
PADDED="$(printf "%04d" "$ID_RAW")"
DIR_NAME="${PADDED}-${SLUG}"
PROBLEM_DIR="${ROOT}/problems/${DIR_NAME}"
# crate name: p0001_two_sum
SLUG_US="${SLUG//-/_}"
CRATE="p${PADDED}_${SLUG_US}"

[[ -d "$TEMPLATES" ]] || die "templates not found: $TEMPLATES"
[[ ! -e "$PROBLEM_DIR" ]] || die "already exists: $PROBLEM_DIR"

mkdir -p "${PROBLEM_DIR}/src" "${PROBLEM_DIR}/assets"

render() {
  local src="$1" dest="$2"
  ID="$ID_RAW" SLUG="$SLUG" TITLE_ZH="$TITLE_ZH" TITLE_EN="$TITLE_EN" \
  DIFFICULTY="$DIFFICULTY" CRATE="$CRATE" \
  python3 - "$src" "$dest" <<'PY'
import os, sys
src, dest = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
for k, v in {
    "{{ID}}": os.environ["ID"],
    "{{SLUG}}": os.environ["SLUG"],
    "{{TITLE_ZH}}": os.environ["TITLE_ZH"],
    "{{TITLE_EN}}": os.environ["TITLE_EN"],
    "{{DIFFICULTY}}": os.environ["DIFFICULTY"],
    "{{CRATE}}": os.environ["CRATE"],
}.items():
    text = text.replace(k, v)
open(dest, "w", encoding="utf-8").write(text)
PY
}

render "${TEMPLATES}/meta.md" "${PROBLEM_DIR}/meta.md"
render "${TEMPLATES}/analysis.md" "${PROBLEM_DIR}/analysis.md"
render "${TEMPLATES}/notes.md" "${PROBLEM_DIR}/notes.md"
render "${TEMPLATES}/learn.html" "${PROBLEM_DIR}/learn.html"
render "${TEMPLATES}/Cargo.toml" "${PROBLEM_DIR}/Cargo.toml"
render "${TEMPLATES}/lib.rs" "${PROBLEM_DIR}/src/lib.rs"

# Register workspace member if missing
WS="${ROOT}/Cargo.toml"
MEMBER="problems/${DIR_NAME}"
if ! grep -q "\"${MEMBER}\"" "$WS" 2>/dev/null; then
  python3 - "$WS" "$MEMBER" <<'PY'
import sys
path, member = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
needle = "members = ["
idx = text.find(needle)
if idx < 0:
    raise SystemExit("Cargo.toml: members = [ not found")
# find closing ] of members array (first line-based simple approach)
start = idx + len(needle)
# insert before closing bracket of members
# handle empty members = []
close = text.find("]", start)
inner = text[start:close].strip()
entry = f'"{member}"'
if entry in text[start:close]:
    pass
else:
    if not inner:
        new_inner = f"\n    {entry},\n"
    else:
        # ensure trailing comma style
        new_inner = text[start:close]
        if not new_inner.rstrip().endswith(","):
            # put comma after last entry content
            new_inner = new_inner.rstrip() + ",\n"
        new_inner = new_inner + f"    {entry},\n"
    text = text[:start] + new_inner + text[close:]
    open(path, "w", encoding="utf-8").write(text)
    print(f"workspace: added {member}")
PY
fi

# assets placeholder
cat > "${PROBLEM_DIR}/assets/.gitkeep" <<'EOF'
EOF

echo "created: ${PROBLEM_DIR}"
echo "crate:   ${CRATE}"
echo "test:    cargo test -p ${CRATE} --manifest-path ${ROOT}/Cargo.toml"
echo "learn:   open ${PROBLEM_DIR}/learn.html"
