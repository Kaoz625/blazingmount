#!/usr/bin/env python3
"""download-to-console.py - copy a streamed game onto a PS5's own drive, check every byte, switch it to local.

    download-to-console.py plan    CONSOLE TID...              sizes, what is already there, free space, time; changes nothing
    download-to-console.py get     CONSOLE TID... [--yes]      copy (or reuse a full copy already there), verify, switch; resumes
    download-to-console.py verify  CONSOLE TID...              only the piece check of what was sent (needs payload 2.8)
    download-to-console.py switch  CONSOLE TID                 only the switch (everything sent and verified)
    download-to-console.py cancel  CONSOLE TID                 stop wanting it; deletes ONLY its staged files in incoming/
    download-to-console.py put     CONSOLE FILE... --to DIR    any files (ROMs, ...) the same way: pieces checked, then renamed into DIR
    download-to-console.py serve   CONSOLE...                  the worker the app talks to: its requests -> runs, progress -> the console
    download-to-console.py status                              the journal, human-readable

The contract is docs/DOWNLOAD-TO-CONSOLE.md. Markus, 24 Oct 2026, PHAT5 away on a phone hotspot: "is there a way to download
some games to my internal drive from the blazing mount?" Measured that evening: ~0.86 MB/s into PHAT5 on ONE FTP stream and
0.78 MB/s on four, so this sends one stream and spends its effort on never sending a byte twice and never trusting one unchecked.

WHAT IT COPIES. A console starter streams either a whole image (imgserve :4451, link cfg `base /t/<TID>@<bytes>`) or the big
files of a folder game (dirserve :4452, the cfg's `file <size> <path>` lines are 0-byte placeholders in the starter). An image
goes to <drive>/blz-stage/incoming/<name>.part, the files of a folder game to <drive>/blz-stage/incoming/<TID>-files/<path>.part
(three levels down: ShadowMount never scans there). <drive> is the payload's homebrew drive; never /data on a console with an M.2.

EVERY BYTE CHECKED. mac1 hashes each 1 GiB piece (SHA-256) while it sends it and keeps the hash in the journal. The console hashes
the same range itself (payload 2.8 GET /api/sha256), so nothing is read back over the hotspot. A piece that does not match is sent
again from its start (ftpsrv's REST n + STOR truncates the file at n). With an older payload the pieces wait, unverified, and
nothing is switched until `verify` has passed.

RESUMES. Stopped, cut off, mac1 restarted: the next run goes on from the console's .part size (the journal holds the hashes of the
pieces before it; the current piece is re-hashed from the local file).

YIELDS. While imgserve served a request in the last 45 s (a streamed game is playing) the copy pauses: both share the hotspot.

THE SWITCH (never while that title runs; the console is claimed only for those seconds):
  image : hide in the payload -> overlay back when the game needs a newer FW (the newest <drive>/blz-disabled/<TID>-overlay-*)
          -> park the starter in <drive>/blz-stage/stub/<starter>-off-<stamp> -> the image into <drive>/homebrew/<name>
          -> cfg /data/blazing/link/<TID>.cfg -> .cfg.off-<stamp> -> watch ShadowMount -> TV notice.
  files : each 0-byte placeholder -> <drive>/blz-stage/stub/<TID>-placeholders-<stamp>/<path>, the real file into its place
          -> cfg -> .cfg.off-<stamp> -> TV notice. (The starter folder is the game; no ShadowMount change.)
A rename never crosses drives. Nothing outside <drive>/blz-stage/incoming/ is ever deleted.

Claude (dl-local), 24 Oct 2026, for Markus.
"""
import argparse
import datetime
import fcntl
import ftplib
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PIECE = int(os.environ.get("BM_DL_PIECE") or 1 << 30)    # tests use small pieces
CHUNK = 1 << 20
FTP_PORT, WEB_PORT, API_PORT = (int(os.environ.get("BM_DL_%s_PORT" % k) or d) for k, d in (("FTP", 2121), ("WEB", 8080), ("API", 8094)))
HOME = os.environ.get("BM_DL_HOME") or os.path.expanduser("~/.blazing/download-local")
IMGSERVE = os.environ.get("BM_IMGSERVE", "http://127.0.0.1:4451")
DIRSERVE = os.environ.get("BM_DIRSERVE", "http://127.0.0.1:4452")
ROOTS = (os.environ.get("BM_DL_ROOTS") or "/Volumes/Untitled:/Volumes/My Book").split(":")
IMGLOG = os.environ.get("BM_IMGSERVE_LOG", os.path.expanduser("~/.blazing/imgserve/imgserve.log"))
TOOLS = os.environ.get("BLAZING_PS5_TOOLS", os.path.expanduser("~/NYCT/blazing-ps5/tools"))
HUB = os.environ.get("BM_DL_HUB", "")          # a team chat file to post milestones to ("" = none)
DATA_KEEP_MB = 55 * 1024                       # /data stops writes near 52 GiB free (memory ps5-data-stops-writes-near-52gib)
YIELD_S = 45
SMP_WAIT = int(os.environ.get("BM_DL_SMP_WAIT") or 180)    # seconds to watch ShadowMount after a switch
DRIVES = ("/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1")
VERIFY_MISS = "payload 2.8 needed to verify"
NET_ERRORS = ftplib.all_errors + (socket.timeout, urllib.error.URLError)   # flat: Python 3.14 refuses a tuple in a tuple


# ---------------------------------------------------------------- small things

def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    try:
        os.makedirs(HOME, exist_ok=True)
        with open(os.path.join(HOME, "log"), "a", encoding="utf-8") as fh:
            fh.write(time.strftime("%Y-%m-%d ") + line + "\n")
    except OSError:
        pass


def hub(msg):
    if not HUB:
        return
    try:
        with open(HUB, "a", encoding="utf-8") as fh:
            fh.write("[%s] dl-local(tool): %s\n" % (time.strftime("%Y-%m-%d %H:%M"), msg))
    except OSError:
        pass


def gb(n):
    return "%.2f GB" % (n / 1e9)


def dur(s):
    s = int(max(0, s))
    return "%dh%02dm" % (s // 3600, s % 3600 // 60) if s >= 3600 else "%dm%02ds" % (s // 60, s % 60)


def stamp():
    return time.strftime("%Y%m%d-%H%M")


def http(url, method="GET", headers=None, timeout=30):
    req = urllib.request.Request(url, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def job_file(key):
    return os.path.join(HOME, "jobs", key.replace(":", "-") + ".json")


def load_state():
    """Every job, one file each (jobs/<console>-<TID>.json): two runs on different titles never overwrite each other."""
    st = {"jobs": {}, "own": set()}
    d = os.path.join(HOME, "jobs")
    for fn in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        try:
            with open(os.path.join(d, fn), encoding="utf-8") as fh:
                j = json.load(fh)
            st["jobs"]["%s:%s" % (j["console"], j["tid"])] = j
        except (OSError, ValueError, KeyError):
            pass
    return st


_LOCKS = {}


def lock_job(key):
    """One run per job: an flock on jobs/<console>-<TID>.lock (dropped when the run ends, however it ends)."""
    os.makedirs(os.path.join(HOME, "jobs"), exist_ok=True)
    fh = open(job_file(key)[:-5] + ".lock", "a+")
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fh.close()
        return False
    _LOCKS[key] = fh
    return True


def own_job(st, key, tid, console):
    st["own"].add(key)
    return st["jobs"].setdefault(key, {"tid": tid, "console": console, "files": {}, "created": int(time.time())})


def save_state(st):
    """Writes only the jobs this run works on."""
    os.makedirs(os.path.join(HOME, "jobs"), exist_ok=True)
    now = int(time.time())
    for key in st["own"]:
        st["jobs"][key]["at"] = now
        p = job_file(key)
        with open(p + ".tmp", "w", encoding="utf-8") as fh:
            json.dump(st["jobs"][key], fh, indent=1, sort_keys=True)
        os.replace(p + ".tmp", p)


# ---------------------------------------------------------------- the console

class Console:
    def __init__(self, target):
        sys.path.insert(0, TOOLS)
        self.name, self.fw = target, os.environ.get("BM_DL_CONSOLE_FW", "")
        try:
            import ps5where
        except ImportError:
            ps5where = None
        self.ip = target
        if ps5where:
            n = ps5where.canon(target)
            if n:
                self.name, self.fw = n, self.fw or ps5where.CONSOLES[n].get("fw", "")
            try:
                self.ip = ps5where.host(target)
            except Exception as e:                           # Remote Play discovery does not answer during some games:
                ip = (ps5where.read_cache(n) or {}).get("ip") if n else None      # its last proven or tailnet address,
                for a in [x for x in (ip, n and ps5where.CONSOLES[n].get("tailnet")) if x]:   # when websrv answers there
                    try:
                        http("http://%s:%d/fs/data/blazing/?fmt=json" % (a, WEB_PORT), timeout=15)
                        self.ip = a
                        break
                    except (urllib.error.URLError, OSError):
                        pass
                else:
                    raise SystemExit("%s not found: %s" % (target, str(e).splitlines()[0]))
        self.web = "http://%s:%d" % (self.ip, WEB_PORT)
        self.api = "http://%s:%d" % (self.ip, API_PORT)

    def ftp(self):
        f = ftplib.FTP()
        f.connect(self.ip, FTP_PORT, timeout=60)
        f.login()
        f.voidcmd("TYPE I")
        return f

    def ls(self, path):
        """{name: {"size", "dir"}} of a console folder over websrv, or None when it is not there."""
        try:
            rows = json.loads(http(self.web + "/fs" + urllib.parse.quote(path.rstrip("/")) + "/?fmt=json", timeout=30))
        except (urllib.error.URLError, OSError, ValueError):
            return None
        out = {}
        for e in rows:
            if e.get("name") in (".", "..", None):
                continue
            out[e["name"]] = {"size": int(e.get("size") or 0), "dir": e.get("type") == "dir" or str(e.get("mode", "")).startswith("d")}
        return out

    def read(self, path, timeout=30):
        try:
            return http(self.web + "/fs" + urllib.parse.quote(path), timeout=timeout)
        except (urllib.error.URLError, OSError):
            return None

    def status(self):
        try:
            return json.loads(http(self.api + "/api/status", timeout=15))
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def sha256(self, path, off, length):
        """(hex, bytes hashed) from the console itself; (None, 0) no answer; raises NoVerify on a payload older than 2.8."""
        q = urllib.parse.urlencode({"path": path, "off": off, "len": length})
        try:
            d = json.loads(http(self.api + "/api/sha256?" + q, timeout=600))
            return d.get("sha256"), int(d.get("len") or 0)
        except urllib.error.HTTPError as e:
            if e.code == 404 and b"no such file" not in (e.read() or b""):
                raise NoVerify()
            return None, 0
        except (urllib.error.URLError, OSError, ValueError, socket.timeout):
            return None, 0

    def action(self, q):
        try:
            return json.loads(http(self.api + "/api/action?" + q, method="POST", headers={"X-BM-Request": "1"}, timeout=20))
        except urllib.error.HTTPError as e:
            return {"ok": False, "error": "HTTP %d" % e.code}
        except (urllib.error.URLError, OSError, ValueError) as e:
            return {"ok": False, "error": str(e)}

    def running(self):
        """The title id running now ('' = none, None = unknown)."""
        try:
            out = subprocess.run(["ps5-status", self.name], capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.TimeoutExpired):
            return None
        m = re.search(r"RUNNING\s+((?:PPSA|CUSA)\d{5})", out)
        if m:
            return m.group(1)
        return "" if re.search(r"\b(IDLE|HOME)\b", out) else None


class NoVerify(Exception):
    pass


def ftp_size(f, path):
    """Size of one console file by LIST of its folder (SIZE is not trusted there), None when absent."""
    parent, base = path.rsplit("/", 1)
    lines = []
    try:
        f.retrlines("LIST " + parent, lines.append)
    except ftplib.all_errors:
        return None
    for line in lines:
        p = line.split(None, 8)
        if len(p) == 9 and p[8] == base and not line.startswith("d"):
            return int(p[4])
    return None


def ftp_mkdirs(f, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            f.mkd(cur)
        except ftplib.error_perm:
            pass


def ftp_delete(f, path):
    """DELE that takes any 2xx: ftpsrv answers 226, which ftplib's delete() raises on."""
    return f.sendcmd("DELE " + path)


def ftp_rename(f, a, b):
    f.sendcmd("RNFR " + a)
    f.sendcmd("RNTO " + b)


def avbl_mb(f, drive):
    try:
        return int(f.sendcmd("AVBL " + drive).split()[1]) // (1024 * 1024)
    except NET_ERRORS + (IndexError, ValueError):
        return None


# ---------------------------------------------------------------- what to copy

def games_json():
    d = json.loads(http(IMGSERVE + "/games.json", timeout=60))
    return d["games"] if isinstance(d, dict) else d


def find_local(name, size):
    for root in ROOTS:
        for dp, dns, fns in os.walk(root):
            if dp[len(root):].count(os.sep) >= 4:
                dns[:] = []
            if name in fns:
                p = os.path.join(dp, name)
                if os.path.getsize(p) == size:
                    return p
            dns[:] = [d for d in dns if not d.startswith(".") and not d.startswith("$")]
    return None


def read_cfg(con, tid):
    raw = con.read("/data/blazing/link/%s.cfg" % tid)
    cfg = {"files": [], "raw": raw}
    for line in (raw or b"").decode("utf-8", "replace").splitlines():
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "file" and len(p) >= 3:
            cfg["files"].append((int(p[1]), line.split(None, 2)[2].strip()))
        elif p[0] in ("server", "base") and len(p) >= 2:
            cfg[p[0]] = p[1:]
    return cfg


def find_starter(con, hb, tid):
    for n, e in (con.ls(hb) or {}).items():
        if e["dir"] and n.endswith("(%s)" % tid):
            return n
    return None


def fw_tuple(s):
    try:
        return tuple(int(x) for x in s.split(".")[:2])
    except (ValueError, AttributeError):
        return (0, 0)


def make_plan(con, tid, st=None):
    """Everything about one title on one console; changes nothing."""
    st = st or con.status() or {}
    hb = st.get("homebrew_dir") or "/data/homebrew"
    drive = hb.rsplit("/homebrew", 1)[0] or "/data"
    free = st.get("free_mb") or {}
    if drive == "/data" and (free.get("ext1") or 0) > 1:
        raise SystemExit("refused: the payload's homebrew is on /data but this console has an M.2 (never /data then)")
    p = {"tid": tid, "console": con.name, "ip": con.ip, "drive": drive, "hb": hb, "console_fw": con.fw,
         "min_free_mb": int(st.get("min_free_mb") or 51200), "route": st.get("route", "?")}
    cfg = read_cfg(con, tid)
    p["starter"] = find_starter(con, hb, tid)
    p["cfg"] = bool(cfg["raw"])
    server = cfg.get("server") or []
    if len(server) >= 2 and server[1] == "4452":
        key = (cfg.get("base") or [""])[0].rsplit("/", 1)[-1]          # /t/PPSA38474@01.002.000
        ver = key.split("@", 1)[1] if "@" in key else ""
        listing = http(DIRSERVE + "/", timeout=30).decode("utf-8", "replace")
        src = None
        for line in listing.splitlines():
            m = re.match(r"\s+(\S+)\s+(\S+)\s+(/.+)$", line)
            if m and m.group(1) == tid and (not ver or m.group(2) == ver):
                src = m.group(3).strip()
        if not src:
            raise SystemExit("%s: dirserve has no folder for %s" % (tid, key))
        p.update(kind="files", key=key, src=src, title=p["starter"] or tid, game_fw="",
                 files=[{"rel": rel, "size": size, "src": os.path.join(src, rel),
                         "dest": "%s/blz-stage/incoming/%s-files/%s.part" % (drive, tid, rel)} for size, rel in cfg["files"]])
        for f in p["files"]:
            if not os.path.isfile(f["src"]) or os.path.getsize(f["src"]) != f["size"]:
                raise SystemExit("%s: %s is not %d B on mac1" % (tid, f["src"], f["size"]))
    else:
        key = (cfg.get("base") or [""])[0].rsplit("/", 1)[-1]
        imgs = [g for g in games_json() if g.get("tid") == tid]
        if not imgs:
            raise SystemExit("%s: imgserve has no image of it" % tid)
        g = next((g for g in imgs if g.get("key") == key), None) or next((g for g in imgs if con.name in g.get("name", "")), imgs[0])
        path = find_local(g["name"], int(g["size"]))
        if not path:
            raise SystemExit("%s: %s (%d B) not found under %s" % (tid, g["name"], int(g["size"]), ROOTS))
        p.update(kind="image", key=g["key"], title=g.get("title") or tid, game_fw=g.get("fw", ""), name=g["name"],
                 files=[{"rel": g["name"], "size": int(g["size"]), "src": path,
                         "dest": "%s/blz-stage/incoming/%s.part" % (drive, g["name"])}])
        # a full copy of this image already on the console: its homebrew (done), parked on this drive, parked elsewhere
        p["have"] = []
        for where in (hb, drive + "/blz-stage/parked", "/data/blz-parked", "/data/homebrew", drive + "/blz-stage/incoming"):
            e = (con.ls(where) or {}).get(g["name"])
            if e and not e["dir"] and e["size"] == int(g["size"]):
                p["have"].append(where + "/" + g["name"])
    p["total"] = sum(f["size"] for f in p["files"])
    p["needs_overlay"] = p["kind"] == "image" and bool(p["game_fw"]) and fw_tuple(p["game_fw"]) > fw_tuple(con.fw or "99.99")
    return p


# ---------------------------------------------------------------- the transfer

class Stop(Exception):
    pass


def stop_check(args, job):
    """STOP (every run) or jobs/<console>-<TID>.stop (this title: the app's Cancel, through `serve`)."""
    for p in (args.cancel_file, job_file("%s:%s" % (job["console"], job["tid"]))[:-5] + ".stop"):
        if p and os.path.exists(p):
            raise Stop("stop file " + p)


def streaming_now():
    """True when imgserve served a request in the last YIELD_S seconds (a streamed game is playing)."""
    try:
        with open(IMGLOG, "rb") as fh:
            fh.seek(0, 2)
            fh.seek(max(0, fh.tell() - 65536))
            tail = fh.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return False
    now = datetime.datetime.now()
    for line in reversed(tail[-200:]):
        m = re.match(r"(\d\d):(\d\d):(\d\d)\.\d+ GET ", line)
        if not m:
            continue
        t = now.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=int(m.group(3)), microsecond=0)
        if t > now:
            t -= datetime.timedelta(days=1)
        return (now - t).total_seconds() < YIELD_S
    return False


def local_piece_sha(src, k, size):
    h = hashlib.sha256()
    with open(src, "rb") as fh:
        fh.seek(k * PIECE)
        left = min(PIECE, size - k * PIECE)
        while left > 0:
            b = fh.read(min(4 * CHUNK, left))
            if not b:
                raise OSError("short read in " + src)
            h.update(b)
            left -= len(b)
    return h.hexdigest()


class Rate:
    def __init__(self):
        self.pts = []

    def add(self, n):
        t = time.monotonic()
        self.pts.append((t, n))
        while self.pts and t - self.pts[0][0] > 60:
            self.pts.pop(0)

    def bps(self):
        if len(self.pts) < 2 or self.pts[-1][0] - self.pts[0][0] < 5:
            return 0.0
        return (self.pts[-1][1] - self.pts[0][1]) / (self.pts[-1][0] - self.pts[0][0])


def verify_piece(con, fj, f, k):
    """True match, False mismatch, None console could not answer (yet). Raises NoVerify."""
    want_len = min(PIECE, f["size"] - k * PIECE)
    got, n = con.sha256(f["dest"], k * PIECE, want_len)
    if not got or n != want_len:
        return None
    if got.lower() == fj["pieces"][k]:
        if k not in fj["verified"]:
            fj["verified"].append(k)
        return True
    return False


def send_file(con, st, job, f, rate, base_done, total, args):
    """Send one file to its .part until whole; pieces hashed, verified when the payload can. Returns when whole."""
    fj = job["files"].setdefault(f["rel"], {"size": f["size"], "pieces": [], "verified": [], "bad": {}})
    npieces = (f["size"] + PIECE - 1) // PIECE
    fails = 0
    while True:
        stop_check(args, job)
        if not args.no_yield and streaming_now():
            job["state"], job["msg"] = "waiting", "a streamed game is playing (imgserve busy): paused"
            save_state(st)
            log("%s: paused, imgserve is serving a streamed game; checking again in 60 s" % job["tid"])
            while streaming_now():
                time.sleep(60)
            log("%s: imgserve quiet, going on" % job["tid"])
        try:
            c = con.ftp()
        except NET_ERRORS as e:
            fails += 1
            wait = min(300, 15 * fails)
            log("%s: console FTP unreachable (%s); again in %d s" % (job["tid"], e, wait))
            time.sleep(wait)
            continue
        try:
            ftp_mkdirs(c, f["dest"].rsplit("/", 1)[0])
            have = ftp_size(c, f["dest"]) or 0
            start = have if have <= f["size"] else 0
            bad = sorted(int(k) for k in fj["bad"])
            if bad:                                          # a piece that failed its check: from its start again
                start = min(start, bad[0] * PIECE)
            k0 = min(start, f["size"]) // PIECE
            del fj["pieces"][k0:]
            fj["verified"] = [k for k in fj["verified"] if k < k0]
            # whole pieces already on the console that the journal has no hash for (a stop between the last byte
            # and the journal write, or bytes sent by another run): hashed from mac1's copy, checked like the rest
            for k in range(len(fj["pieces"]), npieces if start >= f["size"] else k0):
                fj["pieces"].append(local_piece_sha(f["src"], k, f["size"]))
            save_state(st)
            if start >= f["size"]:
                c.quit()
                return
            h = hashlib.sha256()
            with open(f["src"], "rb") as fh:
                if start > k0 * PIECE:                       # re-hash the head of the current piece from mac1's copy
                    fh.seek(k0 * PIECE)
                    left = start - k0 * PIECE
                    while left:
                        b = fh.read(min(4 * CHUNK, left))
                        h.update(b)
                        left -= len(b)
                fh.seek(start)
                if start == 0 and have:
                    ftp_delete(c, f["dest"])
                job["state"], job["msg"] = "sending", ""
                save_state(st)
                log("%s: %s from %s of %s (%d/%d pieces checked)" % (job["tid"], f["rel"], gb(start), gb(f["size"]),
                                                                    len(fj["verified"]), npieces))
                conn = c.transfercmd("STOR " + f["dest"], rest=start or None)
                pos, last_note, verify_due = start, time.monotonic(), []
                try:
                    while pos < f["size"]:
                        b = fh.read(min(CHUNK, f["size"] - pos, PIECE - pos % PIECE))
                        if not b:
                            raise OSError("short read of " + f["src"])
                        conn.sendall(b)
                        h.update(b)
                        pos += len(b)
                        rate.add(base_done + pos)
                        if pos % PIECE == 0 or pos == f["size"]:
                            fj["pieces"].append(h.hexdigest())
                            fj["bad"].pop(str(len(fj["pieces"]) - 1), None)
                            h = hashlib.sha256()
                            verify_due.append(len(fj["pieces"]) - 1)
                            save_state(st)
                        # check a piece once 64 MiB more went after it (ftpsrv has written it by then; one piece in the tests)
                        if verify_due and job.get("can_verify") is not False and pos - (verify_due[0] + 1) * PIECE >= min(64 * CHUNK, PIECE):
                            k = verify_due.pop(0)
                            try:
                                ok = verify_piece(con, fj, f, k)
                                job["can_verify"] = True
                            except NoVerify:
                                job["can_verify"], ok = False, None
                            if ok is False:
                                fj["bad"][str(k)] = fj["bad"].get(str(k), 0) + 1
                                save_state(st)
                                raise IOError("piece %d of %s did not match on the console" % (k, f["rel"]))
                            if ok:
                                save_state(st)
                        if time.monotonic() - last_note >= args.every:
                            last_note = time.monotonic()
                            bps = rate.bps()
                            done = base_done + pos
                            job["done"], job["rate"] = done, int(bps)
                            job["eta"] = int((total - done) / bps) if bps > 0 else 0
                            save_state(st)
                            log("%s: %s / %s (%.1f%%)  %.2f MB/s  ~%s left" % (job["tid"], gb(done), gb(total), 100.0 * done / total,
                                                                         bps / 1e6, dur(job["eta"]) if bps else "?"))
                            stop_check(args, job)
                            if not args.no_yield and streaming_now():
                                raise IOError("yield to a streamed game")
                finally:
                    conn.close()
                c.voidresp()
            c.quit()
            fails = 0
            if pos >= f["size"]:
                return
        except Stop:
            raise
        except NET_ERRORS as e:
            fails += 1
            if "did not match" in str(e) and max(fj["bad"].values() or [0]) >= 3:
                raise SystemExit("%s: a piece of %s failed its check 3 times; stopping" % (job["tid"], f["rel"]))
            wait = 5 if "yield" in str(e) else min(300, 15 * fails)
            log("%s: send stopped (%s); going on in %d s" % (job["tid"], e, wait))
            try:
                c.close()
            except Exception:
                pass
            time.sleep(wait)


def verify_all(con, st, job, plan):
    """Every piece of every file checked on the console. True = all match."""
    for f in plan["files"]:
        fj = job["files"].get(f["rel"])
        npieces = (f["size"] + PIECE - 1) // PIECE
        if not fj or len(fj["pieces"]) < npieces:
            return False
        for k in range(npieces):
            if k in fj["verified"]:
                continue
            ok = verify_piece(con, fj, f, k)                 # NoVerify goes up
            if ok is None:
                log("%s: the console did not answer for piece %d of %s" % (job["tid"], k, f["rel"]))
                return False
            if not ok:
                fj["bad"][str(k)] = fj["bad"].get(str(k), 0) + 1
                save_state(st)
                log("%s: piece %d of %s does NOT match: it will be sent again" % (job["tid"], k, f["rel"]))
                return False
            save_state(st)
        log("%s: %s - all %d pieces match on the console" % (job["tid"], f["rel"], npieces))
    return True


# ---------------------------------------------------------------- the switch

def wait_not_running(con, tid):
    while True:
        r = con.running()
        if r is not None and r != tid:
            return
        log("%s on %s: the switch waits (checking every 2 min)" % ("%s is running" % tid if r else "cannot tell what runs", con.name))
        time.sleep(120)


def claim(con, why):
    try:
        r = subprocess.run(["ps5-claim", con.name, "dl-local", why], capture_output=True, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the console is claimed by someone else: " + (r.stdout + r.stderr).strip()[:300])
    except FileNotFoundError:
        pass


def release(con):
    try:
        subprocess.run(["ps5-release", con.name, "dl-local"], capture_output=True, text=True, timeout=60)
    except FileNotFoundError:
        pass


def notice(con, text):
    try:
        sys.path.insert(0, TOOLS)
        from ps5notice import notify
        notify(con.ip, text)
    except Exception as e:                                   # best effort, like ps5notice itself
        log("TV notice not sent: %s" % e)


def smp_lines(con, tid, since):
    raw = con.read("/data/shadowmount/debug.log", timeout=60) or b""
    out = []
    for line in raw.decode("utf-8", "replace").splitlines()[-400:]:
        m = re.search(r"(\d\d:\d\d:\d\d)", line)
        if tid in line and m and m.group(1) >= since:
            out.append(line.strip())
    return out


def do_switch(con, st, job, plan, src_path=None):
    tid, drive, hb, s = plan["tid"], plan["drive"], plan["hb"], stamp()
    wait_not_running(con, tid)
    claim(con, "switch %s to its local copy" % tid)
    undo = job.setdefault("undo", [])
    t0 = time.strftime("%H:%M:%S")
    try:
        c = con.ftp()
        if plan["kind"] == "files":
            starter = "%s/%s" % (hb, plan["starter"])
            park = "%s/blz-stage/stub/%s-placeholders-%s" % (drive, tid, s)
            for f in plan["files"]:
                live = "%s/%s" % (starter, f["rel"])
                cur = ftp_size(c, live)
                if cur not in (0, None):
                    raise SystemExit("%s is %s B, not a 0-byte placeholder: stopping before any change" % (live, cur))
            for f in plan["files"]:
                live = "%s/%s" % (starter, f["rel"])
                if ftp_size(c, live) == 0:
                    ftp_mkdirs(c, (park + "/" + f["rel"]).rsplit("/", 1)[0])
                    ftp_rename(c, live, park + "/" + f["rel"])
                    undo.append(["rename", park + "/" + f["rel"], live])
                ftp_rename(c, f["dest"], live)
                undo.append(["rename", live, f["dest"]])
                save_state(st)
                log("%s: %s in place (%s)" % (tid, f["rel"], gb(f["size"])))
        else:
            r = con.action("do=hide&tid=" + tid)
            log("%s: hidden in the payload: %s" % (tid, r))
            undo.append(["unhide", tid])
            if plan["needs_overlay"]:
                ov = "%s/backports/%s" % (hb, tid)
                if (con.ls(hb + "/backports") or {}).get(tid):
                    log("%s: overlay %s already there" % (tid, ov))
                else:
                    dis = sorted(n for n in (con.ls(drive + "/blz-disabled") or {}) if n.startswith(tid + "-overlay-"))
                    if not dis:
                        raise SystemExit("%s needs FW %s (console %s) and no overlay is kept in %s/blz-disabled: stopping before "
                                         "the starter is parked" % (tid, plan["game_fw"], con.fw, drive))
                    ftp_mkdirs(c, hb + "/backports")
                    ftp_rename(c, "%s/blz-disabled/%s" % (drive, dis[-1]), ov)
                    undo.append(["rename", ov, "%s/blz-disabled/%s" % (drive, dis[-1])])
                    log("%s: overlay %s restored from blz-disabled/%s" % (tid, ov, dis[-1]))
            if plan["starter"]:
                parked = "%s/blz-stage/stub/%s-off-%s" % (drive, plan["starter"], s)
                ftp_mkdirs(c, drive + "/blz-stage/stub")
                ftp_rename(c, "%s/%s" % (hb, plan["starter"]), parked)
                undo.append(["rename", parked, "%s/%s" % (hb, plan["starter"])])
                log("%s: starter parked -> %s" % (tid, parked))
            final = "%s/%s" % (hb, plan["name"])
            src = src_path or plan["files"][0]["dest"]
            if src.split("/")[1:3] != final.split("/")[1:3] and not (src.startswith("/data/") and final.startswith("/data/")):
                raise SystemExit("refused: %s and %s are on different drives (a cross-drive rename panics the kernel)" % (src, final))
            ftp_rename(c, src, final)
            undo.append(["rename", final, src])
            log("%s: image in place -> %s" % (tid, final))
            save_state(st)
        if plan["cfg"]:
            cfgp = "/data/blazing/link/%s.cfg" % tid
            ftp_rename(c, cfgp, cfgp + ".off-" + s)
            undo.append(["rename", cfgp + ".off-" + s, cfgp])
            log("%s: link cfg -> %s.cfg.off-%s" % (tid, tid, s))
        c.quit()
        save_state(st)
    finally:
        release(con)
    if plan["kind"] == "image":
        for _ in range(SMP_WAIT // 10):                      # ShadowMount: up to 3 minutes
            time.sleep(10)
            lines = smp_lines(con, tid, t0)
            if any("source removed" in l for l in lines) and any(("mount" in l.lower() or "register" in l.lower()) and "removed" not in l
                                                                  for l in lines):
                break
        for l in smp_lines(con, tid, t0)[-8:]:
            log("  smp: " + l)
    job["state"], job["msg"], job["switched_at"] = "done", "plays from the console's own drive", int(time.time())
    save_state(st)
    notice(con, "%s is on your console now: it plays without the network." % plan["title"])
    hub("%s %s (%s) SWITCHED to its local copy on %s; undo steps in ~/.blazing/download-local/jobs/" % (
        con.name, plan["title"], tid, plan["drive"]))


# ---------------------------------------------------------------- commands

def jkey(con, tid):
    return "%s:%s" % (con.name, tid)


def cmd_plan(args):
    con = Console(args.console)
    st = con.status() or {}
    c = con.ftp()
    rate = float(args.rate) * 1e6
    for tid in args.tid:
        p = make_plan(con, tid, st)
        free = avbl_mb(c, p["drive"])
        print("%s  %s  [%s]" % (tid, p["title"], p["kind"]))
        print("  size %s  -> %s (free %s MB, keeps %s MB)" % (gb(p["total"]), p["drive"], free, p["min_free_mb"]))
        if p.get("have"):
            print("  ALREADY ON THE CONSOLE: %s" % ", ".join(p["have"]))
        else:
            print("  at %.2f MB/s: %s, %s of hotspot data" % (rate / 1e6, dur(p["total"] / rate), gb(p["total"])))
        print("  starter %s, cfg %s, game FW %s vs console %s%s" % (p["starter"], "yes" if p["cfg"] else "no", p.get("game_fw") or "-",
                                                                   con.fw, " -> needs its overlay" if p["needs_overlay"] else ""))
    c.quit()


def cmd_get(args):
    con = Console(args.console)
    st = load_state()
    for tid in args.tid:
        if not lock_job(jkey(con, tid)):
            log("%s: another run is working on it; skipped here" % tid)
            continue
        st = load_state()                                    # what other runs finished meanwhile
        status = con.status() or {}
        plan = make_plan(con, tid, status)
        job = own_job(st, jkey(con, tid), tid, con.name)
        job.update(title=plan["title"], kind=plan["kind"], total=plan["total"], drive=plan["drive"])
        if job.get("state") == "done":
            log("%s: already done (%s)" % (tid, job.get("msg")))
            continue
        job.pop("can_verify", None)                          # the payload may have been updated since the last run
        have = plan.get("have", [])
        if any(h.startswith(plan["hb"] + "/") for h in have):
            job["state"], job["msg"] = "failed", "the image is already in %s: look before changing anything" % plan["hb"]
            save_state(st)
            log("%s: %s" % (tid, job["msg"]))
            continue
        same = lambda h: h.split("/")[1:3] == plan["drive"].split("/")[1:3] if plan["drive"] != "/data" else h.startswith("/data/")
        here = [h for h in have if same(h)]
        there = [h for h in have if not same(h)]
        if plan["kind"] == "image" and here:
            log("%s: a full copy is already on %s: %s -> switching to it (0 bytes over the network)" % (tid, plan["drive"], here[0]))
            if args.no_switch:
                continue
            do_switch(con, st, job, plan, src_path=here[0])
            continue
        if plan["kind"] == "image" and there:
            log("%s: a full copy is on another drive of the console (%s): copied ON the console, 0 bytes over the network"
                % (tid, there[0]))
            hub("%s (%s): full copy found at %s -> on-console copy to %s (0 network bytes), then piece check + switch" % (
                plan["title"], tid, there[0], plan["drive"]))
            console_copy(con, st, job, plan, there[0], args)
            finish(con, st, job, plan, args)
            continue
        c = con.ftp()
        free = avbl_mb(c, plan["drive"]) or 0
        c.quit()
        staged = 0
        for f in plan["files"]:
            fj = job["files"].get(f["rel"])
            staged += min(f["size"], len(fj["pieces"]) * PIECE) if fj else 0
        need_mb = -(-(plan["total"] - staged) // (1024 * 1024))
        keep = max(plan["min_free_mb"], DATA_KEEP_MB if plan["drive"] == "/data" else 0)
        if free - need_mb < keep:
            job["state"], job["msg"] = "failed", "no space: %d MB free, %d MB needed, %d MB kept free" % (free, need_mb, keep)
            save_state(st)
            log("%s: %s" % (tid, job["msg"]))
            continue
        mbps = (job.get("rate") or 0) / 1e6 or float(args.rate)          # this title's last measured speed, else --rate
        fresh = not any(f.get("pieces") for f in job["files"].values())
        eta = (plan["total"] - staged) / (mbps * 1e6)
        log("%s %s: %s to %s:%s (%d file%s), free %d MB; at ~%.2f MB/s about %s" % (
            tid, plan["title"], gb(plan["total"]), con.name, plan["drive"], len(plan["files"]), "s" * (len(plan["files"]) > 1),
            free, mbps, dur(eta)))
        hub("%s %s (%s): %s -> %s:%s, ETA ~%s at %.2f MB/s; resumable, every 1 GiB piece hashed" % (
            "START" if fresh else "RESUME", plan["title"], tid, gb(plan["total"]), con.name, plan["drive"], dur(eta), mbps))
        if args.notice and fresh:                            # once per title, not at every resume
            notice(con, "Downloading %s to your console: %s, about %s on this connection." % (plan["title"], gb(plan["total"]), dur(eta)))
        rate, base = Rate(), 0
        for f in plan["files"]:
            send_file(con, st, job, f, rate, base, plan["total"], args)
            base += f["size"]
        job["done"] = plan["total"]
        job["state"], job["msg"] = "verifying", ""
        save_state(st)
        log("%s: all %s sent" % (tid, gb(plan["total"])))
        hub("%s (%s): all %s sent to %s; verifying" % (plan["title"], tid, gb(plan["total"]), con.name))
        if not finish(con, st, job, plan, args):
            continue


def console_copy(con, st, job, plan, src, args):
    """ftpsrv's SITE CPFR/CPTO: the console copies a file of its own into incoming/ in the background. Never while a game
    runs (its reads share the drive), unless --while-playing. mac1 hashes its own copy meanwhile; verify_all compares."""
    f = plan["files"][0]
    fj = job["files"].setdefault(f["rel"], {"size": f["size"], "pieces": [], "verified": [], "bad": {}})
    while not args.while_playing:
        r = con.running()
        if r == "":
            break
        log("%s: %s is running: the on-console copy waits (checking every 2 min)" % (plan["tid"], r or "something"))
        job["state"], job["msg"] = "waiting", "a game is running: the on-console copy waits"
        save_state(st)
        time.sleep(120)
    c = con.ftp()
    ftp_mkdirs(c, f["dest"].rsplit("/", 1)[0])
    cur = ftp_size(c, f["dest"])
    if cur != f["size"]:
        if cur is not None:
            ftp_delete(c, f["dest"])
        fj.update(pieces=[], verified=[], bad={})
        c.sendcmd("SITE CPFR " + src)
        log("%s: %s" % (plan["tid"], c.sendcmd("SITE CPTO " + f["dest"])))
    c.quit()
    job["state"], job["msg"] = "sending", "copied on the console from " + src
    save_state(st)
    npieces = (f["size"] + PIECE - 1) // PIECE
    for k in range(len(fj["pieces"]), npieces):              # mac1's hashes, while the console copies
        fj["pieces"].append(local_piece_sha(f["src"], k, f["size"]))
        save_state(st)
    last, same, t_last = -1, 0, time.monotonic()
    while True:
        try:
            c = con.ftp()
            n = ftp_size(c, f["dest"]) or 0
            c.quit()
        except NET_ERRORS:
            n = last
        if n == f["size"] and n == last:
            same += 1
            if same >= 2:
                break
        elif n != last:
            same, t_last = 0, time.monotonic()
            log("%s: on-console copy %s / %s" % (plan["tid"], gb(n), gb(f["size"])))
        if time.monotonic() - t_last > 900:
            raise SystemExit("%s: the on-console copy stopped growing at %s for 15 min" % (plan["tid"], gb(n)))
        last = n
        time.sleep(15)
    job["done"] = f["size"]
    log("%s: on-console copy complete (%s)" % (plan["tid"], gb(f["size"])))


def finish(con, st, job, plan, args):
    """Verify, then switch. False when it has to wait (old payload) or a piece must be sent again."""
    for attempt in range(4):
        try:
            ok = verify_all(con, st, job, plan)
        except NoVerify:
            job["state"], job["msg"] = "waiting", VERIFY_MISS
            save_state(st)
            log("%s: sent, NOT switched: %s. Run `verify` / `get` again after the payload update." % (plan["tid"], VERIFY_MISS))
            hub("%s (%s): sent; waits for payload 2.8 to verify on the console before the switch" % (plan["title"], plan["tid"]))
            return False
        if ok:
            break
        bad = [f for f in plan["files"] if job["files"].get(f["rel"], {}).get("bad")]
        if not bad:
            return False
        rate, base = Rate(), 0
        for f in plan["files"]:
            if f in bad:
                send_file(con, st, job, f, rate, base, plan["total"], args)
            base += f["size"]
    else:
        return False
    job["state"], job["verified_at"] = "switching", int(time.time())
    save_state(st)
    hub("%s (%s): every piece verified on %s" % (plan["title"], plan["tid"], con.name))
    if args.no_switch:
        log("%s: verified; --no-switch given" % plan["tid"])
        return True
    if plan["kind"] == "put":
        do_place(con, st, job, plan)
    else:
        do_switch(con, st, job, plan)
    return True


# ---------------------------------------------------------------- put: any files, same pieces and checks

def make_put_plan(con, srcs, to, label, st=None):
    st = st or con.status() or {}
    to = to.rstrip("/")
    drive = next((d for d in DRIVES if to.startswith(d + "/")), None)
    if not drive:
        raise SystemExit("refused: --to must be under one of %s" % ", ".join(DRIVES))
    if drive == "/data" and ((st.get("free_mb") or {}).get("ext1") or 0) > 1:
        raise SystemExit("refused: never /data on a console with an M.2 (use /mnt/ext1/...)")
    files = []
    for src in srcs:
        if not os.path.isfile(src):
            raise SystemExit("not a file: " + src)
        n = os.path.basename(src)
        files.append({"rel": n, "size": os.path.getsize(src), "src": os.path.abspath(src),
                      "dest": "%s/blz-stage/incoming/put-%s/%s.part" % (drive, label, n)})
    return {"tid": "put-" + label, "kind": "put", "title": "%d file%s to %s" % (len(files), "s" * (len(files) > 1), to), "to": to,
            "drive": drive, "files": files, "total": sum(f["size"] for f in files), "console": con.name,
            "min_free_mb": int(st.get("min_free_mb") or 51200)}


def do_place(con, st, job, plan):
    """Every verified file renamed from incoming/ into --to (same drive). A name already there is left alone and reported."""
    c = con.ftp()
    ftp_mkdirs(c, plan["to"])
    undo = job.setdefault("undo", [])
    kept = []
    for f in plan["files"]:
        target = "%s/%s" % (plan["to"], f["rel"])
        if ftp_size(c, target) is not None:
            kept.append(f["rel"])
            log("%s: %s is already there: left as it is, the copy stays in %s" % (plan["tid"], target, f["dest"]))
            continue
        ftp_rename(c, f["dest"], target)
        undo.append(["rename", target, f["dest"]])
        save_state(st)
        log("%s: in place -> %s (%s)" % (plan["tid"], target, gb(f["size"])))
    c.quit()
    job["state"] = "done"
    job["msg"] = "in %s" % plan["to"] + (" (%d already there, not replaced)" % len(kept) if kept else "")
    job["switched_at"] = int(time.time())
    save_state(st)
    hub("%s: %s on %s, every piece verified" % (plan["tid"], plan["title"], con.name))


def cmd_put(args):
    if not args.to:
        raise SystemExit("put needs --to <console folder>")
    con = Console(args.console)
    label = args.label or re.sub(r"[^A-Za-z0-9._-]+", "-", args.to.rstrip("/").rsplit("/", 1)[-1])
    key = "%s:put-%s" % (con.name, label)
    if not lock_job(key):
        raise SystemExit("put-%s: another run is working on it" % label)
    st = load_state()
    status = con.status() or {}
    plan = make_put_plan(con, args.tid, args.to, label, status)
    job = own_job(st, key, plan["tid"], con.name)
    job.pop("can_verify", None)
    job.update(title=plan["title"], kind="put", total=plan["total"], drive=plan["drive"], to=plan["to"])
    if job.get("state") == "done":
        log("%s: already done (%s)" % (plan["tid"], job.get("msg")))
        return
    c = con.ftp()
    free = avbl_mb(c, plan["drive"]) or 0
    c.quit()
    need_mb = -(-plan["total"] // (1024 * 1024))
    if free - need_mb < plan["min_free_mb"]:
        job["state"], job["msg"] = "failed", "no space: %d MB free, %d MB needed, %d MB kept free" % (free, need_mb, plan["min_free_mb"])
        save_state(st)
        raise SystemExit(job["msg"])
    log("%s: %s, %s, at ~%.2f MB/s about %s" % (plan["tid"], plan["title"], gb(plan["total"]), float(args.rate),
                                                dur(plan["total"] / (float(args.rate) * 1e6))))
    hub("%s START: %s, %s -> %s" % (plan["tid"], plan["title"], gb(plan["total"]), con.name))
    rate, base = Rate(), 0
    for f in plan["files"]:
        send_file(con, st, job, f, rate, base, plan["total"], args)
        base += f["size"]
    job["done"], job["state"] = plan["total"], "verifying"
    save_state(st)
    finish(con, st, job, plan, args)


def cmd_verify(args):
    con = Console(args.console)
    st = load_state()
    args.no_switch = True
    for tid in args.tid:
        job = st["jobs"].get(jkey(con, tid))
        if not job:
            log("%s: no job" % tid)
            continue
        st["own"].add(jkey(con, tid))
        finish(con, st, job, make_plan(con, tid), args)


def cmd_switch(args):
    con = Console(args.console)
    st = load_state()
    job = st["jobs"].get(jkey(con, args.tid[0]))
    st["own"].add(jkey(con, args.tid[0]))
    plan = make_plan(con, args.tid[0])
    if not job or job.get("state") != "switching":
        raise SystemExit("%s: not verified yet (state %s)" % (args.tid[0], job and job.get("state")))
    do_switch(con, st, job, plan)


def cmd_cancel(args):
    con = Console(args.console)
    st = load_state()
    for tid in args.tid:
        job = st["jobs"].get(jkey(con, tid))
        if not job or job.get("state") in ("done", "switching"):
            log("%s: nothing to cancel (state %s)" % (tid, job and job.get("state")))
            continue
        st["own"].add(jkey(con, tid))
        plan = make_plan(con, tid)
        c = con.ftp()
        for f in plan["files"]:
            if "/blz-stage/incoming/" in f["dest"] and ftp_size(c, f["dest"]) is not None:
                ftp_delete(c, f["dest"])
                log("%s: deleted %s" % (tid, f["dest"]))
        c.quit()
        job.update(state="cancelled", files={}, msg="cancelled")
        save_state(st)


# ---------------------------------------------------------------- the worker the app talks to (through the payload)

ACTIVE = ("queued", "waiting", "sending", "verifying", "switching", "removing")
DL_STATUS = "/data/blazing/dl-status.tsv"
WORKER_VERSION = "1"


def status_tsv(con, route):
    """dl-status.tsv for one console from the job files (docs/DOWNLOAD-TO-CONSOLE.md 3): worker, rate, one job line per title."""
    now = int(time.time())
    rate, jobs = 0, []
    for k, j in sorted(load_state()["jobs"].items()):
        if j.get("console") != con.name or not re.match(r"^(PPSA|CUSA)\d{5}$", j.get("tid", "")):
            continue
        state = j.get("state") or "queued"
        if state in ACTIVE and now - int(j.get("at") or 0) < 180:
            rate += int(j.get("rate") or 0)
        msg = re.sub(r"[\t\r\n]+", " ", j.get("msg") or "")[:200]
        jobs.append("job\t%s\t%s\t%d\t%d\t%d\t%d\t%s" % (j["tid"], state, int(j.get("done") or 0), int(j.get("total") or 0),
                                                         int(j.get("eta") or 0), int(j.get("at") or j.get("created") or 0), msg))
    return "".join(l + "\n" for l in ["# written by mac1 (blazingmount host/download-to-console.py serve); read by blazingmountd 2.8",
                                         "worker\t%d\t%s" % (now, WORKER_VERSION), "rate\t%d\t%d\t%s" % (rate, now, route)] + jobs)


def put_status(con, text):
    """STOR .tmp, then rename over the old file (ftpsrv never overwrites: the old one is deleted first)."""
    import io
    c = con.ftp()
    try:
        tmp = DL_STATUS + ".tmp"
        if ftp_size(c, tmp) is not None:
            ftp_delete(c, tmp)
        c.storbinary("STOR " + tmp, io.BytesIO(text.encode("utf-8")))
        if ftp_size(c, DL_STATUS) is not None:
            ftp_delete(c, DL_STATUS)
        ftp_rename(c, tmp, DL_STATUS)
    finally:
        try:
            c.quit()
        except NET_ERRORS:
            pass


def cmd_serve(args):
    """Answers the app: every --every-poll seconds GET /api/downloads of each console; a new `download` request starts a `get`
    run (at most --max at once per console), `cancel` stops that title's run and deletes its staged files, `remove_local` is
    reported as not built yet. dl-status.tsv goes back to the console each round, so the app shows the progress."""
    cons = [Console(t) for t in [args.console] + list(args.tid)]
    seenp = os.path.join(HOME, "serve-seen.json")
    try:
        seen = json.load(open(seenp))
    except (OSError, ValueError):
        seen = {}
    procs, cancelled = {}, set()
    log("serve: watching %s (poll %ds, at most %d runs per console)" % (", ".join(c.name for c in cons), args.poll, args.max))
    while True:
        for con in cons:
            try:
                d = json.loads(http(con.api + "/api/downloads", timeout=20))
            except (urllib.error.URLError, OSError, ValueError):
                d = None
            for r in (d or {}).get("requests", []):
                key, at = jkey(con, r.get("tid", "")), int(r.get("at") or 0)
                if not re.match(r"^(PPSA|CUSA)\d{5}$", r.get("tid", "")) or at <= int(seen.get(key, 0)):
                    continue
                seen[key] = at
                with open(seenp + ".tmp", "w") as fh:
                    json.dump(seen, fh)
                os.replace(seenp + ".tmp", seenp)
                stopf = job_file(key)[:-5] + ".stop"
                if r["what"] == "download":
                    if key in procs and procs[key].poll() is None:
                        log("serve: %s asked again: already running" % key)
                        continue
                    if sum(1 for k, p in procs.items() if k.startswith(con.name + ":") and p.poll() is None) >= args.max:
                        log("serve: %s waits: %d runs already on %s" % (key, args.max, con.name))
                        seen[key] = at - 1                    # look at it again next round
                        continue
                    if os.path.exists(stopf):
                        os.remove(stopf)
                    os.makedirs(os.path.join(HOME, "runs"), exist_ok=True)
                    cmd = [sys.executable, "-I", os.path.abspath(__file__), "get", con.name if con.name != con.ip else con.ip,
                           r["tid"], "--notice", "--hub", HUB, "--every", str(args.every)]
                    procs[key] = subprocess.Popen(cmd, stdout=open(os.path.join(HOME, "runs", key.replace(":", "-") + ".log"), "a"),
                                                  stderr=subprocess.STDOUT)
                    log("serve: %s: download asked by the app -> run pid %d" % (key, procs[key].pid))
                elif r["what"] == "cancel":
                    open(stopf, "w").write("cancelled by the app %d\n" % at)
                    cancelled.add(key)
                    log("serve: %s: cancel asked by the app" % key)
                elif r["what"] == "remove_local":
                    log("serve: %s: remove-local asked: not built yet (DOWNLOAD-TO-CONSOLE.md 5.5)" % key)
            for key in [k for k in cancelled if k.startswith(con.name + ":")]:
                if key in procs and procs[key].poll() is None:
                    continue                                  # it stops at its next progress check
                subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "cancel", con.name if con.name != con.ip else con.ip,
                                key.split(":", 1)[1]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
                cancelled.discard(key)
                log("serve: %s: cancelled, staged files deleted" % key)
            try:
                put_status(con, status_tsv(con, ((con.status() or {}).get("route") or "?")))
            except NET_ERRORS as e:
                log("serve: %s: dl-status not written (%s)" % (con.name, e))
        if args.once:
            if all(p.poll() is not None for p in procs.values()) and not cancelled:
                for con in cons:                              # the last word: every run's final state
                    try:
                        put_status(con, status_tsv(con, ((con.status() or {}).get("route") or "?")))
                    except NET_ERRORS:
                        pass
                return
            time.sleep(1)
            continue
        time.sleep(args.poll)


def cmd_status(args):
    st = load_state()
    for k, j in sorted(st["jobs"].items()):
        n = sum(len(f.get("verified", [])) for f in j.get("files", {}).values())
        p = sum(len(f.get("pieces", [])) for f in j.get("files", {}).values())
        print("%-16s %-10s %s  %s/%s  pieces hashed %d, checked on console %d  %s" % (
            k, j.get("state"), j.get("title", ""), gb(j.get("done", 0)), gb(j.get("total", 0)), p, n, j.get("msg", "")))


def main():
    global HUB
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("plan", "get", "put", "verify", "switch", "cancel", "serve", "status"))
    ap.add_argument("console", nargs="?")
    ap.add_argument("tid", nargs="*")
    ap.add_argument("--rate", default="0.85", help="MB/s used for time estimates (measured 24 Oct away: 0.86)")
    ap.add_argument("--no-switch", action="store_true", help="copy and verify only")
    ap.add_argument("--no-yield", action="store_true", help="do not pause while imgserve serves a streamed game")
    ap.add_argument("--while-playing", action="store_true", help="on-console copies run even while a game runs")
    ap.add_argument("--notice", action="store_true", help="a TV notice with size + time when a copy starts")
    ap.add_argument("--hub", default=HUB, help="team chat file for milestone lines")
    ap.add_argument("--to", help="put: the console folder the files end up in (same drive as the staging)")
    ap.add_argument("--label", help="put: the job's name (default: the last part of --to)")
    ap.add_argument("--poll", type=int, default=20, help="serve: seconds between rounds")
    ap.add_argument("--max", type=int, default=3, help="serve: runs at once per console (one stream each)")
    ap.add_argument("--once", action="store_true", help="serve: one round, wait for the runs it started, write the status, exit")
    ap.add_argument("--every", type=float, default=30.0, help="progress line every N seconds")
    ap.add_argument("--cancel-file", default=os.path.join(HOME, "STOP"), help="stop cleanly when this file exists")
    args = ap.parse_args()
    HUB = args.hub
    if args.cmd not in ("status", "serve") and (not args.console or not args.tid):
        ap.error("CONSOLE and at least one TID please")
    if args.cmd == "serve" and not args.console:
        ap.error("serve CONSOLE [CONSOLE...]")
    return {"plan": cmd_plan, "get": cmd_get, "put": cmd_put, "verify": cmd_verify, "switch": cmd_switch, "cancel": cmd_cancel,
            "serve": cmd_serve, "status": cmd_status}[args.cmd](args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Stop as e:
        log("stopped: %s (resumes from here next time)" % e)
        sys.exit(2)
    except KeyboardInterrupt:
        log("stopped by Ctrl-C (resumes from here next time)")
        sys.exit(2)
