#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_REL='learning/studies/20260910-stackful-coroutine/demo/bench/cacs_scale_evidence.sh'
readonly SOURCE_ROOT_REL='learning/studies/20260910-stackful-coroutine/demo'
readonly WORK_ROOT_REL='.tmp/rco-cacs/work'
readonly CLEAN_ROOT_REL='.tmp/rco-cacs/final-evidence'
readonly PROMOTION_ROOT_REL='learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale'
readonly ZIG_REL='.tmp/zig/dist/zig'
readonly ZIG_ARCHIVE_REL='.tmp/zig/downloads/zig-x86_64-linux-0.16.0.tar.xz'
readonly ZIG_ARCHIVE_SHA256='70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00'
readonly FILC_ROOT_REL='.tmp/fil-c'
readonly FILC_VERSION_REL='.tmp/fil-c/dist/README.md'
readonly DEBIAN_IMAGE='debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171'
readonly FILC_LD='/usr/bin/x86_64-linux-gnu-ld.bfd'
readonly FILC_BFD='/lib/x86_64-linux-gnu/libbfd-2.31.1-system.so'

die() {
    printf 'cacs_scale_evidence: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command is unavailable: $1"
}

path_overlaps() {
    [[ "$1" == "$2" || "$1" == "$2/"* || "$2" == "$1/"* ]]
}

validate_relative_root() {
    local canonical
    local component
    local current
    local label=$1
    local relative=$2
    local result_variable=$3
    local -a components

    [[ -n "$relative" ]] || die "$label must not be empty"
    [[ "$relative" != /* ]] || die "$label must be repository-relative: $relative"
    [[ "$relative" != '.' ]] || die "$label must not be the repository root"
    [[ "$relative" != */ ]] || die "$label must not end with '/': $relative"
    [[ "$relative" != *//* ]] || die "$label is not normalized: $relative"
    case "/$relative/" in
        */../*) die "$label contains a '..' component: $relative" ;;
        */./*) die "$label contains a '.' component: $relative" ;;
    esac

    canonical=$(realpath -m -- "$repo_root/$relative")
    [[ "$canonical" == "$repo_root/$relative" ]] ||
        die "$label is not normalized: $relative"
    [[ "$canonical" == "$repo_root/"* ]] ||
        die "$label escapes the repository: $relative"

    current=$repo_root
    IFS='/' read -r -a components <<<"$relative"
    for component in "${components[@]}"; do
        current="$current/$component"
        [[ ! -L "$current" ]] ||
            die "$label traverses a symlink: $relative"
    done

    printf -v "$result_variable" '%s' "$canonical"
}

capture_build_commands() {
    local build_root=$1
    local profile=$2
    local evidence_root="$clean_root/build-commands/$profile"

    mkdir -p -- "$evidence_root"
    [[ -s "$build_root/compile_commands.json" ]] ||
        die "$profile did not produce compile_commands.json"
    cp -- "$build_root/compile_commands.json" \
        "$evidence_root/compile_commands.json"

    python3 - "$build_root/compile_commands.json" \
        "$evidence_root/compile-commands.txt" <<'PY'
import json
from pathlib import Path
import re
import shlex
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
entries = json.loads(source.read_text(encoding="utf-8"))
if not isinstance(entries, list) or not entries:
    raise SystemExit("compile_commands.json is empty")

records = []
for entry in entries:
    if not isinstance(entry, dict):
        raise SystemExit("compile command entry is not an object")
    if "command" in entry:
        command = entry["command"]
    elif "arguments" in entry:
        command = shlex.join(entry["arguments"])
    else:
        raise SystemExit("compile command has neither command nor arguments")
    output = entry.get("output", "")
    match = re.search(
        r"(?:^|[ /])CMakeFiles/([^/ ]+)\.dir/", output + " " + command
    )
    if match is None:
        raise SystemExit("cannot bind compile command to a CMake target")
    records.append(
        (
            match.group(1),
            entry.get("file", ""),
            entry.get("directory", ""),
            output,
            command,
        )
    )

with destination.open("w", encoding="utf-8", newline="\n") as stream:
    for target, file_name, directory, output, command in sorted(records):
        stream.write(f"target={target}\n")
        stream.write(f"directory={directory}\n")
        stream.write(f"file={file_name}\n")
        stream.write(f"output={output}\n")
        stream.write(f"command={command}\n\n")
PY

    : >"$evidence_root/link-commands.txt"
    while IFS= read -r -d '' link_file; do
        local relative=${link_file#"$build_root/"}
        local target=${relative#CMakeFiles/}
        target=${target%%.dir/*}
        {
            printf 'target=%s\n' "$target"
            printf 'file=%s\n' "$relative"
            printf 'command='
            tr '\n' ' ' <"$link_file"
            printf '\n\n'
        } >>"$evidence_root/link-commands.txt"
    done < <(
        find "$build_root/CMakeFiles" -type f -name link.txt -print0 |
            LC_ALL=C sort -z
    )
    [[ -s "$evidence_root/link-commands.txt" ]] ||
        die "$profile did not produce link commands"
}

configure_and_build() {
    local build_root=$1
    local profile=$2
    local build_type=$3
    local c_flags=$4
    local link_flags=$5
    local command_root="$clean_root/build-commands/$profile"

    mkdir -p -- "$command_root"
    cmake -S "$source_root" -B "$build_root" -G 'Unix Makefiles' \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DCMAKE_C_COMPILER="$zig_cc" \
        -DCMAKE_ASM_COMPILER="$zig_cc" \
        -DCMAKE_C_FLAGS="$c_flags" \
        -DCMAKE_C_FLAGS_DEBUG= \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO= \
        -DCMAKE_C_FLAGS_RELEASE= \
        -DCMAKE_ASM_FLAGS='-fcf-protection=branch' \
        -DCMAKE_ASM_FLAGS_DEBUG= \
        -DCMAKE_ASM_FLAGS_RELWITHDEBINFO= \
        -DCMAKE_ASM_FLAGS_RELEASE= \
        -DCMAKE_EXE_LINKER_FLAGS="$link_flags" \
        -DCMAKE_EXE_LINKER_FLAGS_DEBUG= \
        -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO= \
        -DCMAKE_EXE_LINKER_FLAGS_RELEASE= \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DRCO_BUILD_CACS=ON \
        -DRCO_BUILD_EXAMPLES=ON \
        -DRCO_BUILD_BENCHMARKS=ON \
        -DRCO_BUILD_TESTS=ON \
        2>&1 | tee "$command_root/configure.txt"
    cmake --build "$build_root" -j2 \
        2>&1 | tee "$command_root/build.txt"
    capture_build_commands "$build_root" "$profile"
}

generate_manifest() {
    python3 - "$1" <<'PY'
from hashlib import sha256
import os
from pathlib import Path
import stat
import sys

root = Path(sys.argv[1])
manifest = root / "SHA256SUMS"
payloads = []
for path in root.rglob("*"):
    relative = path.relative_to(root)
    if relative == Path("SHA256SUMS"):
        continue
    mode = path.lstat().st_mode
    if stat.S_ISLNK(mode):
        raise SystemExit(f"payload is a symlink: {relative}")
    if stat.S_ISREG(mode):
        payloads.append(relative)
    elif not stat.S_ISDIR(mode):
        raise SystemExit(f"payload is not a regular file or directory: {relative}")

payloads.sort(key=lambda path: os.fsencode(str(path)))
with manifest.open("w", encoding="utf-8", newline="\n") as stream:
    for relative in payloads:
        digest = sha256()
        with (root / relative).open("rb") as payload:
            for chunk in iter(lambda: payload.read(1024 * 1024), b""):
                digest.update(chunk)
        stream.write(f"{digest.hexdigest()}  {relative.as_posix()}\n")
PY
}

verify_manifest() {
    python3 - "$1" <<'PY'
from hashlib import sha256
import os
from pathlib import Path
import re
import stat
import sys

root = Path(sys.argv[1])
manifest = root / "SHA256SUMS"
if not manifest.is_file() or manifest.is_symlink():
    raise SystemExit("SHA256SUMS is missing or is a symlink")

entries = []
for line_number, line in enumerate(
    manifest.read_text(encoding="utf-8").splitlines(), 1
):
    match = re.fullmatch(r"([0-9a-f]{64})  ([^\n]+)", line)
    if match is None:
        raise SystemExit(f"invalid manifest line {line_number}")
    relative = Path(match.group(2))
    if relative.is_absolute() or relative == Path(".") or ".." in relative.parts:
        raise SystemExit(f"unsafe manifest path: {relative}")
    entries.append((relative, match.group(1)))

entry_paths = [relative for relative, _digest in entries]
if entry_paths != sorted(entry_paths, key=lambda path: os.fsencode(str(path))):
    raise SystemExit("manifest paths are not in bytewise order")
if len(entry_paths) != len(set(entry_paths)):
    raise SystemExit("manifest contains duplicate paths")

payloads = []
for path in root.rglob("*"):
    relative = path.relative_to(root)
    if relative == Path("SHA256SUMS"):
        continue
    mode = path.lstat().st_mode
    if stat.S_ISLNK(mode):
        raise SystemExit(f"payload is a symlink: {relative}")
    if stat.S_ISREG(mode):
        payloads.append(relative)
    elif not stat.S_ISDIR(mode):
        raise SystemExit(f"unexpected payload type: {relative}")
payloads.sort(key=lambda path: os.fsencode(str(path)))
if entry_paths != payloads:
    raise SystemExit("manifest path coverage does not match payload files")

for relative, expected in entries:
    digest = sha256()
    with (root / relative).open("rb") as payload:
        for chunk in iter(lambda: payload.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise SystemExit(f"manifest digest mismatch: {relative}")
PY
}

[[ $# -eq 0 ]] || die 'this driver accepts no arguments'

for command in bash git realpath uname; do
    require_command "$command"
done

[[ $(uname -s) == Linux ]] ||
    die 'the evidence driver runs only on Linux'
case "$(uname -m)" in
    x86_64|amd64) ;;
    *) die 'the evidence driver runs only on x86-64' ;;
esac

for command in \
    awk cat cmake cp ctest docker find gcc grep iperf3 jq lscpu make mkdir \
    mktemp numactl objdump perf python3 readelf rm rsync sha256sum sort stat \
    tee seq timeout tr wc; do
    require_command "$command"
done

repo_root=$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" \
    rev-parse --show-toplevel)
repo_root=$(realpath -- "$repo_root")
[[ "$(realpath -- "${BASH_SOURCE[0]}")" == "$repo_root/$SCRIPT_REL" ]] ||
    die "driver must run from its tracked repository path: $SCRIPT_REL"
cd "$repo_root"

validate_relative_root work_root "$WORK_ROOT_REL" work_root
validate_relative_root clean_root "$CLEAN_ROOT_REL" clean_root
validate_relative_root promotion_root "$PROMOTION_ROOT_REL" promotion_root
validate_relative_root source_root "$SOURCE_ROOT_REL" source_root
validate_relative_root zig "$ZIG_REL" zig
validate_relative_root \
    zig_archive "$ZIG_ARCHIVE_REL" zig_archive
validate_relative_root filc_root "$FILC_ROOT_REL" filc_root
validate_relative_root \
    filc_version_file "$FILC_VERSION_REL" filc_version_file

for pair in \
    "$work_root|$clean_root" \
    "$work_root|$promotion_root" \
    "$clean_root|$promotion_root"; do
    left=${pair%%|*}
    right=${pair#*|}
    path_overlaps "$left" "$right" &&
        die "declared roots overlap: $left and $right"
done

[[ "$PROMOTION_ROOT_REL" == \
   'learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale' ]] ||
    die 'promotion root is not the declared CACS evidence root'
if [[ -e "$promotion_root" ]]; then
    [[ -d "$promotion_root" && ! -L "$promotion_root" ]] ||
        die "promotion root is unsafe: $PROMOTION_ROOT_REL"
    [[ -z $(find "$promotion_root" -type l -print -quit) ]] ||
        die "promotion root contains a symlink: $PROMOTION_ROOT_REL"
fi
[[ -d "$(dirname -- "$promotion_root")" ]] ||
    die "promotion root parent is missing: $PROMOTION_ROOT_REL"

for disposable_root in "$WORK_ROOT_REL" "$CLEAN_ROOT_REL"; do
    [[ -z $(git ls-files -- "$disposable_root") ]] ||
        die "clean/work root overlaps tracked files: $disposable_root"
    git check-ignore -q -- "$disposable_root/.evidence-path-probe" ||
        die "clean/work root is not ignored: $disposable_root"
done

[[ -x "$zig" && -f "$zig" && ! -L "$zig" ]] ||
    die "pinned Zig is missing or unsafe: $ZIG_REL"
[[ "$("$zig" version)" == '0.16.0' ]] ||
    die "pinned Zig version is not 0.16.0: $ZIG_REL"
[[ -f "$zig_archive" && ! -L "$zig_archive" ]] ||
    die "pinned Zig archive is missing or unsafe: $ZIG_ARCHIVE_REL"
[[ "$(sha256sum -- "$zig_archive" | awk '{print $1}')" == \
   "$ZIG_ARCHIVE_SHA256" ]] ||
    die "pinned Zig archive digest mismatch: $ZIG_ARCHIVE_REL"
for filc_tool in bin/filcc bin/filrun; do
    [[ -x "$filc_root/$filc_tool" && ! -L "$filc_root/$filc_tool" ]] ||
        die "pinned FIL-C tool is missing or unsafe: $FILC_ROOT_REL/$filc_tool"
done
[[ -f "$filc_version_file" && ! -L "$filc_version_file" ]] ||
    die "pinned FIL-C version record is missing or unsafe: $FILC_VERSION_REL"
grep -Fxq '# Fil-C 0.684' "$filc_version_file" ||
    die "pinned FIL-C release is not 0.684: $FILC_VERSION_REL"
[[ -f "$FILC_LD" && -f "$FILC_BFD" ]] ||
    die 'the pinned FIL-C container linker adapter is unavailable'
docker image inspect "$DEBIAN_IMAGE" >/dev/null 2>&1 ||
    die "pinned FIL-C container image is not installed: $DEBIAN_IMAGE"

source_status=$(git status --porcelain=v1 --untracked-files=all -- \
    "$SOURCE_ROOT_REL")
[[ -z "$source_status" ]] ||
    die "tracked source must be clean before evidence generation: $SOURCE_ROOT_REL"
source_commit=$(git rev-parse --verify HEAD^{commit})

rm -rf -- "$work_root" "$clean_root"
mkdir -p -- "$work_root" "$clean_root"

readonly zig_cc="$zig;cc"
readonly common_flags='-march=native -mtune=native -Wall -Wextra -Werror -fno-omit-frame-pointer -fcf-protection=branch'
readonly o0_flags="-O0 -g $common_flags"
readonly o2_flags="-O2 -g $common_flags"
readonly o3_flags="-O3 -DNDEBUG -flto -fno-ipa-icf $common_flags"
readonly sanitizer_flags="-O1 -g -fsanitize=address,undefined $common_flags"

readonly o3_build="$work_root/o3-build"
readonly o0_build="$work_root/o0-build"
readonly o2_build="$work_root/o2-build"
readonly sanitizer_build="$work_root/sanitizer-build"
readonly gcc_negative_build="$work_root/gcc-negative-build"

docker run --rm --pull=never --network none \
    -v "$repo_root:$repo_root" \
    -v "$FILC_LD:/usr/bin/ld:ro" \
    -v "$FILC_BFD:$FILC_BFD:ro" \
    -w "$repo_root" \
    -e SOURCE_ROOT_REL="$SOURCE_ROOT_REL" \
    -e WORK_ROOT_REL="$WORK_ROOT_REL" \
    -e FILC_ROOT_REL="$FILC_ROOT_REL" \
    "$DEBIAN_IMAGE" \
    sh -ceu '
        src=$SOURCE_ROOT_REL
        filcc=$FILC_ROOT_REL/bin/filcc
        filrun=$FILC_ROOT_REL/bin/filrun
        runtime=$WORK_ROOT_REL/filc-runtime
        scale=$WORK_ROOT_REL/filc-scale
        parser_stdout=$WORK_ROOT_REL/filc-parser.stdout
        parser_stderr=$WORK_ROOT_REL/filc-parser.stderr
        set -x
        "$filcc" --version
        "$filcc" -DRCO_FILC -std=c11 -O2 -g -Wall -Wextra -Werror \
            -I "$src/include" -I "$src/src" \
            "$src/src/rco.c" "$src/tests/rco_context_filc.c" \
            "$src/tests/rco_filc_test.c" -o "$runtime"
        "$filrun" "$runtime"
        "$filcc" -DRCO_FILC -std=c11 -O2 -g -Wall -Wextra -Werror \
            -I "$src/include" -I "$src/src" \
            "$src/src/rco.c" "$src/tests/rco_context_filc.c" \
            "$src/bench/rco_high_concurrency_bench.c" -o "$scale"
        set +e
        "$filrun" "$scale" \
            --tasks 0 --stack-bytes 32768 \
            --touch-bytes 4096 --yields-per-task 1 \
            >"$parser_stdout" 2>"$parser_stderr"
        status=$?
        set -e
        test "$status" -eq 2
        grep -q "tasks must be" "$parser_stderr"
        cat "$parser_stdout"
        cat "$parser_stderr"
        printf "FIL-C high-concurrency parser/bounds PASS\n"
    ' >"$clean_root/filc.txt" 2>&1

configure_and_build "$o3_build" o3 Release "$o3_flags" '-flto'
ctest --test-dir "$o3_build" --output-on-failure \
    2>&1 | tee "$clean_root/native-tests.txt"

configure_and_build "$o0_build" o0 Debug "$o0_flags" ''
{
    printf 'profile=o0\n'
    ctest --test-dir "$o0_build" --output-on-failure \
        -R '^(rco_native|rco_guard_page|rco_cacs|rco_high_concurrency_)'
} 2>&1 | tee "$clean_root/optimization-matrix.txt"

configure_and_build "$o2_build" o2 RelWithDebInfo "$o2_flags" ''
{
    printf '\nprofile=o2\n'
    ctest --test-dir "$o2_build" --output-on-failure \
        -R '^(rco_native|rco_guard_page|rco_cacs|rco_high_concurrency_)'
} 2>&1 | tee -a "$clean_root/optimization-matrix.txt"

set +e
cmake -S "$source_root" -B "$gcc_negative_build" -G 'Unix Makefiles' \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRCO_BUILD_CACS=ON >"$clean_root/gcc-negative.txt" 2>&1
gcc_negative_status=$?
set -e
[[ "$gcc_negative_status" -ne 0 ]] ||
    die 'GCC unexpectedly configured the CACS targets'
grep -q 'requires Zig cc/Clang 21' "$clean_root/gcc-negative.txt" ||
    die 'GCC negative configure failed for an unexpected reason'

configure_and_build \
    "$sanitizer_build" asan Debug "$sanitizer_flags" \
    '-fsanitize=address,undefined'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir "$sanitizer_build" --output-on-failure \
        -E 'rco_guard_page' \
        2>&1 | tee "$clean_root/sanitizer-tests.txt"

printf 'run,position,mode,rco_yield_ns,function_call_ns,sched_yield_ns,sink\n' \
    >"$clean_root/context-switch.csv"
for run in 1 2 3 4 5; do
    case $(((run - 1) % 3)) in
        0) modes=(sysv cacs cacs-preserve-none) ;;
        1) modes=(cacs cacs-preserve-none sysv) ;;
        2) modes=(cacs-preserve-none sysv cacs) ;;
    esac
    position=0
    for mode in "${modes[@]}"; do
        position=$((position + 1))
        case "$mode" in
            sysv) binary="$o3_build/rco_bench" ;;
            cacs) binary="$o3_build/rco_bench_cacs" ;;
            cacs-preserve-none)
                binary="$o3_build/rco_bench_cacs_preserve_none"
                ;;
        esac
        sample="$clean_root/context-${mode}-${run}.txt"
        numactl --physcpubind=0 --membind=0 \
            "$binary" 1000000 11 >"$sample"
        awk -F, -v run="$run" -v position="$position" -v mode="$mode" '
            $1 == "rco_yield" { coroutine = $2 }
            $1 == "function_call" { call = $2 }
            $1 == "sched_yield" { kernel = $2 }
            /^sink=/ {
                split($0, value, "=")
                sink = value[2]
            }
            END {
                if (coroutine == "" || call == "" ||
                    kernel == "" || sink == "") {
                    exit 1
                }
                printf "%s,%s,%s,%s,%s,%s,%s\n",
                       run, position, mode, coroutine, call, kernel, sink
            }
        ' "$sample" >>"$clean_root/context-switch.csv"
    done
done

numactl --physcpubind=0 --membind=0 \
    python3 "$source_root/bench/rco_high_concurrency_bench.py" \
        --sysv "$o3_build/rco_high_concurrency_bench_sysv" \
        --cacs "$o3_build/rco_high_concurrency_bench_cacs" \
        --cacs-preserve-none \
            "$o3_build/rco_high_concurrency_bench_cacs_preserve_none" \
        --output-dir "$clean_root/high-concurrency" \
        --runs 5 \
        --timeout-seconds 120

RUNS=5 DURATION=3 PARALLEL=4 BASE_PORT=61000 \
    "$source_root/bench/l4_bench.sh" \
    "$o3_build/rco_l4_forwarder" \
    "$o3_build/rco_l4_forwarder_epoll" \
    "$clean_root/l4-five-mode" \
    "$o3_build/rco_l4_forwarder_cacs" \
    "$o3_build/rco_l4_forwarder_cacs_preserve_none" \
    2>&1 | tee "$clean_root/l4-summary.txt"

{
    printf 'source_commit=%s\n' "$source_commit"
    printf 'source_root=%s\n' "$SOURCE_ROOT_REL"
    printf 'work_root=%s\n' "$WORK_ROOT_REL"
    printf 'clean_root=%s\n' "$CLEAN_ROOT_REL"
    printf 'promotion_root=%s\n' "$PROMOTION_ROOT_REL"
    printf 'zig_archive_sha256=%s\n' "$ZIG_ARCHIVE_SHA256"
    printf 'filc_release=0.684\n'
    uname -a
    lscpu
    "$zig" version
    "$zig" cc --version
    cmake --version
    gcc --version
    python3 --version
    printf 'o0_flags=%s\n' "$o0_flags"
    printf 'o2_flags=%s\n' "$o2_flags"
    printf 'o3_flags=%s\n' "$o3_flags"
    printf 'sanitizer_flags=%s\n' "$sanitizer_flags"
    printf 'runs=5\n'
    printf 'context_cpu=0 context_numa=0\n'
    printf 'l4_parallel=4 l4_modes=5 l4_forwarders=4\n'
    printf 'rlimit_nofile_soft=%s\n' "$(ulimit -Sn)"
    printf 'rlimit_nofile_hard=%s\n' "$(ulimit -Hn)"
    printf 'perf_event_paranoid='
    cat /proc/sys/kernel/perf_event_paranoid
    printf 'cpufreq_policy_paths='
    find /sys/devices/system/cpu/cpufreq -mindepth 1 -maxdepth 1 \
        -type d -print 2>/dev/null | LC_ALL=C sort | tr '\n' ',' || true
    printf '\n'
    printf 'tool_sha256:\n'
    sha256sum -- \
        "$ZIG_REL" \
        "$ZIG_ARCHIVE_REL" \
        "$FILC_ROOT_REL/bin/filcc" \
        "$FILC_ROOT_REL/bin/filrun" \
        "$FILC_VERSION_REL"
    printf 'source_sha256:\n'
    while IFS= read -r -d '' source_file; do
        sha256sum -- "$source_file"
    done < <(git ls-files -z -- "$SOURCE_ROOT_REL" | LC_ALL=C sort -z)
    printf 'binary_sha256:\n'
    sha256sum -- \
        "$o3_build/rco_bench" \
        "$o3_build/rco_bench_cacs" \
        "$o3_build/rco_bench_cacs_preserve_none" \
        "$o3_build/rco_high_concurrency_bench_sysv" \
        "$o3_build/rco_high_concurrency_bench_cacs" \
        "$o3_build/rco_high_concurrency_bench_cacs_preserve_none" \
        "$o3_build/rco_l4_forwarder" \
        "$o3_build/rco_l4_forwarder_cacs" \
        "$o3_build/rco_l4_forwarder_cacs_preserve_none" \
        "$o3_build/rco_l4_forwarder_epoll"
} >"$clean_root/environment.txt" 2>&1

for mode in sysv cacs cacs-preserve-none; do
    case "$mode" in
        sysv) binary="$o3_build/rco_high_concurrency_bench_sysv" ;;
        cacs) binary="$o3_build/rco_high_concurrency_bench_cacs" ;;
        cacs-preserve-none)
            binary="$o3_build/rco_high_concurrency_bench_cacs_preserve_none"
            ;;
    esac
    objdump -dr -Mintel "$binary" >"$clean_root/disassembly-$mode.txt"
    readelf -W -l "$binary" | grep GNU_STACK \
        >"$clean_root/gnu-stack-$mode.txt"
done

[[ -s "$o3_build/rco_cacs_preserve_none-runtime.ll" ]] ||
    die 'the O3 codegen test did not produce preserve-none LLVM IR'
grep -n 'preserve_nonecc' \
    "$o3_build/rco_cacs_preserve_none-runtime.ll" \
    >"$clean_root/preserve-none-ir.txt"

set +e
perf stat \
    -e cycles,instructions,branches,branch-misses,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses \
    -o "$clean_root/perf-stat.txt" \
    numactl --physcpubind=0 --membind=0 \
    "$o3_build/rco_bench_cacs" 500000 3 >/dev/null
perf_status=$?
set -e
printf '\nperf_exit_status=%s\n' "$perf_status" \
    >>"$clean_root/perf-stat.txt"

python3 - \
    "$clean_root" "$o3_build" "$source_commit" \
    "$CLEAN_ROOT_REL" "$PROMOTION_ROOT_REL" <<'PY'
import csv
from collections import Counter, defaultdict
from hashlib import sha256
import json
from pathlib import Path
import re
import statistics
import sys

root = Path(sys.argv[1])
build = Path(sys.argv[2])
source_commit = sys.argv[3]
clean_root_relative = sys.argv[4]
promotion_root_relative = sys.argv[5]


def fail(message):
    raise SystemExit(f"validation failed: {message}")


def file_sha256(path):
    digest = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(relative, expected_fields):
    path = root / relative
    if not path.is_file():
        fail(f"missing CSV: {relative}")
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != expected_fields:
            fail(f"unexpected schema in {relative}: {reader.fieldnames}")
        return list(reader)


def require_positive(row, fields, context):
    for field in fields:
        try:
            value = float(row[field])
        except (KeyError, ValueError):
            fail(f"{context} has a nonnumeric {field}")
        if value <= 0:
            fail(f"{context} has a nonpositive {field}")


def ctest_count(relative, expected):
    text = (root / relative).read_text(encoding="utf-8")
    matches = re.findall(
        r"100% tests passed, 0 tests failed out of ([0-9]+)", text
    )
    if matches != [str(value) for value in expected]:
        fail(f"unexpected CTest counts in {relative}: {matches}")


expected_targets = {
    "rco",
    "rco_cacs",
    "rco_cacs_preserve_none",
    "rco_bench",
    "rco_bench_cacs",
    "rco_bench_cacs_preserve_none",
    "rco_high_concurrency_bench_sysv",
    "rco_high_concurrency_bench_cacs",
    "rco_high_concurrency_bench_cacs_preserve_none",
    "rco_l4_forwarder",
    "rco_l4_forwarder_cacs",
    "rco_l4_forwarder_cacs_preserve_none",
    "rco_l4_forwarder_epoll",
}
profile_options = {
    "o0": ("-O0",),
    "o2": ("-O2",),
    "o3": ("-O3", "-DNDEBUG", "-flto"),
    "asan": ("-O1", "-fsanitize=address,undefined"),
}
for profile, options in profile_options.items():
    command_root = root / "build-commands" / profile
    raw_path = command_root / "compile_commands.json"
    entries = json.loads(raw_path.read_text(encoding="utf-8"))
    targets = set()
    for entry in entries:
        command = entry.get("command", " ".join(entry.get("arguments", ())))
        output = entry.get("output", "")
        match = re.search(
            r"(?:^|[ /])CMakeFiles/([^/ ]+)\.dir/", output + " " + command
        )
        if match is None:
            fail(f"{profile} compile command is not target-bound")
        target = match.group(1)
        targets.add(target)
        if entry.get("file", "").endswith(".c"):
            for option in options:
                if option not in command:
                    fail(f"{profile}/{target} omits {option}")
            for option in (
                "-march=native",
                "-mtune=native",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-omit-frame-pointer",
                "-fcf-protection=branch",
            ):
                if option not in command:
                    fail(f"{profile}/{target} omits {option}")
    missing = expected_targets - targets
    if missing:
        fail(f"{profile} compile commands omit targets: {sorted(missing)}")
    captured = (command_root / "compile-commands.txt").read_text(
        encoding="utf-8"
    )
    linked = (command_root / "link-commands.txt").read_text(encoding="utf-8")
    for target in expected_targets:
        if f"target={target}\n" not in captured:
            fail(f"{profile} per-target compile evidence omits {target}")
        if f"target={target}\n" not in linked:
            fail(f"{profile} link evidence omits {target}")

ctest_count("native-tests.txt", (25,))
ctest_count("optimization-matrix.txt", (16, 16))
ctest_count("sanitizer-tests.txt", (22,))
filc_text = (root / "filc.txt").read_text(encoding="utf-8")
for marker in (
    "FIL-C high-concurrency parser/bounds PASS",
    "FIL-C rco tests passed",
):
    if marker not in filc_text:
        fail(f"FIL-C evidence omits {marker}")
if "requires Zig cc/Clang 21" not in (
    root / "gcc-negative.txt"
).read_text(encoding="utf-8"):
    fail("GCC negative gate evidence is missing")

context_fields = [
    "run",
    "position",
    "mode",
    "rco_yield_ns",
    "function_call_ns",
    "sched_yield_ns",
    "sink",
]
context_rows = read_csv("context-switch.csv", context_fields)
context_modes = ("sysv", "cacs", "cacs-preserve-none")
if len(context_rows) != 15:
    fail("context benchmark does not contain 15 samples")
if Counter((row["run"], row["mode"]) for row in context_rows) != Counter(
    (str(run), mode) for run in range(1, 6) for mode in context_modes
):
    fail("context benchmark run/mode coverage is incomplete")
for row in context_rows:
    require_positive(
        row,
        ("rco_yield_ns", "function_call_ns", "sched_yield_ns", "sink"),
        "context sample",
    )

sample_fields = [
    "schema",
    "case",
    "run",
    "position",
    "mode",
    "backend_identity",
    "binary_sha256",
    "tasks",
    "stack_bytes",
    "touch_bytes",
    "page_size",
    "sentinels_per_task",
    "yields_per_task",
    "requested_yields",
    "barrier_yields",
    "measured_yields",
    "checksum",
    "expected_checksum",
    "runtime_switches",
    "expected_runtime_switches",
    "wall_ns",
    "user_us",
    "system_us",
    "minor_faults_delta",
    "major_faults_delta",
    "voluntary_context_switches_delta",
    "involuntary_context_switches_delta",
    "spawned",
    "completed",
    "peak_active",
    "vm_peak_kb",
    "vm_hwm_kb",
    "vm_size_kb",
    "vm_rss_kb",
    "rss_anon_kb",
    "rss_file_kb",
    "rss_shmem_kb",
    "setup_minor_faults",
    "setup_major_faults",
    "smaps_rss_kb",
    "smaps_pss_kb",
    "smaps_private_clean_kb",
    "smaps_private_dirty_kb",
    "smaps_anonymous_kb",
]
summary_fields = [
    "schema",
    "case",
    "mode",
    "backend_identity",
    "binary_sha256",
    "samples",
    "tasks",
    "stack_bytes",
    "touch_bytes",
    "yields_per_task",
    "requested_yields",
    "barrier_yields",
    "measured_yields",
    "checksum",
    "runtime_switches",
    "median_wall_ns",
    "median_user_us",
    "median_system_us",
    "median_wall_ns_per_measured_yield",
    "median_cpu_ns_per_measured_yield",
    "paired_wall_ratio_vs_sysv",
    "paired_cpu_ratio_vs_sysv",
    "median_setup_minor_faults",
    "median_setup_major_faults",
    "median_minor_faults_delta",
    "median_major_faults_delta",
    "median_voluntary_context_switches_delta",
    "median_involuntary_context_switches_delta",
    "median_vm_hwm_kb",
    "median_vm_rss_kb",
    "median_smaps_rss_kb",
    "median_smaps_pss_kb",
    "median_smaps_anonymous_kb",
]
sample_rows = read_csv("high-concurrency/samples.csv", sample_fields)
summary_rows = read_csv("high-concurrency/summary.csv", summary_fields)
cases = (
    "tasks-256",
    "tasks-1024",
    "tasks-4096",
    "tasks-16384",
    "working-set-32k",
    "working-set-128k",
    "working-set-256k",
    "yields-16",
    "yields-128",
    "yields-1024",
)
high_binaries = {
    "sysv": build / "rco_high_concurrency_bench_sysv",
    "cacs": build / "rco_high_concurrency_bench_cacs",
    "cacs-preserve-none": (
        build / "rco_high_concurrency_bench_cacs_preserve_none"
    ),
}
high_hashes = {mode: file_sha256(path) for mode, path in high_binaries.items()}
expected_high_keys = Counter(
    (case, str(run), mode)
    for run in range(1, 6)
    for case in cases
    for mode in context_modes
)
if len(sample_rows) != 150:
    fail("high-concurrency matrix does not contain 150 samples")
if Counter(
    (row["case"], row["run"], row["mode"]) for row in sample_rows
) != expected_high_keys:
    fail("high-concurrency case/run/mode coverage is incomplete")
positions = defaultdict(set)
for row in sample_rows:
    mode = row["mode"]
    context = f"high-concurrency {row['case']}/{row['run']}/{mode}"
    if row["schema"] != "rco-high-concurrency-v2":
        fail(f"{context} has the wrong schema")
    if row["backend_identity"] != mode:
        fail(f"{context} has the wrong backend identity")
    if row["binary_sha256"] != high_hashes[mode]:
        fail(f"{context} is not bound to the measured binary")
    positions[(row["case"], row["run"])].add(int(row["position"]))
    tasks = int(row["tasks"])
    requested = int(row["requested_yields"])
    barrier = int(row["barrier_yields"])
    measured = int(row["measured_yields"])
    if requested != tasks * int(row["yields_per_task"]):
        fail(f"{context} has an invalid requested-yield count")
    if barrier != tasks - 1 or measured != requested + barrier:
        fail(f"{context} has an invalid barrier/measured-yield count")
    if row["checksum"] != row["expected_checksum"]:
        fail(f"{context} failed the checksum oracle")
    if row["runtime_switches"] != row["expected_runtime_switches"]:
        fail(f"{context} failed the switch-count oracle")
    if any(int(row[field]) != tasks for field in ("spawned", "completed", "peak_active")):
        fail(f"{context} failed the lifecycle oracle")
    if int(row["sentinels_per_task"]) != (
        int(row["touch_bytes"]) // int(row["page_size"])
    ):
        fail(f"{context} failed the page-sentinel oracle")
    require_positive(row, ("wall_ns", "smaps_pss_kb"), context)
if any(value != {1, 2, 3} for value in positions.values()):
    fail("high-concurrency position rotation is incomplete")
if len(set(high_hashes.values())) != 3:
    fail("high-concurrency backend binaries do not have distinct hashes")

if len(summary_rows) != 30:
    fail("high-concurrency summary does not contain 30 rows")
if Counter((row["case"], row["mode"]) for row in summary_rows) != Counter(
    (case, mode) for case in cases for mode in context_modes
):
    fail("high-concurrency summary coverage is incomplete")
for row in summary_rows:
    mode = row["mode"]
    if (
        row["schema"] != "rco-high-concurrency-v2"
        or row["backend_identity"] != mode
        or row["binary_sha256"] != high_hashes[mode]
        or row["samples"] != "5"
    ):
        fail("high-concurrency summary identity/schema/count check failed")

throughput_fields = [
    "mode",
    "run",
    "backend_identity",
    "binary_sha256",
    "bits_per_second",
]
stream_fields = [
    "mode",
    "run",
    "stream",
    "sender_socket",
    "sender_bytes",
    "sender_bits_per_second",
    "receiver_socket",
    "receiver_bytes",
    "receiver_bits_per_second",
]
resource_fields = [
    "mode",
    "run",
    "backend_identity",
    "binary_sha256",
    "vm_peak_kb",
    "vm_hwm_kb",
    "user_ticks",
    "system_ticks",
    "voluntary_context_switches",
    "nonvoluntary_context_switches",
]
rotation_fields = ["run", "position", "mode"]
throughput_rows = read_csv("l4-five-mode/throughput.csv", throughput_fields)
stream_rows = read_csv("l4-five-mode/stream-throughput.csv", stream_fields)
resource_rows = read_csv(
    "l4-five-mode/process-resources.csv", resource_fields
)
rotation_rows = read_csv("l4-five-mode/run-order.csv", rotation_fields)
l4_modes = (
    "direct",
    "coroutine-sysv",
    "cacs",
    "cacs-preserve-none",
    "epoll",
)
l4_forwarders = {
    "coroutine-sysv": build / "rco_l4_forwarder",
    "cacs": build / "rco_l4_forwarder_cacs",
    "cacs-preserve-none": build / "rco_l4_forwarder_cacs_preserve_none",
    "epoll": build / "rco_l4_forwarder_epoll",
}
l4_hashes = {mode: file_sha256(path) for mode, path in l4_forwarders.items()}
if len(set(l4_hashes.values())) != 4:
    fail("the four L4 forwarders do not have distinct hashes")

expected_l4_keys = Counter(
    (str(run), mode) for run in range(1, 6) for mode in l4_modes
)
if (
    len(throughput_rows) != 25
    or Counter((row["run"], row["mode"]) for row in throughput_rows)
    != expected_l4_keys
):
    fail("five-mode L4 aggregate coverage is incomplete")
for row in throughput_rows:
    mode = row["mode"]
    expected_identity = mode
    expected_hash = "not-applicable" if mode == "direct" else l4_hashes[mode]
    if (
        row["backend_identity"] != expected_identity
        or row["binary_sha256"] != expected_hash
    ):
        fail(f"L4 aggregate {row['run']}/{mode} has the wrong identity/hash")
    require_positive(row, ("bits_per_second",), "L4 aggregate")

if (
    len(resource_rows) != 20
    or Counter((row["run"], row["mode"]) for row in resource_rows)
    != Counter(
        (str(run), mode)
        for run in range(1, 6)
        for mode in l4_forwarders
    )
):
    fail("four-forwarder resource coverage is incomplete")
for row in resource_rows:
    mode = row["mode"]
    if (
        row["backend_identity"] != mode
        or row["binary_sha256"] != l4_hashes[mode]
    ):
        fail(f"L4 resource {row['run']}/{mode} has the wrong identity/hash")
    require_positive(row, ("vm_peak_kb", "vm_hwm_kb"), "L4 resource")

if len(stream_rows) != 100:
    fail("five-mode L4 stream coverage does not contain 100 rows")
if Counter((row["run"], row["mode"]) for row in stream_rows) != Counter(
    (str(run), mode)
    for run in range(1, 6)
    for mode in l4_modes
    for _stream in range(4)
):
    fail("five-mode L4 stream run/mode coverage is incomplete")
if Counter(
    (row["run"], row["mode"], row["stream"]) for row in stream_rows
) != Counter(
    (str(run), mode, str(stream))
    for run in range(1, 6)
    for mode in l4_modes
    for stream in range(1, 5)
):
    fail("five-mode L4 stream identities are incomplete")
for row in stream_rows:
    require_positive(
        row,
        (
            "sender_bytes",
            "sender_bits_per_second",
            "receiver_bytes",
            "receiver_bits_per_second",
        ),
        "L4 stream",
    )

if len(rotation_rows) != 25:
    fail("five-mode L4 rotation does not contain 25 rows")
if Counter((row["run"], row["mode"]) for row in rotation_rows) != expected_l4_keys:
    fail("five-mode L4 rotation run/mode coverage is incomplete")
rotation_positions = defaultdict(set)
for row in rotation_rows:
    rotation_positions[row["run"]].add(int(row["position"]))
if any(value != {1, 2, 3, 4, 5} for value in rotation_positions.values()):
    fail("five-mode L4 position coverage is incomplete")

for mode in context_modes:
    disassembly = (root / f"disassembly-{mode}.txt").read_text(
        encoding="utf-8"
    )
    if "rco_runtime_run" not in disassembly:
        fail(f"{mode} disassembly omits rco_runtime_run")
    stack = (root / f"gnu-stack-{mode}.txt").read_text(encoding="utf-8")
    if re.search(r"GNU_STACK.*\bRW\b", stack) is None:
        fail(f"{mode} GNU_STACK is not non-executable RW")
if "preserve_nonecc" not in (
    root / "preserve-none-ir.txt"
).read_text(encoding="utf-8"):
    fail("preserve-none LLVM IR evidence is missing")

perf_text = (root / "perf-stat.txt").read_text(
    encoding="utf-8", errors="replace"
)
perf_match = re.search(r"perf_exit_status=([0-9]+)", perf_text)
if perf_match is None:
    fail("PMU evidence omits the perf exit status")
if re.search(
    r"not supported|No permission|Permission denied|perf_event_paranoid",
    perf_text,
    re.IGNORECASE,
):
    pmu_status = "unavailable"
elif perf_match.group(1) == "0":
    pmu_status = "available"
else:
    fail("perf failed for an unclassified reason")

environment = (root / "environment.txt").read_text(encoding="utf-8")
if f"source_commit={source_commit}\n" not in environment:
    fail("environment evidence is not bound to the source commit")

context_medians = {
    mode: statistics.median(
        float(row["rco_yield_ns"]) for row in context_rows if row["mode"] == mode
    )
    for mode in context_modes
}
l4_medians = {
    mode: statistics.median(
        float(row["bits_per_second"])
        for row in throughput_rows
        if row["mode"] == mode
    )
    / 1_000_000_000
    for mode in l4_modes
}
l4_by_mode_run = {
    mode: {
        int(row["run"]): float(row["bits_per_second"])
        for row in throughput_rows
        if row["mode"] == mode
    }
    for mode in l4_modes
}
l4_paired_ratios = {
    "cacs": statistics.median(
        l4_by_mode_run["cacs"][run]
        / l4_by_mode_run["coroutine-sysv"][run]
        for run in range(1, 6)
    ),
    "cacs-preserve-none": statistics.median(
        l4_by_mode_run["cacs-preserve-none"][run]
        / l4_by_mode_run["coroutine-sysv"][run]
        for run in range(1, 6)
    ),
}
l4_summary = (root / "l4-summary.txt").read_text(encoding="utf-8")
for mode, ratio in l4_paired_ratios.items():
    key = mode.replace("-", "_")
    expected = (
        f"{key}_over_coroutine_sysv_median_paired_ratio={ratio:.3f}\n"
    )
    if expected not in l4_summary:
        fail(f"L4 summary omits paired ratio: {expected.strip()}")
summary_by_key = {
    (row["case"], row["mode"]): row for row in summary_rows
}

validation = root / "validation.txt"
with validation.open("w", encoding="utf-8", newline="\n") as stream:
    def emit(key, value):
        stream.write(f"{key}={value}\n")

    emit("source_commit", source_commit)
    emit("compiler", "zig-0.16.0-clang-21")
    emit("filc_runtime_lifecycle", "PASS")
    emit("filc_high_concurrency_parser_bounds", "PASS")
    emit("zig_o0_focused_ctest", "16/16")
    emit("zig_o2_focused_ctest", "16/16")
    emit("zig_o3_full_ctest", "25/25")
    emit("zig_asan_ubsan_ctest", "22/22")
    emit("gcc_cacs_negative_gate", "PASS")
    emit("compile_command_profiles", "o0,o2,o3,asan")
    emit("compile_and_link_target_binding", "PASS")
    emit("context_samples", len(context_rows))
    emit("context_samples_per_mode", 5)
    emit("high_concurrency_schema", "rco-high-concurrency-v2")
    emit("high_concurrency_samples", len(sample_rows))
    emit("high_concurrency_summary_rows", len(summary_rows))
    emit("high_concurrency_cases", len(cases))
    emit("high_concurrency_runs_per_case_mode", 5)
    emit("backend_identity_binding", "PASS")
    emit("binary_sha256_binding", "PASS")
    emit("checksum_oracle", "PASS")
    emit("runtime_switch_oracle", "PASS")
    emit("task_lifecycle_oracle", "PASS")
    emit("stack_page_sentinels", "PASS")
    emit("end_barrier_yields_accounted", "PASS")
    emit("l4_forwarders", 4)
    emit("l4_modes", 5)
    emit("l4_five_mode_aggregate_rows", len(throughput_rows))
    emit("l4_five_mode_stream_rows", len(stream_rows))
    emit("l4_five_mode_resource_rows", len(resource_rows))
    emit("l4_five_mode_rotation_rows", len(rotation_rows))
    emit("l4_five_mode_positive_sender_receiver", "PASS")
    for mode in context_modes:
        emit(f"{mode.replace('-', '_')}_context_median_ns", f"{context_medians[mode]:.3f}")
    for mode in l4_modes:
        emit(f"l4_{mode.replace('-', '_')}_median_gbps", f"{l4_medians[mode]:.3f}")
    emit(
        "l4_cacs_paired_ratio_vs_sysv",
        f"{l4_paired_ratios['cacs']:.6f}",
    )
    emit(
        "l4_cacs_preserve_none_paired_ratio_vs_sysv",
        f"{l4_paired_ratios['cacs-preserve-none']:.6f}",
    )
    for case in ("tasks-16384", "working-set-256k"):
        for mode in context_modes:
            value = summary_by_key[(case, mode)][
                "median_cpu_ns_per_measured_yield"
            ]
            emit(
                f"{case.replace('-', '_')}_{mode.replace('-', '_')}_cpu_ns_per_measured_yield",
                f"{float(value):.2f}",
            )
    emit("disassembly_ir_gnu_stack", "PASS")
    emit("pmu_status", pmu_status)
    emit("promotion_clean_root", clean_root_relative)
    emit("promotion_generated_output", promotion_root_relative)
    emit("promotion_path_contract", "PASS")
PY

payload_count=$(find "$clean_root" -type f \
    ! -path "$clean_root/SHA256SUMS" -print |
    wc -l | awk '{print $1}')
{
    printf 'manifest_file=SHA256SUMS\n'
    printf 'manifest_algorithm=SHA-256\n'
    printf 'manifest_payload_files=%s\n' "$payload_count"
} >>"$clean_root/validation.txt"

generate_manifest "$clean_root"
verify_manifest "$clean_root"

# This is the first tracked-output mutation in the driver.
mkdir -p -- "$promotion_root"
rsync --archive --delete --checksum \
    "$clean_root/" "$promotion_root/"
verify_manifest "$promotion_root"

printf 'CACS evidence validated and promoted to %s\n' "$PROMOTION_ROOT_REL"
