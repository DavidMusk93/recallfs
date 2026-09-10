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
printf 'mode,run,bits_per_second\n' >"$csv"

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
    sleep 0.25

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
    local throughput
    throughput=$(jq -r '.end.sum_sent.bits_per_second' "$client_json")
    printf '%s,%s,%s\n' "$mode" "$run" "$throughput" >>"$csv"
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
