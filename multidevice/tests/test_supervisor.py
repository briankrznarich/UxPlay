# SPDX-License-Identifier: GPL-3.0-or-later
"""Transport/lifecycle integration tests; these do not simulate AirPlay pairing."""
import concurrent.futures
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

HARNESS, WORKER = sys.argv[1:3]
sys.argv = sys.argv[:1]


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        block = sock.recv(size - len(data))
        if not block:
            raise EOFError
        data.extend(block)
    return bytes(data)


def exchange(sock, data):
    sock.sendall(struct.pack("!I", len(data)) + data)
    size = struct.unpack("!I", exact(sock, 4))[0]
    return exact(sock, size)


class Harness:
    def __init__(self, mode="normal", capacity=2):
        self.process = subprocess.Popen([HARNESS, sys.executable, WORKER, mode],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True, bufsize=1,
                                        env={**os.environ, "UXPLAY_TEST_CAPACITY": str(capacity)})
        self.events = queue.Queue()
        self.lines = []
        ready = self.process.stdout.readline().split()
        if len(ready) != 2 or ready[0] != "READY":
            self.process.stdin.close()
            self.process.stdout.close()
            self.process.wait(timeout=5)
            raise RuntimeError("Test listener failed to start (local socket access is required)")
        self.port = int(ready[1])
        self.sockets = []
        self.pids = set()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            self.lines.append(line.strip().split())
            self.events.put(self.lines[-1])

    def event(self, kind, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            item = self.events.get(timeout=deadline - time.monotonic())
            if item[1] == kind:
                return item
        raise AssertionError(f"Missing {kind} event: {self.lines}")

    def connect(self, ipv6=False, hello=True, source=None):
        sock = socket.socket(socket.AF_INET6 if ipv6 else socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(8)
        self.sockets.append(sock)
        if source:
            sock.bind((source, 0))
        sock.connect(("::1" if ipv6 else "127.0.0.1", self.port))
        if not hello:
            return sock
        line = bytearray()
        while not line.endswith(b"\n"):
            line.extend(exact(sock, 1))
        pid, peer = line.decode().strip().split()
        self.pids.add(int(pid))
        return sock, int(pid), peer

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.command("STOP")
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("Supervisor failed to shut down")
        finally:
            for sock in self.sockets:
                sock.close()
            self.process.stdin.close()
            self.thread.join(timeout=2)
            self.process.stdout.close()


class SupervisorTests(unittest.TestCase):
    def start(self, mode="normal", capacity=2):
        harness = Harness(mode, capacity)
        self.addCleanup(harness.close)
        return harness

    def test_private_worker_messages(self):
        h = self.start()
        a, _, _ = h.connect()
        session = h.event("client")[2]
        payload = '{"token":"123456789", "text":"quoted \"value\""}'
        h.command("SEND " + session + " " + payload)
        h.event("message")
        deadline = time.monotonic() + 3
        while not any(line[0] == "PAYLOAD" for line in h.lines) and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIn(["PAYLOAD"] + payload.split(), h.lines)
        self.assertEqual(exchange(a, b"still streaming"), b"still streaming")

    def test_peer_isolation_reuse_and_binary_duplex(self):
        h = self.start()
        a, pid_a, peer_a = h.connect()
        same, pid_same, _ = h.connect()
        b, pid_b, peer_b = h.connect(ipv6=True)
        self.assertEqual(pid_a, pid_same)
        self.assertNotEqual(pid_a, pid_b)
        self.assertEqual((peer_a, peer_b), ("127.0.0.1", "::1"))
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            payloads = [os.urandom(1024 * 1024) for _ in range(3)]
            results = list(pool.map(exchange, [a, same, b], payloads))
        self.assertEqual(results, payloads)
        # Preserve long-lived control connections past the connect timeout.
        time.sleep(6)
        self.assertEqual(exchange(a, b"still connected"), b"still connected")

    def test_crash_isolation_and_reconnect(self):
        h = self.start()
        a, old_pid, _ = h.connect()
        b, _, _ = h.connect(ipv6=True)
        with self.assertRaises((EOFError, ConnectionError)):
            exchange(a, b"CRASH")
        h.event("stopped")
        self.assertEqual(exchange(b, b"unaffected"), b"unaffected")
        new, new_pid, _ = h.connect()
        self.assertNotEqual(old_pid, new_pid)
        self.assertEqual(exchange(new, b"reconnected"), b"reconnected")

    def test_explicit_disconnect_and_idle_cleanup(self):
        h = self.start()
        a, _, _ = h.connect()
        started = h.event("starting")
        h.command("DROP " + started[2])
        h.event("stopped")
        with self.assertRaises((EOFError, ConnectionError)):
            exact(a, 1)
        b, _, _ = h.connect()
        b.close()
        h.event("stopped")

    def test_backend_eof_closes_client(self):
        h = self.start()
        a, _, _ = h.connect()
        with self.assertRaises((EOFError, ConnectionError)):
            exchange(a, b"CLOSE")

    def test_worker_capacity(self):
        h = self.start(capacity=1)
        a, _, _ = h.connect()
        refused = h.connect(ipv6=True, hello=False)
        with self.assertRaises((EOFError, ConnectionError)):
            exact(refused, 1)
        self.assertEqual(exchange(a, b"at capacity"), b"at capacity")

    def test_connection_cap(self):
        h = self.start()
        for _ in range(8):
            h.connect()
        refused = h.connect(hello=False)
        with self.assertRaises((EOFError, ConnectionError)):
            exact(refused, 1)

    def test_invalid_and_oversized_worker_records(self):
        for mode in ("invalid", "oversized"):
            with self.subTest(mode=mode):
                h = self.start(mode)
                sock = h.connect(hello=False)
                h.event("stopped")
                with self.assertRaises((EOFError, ConnectionError)):
                    exact(sock, 1)

    @unittest.skipIf(os.name == "nt", "POSIX process-existence check")
    def test_parent_crash_closes_workers(self):
        h = self.start()
        a, pid, _ = h.connect()
        h.process.kill()
        h.process.wait(timeout=5)
        with self.assertRaises((EOFError, ConnectionError)):
            exact(a, 1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        else:
            self.fail("Worker survived parent crash")


if __name__ == "__main__":
    unittest.main(verbosity=2)
