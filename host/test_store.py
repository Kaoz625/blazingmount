#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_store.py - fake-console test of blazingmountd v2.9's homebrew STORE (bm_store.c: GET /api/store, POST /api/store/install).
    python3 -I host/test_store.py      (a fake imgserve with /homebrew.json + /hb/, a fake PKG Manager, the payload's own C code)
Checks:
  1. GET /api/store: the host's feed passed through, plus this console's homebrew folders and payload slots
  2. app: every file of the folder pulled byte-exact into the homebrew dir (subfolders, an empty file), eboot.bin 0755, other 0644,
     no .blzpull records and no incoming/ folder left; installing it again is refused (already there)
  3. payload: the ELF in its own slot /data/pldmgr/payloads/local/<slot>/ (0755) + meta.json; the boot list is not touched
  4. pkg: v2.10 starts BlazingMount's own installer job (/api/install; PKG Manager is that job's fallback, test_install.py)
  5. a file the host does not serve: the job fails and nothing reaches the homebrew dir
  6. refused: no X-BM-Request (403), an unknown kind / a '..' path / a payload without size (400), a second install while one runs (409)
"""
import hashlib, http.server, json, os, shutil, socket, stat, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
SHELF = {}          # relative path -> bytes
SLOW = [0.0]
NOT_SERVED = set()  # listed in ?list but answered 404 (a file the host lost)
PM_SEEN = []
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


class Img(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json", extra=None):
        self.send_response(code)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.unquote(self.path)
        if path == "/games.txt":
            return self._send(200, b"", "text/plain")
        if path == "/homebrew.json":
            return self._send(200, json.dumps([
                {"kind": "app", "path": "Kodi (PPSA99420)", "name": "Kodi", "title_id": "PPSA99420", "version": "01.000.000", "size": 1, "files": 4, "icon": ""},
                {"kind": "payload", "path": "payloads/byepervisor_reborn.elf", "name": "byepervisor_reborn", "size": len(SHELF["payloads/byepervisor_reborn.elf"])},
                {"kind": "pkg", "path": "Store App.pkg", "pkg": "Apps and Emulators/PS5/Store App.pkg", "name": "Store App", "size": 5}]).encode())
        if path.startswith("/hb/") and path.endswith("/?list"):
            folder = path[4:-6]
            files = [{"path": k[len(folder) + 1:], "size": len(v)} for k, v in SHELF.items() if k.startswith(folder + "/")]
            return self._send(200, json.dumps(files).encode()) if files else self._send(404, b"")
        if path.startswith("/hb/") and path[4:] in SHELF and path[4:] not in NOT_SERVED:
            data = SHELF[path[4:]]
            a, b = self.headers["Range"].split("=")[1].split("-")
            a, b = int(a), min(int(b), len(data) - 1)
            if SLOW[0]:
                time.sleep(SLOW[0])
            return self._send(206, data[a:b + 1], "application/octet-stream", {"Content-Range": "bytes %d-%d/%d" % (a, b, len(data))})
        self._send(404, b"")


class PM(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode()
        PM_SEEN.append((self.path, body))
        b = b'{"success":true,"message":"Installation queued"}'
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    SHELF.update({
        "Kodi (PPSA99420)/eboot.bin": os.urandom(300_000),
        "Kodi (PPSA99420)/sce_sys/param.json": b'{"titleId":"PPSA99420"}',
        "Kodi (PPSA99420)/share/kodi/addons/x.xml": os.urandom(9_000_000),
        "Kodi (PPSA99420)/share/empty.txt": b"",
        "payloads/byepervisor_reborn.elf": b"\x7fELF" + os.urandom(20_000),
    })
    tmp = tempfile.mkdtemp(prefix="bm-store-")
    img = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Img); hport = img.server_address[1]
    pm = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PM); pport = pm.server_address[1]
    for s in (img, pm):
        threading.Thread(target=s.serve_forever, daemon=True).start()
    aport = free_port()
    exe = os.path.join(tmp, "bmd")
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="store-test"', "-DBM_API_PORT=%d" % aport,
                        "-DBM_PM_PORT=%d" % pport, "-I" + OUT, "-o", exe, os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr); sys.exit("build failed or warned")
    root = os.path.join(tmp, "console")
    b = os.path.join(root, "data", "blazing"); os.makedirs(b)
    hb = os.path.join(root, "mnt", "ext1", "homebrew"); os.makedirs(os.path.join(hb, "Already Here (PPSA90000)"))
    os.makedirs(os.path.join(root, "data", "pldmgr", "payloads", "local", "web-file-mgr"))
    boot = os.path.join(root, "data", "pldmgr", "autoload.txt"); open(boot, "w").write("repo/websrv@e8cce5d1\n")
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n"
                                                   "homebrew_dir /mnt/ext1/homebrew\n" % hport)
    proc = subprocess.Popen([exe], env=dict(os.environ, BM_ROOT=root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    api = "http://127.0.0.1:%d" % aport

    def get():
        return json.loads(urllib.request.urlopen(api + "/api/store", timeout=20).read())

    def install(hdr=True, **q):
        req = urllib.request.Request(api + "/api/store/install?" + urllib.parse.urlencode(q), method="POST", headers={"X-BM-Request": "1"} if hdr else {})
        try:
            return 200, json.loads(urllib.request.urlopen(req, timeout=20).read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def wait_job(secs=60):
        t = time.time()
        while time.time() - t < secs:
            j = get()["job"]
            if j["state"] in ("done", "failed"):
                return j
            time.sleep(0.2)
        return get()["job"]

    try:
        for _ in range(50):
            try:
                get(); break
            except OSError:
                time.sleep(0.2)
        s = get()
        check(isinstance(s["feed"], list) and len(s["feed"]) == 3, "1. the host's feed passed through (3 items)")
        check("Already Here (PPSA90000)" in s["have"]["folders"] and "web-file-mgr" in s["have"]["slots"], "1. this console's homebrew folders + payload slots")

        code, j = install(kind="app", path="Kodi (PPSA99420)", name="Kodi")
        check(code == 200, "2. app install accepted")
        j = wait_job()
        dest = os.path.join(hb, "Kodi (PPSA99420)")
        same = all(open(os.path.join(dest, k[len("Kodi (PPSA99420)/"):]), "rb").read() == v for k, v in SHELF.items() if k.startswith("Kodi"))
        check(j["state"] == "done" and same and j["files_done"] == 4, "2. app: 4 files byte-exact in the homebrew dir (%s %s)" % (j["state"], j["err"] or j["step"]))
        check(stat.S_IMODE(os.stat(os.path.join(dest, "eboot.bin")).st_mode) == 0o755 and stat.S_IMODE(os.stat(os.path.join(dest, "sce_sys", "param.json")).st_mode) == 0o644,
              "2. eboot.bin 0755, param.json 0644")
        left = [f for d, _, fs in os.walk(dest) for f in fs if f.endswith(".blzpull")]
        inc = os.path.join(root, "mnt", "ext1", "blz-stage", "incoming")
        check(not left and not [x for x in (os.listdir(inc) if os.path.isdir(inc) else []) if x.startswith("store-")], "2. no .blzpull, nothing left in incoming/")
        install(kind="app", path="Kodi (PPSA99420)", name="Kodi")
        j = wait_job()
        check(j["state"] == "failed" and "already" in j["err"], "2. again: refused, already on the console (%s)" % j["err"])

        size = len(SHELF["payloads/byepervisor_reborn.elf"])
        install(kind="payload", path="payloads/byepervisor_reborn.elf", name="byepervisor_reborn", size=size)
        j = wait_job()
        slot = os.path.join(root, "data", "pldmgr", "payloads", "local", "byepervisor_reborn")
        elf = os.path.join(slot, "byepervisor_reborn.elf")
        meta = json.load(open(os.path.join(slot, "meta.json"))) if os.path.exists(os.path.join(slot, "meta.json")) else {}
        check(j["state"] == "done" and os.path.isfile(elf) and open(elf, "rb").read() == SHELF["payloads/byepervisor_reborn.elf"]
              and stat.S_IMODE(os.stat(elf).st_mode) == 0o755, "3. payload in its slot, byte-exact, 0755 (%s)" % (j["err"] or j["step"]))
        check(meta.get("filename") == "byepervisor_reborn.elf" and meta.get("kind") == "local", "3. meta.json for the payload manager")
        check(open(boot).read() == "repo/websrv@e8cce5d1\n", "3. the boot list is not touched")

        install(kind="pkg", path="Store App.pkg", name="Store App")
        j = wait_job()
        ij = json.loads(urllib.request.urlopen(api + "/api/install", timeout=10).read())
        check(j["state"] == "done" and ij.get("src") == "Apps and Emulators/PS5/Store App.pkg" and ij.get("name") == "Store App",
              "4. pkg -> BlazingMount's installer job (/api/install src %s, %s)" % (ij.get("src"), j["err"] or j["step"]))

        SHELF["Broken (PPSA90009)/eboot.bin"] = b"x" * 10
        SHELF["Broken (PPSA90009)/missing.bin"] = b"y" * 10
        NOT_SERVED.add("Broken (PPSA90009)/missing.bin")
        install(kind="app", path="Broken (PPSA90009)", name="Broken")
        j = wait_job()
        check(j["state"] == "failed" and not os.path.exists(os.path.join(hb, "Broken (PPSA90009)")), "5. a file the host does not serve: failed, homebrew untouched (%s)" % j["err"][:80])

        check(install(hdr=False, kind="app", path="Kodi (PPSA99420)")[0] == 403, "6. no X-BM-Request: 403")
        check(install(kind="game", path="x")[0] == 400 and install(kind="app", path="../../etc")[0] == 400
              and install(kind="payload", path="payloads/byepervisor_reborn.elf")[0] == 400, "6. unknown kind / '..' / payload without size: 400")
        shutil.rmtree(slot)
        SLOW[0] = 0.5
        install(kind="payload", path="payloads/byepervisor_reborn.elf", name="byepervisor_reborn", size=size)
        check(install(kind="app", path="Kodi (PPSA99420)")[0] == 409, "6. a second install while one runs: 409")
        wait_job(); SLOW[0] = 0
        log = open(os.path.join(b, "blazingmountd.log")).read()
        check("store: app Kodi (PPSA99420) ->" in log and "not loaded, not in the boot list" in log, "the payload log records the installs")
    finally:
        proc.terminate(); proc.wait(5)
        img.shutdown(); pm.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
