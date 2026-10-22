#!/usr/bin/env python3
"""blazingmount-starter.py - build a NATIVE streaming starter (no backport) for a jailbroken PS5
whose firmware is >= the game's required firmware. The console keeps only the game's SMALL files
plus 0-byte placeholders for the big ones; our libSceAmpr link fork streams the big files on demand
from the host file server (imgserve :4451 for images, dirserve :4452 for folders). Content-neutral:
the user supplies their own legally-owned game on their own host.

Runs ON the host (mac1) where imgserve/dirserve + the smblink tools live. Nothing here touches a console.
Reuses smblink's mkgame.check / cfg_text / amprindex so the starter, index and link config are built
exactly like the proven PHAT5 pipeline, minus the FW re-sign (not needed when console FW >= game FW).

    python3 -I blazingmount-starter.py SRC OUT --fork libSceAmpr.sprx --console-fw 13.60

      SRC   image name under imgserve (".ffpfsc") or a game folder under dirserve
      OUT   fresh output dir: OUT/starter/<Name (TID)>/ , OUT/<TID>.cfg , OUT/<TID>.json
      --console-fw  the target console's firmware; REFUSES if the game needs a newer FW
"""
import argparse, hashlib, json, os, shutil, sys

# the streaming-engine helpers (mkgame/amprindex) are vendored in host/engine/; override with $SMBLINK_TOOLS
ENGINE = os.environ.get("SMBLINK_TOOLS", os.path.join(os.path.dirname(os.path.abspath(__file__)), "engine"))
sys.path.insert(0, ENGINE)
import mkgame        # noqa: E402  (host/engine)
import amprindex     # noqa: E402  (host/engine)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--fork", required=True, help="path to the libSceAmpr.sprx link fork")
    ap.add_argument("--console-fw", type=float, required=True, help="target console firmware (e.g. 13.60)")
    ap.add_argument("--min-mb", type=int, default=1)
    a = ap.parse_args()

    src, files, r = mkgame.check(a.src, a.min_mb, quiet=True)
    if not r.get("tid"):
        sys.exit("no title id (sce_sys/param.json missing?)")
    if not r["loads_libSceAmpr"]:
        sys.exit("refusing: " + r["verdict"][0])
    gfw = r.get("fw")
    if gfw and gfw > a.console_fw:
        sys.exit("refusing: %s needs FW %s but the console is %s - this native builder does NOT backport "
                 "(use mkgame.py make for a lower-FW console)" % (r.get("name"), gfw, a.console_fw))

    tid = r["tid"]
    name = "%s (%s)" % ((r.get("name") or tid).replace("/", " ").replace(":", "").strip(), tid)
    starter = os.path.join(a.out, "starter", name)
    os.makedirs(starter)

    # 1) small files copied as-is (native); big files become 0-byte placeholders (streamed)
    remote = dict(r["remote"])
    for rel, size in files:
        dst = os.path.join(starter, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if rel in remote:
            open(dst, "wb").close()
        elif not rel.startswith("fakelib/"):
            src.copy(rel, dst)
            if os.path.getsize(dst) != size:
                sys.exit("size mismatch " + rel)

    # 2) ampr_emu.index with the REAL sizes (the stub's big files are 0-byte, so the index must carry truth)
    if not r["has_index"]:
        entries = [(("/app0/" + rel).encode(), size, 0)
                   for rel, size in files if not rel.startswith(("fakelib/", "decrypted/"))]
        open(os.path.join(starter, "ampr_emu.index"), "wb").write(amprindex.build(entries))

    # 3) fakelib = our fork as libSceAmpr.sprx (+ the game's own fakelib minus its libSceAmpr;
    #    + the PlayGo emulator for folder games). NO FW re-sign, NO scene Agc/Psml (native console runs them).
    fk = os.path.join(starter, "fakelib")
    os.makedirs(fk, exist_ok=True)
    shutil.copyfile(a.fork, os.path.join(fk, "libSceAmpr.sprx"))
    for rel in r["own_fakelib"]:
        if os.path.basename(rel) == "libSceAmpr.sprx":
            continue
        src.copy(rel, os.path.join(fk, os.path.basename(rel)))
    if src.kind == "folder" and r["loads_playgo"] and not any(f.endswith("libScePlayGo.sprx") for f in r["own_fakelib"]):
        shutil.copyfile(os.path.join(mkgame.SCENE_FAKELIB, "libScePlayGo.sprx"), os.path.join(fk, "libScePlayGo.sprx"))

    # 4) the link config (/data/blazing/link/<TID>.cfg): mode remote, host server, base, one file line per big file
    cfg = mkgame.cfg_text(src, r, name, "remote")
    open(os.path.join(a.out, tid + ".cfg"), "w").write(cfg)

    sz = sum(os.path.getsize(os.path.join(dp, n)) for dp, dn, fn in os.walk(starter) for n in fn)
    manifest = {"tid": tid, "name": r.get("name"), "version": r.get("version"), "fw": gfw,
                "console_fw": a.console_fw, "src": a.src, "kind": src.kind, "starter": name,
                "starter_bytes": sz, "remote_files": len(remote), "remote_bytes": r["remote_bytes"],
                "fork_sha256": hashlib.sha256(open(a.fork, "rb").read()).hexdigest()}
    json.dump(manifest, open(os.path.join(a.out, tid + ".json"), "w"), indent=1)
    print(json.dumps({k: manifest[k] for k in ("tid", "name", "fw", "console_fw", "starter", "remote_files", "remote_bytes")}, indent=1))
    print("starter_MB %.1f   cfg %s" % (sz / 1e6, os.path.join(a.out, tid + ".cfg")))
    print("--- cfg ---\n" + cfg)


if __name__ == "__main__":
    main()
