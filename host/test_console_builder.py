#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_console_builder.py - FAKE-CONSOLE test of blazingmountd's on-console starter builder (no console involved).

Runs the payload's own C code (console/blazingmountd, HOST=1 build) on the host against an imgserve that has the listing routes
(/games.txt, /f/<key>), lets it build starters into a temp "console" (BM_ROOT), and compares each starter with the one mkgame.py
builds for the same image: the same file set, the same sizes, the same bytes (sha256), and the same link config lines.

    python3 -I host/test_console_builder.py --imgserve http://127.0.0.1:4454 --bin console/blazingmountd/out/blazingmountd-host \
        --fork /path/libSceAmpr.sprx --key PPSA02632@58064175104 --key ... [--refuse PPSA23827@1219231744]

--key     a game both builders must build identically (pick games whose FW <= 10.20 so mkgame.py makes no backport)
--refuse  a game the payload must REFUSE (e.g. an eboot without libSceAmpr) and leave no folder behind
Needs (as mkgame.py check does) BLAZINGMOUNT_BACKPORT_TOOLS for decrypt_fself; BLAZINGMOUNT_IMGSERVE is set from --imgserve.
Exit 0 = every comparison identical.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MKGAME = os.path.join(HERE, "engine", "mkgame.py")


def tree(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for n in fn:
            p = os.path.join(dp, n)
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for b in iter(lambda: fh.read(1 << 20), b""):
                    h.update(b)
            out[os.path.relpath(p, root)] = (os.path.getsize(p), h.hexdigest())
    return out


def cfg_lines(path, host_port):
    """Link-config lines that must match: everything but the '#' comment; the server line is the one the builder was told to use."""
    lines = [l for l in open(path).read().splitlines() if l and not l.startswith("#")]
    return [("server " + host_port) if l.startswith("server ") else l for l in lines]


def games(imgserve):
    rows = {}
    for l in urllib.request.urlopen(imgserve + "/games.txt", timeout=600).read().decode().splitlines():
        if l and not l.startswith("#"):
            f = l.split("\t")
            rows[f[0]] = {"key": f[0], "tid": f[1], "size": int(f[2]), "version": f[3], "fw": f[4], "title": f[6]}
    return rows


def run_payload(binary, root, imgserve, keys):
    u = urllib.parse.urlparse(imgserve)
    os.makedirs(os.path.join(root, "data", "blazing"), exist_ok=True)
    with open(os.path.join(root, "data", "blazing", "mount.conf"), "w") as fh:
        fh.write("host %s\nport %d\nmax_starter_mb 0\nmin_free_mb 0\n" % (u.hostname, u.port))
        for k in keys:
            fh.write("only %s\n" % k)
    r = subprocess.run([binary, "once"], env=dict(os.environ, BM_ROOT=root), capture_output=True, text=True, timeout=7200)
    log = open(os.path.join(root, "data", "blazing", "blazingmountd.log")).read()
    return r.returncode, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--imgserve", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--fork", required=True)
    ap.add_argument("--key", action="append", default=[])
    ap.add_argument("--refuse", action="append", default=[])
    ap.add_argument("--keep", action="store_true", help="keep the temp trees for a look")
    ap.add_argument("--mkgame", default=MKGAME, help="the mkgame.py that builds the reference starter (default host/engine/mkgame.py;"
                    " on mac1 the proven ~/.blazing/smblink/tools/mkgame.py, which reads the production imgserve)")
    a = ap.parse_args()
    os.environ["BLAZINGMOUNT_IMGSERVE"] = a.imgserve
    u = urllib.parse.urlparse(a.imgserve)
    host_port = "%s %d" % (u.hostname, u.port)
    rows = games(a.imgserve)
    work = tempfile.mkdtemp(prefix="bm-fake-console-")
    fails = 0
    try:
        console = os.path.join(work, "console")
        rc, log = run_payload(os.path.abspath(a.bin), console, a.imgserve, a.key + a.refuse)
        print("payload exit %d; log:\n%s" % (rc, "".join("   | " + l + "\n" for l in log.splitlines())))
        hb = os.path.join(console, "data", "homebrew")
        for key in a.key:
            g = rows[key]
            ref = os.path.join(work, "mkgame-" + g["tid"])
            src = [l for l in urllib.request.urlopen(a.imgserve + "/", timeout=120).read().decode().splitlines()
                   if re.search(r"  %d B  %s " % (g["size"], g["tid"]), l)][0].strip().split("  ")[0]
            subprocess.run([sys.executable, "-I", a.mkgame, "make", src, ref, "--fork", a.fork], check=True, stdout=subprocess.DEVNULL,
                           env=dict(os.environ))
            name = os.listdir(os.path.join(ref, "starter"))[0]
            want, got_dir = tree(os.path.join(ref, "starter", name)), os.path.join(hb, name)
            if not os.path.isdir(got_dir):
                print("FAIL %s: the payload built no folder %r" % (key, name)); fails += 1; continue
            got = tree(got_dir)
            only_ref = sorted(set(want) - set(got)); only_bm = sorted(set(got) - set(want))
            size_diff = sorted(p for p in set(want) & set(got) if want[p][0] != got[p][0])
            byte_diff = sorted(p for p in set(want) & set(got) if want[p] != got[p])
            cw = cfg_lines(os.path.join(ref, g["tid"] + ".cfg"), host_port)
            cg = cfg_lines(os.path.join(console, "data", "blazing", "link", g["tid"] + ".cfg"), host_port)
            ok = not (only_ref or only_bm or size_diff or byte_diff) and cw == cg
            n_ph = sum(1 for p in got if got[p][0] == 0)
            print("%s %s %r: %d files (%d placeholders, %.1f MB), mkgame %d files; only-mkgame %s only-payload %s size-diff %s byte-diff %s cfg %s"
                  % ("PASS" if ok else "FAIL", key, name, len(got), n_ph, sum(v[0] for v in got.values()) / 1e6, len(want),
                     only_ref[:3], only_bm[:3], size_diff[:3], byte_diff[:3], "same %d lines" % len(cg) if cw == cg else "DIFFERENT"))
            if cw != cg:
                for i, (x, y) in enumerate(zip(cw, cg)):
                    if x != y:
                        print("   cfg line %d: mkgame %r payload %r" % (i, x, y)); break
                print("   cfg lines mkgame %d payload %d" % (len(cw), len(cg)))
            fails += 0 if ok else 1
        for key in a.refuse:
            g = rows[key]
            there = [d for d in os.listdir(hb) if g["tid"] in d] if os.path.isdir(hb) else []
            st = open(os.path.join(console, "data", "blazing", "blazingmountd.state")).read()
            refused = any(l.startswith(key + "\trefused\t") for l in st.splitlines())
            stage = os.path.join(console, "data", "blz-stage", "bm")
            left = os.listdir(stage) if os.path.isdir(stage) else []
            ok = refused and not there and not left
            print("%s refuse %s: state refused=%s, homebrew folders %s, stage leftovers %s" % ("PASS" if ok else "FAIL", key, refused, there, left))
            fails += 0 if ok else 1
        # a second pass must build nothing and download nothing (no re-download loop)
        rc2, log2 = run_payload(os.path.abspath(a.bin), console, a.imgserve, a.key + a.refuse)
        last = [l for l in log2.splitlines() if " pass: " in l][-1]
        m = re.search(r"(\d+) built, (\d+) refused, (\d+) failed", last)
        ok = m and m.groups() == ("0", "0", "0")
        print("%s second pass: %s" % ("PASS" if ok else "FAIL", last.split(" ", 2)[2]))
        fails += 0 if ok else 1
    finally:
        if a.keep:
            print("kept", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAIL" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
