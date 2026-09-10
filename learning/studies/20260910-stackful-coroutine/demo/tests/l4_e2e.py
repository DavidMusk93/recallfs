#!/usr/bin/env python3

import argparse
import concurrent.futures
import re
import select
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.server.connection_opened()
        try:
            received = bytearray()
            while True:
                data = self.request.recv(65536)
                if not data:
                    break
                received.extend(data)
            self.request.sendall(received)
            self.request.shutdown(socket.SHUT_WR)
        finally:
            self.server.connection_closed()


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 512

    def __init__(self, server_address, handler):
        super().__init__(server_address, handler)
        self.active_condition = threading.Condition()
        self.active_connections = 0

    def connection_opened(self):
        with self.active_condition:
            self.active_connections += 1
            self.active_condition.notify_all()

    def connection_closed(self):
        with self.active_condition:
            self.active_connections -= 1
            self.active_condition.notify_all()

    def wait_for_active(self, expected, timeout=2):
        deadline = time.monotonic() + timeout
        with self.active_condition:
            while self.active_connections != expected:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(
                        "backend active connections stayed at {}, expected {}".format(
                            self.active_connections, expected
                        )
                    )
                self.active_condition.wait(remaining)


def reserve_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_listener(port, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("forwarder exited before accepting connections")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("forwarder did not listen within five seconds")


def wait_for_startup_output(process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("forwarder exited before reporting startup")
        readable, _, _ = select.select([process.stdout], [], [], 0.1)
        if readable and process.stdout.readline().startswith("listening="):
            return
    raise RuntimeError("forwarder did not report startup within five seconds")


def forwarder_command(
    binary, proxy_port, backend_port, grace_ms, max_connections=256
):
    return [
        binary,
        "--listen-host",
        "127.0.0.1",
        "--listen-port",
        str(proxy_port),
        "--upstream-host",
        "127.0.0.1",
        "--upstream-port",
        str(backend_port),
        "--max-connections",
        str(max_connections),
        "--buffer-size",
        "32768",
        "--connect-timeout-ms",
        "1000",
        "--grace-ms",
        str(grace_ms),
    ]


def round_trip(port, seed, size):
    payload = bytes(((seed + index * 17) & 0xFF) for index in range(size))
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall(payload)
        sock.shutdown(socket.SHUT_WR)
        received = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            received.extend(chunk)
    if received != payload:
        raise AssertionError(
            "payload mismatch: expected {} bytes, received {}".format(
                len(payload), len(received)
            )
        )
    return len(received)


def check_backend_identity(binary, expected):
    result = subprocess.run(
        [binary, "--backend-identity"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        timeout=2,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            "backend identity query exited {}: {}".format(
                result.returncode, result.stderr
            )
        )
    if result.stdout != expected + "\n" or result.stderr:
        raise AssertionError(
            "backend identity query returned stdout={!r} stderr={!r}, "
            "expected {!r}".format(result.stdout, result.stderr, expected)
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("forwarder")
    parser.add_argument("expected_backend_identity", nargs="?")
    args = parser.parse_args()
    if args.expected_backend_identity is not None:
        check_backend_identity(args.forwarder, args.expected_backend_identity)

    backend_port = reserve_port()
    proxy_port = reserve_port()
    backend = EchoServer(("127.0.0.1", backend_port), EchoHandler)
    backend_thread = threading.Thread(target=backend.serve_forever, daemon=True)
    backend_thread.start()

    command = forwarder_command(args.forwarder, proxy_port, backend_port, 1000)
    process = None
    process_collected = False
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        wait_for_listener(proxy_port, process)
        sizes = [0, 1, 63, 4096, 65537, 262144]
        for index, size in enumerate(sizes):
            round_trip(proxy_port, index + 1, size)

        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            futures = [
                pool.submit(round_trip, proxy_port, index + 100, 262144)
                for index in range(32)
            ]
            total = sum(future.result() for future in futures)
        if total != 32 * 262144:
            raise AssertionError("concurrent byte count mismatch")

        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=5)
        process_collected = True
        if process.returncode != 0:
            raise RuntimeError(
                "forwarder exited {}:\n{}\n{}".format(
                    process.returncode, stdout, stderr
                )
            )
        if "accepted=" not in stdout or "bytes_client_to_upstream=" not in stdout:
            raise AssertionError("forwarder summary is missing counters")
        print("l4 e2e passed: {} concurrent payload bytes".format(total))
        print(stdout.strip())

        capacity_port = reserve_port()
        process = subprocess.Popen(
            forwarder_command(
                args.forwarder,
                capacity_port,
                backend_port,
                1000,
                max_connections=1,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        process_collected = False
        wait_for_startup_output(process)
        held_connection = socket.create_connection(
            ("127.0.0.1", capacity_port), timeout=2
        )
        held_connection.settimeout(2)
        held_connection.sendall(b"held")
        backend.wait_for_active(1)
        with socket.create_connection(
            ("127.0.0.1", capacity_port), timeout=2
        ) as excess_connection:
            excess_connection.settimeout(1)
            try:
                excess_data = excess_connection.recv(1)
            except ConnectionResetError:
                excess_data = b""
            if excess_data:
                raise AssertionError("excess connection received unexpected data")
        held_connection.shutdown(socket.SHUT_WR)
        held_response = bytearray()
        while True:
            chunk = held_connection.recv(16)
            if not chunk:
                break
            held_response.extend(chunk)
        held_connection.close()
        if held_response != b"held":
            raise AssertionError("established connection did not survive rejection")
        backend.wait_for_active(0)
        process.send_signal(signal.SIGTERM)
        capacity_stdout, capacity_stderr = process.communicate(timeout=5)
        process_collected = True
        if process.returncode != 0:
            raise RuntimeError(
                "capacity forwarder exited {}:\n{}\n{}".format(
                    process.returncode, capacity_stdout, capacity_stderr
                )
            )
        summaries = re.findall(r"^accepted=.*$", capacity_stdout, re.MULTILINE)
        if len(summaries) != 1:
            raise AssertionError("capacity run emitted {} summaries".format(len(summaries)))
        rejected_match = re.search(r"\brejected=(\d+)\b", summaries[0])
        if rejected_match is None or int(rejected_match.group(1)) < 1:
            raise AssertionError("capacity run did not count rejection")
        print("l4 capacity rejection passed")

        forced_port = reserve_port()
        process = subprocess.Popen(
            forwarder_command(args.forwarder, forced_port, backend_port, 100),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        process_collected = False
        wait_for_listener(forced_port, process)
        held_connection = socket.create_connection(
            ("127.0.0.1", forced_port), timeout=2
        )
        time.sleep(0.1)
        process.send_signal(signal.SIGTERM)
        forced_stdout, forced_stderr = process.communicate(timeout=3)
        process_collected = True
        held_connection.close()
        if process.returncode != 0:
            raise RuntimeError(
                "forced-drain forwarder exited {}:\n{}\n{}".format(
                    process.returncode, forced_stdout, forced_stderr
                )
            )
        if "forced_shutdown=true" not in forced_stdout:
            raise AssertionError("grace deadline did not force shutdown")
        print("l4 forced-drain passed")
    finally:
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        if process is not None and not process_collected:
            stdout, stderr = process.communicate()
            if stdout:
                print(stdout.strip(), file=sys.stderr)
            if stderr:
                print(stderr.strip(), file=sys.stderr)
        backend.shutdown()
        backend.server_close()
        backend_thread.join()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("l4 e2e failed: {}".format(error), file=sys.stderr)
        sys.exit(1)
