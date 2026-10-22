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

    python3 host/blazingmount-host.py serve OUT --port 4460      # tiny static server for catalog + bundles

OUT/
  catalog.json                 { "games": [ {tid, name, fw, bundle, cfg} ... ] }
  bundles/<TID>.starter.tar    the starter folder (small files + fork fakelib + ampr_emu.index)
  cfg/<TID>.cfg                the link config (server already set to --host-ip)
"""
import argparse, json, os, re, shutil, subprocess, sys, tarfile, tempfile

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
        open(os.path.join(out, "cfg", tid + ".cfg"), "w").write(cfg_txt)
        # tar the starter folder
        os.makedirs(os.path.join(out, "bundles"), exist_ok=True)
        bundle = os.path.join("bundles", tid + ".starter.tar")
        with tarfile.open(os.path.join(out, bundle), "w") as t:
            t.add(os.path.join(starters, name), arcname=name)
        info = json.load(open(os.path.join(work, tid + ".json")))
        return {"tid": tid, "name": name, "fw": info.get("fw"), "bundle": bundle, "cfg": "cfg/%s.cfg" % tid,
                "starter_bytes": info.get("starter_bytes"), "remote_bytes": info.get("remote_bytes")}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def cmd_build(a):
    os.makedirs(a.out, exist_ok=True)
    games = []
    for g in a.game:
        print("=== building", g)
        games.append(build_one(g, a.host_ip, a.console_fw, a.fork, a.out))
    json.dump({"version": 1, "host_ip": a.host_ip, "games": games},
              open(os.path.join(a.out, "catalog.json"), "w"), indent=1)
    # catalog.txt is what blazingmountd reads (tab-separated, no JSON parser needed on the console)
    with open(os.path.join(a.out, "catalog.txt"), "w") as t:
        t.write("# TID\tNAME\tBUNDLE\tCFG  (BlazingMount catalog for blazingmountd)\n")
        for g in games:
            t.write("%s\t%s\t%s\t%s\n" % (g["tid"], g["name"], g["bundle"], g["cfg"]))
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
    b.set_defaults(func=cmd_build)
    s = sub.add_parser("serve")
    s.add_argument("out")
    s.add_argument("--port", type=int, default=4460)
    s.set_defaults(func=cmd_serve)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
