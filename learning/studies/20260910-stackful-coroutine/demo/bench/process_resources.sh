#!/usr/bin/env bash

read_process_resources() {
    if [[ $# -lt 1 || $# -gt 2 ]]; then
        echo "usage: read_process_resources <pid> [proc-root]" >&2
        return 2
    fi

    local pid=$1
    local proc_root=${2:-/proc}
    local stat_path="$proc_root/$pid/stat"
    local status_path="$proc_root/$pid/status"

    awk -v stat_path="$stat_path" '
        FILENAME == stat_path {
            line = $0
            if (!sub(/^[[:digit:]]+[[:space:]]+\(.*\)[[:space:]]+/, "",
                     line)) {
                exit 1
            }
            count = split(line, fields, /[[:space:]]+/)
            if (count < 13) {
                exit 1
            }
            user_ticks = fields[12]
            system_ticks = fields[13]
            next
        }
        /^VmPeak:/ { vm_peak_kb = $2 }
        /^VmHWM:/ { vm_hwm_kb = $2 }
        /^voluntary_ctxt_switches:/ { voluntary_switches = $2 }
        /^nonvoluntary_ctxt_switches:/ { nonvoluntary_switches = $2 }
        END {
            numeric = "^[[:digit:]]+$"
            if (user_ticks !~ numeric || system_ticks !~ numeric ||
                vm_peak_kb !~ numeric || vm_hwm_kb !~ numeric ||
                voluntary_switches !~ numeric ||
                nonvoluntary_switches !~ numeric) {
                exit 1
            }
            printf "%s,%s,%s,%s,%s,%s\n",
                   vm_peak_kb, vm_hwm_kb, user_ticks, system_ticks,
                   voluntary_switches, nonvoluntary_switches
        }
    ' "$stat_path" "$status_path"
}
