#!/usr/bin/env python3

import argparse
import concurrent.futures
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        received = bytearray()
        while True:
            data = self.request.recv(65536)
            if not data:
                break
            received.extend(data)
        self.request.sendall(received)
        self.request.shutdown(socket.SHUT_WR)


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 512


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


def forwarder_command(binary, proxy_port, backend_port, grace_ms):
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
        "256",
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("forwarder")
    args = parser.parse_args()

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
