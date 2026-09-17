#!/usr/bin/env bash
set -euo pipefail

readonly RINGZERO_REVISION=9a56125f02fcbce55448482d442d99729573ab06

die() {
    printf 'run_on_d2: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command is unavailable: $1"
}

[[ $# -le 2 ]] ||
    die 'usage: run_on_d2.sh [ssh-host] [remote-ringzero-root]'

host=${1:-d2}
remote_ringzero=${2:-}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(git -C "$source_root" rev-parse --show-toplevel)
source_relative=${source_root#"$repo_root/"}
[[ "$source_relative" != "$source_root" ]] ||
    die 'benchmark source is outside the repository'

remote_run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
remote_base=/root/recallfs/.tmp/ringzero-c-validation
remote_root="$remote_base/run-$remote_run_id"
remote_source="$remote_root/source"
remote_output="$remote_root/evidence"
remote_packet_output="$remote_root/packet-evidence"
local_output="$repo_root/learning/studies/20260917-ringzero/evidence/raw/d2-c"
local_packet_output="$repo_root/learning/studies/20260917-ringzero/evidence/raw/d2-ringzero"
local_stage_root="$repo_root/.tmp/ringzero-evidence-transfer-$$"
ssh_options=(
    -o BatchMode=yes
    -o ConnectTimeout=10
    -o ServerAliveInterval=15
    -o ServerAliveCountMax=4
)
ssh_transport='ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4'

for command in awk date find git grep mkdir mv realpath rm rsync shasum ssh tr wc; do
    require_command "$command"
done
[[ "$host" != -* && "$host" != *[[:space:]]* ]] ||
    die 'ssh host must not contain options or whitespace'
if [[ -n "$remote_ringzero" ]]; then
    [[ "$remote_ringzero" =~ ^/root/recallfs/\.tmp/[A-Za-z0-9._/-]+$ &&
       "$remote_ringzero" != *'/../'* &&
       "$remote_ringzero" != *'/./'* &&
       "$remote_ringzero" != *'/..' &&
       "$remote_ringzero" != *'/.' ]] ||
        die 'remote RingZero root must be normalized under /root/recallfs/.tmp'
    remote_ringzero=$(
        ssh "${ssh_options[@]}" "$host" "realpath -- '$remote_ringzero'"
    )
    [[ "$remote_ringzero" =~ ^/root/recallfs/\.tmp/[A-Za-z0-9._/-]+$ ]] ||
        die 'canonical RingZero root escaped /root/recallfs/.tmp'
fi
[[ ! -e "$local_stage_root" ]] ||
    die 'local staging root already exists'
mkdir -m 700 -- "$local_stage_root"
active_local=
active_backup=

cleanup_transfer() {
    if [[ -n "$active_backup" && -e "$active_backup" &&
          ! -e "$active_local" ]]; then
        mv -- "$active_backup" "$active_local" || true
    fi
    rm -rf -- "$local_stage_root"
}
trap cleanup_transfer EXIT

copy_evidence() {
    local label=$1
    local remote_path=$2
    local local_path=$3
    local stage="$local_stage_root/$label"
    local backup="$local_stage_root/$label-backup"
    local relative
    local expected_file_count
    local actual_file_count

    mkdir -p -- "$stage"
    rsync -a --delete --timeout=120 -e "$ssh_transport" \
        "$host:$remote_path/" "$stage/"
    [[ -z "$(find "$stage" -type l -print -quit)" ]] ||
        die "$label evidence contains a symlink"
    [[ -f "$stage/evidence-manifest.sha256" ]] ||
        die "$label evidence manifest is missing"
    (cd -- "$stage" && shasum -a 256 -c evidence-manifest.sha256) ||
        die "$label evidence digest verification failed"

    while IFS= read -r -d '' candidate; do
        relative=${candidate#"$stage/"}
        if [[ "$relative" != evidence-manifest.sha256 ]]; then
            awk -v path="./$relative" '
                $2 == path { found = 1 }
                END { exit !found }
            ' "$stage/evidence-manifest.sha256" ||
                die "$label evidence manifest omits $relative"
        fi
        case "$label:$relative" in
        c:benchmark-gcc-[0-4].csv | c:benchmark-zig-[0-4].csv | \
            c:benchmark-summary.csv | c:binary-sha256.txt | \
            c:dce-scaling-gcc.txt | c:dce-scaling-zig.txt | \
            c:environment.txt | c:evidence-manifest.sha256 | c:filc.txt | \
            c:gcc-build.txt | c:gcc-compile-commands.json | \
            c:gcc-configure.txt | c:gcc-disassembly.txt | \
            c:gcc-symbols.txt | c:gcc-tests.txt | \
            c:perf-hardware.stderr | c:perf-hardware.stdout | \
            c:perf-software.stderr | c:perf-software.stdout | \
            c:perf-status.txt | c:source-sha256.txt | c:tool-sha256.txt | \
            c:zig-build.txt | c:zig-compile-commands.json | \
            c:zig-configure.txt | c:zig-disassembly.txt | \
            c:zig-symbols.txt | c:zig-tests.txt | \
            packet:attach.txt | packet:backend-add.txt | \
            packet:backend-offloads.txt | packet:backend-xdp-pass.txt | \
            packet:bpftool-net.txt | packet:cleanup.txt | \
            packet:client-offloads.txt | packet:egress-offloads.txt | \
            packet:evidence-manifest.sha256 | packet:ingress-offloads.txt | \
            packet:input-manifest.txt | packet:list.txt | \
            packet:receiver.txt | packet:sender.txt | \
            packet:stats-after.txt | packet:stats-before.txt | \
            packet:vip-add.txt)
            ;;
        *)
            die "undeclared $label evidence path: $relative"
            ;;
        esac
    done < <(find "$stage" -type f -print0)

    if [[ "$label" == c ]]; then
        expected_file_count=36
    else
        expected_file_count=17
    fi
    actual_file_count=$(find "$stage" -type f | wc -l | tr -d '[:space:]')
    [[ "$actual_file_count" == "$expected_file_count" ]] ||
        die "$label evidence file count is $actual_file_count, expected $expected_file_count"

    if [[ "$label" == c ]]; then
        verify_source_manifest "$stage/source-sha256.txt"
    elif [[ "$label" == packet ]]; then
        verify_packet_manifest "$stage/input-manifest.txt"
    fi

    mkdir -p -- "$(dirname -- "$local_path")"
    if [[ -e "$local_path" ]]; then
        active_local=$local_path
        active_backup=$backup
        mv -- "$local_path" "$backup"
    fi
    if ! mv -- "$stage" "$local_path"; then
        [[ ! -e "$backup" ]] || mv -- "$backup" "$local_path"
        die "$label evidence promotion failed"
    fi
    rm -rf -- "$backup"
    active_local=
    active_backup=
}

verify_source_manifest() {
    local manifest=$1
    local digest
    local remote_path
    local relative
    local actual

    while read -r digest remote_path; do
        relative=${remote_path#"$remote_source/"}
        [[ "$relative" != "$remote_path" && "$relative" != *'..'* &&
           -f "$source_root/$relative" ]] ||
            die "invalid source manifest path: $remote_path"
        actual=$(shasum -a 256 "$source_root/$relative" | awk '{print $1}')
        [[ "$actual" == "$digest" ]] ||
            die "source changed during validation: $relative"
    done <"$manifest"
}

manifest_digest() {
    local manifest=$1
    local suffix=$2
    local matches

    matches=$(
        awk -v suffix="$suffix" '
            length($2) >= length(suffix) &&
            substr($2, length($2) - length(suffix) + 1) == suffix {
                print $1
            }
        ' "$manifest"
    )
    [[ -n "$matches" && "$matches" != *$'\n'* ]] ||
        die "expected one digest for $suffix in $manifest"
    printf '%s\n' "$matches"
}

verify_packet_manifest() {
    local manifest=$1
    local ringzero_build_manifest="$repo_root/learning/studies/20260917-ringzero/evidence/raw/d2-ringzero-build/binary-sha256.txt"
    local c_build_manifest="$local_output/binary-sha256.txt"
    local suffix
    local expected
    local actual

    grep -Fxq "upstream_revision=$RINGZERO_REVISION" "$manifest" ||
        die 'packet evidence has the wrong upstream revision'
    for suffix in zig-out/bin/ringzero bpf/xdp_lb.o bpf/xdp_pass.o; do
        expected=$(manifest_digest "$ringzero_build_manifest" "$suffix")
        actual=$(manifest_digest "$manifest" "$suffix")
        [[ "$actual" == "$expected" ]] ||
            die "packet binary digest mismatch for $suffix"
    done
    expected=$(manifest_digest "$c_build_manifest" \
        zig-build/ringzero_udp_sequence)
    actual=$(manifest_digest "$manifest" \
        zig-build/ringzero_udp_sequence)
    [[ "$actual" == "$expected" ]] ||
        die 'packet sequence binary does not match this C validation run'
    expected=$(
        shasum -a 256 \
            "$source_root/scripts/validate_ringzero_netns.sh" |
            awk '{print $1}'
    )
    actual=$(manifest_digest "$manifest" \
        source/scripts/validate_ringzero_netns.sh)
    [[ "$actual" == "$expected" ]] ||
        die 'packet validation script does not match this source tree'
}

ssh "${ssh_options[@]}" "$host" "
    set -eu
    test ! -L '$remote_base'
    install -d -m 700 '$remote_base'
    mkdir -m 700 '$remote_root'
    install -d -m 700 '$remote_source'
"
rsync -a --delete --timeout=120 -e "$ssh_transport" \
    "$source_root/" "$host:$remote_source/"

ssh "${ssh_options[@]}" "$host" "
    set -eu
    timeout 20m env RZ_WORK_ROOT='$remote_root/work' \
        bash '$remote_source/scripts/validate_d2.sh' '$remote_output'
"

copy_evidence c "$remote_output" "$local_output"

printf 'd2 evidence copied to %s\n' "$local_output"

if [[ -n "$remote_ringzero" ]]; then
    ssh "${ssh_options[@]}" "$host" "
        set -eu
        timeout 5m bash '$remote_source/scripts/validate_ringzero_netns.sh' \
            '$remote_ringzero' \
            '$remote_root/work/zig-build/ringzero_udp_sequence' \
            '$RINGZERO_REVISION' \
            '$remote_packet_output'
    "
    copy_evidence packet "$remote_packet_output" "$local_packet_output"
    printf 'RingZero packet evidence copied to %s\n' "$local_packet_output"
fi

ssh "${ssh_options[@]}" "$host" "
    set -eu
    test ! -L '$remote_root'
    rm -rf '$remote_root'
"
trap - EXIT
rm -rf -- "$local_stage_root"
