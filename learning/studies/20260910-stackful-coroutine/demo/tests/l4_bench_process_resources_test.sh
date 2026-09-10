#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/../bench/process_resources.sh"

fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rco-proc-fixture.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT
mkdir -p "$fixture_root/4242"

cat >"$fixture_root/4242/stat" <<'EOF'
4242 (worker ) with spaces) S 1 2 3 4 5 6 7 8 9 10 123 456 0 0 0 0 0
EOF
cat >"$fixture_root/4242/status" <<'EOF'
Name:	worker ) with spaces
VmPeak:	      111 kB
VmHWM:	       222 kB
voluntary_ctxt_switches:	333
nonvoluntary_ctxt_switches:	444
EOF

actual=$(read_process_resources 4242 "$fixture_root")
expected=111,222,123,456,333,444
if [[ "$actual" != "$expected" ]]; then
    printf 'expected %s, got %s\n' "$expected" "$actual" >&2
    exit 1
fi

echo "process resource parser test passed"
