#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <forwarder-binary> <output-directory>" >&2
    exit 2
fi

forwarder=$1
output_dir=$2
runs=${RUNS:-5}
duration=${DURATION:-3}
parallel=${PARALLEL:-4}
base_port=${BASE_PORT:-43000}
proxy_cpu=${PROXY_CPU:-0}
server_cpu=${SERVER_CPU:-1}
client_cpu=${CLIENT_CPU:-2}
numa_node=${NUMA_NODE:-0}

mkdir -p "$output_dir"
csv="$output_dir/throughput.csv"
stream_csv="$output_dir/stream-throughput.csv"
printf 'mode,run,bits_per_second\n' >"$csv"
printf 'mode,run,stream,sender_socket,sender_bytes,sender_bits_per_second,receiver_socket,receiver_bytes,receiver_bits_per_second\n' \
    >"$stream_csv"

server_pid=
proxy_pid=
cleanup() {
    if [[ -n "$proxy_pid" ]]; then
        kill -TERM "$proxy_pid" 2>/dev/null || true
        wait "$proxy_pid" 2>/dev/null || true
        proxy_pid=
    fi
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}
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
    printf 'forwarder_sha256='
    sha256sum "$forwarder" | awk '{print $1}'
    printf 'runs=%s duration=%s parallel=%s\n' "$runs" "$duration" "$parallel"
    printf 'proxy_cpu=%s server_cpu=%s client_cpu=%s numa_node=%s\n' \
        "$proxy_cpu" "$server_cpu" "$client_cpu" "$numa_node"
    printf 'perf_event_paranoid='
    cat /proc/sys/kernel/perf_event_paranoid
    capture_cpu_frequency_policy
} >"$output_dir/environment.txt" 2>&1

run_sample() {
    local mode=$1
    local run=$2
    local backend_port=$((base_port + run * 4))
    local proxy_port=$((backend_port + 1))
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
    if [[ "$mode" == proxy ]]; then
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
        -t "$duration" -O 1 --json >"$client_json"
    wait "$server_pid"
    server_pid=

    if [[ "$mode" == proxy ]]; then
        kill -TERM "$proxy_pid"
        wait "$proxy_pid"
        proxy_pid=
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

for mode in direct proxy; do
    for run in $(seq 1 "$runs"); do
        run_sample "$mode" "$run"
    done
done

python3 - "$csv" <<'PY'
import csv
import statistics
import sys

samples = {}
with open(sys.argv[1], newline="") as source:
    for row in csv.DictReader(source):
        samples.setdefault(row["mode"], []).append(float(row["bits_per_second"]))

for mode in ("direct", "proxy"):
    values = samples[mode]
    median = statistics.median(values)
    print("{}_median_gbps={:.3f}".format(mode, median / 1e9))

direct = statistics.median(samples["direct"])
proxy = statistics.median(samples["proxy"])
print("proxy_over_direct={:.3f}".format(proxy / direct))
PY

trap - EXIT
