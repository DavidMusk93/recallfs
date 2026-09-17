#!/usr/bin/env bash
set -euo pipefail

readonly ZIG_VERSION=0.16.0
readonly ZIG_ARCHIVE_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
readonly FILC_VERSION=0.684
readonly DEBIAN_IMAGE='debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171'
readonly FILC_LD='/usr/bin/x86_64-linux-gnu-ld.bfd'
readonly FILC_BFD='/lib/x86_64-linux-gnu/libbfd-2.31.1-system.so'

die() {
    printf 'validate_d2: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command is unavailable: $1"
}

validate_generated_root() {
    local label=$1
    local path=$2
    local canonical

    [[ "$path" = /* ]] || die "$label must be absolute: $path"
    [[ "$path" == */.tmp/* ]] || die "$label must be under .tmp: $path"
    [[ "$path" != *'/../'* && "$path" != *'/./'* ]] ||
        die "$label is not normalized: $path"
    canonical=$(realpath -m -- "$path")
    [[ "$canonical" == "$path" ]] || die "$label is not canonical: $path"
    [[ ! -L "$path" ]] || die "$label is a symlink: $path"
}

paths_overlap() {
    [[ "$1" == "$2" || "$1" == "$2/"* || "$2" == "$1/"* ]]
}

[[ $# -eq 1 ]] || die 'usage: validate_d2.sh <absolute-output-directory>'

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_root=$(cd -- "$script_dir/.." && pwd)
run_root=$(realpath -m -- "${source_root%/source}")
output_root=$1
work_root=${RZ_WORK_ROOT:-"${source_root%/source}/work"}
zig=${RZ_ZIG:-/root/recallfs/.tmp/zig/dist/zig}
zig_archive=${RZ_ZIG_ARCHIVE:-/root/recallfs/.tmp/zig/downloads/zig-x86_64-linux-0.16.0.tar.xz}
filc_root=${RZ_FILC_ROOT:-/root/recallfs/.tmp/fil-c}
benchmark_cpu=${RZ_BENCHMARK_CPU:-16}
benchmark_node=${RZ_BENCHMARK_NODE:-0}

for command in awk cat cmake cut date docker find gcc git grep head make ninja \
    nm numactl objdump perf perl realpath sed sha256sum sort uname xargs; do
    require_command "$command"
done
[[ "$(uname -s)" == Linux ]] || die 'this validation requires Linux'
[[ "$(uname -m)" == x86_64 ]] || die 'this validation requires x86_64'
validate_generated_root run_root "$run_root"
validate_generated_root output_root "$output_root"
validate_generated_root work_root "$work_root"
[[ "$output_root" == "$run_root/"* ]] ||
    die 'output root must be a strict descendant of the run root'
[[ "$work_root" == "$run_root/"* ]] ||
    die 'work root must be a strict descendant of the run root'
! paths_overlap "$output_root" "$work_root" ||
    die 'work and output roots overlap'
if paths_overlap "$source_root" "$output_root" ||
    paths_overlap "$source_root" "$work_root"; then
    die 'generated roots overlap maintained source'
fi

[[ -x "$zig" && ! -L "$zig" ]] || die "missing pinned Zig: $zig"
[[ "$("$zig" version)" == "$ZIG_VERSION" ]] ||
    die "unexpected Zig version at $zig"
[[ -f "$zig_archive" && ! -L "$zig_archive" ]] ||
    die "missing pinned Zig archive: $zig_archive"
[[ "$(sha256sum "$zig_archive" | awk '{print $1}')" == \
   "$ZIG_ARCHIVE_SHA256" ]] || die 'Zig archive digest mismatch'
[[ -x "$filc_root/bin/filcc" ]] || die 'FIL-C compiler is missing'
[[ -x "$filc_root/bin/filrun" ]] || die 'FIL-C runner is missing'
grep -Fxq "# Fil-C $FILC_VERSION" "$filc_root/dist/README.md" ||
    die 'unexpected FIL-C version'
[[ -f "$FILC_LD" && -f "$FILC_BFD" ]] ||
    die 'FIL-C linker adapter is unavailable'
docker image inspect "$DEBIAN_IMAGE" >/dev/null 2>&1 ||
    die 'pinned FIL-C container image is unavailable'

[[ ! -e "$work_root" ]] || die "work root already exists: $work_root"
[[ ! -e "$output_root" ]] || die "output root already exists: $output_root"
mkdir -m 700 -- "$work_root" "$output_root"

{
    printf 'date_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'uname='
    uname -a
    printf 'cpu=%s\n' "$benchmark_cpu"
    printf 'numa_node=%s\n' "$benchmark_node"
    printf 'cpu_online='
    cat /sys/devices/system/cpu/online
    printf 'cpufreq_driver='
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null ||
        printf 'unavailable\n'
    printf 'cpufreq_governor='
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null ||
        printf 'unavailable\n'
    printf 'perf_event_paranoid='
    cat /proc/sys/kernel/perf_event_paranoid
    lscpu
    numactl --hardware
    "$zig" version
    gcc --version | head -1
    cmake --version | head -1
    perf --version
} >"$output_root/environment.txt"

{
    sha256sum "$zig" "$zig_archive" "$filc_root/bin/filcc" \
        "$filc_root/bin/filrun" "$filc_root/dist/README.md"
} >"$output_root/tool-sha256.txt"

while IFS= read -r -d '' source_file; do
    sha256sum "$source_file"
done < <(
    find "$source_root/.clang-format" "$source_root/CMakeLists.txt" \
        "$source_root/include" "$source_root/scripts" "$source_root/src" \
        "$source_root/tests" "$source_root/tools" -type f -print0 |
        LC_ALL=C sort -z
) >"$output_root/source-sha256.txt"

docker run --rm --pull=never --network none \
    -v "$filc_root:$filc_root:ro" \
    -v "$source_root:/source:ro" \
    -v "$work_root:/work" \
    -v "$FILC_LD:/usr/bin/ld:ro" \
    -v "$FILC_BFD:$FILC_BFD:ro" \
    -w /source \
    "$DEBIAN_IMAGE" \
    sh -ceu "
        mkdir -p /work/filc
        '$filc_root/bin/filcc' --version
        '$filc_root/bin/filcc' \
            -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
            -Wshadow -Wstrict-prototypes -I include \
            src/hashing.c tests/test_hashing.c \
            -o /work/filc/ringzero_hashing_test
        '$filc_root/bin/filrun' /work/filc/ringzero_hashing_test
        '$filc_root/bin/filcc' \
            -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
            -Wshadow -Wstrict-prototypes -I include \
            src/hashing.c src/compare.c -lm \
            -o /work/filc/ringzero_hashing_compare
        '$filc_root/bin/filrun' /work/filc/ringzero_hashing_compare \
            --keys 10000 --lookup-keys 1000 --samples 3 \
            --backends 8 --table-size 1031 --vnodes 32 \
            --order-offset 0
        '$filc_root/bin/filcc' \
            -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
            -Wshadow -Wstrict-prototypes tools/udp_sequence.c \
            -o /work/filc/ringzero_udp_sequence
        '$filc_root/bin/filrun' /work/filc/ringzero_udp_sequence self-test
        '$filc_root/bin/filrun' /work/filc/ringzero_udp_sequence \
            send 127.0.0.1 9 1 0
    " >"$output_root/filc.txt" 2>&1

zig_build="$work_root/zig-build"
cmake -S "$source_root" -B "$zig_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$zig" \
    -DCMAKE_C_COMPILER_ARG1=cc \
    -DCMAKE_C_COMPILER_AR=/usr/bin/ar \
    -DCMAKE_C_COMPILER_RANLIB=/usr/bin/ranlib \
    -DCMAKE_C_FLAGS_RELEASE='-O3 -march=native -mtune=native -DNDEBUG -fno-omit-frame-pointer' \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    >"$output_root/zig-configure.txt" 2>&1
cmake --build "$zig_build" -j2 >"$output_root/zig-build.txt" 2>&1
ctest --test-dir "$zig_build" --output-on-failure \
    >"$output_root/zig-tests.txt" 2>&1
cp "$zig_build/compile_commands.json" \
    "$output_root/zig-compile-commands.json"

gcc_build="$work_root/gcc-build"
cmake -S "$source_root" -B "$gcc_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/gcc \
    -DCMAKE_C_FLAGS_RELEASE='-O3 -march=native -mtune=native -DNDEBUG -flto -fno-ipa-icf -fno-omit-frame-pointer' \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    >"$output_root/gcc-configure.txt" 2>&1
cmake --build "$gcc_build" -j2 >"$output_root/gcc-build.txt" 2>&1
ctest --test-dir "$gcc_build" --output-on-failure \
    >"$output_root/gcc-tests.txt" 2>&1
cp "$gcc_build/compile_commands.json" \
    "$output_root/gcc-compile-commands.json"

zig_benchmark="$zig_build/ringzero_hashing_compare"
gcc_benchmark="$gcc_build/ringzero_hashing_compare"
sha256sum "$zig_benchmark" "$gcc_benchmark" \
    "$zig_build/ringzero_hashing_test" \
    "$gcc_build/ringzero_hashing_test" \
    "$zig_build/ringzero_udp_sequence" \
    "$gcc_build/ringzero_udp_sequence" \
    >"$output_root/binary-sha256.txt"

for compiler in zig gcc; do
    if [[ "$compiler" == zig ]]; then
        binary=$zig_benchmark
    else
        binary=$gcc_benchmark
    fi
    for run in 0 1 2 3 4; do
        numactl --physcpubind="$benchmark_cpu" --membind="$benchmark_node" \
            "$binary" \
            --keys 1000000 \
            --lookup-keys 1000000 \
            --samples 11 \
            --backends 32 \
            --table-size 4099 \
            --vnodes 256 \
            --order-offset "$run" \
            >"$output_root/benchmark-$compiler-$run.csv"
    done
done

{
    printf '%s\n' \
        'compiler,algorithm,max_over_avg,min_over_avg,cv,add_churn,remove_middle_churn,median_lookup_ns,median_build_us,memory_bytes,checksum'
    for compiler in zig gcc; do
        for algorithm in modulo ring rendezvous jump maglev; do
            mapfile -t rows < <(
                awk -F, -v name="$algorithm" '$1 == name { print }' \
                    "$output_root"/benchmark-"$compiler"-*.csv
            )
            [[ "${#rows[@]}" -eq 5 ]] ||
                die "expected five $compiler/$algorithm benchmark rows"

            IFS=, read -r _ max min cv add remove _ _ memory checksum \
                <<<"${rows[0]}"
            expected="$max,$min,$cv,$add,$remove,$memory,$checksum"
            for row in "${rows[@]}"; do
                IFS=, read -r _ row_max row_min row_cv row_add row_remove \
                    _ _ row_memory row_checksum <<<"$row"
                actual="$row_max,$row_min,$row_cv,$row_add,$row_remove,"
                actual+="$row_memory,$row_checksum"
                [[ "$actual" == "$expected" ]] ||
                    die "semantic result mismatch for $compiler/$algorithm"
            done

            lookup_median=$(
                printf '%s\n' "${rows[@]}" |
                    cut -d, -f7 |
                    sort -n |
                    sed -n '3p'
            )
            build_median=$(
                printf '%s\n' "${rows[@]}" |
                    cut -d, -f8 |
                    sort -n |
                    sed -n '3p'
            )
            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$compiler" "$algorithm" "$max" "$min" "$cv" "$add" \
                "$remove" "$lookup_median" "$build_median" "$memory" \
                "$checksum"
        done
    done
} >"$output_root/benchmark-summary.csv"

for algorithm in modulo ring rendezvous jump maglev; do
    zig_semantic=$(
        awk -F, -v name="$algorithm" \
            '$1 == "zig" && $2 == name {
                print $3 "," $4 "," $5 "," $6 "," $7 "," $10 "," $11
            }' "$output_root/benchmark-summary.csv"
    )
    gcc_semantic=$(
        awk -F, -v name="$algorithm" \
            '$1 == "gcc" && $2 == name {
                print $3 "," $4 "," $5 "," $6 "," $7 "," $10 "," $11
            }' "$output_root/benchmark-summary.csv"
    )
    [[ -n "$zig_semantic" && "$zig_semantic" == "$gcc_semantic" ]] ||
        die "cross-compiler semantic mismatch for $algorithm"
done

for compiler in zig gcc; do
    if [[ "$compiler" == zig ]]; then
        binary=$zig_benchmark
    else
        binary=$gcc_benchmark
    fi

    small_begin=$(date +%s%N)
    numactl --physcpubind="$benchmark_cpu" --membind="$benchmark_node" \
        "$binary" \
        --keys 10000 --lookup-keys 1000000 --samples 3 \
        --backends 32 --table-size 4099 --vnodes 256 --order-offset 0 \
        >/dev/null
    small_end=$(date +%s%N)
    large_begin=$(date +%s%N)
    numactl --physcpubind="$benchmark_cpu" --membind="$benchmark_node" \
        "$binary" \
        --keys 10000 --lookup-keys 4000000 --samples 3 \
        --backends 32 --table-size 4099 --vnodes 256 --order-offset 0 \
        >/dev/null
    large_end=$(date +%s%N)
    small_elapsed=$((small_end - small_begin))
    large_elapsed=$((large_end - large_begin))
    {
        printf 'small_lookup_keys=1000000\n'
        printf 'small_elapsed_ns=%s\n' "$small_elapsed"
        printf 'large_lookup_keys=4000000\n'
        printf 'large_elapsed_ns=%s\n' "$large_elapsed"
    } >"$output_root/dce-scaling-$compiler.txt"
    ((large_elapsed > small_elapsed * 2)) ||
        die "$compiler 4x lookup work did not increase wall time by at least 2x"

    objdump -dr -Mintel "$binary" \
        >"$output_root/$compiler-disassembly.txt"
    nm -an "$binary" >"$output_root/$compiler-symbols.txt"
done

set +e
perf stat \
    -e cycles,instructions,branches,branch-misses,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses \
    -- numactl --physcpubind="$benchmark_cpu" --membind="$benchmark_node" \
    "$zig_benchmark" \
    --keys 100000 --lookup-keys 100000 --samples 3 \
    --backends 32 --table-size 4099 --vnodes 256 --order-offset 0 \
    >"$output_root/perf-hardware.stdout" \
    2>"$output_root/perf-hardware.stderr"
hardware_perf_status=$?
perf stat \
    -e task-clock,context-switches,cpu-migrations,page-faults \
    -- numactl --physcpubind="$benchmark_cpu" --membind="$benchmark_node" \
    "$zig_benchmark" \
    --keys 100000 --lookup-keys 100000 --samples 3 \
    --backends 32 --table-size 4099 --vnodes 256 --order-offset 0 \
    >"$output_root/perf-software.stdout" \
    2>"$output_root/perf-software.stderr"
software_perf_status=$?
set -e
{
    printf 'hardware_perf_status=%s\n' "$hardware_perf_status"
    printf 'software_perf_status=%s\n' "$software_perf_status"
    if [[ "$hardware_perf_status" -eq 0 ]] &&
        ! grep -Eq '<not supported>|<not counted>' \
            "$output_root/perf-hardware.stderr" &&
        grep -Eq '[0-9][0-9,]*[[:space:]]+cycles' \
            "$output_root/perf-hardware.stderr"; then
        printf 'hardware_pmu=available\n'
    else
        printf 'hardware_pmu=unavailable\n'
    fi
    if [[ "$software_perf_status" -eq 0 ]] &&
        ! grep -Eq '<not supported>|<not counted>' \
            "$output_root/perf-software.stderr" &&
        grep -Eq '[0-9][0-9,.]*[[:space:]]+msec task-clock' \
            "$output_root/perf-software.stderr"; then
        printf 'software_perf=available\n'
    else
        printf 'software_perf=unavailable\n'
    fi
} >"$output_root/perf-status.txt"

find "$output_root" -type f ! -name evidence-manifest.sha256 \
    -exec perl -pi -e 's/[ \t]+$//' {} +
find "$output_root" -type f ! -name evidence-manifest.sha256 \
    -exec perl -0777 -pi -e 's/\n+\z/\n/' {} +
(
    cd -- "$output_root"
    find . -type f ! -name evidence-manifest.sha256 -print0 |
        LC_ALL=C sort -z |
        xargs -0 sha256sum
) >"$output_root/evidence-manifest.sha256"

printf 'PASS: d2 C validation complete\n'
