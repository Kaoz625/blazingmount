#!/usr/bin/env python3
"""Tests for host/download-to-console.py against a fake console on 127.0.0.1 (no PS5, no network).

    python3 -I host/test_download_to_console.py

The fake console is a folder: an FTP server that behaves like PHAT5's ftpsrv 1.16-ng where the tool relies on it
(REST n + STOR truncates at n, LIST, AVBL, RNFR/RNTO, DELE, SITE CPFR/CPTO = a copy in the background), websrv's
/fs<path>/?fmt=json + file reads, and the payload API (/api/status, /api/sha256 of 2.8 or the 404 of 2.7,
/api/action). imgserve's /games.json and dirserve's listing are fakes too; ps5-status / ps5-claim / ps5-release and
ps5notice are stand-ins on PATH / BLAZING_PS5_TOOLS. Pieces are 64 KiB so files of a few hundred KiB have several.

Covered: a fresh image copy -> pieces checked -> switch (hide, starter parked, image in homebrew, cfg off, notice);
a connection cut mid-file -> resume from the console's .part; a corrupted piece -> found, sent again; payload 2.7
(no /api/sha256) -> sent but NOT switched, then `verify` + `switch` after the "update"; a folder game (dirserve files
into the starter's 0-byte placeholders); a full copy on another drive -> SITE CPFR/CPTO + check + overlay back +
switch; refusals (no space, a non-placeholder file in the way, a game running = waits); one run per job (lock).
"""
import hashlib
import http.server
import json
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "download-to-console.py")
PIECE = 64 * 1024
CFG = {"requests": [], "stor_rests": [], "sha_route": True, "drop_after": 0, "corrupt_at": -1, "free": 10 * 1024 ** 3, "running": "", "actions": []}


def rnd(n, seed):
    out, h = bytearray(), hashlib.sha256(seed.encode()).digest()
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:n])


# ---------------------------------------------------------------- the fake ftpsrv

class FTP(socketserver.StreamRequestHandler):
    def reply(self, s):
        self.wfile.write((s + "\r\n").encode())

    def cpath(self, p):
        return os.path.join(ROOT, p.lstrip("/"))

    def handle(self):
        self.rest, self.rnfr, self.cpfr, self.pasv = 0, None, None, None
        self.reply("220 fake ftpsrv")
        while True:
            line = self.rfile.readline()
            if not line:
                return
            cmd, _, arg = line.decode().rstrip("\r\n").partition(" ")
            cmd = cmd.upper()
            p = self.cpath(arg) if arg else None
            if cmd in ("USER", "PASS"):
                self.reply("230 ok")
            elif cmd == "TYPE":
                self.reply("200 ok")
            elif cmd == "PASV":
                self.pasv = socket.socket()
                self.pasv.bind(("127.0.0.1", 0))
                self.pasv.listen(1)
                port = self.pasv.getsockname()[1]
                self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
            elif cmd == "REST":
                self.rest = int(arg)
                self.reply("350 rest")
            elif cmd == "LIST":
                d, _ = self.pasv.accept()
                rows = []
                if os.path.isdir(p):
                    for n in sorted(os.listdir(p)):
                        fp = os.path.join(p, n)
                        isd = os.path.isdir(fp)
                        rows.append("%s 1 root root %d Jan 01 00:00 %s" % ("drwxr-xr-x" if isd else "-rw-r--r--",
                                                                          65536 if isd else os.path.getsize(fp), n))
                self.reply("150 list")
                d.sendall(("\r\n".join(rows) + ("\r\n" if rows else "")).encode())
                d.close()
                self.reply("226 done")
            elif cmd == "STOR":
                d, _ = self.pasv.accept()
                CFG["stor_rests"].append((os.path.basename(p), self.rest))
                self.reply("150 stor")
                mode = "r+b" if os.path.exists(p) and self.rest else "wb"
                if not self.rest and os.path.exists(p):
                    d.close()
                    self.reply("550 file exists")                # ftpsrv does not overwrite
                    continue
                with open(p, mode) as fh:
                    fh.seek(self.rest)
                    fh.truncate(self.rest)
                    got = 0
                    while True:
                        b = d.recv(65536)
                        if not b:
                            break
                        if CFG["corrupt_at"] >= 0 and self.rest + got <= CFG["corrupt_at"] < self.rest + got + len(b):
                            i = CFG["corrupt_at"] - self.rest - got
                            b = b[:i] + bytes([b[i] ^ 0xFF]) + b[i + 1:]
                            CFG["corrupt_at"] = -1
                        if CFG["drop_after"] and got + len(b) >= CFG["drop_after"]:
                            fh.write(b[:CFG["drop_after"] - got])
                            CFG["drop_after"] = 0
                            d.close()
                            self.connection.close()              # the hotspot drops
                            return
                        fh.write(b)
                        got += len(b)
                d.close()
                self.rest = 0
                self.reply("226 stored")
            elif cmd == "DELE":
                os.remove(p)
                self.reply("226 deleted")                        # ftpsrv answers 226 to DELE
            elif cmd == "MKD":
                if os.path.exists(p):
                    self.reply("550 exists")
                else:
                    os.mkdir(p)
                    self.reply("257 made")
            elif cmd == "RNFR":
                self.rnfr = p
                self.reply("350 ok" if os.path.exists(p) else "550 no such file")
            elif cmd == "RNTO":
                if not self.rnfr or os.path.exists(p):
                    self.reply("550 refused")
                else:
                    os.rename(self.rnfr, p)
                    self.reply("250 renamed")
            elif cmd == "AVBL":
                self.reply("213 %d" % CFG["free"])
            elif cmd == "SITE":
                sub, _, a = arg.partition(" ")
                if sub.upper() == "CPFR":
                    self.cpfr = self.cpath(a)
                    self.reply("350 ok")
                elif sub.upper() == "CPTO":
                    src, dst = self.cpfr, self.cpath(a)
                    threading.Thread(target=lambda: (time.sleep(0.3), shutil.copyfile(src, dst)), daemon=True).start()
                    self.reply("250 Copy started in background")
                else:
                    self.reply("500 ?")
            elif cmd == "QUIT":
                self.reply("221 bye")
                return
            else:
                self.reply("502 not here")


# ---------------------------------------------------------------- websrv, the payload API, imgserve, dirserve

class Web(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        path = urllib.parse.unquote(u.path)
        port = self.server.server_address[1]
        if port == PORTS["web"] and path.startswith("/fs"):
            fp = os.path.join(ROOT, path[3:].strip("/"))
            if q.get("fmt") == "json":
                if not os.path.isdir(fp):
                    return self.send(404, b"no")
                rows = [{"name": ".", "mode": "d", "size": 0}]
                for n in sorted(os.listdir(fp)):
                    isd = os.path.isdir(os.path.join(fp, n))
                    rows.append({"name": n, "mode": "d" if isd else "f", "size": 65536 if isd else os.path.getsize(os.path.join(fp, n))})
                return self.send(200, rows)
            if os.path.isfile(fp):
                return self.send(200, open(fp, "rb").read(), "application/octet-stream")
            return self.send(404, b"no")
        if port == PORTS["api"]:
            if path == "/api/status":
                return self.send(200, {"version": "2.8-test", "homebrew_dir": "/mnt/ext1/homebrew", "route": "TAILNET",
                                       "free_mb": {"data": 84000, "ext1": CFG["free"] // 2 ** 20}, "min_free_mb": 1})
            if path == "/api/sha256" and CFG["sha_route"]:
                fp = os.path.join(ROOT, q["path"].lstrip("/"))
                if not os.path.isfile(fp):
                    return self.send(404, {"error": "no such file"})
                off, ln = int(q["off"]), int(q["len"])
                with open(fp, "rb") as fh:
                    fh.seek(off)
                    b = fh.read(ln)
                return self.send(200, {"path": q["path"], "off": off, "len": len(b), "size": os.path.getsize(fp),
                                       "sha256": hashlib.sha256(b).hexdigest()})
            if path == "/api/downloads":
                return self.send(200, {"version": "2.8-test", "requests": CFG["requests"], "jobs": []})
            return self.send(404, {"error": "not found"})
        if port == PORTS["img"]:
            if path == "/games.json":
                return self.send(200, {"games": GAMES})
        if port == PORTS["dir"]:
            if path == "/":
                return self.send(200, ("dirserve\ngames (by title id):\n  PPSA70003 01.002.000  %s\n" % DIRSRC).encode(), "text/plain")
        return self.send(404, b"no")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if self.server.server_address[1] == PORTS["api"] and u.path == "/api/action":
            CFG["actions"].append(u.query)
            return self.send(200, {"ok": True})
        return self.send(404, b"no")


class TS(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


class HS(http.server.ThreadingHTTPServer):
    daemon_threads = True


# ---------------------------------------------------------------- the scenarios

def main():
    global ROOT, PORTS, GAMES, DIRSRC
    work = tempfile.mkdtemp(prefix="bm-dl-")
    ROOT = os.path.join(work, "console")
    src = os.path.join(work, "drives", "PS5 Games")
    tools, home, binp = (os.path.join(work, n) for n in ("tools", "home", "bin"))
    for d in (src, tools, home, binp):
        os.makedirs(d)
    ftp = TS(("127.0.0.1", 0), FTP)
    PORTS = {"ftp": ftp.server_address[1]}
    servers = [ftp]
    for k in ("web", "api", "img", "dir"):
        s = HS(("127.0.0.1", 0), Web)
        PORTS[k] = s.server_address[1]
        servers.append(s)
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    # stand-ins: what runs (CFG["running"] via a file), the lock tools, the TV notice
    runfile = os.path.join(work, "running")
    open(os.path.join(binp, "ps5-status"), "w").write('#!/bin/sh\nr=$(cat "%s" 2>/dev/null)\n[ -n "$r" ] && echo "X: RUNNING  $r  Game" || echo "X: IDLE"\n' % runfile)
    for n in ("ps5-claim", "ps5-release"):
        open(os.path.join(binp, n), "w").write('#!/bin/sh\necho "$(basename $0) $*" >> "%s/claims.log"\n' % work)
    for n in os.listdir(binp):
        os.chmod(os.path.join(binp, n), 0o755)
    open(os.path.join(tools, "ps5notice.py"), "w").write(
        "def notify(ip, text):\n    open(%r, 'a').write(text + '\\n')\n    return True\n" % os.path.join(work, "notices.log"))
    imglog = os.path.join(work, "imgserve.log")
    open(imglog, "w").write("")
    env = dict(os.environ, PATH=binp + ":" + os.environ["PATH"], BM_DL_HOME=home, BM_DL_PIECE=str(PIECE),
               BM_DL_FTP_PORT=str(PORTS["ftp"]), BM_DL_WEB_PORT=str(PORTS["web"]), BM_DL_API_PORT=str(PORTS["api"]),
               BM_IMGSERVE="http://127.0.0.1:%d" % PORTS["img"], BM_DIRSERVE="http://127.0.0.1:%d" % PORTS["dir"],
               BM_DL_ROOTS=os.path.join(work, "drives"), BM_IMGSERVE_LOG=imglog, BLAZING_PS5_TOOLS=tools,
               BM_DL_CONSOLE_FW="10.20", BM_DL_HUB="", BM_DL_SMP_WAIT="0")

    # the console: an M.2 with a homebrew, starters + their cfgs
    def mk(*parts):
        p = os.path.join(ROOT, *parts)
        os.makedirs(p, exist_ok=True)
        return p
    hb = mk("mnt", "ext1", "homebrew")
    link = mk("data", "blazing", "link")
    mk("data", "shadowmount")
    open(os.path.join(ROOT, "data", "shadowmount", "debug.log"), "w").write("")

    # three image games: Alpha (FW 7.00), Beta (FW 13.60 + an overlay kept in blz-disabled, a full copy parked on /data),
    # Delta (sent while the payload is 2.7)
    sizes = {"PPSA70001": 5 * PIECE + 12345, "PPSA70002": 3 * PIECE + 77, "PPSA70004": 2 * PIECE + 5}
    names = {"PPSA70001": "Alpha (PPSA70001).ffpfsc", "PPSA70002": "Beta (PPSA70002).ffpfsc", "PPSA70004": "Delta (PPSA70004).ffpfsc"}
    fws = {"PPSA70001": "7.00", "PPSA70002": "13.60", "PPSA70004": "5.00"}
    GAMES = []
    for tid, n in names.items():
        open(os.path.join(src, n), "wb").write(rnd(sizes[tid], tid))
        GAMES.append({"tid": tid, "title": n.split(" (")[0], "fw": fws[tid], "size": sizes[tid], "key": "%s@%d" % (tid, sizes[tid]), "name": n})
        st = mk("mnt", "ext1", "homebrew", "%s (%s)" % (n.split(" (")[0], tid))
        open(os.path.join(st, "eboot.bin"), "wb").write(b"starter")
        open(os.path.join(link, tid + ".cfg"), "w").write("mode remote\nserver 192.168.1.130 4451\nbase /t/%s@%d\nfile 100 eboot.bin\n" % (tid, sizes[tid]))
    mk("mnt", "ext1", "blz-disabled", "PPSA70002-overlay-20261022-0838", "fakelib")
    mk("data", "blz-parked")
    shutil.copyfile(os.path.join(src, names["PPSA70002"]), os.path.join(ROOT, "data", "blz-parked", names["PPSA70002"]))
    # a folder game (dirserve): two big files are 0-byte placeholders in its starter
    DIRSRC = os.path.join(work, "drives", "homebrew", "PPSA70003-app")
    big = {"cj/content/paks/a.ucas": 4 * PIECE + 999, "cj/content/movies/b.bk2": PIECE // 3}
    for rel, n in big.items():
        os.makedirs(os.path.dirname(os.path.join(DIRSRC, rel)), exist_ok=True)
        open(os.path.join(DIRSRC, rel), "wb").write(rnd(n, rel))
    gst = mk("mnt", "ext1", "homebrew", "Gamma (PPSA70003)")
    for rel in big:
        os.makedirs(os.path.dirname(os.path.join(gst, rel)), exist_ok=True)
        open(os.path.join(gst, rel), "wb").close()
    open(os.path.join(link, "PPSA70003.cfg"), "w").write("mode remote\nserver 192.168.1.130 4452\nbase /t/PPSA70003@01.002.000\n" +
                                                         "".join("file %d %s\n" % (n, rel) for rel, n in big.items()))

    fails = []

    def check(cond, what):
        print(("PASS " if cond else "FAIL ") + what, flush=True)
        if not cond:
            fails.append(what)

    def run(*a, timeout=120):
        r = subprocess.run([sys.executable, "-I", TOOL] + list(a) + ["--every", "0.2"], env=env, capture_output=True, text=True,
                           timeout=timeout)
        return r.returncode, r.stdout + r.stderr

    def job(tid):
        try:
            return json.load(open(os.path.join(home, "jobs", "127.0.0.1-%s.json" % tid)))
        except OSError:
            return {}

    def same(console_path, local):
        p = os.path.join(ROOT, console_path.lstrip("/"))
        return os.path.isfile(p) and open(p, "rb").read() == open(local, "rb").read()

    con = "127.0.0.1"
    try:
        # 1) plan changes nothing
        before = sorted(os.walk(ROOT))
        code, out = run("plan", con, "PPSA70001", "PPSA70002", "PPSA70003")
        check(code == 0 and "ALREADY ON THE CONSOLE: /data/blz-parked/Beta" in out and "[files]" in out and "needs its overlay" in out,
              "plan: sizes, the parked full copy, the folder game, the overlay need" + ("" if code == 0 else ": " + out[-600:]))
        check(sorted(os.walk(ROOT)) == before, "plan changed nothing on the console")

        # 2) Alpha: a cut after 2.5 pieces, then a corrupted byte in piece 4: resumed, found, sent again, switched
        CFG.update(drop_after=2 * PIECE + PIECE // 2, corrupt_at=4 * PIECE + 100)
        code, out = run("get", con, "PPSA70001")
        final = "/mnt/ext1/homebrew/" + names["PPSA70001"]
        check(code == 0 and same(final, os.path.join(src, names["PPSA70001"])), "Alpha: in homebrew, byte for byte" + ("" if code == 0 else ": " + out[-900:]))
        rests = [r for n, r in CFG["stor_rests"] if n.startswith("Alpha")]
        check("send stopped" in out and 2 * PIECE + PIECE // 2 in rests, "Alpha: the cut was resumed from the console's .part (REST %s)" % rests)
        check(("did not match" in out or "does NOT match" in out) and 4 * PIECE in rests, "Alpha: the corrupted piece was found on the console and sent again from its start")
        j = job("PPSA70001")
        f = j.get("files", {}).get(names["PPSA70001"], {})
        check(j.get("state") == "done" and len(f.get("verified", [])) == 6 and len(f.get("pieces", [])) == 6, "Alpha: 6 pieces, 6 checked, done")
        check(not os.path.exists(os.path.join(hb, "Alpha (PPSA70001)")) and
              any(n.startswith("Alpha (PPSA70001)-off-") for n in os.listdir(os.path.join(ROOT, "mnt", "ext1", "blz-stage", "stub"))),
              "Alpha: starter parked in blz-stage/stub")
        check(not os.path.exists(os.path.join(link, "PPSA70001.cfg")) and any(n.startswith("PPSA70001.cfg.off-") for n in os.listdir(link)),
              "Alpha: link cfg renamed .cfg.off-<stamp>")
        check("do=hide&tid=PPSA70001" in CFG["actions"], "Alpha: hidden in the payload first")
        check(not os.path.exists(os.path.join(hb, "backports", "PPSA70001")), "Alpha (FW 7.00): no overlay made")
        check("Alpha is on your console now" in open(os.path.join(work, "notices.log")).read(), "Alpha: TV notice")
        check("ps5-claim 127.0.0.1 dl-local" in open(os.path.join(work, "claims.log")).read(), "Alpha: claimed for the switch, released after")

        # 3) Delta while the payload is 2.7: sent, not switched; then the 'update', verify, switch
        CFG["sha_route"] = False
        code, out = run("get", con, "PPSA70004")
        check(job("PPSA70004").get("state") == "waiting" and "payload 2.8" in job("PPSA70004").get("msg", "")
              and os.path.isdir(os.path.join(hb, "Delta (PPSA70004)")), "Delta (2.7): sent, waits for 2.8, starter untouched")
        CFG["sha_route"] = True
        code, out = run("verify", con, "PPSA70004")
        check(job("PPSA70004").get("state") == "switching", "Delta: verify after the update -> ready to switch")
        code, out = run("switch", con, "PPSA70004")
        check(job("PPSA70004").get("state") == "done" and same("/mnt/ext1/homebrew/" + names["PPSA70004"], os.path.join(src, names["PPSA70004"])),
              "Delta: switched" + ("" if code == 0 else ": " + out[-600:]))

        # 4) Gamma, a folder game: the files go into the starter's placeholders
        code, out = run("get", con, "PPSA70003")
        ok = all(same("/mnt/ext1/homebrew/Gamma (PPSA70003)/" + rel, os.path.join(DIRSRC, rel)) for rel in big)
        check(code == 0 and ok and job("PPSA70003").get("state") == "done", "Gamma: the real files are in the starter" + ("" if ok else ": " + out[-900:]))
        stub = os.path.join(ROOT, "mnt", "ext1", "blz-stage", "stub")
        ph = [n for n in os.listdir(stub) if n.startswith("PPSA70003-placeholders-")]
        check(ph and os.path.getsize(os.path.join(stub, ph[0], "cj/content/paks/a.ucas")) == 0, "Gamma: the placeholders are kept in blz-stage/stub")
        check(any(n.startswith("PPSA70003.cfg.off-") for n in os.listdir(link)), "Gamma: cfg off")

        # 5) Beta: the full copy on /data -> copied on the console (waits while a game runs), checked, overlay back, switched
        open(runfile, "w").write("PPSA70001")
        p = subprocess.Popen([sys.executable, "-I", TOOL, "get", con, "PPSA70002", "--every", "0.2"], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(4)
        check(job("PPSA70002").get("state") == "waiting" and not os.path.exists(os.path.join(ROOT, "mnt", "ext1", "blz-stage", "incoming",
                                                                                              names["PPSA70002"] + ".part")),
              "Beta: no on-console copy while a game runs")
        # a second run on the same title is refused by the lock
        code, out = run("get", con, "PPSA70002")
        check("another run is working on it" in out, "one run per job (lock)")
        p.kill()
        p.wait()
        os.remove(runfile)
        code, out = run("get", con, "PPSA70002", timeout=200)
        check(code == 0 and same("/mnt/ext1/homebrew/" + names["PPSA70002"], os.path.join(src, names["PPSA70002"])),
              "Beta: copied on the console, checked, switched" + ("" if code == 0 else ": " + out[-900:]))
        check(os.path.isdir(os.path.join(hb, "backports", "PPSA70002", "fakelib")) and
              not os.path.exists(os.path.join(ROOT, "mnt", "ext1", "blz-disabled", "PPSA70002-overlay-20261022-0838")),
              "Beta (FW 13.60 on 10.20): its overlay renamed back from blz-disabled")
        check(os.path.exists(os.path.join(ROOT, "data", "blz-parked", names["PPSA70002"])), "Beta: the /data copy is left alone (not the tool's to delete)")

        # 5b) put: plain files (BlazingArch ROMs) with the same pieces and checks, then renamed into --to
        roms = []
        for i, n in enumerate(("Sonic Adventure DX (USA).rvz", "Tom Clancy's Splinter Cell (USA).rvz")):
            roms.append(os.path.join(work, n))
            open(roms[-1], "wb").write(rnd(2 * PIECE + 1000 * (i + 1), n))
        code, out = run("put", con, *roms, "--to", "/mnt/ext1/BlazingArch/content/gamecube")
        ok = all(same("/mnt/ext1/BlazingArch/content/gamecube/" + os.path.basename(r), r) for r in roms)
        check(code == 0 and ok and job("put-gamecube").get("state") == "done", "put: ROMs checked and placed in --to" + ("" if ok else ": " + out[-700:]))
        code, out = run("put", con, roms[0], "--to", "/data/homebrew/PPSA99171/content/gamecube")
        check(code != 0 and "never /data" in out, "put: /data refused on a console with an M.2")
        # 5c) serve: the app's request (through the payload) -> a run -> switched; progress written back as dl-status.tsv
        n6 = "Zeta (PPSA70006).ffpfsc"
        open(os.path.join(src, n6), "wb").write(rnd(2 * PIECE + 3, "zeta"))
        GAMES.append({"tid": "PPSA70006", "title": "Zeta", "fw": "5.00", "size": 2 * PIECE + 3, "key": "PPSA70006@%d" % (2 * PIECE + 3), "name": n6})
        os.makedirs(os.path.join(hb, "Zeta (PPSA70006)"))
        open(os.path.join(link, "PPSA70006.cfg"), "w").write("mode remote\nserver 1.2.3.4 4451\nbase /t/PPSA70006@%d\n" % (2 * PIECE + 3))
        CFG["requests"] = [{"what": "download", "tid": "PPSA70006", "at": 1000}, {"what": "cancel", "tid": "PPSA70007", "at": 1001}]
        code, out = run("serve", con, "--once", "--poll", "1", timeout=200)
        st6 = os.path.join(ROOT, "data", "blazing", "dl-status.tsv")
        lines = open(st6).read().splitlines() if os.path.exists(st6) else []
        check(code == 0 and same("/mnt/ext1/homebrew/" + n6, os.path.join(src, n6)), "serve: the app's download request ran and switched" + ("" if code == 0 else ": " + out[-700:]))
        check(any(l.startswith("job\tPPSA70006\tdone\t%d\t%d\t" % (2 * PIECE + 3, 2 * PIECE + 3)) for l in lines) and
              any(l.startswith("worker\t") for l in lines) and any(l.startswith("rate\t") for l in lines),
              "serve: dl-status.tsv on the console (worker, rate, the job done)" + ("" if lines else ": no file"))
        check(not any("put-" in l for l in lines), "serve: put jobs are not written as titles")
        check(os.path.exists(os.path.join(home, "jobs", "127.0.0.1-PPSA70007.stop")), "serve: the app's cancel leaves the title's stop file")
        code, out = run("serve", con, "--once", "--poll", "1", timeout=60)
        check("download asked" not in out and code == 0, "serve: a request already handled is not run twice")
        # 6) refusals
        GAMES.append({"tid": "PPSA70005", "title": "Eps", "fw": "5.00", "size": 3 * PIECE, "key": "PPSA70005@%d" % (3 * PIECE), "name": "Eps (PPSA70005).ffpfsc"})
        open(os.path.join(src, "Eps (PPSA70005).ffpfsc"), "wb").write(rnd(3 * PIECE, "eps"))
        CFG["free"] = 0
        code, out = run("get", con, "PPSA70005")
        check(job("PPSA70005").get("state") == "failed" and "no space" in job("PPSA70005").get("msg", ""), "no space: failed before a byte is sent")
        CFG["free"] = 10 * 1024 ** 3
    finally:
        for s in servers:
            s.shutdown()
        if not fails:
            shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAILED (left in %s)" % (len(fails), work))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
