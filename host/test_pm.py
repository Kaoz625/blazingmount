#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_pm.py - fake-console test of blazingmountd v2.9's /api/pm/*: install a PKG from the drives through PKG Manager (1x storage).
    python3 -I host/test_pm.py            (no console: a fake PKG Manager on 127.0.0.1, the payload's own C code as a HOST=1 build)
Checks:
  1. GET drives / packages?drive= / queue / status come back exactly as PKG Manager answered (the drive id is passed on, encoded)
  2. POST install?path=smb://... is sent to PKG Manager's /api/install as {"path": ...} (spaces and quotes survive), cancel?id=N as
     /api/queue/cancel {"id": N}, refresh as /api/packages/refresh; PKG Manager's own status code is kept (409 = busy)
  3. refused: no X-BM-Request header (403), a path that is not smb://...pkg (400), an unknown /api/pm/ call (404)
  4. PKG Manager not running -> 502 with a plain reason
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
SEEN = []
BUSY = [False]
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


class PM(http.server.BaseHTTPRequestHandler):
    """PKG Manager 1.5.1's answers, as recorded from PHAT5 on 26 Oct 2026 (shortened)."""
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        SEEN.append(("GET", self.path, None))
        if self.path == "/api/drives":
            return self._send(200, [{"id": "mac1_8tb", "label": "mac1 8TB - PS5 games (not packed yet)", "type": "smb", "pkg_count": 3}])
        if self.path.startswith("/api/packages"):
            return self._send(200, [{"path": "smb://192.168.1.130/mac1-8TB/x/A Game.pkg", "title_id": "PPSA00001", "size": 123}])
        if self.path == "/api/queue":
            return self._send(200, {"active_id": 0, "jobs": []})
        if self.path == "/api/status":
            return self._send(200, {"status": "idle", "downloaded_bytes": 0, "total_bytes": 0, "progress": 0.0})
        self._send(404, {"error": "nf"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode() if n else ""
        SEEN.append(("POST", self.path, body))
        if self.path == "/api/install":
            return self._send(409 if BUSY[0] else 200, {"success": not BUSY[0], "message": "Installation queued"})
        self._send(200, {"success": True})


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    tmp = tempfile.mkdtemp(prefix="bm-pm-")
    aport, pport = free_port(), free_port()
    exe = os.path.join(tmp, "bmd")
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="pm-test"', "-DBM_API_PORT=%d" % aport,
                        "-DBM_PM_PORT=%d" % pport, "-I" + OUT, "-o", exe, os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr); sys.exit("build failed or warned")
    root = os.path.join(tmp, "console"); b = os.path.join(root, "data", "blazing"); os.makedirs(b)
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport 9\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n")
    proc = subprocess.Popen([exe], env=dict(os.environ, BM_ROOT=root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    api = "http://127.0.0.1:%d" % aport

    def call(method, path, hdr=True):
        req = urllib.request.Request(api + path, method=method, headers={"X-BM-Request": "1"} if hdr else {})
        try:
            return 200, json.loads(urllib.request.urlopen(req, timeout=10).read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(api + "/api/status", timeout=1); break
            except OSError:
                time.sleep(0.2)
        # 4 first: nothing listens on pport yet
        code, j = call("GET", "/api/pm/drives")
        check(code == 502 and "not running" in j.get("error", ""), "4. PKG Manager down: 502 '%s'" % j.get("error"))
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", pport), PM)
        threading.Thread(target=srv.serve_forever, daemon=True).start()

        code, j = call("GET", "/api/pm/drives")
        check(code == 200 and j[0]["id"] == "mac1_8tb", "1. drives passed through")
        code, j = call("GET", "/api/pm/packages?drive=" + urllib.parse.quote("mac1 8tb"))
        check(code == 200 and j[0]["title_id"] == "PPSA00001" and SEEN[-1][1] == "/api/packages?drive=mac1%208tb", "1. packages?drive= passed on, encoded (%s)" % SEEN[-1][1])
        check(call("GET", "/api/pm/queue")[1].get("jobs") == [] and call("GET", "/api/pm/status")[1].get("status") == "idle", "1. queue + status passed through")

        p = 'smb://192.168.1.130/mac1-8TB/PS5 Game Add-ons/A "Quoted" Game (PPSA00001)/x.pkg'
        code, j = call("POST", "/api/pm/install?path=" + urllib.parse.quote(p))
        check(code == 200 and SEEN[-1][:2] == ("POST", "/api/install") and json.loads(SEEN[-1][2]) == {"path": p}, "2. install -> /api/install {path} (spaces + quotes survive)")
        BUSY[0] = True
        check(call("POST", "/api/pm/install?path=" + urllib.parse.quote(p))[0] == 409, "2. PKG Manager's 409 (busy) is kept")
        BUSY[0] = False
        code, j = call("POST", "/api/pm/cancel?id=7")
        check(code == 200 and SEEN[-1][1] == "/api/queue/cancel" and json.loads(SEEN[-1][2]) == {"id": 7}, "2. cancel?id=7 -> /api/queue/cancel {id:7}")
        call("POST", "/api/pm/refresh")
        check(SEEN[-1][1] == "/api/packages/refresh", "2. refresh -> /api/packages/refresh")

        n = len(SEEN)
        check(call("POST", "/api/pm/install?path=" + urllib.parse.quote(p), hdr=False)[0] == 403, "3. no X-BM-Request: 403")
        check(call("POST", "/api/pm/install?path=/data/x.pkg")[0] == 400, "3. a path that is not smb://: 400")
        check(call("POST", "/api/pm/install?path=smb://h/s/x.elf")[0] == 400, "3. a file that is not .pkg: 400")
        check(call("GET", "/api/pm/settings")[0] == 404 and call("POST", "/api/pm/queue/clear")[0] == 404, "3. calls outside the list: 404")
        check(len(SEEN) == n, "3. none of the refused calls reached PKG Manager")
        log = open(os.path.join(b, "blazingmountd.log")).read()
        check("ui: install from the drives (PKG Manager)" in log and "ui: cancel install job 7" in log, "the payload log records installs and cancels")
        srv.shutdown()
    finally:
        proc.terminate(); proc.wait(5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
