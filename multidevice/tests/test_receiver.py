# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional real-receiver smoke test. Briefly advertises a unique Bonjour name.

Run: python3 multidevice/tests/test_receiver.py build/uxplay
Requires working Bonjour/Avahi, GStreamer, IPv4/IPv6 and local socket access.
Does NOT replace simultaneous mirroring tests with real iPhones.
"""
import ast
import os
import plistlib
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        block = sock.recv(size - len(data))
        if not block:
            raise EOFError
        data.extend(block)
    return bytes(data)


def info(sock, seq=1):
    sock.sendall(f"GET /info RTSP/1.0\r\nCSeq: {seq}\r\n\r\n".encode())
    header = bytearray()
    while not header.endswith(b"\r\n\r\n"):
        header.extend(read_exact(sock, 1))
        assert len(header) < 16384
    assert header.startswith(b"RTSP/1.0 200"), header
    size = int(re.search(rb"(?i)Content-Length: (\d+)", header)[1])
    return plistlib.loads(read_exact(sock, size))


def main(binary):
    name = "UxPlay-multidevice-test-" + uuid.uuid4().hex[:8]
    # Do not read a user's regular UxPlay configuration.
    with tempfile.NamedTemporaryFile() as config, tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([os.path.abspath(binary), "-rc", config.name, "-multi", "2",
            "-n", name, "-nh", "-m", "02:ff:00:ab:cd:ef", "-vs", "fakesink", "-vsync", "no"],
            stdout=subprocess.PIPE, stderr=log, text=True)
        lines = queue.Queue()
        def read():
            for line in process.stdout:
                log.write(line)
                log.flush()
                lines.put(line)
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        sockets = []
        try:
            deadline = time.monotonic() + 15
            while True:
                line = lines.get(timeout=max(0.1, deadline - time.monotonic()))
                match = re.search(r"listening on port (\d+)", line)
                if match:
                    port = int(match[1])
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError("Receiver startup")
            results = []
            for family, address in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")):
                sock = socket.socket(family, socket.SOCK_STREAM)
                sockets.append(sock)
                sock.settimeout(15)
                sock.connect((address, port))
                results.append(info(sock))
            assert results[0]["name"] == results[1]["name"] == name, results
            assert results[0]["pk"] == results[1]["pk"], "Workers disagree on receiver key"
            assert results[0]["deviceID"] == results[1]["deviceID"], "Workers disagree on receiver ID"
            # Reconnection on one peer must not evict the other peer.
            sockets[0].close()
            time.sleep(0.5)
            replacement = socket.create_connection(("127.0.0.1", port), timeout=10)
            sockets.append(replacement)
            assert info(replacement)["pk"] == results[0]["pk"]
            assert info(sockets[1], 2)["pk"] == results[1]["pk"]
            print("PASS: two real RAOP workers, shared identity, independent control and reconnect")
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            for sock in sockets:
                sock.close()
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("Receiver did not shut down")
            reader.join(timeout=2)
            process.stdout.close()
        assert process.returncode == 0, process.returncode


def single(binary):
    """Exercise the unmodified single-receiver entry path, with either build."""
    name = "UxPlay-single-test-" + uuid.uuid4().hex[:8]
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    assert 1025 < port < 65534
    with tempfile.NamedTemporaryFile() as config, tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([os.path.abspath(binary), "-rc", config.name,
            "-n", name, "-nh", "-m", "02:ff:00:ab:cd:ef", "-vs", "fakesink", "-as", "0",
            "-p", "tcp", f"{port-1},{port},{port+1}", "-vsync", "no"], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                    break
                except ConnectionRefusedError:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise
                    time.sleep(0.1)
            with sock:
                sock.settimeout(10)
                assert info(sock)["name"] == name
            print("PASS: normal single-device receiver: " + binary)
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            process.terminate()
            process.wait(timeout=10)
        assert process.returncode == 0, process.returncode


def worker_parent_eof(binary):
    """Exercise the actual worker's orphan-prevention pipe, without discovery."""
    with tempfile.NamedTemporaryFile() as config, tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([os.path.abspath(binary), "-rc", config.name,
            "-n", "Unadvertised test worker", "-nh", "-m", "02:ff:00:ab:cd:ef",
            "-vs", "fakesink", "-vsync", "no"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=log, text=True, env={**os.environ, "UXPLAY_WORKER_PEER": "127.0.0.1"})
        try:
            events = queue.Queue()
            def read():
                for line in process.stdout:
                    events.put(line)
            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            event = ast.literal_eval(events.get(timeout=15))
            assert event[0] == "ready", event
            with socket.create_connection(("127.0.0.1", int(event[1])), timeout=10) as sock:
                assert info(sock)["name"] == "Unadvertised test worker"
                process.stdin.close()
                process.wait(timeout=5)
                assert sock.recv(1) == b""
            assert process.returncode == 0, process.returncode
            print("PASS: actual worker exits on parent pipe EOF")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if not process.stdin.closed:
                process.stdin.close()
            reader.join(timeout=2)
            process.stdout.close()


if __name__ == "__main__":
    if "--single" in sys.argv:
        single(sys.argv[1])
    else:
        main(sys.argv[1])
        worker_parent_eof(sys.argv[1])
