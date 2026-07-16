#!/usr/bin/env bash
# Compress skill storyboard PNGs → WebP (960×540, q82) for fast remote serve.
# Usage:
#   bash learning/algorithms/scripts/optimize-assets.sh [problem-dir|assets-dir|png...]
# Examples:
#   bash learning/algorithms/scripts/optimize-assets.sh
#   bash learning/algorithms/scripts/optimize-assets.sh problems/0002-add-two-numbers
#   bash learning/algorithms/scripts/optimize-assets.sh path/to/frame.png
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="${WEBP_Q:-82}"
W="${WEBP_W:-960}"
H="${WEBP_H:-540}"
KEEP_PNG="${KEEP_PNG:-0}"

if ! command -v cwebp >/dev/null 2>&1; then
  echo "error: cwebp not found (brew install webp)" >&2
  exit 1
fi

collect() {
  if [[ $# -eq 0 ]]; then
    find "$ROOT/problems" -path '*/assets/*.png' -type f 2>/dev/null || true
    return
  fi
  for arg in "$@"; do
    if [[ -d "$arg" ]]; then
      find "$arg" -name '*.png' -type f
    elif [[ -f "$arg" && "$arg" == *.png ]]; then
      printf '%s\n' "$arg"
    else
      echo "skip: $arg" >&2
    fi
  done
}

n=0
while IFS= read -r png; do
  [[ -z "$png" ]] && continue
  out="${png%.png}.webp"
  cwebp -quiet -q "$Q" -m 6 -mt -resize "$W" "$H" "$png" -o "$out"
  before=$(wc -c <"$png" | tr -d ' ')
  after=$(wc -c <"$out" | tr -d ' ')
  printf '%s  %s → %s bytes  %s\n' "$(basename "$png")" "$before" "$after" "$out"
  if [[ "$KEEP_PNG" != "1" ]]; then
    rm -f "$png"
  fi
  n=$((n + 1))
done < <(collect "$@")

echo "done: $n file(s)  (q=$Q ${W}x${H} KEEP_PNG=$KEEP_PNG)"
