#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_pull.py - fake-console test of blazingmountd's console-side pull (bm_pull.c, v2.9: POST/GET /api/pull).
    python3 -I host/test_pull.py            (no console: a fake host with Range in-process, the payload's own C code as a HOST=1 build)
Checks:
  1. a 50 MiB file arrives on 4 Range connections byte-exact (sha256), its piece record all '1', state done, rate + pieces reported
  2. connections cut in the middle of a piece are fetched again from the piece's start and the file is still exact
  3. cancel mid-way -> state cancelled with the done pieces kept; the same POST again RESUMES (only the missing pieces are served)
  4. a server whose Content-Range total is not the size asked for is never written: state failed, the error says what came back
  5. refused: no X-BM-Request header, dest outside <drive>/blz-stage/incoming/, a src that is not /raw/ /d/ /t/, a port that is
     not imgserve's or dirserve's, a second job while one runs (409)
"""
import hashlib, http.server, json, os, random, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "console", "blazingmountd", "src")
OUT = os.path.join(HERE, "..", "console", "blazingmountd", "out")
MIB = 1 << 20
FILES = {}                        # served name -> bytes
STATE = {"cut": 0, "slow": 0.0, "lie_total": False, "served": 0}
LOCK = threading.Lock()
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


class Fake(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/games.txt":
            return self._send(200, b"")
        if not self.path.startswith("/raw/") or self.path[5:] not in FILES:
            return self._send(404, b"")
        data = FILES[self.path[5:]]
        a, b = self.headers.get("Range", "bytes=0-").split("=")[1].split("-")
        a, b = int(a), min(int(b or len(data) - 1), len(data) - 1)
        total = len(data) + (1 if STATE["lie_total"] else 0)
        body = data[a:b + 1]
        self.send_response(206)
        self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, total))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        with LOCK:
            cut = STATE["cut"] > 0 and random.random() < 0.5
            if cut:
                STATE["cut"] -= 1
        if cut:                                   # a connection that dies half way through the piece
            self.wfile.write(body[:len(body) // 2])
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        for i in range(0, len(body), 256 * 1024):
            if STATE["slow"]:
                time.sleep(STATE["slow"])
            self.wfile.write(body[i:i + 256 * 1024])
            with LOCK:
                STATE["served"] += len(body[i:i + 256 * 1024])

    def _send(self, code, body):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    tmp = tempfile.mkdtemp(prefix="bm-pull-")
    hport, aport = free_port(), free_port()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", hport), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    exe = os.path.join(tmp, "blazingmountd-host")
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-std=gnu11", '-DBM_VERSION="pull-test"', "-DBM_API_PORT=%d" % aport,
                        "-I" + OUT, "-o", exe, os.path.join(SRC, "blazingmountd.c")], capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr); sys.exit("build failed or warned")
    root = os.path.join(tmp, "console")
    b = os.path.join(root, "data", "blazing"); os.makedirs(b)
    os.makedirs(os.path.join(root, "mnt", "ext1", "blz-stage", "incoming"))
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport %d\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n" % hport)
    proc = subprocess.Popen([exe], env=dict(os.environ, BM_ROOT=root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    api = "http://127.0.0.1:%d" % aport

    def get():
        return json.loads(urllib.request.urlopen(api + "/api/pull", timeout=10).read())

    def post(hdr=True, **q):
        req = urllib.request.Request(api + "/api/pull?" + urllib.parse.urlencode(q), method="POST",
                                     headers={"X-BM-Request": "1"} if hdr else {})
        try:
            return 200, json.loads(urllib.request.urlopen(req, timeout=10).read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def wait(pred, secs=60):
        t = time.time()
        while time.time() - t < secs:
            try:
                s = get()
                if pred(s):
                    return s
            except OSError:
                pass
            time.sleep(0.2)
        return get()

    def disk(dest):
        return open(root + dest, "rb").read()

    try:
        wait(lambda s: True, 10)
        random.seed(7)
        data = random.randbytes(50 * MIB + 12345)
        FILES["game.ffpfsc"] = data
        dest = "/mnt/ext1/blz-stage/incoming/game.ffpfsc.part"

        # 1. clean pull
        code, j = post(src="/raw/game.ffpfsc", port=hport, dest=dest, size=len(data), conns=4)
        check(code == 200 and j.get("pieces") == 7, "start: 200, 7 pieces of 8 MiB (%s %s)" % (code, j))
        s = wait(lambda s: s["state"] != "pulling")
        check(s["state"] == "done" and s["done"] == len(data) and s["pieces_done"] == 7, "1. done, every byte (%s)" % s["state"])
        check(hashlib.sha256(disk(dest)).digest() == hashlib.sha256(data).digest(), "1. the file is byte-exact (sha256)")
        check(open(root + dest + ".blzpull", "rb").read() == b"1" * 7, "1. piece record all '1'")

        # 2. cut connections
        os.remove(root + dest); os.remove(root + dest + ".blzpull")
        STATE["cut"] = 5
        post(src="/raw/game.ffpfsc", port=hport, dest=dest, size=len(data), conns=4)
        s = wait(lambda s: s["state"] != "pulling")
        check(s["state"] == "done" and STATE["cut"] < 5, "2. done although %d connections were cut" % (5 - STATE["cut"]))
        check(disk(dest) == data, "2. the file is still byte-exact")

        # 3. cancel + resume
        os.remove(root + dest); os.remove(root + dest + ".blzpull")
        STATE["cut"], STATE["slow"] = 0, 0.02
        post(src="/raw/game.ffpfsc", port=hport, dest=dest, size=len(data), conns=2)
        wait(lambda s: s["pieces_done"] >= 2)
        code, j = post(src="/raw/game.ffpfsc", port=hport, dest=dest, size=len(data), conns=2)
        check(code == 409, "5. a second POST while it runs: 409 (%s)" % code)
        code, j = post(cancel=1)
        s = wait(lambda s: s["state"] != "pulling")
        kept = s["pieces_done"]
        check(s["state"] == "cancelled" and 2 <= kept < 7, "3. cancelled with %d pieces kept" % kept)
        STATE["slow"], STATE["served"] = 0, 0
        code, j = post(src="/raw/game.ffpfsc", port=hport, dest=dest, size=len(data), conns=4)
        check(code == 200 and j.get("pieces_done") == kept, "3. resume starts with the %d kept pieces (%s)" % (kept, j))
        s = wait(lambda s: s["state"] != "pulling")
        check(s["state"] == "done" and disk(dest) == data, "3. resumed to done, byte-exact")
        check(STATE["served"] <= len(data) - kept * 8 * MIB + 8 * MIB, "3. only the missing pieces were served (%d MiB)" % (STATE["served"] // MIB))

        # 4. wrong total
        FILES["other.ffpfsc"] = data[:9 * MIB]
        STATE["lie_total"] = True
        d2 = "/mnt/ext1/blz-stage/incoming/other.part"
        post(src="/raw/other.ffpfsc", port=hport, dest=d2, size=9 * MIB, conns=2)
        s = wait(lambda s: s["state"] != "pulling")
        STATE["lie_total"] = False
        check(s["state"] == "failed" and "wanted" in s["err"], "4. wrong total refused: %s" % s["err"])
        check(not os.path.exists(root + d2) or os.path.getsize(root + d2) == 0, "4. nothing of it was written")

        # 5. refusals
        check(post(hdr=False, src="/raw/game.ffpfsc", port=hport, dest=dest, size=1)[0] == 403, "5. no X-BM-Request: 403")
        check(post(src="/raw/game.ffpfsc", port=hport, dest="/data/blazing/mount.conf", size=1)[0] == 403, "5. dest outside incoming/: 403")
        check(post(src="/raw/game.ffpfsc", port=hport, dest="/mnt/ext1/blz-stage/incoming/../../../data/x", size=1)[0] == 403, "5. dest with ..: 403")
        check(post(src="/etc/passwd", port=hport, dest=dest, size=1)[0] == 400, "5. src not /raw/ /d/ /t/: 400")
        check(post(src="/raw/game.ffpfsc", port=22, dest=dest, size=1)[0] == 400, "5. a port that is not imgserve/dirserve: 400")
        log = open(os.path.join(b, "blazingmountd.log")).read()
        check("pull: done /raw/game.ffpfsc" in log and "pull: cancelled" in log, "the log records each job")
    finally:
        proc.terminate(); proc.wait(5)
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    import urllib.parse
    main()
