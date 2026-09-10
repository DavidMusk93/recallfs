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

    if (( ${#test_pids[@]} > 0 )); then
        for pid in "${test_pids[@]}"; do
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        done
    fi
    chmod -R u+rwX "$fixture_root" 2>/dev/null || true
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

file_mode() {
    stat -c '%a' -- "$1" 2>/dev/null || stat -f '%Lp' "$1"
}

file_owner() {
    stat -c '%u' -- "$1" 2>/dev/null || stat -f '%u' "$1"
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
    if (( ${#retained[@]} == 0 )); then
        test_pids=()
    else
        test_pids=("${retained[@]}")
    fi
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

make_identity_forwarder() {
    local identity=$2
    local marker=$3
    local path=$1

    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [[ $# -eq 1 && $1 == --backend-identity ]]; then\n'
        printf "    printf '%%s\\\\n' '%s'\n" "$identity"
        printf '    exit 0\n'
        printf 'fi\n'
        printf 'exit 64\n'
        printf '# %s\n' "$marker"
    } >"$path"
    chmod 700 "$path"
}

make_basename_identity_forwarder() {
    local path=$1

    cat >"$path" <<'EOF'
#!/usr/bin/env bash
if [[ $# -ne 1 || $1 != --backend-identity ]]; then
    exit 64
fi
case "${0##*/}" in
    *cacs-preserve-none*) printf '%s\n' cacs-preserve-none ;;
    *cacs*) printf '%s\n' cacs ;;
    *sysv*) printf '%s\n' coroutine-sysv ;;
    *epoll*) printf '%s\n' epoll ;;
    *) exit 65 ;;
esac
EOF
    chmod 700 "$path"
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

make_identity_forwarder \
    "$fixture_root/sysv" coroutine-sysv unique-sysv
make_identity_forwarder "$fixture_root/epoll" epoll unique-epoll
make_identity_forwarder "$fixture_root/cacs" cacs unique-cacs
make_identity_forwarder \
    "$fixture_root/cacs-preserve-none" cacs-preserve-none unique-cacs-pn
configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/legacy-output"
prepare_forwarders
write_binary_hashes >"$fixture_root/legacy-hashes"
{
    printf 'coroutine_forwarder_backend_identity=coroutine-sysv\n'
    printf 'coroutine_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/sysv" | awk '{print $1}')"
    printf 'coroutine_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode coroutine)"
    printf 'epoll_forwarder_backend_identity=epoll\n'
    printf 'epoll_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/epoll" | awk '{print $1}')"
    printf 'epoll_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode epoll)"
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
assert_equals "$fixture_root/sysv" \
    "$(configured_forwarder_for_mode coroutine-sysv)" \
    "five-mode configured SysV path"
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

prepare_forwarders
if [[ "$(forwarder_for_mode coroutine-sysv)" == "$fixture_root/sysv" ]]; then
    fail "five-mode SysV forwarder did not use a private copy"
fi
assert_equals 500 "$(file_mode "$private_forwarder_dir")" \
    "private forwarder directory mode"
assert_equals "$(id -u)" "$(file_owner "$private_forwarder_dir")" \
    "private forwarder directory owner"
for mode in coroutine-sysv cacs cacs-preserve-none epoll; do
    private_path=$(forwarder_for_mode "$mode")
    [[ -x "$private_path" ]] \
        || fail "$mode private forwarder is not executable"
    assert_equals 500 "$(file_mode "$private_path")" \
        "$mode private forwarder mode"
    assert_equals "$(id -u)" "$(file_owner "$private_path")" \
        "$mode private forwarder owner"
    case "$private_path" in
        "$output_dir"/.l4-forwarders.*/*) ;;
        *) fail "$mode private forwarder is outside the output root" ;;
    esac
done

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
    printf 'coroutine_sysv_forwarder_backend_identity=coroutine-sysv\n'
    printf 'coroutine_sysv_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/sysv" | awk '{print $1}')"
    printf 'coroutine_sysv_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode coroutine-sysv)"
    printf 'cacs_forwarder_backend_identity=cacs\n'
    printf 'cacs_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/cacs" | awk '{print $1}')"
    printf 'cacs_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode cacs)"
    printf 'cacs_preserve_none_forwarder_backend_identity=cacs-preserve-none\n'
    printf 'cacs_preserve_none_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/cacs-preserve-none" | awk '{print $1}')"
    printf 'cacs_preserve_none_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode cacs-preserve-none)"
    printf 'epoll_forwarder_backend_identity=epoll\n'
    printf 'epoll_forwarder_sha256=%s\n' \
        "$(sha256sum "$fixture_root/epoll" | awk '{print $1}')"
    printf 'epoll_forwarder_private_path=%s\n' \
        "$(forwarder_for_mode epoll)"
} >"$fixture_root/expected-five-hashes"
assert_file_equals \
    "$fixture_root/expected-five-hashes" "$fixture_root/five-hashes"

configure_benchmark \
    "$fixture_root/cacs" \
    "$fixture_root/epoll" \
    "$fixture_root/swapped-output" \
    "$fixture_root/sysv" \
    "$fixture_root/cacs-preserve-none"
if prepare_forwarders 2>"$fixture_root/swapped.log"; then
    fail "swapped SysV and CACS forwarders were accepted"
fi
grep -Fq 'forwarder identity mismatch' "$fixture_root/swapped.log" \
    || fail "swapped forwarders did not report an identity mismatch"

make_identity_forwarder \
    "$fixture_root/duplicate-identity-cacs" coroutine-sysv duplicate-identity
configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/duplicate-identity-output" \
    "$fixture_root/duplicate-identity-cacs" \
    "$fixture_root/cacs-preserve-none"
if prepare_forwarders 2>"$fixture_root/duplicate-identity.log"; then
    fail "duplicate backend identities were accepted"
fi
grep -Fq 'duplicate backend identity' "$fixture_root/duplicate-identity.log" \
    || fail "duplicate backend identity was not diagnosed"

mkdir -p "$fixture_root/duplicate-digest"
make_basename_identity_forwarder "$fixture_root/duplicate-digest/sysv"
cp "$fixture_root/duplicate-digest/sysv" \
    "$fixture_root/duplicate-digest/cacs"
cp "$fixture_root/duplicate-digest/sysv" \
    "$fixture_root/duplicate-digest/cacs-preserve-none"
cp "$fixture_root/duplicate-digest/sysv" \
    "$fixture_root/duplicate-digest/epoll"
configure_benchmark \
    "$fixture_root/duplicate-digest/sysv" \
    "$fixture_root/duplicate-digest/epoll" \
    "$fixture_root/duplicate-digest-output" \
    "$fixture_root/duplicate-digest/cacs" \
    "$fixture_root/duplicate-digest/cacs-preserve-none"
if prepare_forwarders 2>"$fixture_root/duplicate-digest.log"; then
    fail "duplicate forwarder digests were accepted"
fi
grep -Fq 'duplicate forwarder SHA-256' "$fixture_root/duplicate-digest.log" \
    || fail "duplicate forwarder digest was not diagnosed"

configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/source-replacement-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
prepare_forwarders
make_identity_forwarder \
    "$fixture_root/sysv-replacement" coroutine-sysv replaced-source
mv "$fixture_root/sysv-replacement" "$fixture_root/sysv"
if verify_forwarder_artifacts \
    coroutine-sysv post-sample 2>"$fixture_root/source-replacement.log"; then
    fail "source forwarder replacement was accepted"
fi
grep -Fq 'source forwarder SHA-256 changed' \
    "$fixture_root/source-replacement.log" \
    || fail "source forwarder replacement was not diagnosed"

make_identity_forwarder \
    "$fixture_root/sysv" coroutine-sysv unique-sysv
configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/private-replacement-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
prepare_forwarders
private_cacs=$(forwarder_for_mode cacs)
make_identity_forwarder \
    "$fixture_root/cacs-replacement" cacs replaced-private-copy
chmod 700 "$private_forwarder_dir"
mv "$fixture_root/cacs-replacement" "$private_cacs"
if verify_forwarder_artifacts \
    cacs post-sample 2>"$fixture_root/private-replacement.log"; then
    fail "private forwarder replacement was accepted"
fi
grep -Fq 'private-copy forwarder SHA-256 changed' \
    "$fixture_root/private-replacement.log" \
    || fail "private forwarder replacement was not diagnosed"

configure_benchmark \
    "$fixture_root/sysv" \
    "$fixture_root/epoll" \
    "$fixture_root/identity-columns-output" \
    "$fixture_root/cacs" \
    "$fixture_root/cacs-preserve-none"
prepare_forwarders
csv="$fixture_root/identity-throughput.csv"
stream_csv="$fixture_root/identity-stream.csv"
resource_csv="$fixture_root/identity-resources.csv"
initialize_result_files
append_throughput_row direct 1 123
append_throughput_row cacs 1 456
append_resource_row cacs 1 10 20 30 40 50 60
cacs_digest=$(sha256sum "$fixture_root/cacs" | awk '{print $1}')
cat >"$fixture_root/expected-identity-throughput.csv" <<EOF
mode,run,backend_identity,binary_sha256,bits_per_second
direct,1,direct,not-applicable,123
cacs,1,cacs,$cacs_digest,456
EOF
cat >"$fixture_root/expected-identity-resources.csv" <<EOF
mode,run,backend_identity,binary_sha256,vm_peak_kb,vm_hwm_kb,user_ticks,system_ticks,voluntary_context_switches,nonvoluntary_context_switches
cacs,1,cacs,$cacs_digest,10,20,30,40,50,60
EOF
assert_file_equals \
    "$fixture_root/expected-identity-throughput.csv" "$csv"
assert_file_equals \
    "$fixture_root/expected-identity-resources.csv" "$resource_csv"

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
summarize_throughput "$fixture_root/legacy-throughput.csv" 2 \
    >"$fixture_root/legacy-summary"
cat >"$fixture_root/expected-legacy-summary" <<'EOF'
direct_median_gbps=11.000
coroutine_median_gbps=9.000
epoll_median_gbps=8.000
coroutine_over_direct_ratio_of_medians=0.818
coroutine_over_direct_median_paired_ratio=0.817
coroutine_over_direct=0.817
epoll_over_direct_ratio_of_medians=0.727
epoll_over_direct_median_paired_ratio=0.725
epoll_over_direct=0.725
coroutine_over_epoll_ratio_of_medians=1.125
coroutine_over_epoll_median_paired_ratio=1.127
coroutine_over_epoll=1.127
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
summarize_throughput "$fixture_root/five-throughput.csv" 2 \
    >"$fixture_root/five-summary"
cat >"$fixture_root/expected-five-summary" <<'EOF'
direct_median_gbps=11.000
coroutine_sysv_median_gbps=9.000
cacs_median_gbps=10.000
cacs_preserve_none_median_gbps=11.000
epoll_median_gbps=8.000
coroutine_sysv_over_direct_ratio_of_medians=0.818
coroutine_sysv_over_direct_median_paired_ratio=0.817
coroutine_sysv_over_direct=0.817
cacs_over_direct_ratio_of_medians=0.909
cacs_over_direct_median_paired_ratio=0.908
cacs_over_direct=0.908
cacs_preserve_none_over_direct_ratio_of_medians=1.000
cacs_preserve_none_over_direct_median_paired_ratio=1.000
cacs_preserve_none_over_direct=1.000
epoll_over_direct_ratio_of_medians=0.727
epoll_over_direct_median_paired_ratio=0.725
epoll_over_direct=0.725
coroutine_sysv_over_epoll_ratio_of_medians=1.125
coroutine_sysv_over_epoll_median_paired_ratio=1.127
coroutine_sysv_over_epoll=1.127
cacs_over_coroutine_sysv_ratio_of_medians=1.111
cacs_over_coroutine_sysv_median_paired_ratio=1.113
cacs_over_coroutine_sysv=1.113
cacs_preserve_none_over_coroutine_sysv_ratio_of_medians=1.222
cacs_preserve_none_over_coroutine_sysv_median_paired_ratio=1.225
cacs_preserve_none_over_coroutine_sysv=1.225
EOF
assert_file_equals \
    "$fixture_root/expected-five-summary" "$fixture_root/five-summary"

cat >"$fixture_root/divergent-ratios.csv" <<'EOF'
mode,run,bits_per_second
direct,1,1000000000
coroutine,1,1000000000
epoll,1,1000000000
direct,2,100000000000
coroutine,2,2000000000
epoll,2,100000000000
direct,3,100000000000
coroutine,3,100000000000
epoll,3,100000000000
EOF
configure_benchmark legacy-sysv legacy-epoll legacy-output
summarize_throughput "$fixture_root/divergent-ratios.csv" 3 \
    >"$fixture_root/divergent-ratios-summary"
grep -Fxq 'coroutine_over_direct_ratio_of_medians=0.020' \
    "$fixture_root/divergent-ratios-summary" \
    || fail "ratio-of-medians was not reported independently"
grep -Fxq 'coroutine_over_direct_median_paired_ratio=1.000' \
    "$fixture_root/divergent-ratios-summary" \
    || fail "median paired ratio was not indexed by run"
grep -Fxq 'coroutine_over_direct=1.000' \
    "$fixture_root/divergent-ratios-summary" \
    || fail "stated comparison did not use the paired ratio"

sed '/^epoll,3,/d' "$fixture_root/divergent-ratios.csv" \
    >"$fixture_root/missing-run.csv"
if summarize_throughput \
    "$fixture_root/missing-run.csv" 3 \
    >"$fixture_root/missing-run-summary" \
    2>"$fixture_root/missing-run-error"; then
    fail "incomplete mode/run matrix was accepted"
fi
grep -Fq 'mode epoll has runs [1, 2], expected [1, 2, 3]' \
    "$fixture_root/missing-run-error" \
    || fail "incomplete mode/run matrix was not diagnosed"

sed 's/^coroutine,2,2000000000$/coroutine,2,0/' \
    "$fixture_root/divergent-ratios.csv" >"$fixture_root/nonpositive.csv"
if summarize_throughput \
    "$fixture_root/nonpositive.csv" 3 \
    >"$fixture_root/nonpositive-summary" \
    2>"$fixture_root/nonpositive-error"; then
    fail "nonpositive throughput was accepted"
fi
grep -Fq 'throughput must be positive' "$fixture_root/nonpositive-error" \
    || fail "nonpositive throughput was not diagnosed"

{
    cat "$fixture_root/divergent-ratios.csv"
    printf 'coroutine,2,2000000000\n'
} >"$fixture_root/duplicate-run.csv"
if summarize_throughput \
    "$fixture_root/duplicate-run.csv" 3 \
    >"$fixture_root/duplicate-run-summary" \
    2>"$fixture_root/duplicate-run-error"; then
    fail "duplicate mode/run throughput was accepted"
fi
grep -Fq 'duplicate sample for mode coroutine run 2' \
    "$fixture_root/duplicate-run-error" \
    || fail "duplicate mode/run throughput was not diagnosed"

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
