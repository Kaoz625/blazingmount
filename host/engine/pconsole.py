#!/usr/bin/env python3
"""pconsole.py - small, explicit PS5 helper for the SMB-link tests (stdlib only). READ functions are free; WRITE functions (put/delete/launch) need the console claim.

    pconsole.py HOST ps                       process list from shsrv :2323
    pconsole.py HOST ls /path                 websrv listing (json)
    pconsole.py HOST cat /path [start]        file bytes via websrv /fs (Range)
    pconsole.py HOST put LOCAL /remote        FTP STOR to <remote>.tmp, rename over, sha256 read-back through websrv
    pconsole.py HOST rm /remote               FTP delete
    pconsole.py HOST mkd /remote/dir          FTP MKD (ok if it exists)
    pconsole.py HOST klog SECONDS OUTFILE     capture klog :3232 for N seconds
    pconsole.py HOST launch PPSAxxxxx         appmove.elf launch through websrv elfldr (NEVER used to close a game)
    pconsole.py HOST stub SRC_DIR /remote/dir REL[,REL...] [--exclude PREFIX ...] [--dry-run]
                                              push a game folder as a STUB: every file is copied (FTP to <name>.tmp, rename, sha256 read-back) except the
                                              listed REL paths, which become 0-byte placeholders (the SMB link serves them), and the --exclude prefixes.
                                              Run it where SRC_DIR is local (mac1: ssh mac1 'python3 - HOST stub ...' < pconsole.py). Python 3.9-safe.
                                              Re-runs skip files already there at the right size (a final name only exists after a complete STOR + rename).
"""
import ftplib
import hashlib
import io
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class Console:
    def __init__(self, host):
        self.host = host

    def _http(self, path, timeout=20, rng=None):
        req = urllib.request.Request("http://%s:8080%s" % (self.host, path))
        if rng:
            req.add_header("Range", rng)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()

    def cat(self, path, start=0):
        return self._http("/fs" + urllib.parse.quote(path), rng=("bytes=%d-" % start) if start else None)

    def ls(self, path):
        d = json.loads(self._http("/fs" + urllib.parse.quote(path) + "/?fmt=json"))
        return [e for e in d if e.get("name") not in (".", "..")]

    def _ftp(self):
        f = ftplib.FTP()
        f.connect(self.host, 2121, timeout=60)
        f.login()
        f.set_pasv(True)
        return f

    def put(self, data, remote):
        f = self._ftp()
        try:
            tmp = remote + ".tmp"
            f.storbinary("STOR " + tmp, io.BytesIO(data))
            f.rename(tmp, remote)
        finally:
            f.quit()
        got = self.cat(remote)
        if hashlib.sha256(got).digest() != hashlib.sha256(data).digest():
            raise RuntimeError("read-back differs for " + remote)

    def put_file(self, local, remote, ftp):
        """Stream one local file to <remote>.tmp, rename over, then compare sha256 of a streamed websrv read-back."""
        h = hashlib.sha256()
        with open(local, "rb") as fh:
            tmp = remote + ".tmp"
            ftp.storbinary("STOR " + tmp, fh, blocksize=1 << 20, callback=h.update)
            ftp.rename(tmp, remote)
        want = h.digest()
        req = urllib.request.Request("http://%s:8080/fs%s" % (self.host, urllib.parse.quote(remote)))
        g = hashlib.sha256()
        with urllib.request.urlopen(req, timeout=120) as r:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                g.update(b)
        if g.digest() != want:
            raise RuntimeError("read-back differs for " + remote)

    def remote_sizes(self, remote_dir):
        """{name: size} of the files directly in remote_dir ({} if it does not exist)."""
        try:
            return {e["name"]: int(e.get("size", -1)) for e in self.ls(remote_dir) if e.get("mode") == "-"}   # websrv: mode d/m = dir, - = file
        except (urllib.error.URLError, ValueError, OSError):
            return {}

    def stub(self, src, dst, remote_rels, excludes, dry):
        """Push folder src to dst on the console; remote_rels become 0-byte placeholders. Returns (files, bytes copied, placeholders)."""
        files, copied, holders, made = 0, 0, 0, set()
        ftp = None if dry else self._ftp()
        try:
            for d, dn, fn in os.walk(src):
                dn.sort()
                rel_dir = os.path.relpath(d, src).replace(os.sep, "/")
                rel_dir = "" if rel_dir == "." else rel_dir
                if any((rel_dir + "/").startswith(x.rstrip("/") + "/") for x in excludes if rel_dir):
                    dn[:] = []
                    continue
                rdir = dst.rstrip("/") + ("/" + rel_dir if rel_dir else "")
                if not dry and rdir not in made:
                    parts, cur = rdir.strip("/").split("/"), ""
                    for p in parts:
                        cur += "/" + p
                        if cur not in made:
                            try:
                                ftp.mkd(cur)
                            except ftplib.error_perm:
                                pass
                            made.add(cur)
                have = {} if dry else self.remote_sizes(rdir)
                for name in sorted(fn):
                    rel = (rel_dir + "/" + name) if rel_dir else name
                    if any(rel == x or rel.startswith(x.rstrip("/") + "/") for x in excludes):
                        continue
                    files += 1
                    local = os.path.join(d, name)
                    size = os.path.getsize(local)
                    target = rdir + "/" + name
                    if rel in remote_rels:
                        holders += 1
                        print("placeholder %s (real size %d)" % (rel, size), flush=True)
                        if not dry and have.get(name) != 0:
                            ftp.storbinary("STOR " + target, io.BytesIO(b""))
                        continue
                    if dry:
                        print("copy %s %d" % (rel, size))
                        copied += size
                        continue
                    if have.get(name) == size:
                        print("skip (already there) %s" % rel, flush=True)
                        continue
                    t0 = time.time()
                    self.put_file(local, target, ftp)
                    copied += size
                    print("copied %s %d B %.1f s" % (rel, size, time.time() - t0), flush=True)
        finally:
            if ftp is not None:
                ftp.quit()
        missing = sorted(set(remote_rels) - set(self._walk_rels(src)))
        if missing:
            print("WARNING: placeholder paths not in the source folder:", ", ".join(missing))
        return files, copied, holders

    @staticmethod
    def _walk_rels(src):
        for d, _dn, fn in os.walk(src):
            for name in fn:
                yield os.path.relpath(os.path.join(d, name), src).replace(os.sep, "/")

    def mkd(self, remote):
        f = self._ftp()
        try:
            try:
                f.mkd(remote)
            except ftplib.error_perm:
                pass
        finally:
            f.quit()

    def rm(self, remote):
        f = self._ftp()
        try:
            f.delete(remote)
        finally:
            f.quit()

    def ps(self):
        s = socket.create_connection((self.host, 2323), timeout=8)
        s.settimeout(3)
        time.sleep(0.3)
        try:
            s.recv(4096)
        except OSError:
            pass
        s.sendall(b"ps\n")
        buf, end = b"", time.time() + 8
        while time.time() < end:
            try:
                b = s.recv(65536)
            except socket.timeout:
                if buf:
                    break
                continue
            if not b:
                break
            buf += b
        s.close()
        return buf.decode("utf-8", "replace")

    def klog(self, secs, out):
        s = socket.create_connection((self.host, 3232), timeout=8)
        s.settimeout(1)
        end = time.time() + secs
        n = 0
        with open(out, "wb") as fh:
            while time.time() < end:
                try:
                    b = s.recv(65536)
                except socket.timeout:
                    continue
                if not b:
                    break
                fh.write(b)
                fh.flush()
                n += len(b)
        s.close()
        return n

    def launch(self, tid):
        return self._http("/elfldr?elf=/data/blazing/appmove.elf&args=appmove.elf%%20launch%%20%s&pipe=0" % tid, timeout=30).decode("utf-8", "replace")


def main():
    host, cmd, a = sys.argv[1], sys.argv[2], sys.argv[3:]
    c = Console(host)
    if cmd == "ps":
        print(c.ps())
    elif cmd == "ls":
        for e in c.ls(a[0]):
            print(e)
    elif cmd == "cat":
        sys.stdout.buffer.write(c.cat(a[0], int(a[1]) if len(a) > 1 else 0))
    elif cmd == "put":
        c.put(open(a[0], "rb").read(), a[1])
        print("put ok", a[1])
    elif cmd == "rm":
        c.rm(a[0])
        print("deleted", a[0])
    elif cmd == "mkd":
        c.mkd(a[0])
        print("mkd ok", a[0])
    elif cmd == "klog":
        print(c.klog(int(a[0]), a[1]), "bytes")
    elif cmd == "launch":
        print(c.launch(a[0]))
    elif cmd == "stub":
        src, dst, rels = a[0], a[1], set(x for x in a[2].split(",") if x)
        excludes = [a[i + 1] for i in range(3, len(a) - 1) if a[i] == "--exclude"]
        dry = "--dry-run" in a
        if not (dst.startswith("/mnt/ext1/homebrew/") or dst.startswith("/mnt/ext1/blz-stage/")):    # blz-stage: same M.2, not scanned (swap in by one rename)
            sys.exit("stub: the destination must be under /mnt/ext1/homebrew/ or /mnt/ext1/blz-stage/ (the M.2; never the internal SSD)")
        n, b, h = c.stub(src, dst, rels, excludes, dry)
        print("stub %s: %d files, %d B copied, %d placeholders%s" % (dst, n, b, h, " (dry run)" if dry else ""))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
