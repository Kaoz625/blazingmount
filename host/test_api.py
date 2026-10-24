#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_api.py - fake-console test of blazingmountd's local API (bm_api.c, :8094): what the BlazingMount app reads and asks.

    python3 -I host/test_api.py            (no console, no real imgserve: a fake imgserve with three tiny games runs in-process)

Builds a HOST=1 blazingmountd with the API hooks (when blazingmountd.c does not #include "bm_api.c" yet, the hooks from
host/api_hooks.py are applied to a temp copy), runs it resident against a fake console root, and checks:
  1. /api/status, /api/library (host facts + this console's copy + choice + proof), /api/settings, /api/log, /api/icon
  2. a POST without "X-BM-Request: 1" is refused; unknown actions, settings and newline values are refused
  3. install -> the starter is built by the main loop (eboot 0755), state 'installed'; remove -> folder + cfg gone, 'skip'
  4. remove is refused while the game runs, and never deletes a full copy
  5. hide / unhide (forget) / set / auto land in choices.tsv and mount.conf
  6. a game added on the drives shows up by itself (the API's watch) and, with auto on, gets built without any request
  7. DISCOVER: /api/discover + /api/wishlist + dsheet covers pass through; a wish press is forwarded at once to the host's POST /wishlist
     with X-BM-Request (bad ids refused, a host refusal reported); the host's alerts raise ONE notice each per pass, never twice (304,
     same alerts under a new ETag), a new alert raises one more, and alerts-seen.tsv stays capped at 200 lines
"""
import hashlib, http.server, urllib.error, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 64

def game(tid, title, fw="4.50"):
    files = {"eboot.bin": b"ELF.." + b"libSceAmpr.prx" + b"\0" * 2000, "sce_sys/param.json": json.dumps({"titleId": tid}).encode(),
             "sce_sys/icon0.png": PNG, "data/big.pak": b"\1" * (1 << 20) + b"\2" * 10, "data/small.txt": b"hello"}
    size = 10_000_000 + int(tid[4:])
    return {"tid": tid, "title": title, "version": "01.000.000", "fw": fw, "sdk": "4.00", "size": size, "key": "%s@%d" % (tid, size),
            "name": "%s (%s).ffpfsc" % (title, tid), "files": files}

GAMES = [game("PPSA90001", "Alpha Game"), game("PPSA90002", "Beta Game"), game("PPSA90003", "Gamma Needs FW", fw="99.00")]
LATE = game("PPSA90004", "Delta Added Later")

DISC = {"etag": "disc0001", "alerts": [
    {"id": "aaaaaaaaaaaa", "title": "Wished \u00e9 Game", "kind": "out", "date": 1761200000, "key": "aaaaaaaaaaaa:out:1761200000", "date_text": "23 Oct 2026"},
    {"id": "bbbbbbbbbbbb", "title": "Later Big", "kind": "moved", "date": 1766000000, "key": "bbbbbbbbbbbb:moved:1766000000", "date_text": "17 Dec 2026"}]}
WISH_POSTS = []

class Fake(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        WISH_POSTS.append((self.path, self.headers.get("X-BM-Request")))
        ok = self.path.startswith("/wishlist?id=aaaaaaaaaaaa&on=")
        body = b'{"ok":true}' if ok else b'{"ok":false,"error":"not a game in discover.json"}'
        self.send_response(200 if ok else 404); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def send(self, body, ctype="text/plain"):
        self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        p = urllib.request.unquote(self.path)
        if p == "/games.json":
            return self.send(json.dumps([{k: v for k, v in g.items() if k != "files"} for g in GAMES]).encode(), "application/json")
        if p == "/games.txt":
            return self.send(("# key\ttid\tsize\tversion\tfw\tsdk\ttitle\n" + "".join(
                "%s\t%s\t%d\t%s\t%s\t%s\t%s\n" % (g["key"], g["tid"], g["size"], g["version"], g["fw"], g["sdk"], g["title"]) for g in GAMES)).encode())
        if p == "/library.json":
            body = json.dumps({"count": 2, "games": [{"id": "aaaaaaaaaaaa", "title": "Alpha Game", "system": "ps5"},
                                                     {"id": "bbbbbbbbbbbb", "title": "Old Thing", "system": "ps2"}]}).encode()
            self.send_response(200); self.send_header("ETag", '"cat0001"'); self.send_header("Content-Length", str(len(body))); self.end_headers()
            return self.wfile.write(body)
        if p == "/covers.json":
            return self.send(b'{"covers":{"aaaaaaaaaaaa":{"page":0,"cell":0}}}', "application/json")
        if p == "/covers/thumb/aaaaaaaaaaaa.jpg":
            return self.send(b"\xff\xd8\xff" + b"0" * 100, "image/jpeg")
        if p == "/discover.json":
            tag = '"%s"' % DISC["etag"]
            if self.headers.get("If-None-Match") == tag:
                self.send_response(304); self.send_header("ETag", tag); self.end_headers(); return
            body = json.dumps({"alerts": DISC["alerts"], "shelves": [], "games": [{"id": "aaaaaaaaaaaa", "title": "Wished \u00e9 Game"}]}).encode()
            self.send_response(200); self.send_header("ETag", tag); self.send_header("Content-Length", str(len(body))); self.end_headers()
            return self.wfile.write(body)
        if p == "/wishlist.json":
            return self.send(b'{"version":1,"items":[{"id":"aaaaaaaaaaaa"}]}', "application/json")
        if p == "/covers/dsheet/0.jpg":
            return self.send(b"\xff\xd8\xff" + b"1" * 50, "image/jpeg")
        if p == "/proven.tsv":
            return self.send(b"PPSA90001\tDIGI5\t2026-10-24 10:00\tplayed: New Game, walked\n")
        for g in GAMES:
            if p == "/f/" + g["key"]:
                return self.send("".join("%d\t%s\n" % (len(b), n) for n, b in g["files"].items()).encode())
            pre = "/t/%s/" % g["key"]
            if p.startswith(pre) and p[len(pre):] in g["files"]:
                return self.send(g["files"][p[len(pre):]])
        self.send_response(404); self.end_headers()

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def main():
    fails = 0
    def check(cond, what):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + what); fails += 0 if cond else 1

    work = tempfile.mkdtemp(prefix="bm-api-")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    hport, aport = srv.server_address[1], free_port()
    proc = None
    try:
        # 1) the build: the real blazingmountd.c when it already has the hooks, else a temp copy with them
        src = os.path.join(work, "src"); shutil.copytree(SRC, src)
        main_c = os.path.join(src, "blazingmountd.c")
        if '#include "bm_api.c"' not in open(main_c).read():
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "api_hooks.py"), main_c], check=True)
        out = os.path.join(HERE, "..", "console", "blazingmountd", "out")
        blob = os.path.join(out, "fork_blob.h")
        inc = out if os.path.exists(blob) else work
        if not os.path.exists(blob):
            open(os.path.join(work, "fork_blob.h"), "w").write('#define BM_FORK_SHA "0000"\nstatic const unsigned long bm_fork_len = 8;\n'
                                                               'static const unsigned char bm_fork[] = "blz_link";\n')
        exe = os.path.join(work, "bmd")
        r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="api-test"', "-DBM_API_PORT=%d" % aport,
                            "-DAPI_WATCH_S=2"] + os.environ.get("BM_TEST_DEFS", "").split() +   # e.g. BM_TEST_DEFS=-DBM_API_STACK=65536
                           ["-I", inc, "-o", exe, main_c], capture_output=True, text=True)
        check(r.returncode == 0 and "warning" not in r.stderr, "build with the API hooks, no warnings" + (": " + r.stderr[:400] if r.stderr else ""))
        if r.returncode:
            return 1
        # 2) the fake console: Alpha's full copy is NOT here; a foreign full copy of Beta; an image file of another title
        root = os.path.join(work, "console"); b = os.path.join(root, "data", "blazing"); hb = os.path.join(root, "data", "homebrew")
        os.makedirs(b); os.makedirs(os.path.join(hb, "Some Full Copy (PPSA90002)", "sce_sys"))
        open(os.path.join(hb, "Old Thing (PPSA80000).ffpfsc"), "wb").write(b"x" * 100)
        open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n" % hport)
        open(os.path.join(b, "choices.tsv"), "w").write("auto\t0\n")
        proc = subprocess.Popen([exe], env=dict(os.environ, BM_ROOT=root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        api = "http://127.0.0.1:%d" % aport

        def get(path):
            with urllib.request.urlopen(api + path, timeout=10) as r:
                return r.read()
        def j(path): return json.loads(get(path))
        def act(q, header=True):
            req = urllib.request.Request(api + "/api/action?" + q, method="POST", headers={"X-BM-Request": "1"} if header else {})
            try:
                with urllib.request.urlopen(req, timeout=10) as r: return r.status, json.loads(r.read())
            except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
        def row(tid): return next((g for g in j("/api/library")["games"] if g["tid"] == tid), None)
        def wait(cond, secs=30):
            t = time.time()
            while time.time() - t < secs:
                try:
                    if cond(): return True
                except Exception: pass
                time.sleep(0.3)
            return False

        check(wait(lambda: j("/api/status")["last_pass"] > 0), "API answers and a first pass ran")
        st = j("/api/status")
        check(st["version"] == "api-test" and st["route"] == "LAN" and "data" in st["free_mb"], "/api/status: version, route, free space")
        lib = j("/api/library")
        rows = {g["tid"]: g for g in lib["games"]}
        check(lib["host_ok"] and set(rows) >= {"PPSA90001", "PPSA90002", "PPSA90003", "PPSA80000"}, "/api/library: host games + the console's own")
        check(rows["PPSA90001"]["state"] == "available" and rows["PPSA90001"]["local"] == "none", "auto 0: Alpha is 'available', not built")
        check(rows["PPSA90001"]["proof"].startswith("DIGI5 2026-10-24 10:00"), "proof line from the host's /proven.tsv")
        check(rows["PPSA90001"]["format"] == "ffpfsc" and rows["PPSA90001"]["compressed"] is True, "file type + compressed")
        check(rows["PPSA90002"]["local"] == "full" and rows["PPSA90002"]["state"] == "installed", "a full copy is 'installed', kind 'full'")
        check(rows["PPSA90003"]["state"] == "needs_fw", "needs newer FW than the console")
        check(rows["PPSA80000"]["local"] == "image" and rows["PPSA80000"]["on_drives"] is False, "an image file on the console only")
        s = j("/api/settings")
        check(any(k["key"] == "conns" for k in s["keys"]) and s["auto"] == 0, "/api/settings lists the keys + auto")
        check(get("/api/icon/PPSA90001").startswith(b"\x89PNG"), "/api/icon from the host (cached after)")
        check(os.path.exists(os.path.join(b, "icons", "PPSA90001.png")), "icon cached on the console")
        check(any("pass:" in l for l in j("/api/log?lines=50")["lines"]), "/api/log tail")
        check(wait(lambda: j("/api/status")["catalog_etag"] == "cat0001"), "status carries the host's whole-library ETag")
        cat = j("/api/catalog")
        check(cat["count"] == 2 and cat["games"][1]["system"] == "ps2", "/api/catalog passes the host's library.json through")
        check(j("/api/covers.json")["covers"]["aaaaaaaaaaaa"]["page"] == 0, "/api/covers.json passed through")
        check(get("/api/cover/thumb/aaaaaaaaaaaa.jpg").startswith(b"\xff\xd8"), "a cover JPEG passed through")
        def code(path):
            try: get(path); return 200
            except urllib.error.HTTPError as e: return e.code
        check(code("/api/cover/thumb/../../etc.jpg") == 404 and code("/api/cover/evil/aaaaaaaaaaaa.jpg") == 404, "a bad cover name is refused")
        # 7) DISCOVER: pass-through, the wish press, release alerts
        check(j("/api/discover")["alerts"][0]["kind"] == "out" and j("/api/wishlist")["items"][0]["id"] == "aaaaaaaaaaaa", "/api/discover + /api/wishlist passed through")
        check(get("/api/cover/dsheet/0.jpg").startswith(b"\xff\xd8") and code("/api/cover/dsheet/x0.jpg") == 404, "a dsheet cover passed through; a bad name refused")
        st, r = act("do=wish&id=aaaaaaaaaaaa&value=1")
        check(st == 200 and r["ok"] and WISH_POSTS[-1] == ("/wishlist?id=aaaaaaaaaaaa&on=1", "1"), "wish press forwarded at once to POST /wishlist with X-BM-Request")
        check(act("do=wish&id=aaaaaaaaaaaa&value=1", header=False)[0] == 403 and act("do=wish&id=../../x&value=1")[0] == 400
              and act("do=wish&id=aaaaaaaaaaaa&value=7")[0] == 400, "wish: no header / bad id / bad value refused, nothing forwarded")
        st, r = act("do=wish&id=cccccccccccc&value=1")
        check(st == 502 and "not a Discover game" in r["error"], "the host's refusal is reported to the app")
        logp = os.path.join(b, "blazingmountd.log")
        notices = lambda: [l for l in open(logp).read().splitlines() if "notice: " in l]
        check(wait(lambda: len(notices()) == 2), "the host's 2 alerts raised 2 notices at a pass")
        check(any("notice: Out now: Wished \u00e9 Game (on your wishlist)" in l for l in notices())
              and any("notice: New date for Later Big: 17 Dec 2026 (wishlist)" in l for l in notices()), "notice texts (out / moved), the title decoded")
        check(wait(lambda: j("/api/status")["discover_etag"] == "disc0001"), "status carries the Discover ETag")
        seenp = os.path.join(b, "alerts-seen.tsv")
        check(open(seenp).read().split() == ["aaaaaaaaaaaa:out:1761200000", "bbbbbbbbbbbb:moved:1766000000"], "both keys remembered on the console")
        act("do=sync"); time.sleep(2.5)
        check(len(notices()) == 2, "next pass (304, unchanged): no notice again")
        DISC["etag"] = "disc0002"; act("do=sync")
        check(wait(lambda: j("/api/status")["discover_etag"] == "disc0002") and len(notices()) == 2, "new ETag, same alerts: still no repeat")
        kept = open(seenp).read()                                 # (read first: open(..., "w") would empty it before the read)
        open(seenp, "w").write("".join("old:%d\n" % i for i in range(250)) + kept)
        DISC["alerts"].append({"id": "dddddddddddd", "title": "Third", "kind": "out", "date": 1761300000, "key": "dddddddddddd:out:1761300000", "date_text": "24 Oct 2026"})
        DISC["etag"] = "disc0003"; act("do=sync")
        check(wait(lambda: len(notices()) == 3), "a new alert raises exactly one more notice" + ("" if len(notices()) == 3 else ": %r" % notices()))
        lines = open(seenp).read().splitlines()
        check(len(lines) == 200 and lines[-1] == "dddddddddddd:out:1761300000" and "aaaaaaaaaaaa:out:1761200000" in lines, "alerts-seen.tsv capped at 200 lines, newest kept")
        # 2) refusals
        check(act("do=install&tid=PPSA90001", header=False)[0] == 403, "POST without X-BM-Request is refused")
        check(act("do=explode&tid=PPSA90001")[0] == 400, "unknown action refused")
        check(act("do=set&key=host_evil&value=1")[0] == 400, "unknown setting refused")
        check(act("do=set&key=conns&value=4%0Aonly%20X")[0] == 400, "a newline in a value is refused")
        check(act("do=install&tid=../etc")[0] == 400, "a bad title id is refused")
        # 3) install / remove
        check(act("do=install&tid=PPSA90001")[0] == 200, "install queued")
        check(wait(lambda: row("PPSA90001")["state"] == "installed"), "install: the main loop built Alpha")
        sdir = os.path.join(hb, "Alpha Game (PPSA90001)")
        check(os.stat(os.path.join(sdir, "eboot.bin")).st_mode & 0o111 != 0, "eboot.bin is executable")
        check(row("PPSA90001")["local"] == "starter" and row("PPSA90001")["choice"] == "want", "kind starter, choice want")
        # 4) guards
        sand = os.path.join(root, "mnt", "sandbox", "PPSA90001_000"); os.makedirs(sand)
        check(wait(lambda: row("PPSA90001")["state"] == "playing"), "a running game shows 'playing'")
        act("do=remove&tid=PPSA90001")
        check(wait(lambda: any("is running" in x for x in j("/api/status")["recent"])) and os.path.isdir(sdir), "remove refused while it runs")
        os.rmdir(sand)
        act("do=remove&tid=PPSA90001")
        check(wait(lambda: not os.path.exists(sdir)), "remove: starter folder deleted")
        check(not os.path.exists(os.path.join(b, "link", "PPSA90001.cfg")) and row("PPSA90001")["choice"] == "skip", "remove: cfg gone, choice skip")
        time.sleep(1)
        check(not os.path.exists(sdir), "a removed game is not rebuilt by the next pass")
        act("do=remove&tid=PPSA90002")
        check(wait(lambda: any("full copy" in x for x in j("/api/status")["recent"])) and os.path.isdir(os.path.join(hb, "Some Full Copy (PPSA90002)")),
              "remove never deletes a full copy")
        # 5) choices + settings
        act("do=hide&tid=PPSA90003"); act("do=set&key=conns&value=6")
        check(wait(lambda: row("PPSA90003")["choice"] == "skip"), "hide -> skip")
        check(wait(lambda: "conns 6" in open(os.path.join(b, "mount.conf")).read()), "set conns -> mount.conf")
        check("host 127.0.0.1" in open(os.path.join(b, "mount.conf")).read(), "other mount.conf lines kept")
        act("do=unhide&tid=PPSA90003")
        check(wait(lambda: row("PPSA90003")["choice"] == ""), "unhide -> back to the normal rule (no choice)")
        # 6) a game added on the drives: seen by the watch, built by itself with auto on
        act("do=auto&value=1")
        check(wait(lambda: "auto\t1" in open(os.path.join(b, "choices.tsv")).read()), "auto 1 saved")
        gen = j("/api/status")["gen"]
        GAMES.append(LATE)
        check(wait(lambda: row("PPSA90004") is not None, 20), "a game added on the drives appears by itself")
        check(wait(lambda: row("PPSA90004")["state"] == "installed", 30), "... and is built with no request (auto on)")
        check(j("/api/status")["gen"] > gen, "gen moved (the app knows to refresh)")
        check(any("game list changed" in l for l in j("/api/log?lines=200")["lines"]), "the watch logged the change")
    finally:
        if proc:
            proc.terminate(); proc.wait(5)
        srv.shutdown()
        shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAIL" % fails)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
