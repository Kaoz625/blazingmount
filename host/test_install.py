#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_install.py - fake-console test of blazingmountd v2.10's /api/install (bm_install.c): the PS5's installer reads a PKG from the
host through the payload's 127.0.0.1 proxy, with live progress and cancel; PKG Manager is the fallback.
    python3 -I host/test_install.py     (no console: fake imgserve + fake "PC" server + fake PKG Manager on 127.0.0.1, the payload's own
                                         C code as a host build whose FAKE installer reads the proxy URL the way Sony's does)
Checks:
  1. path=<drive pkg> -> imgserve /pkg/<path> with Range, through the proxy: done, every byte served once, title id from the content id
  2. url=http://<ip>:<port>/<x>.pkg (what a PC serves, host/install-pkg.py) installs the same way; GET /api/install/pkgs = /pkgs.json
  3. cancel mid-way: state cancelled, the proxy stops answering (the installer's next read fails), an old job id is a 404
  4. refused: no X-BM-Request (403), a second install while one runs (409), bad path/url (400), unknown (404), cancel with none (409)
  5. Sony's installer unavailable or refusing -> a drive pkg goes to PKG Manager as smb://<host>/mac1-8TB/<path> and is followed there
  6. nothing moves for INST_STALL_S -> failed with the PS5 status in the reason
"""
import hashlib, http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
PKG = os.urandom(5 * 1024 * 1024 + 12345)
REL = "Apps and Emulators/PS5/A Test & App (v1).pkg"
SEEN, PMSEEN = [], []
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


def ranged(h, data):
    rng = h.headers.get("Range", "")
    a, b = 0, len(data) - 1
    if rng.startswith("bytes="):
        s, e = rng[6:].split("-")
        a = int(s); b = min(int(e), len(data) - 1) if e else len(data) - 1
        h.send_response(206); h.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(data)))
    else:
        h.send_response(200)
    h.send_header("Content-Length", str(b - a + 1)); h.end_headers()
    if h.command == "GET":
        try:
            h.wfile.write(data[a:b + 1])
        except OSError:
            pass


class Img(http.server.BaseHTTPRequestHandler):
    """imgserve: /pkgs.json + /pkg/<path> (and the "PC" for url= installs: /files/<name>.pkg)."""
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        p = urllib.parse.unquote(self.path)
        SEEN.append((p, self.headers.get("Range")))
        if p == "/pkgs.json":
            b = json.dumps([{"path": REL, "name": os.path.basename(REL), "size": len(PKG), "parts": 1}]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if p in ("/pkg/" + REL, "/files/From The PC.pkg"):
            return ranged(self, PKG)
        self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
    do_HEAD = do_GET


class PM(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    polls = [0]

    def log_message(self, *a):
        pass

    def _send(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        if self.path == "/api/status":
            PM.polls[0] += 1
            done = PM.polls[0] >= 3
            return self._send({"status": "completed" if done else "installing", "downloaded_bytes": 1000 if done else 400,
                               "total_bytes": 1000, "completed": done, "failed": False, "prompt_message": ""})
        if self.path == "/api/queue":
            return self._send({"active_id": 4, "jobs": []})
        self._send({})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        PMSEEN.append((self.path, self.rfile.read(n).decode() if n else ""))
        PM.polls[0] = 0
        self._send({"success": True, "message": "Installation queued"})


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def build(tmp, aport, iport, pport, ipc):
    exe, helper = os.path.join(tmp, "bmd-%d" % aport), os.path.join(tmp, "helper-%d" % aport)
    for out, src, extra in ((exe, "blazingmountd.c", ['-DBM_VERSION="install-test"', "-DBM_API_PORT=%d" % aport, "-DBM_PM_PORT=%d" % pport,
                                                      "-DINST_STALL_S=4", "-I" + OUT]), (helper, "bm_inst_helper.c", [])):
        r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", "-DBM_INST_PORT=%d" % iport, "-DBM_INST_IPC=%d" % ipc] + extra
                           + ["-o", out, os.path.join(SRC, src), "-lpthread"], capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            print(r.stderr); sys.exit("build failed or warned: " + src)
    return exe, helper


def start(tmp, iport_img, pport, fake, delay=0, helper=True):
    aport, iport, ipc = free_port(), free_port(), free_port()
    exe, hexe = build(tmp, aport, iport, pport, ipc)
    root = os.path.join(tmp, "console-%d" % aport); b = os.path.join(root, "data", "blazing"); os.makedirs(b)
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n" % iport_img)
    env = dict(os.environ, BM_ROOT=root, BM_FAKE_AI=fake, BM_FAKE_AI_DELAY_MS=str(delay))
    if helper:
        env["BM_INST_HELPER"] = hexe
    proc = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    api = "http://127.0.0.1:%d" % aport
    for _ in range(50):
        try:
            urllib.request.urlopen(api + "/api/status", timeout=1); break
        except OSError:
            time.sleep(0.2)
    return proc, api, iport, os.path.join(b, "blazingmountd.log")


def call(api, method, path, hdr=True):
    req = urllib.request.Request(api + path, method=method, headers={"X-BM-Request": "1"} if hdr else {})
    try:
        r = urllib.request.urlopen(req, timeout=10); return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait(api, states=("done", "failed", "cancelled"), t=30):
    end = time.time() + t
    while time.time() < end:
        j = call(api, "GET", "/api/install")[1]
        if j.get("state") in states:
            return j
        time.sleep(0.2)
    return call(api, "GET", "/api/install")[1]


def main():
    tmp = tempfile.mkdtemp(prefix="bm-inst-")
    img_port, pm_port = free_port(), free_port()
    img = http.server.ThreadingHTTPServer(("127.0.0.1", img_port), Img); threading.Thread(target=img.serve_forever, daemon=True).start()
    pm = http.server.ThreadingHTTPServer(("127.0.0.1", pm_port), PM); threading.Thread(target=pm.serve_forever, daemon=True).start()
    procs = []
    try:
        proc, api, iport, log = start(tmp, img_port, pm_port, "on"); procs.append(proc)
        # 4 (before any job)
        check(call(api, "POST", "/api/install/cancel")[0] == 409, "4. cancel with nothing running: 409")
        check(call(api, "POST", "/api/install?path=" + urllib.parse.quote(REL), hdr=False)[0] == 403, "4. no X-BM-Request: 403")
        for bad in ("path=../x.pkg", "path=%2Fdata%2Fx.pkg", "path=a.txt", "url=https://1.2.3.4/x.pkg", "url=http://host.name:80/x.pkg",
                    "url=http://1.2.3.4:80/x.elf", "url=http://1.2.3.4:80/a/../x.pkg", ""):
            check(call(api, "POST", "/api/install?" + bad)[0] == 400, "4. refused with 400: '%s'" % bad)
        check(call(api, "GET", "/api/install/nope")[0] == 404, "4. unknown /api/install/ call: 404")
        check(call(api, "GET", "/api/install")[1].get("state") == "idle", "GET /api/install before any job: idle")

        # 1
        n0 = len(SEEN)
        code, j = call(api, "POST", "/api/install?path=" + urllib.parse.quote(REL))
        check(code == 200 and j.get("ok"), "1. path install accepted (job %s)" % j.get("id"))
        j = wait(api)
        check(j.get("state") == "done" and j.get("via") == "sony", "1. done via the PS5 installer (%s / %s / %s)" % (j.get("state"), j.get("via"), j.get("error")))
        check(j.get("total") == len(PKG) and j.get("done") == len(PKG) and j.get("progress") == 100.0, "1. total/done/progress = %s/%s/%s" % (j.get("total"), j.get("done"), j.get("progress")))
        check(j.get("served") == len(PKG), "1. every byte served once through the proxy (%s of %d)" % (j.get("served"), len(PKG)))
        check(j.get("title_id") == "FAKE00001" and j.get("name") == "A Test & App (v1)", "1. title id from the content id, name from the file (%s, %s)" % (j.get("title_id"), j.get("name")))
        ups = [r for p, r in SEEN[n0:] if p == "/pkg/" + REL]
        check(len(ups) >= 3 and all(r and r.startswith("bytes=") for r in ups), "1. imgserve read with Range only (%d reads)" % len(ups))
        code, body = call(api, "GET", "/api/install/pkgs")
        check(code == 200 and body[0]["path"] == REL, "2. /api/install/pkgs = the host's /pkgs.json")

        # 2
        url = "http://127.0.0.1:%d/files/From%%20The%%20PC.pkg" % img_port
        code, j = call(api, "POST", "/api/install?name=PC%20game&url=" + urllib.parse.quote(url, safe=""))
        j = wait(api)
        check(code == 200 and j.get("state") == "done" and j.get("served") == len(PKG) and j.get("name") == "PC game", "2. url= install from a PC server: %s, %s bytes" % (j.get("state"), j.get("served")))

        # 3: a slow installer, cancelled mid-way
        proc2, api2, iport2, log2 = start(tmp, img_port, pm_port, "on", delay=400); procs.append(proc2)
        call(api2, "POST", "/api/install?path=" + urllib.parse.quote(REL))
        time.sleep(1.2)
        check(call(api2, "POST", "/api/install?path=" + urllib.parse.quote(REL))[0] == 409, "4. a second install while one runs: 409")
        j = call(api2, "GET", "/api/install")[1]
        check(j.get("state") == "installing" and 0 < j.get("done", 0) < len(PKG) and j.get("rate", 0) > 0 and j.get("eta", -1) >= 0,
              "3. live progress mid-way: %.1f%%, %s B/s, eta %s s" % (j.get("progress", 0), j.get("rate"), j.get("eta")))
        code, c = call(api2, "POST", "/api/install/cancel")
        j = wait(api2)
        time.sleep(1.0)
        served = call(api2, "GET", "/api/install")[1].get("served")
        check(code == 200 and j.get("state") == "cancelled", "3. cancel -> cancelled (%s)" % j.get("step"))
        check(served < len(PKG), "3. the proxy stopped: %s of %d bytes served" % (served, len(PKG)))
        s = socket.create_connection(("127.0.0.1", iport2)); s.sendall(b"GET /i/%d-%d.pkg HTTP/1.1\r\nRange: bytes=0-9\r\n\r\n" % (j["id"], int(time.time())))
        check(s.recv(100).startswith(b"HTTP/1.1 404"), "3. the cancelled job's URL is a 404 now"); s.close()

        # 5: no installer in the payload / the installer refuses -> PKG Manager (drive pkgs); a url install fails plainly
        for mode in ("off", "refuse"):
            p3, api3, _, _ = start(tmp, img_port, pm_port, mode); procs.append(p3)
            del PMSEEN[:]
            call(api3, "POST", "/api/install?path=" + urllib.parse.quote(REL))
            j = wait(api3)
            sent = PMSEEN[0] if PMSEEN else ("", "{}")
            check(j.get("state") == "done" and j.get("via") == "pkgmanager" and sent[0] == "/api/install"
                  and json.loads(sent[1]) == {"path": "smb://127.0.0.1/mac1-8TB/" + REL}, "5. installer %s -> PKG Manager smb path, followed to done" % mode)
            call(api3, "POST", "/api/install?url=" + urllib.parse.quote(url, safe=""))
            j = wait(api3)
            check(j.get("state") == "failed" and ("installer" in j.get("error", "")), "5. installer %s + url= -> failed plainly: %s" % (mode, j.get("error")))

        # 5b: a helper stuck inside the installer call -> stopped, and the drive pkg goes to PKG Manager
        p6, api6, _, log6 = start(tmp, img_port, pm_port, "hang"); procs.append(p6)
        call(api6, "POST", "/api/install?path=" + urllib.parse.quote(REL))
        j = wait(api6, t=60)
        import re
        m = re.search(r"stopping helper pid (\d+)", open(log6).read())
        gone = False
        if m:
            time.sleep(0.5)
            try:
                os.kill(int(m.group(1)), 0)
                gone = open("/proc/%s/stat" % m.group(1)).read().split()[2] == "Z" if os.path.exists("/proc") else subprocess.run(["ps", "-o", "stat=", "-p", m.group(1)], capture_output=True, text=True).stdout.strip().startswith("Z")
            except OSError:
                gone = True
        check(j.get("state") == "done" and j.get("via") == "pkgmanager" and m and gone, "5b. hung helper stopped (pid %s, gone %s) -> PKG Manager: %s/%s" % (m and m.group(1), gone, j.get("state"), j.get("via")))

        # 6: the installer never finishes
        p4, api4, _, _ = start(tmp, img_port, pm_port, "stall"); procs.append(p4)
        call(api4, "POST", "/api/install?path=" + urllib.parse.quote(REL))
        j = wait(api4, t=20)
        check(j.get("state") == "failed" and "nothing moved" in j.get("error", "") and "downloading" in j.get("error", ""), "6. stalled -> failed: %s" % j.get("error"))

        # 8: host/install-pkg.py - this computer serves a (split) pkg, the console installs it, live progress; Ctrl-C cancels
        tool = os.path.join(HERE, "install-pkg.py")
        half = len(PKG) // 2
        open(os.path.join(tmp, "Split Game.pkg.0"), "wb").write(PKG[:half]); open(os.path.join(tmp, "Split Game.pkg.1"), "wb").write(PKG[half:])
        aport = int(api.rsplit(":", 1)[1])
        r = subprocess.run([sys.executable, "-I", tool, "127.0.0.1", os.path.join(tmp, "Split Game.pkg.0"), "--bind", "127.0.0.1",
                            "--api-port", str(aport), "--name", "Split Game"], capture_output=True, text=True, timeout=60)
        j = call(api, "GET", "/api/install")[1]
        check(r.returncode == 0 and "installed" in r.stdout and j.get("served") == len(PKG) and j.get("name") == "Split Game",
              "8. install-pkg.py: a split set served as one file, installed (exit %d, %s bytes)" % (r.returncode, j.get("served")))
        check("ETA" in r.stdout and "%" in r.stdout, "8. install-pkg.py shows %/speed/ETA")
        aport2 = int(api2.rsplit(":", 1)[1])
        pr = subprocess.Popen([sys.executable, "-I", tool, "127.0.0.1", os.path.join(tmp, "Split Game.pkg.0"), "--bind", "127.0.0.1",
                               "--api-port", str(aport2)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(2.5)
        import signal as _sig
        pr.send_signal(_sig.SIGINT)
        out, _ = pr.communicate(timeout=30)
        check(pr.returncode == 2 and "cancelled" in out, "8. install-pkg.py Ctrl-C -> the install is cancelled (exit %s)" % pr.returncode)

        # 7: the installer installs ONE package per process (Sony: 0x80B2116F after that). With the helper every install gets a fresh
        # process (1 + 2 above ran on one payload); without one, the 2nd install in the payload falls back to PKG Manager.
        p5, api5, _, log5 = start(tmp, img_port, pm_port, "on", helper=False); procs.append(p5)
        call(api5, "POST", "/api/install?path=" + urllib.parse.quote(REL)); j1 = wait(api5)
        del PMSEEN[:]
        call(api5, "POST", "/api/install?path=" + urllib.parse.quote(REL)); j2 = wait(api5)
        check(j1.get("state") == "done" and j1.get("via") == "sony" and j2.get("state") == "done" and j2.get("via") == "pkgmanager" and PMSEEN,
              "7. no helper: 1st install in the payload (%s/%s), 2nd -> 0x80B2116F -> PKG Manager (%s/%s)" % (j1.get("state"), j1.get("via"), j2.get("state"), j2.get("via")))
        check("0x80b2116f" in open(log5).read().lower(), "7. the payload log shows the one-per-process refusal")
        check("helper process" in open(log).read(), "7. with the helper: installs ran in a helper process")

        text = open(log).read()
        import re
        uris = re.findall(r"InstallByPackage (http://127\.0\.0\.1:\d+/i/\S+\.pkg)", text)
        check(len(uris) >= 2 and len(set(uris)) == len(uris), "the installer never gets the same URL twice (%s)" % uris)
        check("sceAppInstUtilInstallByPackage http://127.0.0.1:" in text and "ui: install A Test & App (v1)" in text, "the payload log records the install")
    finally:
        for p in procs:
            p.terminate(); p.wait(5)
        img.shutdown(); pm.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
