#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mkgame.py - ONE command that turns a game on mac1's drives into a PHAT5 STARTER that streams its big files from mac1.
Runs ON mac1 (drives, imgserve :4451, dirserve :4452 and the backport tools are there). Nothing here touches a console.

    python3 -I mkgame.py check SRC                       can it stream? (FW, backport, AMPR/AIO, big files, index) - read-only
    python3 -I mkgame.py make  SRC OUT --fork libSceAmpr.sprx [--min-mb 1]
                                                         build everything in OUT (a mac1 folder):
        OUT/starter/<Name (TID)>/    every file of the game except the big ones, which are 0-byte placeholders (+ ampr_emu.index)
        OUT/backport/<TID>/          eboot + modules re-signed for FW 10.20 when the game needs more (mkbackport.py), and fakelib/ =
                                     our link fork as libSceAmpr.sprx + the game's own fakelib files + the scene Agc/Psml libs if needed
        OUT/<TID>.cfg                the link config (/data/blazing/link/<TID>.cfg): mode remote, server, base, one file line per big file
        OUT/PLAN.txt                 GB per part + the exact push commands (each needs Markus's yes for new space + the PHAT5 claim)

SRC = an image name in the 5 TB PS5-DOWNLOADS ("The Mound - Omen of Cthulhu (PPSA26223).ffpfsc", served by imgserve) or a game FOLDER
under the 8 TB homebrew (served by dirserve), e.g. "/path/to/your/folders/<game-folder>".

Why each check (21 Oct 2026): the link lives in our libSceAmpr fork, so a game that does not load libSceAmpr cannot stream; a game that
does its own AIO needs the AIO-intercept build (ours is); a dump without ampr_emu.index gets one with the REAL sizes (amprindex.py),
else the placeholders would be indexed as empty; FW > 10.20 needs a per-game backport; at most 64 streamed files (kMaxFiles).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import amprindex  # noqa: E402

# Host-specific settings - override with environment variables for your own host (see host/README.md).
IMGSERVE = os.environ.get("BLAZINGMOUNT_IMGSERVE", "http://127.0.0.1:4451")        # your local imgserve
DRIVE5_DL = os.environ.get("BLAZINGMOUNT_IMAGE_DIR", "/path/to/your/image-drive")  # where your .ffpfsc images live
DIRSERVE_ROOT = os.environ.get("BLAZINGMOUNT_FOLDER_DIR", "/path/to/your/folders") # where your game folders live
MAC1_LAN = os.environ.get("BLAZINGMOUNT_HOST_IP", "192.168.1.2")                   # host IP written into a cfg; the installer overrides via --host-ip
# The settings below are ONLY used by the FW-downgrade/backport path, which is OUT OF SCOPE for the
# native builder (it refuses a game that needs newer FW than the console). Set them only if you add that path.
BACKPORT_TOOLS = os.environ.get("BLAZINGMOUNT_BACKPORT_TOOLS", "")
P5_EXPORTS = os.environ.get("BLAZINGMOUNT_P5_EXPORTS", "")
SCENE_FAKELIB = os.environ.get("BLAZINGMOUNT_SCENE_FAKELIB", "")                   # libSceAgc / AgcDriver / Psml (downgrade path only)
PHAT5_FW = float(os.environ.get("BLAZINGMOUNT_MIN_CONSOLE_FW", "10.20"))           # lowest-FW console you target
MAX_REMOTE = 1024                 # = the link's kMaxFiles (21 Oct: 1024)


def nid(name):
    h = hashlib.sha1(name.encode() + bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")).digest()[:8]
    import base64
    return base64.b64encode(struct.pack(">Q", struct.unpack("<Q", h)[0])).decode()[:11].replace("/", "-")


AIO = {nid(n): n for n in ("sceKernelAioSubmitReadCommands", "sceKernelAioSubmitReadCommandsMultiple", "sceKernelAioWaitRequest",
                           "sceKernelAioPollRequest", "sceKernelAioWaitRequests")}
APR_READ = {nid(n): n for n in ("sceAmprAprCommandBufferReadFile", "sceAmprAprCommandBufferReadFileGather",
                                "sceAmprAprCommandBufferReadFileScatter", "sceAmprAprCommandBufferReadFileGatherScatter")}


def image_path(name):
    """Full path of an image (by file name) from imgserve's catalog page ('  <name>  <bytes> B  <tid> <ver>  <path>')."""
    with urllib.request.urlopen(IMGSERVE + "/", timeout=60) as r:
        for line in r.read().decode(errors="replace").splitlines():
            if line.startswith("  " + name + "  "):
                return line.rsplit("  ", 1)[-1].strip()
    sys.exit("imgserve does not list " + name)


class Src:
    """A game as files: list() -> [(rel, size)], read(rel) -> bytes, copy(rel, dst)."""

    def __init__(self, spec):
        self.spec = spec
        if os.path.isdir(spec):
            self.kind, self.root = "folder", spec
            self.base = "/d/" + urllib.parse.quote(os.path.relpath(spec, DIRSERVE_ROOT))
            self.port = 4452
        else:
            self.kind, self.image = "image", os.path.basename(spec)
            self.port = 4451
            self.path = spec if os.path.isfile(spec) else image_path(self.image)
            # Prefer the title-id+size base over /v/<filename>: it disambiguates two builds of one TID
            # (imgserve-duplicate-tid-needs-exact-size) and avoids /v/ failing on non-ASCII image names (Tokon, 22 Oct 2026).
            m = re.search(r"(PPSA\d{5})", self.image)
            if m and os.path.isfile(self.path):
                self.base = "/t/%s@%d" % (m.group(1), os.path.getsize(self.path))
            else:
                self.base = "/v/" + urllib.parse.quote(self.image)

    def tbase(self, r):
        """Title-id base (smb-link-5, 21 Oct 2026): renames/moves on mac1's drives never break the starter. Images are pinned to their exact
        size (one title id can have several builds), folders to their content version."""
        if self.kind == "folder":
            return "/t/%s@%s" % (r["tid"], r.get("version"))
        return "/t/%s@%d" % (r["tid"], os.path.getsize(self.path))   # = the image file's size, as imgserve's catalog has it

    def list(self):
        if self.kind == "folder":
            out = []
            for dp, dn, fn in os.walk(self.root):
                dn[:] = sorted(x for x in dn if x != "decrypted")
                for n in sorted(fn):
                    p = os.path.join(dp, n)
                    out.append((os.path.relpath(p, self.root).replace(os.sep, "/"), os.path.getsize(p)))
            return out
        with urllib.request.urlopen(IMGSERVE + self.base + "/?list", timeout=60) as r:
            return [(x["path"], x["size"]) for x in json.loads(r.read()) if not x["path"].startswith("decrypted/")]

    def copy(self, rel, dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if self.kind == "folder":
            shutil.copyfile(os.path.join(self.root, rel), dst)
            return
        with urllib.request.urlopen(IMGSERVE + self.base + "/" + urllib.parse.quote(rel), timeout=600) as r, open(dst, "wb") as fh:
            shutil.copyfileobj(r, fh, 1 << 20)

    def read(self, rel):
        tmp = tempfile.mktemp(prefix="mkgame-")
        try:
            self.copy(rel, tmp)
            return open(tmp, "rb").read()
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)


def decrypt(src, rel, dst):
    """fself -> plain ELF (decrypt_fself.py). Plain ELFs are copied."""
    raw = tempfile.mktemp(prefix="mkgame-self-")
    try:
        src.copy(rel, raw)
        with open(raw, "rb") as fh:
            magic = fh.read(4)
        if magic == b"\x7fELF":
            shutil.copyfile(raw, dst)
            return True
        sys.path.insert(0, os.path.join(BACKPORT_TOOLS, "src"))
        from decrypt_fself import UnsignedELFConverter
        return UnsignedELFConverter().convert_file(raw, dst)
    finally:
        if os.path.exists(raw):
            os.unlink(raw)


def elf_needs(path):
    """(DT_NEEDED names, import NIDs) of a plain PS5 ELF."""
    from mkbackport import Dyn  # same folder as this file
    data = open(path, "rb").read()
    d = Dyn(data)
    needed = [d.cstr(v) for t, v in d.tags if t == 1]
    tab = data[d.str_off:d.str_off + d.str_size]
    nids = set(x.split(b"#")[0].decode(errors="replace") for x in tab.split(b"\0") if x.count(b"#") == 2)
    return needed, nids


def param(src, files):
    rel = "sce_sys/param.json"
    if rel not in dict(files):
        return {}
    d = json.loads(src.read(rel))
    loc = d.get("localizedParameters", {})
    v = d.get("requiredSystemSoftwareVersion", 0)
    v = int(v, 0) if isinstance(v, str) else v
    return {"tid": d.get("titleId"), "name": (loc.get("en-US") or loc.get(loc.get("defaultLanguage", "en-US")) or {}).get("titleName"),
            "version": d.get("contentVersion"), "fw": float("%x.%02x" % ((v >> 56) & 0xFF, (v >> 48) & 0xFF)) if v else None}


def check(spec, min_mb, quiet=False):
    src = Src(spec)
    files = src.list()
    info = param(src, files)
    work = tempfile.mkdtemp(prefix="mkgame-check-")
    try:
        eb = os.path.join(work, "eboot.elf")
        ok = decrypt(src, "eboot.bin", eb)
        needed, nids = elf_needs(eb) if ok else ([], set())
    finally:
        shutil.rmtree(work, ignore_errors=True)
    big = sorted([f for f in files if f[1] >= min_mb << 20 and f[0] != "eboot.bin" and not f[0].startswith(("sce_sys/", "sce_module/", "prx/", "fakelib"))],
                 key=lambda f: -f[1])
    r = {"src": spec, "kind": src.kind, **info, "files": len(files), "bytes": sum(f[1] for f in files),
         "loads_libSceAmpr": "libSceAmpr.prx" in needed, "loads_playgo": "libScePlayGo.prx" in needed, "apr_reads": sorted(APR_READ[n] for n in nids if n in APR_READ),
         "game_aio": sorted(AIO[n] for n in nids if n in AIO), "has_index": "ampr_emu.index" in dict(files),
         "own_fakelib": sorted(f[0] for f in files if f[0].startswith("fakelib/")),
         "remote": big[:MAX_REMOTE], "remote_bytes": sum(f[1] for f in big[:MAX_REMOTE]),
         "starter_bytes": sum(f[1] for f in files) - sum(f[1] for f in big[:MAX_REMOTE]),
         "needs_backport": bool(info.get("fw") and info["fw"] > PHAT5_FW)}
    verdict = []
    if not r["loads_libSceAmpr"]:
        verdict.append("CANNOT STREAM: the eboot does not load libSceAmpr, so our link never runs in this game")
    if len(big) > MAX_REMOTE:
        verdict.append("more than %d big files: the smallest %d stay in the starter" % (MAX_REMOTE, len(big) - MAX_REMOTE))
    r["verdict"] = verdict or ["can stream: link loads with libSceAmpr%s" % (" (game AIO: intercept build)" if r["game_aio"] else "")]
    if not quiet:
        print(json.dumps({k: v for k, v in r.items() if k != "remote"}, indent=1))
        for rel, size in r["remote"]:
            print("  stream %12d %s" % (size, rel))
    return src, files, r


# Disk netcache size for a game's starter (smb-link-6, 22 Oct 2026): a first playthrough of a big game re-reads far more than the old
# 4 GiB cap (RESIDENT EVIL requiem pulled 8+ GiB just reaching its opening). Default = the whole streamed set when it is small, else a
# ceiling so the M.2 is not flooded. Override with env MKGAME_CACHE_MB (0 = turn the netcache off for this starter).
CACHE_CEIL_MB = int(os.environ.get("MKGAME_CACHE_CEIL_MB", "16384"))      # 16 GiB default ceiling per game
CACHE_FLOOR_MB = 2048


def disk_cache_mb(remote_bytes):
    env = os.environ.get("MKGAME_CACHE_MB")
    if env is not None:
        return max(0, int(env))
    whole = (remote_bytes + (1 << 20) - 1) >> 20                          # cache the whole game when it is small enough
    return max(CACHE_FLOOR_MB, min(whole, CACHE_CEIL_MB))


def cfg_text(src, r, name, mode):
    """The link config: mode, mac1 server (imgserve :4451 for images, dirserve :4452 for folders), one file line per streamed file."""
    base = src.tbase(r)
    cfg = ["# mkgame.py v%s: %s, %d files read from mac1 (%.1f GB)" % (r["version"], name, len(r["remote"]), r["remote_bytes"] / 1e9),
           "mode " + mode, "server %s %d" % (MAC1_LAN, src.port), "base " + base]
    cfg += ["file %d %s" % (size, rel) for rel, size in r["remote"]]
    dc = disk_cache_mb(r["remote_bytes"])
    cfg += ["block_kb 256", "cache_mb 32", "conns 4", "async 1", "timeout_ms 8000", "stats_s 2", "log_first 60", "trace_opens 100",
            "trace_io 60", "trace_apr 60"]
    if dc:
        cfg += ["disk_cache_mb %d" % dc]
    return "\n".join(cfg) + "\n"


def overlay_files(overlay):
    """(rel, abs path) of every file in a game's proven backports/<TID> overlay (a local mirror: phat5-games.py fetch-overlay)."""
    out = []
    for dp, dn, fn in os.walk(overlay):
        for n in fn:
            out.append((os.path.relpath(os.path.join(dp, n), overlay).replace(os.sep, "/"), os.path.join(dp, n)))
    return sorted(out)


def build_backport(src, files, r, bp, fork=None, overlay=None):
    """backports/<TID> overlay: fakelib = [our fork] + the game's own fakelib files (not its libSceAmpr when a fork is given) + the PlayGo
    emulator for folder games + the scene Agc/AgcDriver/Psml when the game needs FW > 10.20; eboot + modules re-signed (mkbackport.py)
    when it needs FW > 10.20, else fakelib only.
    overlay = a local copy of the game's PROVEN backports/<TID> overlay (smb-link-6, 21 Oct 2026: The Mound's starter with mkbackport's
    generic Agc/AgcDriver/Psml crashed with a GPU fault 'Bad packet: Privilege reg'; its overlay has other builds of those libs). Then the
    starter gets exactly what the working full copy saw (image files, the overlay on top, overlay wins) + our fork as libSceAmpr: no
    mkbackport, no scene libs."""
    fakelibs = [fork] if fork else []
    tmpd = tempfile.mkdtemp(prefix="mkgame-fk-")
    try:
        for rel in r["own_fakelib"]:
            if not (fork and os.path.basename(rel) == "libSceAmpr.sprx"):
                d = os.path.join(tmpd, os.path.basename(rel))
                src.copy(rel, d)
                fakelibs.append(d)
        if overlay:
            remote = dict(r["remote"])
            ov = overlay_files(overlay)
            clash = [rel for rel, p in ov if rel in remote]
            if clash:
                sys.exit("refusing: the overlay replaces streamed file(s) %s - the link would serve the unpatched ones" % clash[:3])
            byname = {os.path.basename(f): f for f in fakelibs}
            for rel, p in ov:
                if rel.startswith("fakelib/") and rel.count("/") == 1 and not (fork and rel == "fakelib/libSceAmpr.sprx"):
                    byname[os.path.basename(rel)] = p                       # overlay wins over the game's own fakelib
            os.makedirs(os.path.join(bp, "fakelib"))
            for n, f in byname.items():
                shutil.copyfile(f, os.path.join(bp, "fakelib", n))
            for rel, p in ov:
                if not rel.startswith("fakelib/"):
                    d = os.path.join(bp, *rel.split("/"))
                    os.makedirs(os.path.dirname(d), exist_ok=True)
                    shutil.copyfile(p, d)
            return
        if fork and src.kind == "folder" and r["loads_playgo"] and not any(f.endswith("libScePlayGo.sprx") for f in r["own_fakelib"]):
            fakelibs.append(os.path.join(SCENE_FAKELIB, "libScePlayGo.sprx"))   # folder games: the PlayGo emulator (as Samson's / Onimusha's fakelib)
        if not r["needs_backport"]:
            os.makedirs(os.path.join(bp, "fakelib"))
            for f in fakelibs:
                shutil.copyfile(f, os.path.join(bp, "fakelib", os.path.basename(f)))
            return
        for lib in ("libSceAgc.sprx", "libSceAgcDriver.sprx", "libScePsml.sprx"):
            fakelibs.append(os.path.join(SCENE_FAKELIB, lib))
        # mkbackport walks SRC for eboot.bin / *.prx and reads the plain ELF from SRC/decrypted/<rel>: give it exactly that
        dec = os.path.join(tmpd, "game")
        for rel, size in files:
            if rel == "eboot.bin" or (rel.endswith((".prx", ".sprx")) and not rel.startswith(("fakelib/", "sce_sys/"))):
                plain = os.path.join(dec, "decrypted", *rel.split("/"))
                os.makedirs(os.path.dirname(plain), exist_ok=True)
                if not decrypt(src, rel, plain):
                    sys.exit("decrypt failed " + rel)
                marker = os.path.join(dec, *rel.split("/"))
                os.makedirs(os.path.dirname(marker), exist_ok=True)
                open(marker, "wb").close()
        cmd = [sys.executable, "-I", os.path.join(HERE, "mkbackport.py"), dec, bp, "--compat", os.path.join(HERE, "..", "compat-13to10.json")]
        for f in fakelibs:
            cmd += ["--fakelib", f]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)


def make(spec, out, fork, min_mb, overlay=None):
    """A SELF-CONTAINED starter (smb-link-5, 21 Oct 2026): the game's small files + 0-byte placeholders for the streamed ones, the backported
    eboot/modules IN PLACE when the game needs FW > 10.20, and its own fakelib/ (our link fork + the game's fakelib + scene Agc/Psml +
    PlayGo emulator). No backports/<TID> overlay: ShadowMount uses a folder game's own fakelib, and a title-level overlay is what
    panicked/wedged PHAT5 with Tokon on 20 Oct. One folder = one game; deleting it removes the game completely."""
    src, files, r = check(spec, min_mb, quiet=True)
    if not r.get("tid"):
        sys.exit("no title id (sce_sys/param.json missing?)")
    if not r["loads_libSceAmpr"]:
        sys.exit("refusing: " + r["verdict"][0])
    tid, name = r["tid"], "%s (%s)" % ((r.get("name") or r["tid"]).replace("/", " ").replace(":", "").strip(), r["tid"])
    starter = os.path.join(out, "starter", name)
    os.makedirs(starter)
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
    if not r["has_index"]:
        entries = [(("/app0/" + rel).encode(), size, 0) for rel, size in files if not rel.startswith(("fakelib/", "decrypted/"))]
        open(os.path.join(starter, "ampr_emu.index"), "wb").write(amprindex.build(entries))
    tmp = tempfile.mkdtemp(prefix="mkgame-bp-")
    try:
        bp = os.path.join(tmp, tid)
        build_backport(src, files, r, bp, fork=fork, overlay=overlay)      # fakelib (+ re-signed ELFs, or the proven overlay)
        for dp, dn, fn in os.walk(bp):                       # merge into the starter: backported ELFs replace the originals
            for n in fn:
                rel = os.path.relpath(os.path.join(dp, n), bp)
                dst = os.path.join(starter, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(os.path.join(dp, n), dst)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    open(os.path.join(out, tid + ".cfg"), "w").write(cfg_text(src, r, name, "remote"))
    manifest = {"tid": tid, "name": r.get("name"), "version": r.get("version"), "fw": r.get("fw"), "src": spec, "kind": src.kind,
                "starter": name, "backported": ("overlay " + os.path.abspath(overlay)) if overlay else r["needs_backport"], "remote_files": len(remote), "remote_bytes": r["remote_bytes"],
                "fork_sha256": hashlib.sha256(open(fork, "rb").read()).hexdigest()}
    json.dump(manifest, open(os.path.join(out, tid + ".json"), "w"), indent=1)

    def size_of(p):
        return sum(os.path.getsize(os.path.join(dp, n)) for dp, dn, fn in os.walk(p) for n in fn)
    s_gb = size_of(starter) / 1e9
    plan = ["%s  v%s  FW %s  %s" % (name, r["version"], r["fw"], "(backported in the starter)" if r["needs_backport"] else "(no backport needed)"),
            "starter  %.2f GB  %s   (self-contained: its own fakelib, no backports overlay)" % (s_gb, starter),
            "streams  %.1f GB in %d files from mac1 :%d by title id (%s)" % (r["remote_bytes"] / 1e9, len(remote), src.port,
                                                                           "imgserve, serve-5tb.command" if src.port == 4451 else "dirserve, serve-8tb-folders.command"),
            "NEW SPACE ON PHAT5: %.2f GB -> needs Markus's yes first" % s_gb, "",
            "push = phat5-games.py push %s (claim PHAT5 + TV notice first). If PHAT5 has an old backports/%s overlay, move it OUT of" % (tid, tid),
            "backports first (its fakelib would win over the starter's; a renamed '.disabled' copy is still live with this SMP)."]
    open(os.path.join(out, "PLAN.txt"), "w").write("\n".join(plan) + "\n")
    print("\n".join(plan))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("src")
    c.add_argument("--min-mb", type=int, default=1)
    m = sub.add_parser("make")
    m.add_argument("src")
    m.add_argument("out")
    m.add_argument("--fork", required=True)
    m.add_argument("--overlay", help="local copy of the game's PROVEN backports/<TID> overlay (phat5-games.py fetch-overlay): used instead of mkbackport")
    m.add_argument("--min-mb", type=int, default=1)
    b = sub.add_parser("backport", help="only the backports/<TID> overlay for a full-copy install (no link fork): FW > 10.20 games")
    b.add_argument("src")
    b.add_argument("out")
    g = sub.add_parser("cfg", help="print the link config only (e.g. 'remote' to test a game whose FULL copy is still on the console)")
    g.add_argument("src")
    g.add_argument("--mode", default="remote", choices=["remote", "verify", "emul", "off"])
    g.add_argument("--min-mb", type=int, default=1)
    a = ap.parse_args()
    if a.cmd == "check":
        check(a.src, a.min_mb)
    elif a.cmd == "backport":
        if os.path.exists(a.out):
            sys.exit("OUT exists: " + a.out)
        src, files, r = check(a.src, 1, quiet=True)
        if not r["needs_backport"]:
            sys.exit("%s needs FW %s <= 10.20: no backport needed" % (r.get("name"), r.get("fw")))
        build_backport(src, files, r, a.out)
        print("backport %s v%s -> %s" % (r.get("name"), r.get("version"), a.out))
    elif a.cmd == "cfg":
        src, files, r = check(a.src, a.min_mb, quiet=True)
        sys.stdout.write(cfg_text(src, r, "%s (%s)" % (r.get("name"), r.get("tid")), a.mode))
    else:
        if os.path.exists(a.out):
            sys.exit("OUT exists: " + a.out)
        make(a.src, a.out, a.fork, a.min_mb, overlay=a.overlay)


if __name__ == "__main__":
    main()
