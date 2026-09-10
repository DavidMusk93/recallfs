#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1090
L4_BENCH_SOURCE_ONLY=1 source "$script_dir/../bench/l4_bench.sh"

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

# shellcheck disable=SC2034
runs=6
order_csv="$fixture_root/run-order.csv"
invocations="$fixture_root/invocations.csv"
run_sample() {
    printf '%s,%s\n' "$2" "$1" >>"$invocations"
}
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
