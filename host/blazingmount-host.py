#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""blazingmount-host.py - build the catalog + starter bundles the on-console daemon (blazingmountd) pulls.

For each game you choose on your host, this builds a native streaming starter (via
blazingmount-starter.py), packs it into a .tar bundle, and writes a catalog.json. Point
blazingmountd at this catalog and the console installs + registers every game by itself -
no per-game step, no computer driving the console.

Run ON the host (the machine with your drives + imgserve/dirserve).

    python3 host/blazingmount-host.py build OUT --host-ip 192.168.1.50 --console-fw 13.60 \
        --game "The Mound - Omen of Cthulhu (PPSA26223).ffpfsc" \
        --game "Street Fighter 6 (PPSA02632).ffpfsc"

    python3 host/blazingmount-host.py build OUT ... --merge       # add/update these games, KEEP the others
    python3 host/blazingmount-host.py serve OUT --port 4460      # tiny static server for catalog + bundles

`build` without --merge writes a catalog of exactly the games named in THAT call (what it has always
done). With --merge the games already in OUT/catalog.json stay: each named game is added, or replaced
when its title id is already there. The catalog is read and written under a lock and every file is
renamed into place, so a console polling mid-build never sees half a file and two builds never lose
each other's rows.

OUT/
  catalog.json                 { "games": [ {tid, name, fw, bundle, cfg} ... ] }
  bundles/<TID>.starter.tar    the starter folder (small files + fork fakelib + ampr_emu.index)
  cfg/<TID>.cfg                the link config (server already set to --host-ip)
"""
import argparse, contextlib, json, os, re, shutil, subprocess, sys, tarfile, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
STARTER_BUILDER = os.path.join(HERE, "blazingmount-starter.py")
DEFAULT_FORK = os.path.join(HERE, "..", "console", "link-fork", "prebuilt", "libSceAmpr.sprx")


def build_one(game, host_ip, console_fw, fork, out):
    work = tempfile.mkdtemp(prefix="bm-host-")
    try:
        subprocess.run([sys.executable, "-I", STARTER_BUILDER, game, work, "--fork", fork,
                        "--console-fw", str(console_fw)], check=True)
        starters = os.path.join(work, "starter")
        name = os.listdir(starters)[0]
        tid = re.search(r"\(([A-Z]{4}\d{5})\)", name).group(1)
        cfg_src = os.path.join(work, tid + ".cfg")
        # point the cfg at this host
        cfg_txt = re.sub(r"(?m)^server\s+\S+\s+(\d+)\s*$", r"server %s \1" % host_ip, open(cfg_src).read())
        os.makedirs(os.path.join(out, "cfg"), exist_ok=True)
        tmp_cfg = os.path.join(out, "cfg", tid + ".cfg.tmp-%d" % os.getpid())
        open(tmp_cfg, "w").write(cfg_txt)
        os.replace(tmp_cfg, os.path.join(out, "cfg", tid + ".cfg"))
        # tar the starter folder
        os.makedirs(os.path.join(out, "bundles"), exist_ok=True)
        bundle = os.path.join("bundles", tid + ".starter.tar")
        tmp_tar = os.path.join(out, bundle + ".tmp-%d" % os.getpid())
        with tarfile.open(tmp_tar, "w") as t:
            t.add(os.path.join(starters, name), arcname=name)
        os.replace(tmp_tar, os.path.join(out, bundle))        # a console fetching now gets the old or the new, never half
        info = json.load(open(os.path.join(work, tid + ".json")))
        return {"tid": tid, "name": name, "fw": info.get("fw"), "bundle": bundle, "cfg": "cfg/%s.cfg" % tid,
                "starter_bytes": info.get("starter_bytes"), "remote_bytes": info.get("remote_bytes")}
    finally:
        shutil.rmtree(work, ignore_errors=True)


@contextlib.contextmanager
def catalog_lock(out):
    """Held while the catalog is read-modified-written, so two builds cannot lose each other's rows.
    POSIX flock; on a platform without fcntl the lock is skipped (single builder assumed)."""
    try:
        import fcntl
    except ImportError:
        yield
        return
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, ".catalog.lock"), "w") as fh:
        fcntl.flock(fh, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(fh, fcntl.LOCK_UN)


def read_catalog(out):
    """The games already in OUT/catalog.json ([] when there is none or it is unreadable)."""
    try:
        with open(os.path.join(out, "catalog.json")) as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return []
    games = d.get("games") if isinstance(d, dict) else None
    return [g for g in (games or []) if isinstance(g, dict) and g.get("tid")]


def write_catalog(out, host_ip, games):
    """catalog.json + catalog.txt, each written beside itself and renamed into place."""
    j = os.path.join(out, "catalog.json")
    tmp = j + ".tmp-%d" % os.getpid()
    with open(tmp, "w") as fh:
        json.dump({"version": 1, "host_ip": host_ip, "games": games}, fh, indent=1)
    os.replace(tmp, j)
    # catalog.txt is what blazingmountd reads (tab-separated, no JSON parser needed on the console)
    t = os.path.join(out, "catalog.txt")
    tmp = t + ".tmp-%d" % os.getpid()
    with open(tmp, "w") as fh:
        fh.write("# TID\tNAME\tBUNDLE\tCFG  (BlazingMount catalog for blazingmountd)\n")
        for g in games:
            fh.write("%s\t%s\t%s\t%s\n" % (g["tid"], g["name"], g["bundle"], g["cfg"]))
    os.replace(tmp, t)


def merge_games(existing, built):
    """existing + built, keyed by title id: a built game replaces its old row in place, a new one is appended."""
    out = [dict(g) for g in existing]
    index = {g["tid"]: i for i, g in enumerate(out)}
    for g in built:
        if g["tid"] in index:
            out[index[g["tid"]]] = g
        else:
            index[g["tid"]] = len(out)
            out.append(g)
    return out


def cmd_build(a):
    os.makedirs(a.out, exist_ok=True)
    built = []
    for g in a.game:
        print("=== building", g)
        built.append(build_one(g, a.host_ip, a.console_fw, a.fork, a.out))
    with catalog_lock(a.out):
        games = merge_games(read_catalog(a.out), built) if a.merge else built
        write_catalog(a.out, a.host_ip, games)
    print("catalog.json + catalog.txt written with %d game(s): %s" % (len(games), ", ".join(x["name"] for x in games)))


def cmd_serve(a):
    import http.server, socketserver
    os.chdir(a.out)
    with socketserver.TCPServer(("", a.port), http.server.SimpleHTTPRequestHandler) as s:
        print("serving %s on :%d (catalog at /catalog.json)" % (a.out, a.port))
        s.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("out")
    b.add_argument("--host-ip", required=True)
    b.add_argument("--console-fw", type=float, required=True)
    b.add_argument("--game", action="append", required=True)
    b.add_argument("--fork", default=DEFAULT_FORK)
    b.add_argument("--merge", action="store_true",
                   help="keep the games already in OUT/catalog.json; add or replace only the ones named here")
    b.set_defaults(func=cmd_build)
    s = sub.add_parser("serve")
    s.add_argument("out")
    s.add_argument("--port", type=int, default=4460)
    s.set_defaults(func=cmd_serve)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
