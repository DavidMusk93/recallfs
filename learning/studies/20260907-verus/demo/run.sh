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

if ! verus_version_json="$("$verus_bin" --version --output-json 2>&1)"; then
    echo "Unable to read Verus version metadata:" >&2
    printf '%s\n' "$verus_version_json" >&2
    exit 2
fi
toolchain="$(
    printf '%s\n' "$verus_version_json" |
        sed -n 's/^[[:space:]]*"toolchain"[[:space:]]*:[[:space:]]*"\([^"[:space:]]*\).*$/\1/p'
)"
if [[ -z "$toolchain" || "$toolchain" == *$'\n'* ]]; then
    echo "Unable to extract the required Rust toolchain from Verus version metadata:" >&2
    printf '%s\n' "$verus_version_json" >&2
    exit 2
fi

if [[ -n "${VERUS_Z3_PATH:-}" ]]; then
    selected_z3="$VERUS_Z3_PATH"
elif [[ -x "$local_z3" ]]; then
    selected_z3="$local_z3"
elif selected_z3="$(command -v z3 2>/dev/null)"; then
    :
else
    cat >&2 <<EOF
Z3 executable not found.

Install Z3 4.16.0 or set:
  VERUS_Z3_PATH=/absolute/path/to/z3
EOF
    exit 2
fi

if [[ ! -x "$selected_z3" ]]; then
    echo "Z3 executable not found or not executable: $selected_z3" >&2
    exit 2
fi
if ! z3_version="$("$selected_z3" --version 2>&1)"; then
    echo "Unable to read Z3 version from $selected_z3:" >&2
    printf '%s\n' "$z3_version" >&2
    exit 2
fi
if [[ "$z3_version" != "Z3 version 4.16.0" &&
    "$z3_version" != "Z3 version 4.16.0 "* ]]; then
    echo "Unsupported Z3 version from $selected_z3: $z3_version" >&2
    echo "Expected Z3 version 4.16.0." >&2
    exit 2
fi
export VERUS_Z3_PATH="$selected_z3"

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
    local matched_line error_count
    if ! matched_line="$(grep -F -m 1 "$expected_text" "$log_file")"; then
        echo "Verus rejected $source_file for an unexpected reason:" >&2
        cat "$log_file" >&2
        exit 1
    fi
    error_count="$(
        awk '{ count += gsub(/error:/, "&") } END { print count + 0 }' "$log_file"
    )"
    if [[ "$error_count" -ne 2 ]]; then
        echo "Verus produced $error_count error-level diagnostics for $source_file; expected 2:" >&2
        cat "$log_file" >&2
        exit 1
    fi
    printf '%s\n' "$matched_line"
}

echo "[1/5] Ordinary Rust tests exercise only selected inputs"
rustup run "$toolchain" rustc --edition=2021 --test "$demo_dir/ordinary_rust.rs" \
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
