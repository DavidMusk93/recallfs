#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
readonly REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
readonly STUDY_REL="learning/studies/20260915-affine-decision-tree-point-location"
readonly STUDY_DIR="$REPO_ROOT/$STUDY_REL"
readonly LIB_DIR="$STUDY_DIR/lib"
readonly EXAMPLE_DIR="$STUDY_DIR/example"
readonly EVIDENCE_DIR="$STUDY_DIR/evidence"
readonly RAW_DIR="$EVIDENCE_DIR/raw"
readonly BUILD_ROOT="$REPO_ROOT/.tmp/affine-decision-tree/u8-verify"
readonly LOG_DIR="$BUILD_ROOT/logs"
readonly GENERATED_EVIDENCE="$BUILD_ROOT/evidence"
readonly STAGES_TSV="$BUILD_ROOT/stages.tsv"
readonly COMMANDS_TSV="$BUILD_ROOT/commands.tsv"
readonly TOOLS_TSV="$BUILD_ROOT/tools.tsv"
readonly RAW_MAP_TSV="$BUILD_ROOT/raw-map.tsv"
readonly SOURCE_FILES="$BUILD_ROOT/source-files.txt"
readonly DATA_FILES="$BUILD_ROOT/data-files.txt"
readonly BINARY_FILES="$BUILD_ROOT/binary-files.txt"
readonly TEST_NAMES="$BUILD_ROOT/test-names.txt"

readonly EXPECTED_SOURCE_COUNT=46
readonly EXPECTED_DATA_COUNT=5
readonly EXPECTED_FORMAT_COUNT=27
readonly EXPECTED_CTEST_COUNT=10
readonly EXPECTED_PYTHON_TEST_COUNT=10
readonly EXPECTED_FILC_TEST_COUNT=9
readonly EXPECTED_SANITIZER_GATE_COUNT=11
readonly EXPECTED_STATIC_SOURCE_COUNT=9
readonly EXPECTED_FOCUSED_TEST_COUNT=5
readonly EXPECTED_INSTALL_FILE_COUNT=6
readonly EXPECTED_CONSUMER_COUNT=2
readonly EXPECTED_NEGATIVE_PROBE_COUNT=7
readonly EXPECTED_BENCHMARK_CHECKSUM="5c7f9b20aab8aad3"
readonly BENCHMARK_TARGET="fdbd:dc02:e:137::47"
readonly BENCHMARK_FLAGS="-O3 -march=native -mtune=native -DNDEBUG"
readonly TARGET_COMMAND_TIMEOUT_SECONDS=15
readonly TIMEOUT_PROBE_SECONDS=0.2
readonly FILC_ARCHIVE_SHA256="564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666"
readonly ZIG_ARCHIVE_SHA256="b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489"

FILCC="${FILCC:-$REPO_ROOT/.tmp/fil-c/bin/filcc}"
FILRUN="${FILRUN:-$REPO_ROOT/.tmp/fil-c/bin/filrun}"
FILC_ARCHIVE="${FILC_ARCHIVE:-$REPO_ROOT/.tmp/fil-c/downloads/filc-0.684-linux-aarch64.tar.xz}"
ZIG="${ZIG:-$REPO_ROOT/.tmp/zig/dist-macos-0.16.0/zig}"
ZIG_ARCHIVE="${ZIG_ARCHIVE:-$REPO_ROOT/.tmp/zig/zig-aarch64-macos-0.16.0.tar.xz}"
CMAKE="${CMAKE:-$(command -v cmake || true)}"
CTEST="${CTEST:-$(command -v ctest || true)}"
NINJA="${NINJA:-$(command -v ninja || true)}"
PYENV="${PYENV:-$(command -v pyenv || true)}"
UV="${UV:-$(command -v uv || true)}"
CLANG_FORMAT="${CLANG_FORMAT:-$(command -v clang-format || true)}"
CLANG_TIDY="${CLANG_TIDY:-$(command -v clang-tidy || true)}"
SANITIZER_LINKER="${SANITIZER_LINKER:-/usr/bin/clang}"
ASAN_RUNTIME="${ASAN_RUNTIME:-/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/lib/darwin/libclang_rt.asan_osx_dynamic.dylib}"
JQ="${JQ:-$(command -v jq || true)}"
SHASUM="${SHASUM:-$(command -v shasum || true)}"
NATIVE_CC="${NATIVE_CC:-$(command -v cc || true)}"
ORTHRUS_CLI="${ORTHRUS_CLI:-$(command -v orthrus-cli || true)}"
SSH="${SSH:-$(command -v ssh || true)}"

current_stage=preflight

fail() {
    printf 'ODT verification failed at stage %s: %s\n' "$current_stage" "$*" >&2
    exit 1
}

on_error() {
    local status=$?

    printf 'ODT verification failed at stage %s (exit %d); full logs: %s\n' \
        "$current_stage" "$status" "$LOG_DIR" >&2
    exit "$status"
}
trap on_error ERR

require_executable() {
    local name=$1
    local path=$2

    [[ -n "$path" && -x "$path" ]] || fail "missing executable for $name: $path"
}

sha256() {
    "$SHASUM" -a 256 "$1" | awk '{print $1}'
}

expect_sha256() {
    local path=$1
    local expected=$2
    local actual

    [[ -f "$path" ]] || fail "missing pinned artifact: $path"
    actual="$(sha256 "$path")"
    [[ "$actual" == "$expected" ]] ||
        fail "digest mismatch for $path: expected $expected, got $actual"
}

record_command() {
    local stage=$1
    shift

    printf '%s\t' "$stage" >>"$COMMANDS_TSV"
    if (($# != 0)); then
        printf '%q' "$1" >>"$COMMANDS_TSV"
        shift
    fi
    while (($# != 0)); do
        printf ' %q' "$1" >>"$COMMANDS_TSV"
        shift
    done
    printf '\n' >>"$COMMANDS_TSV"
}

run_logged() {
    local stage=$1
    local log=$2
    shift 2

    record_command "$stage" "$@"
    "$@" >>"$log" 2>&1
}

run_with_timeout() {
    local stage=$1
    local log=$2
    local timeout_seconds=$3
    shift 3

    record_command "$stage" portable-timeout "$timeout_seconds" "$@"
    "$PYTHON" - "$timeout_seconds" "$log" "$@" <<'PY'
import os
import shlex
import signal
import subprocess
import sys

timeout_seconds = float(sys.argv[1])
log_path = sys.argv[2]
command = sys.argv[3:]

with open(log_path, "a", encoding="utf-8") as log:
    try:
        process = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except OSError as error:
        print(
            f"timeout_status=not_started error={error} command={shlex.join(command)}",
            file=log,
            flush=True,
        )
        raise SystemExit(127)

    try:
        status = process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        print(
            f"timeout_status=timed_out timeout_seconds={timeout_seconds:g} "
            f"command={shlex.join(command)}",
            file=log,
            flush=True,
        )
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=0.25)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        raise SystemExit(124)

    print(
        f"timeout_status=completed exit_status={status} command={shlex.join(command)}",
        file=log,
        flush=True,
    )
    raise SystemExit(status if status >= 0 else 128 - status)
PY
}

run_in_dir_logged() {
    local stage=$1
    local log=$2
    local directory=$3
    shift 3

    printf '%s\t(cd %q && ' "$stage" "$directory" >>"$COMMANDS_TSV"
    printf '%q ' "$@" >>"$COMMANDS_TSV"
    printf ')\n' >>"$COMMANDS_TSV"
    (
        cd "$directory"
        "$@"
    ) >>"$log" 2>&1
}

expect_failure() {
    local stage=$1
    local log=$2
    shift 2

    record_command "$stage" "$@"
    if ("$@" >>"$log" 2>&1) 2>>"$log"; then
        fail "negative probe unexpectedly succeeded: $*"
    fi
}

record_stage() {
    local id=$1
    local status=$2
    local required=$3
    local expected=$4
    local observed=$5
    local tool=$6
    local record=$7
    local limitation=$8

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$id" "$status" "$required" "$expected" "$observed" "$tool" "$record" "$limitation" \
        >>"$STAGES_TSV"
}

assert_ctest_count() {
    local log=$1
    local expected=$2

    grep -Eq "100% tests passed, 0 tests failed out of ${expected}" "$log" ||
        fail "CTest result does not report ${expected}/${expected} passed: $log"
}

assert_no_unexpected_untracked() {
    local unexpected="$BUILD_ROOT/unexpected-untracked.txt"

    git -C "$REPO_ROOT" ls-files --others --exclude-standard -- "$STUDY_REL" |
        awk -v verify="$STUDY_REL/verify.sh" \
            -v manifest="$STUDY_REL/evidence/manifest.json" \
            -v benchmark="$STUDY_REL/example/src/benchmark.c" \
            -v library_readme="$STUDY_REL/lib/README.md" \
            -v raw_prefix="$STUDY_REL/evidence/raw/u7-" \
            -v benchmark_disassembly="$STUDY_REL/evidence/raw/u8-benchmark-disassembly.txt" \
            -v benchmark_environment="$STUDY_REL/evidence/raw/u8-benchmark-environment.txt" \
            -v benchmark_json="$STUDY_REL/evidence/raw/u8-benchmark.json" \
            -v benchmark_summary="$STUDY_REL/evidence/raw/u8-benchmark.txt" \
            -v target_access="$STUDY_REL/evidence/raw/u8-target-access.txt" '
                $0 == verify ||
                $0 == manifest ||
                $0 == benchmark ||
                $0 == library_readme ||
                $0 == raw_prefix "commands.txt" ||
                $0 == raw_prefix "filc.txt" ||
                $0 == raw_prefix "limitations.txt" ||
                $0 == raw_prefix "native.txt" ||
                $0 == raw_prefix "negative-probes.txt" ||
                $0 == raw_prefix "package.txt" ||
                $0 == raw_prefix "protocol.txt" ||
                $0 == raw_prefix "safety.txt" ||
                $0 == raw_prefix "static-format.txt" ||
                $0 == raw_prefix "tools.txt" ||
                $0 == benchmark_disassembly ||
                $0 == benchmark_environment ||
                $0 == benchmark_json ||
                $0 == benchmark_summary ||
                $0 == target_access { next }
                { print }
            ' >"$unexpected"
    [[ ! -s "$unexpected" ]] ||
        fail "unexpected untracked artifact under study: $(tr '\n' ' ' <"$unexpected")"
}

append_binary() {
    local path=$1

    [[ -f "$path" ]] || fail "expected binary is missing: $path"
    printf '%s\n' "${path#"$REPO_ROOT"/}" >>"$BINARY_FILES"
}

write_wrapper() {
    local path=$1
    local subcommand=$2

    printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" "$subcommand" >"$path"
    chmod 0755 "$path"
}

write_sanitizer_wrappers() {
    local asan_wrapper="$BUILD_ROOT/bin/zig-asan-ubsan-cc"
    local tsan_wrapper="$BUILD_ROOT/bin/zig-tsan-cc"

    printf '%s\n' \
        '#!/bin/sh' \
        "zig='$ZIG'" \
        "linker='$SANITIZER_LINKER'" \
        "shim='$BUILD_ROOT/asan-version-shim.o'" \
        'compile=false' \
        'asan=false' \
        'for argument in "$@"; do' \
        '    [ "$argument" = -c ] && compile=true' \
        '    [ "$argument" = -fsanitize=address ] && asan=true' \
        'done' \
        'if [ "$compile" = true ] || [ "$asan" = false ]; then' \
        '    exec "$zig" cc "$@"' \
        'fi' \
        'exec "$linker" "$@" "$shim"' >"$asan_wrapper"
    printf '%s\n' \
        '#!/bin/sh' \
        "zig='$ZIG'" \
        "linker='$SANITIZER_LINKER'" \
        'compile=false' \
        'tsan=false' \
        'for argument in "$@"; do' \
        '    [ "$argument" = -c ] && compile=true' \
        '    [ "$argument" = -fsanitize=thread ] && tsan=true' \
        'done' \
        'if [ "$compile" = true ] || [ "$tsan" = false ]; then' \
        '    exec "$zig" cc "$@"' \
        'fi' \
        'exec "$linker" "$@"' >"$tsan_wrapper"
    chmod 0755 "$asan_wrapper" "$tsan_wrapper"
}

collect_tracked_inputs() {
    git -C "$REPO_ROOT" ls-files --cached --others --exclude-standard -- \
        "$STUDY_REL/lib" "$STUDY_REL/example" "$STUDY_REL/README.md" \
        "$STUDY_REL/evidence/README.md" "$STUDY_REL/evidence/review.md" \
        "$STUDY_REL/evidence/simplification.md" "$STUDY_REL/exploration.md" \
        "$STUDY_REL/source.md" "$STUDY_REL/verify.sh" |
        awk '
            /(\.c|\.h|\.cpp|\.py|\.md|CMakeLists\.txt|\.cmake|\.cmake\.in|\.toml|uv\.lock|\.clang-format|verify\.sh)$/ {
                print
            }
        ' |
        LC_ALL=C sort >"$SOURCE_FILES"
    [[ "$(wc -l <"$SOURCE_FILES" | tr -d ' ')" == "$EXPECTED_SOURCE_COUNT" ]] ||
        fail "tracked source inventory count changed; update the explicit qualification gate"

    git -C "$REPO_ROOT" ls-files --cached --others --exclude-standard -- \
        "$STUDY_REL/lib" "$STUDY_REL/example" |
        awk '/(\.csv|\.odt|\.sha256)$/ { print }' |
        LC_ALL=C sort >"$DATA_FILES"
    [[ "$(wc -l <"$DATA_FILES" | tr -d ' ')" == "$EXPECTED_DATA_COUNT" ]] ||
        fail "tracked data inventory count changed; update the explicit qualification gate"

    printf '%s\n' \
        odt_api_test \
        odt_batch_test \
        odt_builder_test \
        odt_concurrency_test \
        odt_corruption_test \
        odt_fault_test \
        odt_geometry_test \
        odt_persistence_test \
        odt_query_test >"$TEST_NAMES"

    local actual_tests="$BUILD_ROOT/actual-test-names.txt"
    local source
    : >"$actual_tests"
    for source in "$LIB_DIR"/tests/odt_*_test.c; do
        basename "$source" .c >>"$actual_tests"
    done
    LC_ALL=C sort -o "$actual_tests" "$actual_tests"
    cmp -s "$TEST_NAMES" "$actual_tests" ||
        fail "tracked test executable inventory differs from the expected nine targets"
}

collect_tool() {
    local name=$1
    local path=$2
    local version=$3
    local digest=

    if [[ -f "$path" ]]; then
        digest="$(sha256 "$path")"
    fi
    version="$(printf '%s' "$version" | tr '\t\r\n' '   ')"
    printf '%s\t%s\t%s\t%s\n' "$name" "$path" "$version" "$digest" >>"$TOOLS_TSV"
}

configure_zig_profile() {
    local stage=$1
    local build_dir=$2
    local build_type=$3
    local c_flags=$4
    local linker_flags=$5
    local log=$6
    local compiler="$BUILD_ROOT/bin/zig-cc"
    local flags_variable

    case "$build_type" in
    Debug)
        flags_variable=CMAKE_C_FLAGS_DEBUG
        ;;
    Release)
        flags_variable=CMAKE_C_FLAGS_RELEASE
        ;;
    *)
        fail "unsupported CMake build type: $build_type"
        ;;
    esac
    if [[ "$stage" == sanitizers ]]; then
        compiler="$BUILD_ROOT/bin/zig-asan-ubsan-cc"
    elif [[ "$stage" == tsan ]]; then
        compiler="$BUILD_ROOT/bin/zig-tsan-cc"
    fi

    run_logged "$stage" "$log" "$CMAKE" \
        -S "$LIB_DIR" \
        -B "$build_dir" \
        -G Ninja \
        "-DCMAKE_MAKE_PROGRAM=$NINJA" \
        "-DCMAKE_C_COMPILER=$compiler" \
        "-DCMAKE_BUILD_TYPE=$build_type" \
        "-D${flags_variable}=$c_flags" \
        "-DCMAKE_EXE_LINKER_FLAGS=$linker_flags" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DODT_BUILD_TESTS=ON
    run_logged "$stage" "$log" "$CMAKE" --build "$build_dir"
}

validate_ctest_inventory() {
    local stage=$1
    local build_dir=$2
    local log=$3
    local inventory="$BUILD_ROOT/${stage}-ctest-inventory.txt"
    local actual="$BUILD_ROOT/${stage}-ctest-names.txt"
    local expected="$BUILD_ROOT/${stage}-expected-ctest-names.txt"

    run_logged "$stage" "$inventory" "$CTEST" --test-dir "$build_dir" -N
    grep -Eq "Total Tests: ${EXPECTED_CTEST_COUNT}$" "$inventory" ||
        fail "$stage does not register exactly $EXPECTED_CTEST_COUNT tests"
    sed -n 's/.*Test  *# *[0-9][0-9]*: //p' "$inventory" | LC_ALL=C sort >"$actual"
    {
        cat "$TEST_NAMES"
        printf '%s\n' odt_fixture_manifest
    } | LC_ALL=C sort >"$expected"
    cmp -s "$expected" "$actual" || fail "$stage CTest names differ from the tracked test inventory"
    cat "$inventory" >>"$log"
}

prepare_curated_records() {
    local native_record="$GENERATED_EVIDENCE/u7-native.txt"
    local protocol_record="$GENERATED_EVIDENCE/u7-protocol.txt"
    local safety_record="$GENERATED_EVIDENCE/u7-safety.txt"
    local static_record="$GENERATED_EVIDENCE/u7-static-format.txt"
    local benchmark_record="$GENERATED_EVIDENCE/u8-benchmark.txt"

    {
        printf 'FIL-C correctness checks: %d/%d passed\n' \
            "$EXPECTED_FILC_TEST_COUNT" "$EXPECTED_FILC_TEST_COUNT"
        grep -E 'clang version .*Fil-C|PASS |corruption_gate|fault_gate' "$LOG_DIR/filc.log" || true
        printf '%s\n' \
            'The benchmark self-check exercises benchmark-owned exact arithmetic and checksum logic without timing.' \
            'Boundary: FIL-C validates only the executed supported paths and is not performance evidence.'
    } >"$GENERATED_EVIDENCE/u7-filc.txt"

    {
        printf 'Debug CTest: %d/%d passed\n' "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT"
        printf 'Release CTest: %d/%d passed\n' "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT"
        printf 'ASan/UBSan checks: %d/%d passed (%d CTest plus benchmark self-check)\n' \
            "$EXPECTED_SANITIZER_GATE_COUNT" "$EXPECTED_SANITIZER_GATE_COUNT" \
            "$EXPECTED_CTEST_COUNT"
        printf '%s\n' \
            'ASan instrumentation: __asan_init present in odt_fault_test' \
            'UBSan instrumentation: __ubsan_handle_type_mismatch_v1 present in odt_fault_test'
        grep -E 'benchmark_self_check|PASS odt_benchmark_self_check' \
            "$LOG_DIR/sanitizers.log"
        printf 'ASan runtime: %s sha256=%s\n' "$ASAN_RUNTIME" "$(sha256 "$ASAN_RUNTIME")"
        printf '%s\n' \
            'Link adapter: Zig 0.16.0 compiles every object; Apple Clang 21 links sanitizer runtimes.'
        grep 'architecture_gate ' "$LOG_DIR/release-builder.log"
        printf 'Zig version: %s\n' "$("$ZIG" version)"
        printf 'Zig archive SHA-256: %s\n' "$(sha256 "$ZIG_ARCHIVE")"
        printf 'Zig executable SHA-256: %s\n' "$(sha256 "$ZIG")"
    } >"$native_record"

    {
        printf 'clang-tidy translation units: %d/%d passed\n' \
            "$EXPECTED_STATIC_SOURCE_COUNT" "$EXPECTED_STATIC_SOURCE_COUNT"
        printf 'clang-format files: %d/%d passed\n' \
            "$EXPECTED_FORMAT_COUNT" "$EXPECTED_FORMAT_COUNT"
        "$CLANG_TIDY" --version | sed -n '1,2p'
        "$CLANG_FORMAT" --version
    } >"$static_record"

    {
        cat "$LOG_DIR/generate-dataset.log"
        grep -E 'Ran [0-9]+ tests|^OK$' "$LOG_DIR/python-tests.log"
        cat "$LOG_DIR/oracle.log"
        printf '%s\n' 'Human-mode anchors:'
        grep -E '^(Oblique Decision Tree example|generated inputs:|tree:|result totals:|mismatches:|representative rows:|artifacts:)' \
            "$LOG_DIR/example-human.log"
    } >"$protocol_record"

    {
        printf 'Focused safety CTest: %d/%d passed\n' \
            "$EXPECTED_FOCUSED_TEST_COUNT" "$EXPECTED_FOCUSED_TEST_COUNT"
        grep -E 'corruption_gate|fault_gate|concurrency_gate|100% tests passed' \
            "$LOG_DIR/focused-safety.log"
    } >"$safety_record"

    {
        printf 'Clean install files: %d/%d\n' \
            "$EXPECTED_INSTALL_FILE_COUNT" "$EXPECTED_INSTALL_FILE_COUNT"
        cat "$BUILD_ROOT/install-files.txt"
        printf 'Installed consumers: %d/%d passed\n' \
            "$EXPECTED_CONSUMER_COUNT" "$EXPECTED_CONSUMER_COUNT"
        printf '%s\n' 'C++ frontend flags: -std=c++17 -nostdlib++'
        printf '%s\n' 'PASS odt_consumer_c' 'PASS odt_consumer_cpp'
    } >"$GENERATED_EVIDENCE/u7-package.txt"

    {
        printf 'Controlled negative probes caught: %d/%d\n' \
            "$EXPECTED_NEGATIVE_PROBE_COUNT" "$EXPECTED_NEGATIVE_PROBE_COUNT"
        printf '%s\n' \
            'PASS ASan rejected heap-use-after-free' \
            'PASS clang-tidy rejected null dereference' \
            'PASS clang-format rejected drift' \
            'PASS package configure rejected disabled required package' \
            'PASS benchmark rejected disabled observable sink' \
            'PASS benchmark rejected injected semantic mismatch' \
            'PASS portable timeout terminated sleeping stand-in'
        grep -E -m 1 'AddressSanitizer|heap-use-after-free' "$LOG_DIR/negative.log" || true
        grep -E -m 1 'error:.*Dereference|warning:.*Dereference|null pointer' \
            "$LOG_DIR/negative.log" || true
        grep -E -m 1 'timeout_status=timed_out' "$LOG_DIR/negative.log" || true
    } >"$GENERATED_EVIDENCE/u7-negative-probes.txt"

    {
        cat "$LOG_DIR/benchmark.log"
        printf 'classification_checksum=%s\n' \
            "$("$JQ" -r '.validation.classification_checksum' "$benchmark_json")"
        printf 'scalar_median_speedup=%s\n' \
            "$("$JQ" -r '.speedup.scalar.median' "$benchmark_json")"
        printf 'scalar_ci95_lower=%s\n' \
            "$("$JQ" -r '.speedup.scalar.confidence_95_lower' "$benchmark_json")"
        printf 'batch_median_speedup=%s\n' \
            "$("$JQ" -r '.speedup.batch.median' "$benchmark_json")"
        printf 'batch_ci95_lower=%s\n' \
            "$("$JQ" -r '.speedup.batch.confidence_95_lower' "$benchmark_json")"
        printf 'steady_state_gate_basis=%s\n' \
            "$("$JQ" -r '.gate.basis' "$benchmark_json")"
        printf 'source_scalar_break_even_queries=%s\n' \
            "$("$JQ" -r '.amortization.source_scalar.break_even_queries' "$benchmark_json")"
        printf 'source_batch_break_even_queries=%s\n' \
            "$("$JQ" -r '.amortization.source_batch.break_even_queries' "$benchmark_json")"
        printf 'restored_scalar_break_even_queries=%s\n' \
            "$("$JQ" -r '.amortization.restored_scalar.break_even_queries' "$benchmark_json")"
        printf 'canonical_total_cost_speedup_scalar_batch_restored=%s/%s/%s\n' \
            "$("$JQ" -r '.amortization.source_scalar.canonical_total_cost_speedup' \
                "$benchmark_json")" \
            "$("$JQ" -r '.amortization.source_batch.canonical_total_cost_speedup' \
                "$benchmark_json")" \
            "$("$JQ" -r '.amortization.restored_scalar.canonical_total_cost_speedup' \
                "$benchmark_json")"
        printf 'qualification_scope=%s\n' "$benchmark_evidence_scope"
    } >"$benchmark_record"
    cp "$benchmark_json" "$GENERATED_EVIDENCE/u8-benchmark.json"
    cp "$BUILD_ROOT/benchmark-environment.txt" \
        "$GENERATED_EVIDENCE/u8-benchmark-environment.txt"
    cp "$BUILD_ROOT/benchmark-disassembly.txt" \
        "$GENERATED_EVIDENCE/u8-benchmark-disassembly.txt"
    tr -d '\r' <"$BUILD_ROOT/target-access.txt" \
        >"$GENERATED_EVIDENCE/u8-target-access.txt"

    cp "$COMMANDS_TSV" "$GENERATED_EVIDENCE/u7-commands.txt"
    cp "$BUILD_ROOT/tools.txt" "$GENERATED_EVIDENCE/u7-tools.txt"
    cp "$BUILD_ROOT/limitations.txt" "$GENERATED_EVIDENCE/u7-limitations.txt"

    : >"$RAW_MAP_TSV"
    local record
    for record in "$GENERATED_EVIDENCE"/u7-*.txt "$GENERATED_EVIDENCE"/u8-*; do
        printf '%s\t%s\n' "$STUDY_REL/evidence/raw/$(basename "$record")" "$record" \
            >>"$RAW_MAP_TSV"
    done
}

generate_manifest() {
    local manifest_tmp="$BUILD_ROOT/manifest.json"
    local revision

    revision="$(git -C "$REPO_ROOT" rev-parse HEAD)"
    U7_REPO_ROOT="$REPO_ROOT" \
        U7_STUDY_REL="$STUDY_REL" \
        U7_REVISION="$revision" \
        U7_STAGES_TSV="$STAGES_TSV" \
        U7_COMMANDS_TSV="$COMMANDS_TSV" \
        U7_TOOLS_TSV="$TOOLS_TSV" \
        U7_SOURCE_FILES="$SOURCE_FILES" \
        U7_DATA_FILES="$DATA_FILES" \
        U7_BINARY_FILES="$BINARY_FILES" \
        U7_RAW_MAP_TSV="$RAW_MAP_TSV" \
        U7_TEST_NAMES="$TEST_NAMES" \
        U7_MANIFEST_TMP="$manifest_tmp" \
        U7_TSAN_LIMITATION="$tsan_limitation" \
        U8_TARGET_LIMITATION="$target_limitation" \
        U8_BENCHMARK_BINDING_LIMITATION="$benchmark_binding_limitation" \
        U8_BENCHMARK_JSON="$benchmark_json" \
        "$PYTHON" - <<'PY'
import hashlib
import json
import os
import platform
from datetime import datetime, timezone
from pathlib import Path

repo = Path(os.environ["U7_REPO_ROOT"])


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def rows(path: str) -> list[list[str]]:
    return [
        line.rstrip("\n").split("\t")
        for line in Path(path).read_text(encoding="utf-8").splitlines()
        if line
    ]


commands_by_stage: dict[str, list[str]] = {}
all_commands: list[dict[str, str]] = []
for stage, command in rows(os.environ["U7_COMMANDS_TSV"]):
    commands_by_stage.setdefault(stage, []).append(command)
    all_commands.append({"stage": stage, "command": command})

stages = []
for (
    stage_id,
    status,
    required,
    expected,
    observed,
    tool,
    record,
    limitation,
) in rows(os.environ["U7_STAGES_TSV"]):
    stages.append(
        {
            "id": stage_id,
            "status": status,
            "required": required == "true",
            "expected_count": int(expected),
            "observed_count": int(observed),
            "tool": tool,
            "commands": commands_by_stage.get(stage_id, []),
            "evidence_record": record or None,
            "limitation": limitation or None,
        }
    )

tools = []
for name, path, version, sha256 in rows(os.environ["U7_TOOLS_TSV"]):
    tools.append(
        {
            "name": name,
            "path": path,
            "version": version,
            "sha256": sha256 or None,
        }
    )


def tracked_digests(list_path: str) -> list[dict[str, str]]:
    result = []
    for relative in Path(list_path).read_text(encoding="utf-8").splitlines():
        path = repo / relative
        result.append({"path": relative, "sha256": digest(path)})
    return result


binary_digests = []
for relative in Path(os.environ["U7_BINARY_FILES"]).read_text(encoding="utf-8").splitlines():
    path = repo / relative
    binary_digests.append({"path": relative, "sha256": digest(path)})

evidence_digests = []
for relative, temporary in rows(os.environ["U7_RAW_MAP_TSV"]):
    evidence_digests.append({"path": relative, "sha256": digest(Path(temporary))})

manifest = {
    "schema": "odt-qualification-manifest/v1",
    "unit": "U7-U8",
    "base_revision": os.environ["U7_REVISION"],
    "source_state": "content-addressed study snapshot at or after base revision",
    "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
    "scope": os.environ["U7_STUDY_REL"],
    "environment": {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    },
    "tools": tools,
    "commands": all_commands,
    "stages": stages,
    "tracked_test_executables": Path(os.environ["U7_TEST_NAMES"])
    .read_text(encoding="utf-8")
    .splitlines(),
    "digests": {
        "sources": tracked_digests(os.environ["U7_SOURCE_FILES"]),
        "data": tracked_digests(os.environ["U7_DATA_FILES"]),
        "binaries": binary_digests,
        "evidence_records": evidence_digests,
    },
    "benchmark": json.loads(
        Path(os.environ["U8_BENCHMARK_JSON"]).read_text(encoding="ascii")
    ),
    "limitations": [
        "FIL-C validates only executed supported paths; it is not a complete proof or a performance baseline.",
        os.environ["U7_TSAN_LIMITATION"],
        "Sanitizer objects are compiled by Zig 0.16.0; Apple Clang 21 is used only as the sanitizer runtime linker, with a version-symbol adapter that calls Apple's own runtime check.",
        "LeakSanitizer is unavailable in the Apple AddressSanitizer runtime on this platform; allocator ownership and leak behavior remain covered by FIL-C and explicit allocation counters.",
        "clang-tidy covers the eight production C translation units and the benchmark with clang-analyzer checks.",
        "The C++17 consumer uses no C++ standard-library symbols and links with -nostdlib++ because fixed Zig 0.16.0 cannot build its bundled libc++ against the installed macOS SDK.",
        os.environ["U8_TARGET_LIMITATION"],
        os.environ["U8_BENCHMARK_BINDING_LIMITATION"],
        "The recorded benchmark is local native evidence and is not target-qualifying evidence.",
        "The manifest excludes its own digest to avoid a recursive identity.",
        "The pre-existing projects/clipvault worktree change is outside this run and excluded.",
    ],
}

required_failures = [
    stage["id"]
    for stage in stages
    if stage["required"] and stage["status"] != "passed"
]
count_failures = [
    stage["id"]
    for stage in stages
    if stage["required"] and stage["expected_count"] != stage["observed_count"]
]
if required_failures or count_failures:
    raise SystemExit(
        f"manifest stage validation failed: status={required_failures} counts={count_failures}"
    )
if len(manifest["tracked_test_executables"]) != 9:
    raise SystemExit("manifest must represent exactly nine tracked test executables")

Path(os.environ["U7_MANIFEST_TMP"]).write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    encoding="ascii",
)
PY
}

verify_promoted_manifest() {
    U7_REPO_ROOT="$REPO_ROOT" U7_MANIFEST="$EVIDENCE_DIR/manifest.json" "$PYTHON" - <<'PY'
import hashlib
import json
import os
from pathlib import Path

repo = Path(os.environ["U7_REPO_ROOT"])
manifest = json.loads(Path(os.environ["U7_MANIFEST"]).read_text(encoding="ascii"))


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


checked = 0
for group in ("sources", "data", "binaries", "evidence_records"):
    for entry in manifest["digests"][group]:
        path = repo / entry["path"]
        if digest(path) != entry["sha256"]:
            raise SystemExit(f"digest mismatch: {entry['path']}")
        checked += 1

required = [stage for stage in manifest["stages"] if stage["required"]]
if any(
    stage["status"] != "passed"
    or stage["observed_count"] != stage["expected_count"]
    for stage in required
):
    raise SystemExit("required stage result mismatch")
print(
    f"manifest_gate stages={len(manifest['stages'])} required={len(required)} "
    f"digests={checked} tracked_tests={len(manifest['tracked_test_executables'])}"
)
PY
}

require_executable shasum "$SHASUM"
require_executable filcc "$FILCC"
require_executable filrun "$FILRUN"

[[ "$(git -C "$REPO_ROOT" branch --show-current)" == master ]] ||
    fail "verification must run on master"
[[ -d "$STUDY_DIR" && "$SCRIPT_DIR" == "$STUDY_DIR" ]] ||
    fail "verify.sh is not running from the expected study"

rm -rf "$BUILD_ROOT"
mkdir -p "$LOG_DIR" "$GENERATED_EVIDENCE" "$BUILD_ROOT/bin"
: >"$STAGES_TSV"
: >"$COMMANDS_TSV"
: >"$TOOLS_TSV"
: >"$BINARY_FILES"
cd "$BUILD_ROOT"

assert_no_unexpected_untracked
collect_tracked_inputs
expect_sha256 "$FILC_ARCHIVE" "$FILC_ARCHIVE_SHA256"

readonly LIB_SOURCES=(
    "$LIB_DIR/src/odt.c"
    "$LIB_DIR/src/odt_builder.c"
    "$LIB_DIR/src/odt_crc32c.c"
    "$LIB_DIR/src/odt_exact.c"
    "$LIB_DIR/src/odt_file.c"
    "$LIB_DIR/src/odt_geometry.c"
    "$LIB_DIR/src/odt_persistence.c"
    "$LIB_DIR/src/odt_query.c"
)
readonly FILC_TESTS=(
    odt_api_test
    odt_geometry_test
    odt_builder_test
    odt_query_test
    odt_batch_test
    odt_persistence_test
    odt_corruption_test
    odt_fault_test
)

current_stage=filc
printf '== FIL-C core correctness ==\n'
run_logged filc "$LOG_DIR/filc.log" "$FILCC" --version
filc_observed=0
for test_name in "${FILC_TESTS[@]}"; do
    output="$BUILD_ROOT/filc-$test_name"
    args=(
        "$FILCC"
        -std=c11
        -O1
        -g
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wsign-conversion
        -ffp-contract=off
        -DODT_TEST_SKIP_ARCHITECTURE_GATE
        "-DODT_TEST_FIXTURE_DIR=\"$LIB_DIR/tests/fixtures\""
        -I "$LIB_DIR/include"
        -I "$LIB_DIR/src"
        -I "$LIB_DIR/tests"
        "${LIB_SOURCES[@]}"
        "$LIB_DIR/tests/$test_name.c"
        "$LIB_DIR/tests/odt_test_support.c"
    )
    if [[ "$test_name" == odt_persistence_test ||
          "$test_name" == odt_corruption_test ||
          "$test_name" == odt_fault_test ]]; then
        args+=("$LIB_DIR/tests/odt_persistence_test_support.c")
    fi
    args+=(-lm -o "$output")
    run_logged filc "$LOG_DIR/filc.log" "${args[@]}"
    run_logged filc "$LOG_DIR/filc.log" "$FILRUN" "$output"
    printf 'PASS %s\n' "$test_name" >>"$LOG_DIR/filc.log"
    append_binary "$output"
    filc_observed=$((filc_observed + 1))
done
filc_benchmark_self_check="$BUILD_ROOT/filc-odt-benchmark-self-check"
run_logged filc "$LOG_DIR/filc.log" \
    "$FILCC" \
    -std=c11 \
    -O1 \
    -g \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -Wconversion \
    -Wsign-conversion \
    -ffp-contract=off \
    -I "$LIB_DIR/include" \
    -I "$LIB_DIR/src" \
    "${LIB_SOURCES[@]}" \
    "$EXAMPLE_DIR/src/benchmark.c" \
    -lm \
    -o "$filc_benchmark_self_check"
run_logged filc "$LOG_DIR/filc.log" \
    "$FILRUN" "$filc_benchmark_self_check" --self-check
grep -q 'benchmark_self_check exact_arithmetic=passed' "$LOG_DIR/filc.log" ||
    fail "FIL-C benchmark self-check did not report success"
printf 'PASS odt_benchmark_self_check\n' >>"$LOG_DIR/filc.log"
append_binary "$filc_benchmark_self_check"
filc_observed=$((filc_observed + 1))
[[ "$filc_observed" == "$EXPECTED_FILC_TEST_COUNT" ]] ||
    fail "FIL-C test count mismatch"
record_stage filc passed true "$EXPECTED_FILC_TEST_COUNT" "$filc_observed" \
    "FIL-C 0.684" "$STUDY_REL/evidence/raw/u7-filc.txt" \
    "Executed supported non-pthread paths only; not performance evidence."

current_stage=environment
printf '== fixed toolchain ==\n'
require_executable zig "$ZIG"
require_executable cmake "$CMAKE"
require_executable ctest "$CTEST"
require_executable ninja "$NINJA"
require_executable pyenv "$PYENV"
require_executable uv "$UV"
require_executable clang-format "$CLANG_FORMAT"
require_executable clang-tidy "$CLANG_TIDY"
require_executable sanitizer-linker "$SANITIZER_LINKER"
require_executable jq "$JQ"
require_executable native-cc "$NATIVE_CC"
[[ -f "$ASAN_RUNTIME" ]] || fail "missing AddressSanitizer runtime: $ASAN_RUNTIME"
expect_sha256 "$ZIG_ARCHIVE" "$ZIG_ARCHIVE_SHA256"
[[ "$("$ZIG" version)" == 0.16.0 ]] || fail "Zig must be exactly 0.16.0"
python_prefix="$(PYENV_VERSION=3.13.12 "$PYENV" prefix)"
PYTHON="$python_prefix/bin/python3"
require_executable python-3.13.12 "$PYTHON"
[[ "$("$PYTHON" -c 'import platform; print(platform.python_version())')" == 3.13.12 ]] ||
    fail "Python must be exactly 3.13.12"
write_wrapper "$BUILD_ROOT/bin/zig-cc" cc
write_wrapper "$BUILD_ROOT/bin/zig-cxx" c++
printf '%s\n' \
    'extern void __asan_version_mismatch_check_apple_clang_2100(void);' \
    'void __asan_version_mismatch_check_v8(void) {' \
    '    __asan_version_mismatch_check_apple_clang_2100();' \
    '}' >"$BUILD_ROOT/asan-version-shim.c"
record_command environment "$ZIG" cc -std=c11 -O2 -c \
    -mmacosx-version-min=26.0 "$BUILD_ROOT/asan-version-shim.c" \
    -o "$BUILD_ROOT/asan-version-shim.o"
"$ZIG" cc -std=c11 -O2 -c -mmacosx-version-min=26.0 "$BUILD_ROOT/asan-version-shim.c" \
    -o "$BUILD_ROOT/asan-version-shim.o"
write_sanitizer_wrappers

filc_version="$("$FILCC" --version 2>&1 | sed -n '1p')"
zig_cc_version="$(cd "$BUILD_ROOT" && "$ZIG" cc --version 2>&1 | sed -n '1p')"
collect_tool filcc "$FILCC" "$filc_version"
collect_tool filrun "$FILRUN" "repository FIL-C bridge for $filc_version"
collect_tool filc-archive "$FILC_ARCHIVE" "FIL-C 0.684 Linux AArch64 distribution"
collect_tool zig "$ZIG" "Zig $("$ZIG" version); $zig_cc_version"
collect_tool zig-archive "$ZIG_ARCHIVE" "Zig 0.16.0 macOS AArch64 distribution"
collect_tool cmake "$CMAKE" "$("$CMAKE" --version | sed -n '1p')"
collect_tool ctest "$CTEST" "$("$CTEST" --version | sed -n '1p')"
collect_tool ninja "$NINJA" "$("$NINJA" --version | sed -n '1p')"
collect_tool pyenv "$PYENV" "$("$PYENV" --version | sed -n '1p')"
collect_tool python "$PYTHON" "$("$PYTHON" --version 2>&1)"
collect_tool uv "$UV" "$("$UV" --version 2>&1 | sed -n '1p')"
collect_tool clang-format "$CLANG_FORMAT" "$("$CLANG_FORMAT" --version | sed -n '1p')"
collect_tool clang-tidy "$CLANG_TIDY" "$("$CLANG_TIDY" --version | sed -n '1,2p' | tr '\n' ' ')"
collect_tool sanitizer-linker "$SANITIZER_LINKER" "$("$SANITIZER_LINKER" --version | sed -n '1p')"
collect_tool asan-runtime "$ASAN_RUNTIME" "Apple Clang 21 ASan runtime"
collect_tool jq "$JQ" "$("$JQ" --version)"
collect_tool native-cc "$NATIVE_CC" "$("$NATIVE_CC" --version 2>&1 | sed -n '1,4p' | tr '\n' ' ')"
{
    printf 'host=%s\n' "$(uname -a)"
    printf 'base_revision=%s\n' "$(git -C "$REPO_ROOT" rev-parse HEAD)"
    printf 'filc_archive_sha256=%s\n' "$(sha256 "$FILC_ARCHIVE")"
    printf 'zig_archive_sha256=%s\n' "$(sha256 "$ZIG_ARCHIVE")"
    printf 'zig_binary_sha256=%s\n' "$(sha256 "$ZIG")"
    while IFS=$'\t' read -r name path version digest; do
        printf '%s path=%s version=%s sha256=%s\n' "$name" "$path" "$version" "$digest"
    done <"$TOOLS_TSV"
} >"$BUILD_ROOT/tools.txt"
record_stage environment passed true 17 17 "pinned tool inventory" \
    "$STUDY_REL/evidence/raw/u7-tools.txt" ""

debug_build="$BUILD_ROOT/debug"
release_build="$BUILD_ROOT/release"
sanitizer_build="$BUILD_ROOT/sanitizers"

current_stage=native_debug
printf '== Zig 0.16.0 Debug ==\n'
configure_zig_profile native_debug "$debug_build" Debug "-O0 -g3" "" \
    "$LOG_DIR/native-debug.log"
validate_ctest_inventory native_debug "$debug_build" "$LOG_DIR/native-debug.log"
run_logged native_debug "$LOG_DIR/native-debug.log" \
    "$CTEST" --test-dir "$debug_build" --output-on-failure
assert_ctest_count "$LOG_DIR/native-debug.log" "$EXPECTED_CTEST_COUNT"
record_stage native_debug passed true "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT" \
    "Zig 0.16.0 Debug" "$STUDY_REL/evidence/raw/u7-native.txt" ""

current_stage=native_release
printf '== Zig 0.16.0 Release ==\n'
configure_zig_profile native_release "$release_build" Release "-O3 -DNDEBUG" "" \
    "$LOG_DIR/native-release.log"
validate_ctest_inventory native_release "$release_build" "$LOG_DIR/native-release.log"
run_logged native_release "$LOG_DIR/native-release.log" \
    "$CTEST" --test-dir "$release_build" --output-on-failure
assert_ctest_count "$LOG_DIR/native-release.log" "$EXPECTED_CTEST_COUNT"
run_logged native_release "$LOG_DIR/release-builder.log" "$release_build/odt_builder_test"
grep -q 'architecture_gate ' "$LOG_DIR/release-builder.log" ||
    fail "Release architecture gate did not emit its metrics"
record_stage native_release passed true "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT" \
    "Zig 0.16.0 Release -O3 -DNDEBUG" "$STUDY_REL/evidence/raw/u7-native.txt" ""

current_stage=sanitizers
printf '== Zig 0.16.0 ASan/UBSan ==\n'
sanitize_flags="-O1 -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -Wno-macro-redefined -mmacosx-version-min=26.0"
configure_zig_profile sanitizers "$sanitizer_build" Debug "$sanitize_flags" \
    "-fsanitize=address -fsanitize=undefined" "$LOG_DIR/sanitizers.log"
validate_ctest_inventory sanitizers "$sanitizer_build" "$LOG_DIR/sanitizers.log"
run_logged sanitizers "$LOG_DIR/sanitizers.log" env \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$CTEST" --test-dir "$sanitizer_build" --output-on-failure
assert_ctest_count "$LOG_DIR/sanitizers.log" "$EXPECTED_CTEST_COUNT"
record_command sanitizers nm "$sanitizer_build/odt_fault_test"
nm "$sanitizer_build/odt_fault_test" >"$BUILD_ROOT/sanitizer-symbols.txt"
grep -q '___asan_init' "$BUILD_ROOT/sanitizer-symbols.txt" ||
    fail "sanitizer binary lacks AddressSanitizer instrumentation"
grep -q '___ubsan_handle_type_mismatch_v1' "$BUILD_ROOT/sanitizer-symbols.txt" ||
    fail "sanitizer binary lacks UndefinedBehaviorSanitizer instrumentation"
record_command sanitizers otool -L "$sanitizer_build/odt_fault_test"
otool -L "$sanitizer_build/odt_fault_test" >"$BUILD_ROOT/sanitizer-libraries.txt"
grep -q 'libclang_rt.asan_osx_dynamic.dylib' "$BUILD_ROOT/sanitizer-libraries.txt" ||
    fail "sanitizer binary is not linked to the recorded AddressSanitizer runtime"
sanitizer_benchmark_object_dir="$BUILD_ROOT/odt-benchmark-self-check-objects"
sanitizer_benchmark="$BUILD_ROOT/odt-benchmark-self-check-sanitized"
mkdir -p "$sanitizer_benchmark_object_dir"
sanitizer_benchmark_objects=()
for source in "${LIB_SOURCES[@]}" "$EXAMPLE_DIR/src/benchmark.c"; do
    sanitizer_benchmark_object="$sanitizer_benchmark_object_dir/$(basename "$source" .c).o"
    run_logged sanitizers "$LOG_DIR/sanitizers.log" \
        "$BUILD_ROOT/bin/zig-asan-ubsan-cc" \
        -std=c11 \
        $sanitize_flags \
        -Wall \
        -Wextra \
        -Wpedantic \
        -Werror \
        -Wconversion \
        -Wsign-conversion \
        -ffp-contract=off \
        -I "$LIB_DIR/include" \
        -I "$LIB_DIR/src" \
        -c "$source" \
        -o "$sanitizer_benchmark_object"
    sanitizer_benchmark_objects+=("$sanitizer_benchmark_object")
done
run_logged sanitizers "$LOG_DIR/sanitizers.log" \
    "$BUILD_ROOT/bin/zig-asan-ubsan-cc" \
    $sanitize_flags \
    "${sanitizer_benchmark_objects[@]}" \
    -lm \
    -o "$sanitizer_benchmark"
run_logged sanitizers "$LOG_DIR/sanitizers.log" env \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$sanitizer_benchmark" --self-check
grep -q 'benchmark_self_check exact_arithmetic=passed' "$LOG_DIR/sanitizers.log" ||
    fail "ASan/UBSan benchmark self-check did not report success"
printf 'PASS odt_benchmark_self_check\n' >>"$LOG_DIR/sanitizers.log"
append_binary "$sanitizer_benchmark"
record_stage sanitizers passed true \
    "$EXPECTED_SANITIZER_GATE_COUNT" "$EXPECTED_SANITIZER_GATE_COUNT" \
    "Zig 0.16.0 ASan/UBSan" "$STUDY_REL/evidence/raw/u7-native.txt" ""

for profile in "$debug_build" "$release_build" "$sanitizer_build"; do
    append_binary "$profile/libodt.a"
    while IFS= read -r test_name; do
        append_binary "$profile/$test_name"
    done <"$TEST_NAMES"
done

readonly STATIC_SOURCES=(
    "${LIB_SOURCES[@]}"
    "$EXAMPLE_DIR/src/benchmark.c"
)

current_stage=static_analysis
printf '== static analysis ==\n'
: >"$LOG_DIR/static-analysis.log"
static_observed=0
for source in "${STATIC_SOURCES[@]}"; do
    run_logged static_analysis "$LOG_DIR/static-analysis.log" \
        "$CLANG_TIDY" "$source" \
        -checks=clang-analyzer-\*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
        -warnings-as-errors=clang-analyzer-\* \
        -- \
        -std=c11 \
        -D_DARWIN_C_SOURCE \
        -D_POSIX_C_SOURCE=200809L \
        -I "$LIB_DIR/include" \
        -I "$LIB_DIR/src" \
        -Wall \
        -Wextra \
        -Wpedantic \
        -Werror \
        -Wconversion \
        -Wsign-conversion \
        -ffp-contract=off
    static_observed=$((static_observed + 1))
done
[[ "$static_observed" == "$EXPECTED_STATIC_SOURCE_COUNT" ]] ||
    fail "static-analysis source count mismatch"
record_stage static_analysis passed true "$EXPECTED_STATIC_SOURCE_COUNT" "$static_observed" \
    "clang-tidy clang-analyzer" "$STUDY_REL/evidence/raw/u7-static-format.txt" ""

current_stage=format
printf '== formatting ==\n'
format_list="$BUILD_ROOT/format-files.txt"
git -C "$REPO_ROOT" ls-files --cached --others --exclude-standard -- \
    "$STUDY_REL/lib" "$STUDY_REL/example" |
    awk '/\.(c|h|cpp)$/ { print }' |
    LC_ALL=C sort >"$format_list"
format_count="$(wc -l <"$format_list" | tr -d ' ')"
[[ "$format_count" == "$EXPECTED_FORMAT_COUNT" ]] ||
    fail "format file count changed; update the explicit U7 gate"
format_files=()
while IFS= read -r relative; do
    format_files+=("$REPO_ROOT/$relative")
done <"$format_list"
run_logged format "$LOG_DIR/format.log" \
    "$CLANG_FORMAT" "--style=file:$LIB_DIR/.clang-format" --dry-run --Werror \
    "${format_files[@]}"
record_stage format passed true "$EXPECTED_FORMAT_COUNT" "$format_count" \
    "clang-format" "$STUDY_REL/evidence/raw/u7-static-format.txt" ""

current_stage=protocol
printf '== Python 3.13.12 exact protocol ==\n'
protocol_root="$BUILD_ROOT/protocol"
generated_data="$protocol_root/generated-data"
protocol_outputs="$protocol_root/outputs"
python_venv="$protocol_root/venv"
example_binary="$protocol_root/odt_example"
mkdir -p "$generated_data" "$protocol_outputs"
run_logged protocol "$LOG_DIR/example-build.log" \
    "$BUILD_ROOT/bin/zig-cc" \
    -std=c11 \
    -O2 \
    -g \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -Wconversion \
    -Wsign-conversion \
    -ffp-contract=off \
    -I "$LIB_DIR/include" \
    "$EXAMPLE_DIR/src/odt_example.c" \
    "$release_build/libodt.a" \
    -lm \
    -o "$example_binary"
append_binary "$example_binary"
run_in_dir_logged protocol "$LOG_DIR/python-sync.log" "$EXAMPLE_DIR/python" \
    env "UV_PROJECT_ENVIRONMENT=$python_venv" \
    "$UV" sync --frozen --python "$PYTHON"
run_logged protocol "$LOG_DIR/generate-dataset.log" \
    "$PYTHON" "$EXAMPLE_DIR/python/generate_dataset.py" --output "$generated_data"
cmp -s "$generated_data/sites.csv" "$EXAMPLE_DIR/data/sites.csv" ||
    fail "regenerated sites.csv differs from the tracked canonical data"
cmp -s "$generated_data/queries.csv" "$EXAMPLE_DIR/data/queries.csv" ||
    fail "regenerated queries.csv differs from the tracked canonical data"
run_logged protocol "$LOG_DIR/example-machine.log" \
    "$example_binary" \
    --mode machine \
    --sites "$EXAMPLE_DIR/data/sites.csv" \
    --queries "$EXAMPLE_DIR/data/queries.csv" \
    --results "$protocol_outputs/results.jsonl" \
    --summary "$protocol_outputs/summary.json" \
    --snapshot "$protocol_outputs/generation.odt"
run_logged protocol "$LOG_DIR/oracle.log" \
    "$PYTHON" "$EXAMPLE_DIR/python/point_location_oracle.py" \
    --sites "$EXAMPLE_DIR/data/sites.csv" \
    --queries "$EXAMPLE_DIR/data/queries.csv" \
    --results "$protocol_outputs/results.jsonl" \
    --summary "$protocol_outputs/summary.json"
grep -q 'mismatches=0' "$LOG_DIR/oracle.log" ||
    fail "exact oracle did not report zero mismatches"
[[ "$("$JQ" -r '.inputs.query_count' "$protocol_outputs/summary.json")" == 3102 ]] ||
    fail "protocol query count changed"
[[ "$("$JQ" -r '.mismatches.total' "$protocol_outputs/summary.json")" == 0 ]] ||
    fail "C protocol summary reports mismatches"
run_logged protocol "$LOG_DIR/example-human.log" \
    "$example_binary" \
    --mode human \
    --sites "$EXAMPLE_DIR/data/sites.csv" \
    --queries "$EXAMPLE_DIR/data/queries.csv" \
    --results "$protocol_outputs/human-results.jsonl" \
    --summary "$protocol_outputs/human-summary.json" \
    --snapshot "$protocol_outputs/human-generation.odt"
run_in_dir_logged protocol "$LOG_DIR/python-tests.log" "$EXAMPLE_DIR/python" \
    env \
    "UV_PROJECT_ENVIRONMENT=$python_venv" \
    "ODT_EXAMPLE_BIN=$example_binary" \
    "PYTHON=$PYTHON" \
    "$UV" run --frozen --python "$PYTHON" \
    python -m unittest discover -s tests -v
grep -Eq "Ran ${EXPECTED_PYTHON_TEST_COUNT} tests" "$LOG_DIR/python-tests.log" ||
    fail "Python protocol test count mismatch"
record_stage protocol passed true "$EXPECTED_PYTHON_TEST_COUNT" \
    "$EXPECTED_PYTHON_TEST_COUNT" "CPython 3.13.12 Fraction oracle and uv" \
    "$STUDY_REL/evidence/raw/u7-protocol.txt" ""

current_stage=focused_safety
printf '== corruption, fault, and concurrency qualification ==\n'
run_logged focused_safety "$LOG_DIR/focused-safety.log" \
    "$CTEST" --test-dir "$debug_build" -V \
    -R '^odt_(geometry|builder|corruption|fault|concurrency)_test$'
assert_ctest_count "$LOG_DIR/focused-safety.log" "$EXPECTED_FOCUSED_TEST_COUNT"
grep -q 'corruption_gate ' "$LOG_DIR/focused-safety.log" ||
    fail "corruption test omitted its evidence summary"
grep -q 'fault_gate ' "$LOG_DIR/focused-safety.log" ||
    fail "fault test omitted its evidence summary"
grep -q 'concurrency_gate ' "$LOG_DIR/focused-safety.log" ||
    fail "concurrency test omitted its evidence summary"
record_stage focused_safety passed true "$EXPECTED_FOCUSED_TEST_COUNT" \
    "$EXPECTED_FOCUSED_TEST_COUNT" "Zig Debug focused CTest" \
    "$STUDY_REL/evidence/raw/u7-safety.txt" ""

current_stage=tsan
printf '== ThreadSanitizer capability ==\n'
tsan_probe_source="$BUILD_ROOT/tsan-probe.c"
tsan_probe_binary="$BUILD_ROOT/tsan-probe"
printf '%s\n' \
    '#include <pthread.h>' \
    'static void *worker(void *p) { (void)p; return 0; }' \
    'int main(void) { pthread_t t; return pthread_create(&t, 0, worker, 0) || pthread_join(t, 0); }' \
    >"$tsan_probe_source"
record_command tsan "$BUILD_ROOT/bin/zig-tsan-cc" -std=c11 -g -O1 \
    -fsanitize=thread -mmacosx-version-min=26.0 \
    "$tsan_probe_source" -pthread -o "$tsan_probe_binary"
tsan_limitation=
if "$BUILD_ROOT/bin/zig-tsan-cc" -std=c11 -g -O1 -fsanitize=thread \
    -mmacosx-version-min=26.0 \
    "$tsan_probe_source" -pthread -o "$tsan_probe_binary" \
    >"$LOG_DIR/tsan-probe.log" 2>&1; then
    record_command tsan env TSAN_OPTIONS=halt_on_error=1 "$tsan_probe_binary"
    if env TSAN_OPTIONS=halt_on_error=1 "$tsan_probe_binary" >>"$LOG_DIR/tsan-probe.log" 2>&1; then
        tsan_build="$BUILD_ROOT/tsan"
        tsan_flags="-O1 -g -fsanitize=thread -fno-omit-frame-pointer -mmacosx-version-min=26.0"
        configure_zig_profile tsan "$tsan_build" Debug "$tsan_flags" \
            "-fsanitize=thread" "$LOG_DIR/tsan.log"
        run_logged tsan "$LOG_DIR/tsan.log" env TSAN_OPTIONS=halt_on_error=1 \
            "$CTEST" --test-dir "$tsan_build" --output-on-failure \
            -R '^odt_concurrency_test$'
        assert_ctest_count "$LOG_DIR/tsan.log" 1
        record_command tsan nm "$tsan_build/odt_concurrency_test"
        nm "$tsan_build/odt_concurrency_test" >"$BUILD_ROOT/tsan-symbols.txt"
        grep -q '___tsan_init' "$BUILD_ROOT/tsan-symbols.txt" ||
            fail "concurrency binary lacks ThreadSanitizer instrumentation"
        record_command tsan otool -L "$tsan_build/odt_concurrency_test"
        otool -L "$tsan_build/odt_concurrency_test" >"$BUILD_ROOT/tsan-libraries.txt"
        grep -q 'libclang_rt.tsan_osx_dynamic.dylib' "$BUILD_ROOT/tsan-libraries.txt" ||
            fail "concurrency binary is not linked to the ThreadSanitizer runtime"
        append_binary "$tsan_build/odt_concurrency_test"
        tsan_limitation="ThreadSanitizer instrumentation and runtime linkage were verified; Zig 0.16.0 compiled the objects, Apple Clang 21 linked the runtime, and the concurrency test passed."
        record_stage tsan passed true 1 1 "Zig 0.16.0 ThreadSanitizer" \
            "$STUDY_REL/evidence/raw/u7-limitations.txt" ""
    else
        tsan_reason="$(grep -E -m 1 'use of undeclared identifier|FATAL|ERROR|unsupported|not supported' \
            "$LOG_DIR/tsan-probe.log" || sed -n '1p' "$LOG_DIR/tsan-probe.log")"
        tsan_limitation="ThreadSanitizer skipped: fixed Zig compiled the probe, but its runtime capability probe failed: $tsan_reason"
        record_stage tsan degraded false 1 0 "Zig 0.16.0 ThreadSanitizer" \
            "$STUDY_REL/evidence/raw/u7-limitations.txt" "$tsan_limitation"
    fi
else
    tsan_reason="$(grep -E -m 1 'use of undeclared identifier|error:|unsupported|not supported' \
        "$LOG_DIR/tsan-probe.log" || sed -n '1p' "$LOG_DIR/tsan-probe.log")"
    tsan_limitation="ThreadSanitizer skipped: fixed Zig could not build its runtime on this macOS SDK: $tsan_reason"
    record_stage tsan degraded false 1 0 "Zig 0.16.0 ThreadSanitizer" \
        "$STUDY_REL/evidence/raw/u7-limitations.txt" "$tsan_limitation"
fi

current_stage=package
printf '== clean install and C/C++ consumers ==\n'
install_prefix="$BUILD_ROOT/install"
consumer_build="$BUILD_ROOT/package-consumer"
rm -rf "$install_prefix" "$consumer_build"
run_logged package "$LOG_DIR/package.log" \
    "$CMAKE" --install "$release_build" --prefix "$install_prefix"
(
    cd "$install_prefix"
    find . -type f -print | sed 's|^\./||' | LC_ALL=C sort
) >"$BUILD_ROOT/install-files.txt"
printf '%s\n' \
    include/odt.h \
    lib/cmake/odt/odtConfig.cmake \
    lib/cmake/odt/odtConfigVersion.cmake \
    lib/cmake/odt/odtTargets-release.cmake \
    lib/cmake/odt/odtTargets.cmake \
    lib/libodt.a >"$BUILD_ROOT/expected-install-files.txt"
cmp -s "$BUILD_ROOT/expected-install-files.txt" "$BUILD_ROOT/install-files.txt" ||
    fail "clean install prefix contains missing or undeclared files"
[[ "$(wc -l <"$BUILD_ROOT/install-files.txt" | tr -d ' ')" == \
    "$EXPECTED_INSTALL_FILE_COUNT" ]] || fail "clean install file count mismatch"
run_logged package "$LOG_DIR/package.log" \
    "$CMAKE" \
    -S "$LIB_DIR/tests/package_consumer" \
    -B "$consumer_build" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_C_COMPILER=$BUILD_ROOT/bin/zig-cc" \
    "-DCMAKE_CXX_COMPILER=$BUILD_ROOT/bin/zig-cxx" \
    -DCMAKE_CXX_FLAGS=-nostdlib++ \
    "-DCMAKE_PREFIX_PATH=$install_prefix" \
    -DCMAKE_BUILD_TYPE=Release
run_logged package "$LOG_DIR/package.log" "$CMAKE" --build "$consumer_build"
run_logged package "$LOG_DIR/package.log" "$consumer_build/odt_consumer_c"
printf '%s\n' 'PASS odt_consumer_c' >>"$LOG_DIR/package.log"
run_logged package "$LOG_DIR/package.log" "$consumer_build/odt_consumer_cpp"
printf '%s\n' 'PASS odt_consumer_cpp' >>"$LOG_DIR/package.log"
append_binary "$install_prefix/lib/libodt.a"
append_binary "$consumer_build/odt_consumer_c"
append_binary "$consumer_build/odt_consumer_cpp"
record_stage package passed true \
    "$((EXPECTED_INSTALL_FILE_COUNT + EXPECTED_CONSUMER_COUNT))" \
    "$((EXPECTED_INSTALL_FILE_COUNT + EXPECTED_CONSUMER_COUNT))" \
    "installed odt CMake package with Zig C/C++ consumers" \
    "$STUDY_REL/evidence/raw/u7-package.txt" ""

current_stage=target_benchmark
printf '== named target access ==\n'
target_access_log="$BUILD_ROOT/target-access.txt"
: >"$target_access_log"
target_demand_status=127
target_ssh_status=127
target_demand_timeout=not_run
target_ssh_timeout=not_run
if [[ -n "$ORTHRUS_CLI" && -x "$ORTHRUS_CLI" ]]; then
    if run_with_timeout target_benchmark "$target_access_log" \
        "$TARGET_COMMAND_TIMEOUT_SECONDS" "$ORTHRUS_CLI" demand "$BENCHMARK_TARGET"; then
        target_demand_status=0
        target_demand_timeout=false
    else
        target_demand_status=$?
        if [[ "$target_demand_status" == 124 ]]; then
            target_demand_timeout=true
        else
            target_demand_timeout=false
        fi
    fi
else
    printf 'orthrus-cli unavailable\n' >>"$target_access_log"
fi
if [[ -n "$SSH" && -x "$SSH" ]]; then
    if run_with_timeout target_benchmark "$target_access_log" \
        "$TARGET_COMMAND_TIMEOUT_SECONDS" \
        "$SSH" -o BatchMode=yes -o ConnectTimeout=8 "$BENCHMARK_TARGET" -J j \
        'hostname; uname -a; pwd; git rev-parse --show-toplevel; git rev-parse HEAD'; then
        target_ssh_status=0
        target_ssh_timeout=false
    else
        target_ssh_status=$?
        if [[ "$target_ssh_status" == 124 ]]; then
            target_ssh_timeout=true
        else
            target_ssh_timeout=false
        fi
    fi
else
    printf 'ssh unavailable\n' >>"$target_access_log"
fi
printf 'demand_status=%d\ndemand_timeout=%s\nssh_status=%d\nssh_timeout=%s\n' \
    "$target_demand_status" "$target_demand_timeout" "$target_ssh_status" "$target_ssh_timeout" \
    >>"$target_access_log"
target_access_observed=0
if [[ "$target_demand_status" == 0 ]]; then
    target_access_observed=$((target_access_observed + 1))
fi
if [[ "$target_ssh_status" == 0 ]]; then
    target_access_observed=$((target_access_observed + 1))
fi
if [[ "$target_demand_status" == 0 && "$target_ssh_status" == 0 ]]; then
    target_limitation="Named target $BENCHMARK_TARGET was reachable, but this local qualification run does not publish uncommitted source to it; no target performance result is claimed."
    target_access_summary=reachable
else
    target_limitation="Named target $BENCHMARK_TARGET was attempted first but unavailable: orthrus-cli demand status $target_demand_status (timeout=$target_demand_timeout); jump-host SSH status $target_ssh_status (timeout=$target_ssh_timeout). Target topology, revision, frequency policy, PMU, and performance remain unobserved."
    target_access_summary=unavailable
fi
target_benchmark_summary="degraded/unavailable"
record_stage target_benchmark degraded false 2 "$target_access_observed" \
    "portable subprocess timeout, orthrus-cli, and OpenSSH" \
    "$STUDY_REL/evidence/raw/u8-target-access.txt" "$target_limitation"

current_stage=benchmark
printf '== native benchmark ==\n'
benchmark_lib_build="$BUILD_ROOT/benchmark-lib"
benchmark_install="$BUILD_ROOT/benchmark-install"
benchmark_example_build="$BUILD_ROOT/benchmark-example"
benchmark_output="$BUILD_ROOT/benchmark-output"
benchmark_json="$benchmark_output/benchmark.json"
benchmark_binary="$benchmark_example_build/odt_benchmark"
benchmark_runner=direct
benchmark_affinity="unpinned"
benchmark_numa="unbound"
benchmark_evidence_scope="local-unpinned-non-target"
mkdir -p "$benchmark_output"
run_logged benchmark "$LOG_DIR/benchmark-build.log" \
    "$CMAKE" \
    -S "$LIB_DIR" \
    -B "$benchmark_lib_build" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_C_COMPILER=$NATIVE_CC" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$BENCHMARK_FLAGS" \
    -DODT_BUILD_TESTS=OFF
run_logged benchmark "$LOG_DIR/benchmark-build.log" \
    "$CMAKE" --build "$benchmark_lib_build"
run_logged benchmark "$LOG_DIR/benchmark-build.log" \
    "$CMAKE" --install "$benchmark_lib_build" --prefix "$benchmark_install"
run_logged benchmark "$LOG_DIR/benchmark-build.log" \
    "$CMAKE" \
    -S "$EXAMPLE_DIR" \
    -B "$benchmark_example_build" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_C_COMPILER=$NATIVE_CC" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$BENCHMARK_FLAGS" \
    "-DCMAKE_PREFIX_PATH=$benchmark_install"
run_logged benchmark "$LOG_DIR/benchmark-build.log" \
    "$CMAKE" --build "$benchmark_example_build" --target odt_benchmark
run_logged benchmark "$LOG_DIR/benchmark.log" \
    "$benchmark_binary" --self-check-dce
run_logged benchmark "$LOG_DIR/benchmark.log" \
    "$benchmark_binary" --self-check

if [[ "$(uname -s)" == Linux ]]; then
    benchmark_cpu="$("$PYTHON" - <<'PY'
import os

if hasattr(os, "sched_getaffinity"):
    allowed = sorted(os.sched_getaffinity(0))
    if allowed:
        print(allowed[0])
PY
)"
    benchmark_node=
    if [[ "$benchmark_cpu" =~ ^[0-9]+$ ]]; then
        for node_path in /sys/devices/system/cpu/cpu"$benchmark_cpu"/node[0-9]*; do
            if [[ -e "$node_path" ]]; then
                benchmark_node="${node_path##*node}"
                break
            fi
        done
    fi
    if [[ "$benchmark_cpu" =~ ^[0-9]+$ && "$benchmark_node" =~ ^[0-9]+$ ]] &&
        command -v numactl >/dev/null 2>&1; then
        record_command benchmark numactl "--physcpubind=$benchmark_cpu" \
            "--membind=$benchmark_node" /bin/true
        if numactl "--physcpubind=$benchmark_cpu" "--membind=$benchmark_node" \
            /bin/true >>"$LOG_DIR/benchmark-build.log" 2>&1; then
            benchmark_runner=numactl
            benchmark_affinity="pinned-cpu-$benchmark_cpu"
            benchmark_numa="pinned-node-$benchmark_node"
            benchmark_evidence_scope="local-pinned-non-target"
        fi
    fi
    if [[ "$benchmark_runner" == direct && "$benchmark_cpu" =~ ^[0-9]+$ ]] &&
        command -v taskset >/dev/null 2>&1; then
        record_command benchmark taskset -c "$benchmark_cpu" /bin/true
        if taskset -c "$benchmark_cpu" /bin/true >>"$LOG_DIR/benchmark-build.log" 2>&1; then
            benchmark_runner=taskset
            benchmark_affinity="pinned-cpu-$benchmark_cpu"
            benchmark_numa="unbound-no-safe-node-binding"
            benchmark_evidence_scope="local-cpu-pinned-numa-unbound-non-target"
        fi
    fi
fi

benchmark_arguments=(
    "$benchmark_binary"
    --sites "$EXAMPLE_DIR/data/sites.csv"
    --queries "$EXAMPLE_DIR/data/queries.csv"
    --snapshot "$benchmark_output/generation.odt"
    --output "$benchmark_json"
    --repetitions 9
    --warmup 2
    --tree-loops 128
    --brute-loops 1
    --require-speedup
)
if [[ "$benchmark_runner" == numactl ]]; then
    run_logged benchmark "$LOG_DIR/benchmark.log" \
        numactl "--physcpubind=$benchmark_cpu" "--membind=$benchmark_node" \
        "${benchmark_arguments[@]}"
elif [[ "$benchmark_runner" == taskset ]]; then
    run_logged benchmark "$LOG_DIR/benchmark.log" \
        taskset -c "$benchmark_cpu" "${benchmark_arguments[@]}"
else
    run_logged benchmark "$LOG_DIR/benchmark.log" "${benchmark_arguments[@]}"
fi
"$JQ" -e \
    --arg checksum "$EXPECTED_BENCHMARK_CHECKSUM" \
    '.schema == "odt-benchmark/v1" and
     .inputs.site_count == 1000 and
     .inputs.query_count == 3102 and
     .configuration.repetitions == 9 and
     .validation.classification_checksum == $checksum and
     .validation.scalar_exact_fallbacks == .validation.batch_exact_fallbacks and
     .validation.scalar_exact_fallbacks == .validation.restored_exact_fallbacks and
     .amortization.canonical_query_count == 3102 and
     (.amortization.source_scalar.break_even_queries | type) == "number" and
     (.amortization.source_batch.break_even_queries | type) == "number" and
     (.amortization.restored_scalar.break_even_queries | type) == "number" and
     .amortization.source_scalar.canonical_total_cost_speedup > 0.0 and
     .amortization.source_batch.canonical_total_cost_speedup > 0.0 and
     .amortization.restored_scalar.canonical_total_cost_speedup > 0.0 and
     .gate.basis == "steady_state_query_only" and
     .gate.required == true and .gate.passed == true and
     .speedup.scalar.median >= 2.0 and .speedup.batch.median >= 2.0 and
     .speedup.scalar.confidence_95_lower > 1.0 and
     .speedup.batch.confidence_95_lower > 1.0' \
    "$benchmark_json" >>"$LOG_DIR/benchmark.log"
record_command benchmark "$JQ" "<embedded benchmark schema and threshold validation>" \
    "$benchmark_json"

benchmark_disassembly_dir="$BUILD_ROOT/benchmark-disassembly-slices"
mkdir -p "$benchmark_disassembly_dir"
if [[ "$(uname -s)" == Darwin ]]; then
    record_command benchmark otool -tvV "$benchmark_binary"
    otool -tvV "$benchmark_binary" >"$BUILD_ROOT/benchmark-disassembly-full.txt"
    for variant in scalar batch brute; do
        symbol="odt_benchmark_${variant}_pass"
        awk -v label="_${symbol}:" '
            $0 == label {
                emit = 1
            }
            emit && $0 != label && /^_[[:alnum:]_.$]+:$/ {
                exit
            }
            emit {
                print
            }
        ' "$BUILD_ROOT/benchmark-disassembly-full.txt" \
            >"$benchmark_disassembly_dir/$variant.txt"
    done
else
    require_executable objdump "$(command -v objdump || true)"
    for variant in scalar batch brute; do
        symbol="odt_benchmark_${variant}_pass"
        record_command benchmark objdump -d "--disassemble=$symbol" "$benchmark_binary"
        objdump -d "--disassemble=$symbol" "$benchmark_binary" \
            >"$benchmark_disassembly_dir/$variant.txt"
    done
fi
{
    for variant in scalar batch brute; do
        printf '===== %s =====\n' "$variant"
        cat "$benchmark_disassembly_dir/$variant.txt"
    done
} >"$BUILD_ROOT/benchmark-disassembly.txt"
for variant in scalar batch brute; do
    grep -Eq '^[[:space:]]*[[:xdigit:]]+:?[[:space:]]' \
        "$benchmark_disassembly_dir/$variant.txt" ||
        fail "final benchmark binary has an empty $variant measurement body"
done
grep -Eq '[[:space:]](bl|callq?)[[:space:]].*(_odt_query|<odt_query>)([>@+[:space:]]|$)' \
    "$benchmark_disassembly_dir/scalar.txt" ||
    fail "final scalar benchmark body lacks a call to odt_query"
grep -Eq '[[:space:]](bl|callq?)[[:space:]].*(_odt_query_batch|<odt_query_batch>)([>@+[:space:]]|$)' \
    "$benchmark_disassembly_dir/batch.txt" ||
    fail "final batch benchmark body lacks a call to odt_query_batch"

case "$(uname -s)" in
Darwin)
    benchmark_binding_limitation="Local macOS benchmark evidence is explicitly unpinned: no supported CPU affinity or NUMA binding interface was available. Scope is local and non-target-qualifying."
    benchmark_frequency_record="No fixed-frequency control is exposed by the local macOS host."
    benchmark_pmu_record="perf is unavailable on the local macOS host; no hardware counters were collected."
    ;;
Linux)
    if [[ "$benchmark_evidence_scope" == local-pinned-non-target ]]; then
        benchmark_binding_limitation="Local Linux benchmark used the recorded CPU and NUMA binding ($benchmark_affinity, $benchmark_numa). Scope remains local and non-target-qualifying."
    else
        benchmark_binding_limitation="Local Linux benchmark could not safely establish full CPU/NUMA binding ($benchmark_affinity, $benchmark_numa). Scope is local and non-target-qualifying."
    fi
    benchmark_frequency_record="Frequency policy was observed from the host where available but was not changed."
    benchmark_pmu_record="No PMU counters were collected; the local result uses repeated monotonic wall time."
    ;;
*)
    benchmark_binding_limitation="Local benchmark could not safely establish CPU/NUMA binding on this operating system. Scope is local and non-target-qualifying."
    benchmark_frequency_record="Frequency policy was not controlled."
    benchmark_pmu_record="No PMU counters were collected; the local result uses repeated monotonic wall time."
    ;;
esac
{
    printf 'host=%s\n' "$(uname -a)"
    printf 'compiler=%s\n' \
        "$("$NATIVE_CC" --version 2>&1 | awk 'NR <= 4 { if (NR > 1) printf " "; printf "%s", $0 }')"
    printf 'flags=%s\n' "$BENCHMARK_FLAGS"
    printf 'qualification_scope=%s\n' "$benchmark_evidence_scope"
    printf 'affinity=%s\n' "$benchmark_affinity"
    printf 'numa=%s\n' "$benchmark_numa"
    printf 'source_revision=%s\n' "$(git -C "$REPO_ROOT" rev-parse HEAD)"
    printf 'benchmark_source_sha256=%s\n' "$(sha256 "$EXAMPLE_DIR/src/benchmark.c")"
    printf 'library_binary_sha256=%s\n' "$(sha256 "$benchmark_install/lib/libodt.a")"
    printf 'benchmark_binary_sha256=%s\n' "$(sha256 "$benchmark_binary")"
    printf 'sites_sha256=%s\n' "$(sha256 "$EXAMPLE_DIR/data/sites.csv")"
    printf 'queries_sha256=%s\n' "$(sha256 "$EXAMPLE_DIR/data/queries.csv")"
    if command -v sysctl >/dev/null 2>&1; then
        printf 'cpu=%s\n' "$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
        printf 'logical_cpu=%s\n' "$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
        printf 'physical_cpu=%s\n' "$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
    elif command -v lscpu >/dev/null 2>&1; then
        lscpu
    fi
    if command -v pmset >/dev/null 2>&1; then
        pmset -g therm 2>/dev/null || true
    fi
    printf 'frequency=%s\n' "$benchmark_frequency_record"
    printf 'pmu=%s\n' "$benchmark_pmu_record"
} >"$BUILD_ROOT/benchmark-environment.txt"
append_binary "$benchmark_install/lib/libodt.a"
append_binary "$benchmark_binary"
record_stage benchmark passed true 10 10 \
    "native compiler, monotonic wall time, checked checksums, per-function disassembly" \
    "$STUDY_REL/evidence/raw/u8-benchmark.json" \
    "$benchmark_binding_limitation Named target evidence is separately bounded by the target_benchmark stage."

current_stage=negative_probes
printf '== controlled negative probes ==\n'
negative_dir="$BUILD_ROOT/negative"
mkdir -p "$negative_dir"
printf '%s\n' \
    '#include <stdlib.h>' \
    '__attribute__((noinline)) static int read_byte(volatile char *p) { return p[0]; }' \
    'int main(void) { volatile char *p = malloc(1); if (!p) return 2; p[0] = 1; free((void *)p); return read_byte(p); }' \
    >"$negative_dir/asan.c"
run_logged negative_probes "$LOG_DIR/negative.log" \
    "$BUILD_ROOT/bin/zig-asan-ubsan-cc" -std=c11 -O1 -g \
    -fsanitize=address -fsanitize=undefined \
    -fno-omit-frame-pointer -Wno-macro-redefined -mmacosx-version-min=26.0 \
    "$negative_dir/asan.c" -o "$negative_dir/asan"
expect_failure negative_probes "$LOG_DIR/negative.log" env \
    ASAN_OPTIONS=halt_on_error=1 "$negative_dir/asan"
grep -q 'ERROR: AddressSanitizer: heap-use-after-free' "$LOG_DIR/negative.log" ||
    fail "AddressSanitizer negative probe failed without detecting heap-use-after-free"

printf '%s\n' 'int main(void) { int *p = 0; return *p; }' >"$negative_dir/static.c"
expect_failure negative_probes "$LOG_DIR/negative.log" \
    "$CLANG_TIDY" "$negative_dir/static.c" \
    -checks=clang-analyzer-core.NullDereference \
    -warnings-as-errors=clang-analyzer-core.NullDereference \
    -- -std=c11
grep -q 'clang-analyzer-core.NullDereference' "$LOG_DIR/negative.log" ||
    fail "clang-tidy negative probe failed without detecting the null dereference"

printf '%s\n' 'int main( ){return 0;}' >"$negative_dir/format.c"
expect_failure negative_probes "$LOG_DIR/negative.log" \
    "$CLANG_FORMAT" --dry-run --Werror "$negative_dir/format.c"
grep -q 'code should be clang-formatted' "$LOG_DIR/negative.log" ||
    fail "clang-format negative probe failed without detecting format drift"

expect_failure negative_probes "$LOG_DIR/negative.log" \
    "$CMAKE" \
    -S "$LIB_DIR/tests/package_consumer" \
    -B "$negative_dir/package" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_C_COMPILER=$BUILD_ROOT/bin/zig-cc" \
    "-DCMAKE_CXX_COMPILER=$BUILD_ROOT/bin/zig-cxx" \
    -DCMAKE_CXX_FLAGS=-nostdlib++ \
    -DCMAKE_DISABLE_FIND_PACKAGE_odt=TRUE
grep -q 'CMAKE_DISABLE_FIND_PACKAGE_odt is enabled' "$LOG_DIR/negative.log" ||
    fail "package negative probe failed without rejecting the disabled required package"

benchmark_negative_build="$negative_dir/benchmark-no-sink"
benchmark_negative="$benchmark_negative_build/odt_benchmark"
run_logged negative_probes "$LOG_DIR/negative.log" \
    "$CMAKE" \
    -S "$EXAMPLE_DIR" \
    -B "$benchmark_negative_build" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_C_COMPILER=$NATIVE_CC" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$BENCHMARK_FLAGS" \
    "-DCMAKE_PREFIX_PATH=$benchmark_install" \
    -DODT_BENCHMARK_DISABLE_OBSERVABLE_SINK=ON
run_logged negative_probes "$LOG_DIR/negative.log" \
    "$CMAKE" --build "$benchmark_negative_build" --target odt_benchmark
expect_failure negative_probes "$LOG_DIR/negative.log" \
    "$benchmark_negative" --self-check-dce
grep -q 'observable benchmark checksum sink is disabled' "$LOG_DIR/negative.log" ||
    fail "benchmark DCE negative probe did not detect the disabled sink"

expect_failure negative_probes "$LOG_DIR/negative.log" \
    "$benchmark_binary" \
    --sites "$EXAMPLE_DIR/data/sites.csv" \
    --queries "$EXAMPLE_DIR/data/queries.csv" \
    --snapshot "$negative_dir/mismatch.odt" \
    --output "$negative_dir/mismatch.json" \
    --repetitions 5 \
    --warmup 0 \
    --tree-loops 1 \
    --brute-loops 1 \
    --inject-mismatch
grep -q 'semantic mismatch at query 0' "$LOG_DIR/negative.log" ||
    fail "benchmark semantic negative probe did not detect the injected mismatch"

timeout_standin="$negative_dir/sleeping-standin"
printf '%s\n' '#!/bin/sh' 'sleep 2' >"$timeout_standin"
chmod 0755 "$timeout_standin"
if run_with_timeout negative_probes "$LOG_DIR/negative.log" \
    "$TIMEOUT_PROBE_SECONDS" "$timeout_standin"; then
    fail "portable timeout negative probe unexpectedly completed"
else
    timeout_probe_status=$?
fi
[[ "$timeout_probe_status" == 124 ]] ||
    fail "portable timeout negative probe returned $timeout_probe_status instead of 124"
grep -q "timeout_status=timed_out timeout_seconds=$TIMEOUT_PROBE_SECONDS" \
    "$LOG_DIR/negative.log" ||
    fail "portable timeout negative probe did not terminate the sleeping stand-in"
append_binary "$benchmark_negative"
record_stage negative_probes passed true "$EXPECTED_NEGATIVE_PROBE_COUNT" \
    "$EXPECTED_NEGATIVE_PROBE_COUNT" "controlled gate failures" \
    "$STUDY_REL/evidence/raw/u7-negative-probes.txt" ""

{
    printf '%s\n' \
        'FIL-C validates only executed supported correctness paths and is not a complete proof or performance baseline.' \
        "$tsan_limitation" \
        'Sanitizer objects use Zig 0.16.0; Apple Clang 21 is only the runtime linker, with a version-symbol adapter that calls the Apple runtime check.' \
        'LeakSanitizer is unavailable in this Apple AddressSanitizer runtime; FIL-C and explicit allocation counters retain mandatory leak coverage.' \
        'clang-tidy covers the eight production C translation units and the benchmark with clang-analyzer checks.' \
        'The C++17 consumer has no standard-library dependency and uses -nostdlib++ because Zig 0.16.0 cannot build bundled libc++ against this macOS SDK.' \
        "$target_limitation" \
        'The local benchmark uses native -O3 -march=native -mtune=native -DNDEBUG; it is not target-machine evidence.' \
        "$benchmark_binding_limitation" \
        "$benchmark_pmu_record" \
        'The evidence manifest excludes its own digest to avoid recursive identity.' \
        'The pre-existing projects/clipvault worktree change was neither read as input nor modified.'
} >"$BUILD_ROOT/limitations.txt"

current_stage=evidence_manifest
record_stage evidence_manifest passed true 4 4 "CPython hashlib/json validator" \
    "$STUDY_REL/evidence/manifest.json" \
    "Manifest self-digest is excluded to avoid recursive identity."
record_command evidence_manifest "$PYTHON" "<embedded promoted manifest verifier>"
prepare_curated_records
generate_manifest

mkdir -p "$RAW_DIR"
while IFS=$'\t' read -r relative temporary; do
    cp "$temporary" "$REPO_ROOT/$relative"
done <"$RAW_MAP_TSV"
cp "$BUILD_ROOT/manifest.json" "$EVIDENCE_DIR/manifest.json"

assert_no_unexpected_untracked
verify_promoted_manifest | tee "$LOG_DIR/manifest-verification.log"

required_stage_count="$(awk -F '\t' '$3 == "true" { count += 1 } END { print count + 0 }' \
    "$STAGES_TSV")"
total_stage_count="$(wc -l <"$STAGES_TSV" | tr -d ' ')"
printf 'Local U8 qualification passed: evidence_scope=%s target_benchmark=%s target_access=%s ' \
    "$benchmark_evidence_scope" "$target_benchmark_summary" "$target_access_summary"
printf 'required_stages=%d/%d total_stages=%d ' \
    "$required_stage_count" "$required_stage_count" "$total_stage_count"
printf 'debug=%d/%d release=%d/%d sanitizers=%d/%d filc=%d/%d python=%d/%d focused=%d/%d\n' \
    "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT" \
    "$EXPECTED_CTEST_COUNT" "$EXPECTED_CTEST_COUNT" \
    "$EXPECTED_SANITIZER_GATE_COUNT" "$EXPECTED_SANITIZER_GATE_COUNT" \
    "$EXPECTED_FILC_TEST_COUNT" "$EXPECTED_FILC_TEST_COUNT" \
    "$EXPECTED_PYTHON_TEST_COUNT" "$EXPECTED_PYTHON_TEST_COUNT" \
    "$EXPECTED_FOCUSED_TEST_COUNT" "$EXPECTED_FOCUSED_TEST_COUNT"
