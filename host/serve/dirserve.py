#!/usr/bin/env python3
"""dirserve.py - read-only HTTP Range server for plain GAME FOLDERS (the folder twin of imgserve, smb-link, 21 Oct 2026).

    python3 -I tools/imgserve/dirserve.py <root> [<root> ...] [--port 4452] [--bind 0.0.0.0] [--depth 4]

    GET /                                   text help + the game folders found (title id, version, path)
    GET /d/<folder>/?list                   JSON [{"path","size"}] of every file below <folder> (also /d/<folder>/); <folder> = a direct child of a root
    GET|HEAD /d/<folder>/<inner/path>       the file's bytes; Range: bytes=a-b | a- | -n -> 206 + Content-Range
    GET|HEAD /t/<TITLEID>[@<version>]/<inner/path>   the same, found BY TITLE ID (smb-link-5, 21 Oct 2026): a game folder = a directory with
                                            eboot.bin + sce_sys/param.json anywhere under the roots (--depth levels; not inside backports/decrypted/
                                            fakelib); renaming or moving it never breaks a PS5 starter. Rescans when a title id is not found (every 15 s at most).

Why a second server: imgserve serves files INSIDE .ffpfsc images (Onimusha, Tokon...); Samson (PPSA38474) is a plain folder on the
8 TB, so the PS5 link needs the same wire protocol for folders: the smblink module (tools/smblink) fetches
`<base><path>` with HTTP Range and only needs that URL shape and 206/Content-Range.

Read-only by construction: files are opened 'rb' and read with os.pread; the only write is the log line on stdout. A folder is a direct
child directory of <root>; an inner path may never leave the folder (no '..', no absolute path, a symlink that resolves outside the
folder is refused with 404). Stdlib only (runs on mac1's system Python 3.9).
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = 1 << 20
QUIET = False          # tests set this; the server itself logs one line per file request

# smb-link-6 22 Oct 2026: honest stutter signal for ps5-playwatch. Tag any served range >= SLOW_MS with SLOW, and print a rolling
# p95 of the last P95_N request times every P95_EVERY s, so a streamed game that starves on a camera pan is visible without parsing every line.
import collections as _col, threading as _th
SLOW_MS = int(os.environ.get("DIRSERVE_SLOW_MS", "120"))
P95_N = 512
P95_EVERY = 10.0
_lat = _col.deque(maxlen=P95_N)
_lat_lock = _th.Lock()
_last_p95 = [0.0]
def _note_latency(ms):
    now = time.time()
    with _lat_lock:
        _lat.append(ms)
        if now - _last_p95[0] >= P95_EVERY and len(_lat) >= 20:
            _last_p95[0] = now
            xs = sorted(_lat)
            p50 = xs[len(xs)//2]; p95 = xs[int(len(xs)*0.95)]; mx = xs[-1]
            print("%s P95 last %d reqs: p50 %d ms p95 %d ms max %d ms" % (time.strftime("%H:%M:%S"), len(xs), p50, p95, mx), flush=True)
ROOT = ""
ROOTS = []
DEPTH = 4
_LISTS = {}
_LISTS_LOCK = threading.Lock()
_INDEX = {"t": 0.0, "games": []}          # games: [{"tid","version","path"}]
_INDEX_LOCK = threading.Lock()
_SKIP_DIRS = {"decrypted", "fakelib", "fakelib2", "backports", "PS5-BACKPORTS", "$RECYCLE.BIN", "System Volume Information", ".Spotlight-V100",
              ".fseventsd", ".Trashes"}


def scan(force=False):
    """(Re)build the title-id index of game folders under ROOTS. At most every 15 s unless force."""
    with _INDEX_LOCK:
        if not force and time.time() - _INDEX["t"] < 15 and _INDEX["games"]:
            return False
        _INDEX["t"] = time.time()
    games, seen = [], set()
    for root in ROOTS:
        base_depth = root.rstrip(os.sep).count(os.sep)
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = sorted(d for d in dirnames if d not in _SKIP_DIRS and not d.startswith("."))
            if "eboot.bin" in filenames and os.path.isfile(os.path.join(dirpath, "sce_sys", "param.json")):
                real = os.path.realpath(dirpath)
                if real not in seen:                 # roots may overlap (8 TB/homebrew and the 8 TB itself)
                    seen.add(real)
                    try:
                        with open(os.path.join(dirpath, "sce_sys", "param.json"), "rb") as fh:
                            d = json.loads(fh.read())
                        games.append({"tid": d.get("titleId"), "version": d.get("contentVersion"), "path": dirpath})
                    except (OSError, ValueError):
                        pass
                dirnames[:] = []                 # never look inside a game for another game
                continue
            if dirpath.count(os.sep) - base_depth >= DEPTH:
                dirnames[:] = []
    with _INDEX_LOCK:
        _INDEX["games"] = games
    if not QUIET:
        print("%s scan: %d game folders under %d root(s)" % (time.strftime("%H:%M:%S"), len(games), len(ROOTS)), flush=True)
    return True


def title_dir(key):
    """Game folder for '<TID>' or '<TID>@<version>', or (None, reason)."""
    tid, _, ver = key.partition("@")
    for attempt in (0, 1):
        with _INDEX_LOCK:
            c = [g for g in _INDEX["games"] if (g["tid"] or "").upper() == tid.upper() and (not ver or g["version"] == ver) and os.path.isdir(g["path"])]
        if len(c) == 1:
            return c[0]["path"], ""
        if len(c) > 1:
            return None, "ambiguous: %d folders of %s (add @<version>): %s" % (len(c), tid, ", ".join("%s@%s" % (g["path"], g["version"]) for g in c))
        if attempt == 0 and not scan():
            break
    return None, "no game folder of " + key


def folder_dir(name):
    """Absolute path of a direct child folder of ROOT, or None."""
    if not name or "/" in name or "\\" in name or name in (".", "..") or "\0" in name:
        return None
    for root in ROOTS or [ROOT]:
        p = os.path.join(root, name)
        if os.path.isdir(p) and os.path.realpath(p) == os.path.join(os.path.realpath(root), name):
            return p
    return None


def resolve_inner(folder_path, inner):
    """Absolute path of a regular file inside folder_path, or None. Never leaves the folder."""
    inner = inner.replace("\\", "/").strip("/")
    if not inner or "\0" in inner:
        return None
    parts = inner.split("/")
    if any(p in ("", ".", "..") for p in parts):
        return None
    full = os.path.join(folder_path, *parts)
    real = os.path.realpath(full)
    base = os.path.realpath(folder_path)
    if not (real == base or real.startswith(base + os.sep)):
        return None
    return real if os.path.isfile(real) else None


def file_list(folder, folder_path):
    with _LISTS_LOCK:
        hit = _LISTS.get(folder)
        if hit and time.time() - hit[0] < 30:
            return hit[1]
    out = []
    base = os.path.realpath(folder_path)
    for dirpath, dirnames, filenames in os.walk(base, followlinks=False):
        dirnames.sort()
        for fn in sorted(filenames):
            fp = os.path.join(dirpath, fn)
            try:
                st = os.stat(fp)
            except OSError:
                continue
            real = os.path.realpath(fp)
            if os.path.isfile(fp) and (real == base or real.startswith(base + os.sep)):   # a symlink out of the folder is not served, so not listed
                out.append({"path": os.path.relpath(fp, base).replace(os.sep, "/"), "size": st.st_size})
    with _LISTS_LOCK:
        _LISTS[folder] = (time.time(), out)
    return out


def parse_range(header, size):
    """-> (start, end, status) with end inclusive; status 200 (no/ignored header), 206, or 416."""
    if not header or not header.lower().startswith("bytes="):
        return 0, size - 1, 200
    spec = header[6:].split(",")[0].strip()
    dash = spec.find("-")
    if dash < 0:
        return 0, 0, 416
    sa, sb = spec[:dash], spec[dash + 1:]
    try:
        if sa == "":
            n = int(sb)
            if n <= 0:
                return 0, 0, 416
            return max(0, size - n), size - 1, 206
        a = int(sa)
        b = size - 1 if sb == "" else min(int(sb), size - 1)
    except ValueError:
        return 0, 0, 416
    if a < 0 or a >= size or b < a:
        return 0, 0, 416
    return a, b, 206


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dirserve/1"

    def log_message(self, fmt, *args):      # one explicit line per file request instead (see _serve)
        pass

    def _send(self, status, body=b"", ctype="text/plain", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def do_HEAD(self):
        self._serve()

    def do_GET(self):
        self._serve()

    def _serve(self):
        t0 = time.time()
        url = urllib.parse.urlsplit(self.path)
        path = urllib.parse.unquote(url.path)
        query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
        if path == "/":
            with _INDEX_LOCK:
                games = list(_INDEX["games"])
            names = sorted({n for r in ROOTS for n in os.listdir(r) if folder_dir(n)})
            self._send(200, ("dirserve: GET /d/<folder>/<inner path> or /t/<TITLEID>[@<version>]/<inner path> with Range (read-only)\nroots: %s\n"
                             "folders:\n" % " | ".join(ROOTS) + "".join("  %s\n" % n for n in names) +
                             "games (by title id):\n" + "".join("  %s %s  %s\n" % (g["tid"], g["version"], g["path"]) for g in games)).encode())
            return
        if path.startswith("/t/"):
            folder, _, inner = path[3:].partition("/")
            fpath, why = title_dir(folder)
            if fpath is None:
                if not QUIET:
                    print("%s /t/%s: %s" % (time.strftime("%H:%M:%S"), folder, why), flush=True)
                self._send(404)
                return
        elif path.startswith("/d/"):
            folder, _, inner = path[3:].partition("/")
            fpath = folder_dir(folder)
            if fpath is None:
                self._send(404)
                return
        else:
            self._send(404)
            return
        if inner.strip("/") == "" or "list" in query:
            self._send(200, json.dumps(file_list(fpath, fpath)).encode(), "application/json")
            return
        real = resolve_inner(fpath, inner)
        if real is None:
            if not QUIET:
                print("%s 404 %s/%s" % (time.strftime("%H:%M:%S"), folder, inner), flush=True)
            self._send(404)
            return
        size = os.path.getsize(real)
        start, end, status = parse_range(self.headers.get("Range"), size)
        if status == 416:
            self._send(416, b"", extra={"Content-Range": "bytes */%d" % size})
            return
        length = end - start + 1 if size else 0
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        sent = 0
        if self.command != "HEAD" and length:
            try:
                with open(real, "rb") as fh:
                    fd = fh.fileno()
                    while sent < length:
                        buf = os.pread(fd, min(CHUNK, length - sent), start + sent)
                        if not buf:
                            break
                        self.wfile.write(buf)
                        sent += len(buf)
            except (BrokenPipeError, ConnectionResetError):
                pass
        ms = int((time.time() - t0) * 1000)
        if status == 206:
            _note_latency(ms)
        if not QUIET:
            print("%s %s %s/%s range=%s -> %d %d B %d ms%s" % (time.strftime("%H:%M:%S"), self.command, folder, inner,
                  ("%d-%d" % (start, end)) if status == 206 else "-", status, sent, ms, " SLOW" if ms >= SLOW_MS else ""), flush=True)


def make_server(root, port=4452, bind="0.0.0.0", more_roots=(), depth=4):
    global ROOT, ROOTS, DEPTH
    ROOT = os.path.abspath(root)
    ROOTS = [ROOT] + [os.path.abspath(r) for r in more_roots if os.path.isdir(r) and os.path.abspath(r) != ROOT]
    DEPTH = depth
    scan(force=True)
    srv = ThreadingHTTPServer((bind, port), Handler)
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--port", type=int, default=4452)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--depth", type=int, default=4)
    a = ap.parse_args()
    roots = [r for r in a.roots if os.path.isdir(r)]
    if not roots:
        sys.exit("usage: dirserve.py <root> [<root> ...] [--port 4452] [--bind 0.0.0.0] [--depth 4]")
    srv = make_server(roots[0], a.port, a.bind, roots[1:], a.depth)
    print("dirserve: %s on http://%s:%d (read-only), %d game folders found" % (" | ".join(ROOTS), a.bind, a.port, len(_INDEX["games"])), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
