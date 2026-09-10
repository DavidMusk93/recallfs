#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1090
L4_BENCH_SOURCE_ONLY=1 source "$script_dir/../bench/l4_bench.sh"
coroutine_forwarder=
epoll_forwarder=
output_dir=
cacs_forwarder=
cacs_preserve_none_forwarder=
benchmark_modes=()

fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rco-l4-bench-fixture.XXXXXX")
test_pids=()
test_cleanup() {
    local pid

    for pid in "${test_pids[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$fixture_root"
}
trap test_cleanup EXIT

fail() {
    echo "$*" >&2
    exit 1
}

assert_file_equals() {
    local expected=$1
    local actual=$2

    if ! diff -u "$expected" "$actual"; then
        fail "unexpected contents in $actual"
    fi
}

assert_equals() {
    local expected=$1
    local actual=$2
    local label=$3

    if [[ "$actual" != "$expected" ]]; then
        fail "$label: expected '$expected', got '$actual'"
    fi
}

assert_dead() {
    local pid=$1

    if kill -0 "$pid" 2>/dev/null; then
        fail "pid $pid is still alive"
    fi
}

forget_pid() {
    local pid=$1
    local retained=()
    local tracked_pid

    for tracked_pid in "${test_pids[@]}"; do
        if [[ "$tracked_pid" != "$pid" ]]; then
            retained+=("$tracked_pid")
        fi
    done
    test_pids=("${retained[@]}")
}

start_term_ignoring_child() {
    local ready_file=$1

    bash -c 'trap "" TERM; : >"$1"; exec sleep 30' _ "$ready_file" &
    started_pid=$!
    test_pids+=("$started_pid")
    for _ in $(seq 1 100); do
        if [[ -e "$ready_file" ]]; then
            return 0
        fi
        sleep 0.01
    done
    fail "TERM-ignoring child did not become ready"
}

configure_benchmark legacy-sysv legacy-epoll legacy-output
assert_equals legacy-sysv "$coroutine_forwarder" "legacy coroutine binary"
assert_equals legacy-epoll "$epoll_forwarder" "legacy epoll binary"
assert_equals legacy-output "$output_dir" "legacy output directory"
printf '%s\n' "${benchmark_modes[@]}" >"$fixture_root/legacy-modes"
printf '%s\n' direct coroutine epoll >"$fixture_root/expected-legacy-modes"
assert_file_equals \
    "$fixture_root/expected-legacy-modes" "$fixture_root/legacy-modes"
assert_equals legacy-sysv "$(forwarder_for_mode coroutine)" \
    "legacy coroutine mode"
assert_equals legacy-epoll "$(forwarder_for_mode epoll)" "legacy epoll mode"

for name in sysv epoll cacs cacs-preserve-none; do
    printf '%s\n' "$name-binary" >"$fixture_root/$name"
done
configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/legacy-output"
write_binary_hashes >"$fixture_root/legacy-hashes"
{
    printf 'coroutine_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/sysv" | awk '{print $1}')"
    printf 'epoll_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/epoll" | awk '{print $1}')"
} >"$fixture_root/expected-legacy-hashes"
assert_file_equals \
    "$fixture_root/expected-legacy-hashes" "$fixture_root/legacy-hashes"

configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/five-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
assert_equals "$fixture_root/sysv" "$coroutine_forwarder" \
    "five-mode SysV binary"
assert_equals "$fixture_root/cacs" "$cacs_forwarder" \
    "five-mode CACS binary"
assert_equals "$fixture_root/cacs-preserve-none" \
    "$cacs_preserve_none_forwarder" "five-mode CACS+PN binary"
printf '%s\n' "${benchmark_modes[@]}" >"$fixture_root/five-modes"
printf '%s\n' \
    direct coroutine-sysv cacs cacs-preserve-none epoll \
    >"$fixture_root/expected-five-modes"
assert_file_equals "$fixture_root/expected-five-modes" "$fixture_root/five-modes"
assert_equals "$fixture_root/sysv" "$(forwarder_for_mode coroutine-sysv)" \
    "five-mode SysV mode"
assert_equals "$fixture_root/cacs" "$(forwarder_for_mode cacs)" \
    "five-mode CACS mode"
assert_equals "$fixture_root/cacs-preserve-none" \
    "$(forwarder_for_mode cacs-preserve-none)" "five-mode CACS+PN mode"
assert_equals "$fixture_root/epoll" "$(forwarder_for_mode epoll)" \
    "five-mode epoll mode"

if configure_benchmark one two three four 2>"$fixture_root/usage.log"; then
    fail "four-argument CLI was accepted"
else
    configure_status=$?
fi
assert_equals 2 "$configure_status" "invalid CLI status"
grep -Fq \
    '<coroutine-forwarder> <epoll-forwarder> <output-directory>' \
    "$fixture_root/usage.log" \
    || fail "usage omitted the legacy three-argument form"
grep -Fq \
    '<sysv-coroutine> <epoll> <output-dir> <cacs> <cacs-preserve-none>' \
    "$fixture_root/usage.log" \
    || fail "usage omitted the five-argument form"

write_binary_hashes >"$fixture_root/five-hashes"
{
    printf 'coroutine_sysv_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/sysv" | awk '{print $1}')"
    printf 'cacs_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/cacs" | awk '{print $1}')"
    printf 'cacs_preserve_none_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/cacs-preserve-none" | awk '{print $1}')"
    printf 'epoll_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/epoll" | awk '{print $1}')"
} >"$fixture_root/expected-five-hashes"
assert_file_equals \
    "$fixture_root/expected-five-hashes" "$fixture_root/five-hashes"

# shellcheck disable=SC2034
base_port=43000
for mode in "${benchmark_modes[@]}"; do
    read -r backend_port proxy_port < <(ports_for_sample "$mode" 1)
    printf '%s,%s,%s\n' "$mode" "$backend_port" "$proxy_port"
done >"$fixture_root/five-ports"
cat >"$fixture_root/expected-five-ports" <<'EOF'
direct,43010,43011
coroutine-sysv,43012,43013
cacs,43014,43015
cacs-preserve-none,43016,43017
epoll,43018,43019
EOF
assert_file_equals "$fixture_root/expected-five-ports" "$fixture_root/five-ports"

# shellcheck disable=SC2034
runs=6
order_csv="$fixture_root/run-order.csv"
invocations="$fixture_root/invocations.csv"
run_sample() {
    printf '%s,%s\n' "$2" "$1" >>"$invocations"
}
configure_benchmark legacy-sysv legacy-epoll legacy-output
run_all_samples

cat >"$fixture_root/expected-order.csv" <<'EOF'
run,position,mode
1,1,direct
1,2,coroutine
1,3,epoll
2,1,coroutine
2,2,epoll
2,3,direct
3,1,epoll
3,2,direct
3,3,coroutine
4,1,direct
4,2,coroutine
4,3,epoll
5,1,coroutine
5,2,epoll
5,3,direct
6,1,epoll
6,2,direct
6,3,coroutine
EOF
assert_file_equals "$fixture_root/expected-order.csv" "$order_csv"
awk -F, 'NR > 1 { print $1 "," $3 }' "$order_csv" \
    >"$fixture_root/expected-invocations.csv"
assert_file_equals "$fixture_root/expected-invocations.csv" "$invocations"

configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/five-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
order_csv="$fixture_root/five-run-order.csv"
invocations="$fixture_root/five-invocations.csv"
run_all_samples
cat >"$fixture_root/expected-five-order.csv" <<'EOF'
run,position,mode
1,1,direct
1,2,coroutine-sysv
1,3,cacs
1,4,cacs-preserve-none
1,5,epoll
2,1,coroutine-sysv
2,2,cacs
2,3,cacs-preserve-none
2,4,epoll
2,5,direct
3,1,cacs
3,2,cacs-preserve-none
3,3,epoll
3,4,direct
3,5,coroutine-sysv
4,1,cacs-preserve-none
4,2,epoll
4,3,direct
4,4,coroutine-sysv
4,5,cacs
5,1,epoll
5,2,direct
5,3,coroutine-sysv
5,4,cacs
5,5,cacs-preserve-none
6,1,direct
6,2,coroutine-sysv
6,3,cacs
6,4,cacs-preserve-none
6,5,epoll
EOF
assert_file_equals \
    "$fixture_root/expected-five-order.csv" "$order_csv"
awk -F, 'NR > 1 { print $1 "," $3 }' "$order_csv" \
    >"$fixture_root/expected-five-invocations.csv"
assert_file_equals \
    "$fixture_root/expected-five-invocations.csv" "$invocations"

cat >"$fixture_root/legacy-throughput.csv" <<'EOF'
mode,run,bits_per_second
direct,1,10000000000
coroutine,1,8000000000
epoll,1,7000000000
direct,2,12000000000
coroutine,2,10000000000
epoll,2,9000000000
EOF
configure_benchmark legacy-sysv legacy-epoll legacy-output
summarize_throughput "$fixture_root/legacy-throughput.csv" \
    >"$fixture_root/legacy-summary"
cat >"$fixture_root/expected-legacy-summary" <<'EOF'
direct_median_gbps=11.000
coroutine_median_gbps=9.000
epoll_median_gbps=8.000
coroutine_over_direct=0.818
epoll_over_direct=0.727
coroutine_over_epoll=1.125
EOF
assert_file_equals \
    "$fixture_root/expected-legacy-summary" "$fixture_root/legacy-summary"

cat >"$fixture_root/five-throughput.csv" <<'EOF'
mode,run,bits_per_second
direct,1,10000000000
coroutine-sysv,1,8000000000
cacs,1,9000000000
cacs-preserve-none,1,10000000000
epoll,1,7000000000
direct,2,12000000000
coroutine-sysv,2,10000000000
cacs,2,11000000000
cacs-preserve-none,2,12000000000
epoll,2,9000000000
EOF
configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/five-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
summarize_throughput "$fixture_root/five-throughput.csv" \
    >"$fixture_root/five-summary"
cat >"$fixture_root/expected-five-summary" <<'EOF'
direct_median_gbps=11.000
coroutine_sysv_median_gbps=9.000
cacs_median_gbps=10.000
cacs_preserve_none_median_gbps=11.000
epoll_median_gbps=8.000
coroutine_sysv_over_direct=0.818
cacs_over_direct=0.909
cacs_preserve_none_over_direct=1.000
epoll_over_direct=0.727
coroutine_sysv_over_epoll=1.125
cacs_over_coroutine_sysv=1.111
cacs_preserve_none_over_coroutine_sysv=1.222
EOF
assert_file_equals \
    "$fixture_root/expected-five-summary" "$fixture_root/five-summary"

cat >"$fixture_root/normal.log" <<'EOF'
listening=127.0.0.1:43001 upstream=127.0.0.1:43000
accepted=4 rejected=0 bytes_client_to_upstream=1024 bytes_upstream_to_client=1024 peak_connections=4 forced_shutdown=false
EOF
validate_forwarder_summary "$fixture_root/normal.log"

: >"$fixture_root/missing.log"
if validate_forwarder_summary "$fixture_root/missing.log" 2>/dev/null; then
    fail "missing forwarder summary was accepted"
fi

cat >"$fixture_root/duplicate.log" <<'EOF'
accepted=1 rejected=0 forced_shutdown=false
accepted=1 rejected=0 forced_shutdown=false
EOF
if validate_forwarder_summary "$fixture_root/duplicate.log" 2>/dev/null; then
    fail "duplicate forwarder summaries were accepted"
fi

cat >"$fixture_root/forced.log" <<'EOF'
accepted=1 rejected=0 forced_shutdown=true
EOF
if validate_forwarder_summary "$fixture_root/forced.log" 2>/dev/null; then
    fail "forced forwarder shutdown was accepted"
fi

sleep 0.05 &
short_pid=$!
test_pids+=("$short_pid")
run_child_bounded "$short_pid" 2 "short child"
assert_dead "$short_pid"
forget_pid "$short_pid"

start_term_ignoring_child "$fixture_root/client-ready"
client_timeout_pid=$started_pid
if run_child_bounded \
    "$client_timeout_pid" 1 "synthetic iperf3 client" \
    2>"$fixture_root/client-timeout.log"; then
    fail "timed-out client was accepted"
else
    timeout_status=$?
fi
if [[ "$timeout_status" -ne 124 ]]; then
    fail "timed-out client returned $timeout_status instead of 124"
fi
assert_dead "$client_timeout_pid"
forget_pid "$client_timeout_pid"
grep -q 'sending SIGKILL' "$fixture_root/client-timeout.log" \
    || fail "timed-out client did not escalate to SIGKILL"

start_term_ignoring_child "$fixture_root/proxy-ready"
proxy_timeout_pid=$started_pid
if terminate_and_reap_child \
    "$proxy_timeout_pid" 1 "synthetic forwarder" \
    2>"$fixture_root/proxy-timeout.log"; then
    fail "TERM-ignoring forwarder was accepted"
else
    timeout_status=$?
fi
if [[ "$timeout_status" -ne 124 ]]; then
    fail "TERM-ignoring forwarder returned $timeout_status instead of 124"
fi
assert_dead "$proxy_timeout_pid"
forget_pid "$proxy_timeout_pid"
grep -q 'sending SIGKILL' "$fixture_root/proxy-timeout.log" \
    || fail "TERM-ignoring forwarder did not escalate to SIGKILL"

cleanup_status=0
if (
    sleep 30 &
    client_pid=$!
    sleep 30 &
    proxy_pid=$!
    sleep 30 &
    server_pid=$!
    printf '%s\n' "$client_pid" "$proxy_pid" "$server_pid" \
        >"$fixture_root/cleanup-pids"
    trap cleanup EXIT
    exit 23
); then
    fail "cleanup subshell unexpectedly succeeded"
else
    cleanup_status=$?
fi
if [[ "$cleanup_status" -ne 23 ]]; then
    fail "EXIT cleanup changed status 23 to $cleanup_status"
fi
while IFS= read -r cleanup_pid; do
    assert_dead "$cleanup_pid"
done <"$fixture_root/cleanup-pids"

echo "l4 benchmark harness tests passed"
