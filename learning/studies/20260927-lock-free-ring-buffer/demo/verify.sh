#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../../../.." && pwd)"
work_dir="$repo_root/.tmp/lock-free-ring-buffer/verify"

filcc="${FILCC:-$repo_root/.tmp/fil-c/bin/filcc}"
filrun="${FILRUN:-$repo_root/.tmp/fil-c/bin/filrun}"
zig="${ZIG:-$repo_root/.tmp/zig/dist-macos-0.16.0/zig}"
apple_clang="${APPLE_CLANG:-/usr/bin/clang}"
cmake="${CMAKE:-$repo_root/.tmp/tooling/cmake/venv/bin/cmake}"
ctest="${CTEST:-$repo_root/.tmp/tooling/cmake/venv/bin/ctest}"
ninja="${NINJA:-$repo_root/.tmp/tooling/cmake/venv/bin/ninja}"
clang_format="${CLANG_FORMAT:-$repo_root/.tmp/tooling/clang-format/venv/bin/clang-format}"

include_dir="$script_dir/include"
ring_source="$script_dir/src/spsc_ring.c"
test_source="$script_dir/tests/spsc_ring_test.c"
benchmark_source="$script_dir/src/benchmark.c"

common_flags=(
    -std=c11
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wconversion
    -Wshadow
    -Wstrict-prototypes
    -I "$include_dir"
    -pthread
)

require_executable() {
    local path=$1
    [[ -x "$path" ]] || {
        printf 'missing executable: %s\n' "$path" >&2
        exit 1
    }
}

native_cache_line() {
    local value

    if [[ "$(uname -s)" == Darwin ]]; then
        value="$(sysctl -n hw.cachelinesize)"
    else
        value="$(getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null || true)"
    fi
    if [[ -z "$value" || "$value" == 0 ]]; then
        value=64
    fi
    printf '%s\n' "$value"
}

build_and_run_test() {
    local name=$1
    shift
    local binary="$work_dir/$name"

    "$zig" cc "${common_flags[@]}" "$@" \
        "$ring_source" "$test_source" -o "$binary"
    "$binary"
}

require_executable "$filcc"
require_executable "$filrun"
require_executable "$zig"
require_executable "$cmake"
require_executable "$ctest"
require_executable "$ninja"
require_executable "$clang_format"
mkdir -p "$work_dir"

cache_line="${SPSC_CACHE_LINE_SIZE:-$(native_cache_line)}"

printf '%s\n' '== Formatting =='
"$clang_format" --dry-run --Werror \
    "$include_dir/spsc_ring.h" \
    "$ring_source" \
    "$benchmark_source" \
    "$test_source"

printf '%s\n' '== FIL-C correctness =='
"$filcc" "${common_flags[@]}" -O2 -g -DSPSC_CACHE_LINE_SIZE=64 \
    "$ring_source" "$test_source" -o "$work_dir/spsc-ring-test-filc"
"$filrun" "$work_dir/spsc-ring-test-filc"
"$filcc" "${common_flags[@]}" -O2 -g -DSPSC_CACHE_LINE_SIZE=64 \
    "$ring_source" "$benchmark_source" \
    -o "$work_dir/spsc-ring-benchmark-filc"
"$filrun" "$work_dir/spsc-ring-benchmark-filc" --self-check

printf '%s\n' '== CMake with FIL-C =='
rm -rf "$work_dir/cmake-filc"
"$cmake" \
    -S "$script_dir" \
    -B "$work_dir/cmake-filc" \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_COMPILER="$filcc" \
    -DFIL_RUNNER="$filrun" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPSC_CACHE_LINE_SIZE=64
"$cmake" --build "$work_dir/cmake-filc" -j
"$ctest" --test-dir "$work_dir/cmake-filc" --output-on-failure

printf '%s\n' '== Zig correctness profiles =='
build_and_run_test test-o0 -O0 -g -DSPSC_CACHE_LINE_SIZE="$cache_line"
build_and_run_test test-o2 -O2 -g -DSPSC_CACHE_LINE_SIZE="$cache_line"
build_and_run_test test-o3 -O3 -march=native -mtune=native -DNDEBUG -g \
    -DSPSC_CACHE_LINE_SIZE="$cache_line"

printf '%s\n' '== CMake with Zig =='
rm -rf "$work_dir/cmake-zig"
"$cmake" \
    -S "$script_dir" \
    -B "$work_dir/cmake-zig" \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_C_COMPILER="$zig;cc" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPSC_CACHE_LINE_SIZE="$cache_line"
"$cmake" --build "$work_dir/cmake-zig" -j
"$ctest" --test-dir "$work_dir/cmake-zig" --output-on-failure

printf '%s\n' '== ASan and UBSan =='
if [[ "$(uname -s)" == Darwin ]]; then
    require_executable "$apple_clang"
    mkdir -p "$work_dir/asan"
    printf '%s\n' \
        'extern void __asan_version_mismatch_check_apple_clang_2100(void);' \
        'void __asan_version_mismatch_check_v8(void) {' \
        '    __asan_version_mismatch_check_apple_clang_2100();' \
        '}' >"$work_dir/asan/version-shim.c"
    "$zig" cc -std=c11 -O2 -c "$work_dir/asan/version-shim.c" \
        -o "$work_dir/asan/version-shim.o"
    "$zig" cc "${common_flags[@]}" -O1 -g -fno-omit-frame-pointer \
        -fsanitize=address -fsanitize=undefined -Wno-macro-redefined \
        -DSPSC_CACHE_LINE_SIZE="$cache_line" \
        -c "$ring_source" -o "$work_dir/asan/spsc_ring.o"
    "$zig" cc "${common_flags[@]}" -O1 -g -fno-omit-frame-pointer \
        -fsanitize=address -fsanitize=undefined -Wno-macro-redefined \
        -DSPSC_CACHE_LINE_SIZE="$cache_line" \
        -c "$test_source" -o "$work_dir/asan/spsc_ring_test.o"
    "$apple_clang" -fsanitize=address -fsanitize=undefined \
        -mmacosx-version-min="$(sw_vers -productVersion)" \
        "$work_dir/asan/spsc_ring.o" "$work_dir/asan/spsc_ring_test.o" \
        "$work_dir/asan/version-shim.o" -pthread \
        -o "$work_dir/test-asan-ubsan"
else
    "$zig" cc "${common_flags[@]}" -O1 -g -fno-omit-frame-pointer \
        -fsanitize=address -fsanitize=undefined -Wno-macro-redefined \
        -DSPSC_CACHE_LINE_SIZE="$cache_line" \
        "$ring_source" "$test_source" -o "$work_dir/test-asan-ubsan"
fi
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work_dir/test-asan-ubsan"

printf '%s\n' '== ThreadSanitizer =='
mkdir -p "$work_dir/tsan"
"$zig" cc "${common_flags[@]}" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=thread -DSPSC_CACHE_LINE_SIZE="$cache_line" \
    -c "$ring_source" -o "$work_dir/tsan/spsc_ring.o"
"$zig" cc "${common_flags[@]}" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=thread -DSPSC_CACHE_LINE_SIZE="$cache_line" \
    -c "$test_source" -o "$work_dir/tsan/spsc_ring_test.o"
if [[ "$(uname -s)" == Darwin ]]; then
    require_executable "$apple_clang"
    "$apple_clang" -fsanitize=thread \
        -mmacosx-version-min="$(sw_vers -productVersion)" \
        "$work_dir/tsan/spsc_ring.o" "$work_dir/tsan/spsc_ring_test.o" \
        -pthread -o "$work_dir/test-tsan"
else
    "$zig" cc -fsanitize=thread \
        "$work_dir/tsan/spsc_ring.o" "$work_dir/tsan/spsc_ring_test.o" \
        -pthread -o "$work_dir/test-tsan"
fi
TSAN_OPTIONS=halt_on_error=1 "$work_dir/test-tsan"

printf '%s\n' '== Native benchmark self-check =='
"$zig" cc "${common_flags[@]}" -O3 -march=native -mtune=native -DNDEBUG -g \
    -DSPSC_CACHE_LINE_SIZE="$cache_line" \
    "$ring_source" "$benchmark_source" \
    -o "$work_dir/spsc-ring-benchmark"
"$work_dir/spsc-ring-benchmark" --self-check \
    --hardware-cache-line "$cache_line"

if [[ "${RUN_BENCHMARK:-0}" == 1 ]]; then
    printf '%s\n' '== Native article-shape benchmark =='
    "$work_dir/spsc-ring-benchmark" \
        --count 100000000 \
        --capacity 100000 \
        --samples "${BENCHMARK_SAMPLES:-5}" \
        --hardware-cache-line "$cache_line"
fi

printf '%s\n' 'verification complete'
