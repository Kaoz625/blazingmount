#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""blazingmount-install.py - mount ONE game from your host onto a jailbroken PS5, reboot-surviving.

This is the proven, host-assisted path: it builds a native streaming starter from a game on your
host, pushes it to the console (every file verified by sha256), writes the link config, and lets
ShadowMount Plus register the tile. The game then plays streamed from your host. Nothing stays
"live" - after this runs, the console keeps the tile across reboots and the host just answers file
reads (your network drive).

Run ON the host (the machine that holds your drives and runs imgserve/dirserve).

    python3 install/blazingmount-install.py \
        --console 192.168.1.60 --host-ip 192.168.1.50 \
        --game "The Mound - Omen of Cthulhu (PPSA26223).ffpfsc" \
        --console-fw 13.60

Options:
    --console IP        the PS5's LAN address (ftpsrv :2121 + websrv :8080 must be up)
    --host-ip IP        the host's LAN address the PS5 will stream from (imgserve/dirserve)
    --game SRC          an image name (served by imgserve) or a folder (served by dirserve)
    --console-fw X.YZ   the console firmware; the builder REFUSES a game that needs newer FW
    --fork PATH         libSceAmpr.sprx link fork (default: ../console/link-fork/prebuilt/libSceAmpr.sprx)
    --homebrew-dir D    where tiles live on the console (default /data/homebrew)
    --stage-dir D       staging dir on the console (default /data/blz-stage/incoming)
    --ftp-port N        console FTP port (default 2121)
    --keep              keep the built starter dir instead of deleting it
"""
import argparse, ftplib, hashlib, os, re, shutil, subprocess, sys, tempfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
STARTER_BUILDER = os.path.join(HERE, "..", "host", "blazingmount-starter.py")
DEFAULT_FORK = os.path.join(HERE, "..", "console", "link-fork", "prebuilt", "libSceAmpr.sprx")


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def websrv_sha256(console, remote):
    url = "http://%s:8080/fs%s" % (console, remote)
    h = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=120) as r:
        for b in iter(lambda: r.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def ftp_connect(console, port):
    ftp = ftplib.FTP()
    ftp.connect(console, port, timeout=30)
    ftp.login()
    return ftp


def ftp_makedirs(ftp, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass  # already exists


def ftp_put(ftp, local, remote):
    ftp_makedirs(ftp, os.path.dirname(remote))
    with open(local, "rb") as f:
        ftp.storbinary("STOR " + remote, f)


def build_starter(game, out, fork, console_fw):
    cmd = [sys.executable, "-I", STARTER_BUILDER, game, out, "--fork", fork, "--console-fw", str(console_fw)]
    subprocess.run(cmd, check=True)
    starters = os.path.join(out, "starter")
    name = os.listdir(starters)[0]
    cfg = next(f for f in os.listdir(out) if f.endswith(".cfg"))
    return os.path.join(starters, name), name, os.path.join(out, cfg)


def patch_cfg_server(cfg_path, host_ip):
    txt = open(cfg_path).read()
    txt = re.sub(r"(?m)^server\s+\S+\s+(\d+)\s*$", r"server %s \1" % host_ip, txt)
    open(cfg_path, "w").write(txt)
    return txt


def push_tree(console, port, local_root, stage_root, final_root):
    """Mirror local_root -> console stage_root (sha-verified over websrv), then rename to final_root."""
    ftp = ftp_connect(console, port)
    files = []
    for dp, _, fn in os.walk(local_root):
        for n in fn:
            lp = os.path.join(dp, n)
            rel = os.path.relpath(lp, local_root).replace(os.sep, "/")
            files.append((lp, rel))
    print("pushing %d files (%d MiB) -> %s" % (len(files), sum(os.path.getsize(p) for p, _ in files) >> 20, stage_root))
    for lp, rel in sorted(files, key=lambda x: x[1]):
        remote = stage_root + "/" + rel
        ftp_put(ftp, lp, remote)
        want = sha256_file(lp)
        got = websrv_sha256(console, remote)
        if want != got:
            sys.exit("VERIFY FAILED %s: %s != %s" % (rel, want[:12], got[:12]))
    # atomic swap into place
    ftp_makedirs(ftp, os.path.dirname(final_root))
    try:
        ftp.rename(stage_root, final_root)
    except ftplib.error_perm as e:
        sys.exit("rename %s -> %s failed: %s (does the target already exist?)" % (stage_root, final_root, e))
    ftp.quit()
    print("placed, every file read back by sha256: %s" % final_root)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--console", required=True)
    ap.add_argument("--host-ip", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--console-fw", type=float, required=True)
    ap.add_argument("--fork", default=DEFAULT_FORK)
    ap.add_argument("--homebrew-dir", default="/data/homebrew")
    ap.add_argument("--stage-dir", default="/data/blz-stage/incoming")
    ap.add_argument("--ftp-port", type=int, default=2121)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    out = tempfile.mkdtemp(prefix="blazingmount-")
    try:
        starter_dir, name, cfg_path = build_starter(a.game, out, a.fork, a.console_fw)
        tid = re.search(r"\(([A-Z]{4}\d{5})\)", name).group(1)
        patch_cfg_server(cfg_path, a.host_ip)
        print("starter: %s   TID: %s" % (name, tid))

        push_tree(a.console, a.ftp_port, starter_dir,
                  a.stage_dir.rstrip("/") + "/" + name,
                  a.homebrew_dir.rstrip("/") + "/" + name)

        ftp = ftp_connect(a.console, a.ftp_port)
        ftp_makedirs(ftp, "/data/blazing/link")
        ftp_put(ftp, cfg_path, "/data/blazing/link/%s.cfg" % tid)
        ftp.quit()
        print("link config: /data/blazing/link/%s.cfg (streaming from %s)" % (tid, a.host_ip))
        print("\nDONE. '%s' should appear on the home screen within ~15 s (ShadowMount Plus)." % name)
        print("Launch it: it streams from %s - keep imgserve/dirserve running there." % a.host_ip)
        print("Reboot-surviving: the starter, the fork and the link config all persist; ShadowMount re-registers the tile each boot.")
    finally:
        if not a.keep:
            shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
