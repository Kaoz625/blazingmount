#!/usr/bin/env python3
"""Host tests for dirserve.py: python3 -I tools/imgserve/test_dirserve.py  (stdlib only, loopback, temp folders, no console)."""
import hashlib
import http.client
import json
import os
import sys
import tempfile
import threading
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dirserve  # noqa: E402

dirserve.QUIET = True

FAILS = []
CHECKS = 0


def check(name, cond, detail=""):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(name)
        print("FAIL", name, detail)


def req(port, method, path, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    c.request(method, path, headers=headers or {})
    r = c.getresponse()
    body = r.read()
    out = (r.status, dict((k.lower(), v) for k, v in r.getheaders()), body)
    c.close()
    return out


def main():
    tmp = tempfile.mkdtemp(prefix="dirserve-test-")
    outside = tempfile.mkdtemp(prefix="dirserve-outside-")
    root = os.path.join(tmp, "games")
    game = os.path.join(root, "PPSA38474 test game")
    os.makedirs(os.path.join(game, "cj", "content", "paks"))
    os.makedirs(os.path.join(root, "second"))
    payload = bytes((i * 7 + 3) & 0xFF for i in range(300000))
    small = os.path.join(game, "cj", "content", "paks", "cj ps5 (1).pak")
    open(small, "wb").write(payload)
    empty = os.path.join(game, "empty.bin")
    open(empty, "wb").close()
    big = os.path.join(game, "big.ucas")
    with open(big, "wb") as fh:                         # sparse 6 GiB file with marked bytes beyond 4 GiB
        fh.truncate(6 * 1024 ** 3)
        fh.seek(5 * 1024 ** 3 + 12345)
        fh.write(b"MARK-BEYOND-4GIB")
    secret = os.path.join(outside, "secret.txt")
    open(secret, "w").write("do not serve")
    os.symlink(secret, os.path.join(game, "escape.txt"))
    os.symlink(outside, os.path.join(root, "linked-folder"))

    srv = dirserve.make_server(root, 0, "127.0.0.1")
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "/d/" + urllib.parse.quote("PPSA38474 test game") + "/"
    rel = "cj/content/paks/" + urllib.parse.quote("cj ps5 (1).pak")

    st, h, b = req(port, "GET", "/")
    check("root lists the folder", st == 200 and b"PPSA38474 test game" in b and b"second" in b and b"linked-folder" not in b, b)
    st, h, b = req(port, "GET", base + "?list")
    lst = json.loads(b) if st == 200 else []
    paths = {e["path"]: e["size"] for e in lst}
    check("list has the nested file with its size", paths.get("cj/content/paks/cj ps5 (1).pak") == len(payload), paths)
    check("list has the 6 GiB file", paths.get("big.ucas") == 6 * 1024 ** 3, paths)
    check("list hides a symlink that points outside the folder", "escape.txt" not in paths, paths)
    st, h, b = req(port, "GET", base)
    check("folder path without ?list also lists", st == 200 and json.loads(b) == lst)

    st, h, b = req(port, "GET", base + rel)
    check("full GET 200 + bytes", st == 200 and b == payload and h.get("accept-ranges") == "bytes" and int(h["content-length"]) == len(payload))
    st, h, b = req(port, "HEAD", base + rel)
    check("HEAD gives length and no body", st == 200 and int(h["content-length"]) == len(payload) and b == b"")
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=100-199"})
    check("range a-b", st == 206 and b == payload[100:200] and h["content-range"] == "bytes 100-199/%d" % len(payload))
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=299990-"})
    check("open-ended range", st == 206 and b == payload[299990:] and h["content-range"] == "bytes 299990-299999/300000")
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=-50"})
    check("suffix range", st == 206 and b == payload[-50:] and h["content-range"] == "bytes 299950-299999/300000")
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=299000-999999999"})
    check("end clamped to size", st == 206 and b == payload[299000:] and h["content-range"].endswith("-299999/300000"))
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=300000-"})
    check("start at EOF is 416", st == 416 and h.get("content-range") == "bytes */300000")
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=9-3"})
    check("reversed range is 416", st == 416)
    st, h, b = req(port, "GET", base + rel, {"Range": "bytes=abc"})
    check("garbage range is 416", st == 416)
    st, h, b = req(port, "GET", base + rel, {"Range": "items=1-2"})
    check("non-bytes unit is ignored (200 full)", st == 200 and b == payload)
    st, h, b = req(port, "GET", base + "empty.bin")
    check("empty file 200 with length 0", st == 200 and int(h["content-length"]) == 0 and b == b"")

    off = 5 * 1024 ** 3 + 12345
    st, h, b = req(port, "GET", base + "big.ucas", {"Range": "bytes=%d-%d" % (off, off + 15)})
    check("offset beyond 4 GiB", st == 206 and b == b"MARK-BEYOND-4GIB" and h["content-range"] == "bytes %d-%d/%d" % (off, off + 15, 6 * 1024 ** 3), (st, b))
    st, h, b = req(port, "GET", base + "big.ucas", {"Range": "bytes=-16"})
    check("suffix range on a 6 GiB file", st == 206 and b == b"\0" * 16 and h["content-range"].startswith("bytes %d-" % (6 * 1024 ** 3 - 16)))

    for name, path in (("dotdot", base + "../second/x"), ("encoded dotdot", base + "%2e%2e/second/x"), ("dotdot inside", base + "cj/../../x"),
                       ("backslash dotdot", base + "..%5Csecond%5Cx"), ("absolute", base + "/etc/passwd"), ("nul byte", base + "x%00y"),
                       ("symlink file out", base + "escape.txt"), ("symlinked folder", "/d/linked-folder/secret.txt"),
                       ("folder with slash", "/d/games/x"), ("folder dotdot", "/d/../x"), ("missing", base + "nope.bin"),
                       ("dir not file", base + "cj"), ("wrong prefix", "/v/x/y"), ("empty folder name", "/d//x")):
        st, h, b = req(port, "GET", path)
        check("refused: " + name, st in (404, 200) and b"do not serve" not in b and (st == 404 or name == "dir not file" and False),
              (st, b[:60]))

    # concurrency: 8 threads each read different ranges of the same file; every byte must match
    errs = []

    def worker(k):
        for j in range(25):
            a = (k * 37000 + j * 911) % (len(payload) - 5000)
            st, h, b = req(port, "GET", base + rel, {"Range": "bytes=%d-%d" % (a, a + 4999)})
            if st != 206 or b != payload[a:a + 5000]:
                errs.append((k, j, st))

    ts = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check("8 threads x 25 ranges all identical", not errs, errs[:3])

    srv.shutdown()

    # ---- /t/<TITLEID>: game folders found by title id under several roots; a rename/move is found again (smb-link-5, 21 Oct 2026)
    root2 = os.path.join(tmp, "drive2")
    g1 = os.path.join(root2, "set-63e2b8a99d138de7-app")            # a dump folder with an opaque name, one level down
    os.makedirs(os.path.join(g1, "sce_sys"))
    os.makedirs(os.path.join(g1, "packs"))
    open(os.path.join(g1, "eboot.bin"), "wb").write(b"EB")
    json.dump({"titleId": "PPSA21840", "contentVersion": "01.004.000"}, open(os.path.join(g1, "sce_sys", "param.json"), "w"))
    open(os.path.join(g1, "packs", "data.spk"), "wb").write(payload)
    for d, ver in (("Tokon A", "01.003.000"), ("Tokon B", "01.004.000")):    # one title id twice: needs @version
        p = os.path.join(root2, "dupes", d)
        os.makedirs(os.path.join(p, "sce_sys"))
        open(os.path.join(p, "eboot.bin"), "wb").write(b"EB")
        json.dump({"titleId": "PPSA15595", "contentVersion": ver}, open(os.path.join(p, "sce_sys", "param.json"), "w"))
    bp = os.path.join(root2, "PS5-BACKPORTS", "PPSA21840")              # a backport overlay is never taken for the game
    os.makedirs(os.path.join(bp, "sce_sys"))
    open(os.path.join(bp, "eboot.bin"), "wb").write(b"BP")
    json.dump({"titleId": "PPSA21840", "contentVersion": "01.004.000"}, open(os.path.join(bp, "sce_sys", "param.json"), "w"))
    srv = dirserve.make_server(root, 0, "127.0.0.1", [root2])
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    st, h, b = req(port, "GET", "/t/PPSA21840/packs/data.spk", {"Range": "bytes=10-19"})
    check("/t/ finds the folder by title id (any root, any name)", st == 206 and b == payload[10:20], (st, b[:20]))
    st, h, b = req(port, "GET", "/t/PPSA21840/?list")
    check("/t/ list works", st == 200 and any(e["path"] == "packs/data.spk" for e in json.loads(b)), b[:200])
    st, h, b = req(port, "GET", "/t/PPSA15595/eboot.bin")
    check("/t/ refuses an ambiguous title id", st == 404, st)
    st, h, b = req(port, "GET", "/t/PPSA15595@01.004.000/sce_sys/param.json")
    check("/t/<TID>@<version> picks the right one", st == 200 and b"01.004.000" in b, b)
    st, h, b = req(port, "GET", "/t/PPSA99999/eboot.bin")
    check("/t/ unknown title id = 404", st == 404, st)
    st, h, b = req(port, "GET", "/d/" + urllib.parse.quote("PPSA38474 test game") + "/empty.bin")
    check("/d/ still works with several roots", st == 200, st)
    moved = os.path.join(root2, "PS5 Games", "GreedFall - The Dying World [PPSA21840]")   # the coming rename
    os.makedirs(os.path.dirname(moved))
    os.rename(g1, moved)
    dirserve._INDEX["t"] = 0                                         # (the 15 s rescan limit has passed)
    st, h, b = req(port, "GET", "/t/PPSA21840/packs/data.spk", {"Range": "bytes=0-9"})
    check("/t/ finds the game again after a rename + move", st == 206 and b == payload[0:10], (st, b[:20]))
    srv.shutdown()
    print("%d checks, %d failed" % (CHECKS, len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
