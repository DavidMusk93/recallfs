#!/usr/bin/env bash
set -euo pipefail

demo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$demo_dir" rev-parse --show-toplevel)"
build_dir="$demo_dir/build"
verus_bin="${VERUS_BIN:-$repo_root/.tmp/verus-toolchain/verus-arm64-macos/verus}"
local_z3="$repo_root/.tmp/verus-z3/z3"

if [[ ! -x "$verus_bin" ]]; then
    cat >&2 <<EOF
Verus executable not found: $verus_bin

Install Verus using the official instructions, then run:
  VERUS_BIN=/absolute/path/to/verus $0
EOF
    exit 2
fi

if [[ -z "${VERUS_Z3_PATH:-}" ]] && ! command -v z3 >/dev/null 2>&1; then
    if [[ -x "$local_z3" ]]; then
        export VERUS_Z3_PATH="$local_z3"
    else
        cat >&2 <<EOF
Z3 executable not found.

Install Z3 4.16.0 or set:
  VERUS_Z3_PATH=/absolute/path/to/z3
EOF
        exit 2
    fi
fi

rm -rf "$build_dir"
mkdir -p "$build_dir"

run_verus() {
    local source_file="$1"
    shift
    "$verus_bin" "$demo_dir/$source_file" --time "$@"
}

expect_verus_failure() {
    local source_file="$1"
    local expected_text="$2"
    local log_file="$build_dir/${source_file%.rs}.log"

    if run_verus "$source_file" --expand-errors >"$log_file" 2>&1; then
        echo "expected Verus to reject $source_file" >&2
        exit 1
    fi
    local matched_line
    if ! matched_line="$(grep -F -m 1 "$expected_text" "$log_file")"; then
        echo "Verus rejected $source_file for an unexpected reason:" >&2
        cat "$log_file" >&2
        exit 1
    fi
    printf '%s\n' "$matched_line"
}

echo "[1/5] Ordinary Rust tests exercise only selected inputs"
rustc --edition=2021 --test "$demo_dir/ordinary_rust.rs" \
    -o "$build_dir/ordinary_rust_tests"
"$build_dir/ordinary_rust_tests"

echo
echo "[2/5] Verus rejects arithmetic without a proven bound"
expect_verus_failure "unbounded.rs" "possible arithmetic underflow/overflow"

echo
echo "[3/5] Verus accepts a wrong implementation against a weak specification"
run_verus "weak_spec.rs" --no-cheating | tee "$build_dir/weak_spec.log"

echo
echo "[4/5] Verus rejects the off-by-one implementation against a strong specification"
expect_verus_failure "strong_spec_bug.rs" "postcondition not satisfied"

echo
echo "[5/5] Verus proves, compiles, and runs the corrected implementation"
run_verus "verified.rs" --no-cheating --compile -o "$build_dir/verified" \
    | tee "$build_dir/verified.log"
"$build_dir/verified"
echo "verified executable exited successfully"
