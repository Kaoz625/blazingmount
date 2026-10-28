#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_netsrc.py - fake-console test of blazingmountd v2.12's network sources (bm_src.c): FTP, WebDAV (and SMB when asked).
    python3 -I host/test_netsrc.py              hermetic: our own small FTP + WebDAV servers on 127.0.0.1 (+ rclone's FTP/WebDAV when rclone is installed)
    BM_TEST_SMB=<ip>[:<port>]:<share>[/dir] [BM_TEST_SMB_USER=u BM_TEST_SMB_PASS=p] python3 -I host/test_netsrc.py
                                               also reads a REAL SMB share: lists it, "installs" its first small .pkg into the fake installer and compares
                                               what that read with the gateway's bytes (the login comes from the environment, never argv, never printed)
Checks (per kind):
  1. add a source (login optional): saved 0600, GET /api/src never shows the password, the log never has it, bad input is refused, X-BM-Request needed
  2. browse: folders + .pkg + images, split X.pkg.N sets fold into ONE entry (size = the sum, parts = N); a wrong login says so
  3. install a single pkg and a split pkg: the "installer" (fake, dumps what it read) got EXACTLY the file's bytes; live progress; done
  4. the gateway GET /src/<name>/<path> answers Range (a range that straddles two split parts is byte-exact)
  5. cancel mid-way: cancelled, the proxy stops, the source session is released
"""
import hashlib, http.server, json, os, shutil, socket, socketserver, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request
import xml.sax.saxutils as sx

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_install as ti  # noqa: E402  (build(), free_port(): the same fake-console harness)

fails = 0
SECRET = "S3cr3t-pw-%d" % os.getpid()
USER = "bmtest"


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


# ---------------------------------------------------------------- a small FTP server (passive, REST, RETR, SIZE, MLSD or LIST)
class FTP(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def ftp_server(root, login=None, mlsd=True):
    class H(socketserver.StreamRequestHandler):
        def send(self, s):
            self.wfile.write((s + "\r\n").encode()); self.wfile.flush()

        def handle(self):
            self.send("220 test ftp"); user = None; ok = login is None; rest = 0; pasv = None; cwd = ""
            while True:
                line = self.rfile.readline().decode("utf-8", "replace").strip()
                if not line:
                    return
                cmd, _, arg = line.partition(" ")
                cmd = cmd.upper()
                if cmd == "USER":
                    user = arg; self.send("331 password please" if login else "331 any")
                elif cmd == "PASS":
                    ok = login is None or (user == login[0] and arg == login[1])
                    self.send("230 in" if ok else "530 login incorrect")
                elif not ok:
                    self.send("530 login first")
                elif cmd == "TYPE":
                    self.send("200 binary")
                elif cmd == "PASV":
                    if pasv:
                        pasv.close()
                    pasv = socket.socket(); pasv.bind(("127.0.0.1", 0)); pasv.listen(1); p = pasv.getsockname()[1]
                    self.send("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (p >> 8, p & 255))
                elif cmd == "REST":
                    rest = int(arg); self.send("350 ok")
                elif cmd == "SIZE":
                    f = os.path.join(root, arg.lstrip("/"))
                    self.send("213 %d" % os.path.getsize(f) if os.path.isfile(f) else "550 no such file")
                elif cmd == "RETR":
                    f = os.path.join(root, arg.lstrip("/"))
                    if not os.path.isfile(f) or not pasv:
                        self.send("550 no such file"); continue
                    self.send("150 opening"); d, _ = pasv.accept(); pasv.close(); pasv = None
                    complete = True
                    try:
                        with open(f, "rb") as fh:
                            fh.seek(rest)
                            while True:
                                b = fh.read(65536)
                                if not b:
                                    break
                                d.sendall(b)
                    except OSError:
                        complete = False
                    d.close(); rest = 0
                    self.send("226 done" if complete else "426 closed")
                elif cmd in ("MLSD", "LIST"):
                    d_ = os.path.join(root, arg.lstrip("/"))
                    if cmd == "MLSD" and not mlsd:
                        self.send("500 unknown command"); continue
                    if not os.path.isdir(d_) or not pasv:
                        self.send("550 no such dir"); continue
                    self.send("150 listing"); d, _ = pasv.accept(); pasv.close(); pasv = None
                    out = []
                    for n in sorted(os.listdir(d_)):
                        full = os.path.join(d_, n); isd = os.path.isdir(full); sz = 0 if isd else os.path.getsize(full)
                        out.append(("type=%s;size=%d; %s" % ("dir" if isd else "file", sz, n)) if cmd == "MLSD"
                                   else "%s 1 u g %d Oct 28 10:00 %s" % ("drwxr-xr-x" if isd else "-rw-r--r--", sz, n))
                    if cmd == "MLSD":
                        out.insert(0, "type=cdir;size=0; .")
                    d.sendall(("\r\n".join(out) + "\r\n").encode()); d.close(); self.send("226 done")
                elif cmd == "QUIT":
                    self.send("221 bye"); return
                else:
                    self.send("502 not implemented")
    s = FTP(("127.0.0.1", 0), H); threading.Thread(target=s.serve_forever, daemon=True).start()
    return s, s.server_address[1]


# ---------------------------------------------------------------- a small WebDAV server (Basic login, Range GET, PROPFIND Depth 1)
def dav_server(root, login=None, prefix="/dav"):
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def authed(self):
            if login is None:
                return True
            import base64
            return self.headers.get("Authorization", "") == "Basic " + base64.b64encode(("%s:%s" % login).encode()).decode()

        def fs(self):
            p = urllib.parse.unquote(self.path.split("?")[0])
            return os.path.join(root, p[len(prefix):].lstrip("/")), p

        def deny(self):
            self.send_response(401); self.send_header("WWW-Authenticate", 'Basic realm="t"'); self.send_header("Content-Length", "0"); self.end_headers()

        def do_PROPFIND(self):
            self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if not self.authed():
                return self.deny()
            f, p = self.fs()
            if not os.path.exists(f):
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
            items = [(p.rstrip("/") + ("/" if os.path.isdir(f) else ""), f)]
            if os.path.isdir(f):
                for n in sorted(os.listdir(f)):
                    items.append((p.rstrip("/") + "/" + n + ("/" if os.path.isdir(os.path.join(f, n)) else ""), os.path.join(f, n)))
            body = '<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">'
            for href, full in items:
                isd = os.path.isdir(full)
                body += "<D:response><D:href>%s</D:href><D:propstat><D:prop><D:resourcetype>%s</D:resourcetype>%s</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>" % (
                    sx.escape(urllib.parse.quote(href)), "<D:collection/>" if isd else "", "" if isd else "<D:getcontentlength>%d</D:getcontentlength>" % os.path.getsize(full))
            body += "</D:multistatus>"; b = body.encode()
            self.send_response(207); self.send_header("Content-Type", "application/xml"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

        def do_GET(self):
            if not self.authed():
                return self.deny()
            f, _ = self.fs()
            if not os.path.isfile(f):
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
            size = os.path.getsize(f); a, b = 0, size - 1; rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                s_, e_ = rng[6:].split("-"); a = int(s_); b = min(int(e_), size - 1) if e_ else size - 1
                self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, size))
            else:
                self.send_response(200)
            self.send_header("Content-Length", str(b - a + 1)); self.end_headers()
            try:
                with open(f, "rb") as fh:
                    fh.seek(a); left = b - a + 1
                    while left > 0:
                        c = fh.read(min(65536, left))
                        if not c:
                            break
                        self.wfile.write(c); left -= len(c)
            except OSError:
                pass
    s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H); threading.Thread(target=s.serve_forever, daemon=True).start()
    return s, s.server_address[1]


# ---------------------------------------------------------------- the payload under test
def start(tmp, dump, delay=0):
    aport, iport, ipc = ti.free_port(), ti.free_port(), ti.free_port()
    inc = os.path.join(HERE, "..", "console", "blazingmountd", "out", "libsmb2-host")
    smb = os.path.exists(os.path.join(inc, "lib", "libsmb2.a"))
    exe, hexe = ti.build(tmp, aport, iport, 0, ipc, smb=inc if smb else None)
    root = os.path.join(tmp, "console-%d" % aport); b = os.path.join(root, "data", "blazing"); os.makedirs(b)
    open(os.path.join(b, "mount.conf"), "w").write("host 127.0.0.1\nport 1\nmin_free_mb 0\ntsnet 0\nfw 10.20\npoll_secs 3600\n")
    env = dict(os.environ, BM_ROOT=root, BM_FAKE_AI="on", BM_FAKE_AI_DELAY_MS=str(delay), BM_INST_HELPER=hexe, BM_FAKE_AI_DUMP=dump)
    proc = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    api = "http://127.0.0.1:%d" % aport
    for _ in range(50):
        try:
            urllib.request.urlopen(api + "/api/status", timeout=1); break
        except OSError:
            time.sleep(0.2)
    return proc, api, os.path.join(b, "blazingmountd.log"), os.path.join(b, "sources.conf"), smb


def call(api, method, path, hdr=True, raw=False):
    req = urllib.request.Request(api + path, method=method, headers={"X-BM-Request": "1"} if hdr else {})
    try:
        r = urllib.request.urlopen(req, timeout=30); d = r.read(); return r.status, (d if raw else json.loads(d))
    except urllib.error.HTTPError as e:
        d = e.read(); return e.code, (d if raw else json.loads(d or b"{}"))


def q(**kw):
    return urllib.parse.urlencode(kw)


def wait(api, t=40):
    end = time.time() + t
    while time.time() < end:
        j = call(api, "GET", "/api/install")[1]
        if j.get("state") in ("done", "failed", "cancelled"):
            return j
        time.sleep(0.2)
    return call(api, "GET", "/api/install")[1]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def exercise(kind, api, dump, name, host, port, base, user, pw, data, log, conf, label=None):
    label = label or kind
    code, j = call(api, "POST", "/api/src/add?" + q(name=name, kind=kind, host=host, port=port, path=base, user=user, **{"pass": pw}))
    check(code == 200 and j.get("ok"), "%s 1. add source %s (login %s)" % (label, name, "yes" if user else "no"))
    code, j = call(api, "GET", "/api/src")
    me = [s for s in j.get("sources", []) if s["name"] == name]
    txt = json.dumps(j)
    check(me and me[0]["has_pass"] == bool(pw) and (not pw or pw not in txt), "%s 1. GET /api/src lists it, never the password (has_pass=%s)" % (label, me and me[0]["has_pass"]))
    check(oct(os.stat(conf).st_mode & 0o777) == "0o600", "%s 1. sources.conf is mode 0600 (%s)" % (label, oct(os.stat(conf).st_mode & 0o777)))
    # browse
    code, j = call(api, "GET", "/api/src/ls?" + q(name=name, path=""))
    ents = {e["name"]: e for e in j.get("entries", [])}
    check(code == 200 and j.get("ok") and "games" in ents and ents["games"]["dir"], "%s 2. browse the base: folder 'games' listed (%s)" % (label, sorted(ents)))
    code, j = call(api, "GET", "/api/src/ls?" + q(name=name, path="games"))
    ents = {e["name"]: e for e in j.get("entries", [])}
    check("Single App.pkg" in ents and ents["Single App.pkg"]["size"] == len(data["single"]) and ents["Single App.pkg"]["pkg"], "%s 2. single pkg with its size" % label)
    sp = ents.get("Split Game.pkg", {})
    check(sp.get("parts") == 3 and sp.get("size") == len(data["split"]) and not any(n.startswith("Split Game.pkg.") for n in ents),
          "%s 2. split set folds into ONE entry (parts %s, size %s of %d)" % (label, sp.get("parts"), sp.get("size"), len(data["split"])))
    check("Disc Image.ffpfsc" in ents and ents["Disc Image.ffpfsc"]["image"] and "notes.txt" not in ents, "%s 2. a game image is listed, a .txt is hidden" % label)
    check("notes.txt" in {e["name"] for e in call(api, "GET", "/api/src/ls?" + q(name=name, path="games", all=1))[1]["entries"]}, "%s 2. all=1 shows every file" % label)
    # install single
    code, j = call(api, "POST", "/api/install?" + q(src=name, path="games/Single App.pkg"))
    check(code == 200 and j.get("ok"), "%s 3. install single pkg accepted" % label)
    j = wait(api)
    check(j.get("state") == "done" and j.get("via") == "sony" and j.get("served") == len(data["single"]) and j.get("source") == name,
          "%s 3. installed: %s, %s of %d bytes served, source %s %s" % (label, j.get("state"), j.get("served"), len(data["single"]), j.get("source"), j.get("error", "")))
    check(open(dump, "rb").read() == data["single"], "%s 3. the installer read EXACTLY the file's bytes (sha %s)" % (label, sha(open(dump, "rb").read())[:12]))
    # install split (named by part 0 and by the folded name)
    for pth in ("games/Split Game.pkg.0", "games/Split Game.pkg"):
        code, j = call(api, "POST", "/api/install?" + q(src=name, path=pth))
        j = wait(api)
        check(code == 200 and j.get("state") == "done" and j.get("total") == len(data["split"]) and j.get("served") == len(data["split"]),
              "%s 3. split set via '%s': %s, %s bytes in 3 parts" % (label, os.path.basename(pth), j.get("state"), j.get("served")))
        check(open(dump, "rb").read() == data["split"], "%s 3. split: the installer read the 3 parts as ONE file, byte-exact" % label)
    # gateway ranges
    st, body = call(api, "GET", "/src/%s/games/Single%%20App.pkg" % name, raw=True)
    check(st == 200 and body == data["single"], "%s 4. gateway full read = the file" % label)
    cut = len(data["split"]) // 3
    for a, b in ((0, 99), (cut - 20, cut + 40), (cut * 2 - 5, len(data["split"]) - 1), (7, 7)):
        req = urllib.request.Request(api + "/src/%s/games/Split%%20Game.pkg.1" % name, headers={"Range": "bytes=%d-%d" % (a, b)})
        r = urllib.request.urlopen(req, timeout=30); body = r.read()
        check(r.status == 206 and body == data["split"][a:b + 1], "%s 4. gateway Range %d-%d over the split set (%d bytes) exact" % (label, a, b, len(body)))
    check(call(api, "GET", "/src/%s/games/nope.pkg" % name)[0] in (404, 502), "%s 4. gateway: missing file is an error" % label)
    # refused
    check(call(api, "POST", "/api/install?" + q(src=name, path="../x.pkg"))[0] == 400, "%s 3. install refuses a path with .." % label)
    check(call(api, "POST", "/api/install?" + q(src="nope", path="a.pkg"))[0] == 404, "%s 3. install: unknown source 404" % label)
    check(call(api, "POST", "/api/install?" + q(src=name, path="games/notes.txt"))[0] == 400, "%s 3. install refuses a non-pkg" % label)
    code, j = call(api, "POST", "/api/install?" + q(src=name, path="games/Missing.pkg"))
    j = wait(api)
    check(j.get("state") == "failed" and "Missing" in j.get("error", ""), "%s 3. a missing pkg fails with a reason: %s" % (label, j.get("error")))
    # no secret in the log
    logtxt = open(log).read()
    check(not pw or pw not in logtxt, "%s 1. the log never has the password (%d bytes)" % (label, len(logtxt)))
    check("source %s saved" % name in logtxt and "source %s (%s)" % (name, kind) in logtxt, "%s the log records the source and the install (no login)" % label)


def make_data(tmp):
    root = os.path.join(tmp, "share"); g = os.path.join(root, "games"); os.makedirs(g)
    single = os.urandom(3 * 1024 * 1024 + 4321)
    split = os.urandom(2 * 1024 * 1024 + 777) + os.urandom(1 * 1024 * 1024 + 1) + os.urandom(900 * 1024)
    cuts = [2 * 1024 * 1024 + 777, 3 * 1024 * 1024 + 778]
    parts = [split[:cuts[0]], split[cuts[0]:cuts[1]], split[cuts[1]:]]
    open(os.path.join(g, "Single App.pkg"), "wb").write(single)
    for i, p in enumerate(parts):
        open(os.path.join(g, "Split Game.pkg.%d" % i), "wb").write(p)
    open(os.path.join(g, "Disc Image.ffpfsc"), "wb").write(b"x" * 100)
    open(os.path.join(g, "notes.txt"), "w").write("hi")
    return root, {"single": single, "split": split}


def main():
    tmp = tempfile.mkdtemp(prefix="bm-src-")
    root, data = make_data(tmp)
    dump = os.path.join(tmp, "dump.bin")
    procs = []
    try:
        proc, api, log, conf, smb = start(tmp, dump); procs.append(proc)
        # validation + auth header
        check(call(api, "POST", "/api/src/add?" + q(name="a", kind="ftp", host="127.0.0.1"), hdr=False)[0] == 403, "0. add without X-BM-Request: 403")
        for bad in (dict(name="bad name", kind="ftp", host="127.0.0.1"), dict(name="a", kind="nfs", host="127.0.0.1"), dict(name="a", kind="ftp", host="not.an.ip"),
                    dict(name="a", kind="ftp", host="127.0.0.1", port=70000), dict(name="a", kind="ftp", host="127.0.0.1", user="x\ty")):
            check(call(api, "POST", "/api/src/add?" + q(**bad))[0] == 400, "0. refused: %s" % bad)
        check(call(api, "GET", "/api/src")[1].get("sources") == [], "0. nothing saved by the refused ones")
        check(call(api, "GET", "/api/src/ls?name=nope")[0] == 404, "0. ls of an unknown source: 404")

        # FTP with a login (MLSD)
        s, port = ftp_server(root, login=(USER, SECRET))
        exercise("ftp", api, dump, "ftp-login", "127.0.0.1", port, "/", USER, SECRET, data, log, conf, "ftp+login")
        j = call(api, "POST", "/api/src/add?" + q(name="ftp-bad", kind="ftp", host="127.0.0.1", port=port, path="/", user=USER, **{"pass": "wrong"}))
        j = call(api, "GET", "/api/src/test?name=ftp-bad")[1]
        check(j.get("ok") is False and "login" in j.get("error", ""), "ftp 2. a wrong login says so: %s" % j.get("error"))
        j = call(api, "GET", "/api/src/test?name=ftp-login")[1]
        check(j.get("ok") is True, "ftp 2. /api/src/test ok")
        s.shutdown()
        # FTP anonymous, LIST only, base folder
        s, port = ftp_server(root, login=None, mlsd=False)
        exercise("ftp", api, dump, "ftp-anon", "127.0.0.1", port, "/", "", "", data, log, conf, "ftp LIST-only anonymous")
        s.shutdown()

        # WebDAV with login
        s, port = dav_server(root, login=(USER, SECRET), prefix="/dav")
        exercise("webdav", api, dump, "dav-login", "127.0.0.1", port, "/dav", USER, SECRET, data, log, conf, "webdav+login")
        call(api, "POST", "/api/src/add?" + q(name="dav-bad", kind="webdav", host="127.0.0.1", port=port, path="/dav", user=USER, **{"pass": "wrong"}))
        j = call(api, "GET", "/api/src/test?name=dav-bad")[1]
        check(j.get("ok") is False and "401" in j.get("error", ""), "webdav 2. a wrong login says so: %s" % j.get("error"))
        s.shutdown()
        s, port = dav_server(root, login=None, prefix="/")
        exercise("webdav", api, dump, "dav-open", "127.0.0.1", port, "/", "", "", data, log, conf, "webdav open (base /)")
        s.shutdown()

        # the console profile (bm_prof.c): stored on the console, editable over the API
        code, j = call(api, "GET", "/api/profile")
        check(code == 200 and j.get("install_target") == "auto" and j.get("power") == "awake" and j.get("name") == "" and j.get("target_in_effect") is True,
              "profile: defaults (auto, awake, no name): %s" % {k: j.get(k) for k in ("name", "install_target", "default_source", "user")})
        check(call(api, "POST", "/api/profile/set?name=PHAT5", hdr=False)[0] == 403, "profile: set needs X-BM-Request: 403")
        for bad in ("install_target=nvme", "default_source=nope", "name=" + "x" * 40, "user=a%0Ab"):
            check(call(api, "POST", "/api/profile/set?" + bad)[0] == 400, "profile: refused: %s" % bad)
        code, j = call(api, "POST", "/api/profile/set?" + q(name="PHAT5", user="Peace_Maker", default_source="ftp-login", install_target="m2"))
        check(code == 200 and j.get("name") == "PHAT5" and j.get("user") == "Peace_Maker" and j.get("default_source") == "ftp-login" and j.get("install_target") == "m2",
              "profile: saved name/user/default source/target: %s" % j.get("applied"))
        pf = os.path.join(os.path.dirname(conf), "profile.conf")
        check(open(pf).read() == "name PHAT5\ninstall_target m2\ndefault_source ftp-login\nuser Peace_Maker\n", "profile: stored in /data/blazing/profile.conf")
        mc = os.path.join(os.path.dirname(conf), "mount.conf")
        for _ in range(60):
            if "homebrew_dir /mnt/ext1/homebrew" in open(mc).read():
                break
            time.sleep(0.25)
        j = call(api, "GET", "/api/profile")[1]
        check("homebrew_dir /mnt/ext1/homebrew" in open(mc).read() and j.get("target_in_effect") is True, "profile: install_target m2 -> homebrew_dir /mnt/ext1/homebrew (in effect: %s)" % j.get("target_in_effect"))
        j = call(api, "POST", "/api/profile/set?" + q(default_source=""))[1]
        check(j.get("default_source") == "" and j.get("name") == "PHAT5", "profile: clearing one field keeps the others")
        proc_r = start(tmp, dump); procs.append(proc_r[0])
        check(call(proc_r[1], "GET", "/api/profile")[1].get("name") == "", "profile: a fresh console has none (it is per console)")

        # an unreachable host fails fast and does not hang the API
        call(api, "POST", "/api/src/add?" + q(name="dead", kind="ftp", host="127.0.0.1", port=1, path="/"))
        t0 = time.time(); j = call(api, "GET", "/api/src/test?name=dead")[1]
        check(j.get("ok") is False and "connect" in j.get("error", "") and time.time() - t0 < 8, "unreachable: fails in %.1f s: %s" % (time.time() - t0, j.get("error")))
        check(call(api, "GET", "/api/status")[0] == 200, "the API still answers")

        # delete
        check(call(api, "POST", "/api/src/del?name=dead")[0] == 200 and call(api, "POST", "/api/src/del?name=dead")[0] == 404, "0. delete a source (then 404)")
        names = [x["name"] for x in call(api, "GET", "/api/src")[1]["sources"]]
        check("dead" not in names and "ftp-login" in names, "0. the others remain (%s)" % ",".join(names))

        # cancel mid-way (slow installer), FTP
        proc2, api2, log2, conf2, _ = start(tmp, dump, delay=400); procs.append(proc2)
        s, port = ftp_server(root, login=(USER, SECRET))
        call(api2, "POST", "/api/src/add?" + q(name="f", kind="ftp", host="127.0.0.1", port=port, path="/", user=USER, **{"pass": SECRET}))
        call(api2, "POST", "/api/install?" + q(src="f", path="games/Split Game.pkg"))
        time.sleep(1.5)
        j = call(api2, "GET", "/api/install")[1]
        check(j.get("state") == "installing" and 0 < j.get("done", 0) < j.get("total", 0) and j.get("rate", 0) > 0 and j.get("eta", -1) >= 0,
              "5. live progress from FTP: %.1f%%, %s B/s, eta %s s" % (j.get("progress", 0), j.get("rate"), j.get("eta")))
        code, _ = call(api2, "POST", "/api/install/cancel")
        j = wait(api2)
        time.sleep(1.0)
        served = call(api2, "GET", "/api/install")[1].get("served")
        check(code == 200 and j.get("state") == "cancelled" and served < len(data["split"]), "5. cancel -> cancelled, %s of %d bytes served" % (served, len(data["split"])))
        j2 = call(api2, "POST", "/api/install?" + q(src="f", path="games/Single App.pkg"))
        j = wait(api2)
        check(j.get("state") == "done", "5. a new install right after the cancel works (FTP session released): %s" % j.get("state"))
        s.shutdown()

        # rclone's own FTP + WebDAV servers (a real implementation, not ours)
        rc = shutil.which("rclone")
        if rc:
            for kind, extra, base in (("ftp", ["ftp"], "/"), ("webdav", ["webdav"], "/")):
                port = ti.free_port()
                p = subprocess.Popen([rc, "serve"] + extra + ["--addr", "127.0.0.1:%d" % port, "--user", USER, "--pass", SECRET, "--read-only", root],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); procs.append(p)
                for _ in range(40):
                    try:
                        socket.create_connection(("127.0.0.1", port), 0.5).close(); break
                    except OSError:
                        time.sleep(0.25)
                exercise(kind, api, dump, "rclone-" + kind, "127.0.0.1", port, base, USER, SECRET, data, log, conf, "rclone %s" % kind)
                p.terminate()
        else:
            print("skip  rclone not installed: its FTP/WebDAV servers were not tried")

        # SMB (only when asked: it needs a real share)
        spec = os.environ.get("BM_TEST_SMB")
        if spec and smb:
            host, base = spec.split(":", 1); port = 445
            if base.split(":", 1)[0].isdigit() and ":" in base:                 # ip:port:share/dir
                port, base = int(base.split(":", 1)[0]), base.split(":", 1)[1]
            user = os.environ.get("BM_TEST_SMB_USER", ""); pw = os.environ.get("BM_TEST_SMB_PASS", "")
            call(api, "POST", "/api/src/add?" + q(name="smb-live", kind="smb", host=host, port=port, path=base, user=user, **{"pass": pw}))
            j = call(api, "GET", "/api/src/test?name=smb-live")[1]
            check(not pw or pw not in json.dumps(call(api, "GET", "/api/src")[1]), "smb: the password is not in GET /api/src")
            check(j.get("ok") is True, "smb: login + list the share: %s" % (j.get("error") or "%d entries" % len(j.get("entries", []))))
            for e in j.get("entries", []):
                if e["name"].lower().endswith(".pkg") and not e["dir"] and e["size"] < 200 * 1024 * 1024:
                    code, ij = call(api, "POST", "/api/install?" + q(src="smb-live", path=e["name"]))
                    ij = wait(api, t=120)
                    sz = os.path.getsize(dump)
                    st, g = call(api, "GET", "/src/smb-live/" + urllib.parse.quote(e["name"]), raw=True)
                    check(ij.get("state") == "done" and g == open(dump, "rb").read() and len(g) == e["size"],
                          "smb: installed %s (%d bytes), the dump = the gateway's bytes (sha %s)" % (e["name"], sz, sha(g)[:12]))
                    # parallel sessions (Sony's installer reads on several connections): libsmb2 on the PS5 gave every one the same client GUID
                    # and the server dropped them all - six at once must each get their bytes, byte-exact
                    res = {}
                    def one(i, e=e, g=g):
                        a0 = (len(g) // 6) * i; b0 = a0 + len(g) // 6 - 1
                        rq = urllib.request.Request(api + "/src/smb-live/" + urllib.parse.quote(e["name"]), headers={"Range": "bytes=%d-%d" % (a0, b0)})
                        try:
                            res[i] = urllib.request.urlopen(rq, timeout=90).read() == g[a0:b0 + 1]
                        except OSError:
                            res[i] = False
                    ths = [threading.Thread(target=one, args=(i,)) for i in range(6)]
                    [x.start() for x in ths]; [x.join() for x in ths]
                    check(all(res.get(i) for i in range(6)), "smb: 6 parallel sessions each read their range byte-exact: %s" % [res.get(i) for i in range(6)])
                    break
        elif spec:
            print("FAIL  BM_TEST_SMB set but the host build has no libsmb2 (sh console/blazingmountd/tools/build-libsmb2.sh host)"); globals()["fails"] += 1
        else:
            print("skip  SMB: set BM_TEST_SMB=<ip>:<share>[/dir][:user:pass] to read a real share")
    finally:
        for p in procs:
            p.terminate()
        shutil.rmtree(tmp, ignore_errors=True)
    print("all passed" if not fails else "FAILED: %d" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
