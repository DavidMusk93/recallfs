#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1090
source "$script_dir/process_resources.sh"

server_pid=
proxy_pid=
client_pid=
wait_timed_out=false
child_poll_interval=${CHILD_POLL_INTERVAL_SECONDS:-0.02}
kill_wait_timeout_seconds=${KILL_WAIT_TIMEOUT_SECONDS:-1}
cleanup_term_timeout_seconds=${CLEANUP_TERM_TIMEOUT_SECONDS:-2}
benchmark_modes=()

print_usage() {
    printf 'usage: %s <coroutine-forwarder> <epoll-forwarder> <output-directory>\n' \
        "$0" >&2
    printf '       %s <sysv-coroutine> <epoll> <output-dir> <cacs> <cacs-preserve-none>\n' \
        "$0" >&2
}

configure_benchmark() {
    case $# in
        3)
            benchmark_modes=(direct coroutine epoll)
            ;;
        5)
            benchmark_modes=(
                direct
                coroutine-sysv
                cacs
                cacs-preserve-none
                epoll
            )
            ;;
        *)
            print_usage
            return 2
            ;;
    esac

    coroutine_forwarder=$1
    epoll_forwarder=$2
    output_dir=$3
    cacs_forwarder=
    cacs_preserve_none_forwarder=
    if (( $# == 5 )); then
        cacs_forwarder=$4
        cacs_preserve_none_forwarder=$5
    fi
}

forwarder_for_mode() {
    local mode=$1

    case "$mode" in
        coroutine|coroutine-sysv) printf '%s\n' "$coroutine_forwarder" ;;
        cacs) printf '%s\n' "$cacs_forwarder" ;;
        cacs-preserve-none) printf '%s\n' "$cacs_preserve_none_forwarder" ;;
        epoll) printf '%s\n' "$epoll_forwarder" ;;
        *)
            printf 'benchmark mode has no forwarder: %s\n' "$mode" >&2
            return 1
            ;;
    esac
}

mode_index() {
    local mode=$1
    local index

    for index in "${!benchmark_modes[@]}"; do
        if [[ "${benchmark_modes[$index]}" == "$mode" ]]; then
            printf '%s\n' "$index"
            return 0
        fi
    done
    printf 'unknown benchmark mode: %s\n' "$mode" >&2
    return 1
}

ports_for_sample() {
    local mode=$1
    local run=$2
    local index
    local backend_port

    index=$(mode_index "$mode") || return
    backend_port=$((base_port + run * 10 + index * 2))
    printf '%s %s\n' "$backend_port" "$((backend_port + 1))"
}

write_binary_hashes() {
    local digest
    local forwarder
    local key
    local mode

    for mode in "${benchmark_modes[@]}"; do
        if [[ "$mode" == direct ]]; then
            continue
        fi
        forwarder=$(forwarder_for_mode "$mode") || return
        digest=$(sha256sum "$forwarder" | awk '{print $1}')
        key=${mode//-/_}_forwarder_sha256
        printf '%s=%s\n' "$key" "$digest"
    done
}

summarize_throughput() {
    local throughput_csv=$1

    python3 - "$throughput_csv" "${benchmark_modes[@]}" <<'PY'
import csv
import statistics
import sys

samples = {}
with open(sys.argv[1], newline="") as source:
    for row in csv.DictReader(source):
        samples.setdefault(row["mode"], []).append(float(row["bits_per_second"]))

modes = sys.argv[2:]
medians = {mode: statistics.median(samples[mode]) for mode in modes}

def key(mode):
    return mode.replace("-", "_")

for mode in modes:
    print("{}_median_gbps={:.3f}".format(key(mode), medians[mode] / 1e9))

direct = medians["direct"]
for mode in modes[1:]:
    print("{}_over_direct={:.3f}".format(key(mode), medians[mode] / direct))

sysv_mode = modes[1]
print("{}_over_epoll={:.3f}".format(
    key(sysv_mode), medians[sysv_mode] / medians["epoll"]))
if "cacs" in medians:
    print("cacs_over_{}={:.3f}".format(
        key(sysv_mode), medians["cacs"] / medians[sysv_mode]))
    print("cacs_preserve_none_over_{}={:.3f}".format(
        key(sysv_mode),
        medians["cacs-preserve-none"] / medians[sysv_mode]))
PY
}

child_is_running() {
    local pid=$1
    local stat
    local stat_tail

    if ! kill -0 "$pid" 2>/dev/null; then
        return 1
    fi
    if [[ -r "/proc/$pid/stat" ]] && IFS= read -r stat <"/proc/$pid/stat"; then
        stat_tail=${stat##*) }
        if [[ ${stat_tail:0:1} == Z ]]; then
            return 1
        fi
    fi
    return 0
}

wait_for_child_bounded() {
    local pid=$1
    local timeout_seconds=$2
    local deadline=$((SECONDS + timeout_seconds))
    local status

    wait_timed_out=false
    while child_is_running "$pid"; do
        if ((SECONDS >= deadline)); then
            wait_timed_out=true
            return 124
        fi
        sleep "$child_poll_interval"
    done

    if wait "$pid"; then
        return 0
    else
        status=$?
        return "$status"
    fi
}

terminate_and_reap_child() {
    local pid=$1
    local timeout_seconds=$2
    local label=$3
    local status

    kill -TERM "$pid" 2>/dev/null || true
    if wait_for_child_bounded "$pid" "$timeout_seconds"; then
        return 0
    else
        status=$?
    fi
    if [[ "$wait_timed_out" == false ]]; then
        return "$status"
    fi

    printf '%s pid %s did not exit after SIGTERM within %ss; sending SIGKILL\n' \
        "$label" "$pid" "$timeout_seconds" >&2
    kill -KILL "$pid" 2>/dev/null || true
    if wait_for_child_bounded "$pid" "$kill_wait_timeout_seconds"; then
        :
    elif [[ "$wait_timed_out" == true ]]; then
        printf '%s pid %s remained alive after SIGKILL\n' "$label" "$pid" >&2
    fi
    return 124
}

run_child_bounded() {
    local pid=$1
    local timeout_seconds=$2
    local label=$3
    local status

    if wait_for_child_bounded "$pid" "$timeout_seconds"; then
        return 0
    else
        status=$?
    fi
    if [[ "$wait_timed_out" == false ]]; then
        return "$status"
    fi

    printf '%s pid %s timed out after %ss\n' \
        "$label" "$pid" "$timeout_seconds" >&2
    terminate_and_reap_child "$pid" "$cleanup_term_timeout_seconds" "$label" \
        || true
    return 124
}

cleanup() {
    local pid
    local pid_name

    for pid_name in client_pid proxy_pid server_pid; do
        pid=${!pid_name}
        if [[ -n "$pid" ]]; then
            terminate_and_reap_child \
                "$pid" "$cleanup_term_timeout_seconds" "$pid_name" || true
            printf -v "$pid_name" '%s' ''
        fi
    done
}

validate_forwarder_summary() {
    local proxy_log=$1
    local summaries=()

    mapfile -t summaries < <(grep '^accepted=' "$proxy_log" || true)
    if (( ${#summaries[@]} != 1 )); then
        printf 'expected exactly one forwarder summary in %s, found %s\n' \
            "$proxy_log" "${#summaries[@]}" >&2
        return 1
    fi
    if [[ " ${summaries[0]} " != *" forced_shutdown=false "* ]]; then
        printf 'forwarder summary did not report forced_shutdown=false: %s\n' \
            "${summaries[0]}" >&2
        return 1
    fi
}

mode_order_for_run() {
    local run=$1
    local count=${#benchmark_modes[@]}
    local index
    local start=$(((run - 1) % count))

    for ((index = 0; index < count; index += 1)); do
        printf '%s\n' "${benchmark_modes[$(((start + index) % count))]}"
    done
}

run_all_samples() {
    local mode
    local position
    local run

    printf 'run,position,mode\n' >"$order_csv"
    for run in $(seq 1 "$runs"); do
        position=0
        while IFS= read -r mode; do
            ((position += 1))
            printf '%s,%s,%s\n' "$run" "$position" "$mode" >>"$order_csv"
            run_sample "$mode" "$run"
        done < <(mode_order_for_run "$run")
    done
}

if [[ ${L4_BENCH_SOURCE_ONLY:-0} == 1 ]]; then
    return 0 2>/dev/null || exit 0
fi

configure_benchmark "$@" || exit $?
runs=${RUNS:-5}
duration=${DURATION:-3}
parallel=${PARALLEL:-4}
base_port=${BASE_PORT:-43000}
proxy_cpu=${PROXY_CPU:-0}
server_cpu=${SERVER_CPU:-1}
client_cpu=${CLIENT_CPU:-2}
numa_node=${NUMA_NODE:-0}
client_timeout_seconds=${CLIENT_TIMEOUT_SECONDS:-$((duration + 10))}
server_exit_timeout_seconds=${SERVER_EXIT_TIMEOUT_SECONDS:-5}
proxy_term_timeout_seconds=${PROXY_TERM_TIMEOUT_SECONDS:-5}

mkdir -p "$output_dir"
csv="$output_dir/throughput.csv"
stream_csv="$output_dir/stream-throughput.csv"
resource_csv="$output_dir/process-resources.csv"
order_csv="$output_dir/run-order.csv"
printf 'mode,run,bits_per_second\n' >"$csv"
printf 'mode,run,stream,sender_socket,sender_bytes,sender_bits_per_second,receiver_socket,receiver_bytes,receiver_bits_per_second\n' \
    >"$stream_csv"
printf 'mode,run,vm_peak_kb,vm_hwm_kb,user_ticks,system_ticks,voluntary_context_switches,nonvoluntary_context_switches\n' \
    >"$resource_csv"
trap cleanup EXIT

wait_for_listener() {
    local pid=$1
    local port=$2

    for _ in $(seq 1 200); do
        if ss -H -ltn "sport = :$port" | grep -q .; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.02
    done
    return 1
}

capture_sysfs_value() {
    local label=$1
    local path=$2
    local value

    if [[ ! -e "$path" ]]; then
        printf '%s=unavailable: %s is not exposed\n' "$label" "$path"
    elif [[ ! -r "$path" ]]; then
        printf '%s=unavailable: %s is not readable\n' "$label" "$path"
    elif value=$(cat "$path" 2>/dev/null); then
        printf '%s=%s\n' "$label" "$value"
    else
        printf '%s=unavailable: failed to read %s\n' "$label" "$path"
    fi
}

capture_cpu_frequency_policy() {
    local cpufreq_root=/sys/devices/system/cpu/cpufreq
    local policies=("$cpufreq_root"/policy*)
    local field
    local policy
    local policy_name
    local turbo_path
    local turbo_raw

    if [[ ! -d "${policies[0]}" ]]; then
        for field in scaling_driver scaling_governor scaling_min_freq_khz scaling_max_freq_khz; do
            printf 'cpufreq.%s=unavailable: no policy directories under %s; kernel/hypervisor does not expose cpufreq\n' \
                "$field" "$cpufreq_root"
        done
    else
        for policy in "${policies[@]}"; do
            policy_name=${policy##*/}
            capture_sysfs_value "cpufreq.${policy_name}.scaling_driver" \
                "$policy/scaling_driver"
            capture_sysfs_value "cpufreq.${policy_name}.scaling_governor" \
                "$policy/scaling_governor"
            capture_sysfs_value "cpufreq.${policy_name}.scaling_min_freq_khz" \
                "$policy/scaling_min_freq"
            capture_sysfs_value "cpufreq.${policy_name}.scaling_max_freq_khz" \
                "$policy/scaling_max_freq"
        done
    fi

    if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        turbo_path=/sys/devices/system/cpu/intel_pstate/no_turbo
        if [[ -r "$turbo_path" ]] && turbo_raw=$(cat "$turbo_path" 2>/dev/null); then
            case "$turbo_raw" in
                0) printf 'cpufreq.turbo=enabled (%s=0)\n' "$turbo_path" ;;
                1) printf 'cpufreq.turbo=disabled (%s=1)\n' "$turbo_path" ;;
                *) printf 'cpufreq.turbo=unknown (%s=%s)\n' "$turbo_path" "$turbo_raw" ;;
            esac
        else
            printf 'cpufreq.turbo=unavailable: %s is not readable\n' "$turbo_path"
        fi
    elif [[ -e "$cpufreq_root/boost" ]]; then
        turbo_path="$cpufreq_root/boost"
        if [[ -r "$turbo_path" ]] && turbo_raw=$(cat "$turbo_path" 2>/dev/null); then
            case "$turbo_raw" in
                0) printf 'cpufreq.turbo=disabled (%s=0)\n' "$turbo_path" ;;
                1) printf 'cpufreq.turbo=enabled (%s=1)\n' "$turbo_path" ;;
                *) printf 'cpufreq.turbo=unknown (%s=%s)\n' "$turbo_path" "$turbo_raw" ;;
            esac
        else
            printf 'cpufreq.turbo=unavailable: %s is not readable\n' "$turbo_path"
        fi
    else
        printf 'cpufreq.turbo=unavailable: neither intel_pstate/no_turbo nor cpufreq/boost is exposed by the kernel/hypervisor\n'
    fi
}

{
    uname -a
    lscpu
    gcc --version
    perf --version
    numactl --show
    write_binary_hashes
    printf 'runs=%s duration=%s parallel=%s\n' "$runs" "$duration" "$parallel"
    printf 'proxy_cpu=%s server_cpu=%s client_cpu=%s numa_node=%s\n' \
        "$proxy_cpu" "$server_cpu" "$client_cpu" "$numa_node"
    printf 'rlimit_nofile_soft=%s\n' "$(ulimit -Sn)"
    printf 'rlimit_nofile_hard=%s\n' "$(ulimit -Hn)"
    printf 'perf_event_paranoid='
    cat /proc/sys/kernel/perf_event_paranoid
    capture_cpu_frequency_policy
} >"$output_dir/environment.txt" 2>&1

run_sample() {
    local mode=$1
    local run=$2
    local backend_port
    local port_pair
    local proxy_port
    port_pair=$(ports_for_sample "$mode" "$run") || return
    read -r backend_port proxy_port <<<"$port_pair"
    local server_json="$output_dir/${mode}-${run}-server.json"
    local client_json="$output_dir/${mode}-${run}-client.json"
    local proxy_log="$output_dir/${mode}-${run}-forwarder.log"

    numactl --physcpubind="$server_cpu" --membind="$numa_node" \
        iperf3 -s -1 -p "$backend_port" --json >"$server_json" &
    server_pid=$!
    if ! wait_for_listener "$server_pid" "$backend_port"; then
        echo "iperf3 server did not listen on port $backend_port" >&2
        exit 1
    fi

    local target_port=$backend_port
    if [[ "$mode" != direct ]]; then
        local forwarder
        forwarder=$(forwarder_for_mode "$mode") || return
        numactl --physcpubind="$proxy_cpu" --membind="$numa_node" \
            "$forwarder" \
            --listen-host 127.0.0.1 \
            --listen-port "$proxy_port" \
            --upstream-host 127.0.0.1 \
            --upstream-port "$backend_port" \
            --max-connections 128 \
            --buffer-size 65536 \
            --connect-timeout-ms 1000 \
            --grace-ms 1000 >"$proxy_log" 2>&1 &
        proxy_pid=$!
        for _ in $(seq 1 100); do
            if grep -q '^listening=' "$proxy_log"; then
                break
            fi
            if ! kill -0 "$proxy_pid" 2>/dev/null; then
                cat "$proxy_log" >&2
                exit 1
            fi
            sleep 0.02
        done
        grep -q '^listening=' "$proxy_log"
        target_port=$proxy_port
    fi

    numactl --physcpubind="$client_cpu" --membind="$numa_node" \
        iperf3 -c 127.0.0.1 -p "$target_port" -P "$parallel" \
        -t "$duration" -O 1 --json >"$client_json" &
    client_pid=$!
    local child_status
    if run_child_bounded \
        "$client_pid" "$client_timeout_seconds" "iperf3 client"; then
        child_status=0
    else
        child_status=$?
    fi
    client_pid=
    if ((child_status != 0)); then
        printf 'iperf3 client exited with status %s for %s run %s\n' \
            "$child_status" "$mode" "$run" >&2
        return 1
    fi

    if run_child_bounded \
        "$server_pid" "$server_exit_timeout_seconds" "iperf3 server"; then
        child_status=0
    else
        child_status=$?
    fi
    server_pid=
    if ((child_status != 0)); then
        printf 'iperf3 server exited with status %s for %s run %s\n' \
            "$child_status" "$mode" "$run" >&2
        return 1
    fi

    if [[ "$mode" != direct ]]; then
        if ! kill -0 "$proxy_pid" 2>/dev/null; then
            cat "$proxy_log" >&2
            exit 1
        fi
        local vm_peak_kb
        local vm_hwm_kb
        local user_ticks
        local system_ticks
        local voluntary_switches
        local nonvoluntary_switches
        IFS=, read -r vm_peak_kb vm_hwm_kb user_ticks system_ticks \
            voluntary_switches nonvoluntary_switches \
            < <(read_process_resources "$proxy_pid")
        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$mode" "$run" "$vm_peak_kb" "$vm_hwm_kb" \
            "$user_ticks" "$system_ticks" "$voluntary_switches" \
            "$nonvoluntary_switches" >>"$resource_csv"
        if terminate_and_reap_child \
            "$proxy_pid" "$proxy_term_timeout_seconds" "forwarder"; then
            child_status=0
        else
            child_status=$?
        fi
        proxy_pid=
        if ((child_status != 0)); then
            printf 'forwarder exited with status %s for %s run %s\n' \
                "$child_status" "$mode" "$run" >&2
            cat "$proxy_log" >&2
            return 1
        fi
        if ! validate_forwarder_summary "$proxy_log"; then
            cat "$proxy_log" >&2
            return 1
        fi
    fi

    local error
    error=$(jq -r '.error // empty' "$client_json")
    if [[ -n "$error" ]]; then
        echo "iperf3 failed: $error" >&2
        exit 1
    fi
    if ! jq -e --argjson expected "$parallel" '
        (.end.streams | type == "array" and length == $expected) and
        all(.end.streams[];
            (.sender.bytes | type == "number") and
            (.sender.bytes > 0) and
            (.sender.bits_per_second | type == "number") and
            (.sender.bits_per_second > 0) and
            (.receiver.bytes | type == "number") and
            (.receiver.bytes > 0) and
            (.receiver.bits_per_second | type == "number") and
            (.receiver.bits_per_second > 0)
        )
    ' "$client_json" >/dev/null; then
        printf 'iperf3 invalid stream results for %s run %s: expected exactly %s streams with positive sender/receiver bytes and bits_per_second\n' \
            "$mode" "$run" "$parallel" >&2
        exit 1
    fi
    local throughput
    throughput=$(jq -r '.end.sum_sent.bits_per_second' "$client_json")
    printf '%s,%s,%s\n' "$mode" "$run" "$throughput" >>"$csv"
    jq -r --arg mode "$mode" --arg run "$run" '
        .end.streams
        | to_entries[]
        | [
            $mode,
            $run,
            (.key + 1),
            .value.sender.socket,
            .value.sender.bytes,
            .value.sender.bits_per_second,
            .value.receiver.socket,
            .value.receiver.bytes,
            .value.receiver.bits_per_second
        ]
        | @csv
    ' "$client_json" >>"$stream_csv"
}

run_all_samples

summarize_throughput "$csv"

trap - EXIT
