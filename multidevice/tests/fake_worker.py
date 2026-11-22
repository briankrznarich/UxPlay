# SPDX-License-Identifier: GPL-3.0-or-later
"""A protocol worker used to test the supervisor without AirPlay hardware."""
import os
import socketserver
import struct
import sys
import threading
import time


def parent_watch():
    for line in sys.stdin:
        if line.strip() == "STOP":
            break
    os._exit(0)


def read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        block = stream.read(size - len(data))
        if not block:
            raise EOFError
        data.extend(block)
    return bytes(data)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        identity = f"{os.getpid()} {os.environ['UXPLAY_WORKER_PEER']}\n"
        self.wfile.write(identity.encode())
        try:
            while True:
                size = struct.unpack("!I", read_exact(self.rfile, 4))[0]
                data = read_exact(self.rfile, size)
                if data == b"CRASH":
                    os._exit(7)
                if data == b"CLOSE":
                    return
                self.wfile.write(struct.pack("!I", size) + data)
        except (EOFError, ConnectionError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


threading.Thread(target=parent_watch, daemon=True).start()
mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
if mode == "invalid":
    print("not a worker record", flush=True)
    time.sleep(30)
elif mode == "oversized":
    print("x" * 20000, end="", flush=True)
    time.sleep(30)
else:
    with Server(("127.0.0.1", 0), Handler) as server:
        # Python tuple repr is also valid GVariant text for these ASCII strings.
        print(("ready", str(server.server_address[1]), "", ""), flush=True)
        print(("client", "test-id", "Phone\nwith 'quotes'", "TestPhone"), flush=True)
        server.serve_forever()
