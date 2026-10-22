#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# mkstub.py - build a STUB folder of a game that lives in a .ffpfsc image served by tools/imgserve: every inner file is downloaded, except the
# listed big ones, which become 0-byte placeholders (the SMB link serves them). Sizes are checked against imgserve's listing.
#   python3 mkstub.py HOST PORT IMAGE OUTDIR BIG1,BIG2,...
import hashlib, http.client, json, os, sys, urllib.parse

host, port, image, out, bigs = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], set(sys.argv[5].split(","))
base = "/v/" + urllib.parse.quote(image) + "/"
c = http.client.HTTPConnection(host, port, timeout=120)
c.request("GET", base + "?list")
files = json.loads(c.getresponse().read())
missing = bigs - {f["path"] for f in files}
if missing:
    sys.exit("not in the image: " + ", ".join(sorted(missing)))
os.makedirs(out, exist_ok=False)
total = 0
for f in sorted(files, key=lambda x: x["path"]):
    rel, size = f["path"], f["size"]
    if rel.startswith("/") or ".." in rel.split("/"):
        sys.exit("odd path " + rel)
    dst = os.path.join(out, *rel.split("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if rel in bigs:
        open(dst, "wb").close()
        print("placeholder %s (real size %d)" % (rel, size))
        continue
    c.request("GET", base + urllib.parse.quote(rel))
    r = c.getresponse()
    h = hashlib.sha256()
    n = 0
    with open(dst, "wb") as fh:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            fh.write(b)
            h.update(b)
            n += len(b)
    if r.status != 200 or n != size:
        sys.exit("bad download %s: status %d, %d of %d bytes" % (rel, r.status, n, size))
    total += n
    print("%12d %s %s" % (n, h.hexdigest()[:16], rel))
print("stub ready: %s, %d files downloaded (%d bytes), %d placeholders" % (out, len(files) - len(bigs), total, len(bigs)))
