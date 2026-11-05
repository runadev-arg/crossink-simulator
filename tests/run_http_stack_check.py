#!/usr/bin/env python3
"""Check the simulator's HTTP clients and storage shim against a real server.

Builds tests/http_stack_check.cpp with the simulator's own sources, serves a local
HTTP endpoint from this script and fails unless both sides agree on what crossed
the wire. No SDL needed.
"""
import base64
import hashlib
import http.server
import os
import random
import re
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIG = random.Random(4586).randbytes(5 * 1024 * 1024)
BIG_MD5 = hashlib.md5(BIG).hexdigest()
SEEN = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", headers=None):
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def echo(self):
        data = self.body()
        custom = self.headers.get("X-Custom", "").encode("latin-1").decode("utf-8")
        SEEN.append((self.command, hashlib.md5(data).hexdigest(), len(data), custom,
                     self.headers.get("Expect")))
        text = f"{self.command} {hashlib.md5(data).hexdigest()} {len(data)} {custom}"
        self.reply(200, text.encode("utf-8"),
                   {"X-Seen-User-Agent": self.headers.get("User-Agent", "")})

    def serve_big(self):
        pace = "pace=1" in self.path
        start = 0
        status = 200
        extra = {"X-Content-MD5": BIG_MD5}
        match = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range") or "")
        if match:
            start = int(match.group(1))
            status = 206
            extra["Content-Range"] = f"bytes {start}-{len(BIG) - 1}/{len(BIG)}"
        payload = BIG[start:]
        self.send_response(status)
        for name, value in extra.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        step = 512 * 1024
        try:
            for offset in range(0, len(payload), step):
                self.wfile.write(payload[offset:offset + step])
                self.wfile.flush()
                if pace:
                    time.sleep(0.12)
        except OSError:
            self.close_connection = True

    def route(self):
        path = self.path.split("?")[0]
        if path == "/hello":
            self.reply(200, b"hello world", {"X-Hello": "yes"})
        elif path == "/echo-md5":
            self.echo()
        elif path == "/big":
            self.serve_big()
        elif path == "/redirect":
            self.reply(302, b"", {"Location": "/hello"})
        elif path == "/auth":
            self.reply(200, (self.headers.get("Authorization") or "").encode())
        else:
            self.reply(404, b"nope")

    do_GET = do_POST = do_PUT = do_DELETE = route


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def build(output):
    sources = ["tests/http_stack_check.cpp", "src/HalStorage.cpp", "src/SimFs.cpp",
               "src/NetworkClient.cpp"]
    libs = []
    if os.name == "nt":
        sources += ["src/SimHttpFetchWin.cpp", "src/SimSocket.cpp"]
        libs = ["-static", "-lws2_32"]
    else:
        libs = ["-lssl", "-lcrypto", "-pthread"]
    subprocess.run([os.environ.get("CXX", "g++"), "-std=gnu++2a", "-O1", "-g",
                    "-DSIMULATOR", "-Isrc", *sources, "-o", str(output), *libs],
                   cwd=ROOT, check=True)


def main():
    server = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    refused = f"http://127.0.0.1:{free_port()}"

    with tempfile.TemporaryDirectory(prefix="sim-http-check-") as temp:
        binary = Path(temp) / ("check.exe" if os.name == "nt" else "check")
        print("Building...", flush=True)
        build(binary)
        env = os.environ.copy()
        env["CROSSPOINT_SIM_SD"] = str(Path(temp) / "sd")
        env.pop("CROSSINK_SIM_HTTP_MOCK_ROOT", None)
        result = subprocess.run([str(binary), base, refused], env=env, timeout=120)
        if result.returncode != 0:
            return result.returncode
        if os.path.exists(Path(temp) / "sd" / "dl" / "stream.bin"):
            on_disk = (Path(temp) / "sd" / "dl" / "stream.bin").read_bytes()
            if on_disk != BIG:
                print("FAIL downloaded file differs from what the server sent")
                return 1
            print("PASS byte-for-byte download seen from outside the simulator")
        leaked = [entry for entry in SEEN if entry[4] is not None]
        if leaked:
            print(f"FAIL curl sent an Expect header: {leaked}")
            return 1
        print(f"PASS server saw {len(SEEN)} echo requests without Expect")
    server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
