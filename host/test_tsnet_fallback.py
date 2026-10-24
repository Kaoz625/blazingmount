#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_tsnet_fallback.py - away-from-home test of blazingmountd (no console).

A PS5's tailscale is a tsnet payload, not a system VPN: away from home the console reaches the host only through tsnet's local HTTP
proxy (config.json "httpProxyAddr"). This runs the payload's own C code (HOST=1 build) twice for the same game:
  1. at home: LAN server up                          -> the reference starter
  2. away:    LAN server dead, a forward proxy given -> must build the SAME starter through the proxy (absolute-form requests only),
             log the route switch, and write the proxy/server_ts lines into the link cfg
The proxy here refuses origin-form requests, so a pass proves every download went through it. tsnet is found the way a console finds
it: a fake /data/tailscale/config.json with httpProxyAddr (no proxy line in mount.conf).

    python3 -I host/test_tsnet_fallback.py --imgserve http://127.0.0.1:4454 --bin console/blazingmountd/out/blazingmountd-host --key PPSA02849@1291780096
"""
import argparse, hashlib, os, shutil, socket, subprocess, sys, tempfile, threading, urllib.parse

STATS = {"abs": 0, "refused": 0}


def run_proxy(target_host, target_port):
    """HTTP/1.0-style forward proxy: absolute-form GET only; streams the upstream reply until it closes."""
    def handle(cs):
        try:
            f = cs.makefile("rb")
            line = f.readline().decode()
            hdrs = []
            while True:
                h = f.readline()
                if h in (b"\r\n", b"\n", b""):
                    break
                hdrs.append(h)
            parts = line.split()
            if len(parts) != 3 or not parts[1].startswith("http://"):
                STATS["refused"] += 1
                cs.sendall(b"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                return
            STATS["abs"] += 1
            u = urllib.parse.urlsplit(parts[1])
            up = socket.create_connection((target_host, target_port))
            path = u.path + ("?" + u.query if u.query else "")
            up.sendall(("GET %s HTTP/1.0\r\n" % path).encode() + b"".join(h for h in hdrs if not h.lower().startswith((b"proxy-", b"connection"))) +
                       b"Connection: close\r\n\r\n")
            while True:
                b = up.recv(1 << 16)
                if not b:
                    break
                cs.sendall(b)
            up.close()
        except OSError:
            pass
        finally:
            cs.close()

    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(32)

    def loop():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=handle, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return ls.getsockname()[1]


def tree(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for n in fn:
            p = os.path.join(dp, n)
            out[os.path.relpath(p, root)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out


def run(binary, root, conf, tsconf=None):
    os.makedirs(os.path.join(root, "data", "blazing"), exist_ok=True)
    open(os.path.join(root, "data", "blazing", "mount.conf"), "w").write(conf)
    if tsconf:
        os.makedirs(os.path.join(root, "data", "tailscale"), exist_ok=True)
        open(os.path.join(root, "data", "tailscale", "config.json"), "w").write(tsconf)
    subprocess.run([binary, "once"], env=dict(os.environ, BM_ROOT=root), timeout=3600)
    return open(os.path.join(root, "data", "blazing", "blazingmountd.log")).read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--imgserve", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--key", required=True)
    a = ap.parse_args()
    u = urllib.parse.urlparse(a.imgserve)
    binary = os.path.abspath(a.bin)
    pport = run_proxy(u.hostname, u.port)
    work = tempfile.mkdtemp(prefix="bm-tsnet-")
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + what)
        fails += 0 if cond else 1
    try:
        home = os.path.join(work, "home")
        run(binary, home, "host %s\nport %d\nmin_free_mb 0\ntsnet 0\nonly %s\n" % (u.hostname, u.port, a.key))
        away = os.path.join(work, "away")
        tsconf = '{\n  "hostname": "phat5",\n  "httpProxyAddr": "127.0.0.1:%d"\n}\n' % pport
        log = run(binary, away, "host 127.0.0.1\nport 1\nmin_free_mb 0\nserver_ts %s %d\nonly %s\n" % (u.hostname, u.port, a.key), tsconf)
        hb_home, hb_away = os.path.join(home, "data", "homebrew"), os.path.join(away, "data", "homebrew")
        names = os.listdir(hb_home) if os.path.isdir(hb_home) else []
        check(len(names) == 1, "at home: one starter built (%s)" % names)
        if names:
            t_home = tree(os.path.join(hb_home, names[0]))
            t_away = tree(os.path.join(hb_away, names[0])) if os.path.isdir(os.path.join(hb_away, names[0])) else {}
            check(t_home and t_home == t_away, "away (LAN dead): the same starter built through the proxy (%d files, %d differ)" %
                  (len(t_home), sum(1 for k in t_home if t_away.get(k) != t_home[k])))
        check("tsnet: away-from-home fallback ready (proxy 127.0.0.1:%d" % pport in log, "tsnet proxy found in /data/tailscale/config.json")
        check("TAILNET" in log and "route:" in log, "route switch logged")
        check(STATS["abs"] > 0 and STATS["refused"] == 0, "every away download was an absolute-form proxy request (%d relayed, %d refused)" % (STATS["abs"], STATS["refused"]))
        tid = a.key.split("@")[0]
        cfg = open(os.path.join(away, "data", "blazing", "link", tid + ".cfg")).read()
        check("\nproxy 127.0.0.1 %d\nserver_ts %s %d\nbase " % (pport, u.hostname, u.port) in cfg, "link cfg carries proxy + server_ts right after server")
        cfg_home = open(os.path.join(home, "data", "blazing", "link", tid + ".cfg")).read()
        check("proxy " not in cfg_home, "tsnet 0: no proxy lines")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("ALL PASS" if not fails else "%d FAIL" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
