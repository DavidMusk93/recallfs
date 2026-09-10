#!/usr/bin/env python3

import argparse
import resource
import select
import signal
import socket
import subprocess
import sys
import time


MINIMUM_HIGH_NOFILE = 262144
TARGET_HIGH_NOFILE = 1048576
MAX_RLIMIT_VM_GROWTH_KB = 1024


def reserve_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def read_vm_size_kb(pid):
    with open("/proc/{}/status".format(pid), encoding="ascii") as status:
        for line in status:
            if line.startswith("VmSize:"):
                return int(line.split()[1])
    raise RuntimeError("VmSize is missing for pid {}".format(pid))


def wait_for_startup(process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                "forwarder exited before startup:\n{}\n{}".format(stdout, stderr)
            )
        readable, _, _ = select.select([process.stdout], [], [], 0.1)
        if readable:
            line = process.stdout.readline()
            if line.startswith("listening="):
                return line
    raise RuntimeError("forwarder did not report startup within five seconds")


def measure_startup_vm(binary, nofile_limit):
    _, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)

    def set_child_limit():
        resource.setrlimit(
            resource.RLIMIT_NOFILE,
            (nofile_limit, hard_limit),
        )

    port = reserve_port()
    command = [
        binary,
        "--listen-host",
        "127.0.0.1",
        "--listen-port",
        str(port),
        "--upstream-host",
        "127.0.0.1",
        "--upstream-port",
        "1",
        "--max-connections",
        "1",
        "--buffer-size",
        "4096",
        "--connect-timeout-ms",
        "100",
        "--grace-ms",
        "100",
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        preexec_fn=set_child_limit,
    )
    try:
        startup = wait_for_startup(process)
        vm_size_kb = read_vm_size_kb(process.pid)
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=3)
        if process.returncode != 0:
            raise RuntimeError(
                "forwarder exited {}:\n{}{}\n{}".format(
                    process.returncode, startup, stdout, stderr
                )
            )
        if "forced_shutdown=false" not in stdout:
            raise AssertionError("normal startup probe forced shutdown")
        return vm_size_kb
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("forwarder")
    args = parser.parse_args()

    soft_limit, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard_limit == resource.RLIM_INFINITY:
        high_limit = TARGET_HIGH_NOFILE
    else:
        high_limit = min(hard_limit, TARGET_HIGH_NOFILE)
    if high_limit < MINIMUM_HIGH_NOFILE:
        print(
            "skipped: RLIMIT_NOFILE hard limit {} is below {}".format(
                hard_limit, MINIMUM_HIGH_NOFILE
            )
        )
        return 77

    low_limit = (
        1024
        if soft_limit == resource.RLIM_INFINITY
        else min(soft_limit, 1024)
    )
    if low_limit < 66:
        low_limit = 66
    low_vm_kb = measure_startup_vm(args.forwarder, low_limit)
    high_vm_kb = measure_startup_vm(args.forwarder, high_limit)
    growth_kb = high_vm_kb - low_vm_kb
    if growth_kb > MAX_RLIMIT_VM_GROWTH_KB:
        raise AssertionError(
            "raising RLIMIT_NOFILE from {} to {} grew VmSize by {} KiB "
            "(low={} KiB high={} KiB)".format(
                low_limit, high_limit, growth_kb, low_vm_kb, high_vm_kb
            )
        )
    print(
        "epoll RLIMIT startup passed: low={} KiB high={} KiB growth={} KiB".format(
            low_vm_kb, high_vm_kb, growth_kb
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print("epoll RLIMIT startup failed: {}".format(error), file=sys.stderr)
        sys.exit(1)
