#!/usr/bin/env bash
set -euo pipefail

demo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$demo_dir/../../../.." && pwd)"
work_dir="${WORK_DIR:-$repo_root/.tmp/kache-build-cache-demo}"
kache_bin="${KACHE_BIN:-kache}"
blade_bin="${BLADE_BIN:-}"

rm -rf "$work_dir"
mkdir -p \
    "$work_dir/direct/cpp" \
    "$work_dir/direct/run" \
    "$work_dir/direct/rust-a/src" \
    "$work_dir/direct/rust-b/src"
cp "$demo_dir/Cargo.toml" "$work_dir/direct/rust-a/"
cp "$demo_dir/Cargo.toml" "$work_dir/direct/rust-b/"
cp "$demo_dir/src/main.rs" "$work_dir/direct/rust-a/src/"
cp "$demo_dir/src/main.rs" "$work_dir/direct/rust-b/src/"

export KACHE_CACHE_DIR="$work_dir/direct/cache"
export KACHE_RUNTIME_DIR="$work_dir/direct/run"
export KACHE_PROGRESS=verbose
export CARGO_INCREMENTAL=0

(
    cd "$work_dir/direct/rust-a"
    RUSTC_WRAPPER="$kache_bin" cargo build
)
(
    cd "$work_dir/direct/rust-b"
    RUSTC_WRAPPER="$kache_bin" cargo build
)

"$kache_bin" c++ -std=c++17 -O2 -c "$demo_dir/cpp/answer.cpp" \
    -o "$work_dir/direct/cpp/answer-a.o"
rm "$work_dir/direct/cpp/answer-a.o"
"$kache_bin" c++ -std=c++17 -O2 -c "$demo_dir/cpp/answer.cpp" \
    -o "$work_dir/direct/cpp/answer-b.o"

"$kache_bin" report --format json --since 15m >"$work_dir/direct-report.json"
jq -e '
    .summary.local_hits >= 2 and
    .summary.misses >= 2 and
    .summary.errors == 0
' "$work_dir/direct-report.json" >/dev/null

if [[ -n "$blade_bin" ]]; then
    blade_work="$work_dir/blade"
    shims="$blade_work/shims"
    mkdir -p "$blade_work/plain-a" "$blade_work/plain-b"
    "$kache_bin" install-shims --force "$shims" >/dev/null

    cp -R "$demo_dir/." "$blade_work/plain-a/"
    cp -R "$demo_dir/." "$blade_work/plain-b/"
    rm "$blade_work/plain-a/.kache.toml" "$blade_work/plain-b/.kache.toml"

    export PATH="$shims:$PATH"
    export KACHE_CACHE_DIR="$blade_work/plain-cache"
    export KACHE_RUNTIME_DIR="$blade_work/plain-run"
    (
        cd "$blade_work/plain-a"
        KACHE_BASE_DIR="$PWD" "$blade_bin" build //:answer
    )
    (
        cd "$blade_work/plain-b"
        KACHE_BASE_DIR="$PWD" "$blade_bin" build //:answer
    )
    "$kache_bin" report --format json --since 15m \
        >"$work_dir/blade-without-allowlist-report.json"
    jq -e '
        .summary.local_hits == 0 and
        any(.bypass.reasons[]; .reason | contains("-H"))
    ' "$work_dir/blade-without-allowlist-report.json" >/dev/null

    mkdir -p "$blade_work/fixed-a" "$blade_work/fixed-b"
    cp -R "$demo_dir/." "$blade_work/fixed-a/"
    cp -R "$demo_dir/." "$blade_work/fixed-b/"

    export KACHE_CACHE_DIR="$blade_work/fixed-cache"
    export KACHE_RUNTIME_DIR="$blade_work/fixed-run"
    (
        cd "$blade_work/fixed-a"
        KACHE_BASE_DIR="$PWD" "$blade_bin" build //:answer
    )
    (
        cd "$blade_work/fixed-b"
        KACHE_BASE_DIR="$PWD" "$blade_bin" build //:answer
    )
    "$kache_bin" report --format json --since 15m \
        >"$work_dir/blade-with-allowlist-report.json"
    jq -e '
        any(.all_events[]; .crate_name == "answer.cpp" and .result == "local_hit")
    ' "$work_dir/blade-with-allowlist-report.json" >/dev/null
    cmp \
        "$blade_work/fixed-a/build_release/answer.objs/cpp/answer.cpp.incstk" \
        "$blade_work/fixed-b/build_release/answer.objs/cpp/answer.cpp.incstk"
fi

printf 'PASS: direct Rust and C++ miss-to-hit anchors verified\n'
if [[ -n "$blade_bin" ]]; then
    printf 'PASS: Blade -H passthrough and allowlisted hit anchors verified\n'
fi
