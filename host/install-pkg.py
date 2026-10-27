#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""install-pkg.py - install a PKG on a PS5 straight from THIS computer (BGFT-style: this computer serves the file, the console's own
installer reads it through BlazingMount; nothing is copied onto the console first). Live %, speed, ETA; Ctrl-C cancels.

    python3 host/install-pkg.py <console ip> <file.pkg | file.pkg.0 (split set)> [--name "Shown name"] [--port 0] [--bind <this ip>]
    python3 host/install-pkg.py <console ip> --shelf "Apps and Emulators/PS5/x.pkg"   (a pkg the host's imgserve already lists)
    python3 host/install-pkg.py <console ip> --status | --cancel

Needs blazingmountd >= 2.10 on the console (GET/POST /api/install). Exit: 0 installed, 1 failed, 2 cancelled, 3 refused/unreachable.
A split set X.pkg.0, X.pkg.1, ... (cut for FAT32) is served as ONE file. Only the named file is served, under a random path.
"""
import argparse, http.server, json, os, secrets, socket, sys, threading, time, urllib.error, urllib.parse, urllib.request

API_PORT = 8094


class Parts:
    """One virtual file over a split set (or a single file)."""
    def __init__(self, first):
        if first.endswith(".0") and os.path.exists(first):
            base, files, i = first[:-2], [], 0
            while os.path.exists("%s.%d" % (base, i)):
                files.append("%s.%d" % (base, i)); i += 1
        else:
            base, files = first, [first]
        self.name = os.path.basename(base)
        self.files = [(f, os.path.getsize(f)) for f in files]
        self.size = sum(s for _, s in self.files)

    def read(self, off, n, out):
        for f, s in self.files:
            if off >= s:
                off -= s; continue
            with open(f, "rb") as fh:
                fh.seek(off)
                while n > 0 and off < s:
                    b = fh.read(min(1 << 20, n, s - off))
                    if not b:
                        return
                    out.write(b); n -= len(b); off += len(b)
            off = 0
            if n <= 0:
                return


def make_handler(parts, token):
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def do_GET(self):
            path = urllib.parse.unquote(self.path.split("?")[0])
            if path != "/%s/%s" % (token, parts.name):
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
            a, b = 0, parts.size - 1
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                s, _, e = rng[6:].split(",")[0].partition("-")
                try:
                    if s == "":
                        a = max(0, parts.size - int(e))
                    else:
                        a = int(s); b = min(int(e), parts.size - 1) if e else parts.size - 1
                except ValueError:
                    a, b = 1, 0
                if a > b or a >= parts.size:
                    self.send_response(416); self.send_header("Content-Range", "bytes */%d" % parts.size)
                    self.send_header("Content-Length", "0"); self.end_headers(); return
                self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, parts.size))
            else:
                self.send_response(200)
            self.send_header("Accept-Ranges", "bytes"); self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(b - a + 1)); self.end_headers()
            if self.command == "GET":
                try:
                    parts.read(a, b - a + 1, self.wfile)
                except OSError:
                    pass
        do_HEAD = do_GET
    return H


def local_ip_towards(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 9)); return s.getsockname()[0]
    finally:
        s.close()


def api(console, method, path, timeout=15):
    req = urllib.request.Request("http://%s:%d%s" % (console, API_PORT, path), method=method, headers={"X-BM-Request": "1"})
    try:
        r = urllib.request.urlopen(req, timeout=timeout); return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def fmt_bytes(n):
    for u in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024 or u == "TB":
            return "%.1f %s" % (n, u) if u != "B" else "%d B" % n
        n /= 1024.0


def line(j):
    eta = j.get("eta", -1)
    e = "--:--" if eta is None or eta < 0 else "%d:%02d" % (eta // 60, eta % 60)
    return "%-10s %5.1f%%  %s / %s  %s/s  ETA %s  [%s] %s" % (j.get("state"), j.get("progress", 0), fmt_bytes(j.get("done", 0)),
                                                          fmt_bytes(j.get("total", 0)), fmt_bytes(j.get("rate", 0)), e,
                                                          j.get("sony_status") or j.get("via", ""), j.get("error") or j.get("step", ""))


def follow(console, quiet=False):
    last = ""
    try:
        while True:
            code, j = api(console, "GET", "/api/install")
            if code == 200:
                s = line(j)
                if s != last and not quiet:
                    sys.stdout.write("\r" + s[:150].ljust(150)); sys.stdout.flush(); last = s
                if j.get("state") in ("done", "failed", "cancelled"):
                    print()
                    return {"done": 0, "failed": 1, "cancelled": 2}[j["state"]], j
            time.sleep(1)
    except KeyboardInterrupt:
        print("\ncancelling ...")
        api(console, "POST", "/api/install/cancel")
        return follow(console, quiet)


def main():
    global API_PORT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("console"); ap.add_argument("file", nargs="?")
    ap.add_argument("--name"); ap.add_argument("--port", type=int, default=0); ap.add_argument("--bind")
    ap.add_argument("--shelf"); ap.add_argument("--status", action="store_true"); ap.add_argument("--cancel", action="store_true")
    ap.add_argument("--api-port", type=int, default=API_PORT, help=argparse.SUPPRESS)
    a = ap.parse_args()
    API_PORT = a.api_port
    if a.status:
        code, j = api(a.console, "GET", "/api/install"); print(line(j) if code == 200 else "no answer (%s)" % code); return 0
    if a.cancel:
        code, j = api(a.console, "POST", "/api/install/cancel"); print("cancelled" if code == 200 else "nothing running"); return 0 if code == 200 else 3
    srv = None
    if a.shelf:
        q = "path=" + urllib.parse.quote(a.shelf, safe="")
    elif a.file:
        parts = Parts(a.file)
        token = secrets.token_hex(8)
        bind = a.bind or local_ip_towards(a.console)
        srv = http.server.ThreadingHTTPServer((bind, a.port), make_handler(parts, token))
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        url = "http://%s:%d/%s/%s" % (bind, srv.server_address[1], token, urllib.parse.quote(parts.name))
        print("serving %s (%s, %d part%s) at %s" % (parts.name, fmt_bytes(parts.size), len(parts.files), "" if len(parts.files) == 1 else "s", url))
        q = "url=" + urllib.parse.quote(url, safe="")
    else:
        ap.error("a file, --shelf, --status or --cancel")
    if a.name:
        q += "&name=" + urllib.parse.quote(a.name, safe="")
    try:
        code, j = api(a.console, "POST", "/api/install?" + q)
    except OSError as e:
        print("the console did not answer on :%d (%s)" % (API_PORT, e)); return 3
    if code != 200:
        print("refused (%s): %s" % (code, j.get("error"))); return 3
    rc, j = follow(a.console)
    print({0: "installed", 1: "FAILED: %s" % j.get("error"), 2: "cancelled"}[rc], j.get("title_id") or "")
    if srv:
        srv.shutdown()
    return rc


if __name__ == "__main__":
    sys.exit(main())
