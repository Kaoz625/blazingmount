#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_backport_gate.py - fake-console test of the FW gate: a game that needs a newer FW is refused, unless a hand-placed
<homebrew_dir>/backports/<TID>[suffix] overlay exists; the "needs FW" refusal is re-checked every pass (v2.13).

    python3 -I host/test_backport_gate.py --bin out/blazingmountd-host     (HOST=1 NO_SMB=1 build)
"""
import argparse, http.server, os, subprocess, sys, tempfile, threading

LIST = ("PPSA34008@1\tPPSA34008\t1000\t01.000.000\t13.60\tx\tMortal Shell II\n"
        "PPSA23226@1\tPPSA23226\t1000\t01.000.000\t11.00\tx\tBlack Myth\n")

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = LIST.encode() if self.path == "/games.txt" else b""
        self.send_response(200 if body else 404); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--bin", required=True); a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
    root = tempfile.mkdtemp(prefix="bm-gate-")
    os.makedirs(root + "/data/blazing"); os.makedirs(root + "/data/homebrew/backports")
    open(root + "/data/blazing/mount.conf", "w").write("host 127.0.0.1\nport %d\nfw 10.20\n" % srv.server_port)
    fails = 0
    def check(c, w):
        nonlocal fails; print(("PASS " if c else "FAIL ") + w); fails += 0 if c else 1
    def run():
        subprocess.run([a.bin, "once"], env=dict(os.environ, BM_ROOT=root), timeout=120, check=False)
        return open(root + "/data/blazing/blazingmountd.log").read()
    log = run()
    check("refused PPSA34008@1 Mortal Shell II: needs FW 13.60" in log and "refused PPSA23226@1 Black Myth: needs FW 11.00" in log, "no overlay: both refused")
    os.makedirs(root + "/data/homebrew/backports/PPSA34008")                    # exact name
    n = len(log); log = run()[n:]
    check("building with backport /data/homebrew/backports/PPSA34008" in log, "exact overlay: recheck builds (not 'settled earlier')")
    check("refused PPSA23226@1 Black Myth: needs FW 11.00" in log, "no overlay yet for the other game: refusal is re-checked and kept")
    os.makedirs(root + "/data/homebrew/backports/PPSA23226-overlay-20261029")   # suffix
    n += len(log); log = run()[n:]
    check("building with backport /data/homebrew/backports/PPSA23226-overlay-20261029" in log, "suffix overlay matches")
    if fails: print(open(root + "/data/blazing/blazingmountd.log").read()[-3000:])
    print("FAILED %d" % fails if fails else "ALL PASS"); sys.exit(1 if fails else 0)

main()
