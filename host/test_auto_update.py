#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_auto_update.py - fake-console test of blazingmountd's auto-update (v2.5). No console involved.

    python3 -I host/test_auto_update.py --imgserve http://127.0.0.1:4454 --old-bin OLD --new-bin NEW --key PPSA02849@1291780096 \
        --new-fork NEW_FORK.sprx

OLD = a HOST=1 build with an older fork, NEW = the build under test. Checks:
  1. a starter OLD built gets NEW's fork + the managed link settings (conns, readahead_kb, proxy/server_ts), file lines unchanged
  2. while the game "runs" (/mnt/sandbox/<TID>_000 exists) nothing is touched; after it closes the update happens
  3. a pinned starter (.blazingmount-pin) and a FULL COPY (real bytes where the cfg streams) are never touched
  4. the image on the drive changed (the state's key is no longer listed): the starter is rebuilt and swapped, the old one is gone,
     the result equals a fresh build
  5. a second pass changes nothing
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile, urllib.parse

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

def tree(root):
    return {os.path.relpath(os.path.join(dp, n), root): sha(os.path.join(dp, n)) for dp, dn, fn in os.walk(root) for n in fn}

def run(binary, root):
    subprocess.run([binary, "once"], env=dict(os.environ, BM_ROOT=root), timeout=3600, check=False)
    return open(os.path.join(root, "data", "blazing", "blazingmountd.log")).read()

def main():
    ap = argparse.ArgumentParser()
    for k in ("--imgserve", "--old-bin", "--new-bin", "--key", "--new-fork"):
        ap.add_argument(k, required=True)
    a = ap.parse_args()
    u = urllib.parse.urlparse(a.imgserve)
    tid = a.key.split("@")[0]
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + what)
        fails += 0 if cond else 1
    work = tempfile.mkdtemp(prefix="bm-autoupd-")
    try:
        root = os.path.join(work, "console")
        os.makedirs(os.path.join(root, "data", "blazing"))
        conf = os.path.join(root, "data", "blazing", "mount.conf")
        open(conf, "w").write("host %s\nport %d\nmin_free_mb 0\ntsnet 0\nconns 4\nonly %s\n" % (u.hostname, u.port, tid))
        run(os.path.abspath(a.old_bin), root)
        hb = os.path.join(root, "data", "homebrew")
        name = os.listdir(hb)[0]
        sdir = os.path.join(hb, name)
        cfgp = os.path.join(root, "data", "blazing", "link", tid + ".cfg")
        old_cfg = open(cfgp).read()
        old_files = [l for l in old_cfg.splitlines() if l.startswith("file ")]
        check("conns 4" in old_cfg and sha(os.path.join(sdir, "fakelib", "libSceAmpr.sprx")) != sha(a.new_fork), "OLD build: conns 4, old fork")
        # a foreign FULL COPY of another title with our fork + a streaming cfg: must never be touched
        full = os.path.join(hb, "Full Copy (PPSA00001)")
        shutil.copytree(sdir, full)
        for l in old_files[:3]:
            rel = l.split(" ", 2)[2]
            open(os.path.join(full, rel), "wb").write(b"x" * 10)
        open(os.path.join(full, "sce_sys", "param.json"), "w").write('{"titleId":"PPSA00001"}')
        shutil.copy(cfgp, os.path.join(root, "data", "blazing", "link", "PPSA00001.cfg"))
        full_before = tree(full)
        # the game runs: no update
        sandbox = os.path.join(root, "mnt", "sandbox", tid + "_000")
        os.makedirs(sandbox)
        tsconf = os.path.join(root, "data", "tailscale")
        os.makedirs(tsconf)
        open(os.path.join(tsconf, "config.json"), "w").write('{"httpProxyAddr": "127.0.0.1:8888"}')
        open(conf, "w").write("host %s\nport %d\nmin_free_mb 0\nserver_ts 100.104.191.30 4451\nonly %s\nonly PPSA00001\n" % (u.hostname, u.port, tid))
        before = tree(sdir)
        log = run(os.path.abspath(a.new_bin), root)
        check(tree(sdir) == before and "waits: the game is running" in log, "running game: starter untouched, logged")
        os.rmdir(sandbox)
        log = run(os.path.abspath(a.new_bin), root)
        new_cfg = open(cfgp).read()
        check(sha(os.path.join(sdir, "fakelib", "libSceAmpr.sprx")) == sha(a.new_fork), "after close: fork replaced with NEW's")
        check(os.stat(os.path.join(sdir, "fakelib", "libSceAmpr.sprx")).st_mode & 0o111 != 0, "new fork keeps its execute bit")
        check("conns 8" in new_cfg and "conns 4" not in new_cfg and "\nproxy 127.0.0.1 8888\nserver_ts 100.104.191.30 4451\n" in new_cfg,
              "cfg: conns 8 + tsnet lines after server")
        check([l for l in new_cfg.splitlines() if l.startswith("file ")] == old_files, "cfg file lines unchanged (%d)" % len(old_files))
        check(tree(full) == full_before, "full copy with our fork untouched")
        # pinned starter
        pin = os.path.join(sdir, ".blazingmount-pin")
        open(pin, "w").close()
        shutil.copy(os.path.join(full, "fakelib", "libSceAmpr.sprx"), os.path.join(sdir, "fakelib", "libSceAmpr.sprx"))
        pinned = tree(sdir)
        run(os.path.abspath(a.new_bin), root)
        check(tree(sdir) == pinned, "pinned starter untouched")
        os.remove(pin)
        run(os.path.abspath(a.new_bin), root)
        # image changed: the state says this starter came from an image that is no longer listed
        st = os.path.join(root, "data", "blazing", "blazingmountd.state")
        txt = open(st).read().replace(a.key + "\tdone", tid + "@123\tdone")
        open(st, "w").write(txt)
        open(os.path.join(sdir, "stale-marker.txt"), "w").write("old starter")
        log = run(os.path.abspath(a.new_bin), root)
        check("image changed for %s" % tid in log and not os.path.exists(os.path.join(sdir, "stale-marker.txt")), "image change: rebuilt and swapped, old starter gone")
        fresh = os.path.join(work, "fresh")
        os.makedirs(os.path.join(fresh, "data", "blazing"))
        open(os.path.join(fresh, "data", "blazing", "mount.conf"), "w").write(
            "host %s\nport %d\nmin_free_mb 0\nserver_ts 100.104.191.30 4451\nproxy 127.0.0.1 8888\nonly %s\n" % (u.hostname, u.port, tid))
        run(os.path.abspath(a.new_bin), fresh)
        check(tree(sdir) == tree(os.path.join(fresh, "data", "homebrew", name)), "swapped starter == a fresh build")
        check(not [d for d in os.listdir(os.path.join(root, "data", "blz-stage", "bm"))] if os.path.isdir(os.path.join(root, "data", "blz-stage", "bm")) else True,
              "no stage / old leftovers")
        log = run(os.path.abspath(a.new_bin), root)
        last = [l for l in log.splitlines() if " pass: " in l][-1]
        check("(0 updated), 0 built" in last, "next pass changes nothing: " + last.split("pass: ")[1][:90])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAIL" % fails)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
