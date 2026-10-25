#!/usr/bin/env python3
"""End to end: host/download-to-console.py against the REAL blazingmountd (a HOST=1 build, like test_api.py) on 127.0.0.1.

    python3 -I host/test_download_e2e.py

test_download_to_console.py fakes the payload API; test_api.py fakes mac1's worker. Here both are real and only the console's
file servers are fakes over the payload's own BM_ROOT (the ftpsrv + websrv of test_download_to_console.py, which also writes
ShadowMount's 'source removed' line when something leaves the M.2's homebrew). The fake imgserve is test_api.py's.

Covered (docs/DOWNLOAD-TO-CONSOLE.md 7): the payload builds the starter (auto on) -> the app's Download (POST /api/action) ->
downloads.tsv -> GET /api/downloads -> `serve` -> `get`: the image sent, every piece checked by the payload's own
/api/sha256 -> switch (hide, starter parked, image in the M.2's homebrew, cfg off) -> /api/library: on_console, kind image,
choice skip, mac1's job 'done' from dl-status.tsv. Then the app's Remove local copy -> `remove-local`: starter + cfg back,
unhide, the image deleted from incoming/ only -> kind starter, not on_console, job 'removed'. All along: the homebrew never
holds the starter and the image of the title at once (ShadowMount keeps the FIRST copy it sees), and the payload never
builds a second starter.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_api as A                                    # noqa: E402  the payload build + the fake imgserve
import test_download_to_console as T                    # noqa: E402  the fake ftpsrv + websrv

TOOL = os.path.join(HERE, "download-to-console.py")
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
TID = "PPSA90001"
PIECE = 1 << 20


def main():
    fails = []

    def check(cond, what):
        print(("PASS " if cond else "FAIL ") + what, flush=True)
        if not cond:
            fails.append(what)

    def wait(cond, secs=60):
        t = time.time()
        while time.time() - t < secs:
            try:
                if cond():
                    return True
            except Exception:
                pass
            time.sleep(0.3)
        return False

    work = tempfile.mkdtemp(prefix="bm-e2e-")
    alpha = A.game(TID, "Alpha Game")
    A.GAMES[:] = [alpha]
    name = alpha["name"]
    drives = os.path.join(work, "drives", "PS5 Games")
    os.makedirs(drives)
    image = os.path.join(drives, name)
    open(image, "wb").write(T.rnd(alpha["size"], TID))

    # 1) the payload: the real blazingmountd.c, HOST build
    exe, aport = os.path.join(work, "bmd"), A.free_port()
    out = os.path.join(HERE, "..", "console", "blazingmountd", "out")
    inc = out if os.path.exists(os.path.join(out, "fork_blob.h")) else work
    if inc == work:
        open(os.path.join(work, "fork_blob.h"), "w").write('#define BM_FORK_SHA "0000"\nstatic const unsigned long bm_fork_len = 8;\n'
                                                           'static const unsigned char bm_fork[] = "blz_link";\n')
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="e2e-test"', "-DBM_API_PORT=%d" % aport,
                        "-DAPI_WATCH_S=2", "-I", inc, "-o", exe, os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
    check(r.returncode == 0, "the real payload builds (HOST)" + (": " + r.stderr[:400] if r.returncode else ""))
    if r.returncode:
        return 1

    # 2) the console: PHAT5's layout (homebrew on the M.2), auto on; its ftpsrv + websrv over the same folder
    root = T.ROOT = os.path.join(work, "console")
    b, hb = os.path.join(root, "data", "blazing"), os.path.join(root, "mnt", "ext1", "homebrew")
    for d in (b, hb, os.path.join(root, "data", "shadowmount")):
        os.makedirs(d)
    open(os.path.join(root, "data", "shadowmount", "debug.log"), "w").write("")
    T.CFG["free"] = 200 * 1024 ** 3                     # AVBL: the tool keeps 50 GiB free when the payload's min_free_mb is 0
    img = A.http.server.ThreadingHTTPServer(("127.0.0.1", 0), A.Fake)
    ftp = T.TS(("127.0.0.1", 0), T.FTP)
    web = T.HS(("127.0.0.1", 0), T.Web)
    T.PORTS = {"ftp": ftp.server_address[1], "web": web.server_address[1], "api": aport, "img": -1, "dir": -1}
    for s in (img, ftp, web):
        threading.Thread(target=s.serve_forever, daemon=True).start()
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n"
                                                   "homebrew_dir /mnt/ext1/homebrew\n" % img.server_address[1])
    open(os.path.join(b, "choices.tsv"), "w").write("auto\t1\n")
    proc = subprocess.Popen([exe], env=dict(os.environ, BM_ROOT=root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # mac1's side: stand-ins for ps5-status / ps5-claim / ps5-release and the TV notice
    tools, home, binp = (os.path.join(work, n) for n in ("tools", "home", "bin"))
    for d in (tools, home, binp):
        os.makedirs(d)
    open(os.path.join(binp, "ps5-status"), "w").write('#!/bin/sh\necho "X: IDLE"\n')
    for n in ("ps5-claim", "ps5-release"):
        open(os.path.join(binp, n), "w").write('#!/bin/sh\necho "$(basename $0) $*" >> "%s/claims.log"\n' % work)
    for n in os.listdir(binp):
        os.chmod(os.path.join(binp, n), 0o755)
    open(os.path.join(tools, "ps5notice.py"), "w").write(
        "def notify(ip, text):\n    open(%r, 'a').write(text + '\\n')\n    return True\n" % os.path.join(work, "notices.log"))
    imglog = os.path.join(work, "imgserve.log")
    open(imglog, "w").write("")
    env = dict(os.environ, PATH=binp + ":" + os.environ["PATH"], BM_DL_HOME=home, BM_DL_PIECE=str(PIECE),
               BM_DL_FTP_PORT=str(T.PORTS["ftp"]), BM_DL_WEB_PORT=str(T.PORTS["web"]), BM_DL_API_PORT=str(aport),
               BM_IMGSERVE="http://127.0.0.1:%d" % img.server_address[1], BM_DIRSERVE="http://127.0.0.1:9",
               BM_DL_ROOTS=os.path.join(work, "drives"), BM_IMGSERVE_LOG=imglog, BLAZING_PS5_TOOLS=tools,
               BM_DL_CONSOLE_FW="10.20", BM_DL_HUB="", BM_DL_SMP_WAIT="0", BM_DL_POLL="0.2", BM_DL_SMP_GONE_WAIT="5",
               BM_DL_PROVEN=os.path.join(work, "proven.tsv"), BM_DL_OTHERS="", BM_DIRSERVE_LOG=os.path.join(work, "dirserve.log"))

    api = "http://127.0.0.1:%d" % aport

    def j(path):
        with urllib.request.urlopen(api + path, timeout=10) as r:
            return json.loads(r.read())

    def act(q):
        req = urllib.request.Request(api + "/api/action?" + q, method="POST", headers={"X-BM-Request": "1"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code

    def row():
        return next((g for g in j("/api/library")["games"] if g["tid"] == TID), None)

    def serve():
        r = subprocess.run([sys.executable, "-I", TOOL, "serve", "127.0.0.1", "--once", "--poll", "1", "--max", "1"],
                           env=env, capture_output=True, text=True, timeout=600)
        return r.returncode

    def tool_log():
        try:
            return open(os.path.join(home, "log")).read()
        except OSError:
            return ""

    def here():                                          # what carries the title id in the M.2's homebrew now
        return sorted(n for n in os.listdir(hb) if "(%s)" % TID in n)

    def pass_after(marker):                              # a whole pass ran after the payload logged marker (the action applied)
        ls = open(os.path.join(b, "blazingmountd.log")).read().splitlines()
        at = [k for k, l in enumerate(ls) if marker in l]
        return bool(at) and any("images listed" in l for l in ls[at[-1] + 1:])

    # the whole time: never the starter AND the image in homebrew at once, never two starters
    seen, stop = {"both": [], "max": 0}, threading.Event()

    def watch():
        while not stop.is_set():
            try:
                h = here()
            except OSError:
                h = []
            seen["max"] = max(seen["max"], len(h))
            if len(h) > 1:
                seen["both"].append(h)
            time.sleep(0.02)
    threading.Thread(target=watch, daemon=True).start()

    try:
        starter = os.path.join(hb, "Alpha Game (%s)" % TID)
        check(wait(lambda: row()["local"] == "starter" and os.path.isdir(starter)), "the payload built Alpha's starter on the M.2 (auto on)")
        check(row()["on_console"] is False, "a starter does not play without the network")

        # 3) the app's Download -> the real downloads.tsv -> serve -> get -> switch
        check(act("do=download&tid=" + TID) == 200, "the app's Download is accepted")
        check(wait(lambda: any(q["what"] == "download" and q["tid"] == TID for q in j("/api/downloads")["requests"])),
              "GET /api/downloads carries the request (written by the main loop)")
        code = serve()
        lg = tool_log()
        check(code == 0, "serve --once: the get run finished (exit %d)" % code)
        n = -(-alpha["size"] // PIECE)
        check("all %d pieces match on the console" % n in lg, "all %d pieces checked by the payload's own /api/sha256" % n)
        local = os.path.join(hb, name)
        check(os.path.isfile(local) and open(local, "rb").read() == open(image, "rb").read(), "the image in the M.2's homebrew, byte for byte")
        stub = os.path.join(root, "mnt", "ext1", "blz-stage", "stub")
        check(not os.path.exists(starter) and any(x.startswith("Alpha Game (%s)-off-" % TID) for x in os.listdir(stub)),
              "the starter parked in blz-stage/stub (renamed, not deleted)")
        check(not os.path.exists(os.path.join(b, "link", TID + ".cfg")) and any(x.startswith(TID + ".cfg.off-") for x in os.listdir(os.path.join(b, "link"))),
              "the link cfg renamed off")
        check(not os.listdir(os.path.join(root, "mnt", "ext1", "blz-stage", "incoming")), "nothing left in incoming/")
        check(wait(lambda: row()["local"] == "image" and row()["on_console"] is True), "/api/library: kind image, on_console")
        check(wait(lambda: row()["choice"] == "skip"), "the switch's hide reached choices.tsv (skip)")
        check(wait(lambda: row()["dl_state"] == "done"), "mac1's job 'done' through dl-status.tsv")
        check(j("/api/status")["dl_worker_at"] > 0, "/api/status: dl_worker_at from mac1's status")
        # Install pressed now forgets the payload's state for the key and overrides the skip: only the image being 'present'
        # keeps a second starter (a second tile, one title id) from being built next to it
        act("do=install&tid=" + TID)
        check(wait(lambda: pass_after("ui: install " + TID)) and here() == [name],
              "Install after the switch (state forgotten, skip overridden): no starter built over the image")

        # 4) the app's Remove local copy -> remove-local
        check(act("do=remove_local&tid=" + TID) == 200, "the app's Remove local copy is accepted")
        check(wait(lambda: any(q["what"] == "remove_local" and q["tid"] == TID for q in j("/api/downloads")["requests"])),
              "GET /api/downloads carries remove_local (one line per title: it replaced the download)")
        dels0 = len(T.CFG["deletes"])
        code = serve()
        check(code == 0, "serve --once: the remove-local run finished (exit %d)" % code)
        check(os.path.isdir(starter) and os.path.exists(os.path.join(b, "link", TID + ".cfg")), "the starter and its cfg are back")
        gone = [os.path.join(dp, f) for dp, _, fs in os.walk(root) for f in fs if f.startswith(name)]
        check(not gone, "the image is deleted everywhere on the console" + (": %s" % gone if gone else ""))
        dels = [p for p in T.CFG["deletes"][dels0:] if not p.startswith("/data/blazing/dl-status.tsv")]   # the worker's own file
        check(dels and all("/blz-stage/incoming/" in p for p in dels), "every delete was inside blz-stage/incoming/: %s" % dels)
        check(wait(lambda: row()["local"] == "starter" and row()["on_console"] is False), "/api/library: kind starter, streams again")
        check(wait(lambda: row()["choice"] == ""), "unhide reached choices.tsv (back to the normal rule)")
        check(wait(lambda: row()["dl_state"] == "removed"), "mac1's job 'removed' through dl-status.tsv")
        act("do=sync")
        check(wait(lambda: pass_after("ui: check the drives now")) and here() == ["Alpha Game (%s)" % TID],
              "a pass after the remove builds no second starter")
        stop.set()
        check(not seen["both"], "the homebrew never held two copies of the title at once" + (": %s" % seen["both"][:3] if seen["both"] else ""))
        check(proc.poll() is None, "the payload is still running")
    finally:
        stop.set()
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        for s in (img, ftp, web):
            s.shutdown()
    if fails:
        print("--- tool log tail\n" + "\n".join(tool_log().splitlines()[-30:]))
        for f in sorted(os.listdir(os.path.join(home, "runs"))) if os.path.isdir(os.path.join(home, "runs")) else []:
            print("--- run " + f + "\n" + open(os.path.join(home, "runs", f)).read()[-3000:])
    else:
        shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAIL" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
