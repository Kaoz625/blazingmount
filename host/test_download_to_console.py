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
remove-local: an image (the starter back only after ShadowMount dropped the image), a folder game (placeholders back),
an overlay back to blz-disabled, ShadowMount silent (everything put back, still plays; asked again finishes), a pkg install
and a stranger's full copy refused, the app's request through serve, downloaded again after; nothing deleted outside
incoming/. prove: 0 requests = PROVEN LOCAL + a games-proven line, a request = NOT LOCAL, the other console running it =
inconclusive, never run = not proven.
"""
import hashlib
import http.server
import json
import os
import re
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
CFG = {"requests": [], "stor_rests": [], "sha_route": True, "drop_after": 0, "corrupt_at": -1, "free": 10 * 1024 ** 3, "running": "", "actions": [],
       "deletes": [], "smp_silent": False}


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
                CFG["deletes"].append(arg)
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
                    # ShadowMount: something left homebrew (a starter at a switch, an image at a remove) -> one log line
                    src = "/" + os.path.relpath(self.rnfr, ROOT)
                    m = re.search(r"\((PPSA\d{5})\)", src)
                    if m and os.path.dirname(src) == "/mnt/ext1/homebrew" and not CFG["smp_silent"]:
                        with open(os.path.join(ROOT, "data", "shadowmount", "debug.log"), "a") as fh:
                            fh.write("[%s]   [CACHE] source removed: %s (%s)\n" % (time.strftime("%H:%M:%S"), m.group(1), src))
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
    # running-<name> answers for one console (the 'other' console of prove), else the shared file
    open(os.path.join(binp, "ps5-status"), "w").write('#!/bin/sh\nf="%s-$1"; [ -f "$f" ] || f="%s"\nr=$(cat "$f" 2>/dev/null)\n'
                                                      '[ -n "$r" ] && echo "X: RUNNING  $r  Game" || echo "X: IDLE"\n' % (runfile, runfile))
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
               BM_DL_CONSOLE_FW="10.20", BM_DL_HUB="", BM_DL_SMP_WAIT="0", BM_DL_POLL="0.2", BM_DL_SMP_GONE_WAIT="2",
               BM_DL_PROVEN=os.path.join(work, "proven.tsv"), BM_DL_OTHERS="OTHER1", BM_DIRSERVE_LOG=os.path.join(work, "dirserve.log"))

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

        # 7) remove-local: back to streaming, the copy deleted (only in incoming/), every step from the switch's own journal
        inc = os.path.join(ROOT, "mnt", "ext1", "blz-stage", "incoming")
        stub = os.path.join(ROOT, "mnt", "ext1", "blz-stage", "stub")
        code, out = run("remove-local", con, "PPSA70001")
        check(code != 0 and "--confirm" in out and os.path.isfile(os.path.join(hb, names["PPSA70001"])), "remove-local: refused without --confirm, nothing changed")
        CFG["actions"].clear()
        code, out = run("remove-local", con, "PPSA70001", "--confirm")
        j = job("PPSA70001")
        check(code == 0 and j.get("state") == "removed" and not os.path.exists(os.path.join(hb, names["PPSA70001"])),
              "Alpha: removed, the image is out of homebrew" + ("" if code == 0 else ": " + out[-900:]))
        check(os.path.isfile(os.path.join(hb, "Alpha (PPSA70001)", "eboot.bin")) and os.path.isfile(os.path.join(link, "PPSA70001.cfg"))
              and not any(n.startswith("Alpha (PPSA70001)-off-") for n in os.listdir(stub)), "Alpha: the starter + its cfg are back")
        check(not any("Alpha" in n for n in os.listdir(inc)), "Alpha: the image deleted from incoming/ (space freed)")
        check(CFG["actions"] == ["do=hide&tid=PPSA70001", "do=unhide&tid=PPSA70001"], "Alpha: hidden while it changed, shown again after: %s" % CFG["actions"])
        check("ShadowMount let go" in out and out.index("ShadowMount let go") < out.index("Alpha (PPSA70001)-off-"),
              "Alpha: the starter came back only after ShadowMount dropped the image")
        check(not j.get("undo") and not j.get("files") and len(j.get("history", [])) == 1, "Alpha: journal reset (undo kept in history)")
        check("local copy is removed" in open(os.path.join(work, "notices.log")).read(), "Alpha: TV notice")
        # a folder game: real files out + deleted, 0-byte placeholders back, cfg back
        code, out = run("remove-local", con, "PPSA70003", "--confirm")
        gst_ok = all(os.path.getsize(os.path.join(gst, rel)) == 0 for rel in big)
        check(code == 0 and gst_ok and job("PPSA70003").get("state") == "removed" and os.path.isfile(os.path.join(link, "PPSA70003.cfg")),
              "Gamma (folder game): placeholders back, cfg back" + ("" if code == 0 else ": " + out[-900:]))
        check(not [f for dp, _, fns in os.walk(os.path.join(inc, "PPSA70003-files")) for f in fns], "Gamma: the real files deleted from incoming/")
        # the overlay the switch restored goes back to blz-disabled; the /data copy stays
        code, out = run("remove-local", con, "PPSA70002", "--confirm")
        check(code == 0 and not os.path.exists(os.path.join(hb, "backports", "PPSA70002"))
              and os.path.isdir(os.path.join(ROOT, "mnt", "ext1", "blz-disabled", "PPSA70002-overlay-20261022-0838", "fakelib")),
              "Beta: its overlay back in blz-disabled under its old name" + ("" if code == 0 else ": " + out[-900:]))
        check(os.path.exists(os.path.join(ROOT, "data", "blz-parked", names["PPSA70002"])) and os.path.isdir(os.path.join(hb, "Beta (PPSA70002)")),
              "Beta: starter back, the /data copy still left alone")
        # ShadowMount never lets go -> everything back as it was, it still plays; a later run (it does let go) finishes
        CFG["smp_silent"] = True
        code, out = run("remove-local", con, "PPSA70004", "--confirm")
        j = job("PPSA70004")
        check(code != 0 and j.get("state") == "failed" and "ShadowMount" in j.get("msg", "") and same("/mnt/ext1/homebrew/" + names["PPSA70004"],
              os.path.join(src, names["PPSA70004"])) and not os.path.exists(os.path.join(hb, "Delta (PPSA70004)")),
              "Delta: ShadowMount silent -> copy put back in homebrew, starter still parked, failed with the reason")
        code, out = run("get", con, "PPSA70004")
        check("already on the console" in out and not any(n.startswith("Delta") for n in os.listdir(inc)), "Delta: a stopped remove is still a copy on the console (get sends nothing)")
        CFG["smp_silent"] = False
        code, out = run("remove-local", con, "PPSA70004", "--confirm")
        check(code == 0 and job("PPSA70004").get("state") == "removed" and os.path.isdir(os.path.join(hb, "Delta (PPSA70004)")), "Delta: asked again -> removed")
        # what is not the tool's: a pkg install, a full copy someone else made
        mk("mnt", "ext1", "user", "app", "PPSA70009")
        open(os.path.join(ROOT, "mnt", "ext1", "user", "app", "PPSA70009", "app.pkg"), "wb").write(b"p" * 1000)
        code, out = run("remove-local", con, "PPSA70009", "--confirm")
        check(code != 0 and "installed game" in job("PPSA70009").get("msg", "") and
              os.path.getsize(os.path.join(ROOT, "mnt", "ext1", "user", "app", "PPSA70009", "app.pkg")) == 1000, "a pkg install is refused, untouched")
        om = mk("mnt", "ext1", "homebrew", "Omega (PPSA70010)")
        open(os.path.join(om, "eboot.bin"), "wb").write(b"real")
        code, out = run("remove-local", con, "PPSA70010", "--confirm")
        check(code != 0 and "did not make" in job("PPSA70010").get("msg", "") and os.path.isfile(os.path.join(om, "eboot.bin")),
              "a full copy the tool did not make is refused, untouched")
        # the app asks (through the payload) -> serve runs remove-local -> dl-status says removed
        CFG["requests"] = [{"what": "remove_local", "tid": "PPSA70006", "at": 3000}]
        code, out = run("serve", con, "--once", "--poll", "1", timeout=120)
        lines = open(st6).read().splitlines() if os.path.exists(st6) else []
        check(code == 0 and "remove_local asked" in out and any(l.startswith("job\tPPSA70006\tremoved\t") for l in lines)
              and not os.path.exists(os.path.join(hb, n6)), "serve: the app's remove_local ran; dl-status says removed" + ("" if code == 0 else ": " + out[-700:]))
        # removed, then downloaded again: a fresh copy, not 'already done'
        code, out = run("get", con, "PPSA70001")
        check(code == 0 and job("PPSA70001").get("state") == "done" and same("/mnt/ext1/homebrew/" + names["PPSA70001"], os.path.join(src, names["PPSA70001"])),
              "Alpha: downloaded again after the remove" + ("" if code == 0 else ": " + out[-700:]))
        bad = [d for d in CFG["deletes"] if "/blz-stage/incoming/" not in d and not d.startswith("/data/blazing/dl-status.tsv")]
        check(not bad, "nothing outside blz-stage/incoming/ (and the worker's own dl-status.tsv) was ever deleted %s" % bad)

        # 8) prove: passive; 0 imgserve requests for the title while it runs = PROVEN LOCAL
        open(runfile, "w").write("PPSA70001")
        open(runfile + "-OTHER1", "w").write("")
        code, out = run("prove", con, "PPSA70001", "--minutes", "0.03", "--wait-hours", "0.01")
        proven = os.path.join(work, "proven.tsv")
        check(code == 0 and "PROVEN LOCAL" in out and os.path.exists(proven) and "PPSA70001\t127.0.0.1\t" in open(proven).read()
              and job("PPSA70001").get("proven_at"), "prove: 0 requests -> PROVEN LOCAL, a games-proven line" + ("" if code == 0 else ": " + out[-500:]))
        p = subprocess.Popen([sys.executable, "-I", TOOL, "prove", con, "PPSA70001", "--minutes", "0.05", "--wait-hours", "0.01"], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        outl = []
        for line in p.stdout:                                    # a request once it counts (a timer races a loaded Mac)
            outl.append(line)
            if "counting requests" in line:
                with open(imglog, "a") as fh:
                    fh.write("12:00:00.000 GET PPSA70001@327680/eboot.bin range=0-1 -> 206 2 B 0 ms fast\n")
        code, out = p.wait(), "".join(outl)
        check(code == 1 and "NOT LOCAL" in out, "prove: an imgserve request for it while it runs -> NOT LOCAL" + ("" if code == 1 else ": " + out[-400:]))
        open(runfile + "-OTHER1", "w").write("PPSA70001")
        n0 = open(proven).read().count("\n")
        code, out = run("prove", con, "PPSA70001", "--minutes", "0.03", "--wait-hours", "0.01")
        check(code == 3 and "INCONCLUSIVE" in out and open(proven).read().count("\n") == n0, "prove: the other console runs it too -> inconclusive, no proof line")
        code, out = run("prove", con, "PPSA70002", "--minutes", "0.03", "--wait-hours", "0.0005")
        check(code == 3 and "did not run" in out, "prove: a title that never runs is not proven")
        os.remove(runfile)
        os.remove(runfile + "-OTHER1")
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
