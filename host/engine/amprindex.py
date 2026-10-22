#!/usr/bin/env python3
"""amprindex.py - read / write ampr_emu's /app0/ampr_emu.index (format AMPRIDX3, drakmor/ampr_emu src/ampr_emu_index.cpp).

Why: a game that reads through APR needs this index to map paths to file ids and SIZES. Without the file, ampr_emu builds it at runtime
with the real stat() - and on a STARTER folder the streamed files are 0-byte placeholders, so every big pack would be indexed as empty.
So a starter for a game without its own index (GreedFall PPSA21840 on the 8 TB) gets one made here from the full copy on mac1's drive.

    python3 amprindex.py dump INDEX                    print header + entries
    python3 amprindex.py make GAME_DIR OUT             index of every regular file under GAME_DIR (paths as /app0/<rel>, real size + mtime)
                          [--exclude PREFIX ...]       skip rel paths (default: decrypted/ fakelib/ fakelib2/ and the index/log files)
    python3 amprindex.py selftest INDEX                rebuild INDEX from its own entries (same path-blob order) and compare byte for byte

Layout (all little-endian): header 48 B {magic 'AMPRIDX3', u32 version 3, u32 entrySize 24, u64 entryCount, u64 pathBytes, u64 hashOffset,
u32 hashSlotSize 16, u32 hashSlotCount}; entryCount records {u32 pathOffset, u32 pathLength, u64 size, i64 mtime} sorted by path (ASCII
case-insensitive, '\\' = '/'); the path blob (NUL-terminated paths in scan order); at hashOffset (8-aligned) the open-addressed table
{u64 hash, u32 indexPlusOne, u32 reserved(1 = another slot has the same hash)}, slots = smallest power of two >= 2 * entries, FNV-1a 64 of
the lower-cased path (0 -> 1), linear probing.
"""
import os
import struct
import sys

MAGIC = b"AMPRIDX3"
HDR = struct.Struct("<8sIIQQQII")
REC = struct.Struct("<IIQq")
SLOT = struct.Struct("<QII")
SKIP_DEFAULT = ("decrypted/", "fakelib/", "fakelib2/", "ampr_emu.index", "ampr_emu.index.tmp", "ampr_emu.index.bak")


def lower(b):
    return bytes((c + 32) if 65 <= c <= 90 else c for c in b)


def key(p):
    return lower(p.replace(b"\\", b"/"))


def fnv(p):
    h = 1469598103934665603
    for c in key(p):
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h or 1


def build(entries):
    """entries: [(path bytes, size, mtime)] in path-blob (scan) order. Returns the index bytes."""
    blob, recs = bytearray(), []
    for p, size, mtime in entries:
        recs.append((len(blob), len(p), size, mtime, p))
        blob += p + b"\0"
    recs.sort(key=lambda r: key(r[4]))
    n = len(recs)
    slots = 2
    while slots < 2 * n:
        slots <<= 1
    path_off = HDR.size + REC.size * n
    hash_off = (path_off + len(blob) + 7) & ~7
    table = [[0, 0, 0] for _ in range(slots)]
    mask = slots - 1
    for i, r in enumerate(recs):
        h = fnv(r[4])
        pos, dup = h & mask, False
        while table[pos][1]:
            if table[pos][0] == h:
                table[pos][2] |= 1
                dup = True
            pos = (pos + 1) & mask
        table[pos] = [h, i + 1, 1 if dup else 0]
    out = bytearray(HDR.pack(MAGIC, 3, REC.size, n, len(blob), hash_off, SLOT.size, slots))
    for off, ln, size, mtime, _ in recs:
        out += REC.pack(off, ln, size, mtime)
    out += blob
    out += b"\0" * (hash_off - len(out))
    for h, ip1, res in table:
        out += SLOT.pack(h, ip1, res)
    return bytes(out)


def parse(data):
    magic, ver, esz, n, pbytes, hoff, ssz, scount = HDR.unpack_from(data, 0)
    if magic != MAGIC or ver != 3 or esz != REC.size or ssz != SLOT.size:
        raise SystemExit("not an AMPRIDX3 index (magic %r version %d)" % (magic, ver))
    blob_off = HDR.size + REC.size * n
    recs = []
    for i in range(n):
        off, ln, size, mtime = REC.unpack_from(data, HDR.size + REC.size * i)
        recs.append((off, data[blob_off + off:blob_off + off + ln], size, mtime))
    return (n, pbytes, hoff, scount), recs


def cmd_dump(path):
    hdr, recs = parse(open(path, "rb").read())
    print("entries %d pathBytes %d hashOffset %d slots %d" % hdr)
    for off, p, size, mtime in recs:
        print("%14d %12d %s" % (size, mtime, p.decode(errors="replace")))


def cmd_selftest(path):
    data = open(path, "rb").read()
    hdr, recs = parse(data)
    in_blob_order = sorted(recs, key=lambda r: r[0])
    again = build([(p, size, mtime) for off, p, size, mtime in in_blob_order])
    print("rebuilt %d bytes vs %d: %s" % (len(again), len(data), "IDENTICAL" if again == data else "DIFFERENT"))
    return 0 if again == data else 1


def cmd_make(root, out, excludes):
    entries = []
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for name in sorted(fn):
            full = os.path.join(dp, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if any(rel == x or rel.startswith(x) for x in excludes):
                continue
            st = os.stat(full)
            entries.append((("/app0/" + rel).encode(), st.st_size, int(st.st_mtime)))
    data = build(entries)
    with open(out, "wb") as fh:
        fh.write(data)
    print("%s: %d files, %d bytes, %.2f GB indexed" % (out, len(entries), len(data), sum(e[1] for e in entries) / 1e9))


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "dump":
        cmd_dump(a[1])
    elif len(a) >= 2 and a[0] == "selftest":
        sys.exit(cmd_selftest(a[1]))
    elif len(a) >= 3 and a[0] == "make":
        ex = list(SKIP_DEFAULT)
        i = 3
        while i < len(a):
            if a[i] == "--exclude" and i + 1 < len(a):
                ex.append(a[i + 1])
                i += 2
            else:
                sys.exit("unknown argument " + a[i])
        cmd_make(a[1], a[2], ex)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
