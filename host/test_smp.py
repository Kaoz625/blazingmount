#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_smp.py - fake-console test of blazingmountd v2.9's /api/smp/*: BlazingMount calls ShadowMount+'s local API (:10101).
    python3 -I host/test_smp.py           (no console: a fake ShadowMount+ on 127.0.0.1, the payload's own C code as a HOST=1 build)
Checks:
  1. POST version -> SMP POST /api/v1/version {}; POST uninstall?tid=PPSA34015 -> SMP POST /api/v1/games/uninstall {"title_id"}
  2. SMP's own status and answer are kept (409 game_active)
  3. refused: no X-BM-Request header (403), a bad title id (400), GET or an unknown call (404) - none reach SMP
  4. SMP not answering -> 502 with a plain reason
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
SEEN = []
ACTIVE = [False]
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


class SMP(http.server.BaseHTTPRequestHandler):
    """ShadowMount+ 1.7beta7's API: every call a JSON POST (sm_api_service.c)."""
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode() if n else ""
        SEEN.append((self.path, body, self.headers.get("Content-Type")))
        if self.path == "/api/v1/version":
            return self._send(200, {"ok": True, "version": "1.7beta7"})
        if self.path == "/api/v1/games/uninstall":
            if ACTIVE[0]:
                return self._send(409, {"ok": False, "error": "game_active"})
            return self._send(200, {"ok": True, "uninstall_requested": True})
        self._send(404, {"ok": False})


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    tmp = tempfile.mkdtemp(prefix="bm-smp-")
    aport, sport = free_port(), free_port()
    exe = os.path.join(tmp, "bmd")
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="smp-test"', "-DBM_API_PORT=%d" % aport,
                        "-DBM_SMP_PORT=%d" % sport, "-I" + OUT, "-o", exe, os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
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
        code, j = call("POST", "/api/smp/version")
        check(code == 502 and "not answering" in j.get("error", ""), "4. SMP down: 502 '%s'" % j.get("error"))
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", sport), SMP)
        threading.Thread(target=srv.serve_forever, daemon=True).start()

        code, j = call("POST", "/api/smp/version")
        check(code == 200 and j.get("version") == "1.7beta7" and SEEN[-1][:2] == ("/api/v1/version", "{}"), "1. version -> /api/v1/version {}")
        code, j = call("POST", "/api/smp/uninstall?tid=PPSA34015")
        check(code == 200 and j.get("uninstall_requested") and SEEN[-1][0] == "/api/v1/games/uninstall"
              and json.loads(SEEN[-1][1]) == {"title_id": "PPSA34015"} and SEEN[-1][2] == "application/json",
              "1. uninstall?tid= -> /api/v1/games/uninstall {title_id} as JSON")
        ACTIVE[0] = True
        code, j = call("POST", "/api/smp/uninstall?tid=PPSA34015")
        check(code == 409 and j.get("error") == "game_active", "2. SMP's 409 game_active is kept")
        ACTIVE[0] = False

        n = len(SEEN)
        check(call("POST", "/api/smp/uninstall?tid=PPSA34015", hdr=False)[0] == 403, "3. no X-BM-Request: 403")
        check(call("POST", "/api/smp/uninstall?tid=../etc")[0] == 400 and call("POST", "/api/smp/uninstall")[0] == 400, "3. bad or missing title id: 400")
        check(call("GET", "/api/smp/version")[0] == 404 and call("POST", "/api/smp/images")[0] == 404, "3. GET or a call outside the list: 404")
        check(len(SEEN) == n, "3. none of the refused calls reached SMP")
        log = open(os.path.join(b, "blazingmountd.log")).read()
        check("ui: uninstall PPSA34015 (ShadowMount+)" in log, "the payload log records the uninstall")
        srv.shutdown()
    finally:
        proc.terminate(); proc.wait(5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
