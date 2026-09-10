#!/usr/bin/env python3

import argparse
import csv
import hashlib
import importlib.util
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


def load_harness(path):
    spec = importlib.util.spec_from_file_location("rco_high_concurrency", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("failed to load benchmark harness")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def independent_checksum(tasks, touch_bytes, page_size, yields_per_task):
    sentinel_count = (touch_bytes + page_size - 1) // page_size
    checksum = 0
    for task_id in range(1, tasks + 1):
        sentinel_sum = sum(
            ((task_id * 131) + (index * 17) + 0x5A) & 0xFF
            for index in range(sentinel_count)
        )
        checksum += sentinel_sum * (yields_per_task + 1)
    return checksum


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def wait_for_processes_to_exit(process_ids, timeout_seconds=2):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        live = []
        for process_id in process_ids:
            status_path = Path("/proc") / str(process_id) / "status"
            try:
                status = status_path.read_text(encoding="ascii")
            except FileNotFoundError:
                continue
            if "\nState:\tZ" not in status:
                live.append(process_id)
        if not live:
            return []
        time.sleep(0.02)
    return live


class ArgumentBoundsTests(unittest.TestCase):
    def run_invalid(self, arguments):
        completed = subprocess.run(
            [str(BINARIES["sysv"]), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        self.assertEqual(completed.returncode, 2, arguments)
        self.assertEqual(completed.stdout, "")
        self.assertNotEqual(completed.stderr, "")

    def test_rejects_missing_unknown_and_duplicate_arguments(self):
        self.run_invalid([])
        self.run_invalid(["--tasks"])
        self.run_invalid(
            [
                "--tasks",
                "1",
                "--tasks",
                "2",
                "--stack-bytes",
                "32768",
                "--touch-bytes",
                "4096",
            ]
        )
        self.run_invalid(
            [
                "--tasks",
                "1",
                "--stack-bytes",
                "32768",
                "--touch-bytes",
                "4096",
                "--unknown",
                "1",
            ]
        )

    def test_rejects_numeric_and_cross_field_bounds(self):
        base = {
            "--tasks": "1",
            "--stack-bytes": "32768",
            "--touch-bytes": "4096",
            "--yields-per-task": "1",
        }
        invalid = (
            ("--tasks", "0"),
            ("--tasks", "16385"),
            ("--tasks", "-1"),
            ("--stack-bytes", "16383"),
            ("--stack-bytes", "524289"),
            ("--touch-bytes", "0"),
            ("--touch-bytes", "24577"),
            ("--yields-per-task", "0"),
            ("--yields-per-task", "20000001"),
        )
        for option, value in invalid:
            arguments = []
            for name, current in base.items():
                arguments.extend((name, value if name == option else current))
            with self.subTest(option=option, value=value):
                self.run_invalid(arguments)

        self.run_invalid(
            [
                "--tasks",
                "4096",
                "--stack-bytes",
                "524288",
                "--touch-bytes",
                "135168",
                "--yields-per-task",
                "1",
            ]
        )
        self.run_invalid(
            [
                "--tasks",
                "16384",
                "--stack-bytes",
                "524288",
                "--touch-bytes",
                "4096",
                "--yields-per-task",
                "1",
            ]
        )
        self.run_invalid(
            [
                "--tasks",
                "1",
                "--stack-bytes",
                "32768",
                "--touch-bytes",
                "24576",
                "--yields-per-task",
                "1",
            ]
        )
        self.run_invalid(
            [
                "--tasks",
                "16384",
                "--stack-bytes",
                "32768",
                "--touch-bytes",
                "4096",
                "--yields-per-task",
                "1221",
            ]
        )


class CsvValidationTests(unittest.TestCase):
    def setUp(self):
        self.case = HARNESS.BenchmarkCase(
            name="unit",
            tasks=2,
            stack_bytes=32768,
            touch_bytes=4096,
            yields_per_task=3,
        )

    def row(self, mode, position):
        checksum = independent_checksum(2, 4096, 4096, 3)
        return {
            "schema": HARNESS.RESULT_SCHEMA,
            "case": "unit",
            "run": 1,
            "position": position,
            "mode": mode,
            "backend_identity": mode,
            "binary_sha256": hashlib.sha256(
                mode.encode("ascii")
            ).hexdigest(),
            "tasks": 2,
            "stack_bytes": 32768,
            "touch_bytes": 4096,
            "page_size": 4096,
            "sentinels_per_task": 1,
            "yields_per_task": 3,
            "requested_yields": 6,
            "barrier_yields": 1,
            "measured_yields": 7,
            "checksum": checksum,
            "expected_checksum": checksum,
            "runtime_switches": 26,
            "expected_runtime_switches": 26,
            "wall_ns": 14,
            "user_us": 7,
            "system_us": 0,
            "minor_faults_delta": 0,
            "major_faults_delta": 0,
            "voluntary_context_switches_delta": 0,
            "involuntary_context_switches_delta": 0,
            "spawned": 2,
            "completed": 2,
            "peak_active": 2,
            "vm_peak_kb": 1,
            "vm_hwm_kb": 1,
            "vm_size_kb": 1,
            "vm_rss_kb": 1,
            "rss_anon_kb": 1,
            "rss_file_kb": 1,
            "rss_shmem_kb": 0,
            "setup_minor_faults": 7,
            "setup_major_faults": 0,
            "smaps_rss_kb": 1,
            "smaps_pss_kb": 1,
            "smaps_private_clean_kb": 0,
            "smaps_private_dirty_kb": 1,
            "smaps_anonymous_kb": 1,
        }

    def valid_rows(self):
        return [
            self.row(mode, position)
            for position, mode in enumerate(HARNESS.MODE_NAMES, start=1)
        ]

    def test_accepts_complete_consistent_rows(self):
        rows = self.valid_rows()
        HARNESS.validate_samples(rows, [self.case], runs=1)
        summaries = HARNESS.summarize(rows, [self.case])
        self.assertEqual(
            {
                summary["median_wall_ns_per_measured_yield"]
                for summary in summaries
            },
            {2.0},
        )
        self.assertEqual(
            {
                summary["median_cpu_ns_per_measured_yield"]
                for summary in summaries
            },
            {1000.0},
        )

    def test_rejects_duplicate_missing_nonpositive_and_inconsistent_rows(self):
        mutations = []

        duplicate = self.valid_rows()
        duplicate.append(dict(duplicate[0]))
        mutations.append(duplicate)

        mutations.append(self.valid_rows()[:-1])

        missing_field = self.valid_rows()
        del missing_field[0]["wall_ns"]
        mutations.append(missing_field)

        nonpositive = self.valid_rows()
        nonpositive[0]["wall_ns"] = 0
        mutations.append(nonpositive)

        inconsistent = self.valid_rows()
        inconsistent[0]["runtime_switches"] += 2
        mutations.append(inconsistent)

        inconsistent_yields = self.valid_rows()
        inconsistent_yields[0]["measured_yields"] += 1
        mutations.append(inconsistent_yields)

        cross_mode = self.valid_rows()
        cross_mode[0]["checksum"] += 1
        mutations.append(cross_mode)

        swapped_identity = self.valid_rows()
        swapped_identity[0]["backend_identity"] = "cacs"
        mutations.append(swapped_identity)

        duplicate_binary = self.valid_rows()
        duplicate_binary[1]["binary_sha256"] = duplicate_binary[0][
            "binary_sha256"
        ]
        mutations.append(duplicate_binary)

        invalid_digest = self.valid_rows()
        invalid_digest[0]["binary_sha256"] = "not-a-sha256"
        mutations.append(invalid_digest)

        extra = self.valid_rows()
        extra[0]["unexpected"] = 1
        mutations.append(extra)

        wrong_page_size = self.valid_rows()
        wrong_page_size[0]["page_size"] = 2048
        wrong_page_size[0]["sentinels_per_task"] = 2
        wrong_checksum = independent_checksum(2, 4096, 2048, 3)
        wrong_page_size[0]["checksum"] = wrong_checksum
        wrong_page_size[0]["expected_checksum"] = wrong_checksum
        mutations.append(wrong_page_size)

        for rows in mutations:
            with self.subTest(rows=rows):
                with self.assertRaises(HARNESS.BenchmarkError):
                    HARNESS.validate_samples(rows, [self.case], runs=1)

    def test_rejects_swapped_and_duplicate_mode_binaries(self):
        swapped = [
            HARNESS.Mode("sysv", BINARIES["cacs"]),
            HARNESS.Mode("cacs", BINARIES["sysv"]),
            HARNESS.Mode(
                "cacs-preserve-none",
                BINARIES["cacs-preserve-none"],
            ),
        ]
        duplicate = [
            HARNESS.Mode("sysv", BINARIES["sysv"]),
            HARNESS.Mode("cacs", BINARIES["sysv"]),
            HARNESS.Mode(
                "cacs-preserve-none",
                BINARIES["cacs-preserve-none"],
            ),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                HARNESS.BenchmarkError, "backend identity"
            ):
                HARNESS.run_matrix(
                    modes=swapped,
                    cases=[self.case],
                    runs=1,
                    timeout_seconds=10,
                    output_dir=Path(temporary) / "swapped",
                )
            with self.assertRaisesRegex(
                HARNESS.BenchmarkError, "duplicate benchmark binaries"
            ):
                HARNESS.run_matrix(
                    modes=duplicate,
                    cases=[self.case],
                    runs=1,
                    timeout_seconds=10,
                    output_dir=Path(temporary) / "duplicate",
                )


class OracleChecksumTests(unittest.TestCase):
    def test_real_sample_matches_independent_checksum_and_switch_oracles(self):
        case = HARNESS.BenchmarkCase("oracle", 2, 32768, 4096, 3)
        row = HARNESS.run_sample(
            HARNESS.Mode("sysv", BINARIES["sysv"]),
            case,
            run=1,
            position=1,
            timeout_seconds=10,
        )
        expected_checksum = independent_checksum(2, 4096, 4096, 3)
        self.assertEqual(row["checksum"], expected_checksum)
        self.assertEqual(row["expected_checksum"], expected_checksum)
        self.assertEqual(row["backend_identity"], "sysv")
        self.assertEqual(
            row["binary_sha256"], sha256_file(BINARIES["sysv"])
        )
        self.assertEqual(row["requested_yields"], 2 * 3)
        self.assertEqual(row["barrier_yields"], 2 - 1)
        self.assertEqual(row["measured_yields"], 2 * 3 + (2 - 1))
        expected_switches = 2 * (2 * 3 + 6) + 2 * (2 - 1)
        self.assertEqual(row["runtime_switches"], expected_switches)
        self.assertEqual(row["expected_runtime_switches"], expected_switches)

    def test_rejects_executable_replacement_during_sample(self):
        case = HARNESS.BenchmarkCase("changed", 1, 32768, 4096, 1)
        expected_digest = sha256_file(BINARIES["sysv"])
        with mock.patch.object(
            HARNESS,
            "sha256_file",
            side_effect=(expected_digest, "0" * 64),
        ):
            with self.assertRaisesRegex(
                HARNESS.BenchmarkError, "changed while its sample was running"
            ):
                HARNESS.run_sample(
                    HARNESS.Mode("sysv", BINARIES["sysv"]),
                    case,
                    run=1,
                    position=1,
                    timeout_seconds=10,
                    binary_sha256=expected_digest,
                )

    def test_single_task_reports_no_end_barrier_yield(self):
        case = HARNESS.BenchmarkCase("single", 1, 32768, 4096, 1)
        row = HARNESS.run_sample(
            HARNESS.Mode("sysv", BINARIES["sysv"]),
            case,
            run=1,
            position=1,
            timeout_seconds=10,
        )
        self.assertEqual(row["requested_yields"], 1)
        self.assertEqual(row["barrier_yields"], 0)
        self.assertEqual(row["measured_yields"], 1)
        self.assertEqual(row["runtime_switches"], 8)
        self.assertEqual(row["expected_runtime_switches"], 8)


class TimeoutCleanupTests(unittest.TestCase):
    def test_parent_sigterm_reaps_stopped_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            process_ids_path = root / "process-ids.txt"
            executables = {}
            for mode in HARNESS.MODE_NAMES:
                executable = root / "{}.py".format(mode)
                executable.write_text(
                    "#!/usr/bin/env python3\n"
                    "import os\n"
                    "from pathlib import Path\n"
                    "import signal\n"
                    "import subprocess\n"
                    "import sys\n"
                    "import time\n"
                    "# mode={!s}\n"
                    "child = subprocess.Popen([\n"
                    "    sys.executable, '-c',\n"
                    "    'import signal,time; signal.signal(signal.SIGTERM, "
                    "signal.SIG_IGN); time.sleep(30)'\n"
                    "])\n"
                    "Path({!r}).write_text(\n"
                    "    '{{}} {{}}'.format(os.getpid(), child.pid)\n"
                    ")\n"
                    "os.kill(os.getpid(), signal.SIGSTOP)\n"
                    "time.sleep(30)\n".format(mode, str(process_ids_path)),
                    encoding="ascii",
                )
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
                executables[mode] = executable

            process = subprocess.Popen(
                [
                    sys.executable,
                    str(arguments.harness),
                    "--sysv",
                    str(executables["sysv"]),
                    "--cacs",
                    str(executables["cacs"]),
                    "--cacs-preserve-none",
                    str(executables["cacs-preserve-none"]),
                    "--output-dir",
                    str(root / "output"),
                    "--matrix",
                    "smoke",
                    "--runs",
                    "1",
                    "--timeout-seconds",
                    "10",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            leader_pid = None
            child_pid = None
            try:
                deadline = time.monotonic() + 2
                while True:
                    try:
                        process_ids = process_ids_path.read_text(
                            encoding="ascii"
                        ).split()
                        if len(process_ids) == 2:
                            leader_pid, child_pid = (
                                int(value) for value in process_ids
                            )
                            break
                    except (FileNotFoundError, ValueError):
                        pass
                    if process.poll() is not None:
                        self.fail("benchmark harness exited before child startup")
                    if time.monotonic() >= deadline:
                        self.fail("benchmark process ids were not published")
                    time.sleep(0.01)

                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=3)
                self.assertNotEqual(process.returncode, 0)
                self.assertEqual(stdout, "")
                self.assertIn(
                    "benchmark failed: received SIGTERM", stderr
                )

                live = wait_for_processes_to_exit((leader_pid, child_pid))
                if live:
                    self.fail(
                        "benchmark descendants survived SIGTERM: {}".format(
                            live
                        )
                    )
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=2)
                if leader_pid is not None:
                    try:
                        os.killpg(leader_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_early_exit_with_inherited_pipe_is_bounded(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "early-exit.py"
            executable.write_text(
                "#!/usr/bin/env python3\n"
                "import signal\n"
                "import subprocess\n"
                "import sys\n"
                "subprocess.Popen([\n"
                "    sys.executable, '-c',\n"
                "    'import signal,time; signal.signal(signal.SIGTERM, "
                "signal.SIG_IGN); time.sleep(30)'\n"
                "])\n"
                "raise SystemExit(7)\n",
                encoding="ascii",
            )
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            mode = HARNESS.Mode("sysv", executable)
            case = HARNESS.BenchmarkCase("early", 1, 32768, 4096, 1)
            started = time.monotonic()
            with self.assertRaisesRegex(
                HARNESS.BenchmarkError, "exited before SIGSTOP with 7"
            ):
                HARNESS.run_sample(
                    mode, case, 1, 1, timeout_seconds=1
                )
            self.assertLess(time.monotonic() - started, 2)

    def test_timeout_kills_stopped_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_pid_path = root / "child.pid"
            executable = root / "hang.py"
            executable.write_text(
                "#!/usr/bin/env python3\n"
                "import os\n"
                "from pathlib import Path\n"
                "import signal\n"
                "import subprocess\n"
                "import time\n"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "child = subprocess.Popen(['sleep', '30'])\n"
                f"Path({str(child_pid_path)!r}).write_text(str(child.pid))\n"
                "os.kill(os.getpid(), signal.SIGSTOP)\n"
                "time.sleep(30)\n",
                encoding="ascii",
            )
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            mode = HARNESS.Mode("sysv", executable)
            case = HARNESS.BenchmarkCase("timeout", 1, 32768, 4096, 1)

            with self.assertRaises(HARNESS.BenchmarkError):
                HARNESS.run_sample(mode, case, 1, 1, timeout_seconds=0.2)

            child_pid = int(child_pid_path.read_text(encoding="ascii"))
            if wait_for_processes_to_exit((child_pid,)):
                self.fail(f"child process {child_pid} survived timeout cleanup")

    def test_cleanup_kills_descendant_after_leader_exits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_pid_path = root / "child.pid"
            executable = root / "leader.py"
            executable.write_text(
                "#!/usr/bin/env python3\n"
                "import subprocess\n"
                "import sys\n"
                "import time\n"
                "child = subprocess.Popen([\n"
                "    sys.executable, '-c',\n"
                "    'import signal,time; signal.signal(signal.SIGTERM, "
                "signal.SIG_IGN); time.sleep(30)'\n"
                "])\n"
                f"open({str(child_pid_path)!r}, 'w').write(str(child.pid))\n"
                "time.sleep(30)\n",
                encoding="ascii",
            )
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            process = subprocess.Popen(
                [str(executable)],
                start_new_session=True,
            )
            deadline = time.monotonic() + 2
            while not child_pid_path.exists():
                if time.monotonic() >= deadline:
                    self.fail("descendant pid was not published")
                time.sleep(0.01)
            child_pid = int(child_pid_path.read_text(encoding="ascii"))

            HARNESS.terminate_process_group(process)

            if wait_for_processes_to_exit((child_pid,)):
                self.fail(f"descendant process {child_pid} survived cleanup")


class RealProcessIntegrationTests(unittest.TestCase):
    def test_three_modes_emit_complete_csv_and_match_independent_oracle(self):
        case = HARNESS.BenchmarkCase("smoke", 3, 32768, 4096, 4)
        modes = [
            HARNESS.Mode(name, BINARIES[name]) for name in HARNESS.MODE_NAMES
        ]

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            HARNESS.run_matrix(
                modes=modes,
                cases=[case],
                runs=3,
                timeout_seconds=10,
                output_dir=output,
            )
            with (output / "samples.csv").open(newline="", encoding="ascii") as source:
                samples = list(csv.DictReader(source))
            with (output / "summary.csv").open(newline="", encoding="ascii") as source:
                summary = list(csv.DictReader(source))

        self.assertEqual(len(samples), 9)
        self.assertEqual(len(summary), 3)
        self.assertEqual(
            {float(row["paired_wall_ratio_vs_sysv"]) for row in summary
             if row["mode"] == "sysv"},
            {1.0},
        )
        self.assertEqual(
            {float(row["paired_cpu_ratio_vs_sysv"]) for row in summary
             if row["mode"] == "sysv"},
            {1.0},
        )
        expected = independent_checksum(3, 4096, 4096, 4)
        self.assertEqual({int(row["checksum"]) for row in samples}, {expected})
        self.assertEqual(
            {row["backend_identity"] for row in samples},
            set(HARNESS.MODE_NAMES),
        )
        self.assertEqual(
            {
                row["mode"]: row["backend_identity"]
                for row in samples
            },
            {name: name for name in HARNESS.MODE_NAMES},
        )
        self.assertEqual(
            len({row["binary_sha256"] for row in samples}),
            len(HARNESS.MODE_NAMES),
        )
        self.assertEqual(
            {int(row["requested_yields"]) for row in samples}, {12}
        )
        self.assertEqual(
            {int(row["barrier_yields"]) for row in samples}, {2}
        )
        self.assertEqual(
            {int(row["measured_yields"]) for row in samples}, {14}
        )
        self.assertEqual(
            {int(row["runtime_switches"]) for row in samples}, {46}
        )
        self.assertEqual({int(row["spawned"]) for row in samples}, {3})
        self.assertEqual({int(row["completed"]) for row in samples}, {3})
        self.assertEqual({int(row["peak_active"]) for row in samples}, {3})
        self.assertEqual(
            {
                row["mode"]: row["backend_identity"]
                for row in summary
            },
            {name: name for name in HARNESS.MODE_NAMES},
        )
        self.assertEqual(
            len({row["binary_sha256"] for row in summary}),
            len(HARNESS.MODE_NAMES),
        )
        self.assertEqual(
            {int(row["requested_yields"]) for row in summary}, {12}
        )
        self.assertEqual(
            {int(row["barrier_yields"]) for row in summary}, {2}
        )
        self.assertEqual(
            {int(row["measured_yields"]) for row in summary}, {14}
        )
        self.assertEqual(
            [row["mode"] for row in samples],
            [
                "sysv",
                "cacs",
                "cacs-preserve-none",
                "cacs",
                "cacs-preserve-none",
                "sysv",
                "cacs-preserve-none",
                "sysv",
                "cacs",
            ],
        )


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--sysv", type=Path, required=True)
    parser.add_argument("--cacs", type=Path, required=True)
    parser.add_argument("--cacs-preserve-none", type=Path, required=True)
    parser.add_argument(
        "suite",
        choices=("arguments", "oracle", "csv", "timeout", "integration", "all"),
    )
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_arguments()
    HARNESS = load_harness(arguments.harness)
    BINARIES = {
        "sysv": arguments.sysv,
        "cacs": arguments.cacs,
        "cacs-preserve-none": arguments.cacs_preserve_none,
    }
    suites = {
        "arguments": ArgumentBoundsTests,
        "oracle": OracleChecksumTests,
        "csv": CsvValidationTests,
        "timeout": TimeoutCleanupTests,
        "integration": RealProcessIntegrationTests,
    }
    selected = suites.values() if arguments.suite == "all" else (
        suites[arguments.suite],
    )
    test_suite = unittest.TestSuite(
        unittest.defaultTestLoader.loadTestsFromTestCase(test_case)
        for test_case in selected
    )
    result = unittest.TextTestRunner(verbosity=2).run(test_suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
