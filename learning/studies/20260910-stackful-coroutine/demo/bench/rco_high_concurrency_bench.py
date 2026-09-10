#!/usr/bin/env python3

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import time


RESULT_SCHEMA = "rco-high-concurrency-v2"
MODE_NAMES = ("sysv", "cacs", "cacs-preserve-none")
MAX_TASKS = 16384
MAX_STACK_BYTES = 512 * 1024
MAX_TOTAL_STACK_BYTES = 2 * 1024 * 1024 * 1024
MAX_TOUCHED_BYTES = 512 * 1024 * 1024
MAX_TOTAL_YIELDS = 20_000_000
MAX_RUNS = 10
MAX_CASES = 10
MAX_TIMEOUT_SECONDS = 600.0
CLD_EXITED = getattr(os, "CLD_EXITED", 1)
CLD_KILLED = getattr(os, "CLD_KILLED", 2)
CLD_DUMPED = getattr(os, "CLD_DUMPED", 3)
CLD_STOPPED = getattr(os, "CLD_STOPPED", 5)

SAMPLE_FIELDS = (
    "schema",
    "case",
    "run",
    "position",
    "mode",
    "backend_identity",
    "binary_sha256",
    "tasks",
    "stack_bytes",
    "touch_bytes",
    "page_size",
    "sentinels_per_task",
    "yields_per_task",
    "requested_yields",
    "barrier_yields",
    "measured_yields",
    "checksum",
    "expected_checksum",
    "runtime_switches",
    "expected_runtime_switches",
    "wall_ns",
    "user_us",
    "system_us",
    "minor_faults_delta",
    "major_faults_delta",
    "voluntary_context_switches_delta",
    "involuntary_context_switches_delta",
    "spawned",
    "completed",
    "peak_active",
    "vm_peak_kb",
    "vm_hwm_kb",
    "vm_size_kb",
    "vm_rss_kb",
    "rss_anon_kb",
    "rss_file_kb",
    "rss_shmem_kb",
    "setup_minor_faults",
    "setup_major_faults",
    "smaps_rss_kb",
    "smaps_pss_kb",
    "smaps_private_clean_kb",
    "smaps_private_dirty_kb",
    "smaps_anonymous_kb",
)

SUMMARY_FIELDS = (
    "schema",
    "case",
    "mode",
    "backend_identity",
    "binary_sha256",
    "samples",
    "tasks",
    "stack_bytes",
    "touch_bytes",
    "yields_per_task",
    "requested_yields",
    "barrier_yields",
    "measured_yields",
    "checksum",
    "runtime_switches",
    "median_wall_ns",
    "median_user_us",
    "median_system_us",
    "median_wall_ns_per_measured_yield",
    "median_cpu_ns_per_measured_yield",
    "paired_wall_ratio_vs_sysv",
    "paired_cpu_ratio_vs_sysv",
    "median_setup_minor_faults",
    "median_setup_major_faults",
    "median_minor_faults_delta",
    "median_major_faults_delta",
    "median_voluntary_context_switches_delta",
    "median_involuntary_context_switches_delta",
    "median_vm_hwm_kb",
    "median_vm_rss_kb",
    "median_smaps_rss_kb",
    "median_smaps_pss_kb",
    "median_smaps_anonymous_kb",
)


class BenchmarkError(RuntimeError):
    pass


def raise_on_termination_signal(signum, _frame):
    raise BenchmarkError("received {}".format(signal.Signals(signum).name))


@dataclass(frozen=True)
class Mode:
    name: str
    executable: Path


@dataclass(frozen=True)
class BenchmarkCase:
    name: str
    tasks: int
    stack_bytes: int
    touch_bytes: int
    yields_per_task: int


DEFAULT_CASES = (
    BenchmarkCase("tasks-256", 256, 64 * 1024, 4 * 1024, 64),
    BenchmarkCase("tasks-1024", 1024, 64 * 1024, 4 * 1024, 64),
    BenchmarkCase("tasks-4096", 4096, 64 * 1024, 4 * 1024, 64),
    BenchmarkCase("tasks-16384", 16384, 64 * 1024, 4 * 1024, 64),
    BenchmarkCase("working-set-32k", 1024, 96 * 1024, 32 * 1024, 64),
    BenchmarkCase("working-set-128k", 1024, 192 * 1024, 128 * 1024, 64),
    BenchmarkCase("working-set-256k", 1024, 320 * 1024, 256 * 1024, 64),
    BenchmarkCase("yields-16", 1024, 64 * 1024, 4 * 1024, 16),
    BenchmarkCase("yields-128", 1024, 64 * 1024, 4 * 1024, 128),
    BenchmarkCase("yields-1024", 1024, 64 * 1024, 4 * 1024, 1024),
)

SMOKE_CASES = (
    BenchmarkCase("smoke", 3, 32 * 1024, 4 * 1024, 4),
)


def expected_checksum(case, page_size):
    sentinel_count = case.touch_bytes // page_size
    checksum = 0
    for task_id in range(1, case.tasks + 1):
        sentinel_sum = sum(
            ((task_id * 131) + (page * 17) + 0x5A) & 0xFF
            for page in range(sentinel_count)
        )
        checksum += sentinel_sum * (case.yields_per_task + 1)
    return checksum


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_case(case, page_size):
    if page_size <= 0 or page_size & (page_size - 1):
        raise BenchmarkError("page size must be a positive power of two")
    if not case.name or "," in case.name or "\n" in case.name:
        raise BenchmarkError("case names must be nonempty CSV-safe strings")
    if not 1 <= case.tasks <= MAX_TASKS:
        raise BenchmarkError("{} has an invalid task count".format(case.name))
    if not 16 * 1024 <= case.stack_bytes <= MAX_STACK_BYTES:
        raise BenchmarkError("{} has an invalid stack size".format(case.name))
    if case.stack_bytes % page_size != 0:
        raise BenchmarkError("{} stack size is not page-aligned".format(case.name))
    if case.touch_bytes <= 0 or case.touch_bytes % page_size != 0:
        raise BenchmarkError("{} touch size is not page-aligned".format(case.name))
    if case.touch_bytes + page_size + 8192 > case.stack_bytes:
        raise BenchmarkError("{} leaves insufficient stack headroom".format(case.name))
    if case.tasks * case.stack_bytes > MAX_TOTAL_STACK_BYTES:
        raise BenchmarkError("{} exceeds the mapped-stack bound".format(case.name))
    if case.tasks * case.touch_bytes > MAX_TOUCHED_BYTES:
        raise BenchmarkError("{} exceeds the touched-memory bound".format(case.name))
    if (
        case.yields_per_task <= 0
        or case.tasks * case.yields_per_task > MAX_TOTAL_YIELDS
    ):
        raise BenchmarkError("{} exceeds the yield bound".format(case.name))


def read_status(pid):
    path = Path("/proc") / str(pid) / "status"
    values = {}
    for line in path.read_text(encoding="ascii").splitlines():
        name, separator, remainder = line.partition(":")
        if not separator:
            continue
        values[name] = remainder.strip()
    required = {
        "VmPeak": "vm_peak_kb",
        "VmHWM": "vm_hwm_kb",
        "VmSize": "vm_size_kb",
        "VmRSS": "vm_rss_kb",
        "RssAnon": "rss_anon_kb",
        "RssFile": "rss_file_kb",
        "RssShmem": "rss_shmem_kb",
    }
    result = {}
    for source, destination in required.items():
        parts = values.get(source, "").split()
        if len(parts) != 2 or parts[1] != "kB" or not parts[0].isdigit():
            raise BenchmarkError("invalid {} in {}".format(source, path))
        result[destination] = int(parts[0])
    return result


def read_smaps_rollup(pid):
    path = Path("/proc") / str(pid) / "smaps_rollup"
    values = {}
    for line in path.read_text(encoding="ascii").splitlines():
        name, separator, remainder = line.partition(":")
        if not separator:
            continue
        values[name] = remainder.strip()
    required = {
        "Rss": "smaps_rss_kb",
        "Pss": "smaps_pss_kb",
        "Private_Clean": "smaps_private_clean_kb",
        "Private_Dirty": "smaps_private_dirty_kb",
        "Anonymous": "smaps_anonymous_kb",
    }
    result = {}
    for source, destination in required.items():
        parts = values.get(source, "").split()
        if len(parts) != 2 or parts[1] != "kB" or not parts[0].isdigit():
            raise BenchmarkError("invalid {} in {}".format(source, path))
        result[destination] = int(parts[0])
    return result


def read_setup_faults(pid):
    path = Path("/proc") / str(pid) / "stat"
    line = path.read_text(encoding="ascii").strip()
    separator = line.rfind(") ")
    if separator < 0:
        raise BenchmarkError("invalid process stat in {}".format(path))
    fields = line[separator + 2 :].split()
    if len(fields) < 10 or not fields[7].isdigit() or not fields[9].isdigit():
        raise BenchmarkError("invalid fault counters in {}".format(path))
    return {
        "setup_minor_faults": int(fields[7]),
        "setup_major_faults": int(fields[9]),
    }


def process_group_exists(process_group):
    try:
        os.killpg(process_group, 0)
    except ProcessLookupError:
        return False
    return True


def terminate_process_group(process):
    for group_signal in (signal.SIGCONT, signal.SIGTERM):
        try:
            os.killpg(process.pid, group_signal)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=0.2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            raise BenchmarkError(
                "process group {} survived SIGKILL".format(process.pid)
            )
    if process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def wait_until_stopped(process, deadline):
    while time.monotonic() < deadline:
        try:
            event = os.waitid(
                os.P_PID,
                process.pid,
                os.WSTOPPED | os.WEXITED | os.WNOHANG | os.WNOWAIT,
            )
        except ChildProcessError:
            event = None
        if event is not None and event.si_code == CLD_STOPPED:
            return
        if event is not None and event.si_code in (
            CLD_EXITED,
            CLD_KILLED,
            CLD_DUMPED,
        ):
            process.wait(timeout=0)
            raise BenchmarkError(
                "benchmark exited before SIGSTOP with {}".format(
                    process.returncode
                )
            )
        time.sleep(0.01)
    raise BenchmarkError("benchmark timed out before the residency barrier")


def parse_result(stdout):
    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise BenchmarkError(
            "benchmark must emit exactly one structured result line"
        )
    try:
        result = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise BenchmarkError("invalid benchmark JSON: {}".format(error))
    if not isinstance(result, dict):
        raise BenchmarkError("benchmark result must be a JSON object")
    return result


def run_sample(
    mode,
    case,
    run,
    position,
    timeout_seconds,
    binary_sha256=None,
):
    if mode.name not in MODE_NAMES:
        raise BenchmarkError("unknown benchmark mode: {}".format(mode.name))
    observed_sha256 = sha256_file(mode.executable)
    if (
        binary_sha256 is not None
        and observed_sha256 != binary_sha256
    ):
        raise BenchmarkError(
            "{} changed after matrix validation".format(mode.executable)
        )
    binary_sha256 = observed_sha256
    command = [
        str(mode.executable),
        "--tasks",
        str(case.tasks),
        "--stack-bytes",
        str(case.stack_bytes),
        "--touch-bytes",
        str(case.touch_bytes),
        "--yields-per-task",
        str(case.yields_per_task),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    deadline = time.monotonic() + timeout_seconds
    completed = False
    try:
        wait_until_stopped(process, deadline)
        resources = read_status(process.pid)
        resources.update(read_smaps_rollup(process.pid))
        resources.update(read_setup_faults(process.pid))
        os.kill(process.pid, signal.SIGCONT)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise BenchmarkError("benchmark timed out at the residency barrier")
        try:
            stdout, stderr = process.communicate(timeout=remaining)
        except subprocess.TimeoutExpired:
            raise BenchmarkError("benchmark timed out after SIGCONT")
        if process.returncode != 0:
            raise BenchmarkError(
                "benchmark exited with {}: stdout={!r} stderr={!r}".format(
                    process.returncode, stdout, stderr
                )
            )
        if stderr:
            raise BenchmarkError(
                "benchmark wrote unexpected stderr: {!r}".format(stderr)
            )
        result = parse_result(stdout)
        if result.get("backend_identity") != mode.name:
            raise BenchmarkError(
                "{} backend identity is {!r}, expected {!r}".format(
                    mode.executable,
                    result.get("backend_identity"),
                    mode.name,
                )
            )
        if sha256_file(mode.executable) != binary_sha256:
            raise BenchmarkError(
                "{} changed while its sample was running".format(
                    mode.executable
                )
            )
        result["binary_sha256"] = binary_sha256
        result.update(resources)
        result.update(
            {
                "case": case.name,
                "run": run,
                "position": position,
                "mode": mode.name,
            }
        )
        completed = True
        return result
    finally:
        if not completed:
            terminate_process_group(process)


def integer_field(row, name):
    if name not in row:
        raise BenchmarkError("sample is missing {}".format(name))
    value = row[name]
    if isinstance(value, bool):
        raise BenchmarkError("{} must be an integer".format(name))
    try:
        converted = int(value)
    except (TypeError, ValueError):
        raise BenchmarkError("{} must be an integer".format(name))
    if str(value).strip() != str(converted):
        raise BenchmarkError("{} is not a canonical integer".format(name))
    return converted


def validate_samples(rows, cases, runs):
    if not 1 <= runs <= MAX_RUNS:
        raise BenchmarkError("runs must be in [1, {}]".format(MAX_RUNS))
    case_by_name = {}
    for case in cases:
        if case.name in case_by_name:
            raise BenchmarkError("duplicate case name: {}".format(case.name))
        case_by_name[case.name] = case

    expected_keys = {
        (case.name, run, mode)
        for case in cases
        for run in range(1, runs + 1)
        for mode in MODE_NAMES
    }
    observed_keys = set()
    observed_positions = set()
    sha_by_mode = {}
    mode_by_sha = {}
    host_page_size = os.sysconf("SC_PAGE_SIZE")
    positive_fields = (
        "tasks",
        "stack_bytes",
        "touch_bytes",
        "page_size",
        "sentinels_per_task",
        "yields_per_task",
        "requested_yields",
        "measured_yields",
        "checksum",
        "expected_checksum",
        "runtime_switches",
        "expected_runtime_switches",
        "wall_ns",
        "spawned",
        "completed",
        "peak_active",
        "vm_peak_kb",
        "vm_hwm_kb",
        "vm_size_kb",
        "vm_rss_kb",
        "rss_anon_kb",
        "rss_file_kb",
        "smaps_rss_kb",
        "smaps_pss_kb",
        "smaps_anonymous_kb",
    )
    nonnegative_fields = (
        "user_us",
        "system_us",
        "barrier_yields",
        "minor_faults_delta",
        "major_faults_delta",
        "voluntary_context_switches_delta",
        "involuntary_context_switches_delta",
        "rss_shmem_kb",
        "smaps_private_clean_kb",
        "smaps_private_dirty_kb",
        "setup_minor_faults",
        "setup_major_faults",
    )

    for row in rows:
        fields = set(row)
        missing = set(SAMPLE_FIELDS) - fields
        extra = fields - set(SAMPLE_FIELDS)
        if missing or extra:
            raise BenchmarkError(
                "sample fields differ: missing={} extra={}".format(
                    sorted(missing), sorted(extra)
                )
            )
        if row["schema"] != RESULT_SCHEMA:
            raise BenchmarkError("sample has an unknown schema")
        case_name = row["case"]
        mode = row["mode"]
        if case_name not in case_by_name or mode not in MODE_NAMES:
            raise BenchmarkError("sample has an unknown case or mode")
        if row["backend_identity"] != mode:
            raise BenchmarkError("sample backend identity does not match its mode")
        binary_sha256 = row["binary_sha256"]
        if (
            not isinstance(binary_sha256, str)
            or len(binary_sha256) != 64
            or binary_sha256 != binary_sha256.lower()
            or any(character not in "0123456789abcdef"
                   for character in binary_sha256)
        ):
            raise BenchmarkError("sample has an invalid binary SHA-256")
        previous_sha = sha_by_mode.setdefault(mode, binary_sha256)
        if previous_sha != binary_sha256:
            raise BenchmarkError(
                "mode {} has inconsistent binary SHA-256 values".format(mode)
            )
        previous_mode = mode_by_sha.setdefault(binary_sha256, mode)
        if previous_mode != mode:
            raise BenchmarkError(
                "duplicate benchmark binaries for {} and {}".format(
                    previous_mode, mode
                )
            )
        run = integer_field(row, "run")
        position = integer_field(row, "position")
        if run < 1 or run > runs or position < 1 or position > len(MODE_NAMES):
            raise BenchmarkError("sample has an invalid run or position")
        key = (case_name, run, mode)
        position_key = (case_name, run, position)
        if key in observed_keys:
            raise BenchmarkError("duplicate sample: {}".format(key))
        if position_key in observed_positions:
            raise BenchmarkError(
                "duplicate mode position: {}".format(position_key)
            )
        observed_keys.add(key)
        observed_positions.add(position_key)

        values = {}
        for name in positive_fields:
            values[name] = integer_field(row, name)
            if values[name] <= 0:
                raise BenchmarkError("{} must be positive".format(name))
        for name in nonnegative_fields:
            values[name] = integer_field(row, name)
            if values[name] < 0:
                raise BenchmarkError("{} must be nonnegative".format(name))

        case = case_by_name[case_name]
        if values["page_size"] != host_page_size:
            raise BenchmarkError("sample page size does not match the host")
        validate_case(case, values["page_size"])
        if (
            values["tasks"] != case.tasks
            or values["stack_bytes"] != case.stack_bytes
            or values["touch_bytes"] != case.touch_bytes
            or values["yields_per_task"] != case.yields_per_task
        ):
            raise BenchmarkError("sample configuration does not match its case")
        requested_yields = case.tasks * case.yields_per_task
        barrier_yields = case.tasks - 1
        measured_yields = requested_yields + barrier_yields
        checksum = expected_checksum(case, values["page_size"])
        switches = (
            case.tasks * (2 * case.yields_per_task + 6)
            + 2 * barrier_yields
        )
        if values["sentinels_per_task"] != (
            case.touch_bytes // values["page_size"]
        ):
            raise BenchmarkError("sentinel count is inconsistent")
        if (
            values["requested_yields"] != requested_yields
            or values["barrier_yields"] != barrier_yields
            or values["measured_yields"] != measured_yields
        ):
            raise BenchmarkError("measured yield counts are inconsistent")
        if (
            values["checksum"] != checksum
            or values["expected_checksum"] != checksum
        ):
            raise BenchmarkError("checksum does not match the independent oracle")
        if (
            values["runtime_switches"] != switches
            or values["expected_runtime_switches"] != switches
        ):
            raise BenchmarkError("runtime switch count is inconsistent")
        if (
            values["spawned"] != case.tasks
            or values["completed"] != case.tasks
            or values["peak_active"] != case.tasks
        ):
            raise BenchmarkError("task lifecycle counts are inconsistent")
        if values["smaps_pss_kb"] > values["smaps_rss_kb"]:
            raise BenchmarkError("smaps PSS exceeds RSS")
        if (
            values["smaps_private_clean_kb"]
            + values["smaps_private_dirty_kb"]
            > values["smaps_rss_kb"]
        ):
            raise BenchmarkError("smaps private memory exceeds RSS")

    missing_keys = expected_keys - observed_keys
    extra_keys = observed_keys - expected_keys
    if missing_keys or extra_keys:
        raise BenchmarkError(
            "sample matrix is incomplete: missing={} extra={}".format(
                sorted(missing_keys), sorted(extra_keys)
            )
        )
    if set(sha_by_mode) != set(MODE_NAMES):
        raise BenchmarkError("sample matrix is missing binary identities")


def median(rows, field):
    return statistics.median(integer_field(row, field) for row in rows)


def summarize(rows, cases):
    summaries = []
    for case in cases:
        rows_by_run = {}
        for row in rows:
            if row["case"] == case.name:
                rows_by_run.setdefault(integer_field(row, "run"), {})[
                    row["mode"]
                ] = row
        for mode in MODE_NAMES:
            group = [
                row
                for row in rows
                if row["case"] == case.name and row["mode"] == mode
            ]
            wall_per_measured_yield = [
                integer_field(row, "wall_ns")
                / integer_field(row, "measured_yields")
                for row in group
            ]
            cpu_per_measured_yield = [
                (
                    integer_field(row, "user_us")
                    + integer_field(row, "system_us")
                )
                * 1000
                / integer_field(row, "measured_yields")
                for row in group
            ]
            paired_wall_ratios = []
            paired_cpu_ratios = []
            for run, run_rows in sorted(rows_by_run.items()):
                baseline = run_rows["sysv"]
                current = run_rows[mode]
                baseline_wall = integer_field(baseline, "wall_ns")
                baseline_cpu = (
                    integer_field(baseline, "user_us")
                    + integer_field(baseline, "system_us")
                )
                current_cpu = (
                    integer_field(current, "user_us")
                    + integer_field(current, "system_us")
                )
                if baseline_wall <= 0 or baseline_cpu <= 0:
                    raise BenchmarkError(
                        "sysv baseline is nonpositive for {} run {}".format(
                            case.name, run
                        )
                    )
                paired_wall_ratios.append(
                    integer_field(current, "wall_ns") / baseline_wall
                )
                paired_cpu_ratios.append(current_cpu / baseline_cpu)
            summaries.append(
                {
                    "schema": RESULT_SCHEMA,
                    "case": case.name,
                    "mode": mode,
                    "backend_identity": group[0]["backend_identity"],
                    "binary_sha256": group[0]["binary_sha256"],
                    "samples": len(group),
                    "tasks": case.tasks,
                    "stack_bytes": case.stack_bytes,
                    "touch_bytes": case.touch_bytes,
                    "yields_per_task": case.yields_per_task,
                    "requested_yields": integer_field(
                        group[0], "requested_yields"
                    ),
                    "barrier_yields": integer_field(
                        group[0], "barrier_yields"
                    ),
                    "measured_yields": integer_field(
                        group[0], "measured_yields"
                    ),
                    "checksum": integer_field(group[0], "checksum"),
                    "runtime_switches": integer_field(
                        group[0], "runtime_switches"
                    ),
                    "median_wall_ns": median(group, "wall_ns"),
                    "median_user_us": median(group, "user_us"),
                    "median_system_us": median(group, "system_us"),
                    "median_wall_ns_per_measured_yield": statistics.median(
                        wall_per_measured_yield
                    ),
                    "median_cpu_ns_per_measured_yield": statistics.median(
                        cpu_per_measured_yield
                    ),
                    "paired_wall_ratio_vs_sysv": statistics.median(
                        paired_wall_ratios
                    ),
                    "paired_cpu_ratio_vs_sysv": statistics.median(
                        paired_cpu_ratios
                    ),
                    "median_setup_minor_faults": median(
                        group, "setup_minor_faults"
                    ),
                    "median_setup_major_faults": median(
                        group, "setup_major_faults"
                    ),
                    "median_minor_faults_delta": median(
                        group, "minor_faults_delta"
                    ),
                    "median_major_faults_delta": median(
                        group, "major_faults_delta"
                    ),
                    "median_voluntary_context_switches_delta": median(
                        group, "voluntary_context_switches_delta"
                    ),
                    "median_involuntary_context_switches_delta": median(
                        group, "involuntary_context_switches_delta"
                    ),
                    "median_vm_hwm_kb": median(group, "vm_hwm_kb"),
                    "median_vm_rss_kb": median(group, "vm_rss_kb"),
                    "median_smaps_rss_kb": median(group, "smaps_rss_kb"),
                    "median_smaps_pss_kb": median(group, "smaps_pss_kb"),
                    "median_smaps_anonymous_kb": median(
                        group, "smaps_anonymous_kb"
                    ),
                }
            )
    return summaries


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="ascii") as destination:
        writer = csv.DictWriter(
            destination, fieldnames=fieldnames, extrasaction="raise"
        )
        writer.writeheader()
        writer.writerows(rows)


def validate_mode_binaries(modes):
    digests = {}
    modes_by_digest = {}
    for mode in modes:
        if not mode.executable.is_file() or not os.access(
            str(mode.executable), os.X_OK
        ):
            raise BenchmarkError(
                "{} executable is not runnable: {}".format(
                    mode.name, mode.executable
                )
            )
        digest = sha256_file(mode.executable)
        duplicate = modes_by_digest.setdefault(digest, mode.name)
        if duplicate != mode.name:
            raise BenchmarkError(
                "duplicate benchmark binaries for {} and {}".format(
                    duplicate, mode.name
                )
            )
        digests[mode.name] = digest
    return digests


def run_matrix(modes, cases, runs, timeout_seconds, output_dir):
    if tuple(mode.name for mode in modes) != MODE_NAMES:
        raise BenchmarkError("modes must use the canonical three-mode order")
    if not 1 <= runs <= MAX_RUNS:
        raise BenchmarkError("runs must be in [1, {}]".format(MAX_RUNS))
    if not 0 < timeout_seconds <= MAX_TIMEOUT_SECONDS:
        raise BenchmarkError(
            "timeout must be in (0, {}]".format(MAX_TIMEOUT_SECONDS)
        )
    page_size = os.sysconf("SC_PAGE_SIZE")
    for case in cases:
        validate_case(case, page_size)
    if len({case.name for case in cases}) != len(cases):
        raise BenchmarkError("case names must be unique")
    if not cases or len(cases) > MAX_CASES:
        raise BenchmarkError(
            "case count must be in [1, {}]".format(MAX_CASES)
        )
    binary_sha256_by_mode = validate_mode_binaries(modes)
    rows = []
    for run in range(1, runs + 1):
        for case_index, case in enumerate(cases):
            offset = (run - 1 + case_index) % len(modes)
            order = modes[offset:] + modes[:offset]
            for position, mode in enumerate(order, start=1):
                rows.append(
                    run_sample(
                        mode,
                        case,
                        run,
                        position,
                        timeout_seconds,
                        binary_sha256_by_mode[mode.name],
                    )
                )

    validate_samples(rows, cases, runs)
    summaries = summarize(rows, cases)
    output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(output_dir / "samples.csv", SAMPLE_FIELDS, rows)
    write_csv(output_dir / "summary.csv", SUMMARY_FIELDS, summaries)
    return rows


def positive_integer(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def bounded_timeout(text):
    value = float(text)
    if not 0 < value <= MAX_TIMEOUT_SECONDS:
        raise argparse.ArgumentTypeError(
            "timeout must be in (0, {}]".format(MAX_TIMEOUT_SECONDS)
        )
    return value


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Run the bounded RecallFS high-concurrency benchmark matrix."
    )
    parser.add_argument("--sysv", type=Path, required=True)
    parser.add_argument("--cacs", type=Path, required=True)
    parser.add_argument("--cacs-preserve-none", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runs", type=positive_integer, default=5)
    parser.add_argument(
        "--timeout-seconds", type=bounded_timeout, default=120.0
    )
    parser.add_argument(
        "--matrix", choices=("default", "smoke"), default="default"
    )
    arguments = parser.parse_args()
    if arguments.runs > MAX_RUNS:
        parser.error("runs must be <= {}".format(MAX_RUNS))
    return arguments


def main():
    arguments = parse_arguments()
    signal.signal(signal.SIGINT, raise_on_termination_signal)
    signal.signal(signal.SIGTERM, raise_on_termination_signal)
    modes = [
        Mode("sysv", arguments.sysv.resolve()),
        Mode("cacs", arguments.cacs.resolve()),
        Mode(
            "cacs-preserve-none",
            arguments.cacs_preserve_none.resolve(),
        ),
    ]
    cases = DEFAULT_CASES if arguments.matrix == "default" else SMOKE_CASES
    try:
        rows = run_matrix(
            modes=modes,
            cases=cases,
            runs=arguments.runs,
            timeout_seconds=arguments.timeout_seconds,
            output_dir=arguments.output_dir,
        )
    except (BenchmarkError, OSError) as error:
        raise SystemExit("benchmark failed: {}".format(error))
    print(
        "samples={} samples_csv={} summary_csv={}".format(
            len(rows),
            arguments.output_dir / "samples.csv",
            arguments.output_dir / "summary.csv",
        )
    )


if __name__ == "__main__":
    main()
