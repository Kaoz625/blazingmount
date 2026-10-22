#!/usr/bin/env python3
"""test_link.py - build and run the host tests of the PS5 SMB-link engine (blz_http.cpp + blz_link_core.cpp) against a mock HTTP Range server with fault routes.

    python3 -I tools/smblink/module/test_link.py [--keep] [--no-sanitize] [--real-imgserve HOST:PORT IMAGE.ffpfsc INNERPATH]

The mock serves /v/T.ffpfsc/<rel> from memory (the same URL shape as tools/imgserve): normal Range replies, keep-alive, plus fault routes (headers one byte at a time,
Connection: close, a connection dropped before the first reply, a truncated body, a wrong Content-Length, 404). Exit code 0 only if every check in test_link.cpp passed.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 3_000_017


def make_data():
    r = random.Random(1)
    return bytes(r.getrandbits(8) for _ in range(SIZE))


DATA = make_data()
MUTATED = bytearray(DATA)
MUTATED[70000] ^= 0xFF
MUTATED = bytes(MUTATED)
OTHER = bytes(range(100))
STATE = {"dropfirst": 0}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        path = urllib.parse.unquote(self.path)
        prefix = "/v/T.ffpfsc/"
        if not path.startswith(prefix):
            return self.fail(404)
        rel = path[len(prefix):]
        if rel in ("data.bin", "slow.bin", "close.bin", "dropfirst.bin", "short.bin", "wronglen.bin", "delay.bin"):
            body = DATA
        elif rel == "mutated.bin":
            body = MUTATED
        elif rel == "dir with space/other.bin":
            body = OTHER
        else:
            return self.fail(404)
        if rel == "dropfirst.bin":
            STATE["dropfirst"] += 1
            if STATE["dropfirst"] == 1:
                self.close_connection = True
                self.wfile.flush()
                self.connection.shutdown(2)
                return
        rng = self.headers.get("Range", "")
        a, b = 0, len(body) - 1
        status = 200
        if rng.startswith("bytes="):
            spec = rng[6:].split(",")[0]
            lo, _, hi = spec.partition("-")
            a = int(lo)
            b = min(int(hi), len(body) - 1) if hi else len(body) - 1
            if a >= len(body) or b < a:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % len(body))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            status = 206
        chunk = body[a:b + 1]
        head = "HTTP/1.1 %d %s\r\n" % (status, "Partial Content" if status == 206 else "OK")
        head += "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
        if status == 206:
            head += "Content-Range: bytes %d-%d/%d\r\n" % (a, b, len(body))
        if rel == "wronglen.bin":
            head += "Content-Length: 50\r\n\r\n"
            self.wfile.write(head.encode() + chunk[:50])
            return
        head += "Content-Length: %d\r\n" % len(chunk)
        if rel == "close.bin":
            head += "Connection: close\r\n"
            self.close_connection = True
        head += "\r\n"
        if rel == "delay.bin":
            time.sleep(0.15)
        if rel == "slow.bin":
            for ch in head.encode():
                self.wfile.write(bytes([ch]))
                self.wfile.flush()
                time.sleep(0.0005)
            self.wfile.write(chunk)
            return
        if rel == "short.bin":
            self.wfile.write(head.encode() + chunk[: len(chunk) // 2])
            self.wfile.flush()
            self.close_connection = True
            self.connection.shutdown(2)
            return
        self.wfile.write(head.encode() + chunk)

    def fail(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-sanitize", action="store_true")
    ap.add_argument("--tsan", action="store_true", help="ThreadSanitizer instead of ASan/UBSan (the async workers, the cache states)")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="smblink-")
    with open(os.path.join(work, "data.bin"), "wb") as f:
        f.write(DATA)
    with open(os.path.join(work, "mutated.bin"), "wb") as f:
        f.write(MUTATED)
    exe = os.path.join(work, "test_link")
    cmd = ["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-pthread"]
    if a.tsan:
        cmd += ["-fsanitize=thread", "-fno-omit-frame-pointer"]
    elif not a.no_sanitize:
        cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    cmd += [os.path.join(HERE, "test_link.cpp"), os.path.join(HERE, "blz_http.cpp"), os.path.join(HERE, "blz_link_core.cpp"), "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("compile failed")
    if r.stderr.strip():
        print("compiler warnings:\n" + r.stderr)
    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    port = srv.server_address[1]
    p = subprocess.run([exe, "127.0.0.1", str(port), work], capture_output=True, text=True, timeout=300)
    print(p.stdout + p.stderr)
    srv.shutdown()
    if not a.keep:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(p.returncode)


if __name__ == "__main__":
    main()
