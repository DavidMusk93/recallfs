#!/usr/bin/env bash
set -euo pipefail

die() {
    printf 'validate_ringzero_netns: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command is unavailable: $1"
}

[[ $# -eq 4 ]] ||
    die 'usage: validate_ringzero_netns.sh <ringzero-root> <sequence-binary> <upstream-revision> <output-directory>'

ringzero_root=$(realpath -- "$1")
sequence_binary=$(realpath -- "$2")
upstream_revision=$3
output_root=$(realpath -m -- "$4")
allowed_output_root=$(realpath -- /root/recallfs/.tmp)
ringzero="$ringzero_root/zig-out/bin/ringzero"
run_id=$$
ingress=rzvi$run_id
client_if=rzvc$run_id
egress=rzvo$run_id
backend_if=rzvb$run_id
client_ns=rzv-client-$run_id
backend_ns=rzv-backend-$run_id
pin_root=/sys/fs/bpf/ringzero-validation-$run_id
receiver_pid=
ingress_created=false
egress_created=false
client_ns_created=false
backend_ns_created=false
pin_owned=false

for command in bpftool ethtool find grep ip mkdir perl realpath rm sha256sum sort \
    uname xargs; do
    require_command "$command"
done
[[ "$(uname -s)" == Linux ]] || die 'Linux is required'
[[ "$(id -u)" -eq 0 ]] || die 'root is required'
[[ "$upstream_revision" =~ ^[0-9a-f]{40}$ ]] ||
    die 'upstream revision must be a 40-character lowercase Git object ID'
[[ -x "$ringzero" ]] || die "missing RingZero binary: $ringzero"
[[ -x "$sequence_binary" ]] ||
    die "missing sequence binary: $sequence_binary"
[[ -f "$ringzero_root/bpf/xdp_lb.o" ]] ||
    die 'missing xdp_lb.o'
[[ -f "$ringzero_root/bpf/xdp_pass.o" ]] ||
    die 'missing xdp_pass.o'
[[ "$output_root" == "$allowed_output_root/"* ]] ||
    die "output must be under $allowed_output_root: $output_root"
[[ ! -e "$output_root" ]] || die "output root already exists: $output_root"

cleanup() {
    if [[ -n "$receiver_pid" ]]; then
        kill "$receiver_pid" 2>/dev/null || true
        wait "$receiver_pid" 2>/dev/null || true
    fi
    if [[ "$pin_owned" == true ]]; then
        "$ringzero" detach --iface "$ingress" --purge --pindir "$pin_root" \
            >/dev/null 2>&1 || true
        rm -rf "$pin_root"
        pin_owned=false
    fi
    if [[ "$ingress_created" == true ]]; then
        ip link del "$ingress" 2>/dev/null || true
        ingress_created=false
    fi
    if [[ "$egress_created" == true ]]; then
        ip link del "$egress" 2>/dev/null || true
        egress_created=false
    fi
    if [[ "$client_ns_created" == true ]]; then
        ip netns del "$client_ns" 2>/dev/null || true
        client_ns_created=false
    fi
    if [[ "$backend_ns_created" == true ]]; then
        ip netns del "$backend_ns" 2>/dev/null || true
        backend_ns_created=false
    fi
}

for name in "$client_ns" "$backend_ns"; do
    ! ip netns list | grep -Eq "^$name( |$)" ||
        die "network namespace already exists: $name"
done
for name in "$ingress" "$egress"; do
    ! ip link show "$name" >/dev/null 2>&1 ||
        die "network link already exists: $name"
done
[[ ! -e "$pin_root" ]] || die "BPF pin path already exists: $pin_root"

trap cleanup EXIT
mkdir -m 700 "$pin_root"
pin_owned=true
mkdir -m 700 "$output_root"

{
    printf 'upstream_revision=%s\n' "$upstream_revision"
    printf 'kernel='
    uname -a
    sha256sum "$ringzero" "$ringzero_root/bpf/xdp_lb.o" \
        "$ringzero_root/bpf/xdp_pass.o" "$sequence_binary" "${BASH_SOURCE[0]}"
} >"$output_root/input-manifest.txt"

ip netns add "$client_ns"
client_ns_created=true
ip netns add "$backend_ns"
backend_ns_created=true
ip link add "$ingress" type veth peer name "$client_if"
ingress_created=true
ip link add "$egress" type veth peer name "$backend_if"
egress_created=true
ip link set "$client_if" netns "$client_ns"
ip link set "$backend_if" netns "$backend_ns"

ip addr add 10.211.0.1/24 dev "$ingress"
ip addr add 10.221.0.1/24 dev "$egress"
ip link set "$ingress" up
ip link set "$egress" up

ip netns exec "$client_ns" ip addr add 10.211.0.2/24 dev "$client_if"
ip netns exec "$client_ns" ip link set "$client_if" up
ip netns exec "$client_ns" ip link set lo up
ip netns exec "$backend_ns" ip addr add 10.221.0.2/24 dev "$backend_if"
ip netns exec "$backend_ns" ip link set "$backend_if" up
ip netns exec "$backend_ns" ip link set lo up

ip netns exec "$client_ns" ethtool -K "$client_if" tx off \
    >"$output_root/client-offloads.txt" 2>&1 || true
ethtool -K "$ingress" tx off \
    >"$output_root/ingress-offloads.txt" 2>&1 || true
ip netns exec "$backend_ns" ethtool -K "$backend_if" \
    tx off rx off gro off gso off \
    >"$output_root/backend-offloads.txt" 2>&1 || true
ethtool -K "$egress" tx off rx off gro off gso off \
    >"$output_root/egress-offloads.txt" 2>&1 || true

ip netns exec "$backend_ns" ip link set dev "$backend_if" \
    xdp obj "$ringzero_root/bpf/xdp_pass.o" sec xdp \
    >"$output_root/backend-xdp-pass.txt" 2>&1

router_mac=$(cat "/sys/class/net/$ingress/address")
backend_mac=$(
    ip netns exec "$backend_ns" \
        cat "/sys/class/net/$backend_if/address"
)
egress_mac=$(cat "/sys/class/net/$egress/address")
ip netns exec "$client_ns" ip route add 10.199.0.1/32 dev "$client_if"
ip netns exec "$client_ns" ip neigh replace 10.199.0.1 \
    lladdr "$router_mac" dev "$client_if" nud permanent

"$ringzero" attach --iface "$ingress" \
    --obj "$ringzero_root/bpf/xdp_lb.o" \
    --mode native --pindir "$pin_root" \
    >"$output_root/attach.txt" 2>&1
"$ringzero" vip-add --vip 10.199.0.1 --port 9100 --proto udp \
    --pindir "$pin_root" >"$output_root/vip-add.txt" 2>&1
"$ringzero" backend-add --vip 10.199.0.1:9100/udp \
    --addr 10.221.0.2 --mac "$backend_mac" --router-mac "$egress_mac" \
    --iface "$egress" --pindir "$pin_root" \
    >"$output_root/backend-add.txt" 2>&1

bpftool net show dev "$ingress" >"$output_root/bpftool-net.txt"
"$ringzero" list --pindir "$pin_root" >"$output_root/list.txt" 2>&1
"$ringzero" stats --pindir "$pin_root" >"$output_root/stats-before.txt" 2>&1

ip netns exec "$backend_ns" "$sequence_binary" \
    receive 10.221.0.2 9100 10000 3000 \
    >"$output_root/receiver.txt" 2>&1 &
receiver_pid=$!
sleep 0.2
ip netns exec "$client_ns" "$sequence_binary" \
    send 10.199.0.1 9100 10000 50 \
    >"$output_root/sender.txt" 2>&1
receiver_status=0
wait "$receiver_pid" || receiver_status=$?
receiver_pid=
"$ringzero" stats --pindir "$pin_root" >"$output_root/stats-after.txt" 2>&1

[[ "$receiver_status" -eq 0 ]] ||
    die "receiver exited with status $receiver_status"
grep -Fxq 'mode=send expected=10000 sent=10000' \
    "$output_root/sender.txt" || die 'sender count mismatch'
grep -Fxq \
    'mode=receive expected=10000 unique=10000 missing=0 duplicates=0 invalid=0' \
    "$output_root/receiver.txt" || die 'receiver ledger mismatch'
grep -Eq 'packets=[[:space:]]*10000([[:space:]]|$)' \
    "$output_root/stats-after.txt" || die 'XDP packet count mismatch'
grep -Eq 'dropped=[[:space:]]*0([[:space:]]|$)' \
    "$output_root/stats-after.txt" || die 'XDP drop count mismatch'

cleanup
trap - EXIT
! ip link show "$ingress" >/dev/null 2>&1 ||
    die 'ingress link leaked after cleanup'
! ip netns list | grep -Eq "^($client_ns|$backend_ns)( |$)" ||
    die 'network namespace leaked after cleanup'
[[ ! -e "$pin_root" ]] || die 'BPF pin path leaked after cleanup'
printf 'cleanup=pass\n' >"$output_root/cleanup.txt"
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
printf 'PASS: RingZero native-XDP netns sequence test\n'
