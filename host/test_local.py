#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_local.py - fake-console test of v2.14 "Copy to M.2 / Copy to internal / Back to streaming" (bm_local.c).

    python3 -I host/test_local.py          (no console: a fake imgserve with Range + a fake ShadowMount+ API in-process,
                                            the payload's own C code as a host build with BM_ROOT; ShadowMount's log lines are
                                            written by the test the way SMP writes them)
Checks:
  1. refused up front, with the reason: bad 'to', not enough space (keep), a game needing a newer FW without a backport overlay,
     Back to streaming with no local copy, a folder game, a second job while one runs, no X-BM-Request header
  2. Copy to M.2 of a STREAMING game: the starter stays while the image arrives (pull, byte-exact); while the game runs it waits
     ("switches when ... is closed"); closed -> starter parked in blz-stage/stub, cfg -> .cfg.off-*, image in /mnt/ext1/homebrew,
     ShadowMount asked to scan, /api/library local=image where=m2; homebrew never holds the starter and the image at once
  3. the daemon never builds a starter over the local copy (sync passes, with its old 'done' state record)
  4. Move to internal: copied on the console, switched, the old M.2 copy deleted once ShadowMount logs 'source removed'
  5. Back to streaming refused while the game runs; then: image out, ShadowMount lets go -> the SAME starter + cfg back, image deleted
  6. ShadowMount silent (no game open) for LOCAL_SMP_WAIT_S -> the image is put back and still plays (state local)
  7. Cancel mid-copy -> staged bytes deleted, record gone, starter untouched
  8. a hand-placed image (no record; DD2's case): Back to streaming refused when the drives' image differs in size; with the same
     size it is deleted after ShadowMount lets go, and the next pass builds a NEW starter
  9. a daemon restart mid-copy resumes it to the end
 10. the phone page (/) carries the buttons
"""
import hashlib, http.server, json, os, random, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
MIB = 1 << 20
STATE = {"slow": 0.0, "scans": 0}
random.seed(14)


def game(tid, title, fw="4.50", mib=20):
    files = {"eboot.bin": b"ELF.." + b"libSceAmpr.prx" + b"\0" * 2000, "sce_sys/param.json": json.dumps({"titleId": tid}).encode(),
             "data/big.pak": b"\1" * (2 * MIB), "data/small.txt": b"hello"}
    raw = random.randbytes(mib * MIB + int(tid[4:]))
    return {"tid": tid, "title": title, "version": "01.000.000", "fw": fw, "sdk": "4.00", "size": len(raw), "key": "%s@%d" % (tid, len(raw)),
            "name": "%s (%s).ffpfsc" % (title, tid), "files": files, "raw": raw}


GAMES = [game("PPSA90001", "Alpha Game"), game("PPSA90002", "Beta Game"), game("PPSA90003", "Gamma Needs FW", fw="99.00"),
         game("PPSA90004", "Delta Hand Placed", mib=3)]
FOLDER = {"tid": "PPSA90005", "title": "Folder Game", "version": "01.000.000", "fw": "4.50", "sdk": "4.00", "size": 5, "key": "PPSA90005@5",
          "name": "Folder Game (PPSA90005)"}


class Fake(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def send(self, body, code=200, extra=()):
        self.send_response(code)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = urllib.parse.unquote(self.path)
        meta = [{k: v for k, v in g.items() if k not in ("files", "raw")} for g in GAMES] + [FOLDER]
        if p == "/games.json":
            return self.send(json.dumps(meta).encode())
        if p == "/games.txt":
            return self.send(("# key\ttid\tsize\tversion\tfw\tsdk\ttitle\n" + "".join(
                "%s\t%s\t%d\t%s\t%s\t%s\t%s\n" % (g["key"], g["tid"], g["size"], g["version"], g["fw"], g["sdk"], g["title"]) for g in GAMES)).encode())
        for g in GAMES:
            if p == "/raw/" + g["name"]:
                data = g["raw"]
                a, b = self.headers.get("Range", "bytes=0-").split("=")[1].split("-")
                a, b = int(a), min(int(b or len(data) - 1), len(data) - 1)
                body = data[a:b + 1]
                self.send_response(206)
                self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(data)))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    for i in range(0, len(body), 256 * 1024):
                        if STATE["slow"]:
                            time.sleep(STATE["slow"])
                        self.wfile.write(body[i:i + 256 * 1024])
                except OSError:                       # the console cancelled / was killed mid-piece
                    pass
                return
            if p == "/f/" + g["key"]:
                return self.send("".join("%d\t%s\n" % (len(b), n) for n, b in g["files"].items()).encode())
            pre = "/t/%s/" % g["key"]
            if p.startswith(pre) and p[len(pre):] in g["files"]:
                return self.send(g["files"][p[len(pre):]])
        self.send(b"", 404)


class Smp(http.server.BaseHTTPRequestHandler):            # ShadowMount+'s local API: only /api/v1/scan is called
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        if self.path == "/api/v1/scan":
            STATE["scans"] += 1
        body = b'{"status":0,"queued":true}'
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + what)
        fails += 0 if cond else 1

    work = tempfile.mkdtemp(prefix="bm-local-")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    smp = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Smp)
    threading.Thread(target=smp.serve_forever, daemon=True).start()
    hport, sport, aport = srv.server_address[1], smp.server_address[1], free_port()
    exe = os.path.join(work, "bmd")
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="local-test"', "-DBM_API_PORT=%d" % aport,
                        "-DBM_SMP_PORT=%d" % sport, "-DAPI_WATCH_S=2", "-DLOCAL_TICK_S=1", "-DLOCAL_SMP_WAIT_S=4", "-I", OUT, "-o", exe,
                        os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
    check(r.returncode == 0 and not r.stderr.strip(), "host build, no warnings" + (": " + r.stderr[:600] if r.stderr.strip() else ""))
    if r.returncode:
        return 1
    root = os.path.join(work, "console")
    C = lambda p: root + p
    b = C("/data/blazing")
    for d in ("/data/blazing", "/data/homebrew", "/mnt/ext1/homebrew", "/data/shadowmount", "/mnt/sandbox", "/user/app"):
        os.makedirs(C(d), exist_ok=True)
    conf = "host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\nhomebrew_dir /mnt/ext1/homebrew\n" % hport
    open(os.path.join(b, "mount.conf"), "w").write(conf)
    open(os.path.join(b, "choices.tsv"), "w").write("auto\t1\nskip\tPPSA90004\n")       # Delta: no starter (it is placed by hand below)
    smplog = C("/data/shadowmount/debug.log")
    open(smplog, "w").write("[10:00:00] ShadowMount+ v1.7beta6-fix2 test\n")
    env = dict(os.environ, BM_ROOT=root)
    proc = [subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    api = "http://127.0.0.1:%d" % aport
    both = []                                              # a watcher: homebrew must never hold a starter AND an image of one title

    def watch():
        while not stop[0]:
            for hb in ("/data/homebrew", "/mnt/ext1/homebrew"):
                pass
            names = []
            for hb in ("/data/homebrew", "/mnt/ext1/homebrew"):
                try:
                    names += os.listdir(C(hb))
                except OSError:
                    pass
            for g in GAMES[:2]:
                mine = [n for n in names if g["tid"] in n]
                if any(n.endswith(".ffpfsc") for n in mine) and any(not n.endswith(".ffpfsc") for n in mine):
                    both.append(mine)
            time.sleep(0.05)
    stop = [False]
    threading.Thread(target=watch, daemon=True).start()

    def j(path):
        with urllib.request.urlopen(api + path, timeout=10) as rr:
            return json.loads(rr.read())

    def act(what, tid, extra="", hdr=True):
        req = urllib.request.Request(api + "/api/action?do=%s&tid=%s%s" % (what, tid, extra), method="POST", headers={"X-BM-Request": "1"} if hdr else {})
        try:
            with urllib.request.urlopen(req, timeout=15) as rr:
                return 200, json.loads(rr.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def row(tid):
        for g in j("/api/library")["games"]:
            if g["tid"] == tid:
                return g
        return {}

    def wait(pred, secs=30):
        t = time.time()
        while time.time() - t < secs:
            try:
                v = pred()
                if v:
                    return v
            except (OSError, KeyError, ValueError):
                pass
            time.sleep(0.2)
        return None

    def smp_line(text):
        with open(smplog, "a") as f:
            f.write("[%s]   %s\n" % (time.strftime("%H:%M:%S"), text))

    def starter_dir(g, hb="/mnt/ext1/homebrew"):
        return C("%s/%s (%s)" % (hb, g["title"], g["tid"]))

    A, B_, G3, D = GAMES
    try:
        wait(lambda: j("/api/status"), 10)
        check(wait(lambda: os.path.exists(starter_dir(A) + "/eboot.bin") and os.path.exists(starter_dir(B_) + "/eboot.bin"), 30) is not None,
              "starters of Alpha and Beta built on the M.2 homebrew")
        st = j("/api/status")
        check(st.get("local", {}).get("v") == 1 and st["local"]["m2"] is True, "/api/status says local copies work and the M.2 is there")

        # 1. refusals
        code, v = act("local", A["tid"], "&to=moon")
        check(code == 400 and "m2 or internal" in v.get("error", ""), "1. bad 'to' refused: %s" % v.get("error"))
        code, v = act("local", A["tid"], "&to=m2", hdr=False)
        check(code == 403, "1. no X-BM-Request header: 403")
        open(os.path.join(b, "mount.conf"), "w").write(conf + "local_keep_m2_mb 999999999\n")
        code, v = act("local", A["tid"], "&to=m2")
        check(code == 400 and "not enough space on the M.2" in v.get("error", ""), "1. space refused up front: %s" % v.get("error"))
        open(os.path.join(b, "mount.conf"), "w").write(conf)
        code, v = act("local", G3["tid"], "&to=m2")
        check(code == 400 and "needs FW 99.00" in v.get("error", "") and "backport" in v.get("error", ""), "1. newer FW, no overlay: %s" % v.get("error"))
        code, v = act("stream", A["tid"])
        check(code == 400 and "no local image" in v.get("error", ""), "1. back to streaming with no local copy: %s" % v.get("error"))
        code, v = act("local", FOLDER["tid"], "&to=m2")
        check(code == 400 and "folder game" in v.get("error", ""), "1. a folder game: %s" % v.get("error"))

        # 2. copy Alpha to the M.2 while it 'runs'
        os.makedirs(C("/mnt/sandbox/%s_000" % A["tid"]))
        STATE["slow"] = 0.01
        code, v = act("local", A["tid"], "&to=m2")
        check(code == 200 and v.get("ok"), "2. Copy to M.2 accepted (%s %s)" % (code, v))
        code, v = act("local", B_["tid"], "&to=internal")
        check(code == 400 and "one copy at a time" in v.get("error", ""), "1. a second job refused: %s" % v.get("error"))
        lj = wait(lambda: (row(A["tid"]).get("lj") or {}).get("state") == "copying" and row(A["tid"])["lj"])
        check(lj is not None and lj["to"] == "m2", "2. /api/library shows the copy (lj copying, to m2)")
        check(os.path.exists(starter_dir(A) + "/eboot.bin"), "2. the starter is still there while it copies")
        STATE["slow"] = 0
        lj = wait(lambda: (row(A["tid"]).get("lj") or {}).get("state") == "ready" and "switches when" in row(A["tid"])["lj"]["msg"] and row(A["tid"])["lj"])
        check(lj is not None and "switches when" in lj["msg"], "2. copied, waits while the game runs: %s" % (lj or {}).get("msg"))
        check(os.path.exists(starter_dir(A)), "2. nothing switched while it runs")
        scans0 = STATE["scans"]
        os.rmdir(C("/mnt/sandbox/%s_000" % A["tid"]))
        img = C("/mnt/ext1/homebrew/" + A["name"])
        check(wait(lambda: os.path.exists(img), 15) is not None, "2. closed -> the image is in /mnt/ext1/homebrew")
        check(open(img, "rb").read() == A["raw"], "2. the image is byte-exact")
        check(not os.path.exists(starter_dir(A)), "2. the starter left homebrew")
        stub = os.listdir(C("/mnt/ext1/blz-stage/stub")) if os.path.isdir(C("/mnt/ext1/blz-stage/stub")) else []
        check(any(n.startswith("%s (%s)-off-" % (A["title"], A["tid"])) for n in stub), "2. the starter is parked in blz-stage/stub (%s)" % stub)
        offs = [n for n in os.listdir(C("/data/blazing/link")) if n.startswith(A["tid"] + ".cfg.off-")]
        check(len(offs) == 1 and not os.path.exists(C("/data/blazing/link/%s.cfg" % A["tid"])), "2. link cfg -> .cfg.off-*")
        check(wait(lambda: STATE["scans"] > scans0, 5) is not None, "2. ShadowMount+ asked to scan (POST /api/v1/scan)")
        check(not [n for n in os.listdir(C("/mnt/ext1/blz-stage/incoming"))], "2. nothing left in incoming/")
        g = wait(lambda: row(A["tid"]).get("local") == "image" and row(A["tid"]))
        check(g is not None and g["where"] == "m2" and g["on_console"] and g["lj"]["state"] == "local", "2. library: local=image where=m2 on_console")
        smp_line("[LINK] mount.lnk created: /user/app/%s/mount.lnk -> /mnt/shadowmnt/%s_x" % (A["tid"], A["tid"]))
        check(wait(lambda: "plays from the M.2" in row(A["tid"])["lj"]["msg"], 10) is not None, "2. ShadowMount's mount.lnk line -> 'plays from the M.2'")

        # 3. no starter over it
        act("sync", "")
        time.sleep(3)
        act("rebuild", A["tid"])
        time.sleep(3)
        check(not os.path.exists(starter_dir(A)) and os.path.exists(img), "3. passes (sync, even rebuild) build no starter over the local copy")

        # 4. move to internal
        code, v = act("local", A["tid"], "&to=internal")
        check(code == 200, "4. Move to internal accepted (%s %s)" % (code, v))
        img2 = C("/data/homebrew/" + A["name"])
        check(wait(lambda: os.path.exists(img2), 20) is not None and not os.path.exists(img), "4. the image moved to /data/homebrew")
        check(open(img2, "rb").read() == A["raw"], "4. byte-exact after the console copy")
        rem = [n for n in os.listdir(C("/mnt/ext1/blz-stage/incoming")) if ".remove-" in n]
        check(len(rem) == 1, "4. the old M.2 copy is out of homebrew, kept until ShadowMount lets go (%s)" % rem)
        smp_line("[CACHE] source removed: %s (/mnt/shadowmnt/%s_x)" % (A["tid"], A["tid"]))
        check(wait(lambda: not os.listdir(C("/mnt/ext1/blz-stage/incoming")), 10) is not None, "4. old copy deleted after 'source removed'")
        check(row(A["tid"])["where"] == "internal", "4. library: where=internal")

        # 5. back to streaming
        os.makedirs(C("/mnt/sandbox/%s_000" % A["tid"]))
        code, v = act("stream", A["tid"])
        check(code == 400 and "is running" in v.get("error", ""), "5. back to streaming refused while it runs: %s" % v.get("error"))
        os.rmdir(C("/mnt/sandbox/%s_000" % A["tid"]))
        open(C("/user/app/%s.keep" % A["tid"]), "w").close()
        code, v = act("stream", A["tid"])
        check(code == 200, "5. Back to streaming accepted (%s %s)" % (code, v))
        check(wait(lambda: not os.path.exists(img2), 10) is not None, "5. the image left homebrew first")
        time.sleep(2)
        check(not os.path.exists(starter_dir(A)), "5. the starter waits for ShadowMount to let go")
        smp_line("[CACHE] source removed: %s (/mnt/shadowmnt/%s_x)" % (A["tid"], A["tid"]))
        check(wait(lambda: os.path.exists(starter_dir(A) + "/eboot.bin"), 10) is not None, "5. the SAME starter is back in /mnt/ext1/homebrew")
        check(os.path.exists(C("/data/blazing/link/%s.cfg" % A["tid"])), "5. its cfg is back")
        check(wait(lambda: not [n for n in os.listdir(C("/data/blz-stage/incoming")) if A["tid"] in n], 10) is not None, "5. the local image is deleted")
        g = wait(lambda: row(A["tid"]).get("local") == "starter" and row(A["tid"]))
        check(g is not None and g["lj"] is None, "5. library: streams again, no record")

        # 6. ShadowMount silent -> put back
        code, v = act("local", B_["tid"], "&to=internal")
        imgB = C("/data/homebrew/" + B_["name"])
        check(code == 200 and wait(lambda: os.path.exists(imgB), 20) is not None, "6. Beta copied to internal")
        code, v = act("stream", B_["tid"])
        check(code == 200, "6. Back to streaming accepted")
        check(wait(lambda: (row(B_["tid"]).get("lj") or {}).get("state") == "local" and os.path.exists(imgB), 15) is not None,
              "6. ShadowMount silent %ds -> the copy is put back (state local)" % 4)
        check("did not happen" in row(B_["tid"])["lj"]["msg"], "6. it says so: %s" % row(B_["tid"])["lj"]["msg"])
        check(not os.path.exists(starter_dir(B_)), "6. no starter next to it")

        # 7. cancel: Alpha again, slow
        STATE["slow"] = 0.05
        code, v = act("local", A["tid"], "&to=m2")
        wait(lambda: (row(A["tid"]).get("lj") or {}).get("done", 0) > 2 * MIB, 20)
        code, v = act("lcancel", A["tid"])
        check(code == 200, "7. cancel accepted")
        check(wait(lambda: row(A["tid"]).get("lj") is None, 20) is not None, "7. record gone")
        STATE["slow"] = 0
        check(not [n for n in os.listdir(C("/mnt/ext1/blz-stage/incoming")) if A["tid"] in n], "7. staged bytes deleted")
        check(os.path.exists(starter_dir(A) + "/eboot.bin"), "7. starter untouched")

        # 8. a hand-placed image (no record)
        hand = C("/mnt/ext1/homebrew/" + D["name"])
        open(hand, "wb").write(D["raw"][:-1])
        code, v = act("stream", D["tid"])
        check(code == 400 and "same image" in v.get("error", ""), "8. a different-size image is kept: %s" % v.get("error"))
        open(hand, "wb").write(D["raw"])
        open(os.path.join(b, "choices.tsv"), "w").write("auto\t1\n")
        code, v = act("stream", D["tid"])
        check(code == 200, "8. Back to streaming of a hand-placed image accepted")
        wait(lambda: not os.path.exists(hand), 10)
        smp_line("[LINK] removed stale mount link: /user/app/%s/mount.lnk" % D["tid"])
        check(wait(lambda: os.path.exists(starter_dir(D) + "/eboot.bin"), 20) is not None, "8. a NEW starter is built for it")
        check(not [n for n in os.listdir(C("/mnt/ext1/blz-stage/incoming")) if D["tid"] in n], "8. the hand-placed image is deleted")

        # 9. restart mid-copy
        STATE["slow"] = 0.03
        code, v = act("local", A["tid"], "&to=internal")
        wait(lambda: (row(A["tid"]).get("lj") or {}).get("done", 0) > 2 * MIB, 20)
        proc[0].kill(); proc[0].wait()
        STATE["slow"] = 0
        proc[0] = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        imgA = C("/data/homebrew/" + A["name"])
        check(wait(lambda: os.path.exists(imgA), 30) is not None and open(imgA, "rb").read() == A["raw"], "9. a restart mid-copy resumes to a byte-exact switch")

        # 10. the page
        with urllib.request.urlopen(api + "/", timeout=10) as rr:
            page = rr.read().decode()
        check(all(k in page for k in ("Copy to M.2", "Copy to internal", "Back to streaming", "lcancel", "progress")), "10. the phone page has the buttons")
        check(not both, "never a starter and an image of one title in homebrew at once (%s)" % both[:2])
        log = open(os.path.join(b, "blazingmountd.log")).read()
        check("REFUSED a cross-drive" not in log, "no cross-drive rename was ever tried")
    finally:
        stop[0] = True
        proc[0].kill()
        srv.shutdown(); smp.shutdown()
    print("%d FAIL" % fails if fails else "ALL PASS")
    if not fails:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("kept:", work)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
