#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""library.py - EVERY game Markus owns, on every system, with its cover: the BlazingMount app's whole library (runs on mac1).

    python3 library.py index   [--owned D5.md] [--out ~/.blazing/library]      -> library.json (+ .etag): owned + on the drives + streamable
    python3 library.py covers  [--only SYSTEM] [--limit N]                     -> ~/.blazing/covers/{src,full,thumb}/<id>.*  (each fetched ONCE)
    python3 library.py sheets                                                  -> ~/.blazing/covers/sheet/<n>.png (64 covers a page) + covers.json
    python3 library.py serve   [--port 4453]                                   -> GET /library.json (ETag), /covers/{thumb,full,sheet}/<id>.jpg
    python3 library.py stats
    python3 library.py watch   [--every 300]                                     the library follows the drives and the owned list by itself:
                               a cheap signature (owned list mtime, every game folder's names+mtimes, imgserve /games.txt) every 5 min;
                               when it moves: index, covers (new titles only: the rest are settled), sheets. imgserve serves the new
                               /library.json at once (new ETag), blazingmountd sees the ETag move, the app reloads.

Markus, 24 Oct 2026: "If i had a moment one day where i wanted to play a game i hadnt played in 30 years... i could." The library is
not only the drives: it is everything he owns (his owned list, blazing-ps5 docs/D5-WHAT-MARKUS-HAD.md: PSN library + the Xbox/PC/
Nintendo/Sega/retro sections he added on 15 Oct) joined with what is on mac1's two game drives and what imgserve streams now.

One entry = one game on one system (regional/edition variants of a title are grouped). Each entry says:
  owned (owned / ps_plus / licence / package / on_drive), on the drives (path, type, size, compressed, verified), streamable now
  (the imgserve key BlazingMount builds a starter from), and which console or emulator plays it (PLAYS below).

Covers, each fetched ONCE (state in ~/.blazing/covers/state.jsonl; a miss is remembered too, so no re-asking):
  1. the game's own icon0.png (PS5 images through imgserve /t/<key>/sce_sys/icon0.png, PS4/PS5 folders on the drives)
  2. libretro-thumbnails Named_Boxarts (https://github.com/libretro-thumbnails, exact Redump/No-Intro names, else a normalised match)
  3. IGDB (api.igdb.com v4 /games search, Twitch app token from ~/.credentials/api-keys.env, <= 4 requests/s)
Run it WITHOUT python's -I on mac1: PIL is a user package there.
Then: full/<id>.jpg (<= 1024 px), thumb/<id>.jpg (fit in 256 px), a placeholder colour (the cover's average), and sprite sheets
(64 thumbs of 128 px in one 1024 x 1024 JPEG, in library order) so a page of the grid is ONE request. Every source is in sources.tsv.
JPEG q85 on the app's black: about 3 GB for 18k titles instead of about 10 GB as PNG (space hygiene).
"""
import argparse, collections, hashlib, html, http.server, json, os, re, subprocess, sys, threading, time, urllib.parse, urllib.request

HOME = os.path.expanduser("~")
LIB = os.path.join(HOME, ".blazing", "library")
COV = os.path.join(HOME, ".blazing", "covers")
DRIVES = ["/Volumes/My Book", "/Volumes/Untitled"]
IMGSERVE = "http://127.0.0.1:4451"
UA = "BlazingMount-library/1.0 (personal library covers)"

# ---------------------------------------------------------------- systems: key -> (label, family, libretro repo, IGDB platform id, plays on)
S = {}
def sysdef(key, label, family, libretro, igdb, plays):
    S[key] = dict(key=key, label=label, family=family, libretro=libretro, igdb=igdb, plays=plays)
sysdef("ps5", "PS5", "PlayStation", None, 167, "PS5: streams with BlazingMount (starter + the image on the drives)")
PS5_NOT_PACKED = "PS5: BlazingMount streams it once it is packed (.ffpfsc) on the drives"
PS5_NOT_ON_DRIVES = "PS5: not on the drives yet (BlazingMount streams it once a packed copy is there)"
sysdef("ps4", "PS4", "PlayStation", "Sony_-_PlayStation_4", 48, "PS5: PS4 games stream through the link (PHAT5, Freedom Cry proven 23 Oct)")
sysdef("ps3", "PS3", "PlayStation", "Sony_-_PlayStation_3", 9, "RPCS3 (Dell / PS5 port)")
sysdef("ps2", "PS2", "PlayStation", "Sony_-_PlayStation_2", 8, "PS5: PS5SX2 (120 fps PS2 emulator)")
sysdef("ps1", "PS1", "PlayStation", "Sony_-_PlayStation", 7, "PS5: BlazingArch (RetroArch)")
sysdef("psp", "PSP", "PlayStation", "Sony_-_PlayStation_Portable", 38, "PS5: BlazingArch (PPSSPP)")
sysdef("vita", "PS Vita", "PlayStation", "Sony_-_PlayStation_Vita", 46, "Vita3K (Dell)")
sysdef("switch", "Switch", "Nintendo", None, 130, "PS5: Eden; iPhone: nxnative (native)")
sysdef("switch2", "Switch 2", "Nintendo", None, 508, "Switch 2")
sysdef("wiiu", "Wii U", "Nintendo", "Nintendo_-_Wii_U", 41, "Cemu (Dell)")
sysdef("wii", "Wii", "Nintendo", "Nintendo_-_Wii", 5, "PS5: BlazingArch (Dolphin)")
sysdef("gc", "GameCube", "Nintendo", "Nintendo_-_GameCube", 21, "PS5: BlazingArch (Dolphin)")
sysdef("n64", "N64", "Nintendo", "Nintendo_-_Nintendo_64", 4, "PS5: BlazingArch (Mupen64Plus)")
sysdef("snes", "SNES", "Nintendo", "Nintendo_-_Super_Nintendo_Entertainment_System", 19, "PS5: BlazingArch (Snes9x)")
sysdef("nes", "NES", "Nintendo", "Nintendo_-_Nintendo_Entertainment_System", 18, "PS5: BlazingArch (Mesen)")
sysdef("fds", "Famicom Disk System", "Nintendo", "Nintendo_-_Family_Computer_Disk_System", 51, "PS5: BlazingArch (Mesen)")
sysdef("gb", "Game Boy", "Nintendo", "Nintendo_-_Game_Boy", 33, "PS5: BlazingArch (Gambatte)")
sysdef("gbc", "Game Boy Color", "Nintendo", "Nintendo_-_Game_Boy_Color", 22, "PS5: BlazingArch (Gambatte)")
sysdef("gba", "Game Boy Advance", "Nintendo", "Nintendo_-_Game_Boy_Advance", 24, "PS5: BlazingArch (mGBA)")
sysdef("ds", "DS", "Nintendo", "Nintendo_-_Nintendo_DS", 20, "PS5: BlazingArch (melonDS)")
sysdef("3ds", "3DS", "Nintendo", "Nintendo_-_Nintendo_3DS", 37, "Azahar (Dell)")
sysdef("vb", "Virtual Boy", "Nintendo", "Nintendo_-_Virtual_Boy", 87, "PS5: BlazingArch (Beetle VB)")
sysdef("pokemini", "Pokemon mini", "Nintendo", "Nintendo_-_Pokemon_Mini", 166, "PS5: BlazingArch (PokeMini)")
sysdef("xbox", "Xbox", "Xbox", "Microsoft_-_Xbox", 11, "xemu (Dell)")
sysdef("x360", "Xbox 360", "Xbox", "Microsoft_-_Xbox_360", 12, "Xbox 360 (JTAG/RGH) / Xenia")
sysdef("xone", "Xbox One", "Xbox", None, 49, "Xbox One S")
sysdef("xsx", "Xbox Series X/S", "Xbox", None, 169, "Xbox Series X/S (cloud) / PC")
sysdef("pc", "PC", "PC", None, 6, "Dell PC / PS5: Prospero Win")
sysdef("dc", "Dreamcast", "Sega", "Sega_-_Dreamcast", 23, "PS5: BlazingArch (Flycast)")
sysdef("saturn", "Saturn", "Sega", "Sega_-_Saturn", 32, "PS5: BlazingArch (Beetle Saturn)")
sysdef("genesis", "Genesis", "Sega", "Sega_-_Mega_Drive_-_Genesis", 29, "PS5: BlazingArch (Genesis Plus GX)")
sysdef("segacd", "Sega CD", "Sega", "Sega_-_Mega-CD_-_Sega_CD", 78, "PS5: BlazingArch (Genesis Plus GX)")
sysdef("32x", "32X", "Sega", "Sega_-_32X", 30, "PS5: BlazingArch (PicoDrive)")
sysdef("sms", "Master System", "Sega", "Sega_-_Master_System_-_Mark_III", 64, "PS5: BlazingArch (Genesis Plus GX)")
sysdef("gg", "Game Gear", "Sega", "Sega_-_Game_Gear", 35, "PS5: BlazingArch (Genesis Plus GX)")
sysdef("tg16", "TurboGrafx-16", "Retro", "NEC_-_PC_Engine_-_TurboGrafx_16", 86, "PS5: BlazingArch (Beetle PCE)")
sysdef("neogeo", "Neo Geo", "Retro", "SNK_-_Neo_Geo", 80, "PS5: BlazingArch (FBNeo)")
sysdef("ngp", "Neo Geo Pocket", "Retro", "SNK_-_Neo_Geo_Pocket", 119, "PS5: BlazingArch (Beetle NGP)")
sysdef("a2600", "Atari 2600", "Retro", "Atari_-_2600", 59, "PS5: BlazingArch (Stella)")
sysdef("a5200", "Atari 5200", "Retro", "Atari_-_5200", 66, "PS5: BlazingArch (a5200)")
sysdef("a7800", "Atari 7800", "Retro", "Atari_-_7800", 60, "PS5: BlazingArch (ProSystem)")
sysdef("jaguar", "Atari Jaguar", "Retro", "Atari_-_Jaguar", 62, "PS5: BlazingArch (Virtual Jaguar)")
sysdef("lynx", "Atari Lynx", "Retro", "Atari_-_Lynx", 61, "PS5: BlazingArch (Handy)")
sysdef("coleco", "ColecoVision", "Retro", "Coleco_-_ColecoVision", 68, "PS5: BlazingArch (blueMSX)")
sysdef("intv", "Intellivision", "Retro", "Mattel_-_Intellivision", 67, "PS5: BlazingArch (FreeIntv)")
sysdef("vectrex", "Vectrex", "Retro", "GCE_-_Vectrex", 70, "PS5: BlazingArch (vecx)")
sysdef("o2", "Odyssey 2", "Retro", "Magnavox_-_Odyssey2", 133, "PS5: BlazingArch (O2EM)")
sysdef("msx", "MSX", "Retro", "Microsoft_-_MSX", 27, "PS5: BlazingArch (blueMSX)")
sysdef("c64", "Commodore 64", "Retro", "Commodore_-_64", 15, "PS5: BlazingArch (VICE)")
sysdef("amiga", "Amiga", "Retro", "Commodore_-_Amiga", 16, "PS5: BlazingArch (PUAE)")
sysdef("cpc", "Amstrad CPC", "Retro", "Amstrad_-_CPC", 25, "PS5: BlazingArch (Caprice32)")
sysdef("zx", "ZX Spectrum", "Retro", "Sinclair_-_ZX_Spectrum", 26, "PS5: BlazingArch (Fuse)")
sysdef("wswan", "WonderSwan", "Retro", "Bandai_-_WonderSwan", 57, "PS5: BlazingArch (Beetle Cygne)")
sysdef("arcade", "Arcade", "Retro", "MAME", 52, "PS5: BlazingArch (MAME / FBNeo)")
sysdef("ios", "iOS", "Mobile", None, 39, "iPhone")
sysdef("android", "Android", "Mobile", None, 34, "Android")
sysdef("other", "Other", "Retro", None, None, "")

ALIAS = {  # owned-list platform names and drive folder names -> system key
    "ps5": "ps5", "ps4": "ps4", "ps3": "ps3", "ps2": "ps2", "ps1": "ps1", "playstation": "ps1", "playstation 2": "ps2", "psp": "psp",
    "ps vita": "vita", "switch": "switch", "switch 2": "switch2", "wii u": "wiiu", "wii": "wii", "gamecube": "gc", "n64": "n64",
    "nintendo 64": "n64", "snes": "snes", "nes": "nes", "famicom disk system": "fds", "game boy": "gb", "game boy color": "gbc",
    "game boy advance": "gba", "ds": "ds", "nintendo ds": "ds", "3ds": "3ds", "nintendo 3ds": "3ds", "virtual boy": "vb",
    "pokemon mini": "pokemini", "xbox (original)": "xbox", "xbox": "xbox", "xbox 360": "x360", "xbox 360 (jtag-rgh)": "x360",
    "xbox one": "xone", "xbox series x/s": "xsx", "pc": "pc", "pc ports": "pc", "dreamcast": "dc", "saturn": "saturn",
    "genesis": "genesis", "sega cd": "segacd", "32x": "32x", "master system": "sms", "game gear": "gg", "turbografx-16": "tg16",
    "pc engine": "tg16", "neo geo": "neogeo", "neo geo pocket": "ngp", "atari 2600": "a2600", "atari 5200": "a5200",
    "atari 7800": "a7800", "atari jaguar": "jaguar", "atari lynx": "lynx", "colecovision": "coleco", "intellivision": "intv",
    "vectrex": "vectrex", "odyssey 2": "o2", "msx": "msx", "commodore 64": "c64", "amiga": "amiga", "amstrad cpc": "cpc",
    "zx spectrum": "zx", "wonderswan": "wswan", "arcade": "arcade", "mame": "arcade", "ios": "ios", "android": "android",
}

JUNK = re.compile(r"^(system volume information|what-is-on-this-drive|settings|other|readme.*|.*\bbios\b.*|scph\d+.*)$", re.I)
SIDE = re.compile(r"\.(json|sha256|md5|txt|log|nfo|sfv|jpg|png|part|tmp|cue|m3u|sav|srm|state\d*)$", re.I)
COMPRESSED = {"ffpfsc", "7z", "zip", "rar", "chd", "rvz", "cso", "nsz", "xcz", "gz", "wua", "z64.zip"}
TIDRE = re.compile(r"\b((?:PPSA|CUSA)\d{5}|BL[EUJ]S\d{5}|NP[EUJ][A-Z]\d{5}|SL[EUP][SM][- ]?\d{5}|0100[0-9A-F]{12})\b")

def the_first(t):
    """'Legend of Zelda, The - Twilight Princess' -> 'The Legend of Zelda - Twilight Princess' (No-Intro / TOSEC article order)"""
    return re.sub(r"^(.+?), (The|A|An)\b(?=\s*(-|:|\(|$))", r"\2 \1", t)

def display(t):
    """a name as people say it: no region/language/dump tags, no .nkit, article first"""
    t = the_first(clean_title(t))
    t = re.sub(r"\.nkit$|\s+Rev \d+$|\s+Rev \d+(?=\s*\()", "", t)
    t = re.sub(r"\s*(\([^)]*\)|\[[^\]]*\])", "", t)
    return re.sub(r"\s+", " ", t).strip(" -") or t

def norm(t):
    t = the_first(html.unescape(t))
    t = re.sub(r"\[[^\]]*\]|\([^)]*\)", " ", t)                       # (USA) (En,Fr) [0100...] (PPSA...)
    t = re.sub(r"\.nkit\b|\b[Rr]ev \d+\b|\bv\d+(\.\d+)+\b", " ", t)
    t = t.replace("®", "").replace("™", "").replace("’", "'").replace("&", " and ")
    t = re.sub(r"\b(psn|ps4|ps5|ps3|deluxe edition|complete edition|digital deluxe edition|game of the year edition|goty edition|"
               r"definitive edition|remastered|standard edition|ultimate edition|gold edition|bp4\.xx for phat5|for phat5|digi5)\b", " ", t, flags=re.I)
    t = re.sub(r"[^0-9a-zA-Z]+", " ", t).strip().lower()
    t = re.sub(r"^the ", "", t)
    return re.sub(r"\s+", " ", t)

def eid(system, title):
    return hashlib.sha1(("%s|%s" % (system, norm(title))).encode()).hexdigest()[:12]

def clean_title(t):
    return re.sub(r"\s+", " ", re.sub(r"\s*\((PPSA|CUSA|BL[EUJ]S)\d{5}\)|\s*\[0100[0-9A-F]{12}\]", "", t)).strip()

# ---------------------------------------------------------------- 1. the owned list
def owned_rows(path):
    sec, rows = None, []
    for line in open(path, encoding="utf-8"):
        if line.startswith("## "):
            sec = line[3:].strip(); continue
        if not line.startswith("| ") or line.startswith("| Game") or line.startswith("|---") or (sec or "").startswith("Installed on"):
            continue
        f = [x.strip() for x in line.strip().strip("|").split("|")]
        if len(f) < 8:
            continue
        system = ALIAS.get(f[2].lower())
        if not system:
            continue
        his = f[3]
        how = ("ps_plus" if his.startswith("PS Plus") else "licence" if his.startswith("licence") else
               "package" if his.startswith("installed from") else "free" if his.startswith("comes free") else "owned")
        rows.append(dict(title=f[0], ids=[i.strip() for i in f[1].split(",") if i.strip() not in ("-", "")], system=system, how=how,
                         ftype=f[6] if f[6] != "-" else "", size=f[7] if f[7] != "-" else ""))
    return rows

# ---------------------------------------------------------------- 2. the drives
DRIVE_DIRS = [("PS5 Games", "ps5"), ("PS5-DOWNLOADS", "ps5"), ("PS5 Games - not packed yet", "ps5"), ("homebrew", "ps5"), ("PS4 Games", "ps4"),
              ("PS3 Games", "ps3"), ("PS2 Games", "ps2"), ("Switch Games", "switch"), ("Xbox Games/Xbox", "xbox"),
              ("Xbox Games/Xbox 360 (JTAG-RGH)", "x360"), ("games", "x360")]

def du(p):
    if os.path.isfile(p):
        return os.path.getsize(p)
    n = 0
    for dp, dn, fn in os.walk(p):
        for x in fn:
            try: n += os.path.getsize(os.path.join(dp, x))
            except OSError: pass
    return n

def drive_items(sizes=True):
    out = []
    def add(system, drive, rel, name, full):
        if name.startswith((".", "_", "$")) or SIDE.search(name) or JUNK.match(name.rsplit(".", 1)[0] if "." in name else name):
            return
        isdir = os.path.isdir(full)
        ext = "" if isdir else name.rsplit(".", 1)[-1].lower() if "." in name else ""
        title = name if isdir else name.rsplit(".", 1)[0] if "." in name else name
        m = TIDRE.search(name)
        icon = os.path.join(full, "sce_sys", "icon0.png") if isdir else ""
        out.append(dict(system=system, title=title, tid=m.group(1) if m else "", path=os.path.join(drive, rel, name),
                        drive=os.path.basename(drive), type=ext or "folder", compressed=ext in COMPRESSED,
                        verified=os.path.exists(full + ".verified.json"), size=du(full) if sizes else 0,
                        icon=icon if icon and os.path.exists(icon) else ""))
    for drive in DRIVES:
        if not os.path.isdir(drive):
            continue
        for rel, system in DRIVE_DIRS:
            d = os.path.join(drive, rel)
            if os.path.isdir(d):
                for name in sorted(os.listdir(d)):
                    add(system, drive, rel, name, os.path.join(d, name))
        retro = os.path.join(drive, "Retro Games")
        if os.path.isdir(retro):
            for sysname in sorted(os.listdir(retro)):
                system = ALIAS.get(sysname.lower(), "other")
                sd = os.path.join(retro, sysname)
                if not os.path.isdir(sd):
                    continue
                for name in sorted(os.listdir(sd)):
                    full = os.path.join(sd, name)
                    if os.path.isdir(full) and system not in ("ps1", "ps2", "dc", "saturn", "gc", "wii", "pc"):
                        for sub in sorted(os.listdir(full)):   # one level of sub-folders (packs)
                            add(system, drive, os.path.join("Retro Games", sysname, name), sub, os.path.join(full, sub))
                    else:
                        add(system, drive, os.path.join("Retro Games", sysname), name, full)
    return out

def imgserve_games():
    try:
        with urllib.request.urlopen(IMGSERVE + "/games.json", timeout=15) as r:
            return json.loads(r.read())
    except Exception:
        return []

# ---------------------------------------------------------------- index
def cmd_index(a):
    os.makedirs(LIB, exist_ok=True)
    owned = owned_rows(a.owned)
    drives = drive_items(sizes=not a.fast)
    served = imgserve_games()
    by = collections.OrderedDict()
    tid_ix = {}
    def entry(system, title):
        k = (system, norm(title) or title.lower())
        if k not in by:
            by[k] = dict(id=eid(system, title), title=display(title), system=system, family=S[system]["family"], how="", ids=[],
                         variants=[], files=[], stream=None, plays=S[system]["plays"])
        e = by[k]
        if title not in e["variants"]:
            e["variants"].append(title)
            if "(USA" in title and not any("(USA" in v for v in e["variants"][:-1]): e["title"] = display(title)
        return e
    rank = {"owned": 0, "free": 1, "package": 2, "licence": 3, "ps_plus": 4, "": 9}
    for r in owned:
        e = entry(r["system"], r["title"])
        if rank[r["how"]] < rank.get(e["how"], 9): e["how"] = r["how"]
        for i in r["ids"]:
            if i not in e["ids"]: e["ids"].append(i)
            tid_ix.setdefault((r["system"], i.replace(" ", "").replace("-", "")), e)
    for d in drives:
        e = tid_ix.get((d["system"], d["tid"])) if d["tid"] else None
        if not e: e = entry(d["system"], d["title"])
        if d["tid"] and d["tid"] not in e["ids"]: e["ids"].append(d["tid"])
        e["files"].append({k: d[k] for k in ("path", "drive", "type", "compressed", "verified", "size", "icon")})
        if not e["how"]: e["how"] = "on_drive"
    for g in served:
        e = tid_ix.get(("ps5", g["tid"])) or next((x for x in by.values() if x["system"] == "ps5" and g["tid"] in x["ids"]), None)
        if not e:
            e = entry("ps5", g["name"].rsplit(".", 1)[0]); e["ids"].append(g["tid"]); e["how"] = e["how"] or "on_drive"
        e["stream"] = dict(key=g["key"], tid=g["tid"], version=g.get("version", ""), fw=g.get("fw", ""))
    for e in by.values():   # a PS5 game streams only from a packed image imgserve serves: say what is missing instead
        if e["system"] == "ps5" and not e["stream"]:
            e["plays"] = PS5_NOT_PACKED if e["files"] else PS5_NOT_ON_DRIVES
    games = sorted(by.values(), key=lambda e: (list(S).index(e["system"]), norm(e["title"])))
    systems = collections.Counter(e["system"] for e in games)
    body = dict(version=1, made=time.strftime("%Y-%m-%d %H:%M:%S"), source=dict(owned=a.owned, drives=DRIVES, imgserve=IMGSERVE),
                systems=[dict(key=k, label=S[k]["label"], family=S[k]["family"], count=systems[k], plays=S[k]["plays"]) for k in S if systems[k]],
                count=len(games), games=games)
    data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
    etag = hashlib.sha256(json.dumps(games, sort_keys=True).encode()).hexdigest()[:16]
    tmp = os.path.join(LIB, "library.json.tmp")
    open(tmp, "wb").write(data); os.replace(tmp, os.path.join(LIB, "library.json"))
    open(os.path.join(LIB, "library.etag"), "w").write(etag + "\n")
    print("library: %d games on %d systems, %d on the drives, %d streamable, %.1f MB, etag %s" % (
        len(games), len(systems), sum(1 for e in games if e["files"]), sum(1 for e in games if e["stream"]), len(data) / 1e6, etag))
    for k, n in sorted(systems.items(), key=lambda x: -x[1]): print("  %-22s %6d" % (S[k]["label"], n))

# ---------------------------------------------------------------- covers
def load_state():
    st = {}
    p = os.path.join(COV, "state.jsonl")
    if os.path.exists(p):
        for l in open(p):
            try: d = json.loads(l); st[d["id"]] = d
            except Exception: pass
    return st

def save_state(d):
    with open(os.path.join(COV, "state.jsonl"), "a") as f: f.write(json.dumps(d) + "\n")

def http_get(url, data=None, headers=None, timeout=30):
    req = urllib.request.Request(url, data=data, headers=dict({"User-Agent": UA}, **(headers or {})))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()

_tree = {}
def libretro_names(repo):
    """{normalised name: file name} of a libretro-thumbnails repo's Named_Boxarts (cached in ~/.blazing/covers/libretro/)."""
    if repo in _tree: return _tree[repo]
    cache = os.path.join(COV, "libretro", repo + ".json")
    if not os.path.exists(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        names = []
        try:
            t = json.loads(http_get("https://api.github.com/repos/libretro-thumbnails/%s/git/trees/master?recursive=1" % repo,
                                    headers=gh_headers()))
            names = [x["path"][len("Named_Boxarts/"):] for x in t.get("tree", []) if x["path"].startswith("Named_Boxarts/") and x["path"].endswith(".png")]
        except Exception as e:
            print("  libretro %s: %s" % (repo, e))
        json.dump(names, open(cache, "w"))
    names = json.load(open(cache))
    exact = {n[:-4]: n for n in names}
    loose = {}
    for n in names:
        loose.setdefault(norm(n[:-4]), n)
    _tree[repo] = (exact, loose)
    return _tree[repo]

def gh_headers():
    try:
        tok = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=10).stdout.strip()
        return {"Authorization": "Bearer " + tok} if tok else {}
    except Exception:
        return {}

def libretro_url(e):
    repo = S[e["system"]]["libretro"]
    if not repo: return None
    exact, loose = libretro_names(repo)
    names = sorted(e["variants"], key=lambda v: (0 if "(USA" in v else 1 if "(World" in v else 2 if "(Europe" in v else 3))   # his region first
    for t in names + [e["title"]] + [os.path.basename(f["path"]).rsplit(".", 1)[0] for f in e["files"]]:
        safe = re.sub(r'[&*/:`<>?\\|"]', "_", t)   # libretro's file-name rule
        if safe in exact:
            return "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s" % (repo, urllib.parse.quote(exact[safe]))
    for t in [e["title"]] + e["variants"]:
        n = loose.get(norm(t))
        if n:
            return "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s" % (repo, urllib.parse.quote(n))
    return None

class Igdb:
    def __init__(self):
        env = {}
        for l in open(os.path.join(HOME, ".credentials", "api-keys.env")):
            m = re.match(r"(?:export )?([A-Z_]+)=(.*)", l.strip())
            if m: env[m.group(1)] = m.group(2).strip().strip("'\"")
        self.cid = env.get("IGDB_CLIENT_ID") or env.get("TWITCH_CLIENT_ID")
        sec = env.get("IGDB_CLIENT_SECRET") or env.get("TWITCH_CLIENT_SECRET")
        tok = json.loads(http_get("https://id.twitch.tv/oauth2/token", data=urllib.parse.urlencode(
            dict(client_id=self.cid, client_secret=sec, grant_type="client_credentials")).encode()))
        self.token = tok["access_token"]
        self.last = 0.0

    def cover(self, title, plat):
        """the best cover for a title: an exact (normalised) name first, then one on the right platform, then the first hit"""
        t = display(title).replace('"', " ")
        wait = 0.26 - (time.time() - self.last)   # IGDB: 4 requests a second
        if wait > 0: time.sleep(wait)
        self.last = time.time()
        res = json.loads(http_get("https://api.igdb.com/v4/games", data=('fields name,cover.image_id,platforms; search "%s"; '
                                  'where cover != null; limit 6;' % t).encode(),
                                  headers={"Client-ID": self.cid, "Authorization": "Bearer " + self.token}))
        if not res: return None
        want = norm(t)
        best = max(res, key=lambda r: (norm(r.get("name", "")) == want) * 2 + (plat in (r.get("platforms") or [])))
        img = best.get("cover", {}).get("image_id")
        self.matched = best.get("name", "")
        return "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/%s.jpg" % img if img else None
    # (IGDB multiquery answers [] to `search`, 24 Oct 2026: one query per title)

def make_images(cid, raw):
    """raw bytes (PNG/JPEG) -> full/<id>.png (<= 1024), thumb/<id>.png (fit 256), the average colour; None when unreadable."""
    from io import BytesIO
    from PIL import Image
    try:
        im = Image.open(BytesIO(raw)); im.load(); im = im.convert("RGBA")
    except Exception:
        return None
    flat = Image.new("RGB", im.size, (10, 10, 11)); flat.paste(im, mask=im.split()[3])     # covers go on the app's black (#0A0A0B)
    full = flat.copy(); full.thumbnail((1024, 1024), Image.LANCZOS); full.save(os.path.join(COV, "full", cid + ".jpg"), quality=85, optimize=True)
    th = flat.copy(); th.thumbnail((256, 256), Image.LANCZOS); th.save(os.path.join(COV, "thumb", cid + ".jpg"), quality=85, optimize=True)
    r, g, b, _ = im.convert("RGBA").resize((1, 1), Image.BOX).getpixel((0, 0))
    return "#%02X%02X%02X" % (r, g, b), im.size

def cmd_covers(a):
    for d in ("src", "full", "thumb", "sheet", "libretro"): os.makedirs(os.path.join(COV, d), exist_ok=True)
    lib = json.load(open(os.path.join(LIB, "library.json")))
    st = load_state()
    todo = [e for e in lib["games"] if e["id"] not in st and (not a.only or e["system"] == a.only)]
    if a.limit: todo = todo[:a.limit]
    print("covers: %d games, %d already settled, %d to fetch" % (len(lib["games"]), len(st), len(todo)))
    src_log = open(os.path.join(COV, "sources.tsv"), "a")
    def land(e, url, raw, source, matched=""):
        r = make_images(e["id"], raw)
        if not r:
            save_state(dict(id=e["id"], ok=False, source=source, url=url, why="unreadable image")); return False
        colour, size = r
        save_state(dict(id=e["id"], ok=True, source=source, url=url, colour=colour, w=size[0], h=size[1], matched=matched, at=int(time.time())))
        src_log.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (e["id"], e["system"], e["title"], source, url, matched)); src_log.flush()
        return True
    need_igdb, done, n = [], 0, 0
    for e in todo:
        n += 1
        url = raw = None; source = ""
        icons = [f["icon"] for f in e["files"] if f.get("icon")]
        if icons:
            url, source, raw = "file://" + icons[0], "icon0", open(icons[0], "rb").read()
        elif e.get("stream"):
            url, source = IMGSERVE + "/t/%s/sce_sys/icon0.png" % e["stream"]["key"], "icon0"
            try: raw = http_get(url, timeout=20)
            except Exception: raw = None
        if not raw:
            url = libretro_url(e)
            if url:
                source = "libretro"
                try: raw = http_get(url, timeout=30)
                except Exception: raw = None
        if raw and land(e, url, raw, source): done += 1; continue
        if S[e["system"]]["igdb"] is not None or e["system"] == "other": need_igdb.append(e)
        else: save_state(dict(id=e["id"], ok=False, source="", why="no source"))
        if n % 200 == 0: print("  %d / %d (%d covers)" % (n, len(todo), done), flush=True)
    print("  icon0 + libretro: %d covers; %d go to IGDB" % (done, len(need_igdb)), flush=True)
    if need_igdb:
        ig = Igdb()
        for i, e in enumerate(need_igdb):
            try:
                url = ig.cover(e["title"], S[e["system"]]["igdb"])
            except Exception as ex:
                print("  IGDB failed for %s (%s): retried next run" % (e["title"], ex)); time.sleep(2); continue
            if not url:
                save_state(dict(id=e["id"], ok=False, source="igdb", why="no match")); continue
            try: raw = http_get(url, timeout=30)
            except Exception: save_state(dict(id=e["id"], ok=False, source="igdb", url=url, why="download failed")); continue
            done += land(e, url, raw, "igdb", ig.matched)
            if i % 200 == 0: print("  IGDB %d / %d (%d covers so far)" % (i, len(need_igdb), done), flush=True)
    print("covers: %d new; %s" % (done, stats_line()))

def stats_line():
    st = load_state()
    ok = collections.Counter(d.get("source") for d in st.values() if d.get("ok"))
    return "%d settled: %d covers (%s), %d without" % (len(st), sum(ok.values()), ", ".join("%s %d" % kv for kv in ok.most_common()),
                                                      sum(1 for d in st.values() if not d.get("ok")))

# ---------------------------------------------------------------- sheets: 64 thumbs of 128 px a page, in library order
def cmd_sheets(a):
    from PIL import Image
    lib = json.load(open(os.path.join(LIB, "library.json")))
    st = load_state()
    cells, per = 128, 64
    index = {}
    pages = (len(lib["games"]) + per - 1) // per
    for p in range(pages):
        sheet = Image.new("RGB", (1024, 1024), (10, 10, 11))
        for i, e in enumerate(lib["games"][p * per:(p + 1) * per]):
            s = st.get(e["id"], {})
            index[e["id"]] = dict(page=p, cell=i, colour=s.get("colour", ""), cover=bool(s.get("ok")))
            if not s.get("ok"): continue
            th = Image.open(os.path.join(COV, "thumb", e["id"] + ".jpg")).convert("RGB")
            th.thumbnail((cells, cells), Image.LANCZOS)
            sheet.paste(th, ((i % 8) * cells + (cells - th.width) // 2, (i // 8) * cells + (cells - th.height) // 2))
        sheet.save(os.path.join(COV, "sheet", "%d.jpg" % p), quality=85, optimize=True)
    json.dump(dict(etag=open(os.path.join(LIB, "library.etag")).read().strip(), cell=cells, per_sheet=per, covers=index),
              open(os.path.join(COV, "covers.json"), "w"), separators=(",", ":"))
    print("sheets: %d pages of %d; covers.json %d entries" % (pages, per, len(index)))

# ---------------------------------------------------------------- DISCOVER (phase 3, docs/UI-DESIGN.md 10.5): games he does not have yet
# discover.json = shelves (new, soon, popular, notable, wishlist) of IGDB games joined against library.json, an `alerts` list made
# from the wishlist (imgserve writes wishlist.json; this only reads it), covers in their own sprite sheets (dsheet/<n>.jpg).
# Runs only from the visible watch loop: when discover.json is older than 24 h, or at once when imgserve left `discover.wanted`.
CURRENT = ["ps5", "ps4", "switch", "switch2", "pc", "xsx", "xone"]      # new / coming soon / popular: the systems that still get games
DAY = 86400
SHELVES = [("new", "New releases"), ("soon", "Coming soon"), ("popular", "Popular"), ("notable", "Notable I don't own"), ("wishlist", "Wishlist")]
GAME_FIELDS = ("fields name,cover.image_id,first_release_date,release_dates.date,release_dates.platform,platforms,hypes,total_rating,"
               "total_rating_count,summary,version_parent;")

def did(igdb_id):
    return hashlib.sha1(("igdb:%d" % int(igdb_id)).encode()).hexdigest()[:12]

def igdb_query(ig, endpoint, body):
    """one IGDB v4 request at <= 4 a second; one retry after a 429"""
    for attempt in (0, 1):
        wait = 0.26 - (time.time() - ig.last)
        if wait > 0: time.sleep(wait)
        ig.last = time.time()
        try:
            return json.loads(http_get("https://api.igdb.com/v4/" + endpoint, data=body.encode(),
                                       headers={"Client-ID": ig.cid, "Authorization": "Bearer " + ig.token}))
        except urllib.error.HTTPError as e:
            if e.code != 429 or attempt: raise
            time.sleep(1.5)

def his_systems(lib, minimum=3):
    """system key -> IGDB platform id, for every system he has at least `minimum` games on"""
    return {s["key"]: S[s["key"]]["igdb"] for s in lib["systems"] if s["count"] >= minimum and S.get(s["key"], {}).get("igdb")}

def owned_index(lib):
    by_id = {g["id"]: g for g in lib["games"]}
    by_norm = {}
    for g in lib["games"]: by_norm.setdefault(norm(g["title"]), g)
    return by_id, by_norm

def join_owned(name, systems, by_id, by_norm):
    """the library entry that is this game: eid(system, title) on one of its systems first, then the normalised title anywhere"""
    for k in systems:
        g = by_id.get(eid(k, name))
        if g: return g
    return by_norm.get(norm(name))

def discover_entry(r, plat2sys, by_id, by_norm, today):
    systems = [plat2sys[p] for p in (r.get("platforms") or []) if p in plat2sys]
    dates = [d["date"] for d in (r.get("release_dates") or []) if d.get("date") and d.get("platform") in plat2sys]
    date = min(dates) if dates else (r.get("first_release_date") or 0)
    g = join_owned(r.get("name", ""), systems, by_id, by_norm)
    summ = (r.get("summary") or "").replace("\n", " ").strip()
    if len(summ) > 300: summ = summ[:297].rsplit(" ", 1)[0] + "..."
    return dict(id=did(r["id"]), igdb=r["id"], title=r.get("name", ""), date=int(date), date_text=time.strftime("%-d %b %Y", time.localtime(date)) if date else "",
                systems=systems, sys_labels=", ".join(S[k]["label"] for k in systems), hype=int(r.get("hypes") or 0),
                rating=int(round(r.get("total_rating") or 0)), votes=int(r.get("total_rating_count") or 0), summary=summ,
                owned=(g["how"] or "owned") if g else "", on_drives=bool(g and g["files"]), lib_id=g["id"] if g else "",
                cover_image=(r.get("cover") or {}).get("image_id", ""), shelves=[])

def make_alerts(wish_items, current, today):
    """wishlist -> alerts. out: its date has arrived (and is at most 60 days old); moved: IGDB's date differs from the date it had when added"""
    out = []
    for w in wish_items:
        cur = int(current.get(w["id"], w.get("date") or 0) or 0)
        if cur and today - 60 * DAY < cur <= today:
            out.append(dict(id=w["id"], title=w.get("title", ""), kind="out", date=cur, key="%s:out:%d" % (w["id"], cur)))
        elif cur and w.get("date") and cur != int(w["date"]) and cur > today:
            out.append(dict(id=w["id"], title=w.get("title", ""), kind="moved", date=cur, key="%s:moved:%d" % (w["id"], cur)))
    for a in out: a["date_text"] = time.strftime("%-d %b %Y", time.localtime(a["date"]))
    return out

def load_wishlist():
    try: return json.load(open(os.path.join(LIB, "wishlist.json"))).get("items", [])
    except (OSError, ValueError): return []

def write_discover(body):
    """discover.json + discover.etag, each renamed into place (a console never reads half a file); ETag = the content without `made`"""
    etag = hashlib.sha256(json.dumps({k: v for k, v in body.items() if k != "made"}, sort_keys=True).encode()).hexdigest()[:16]
    body["etag"] = etag
    tmp = os.path.join(LIB, "discover.json.tmp")
    open(tmp, "w").write(json.dumps(body, ensure_ascii=False, separators=(",", ":"))); os.replace(tmp, os.path.join(LIB, "discover.json"))
    open(tmp, "w").write(etag + "\n"); os.replace(tmp, os.path.join(LIB, "discover.etag"))
    return etag

def cmd_discover(a, ig=None, today=None):
    lib = json.load(open(os.path.join(LIB, "library.json")))
    today = int(today or time.time())
    ig = ig or Igdb()
    mine = his_systems(lib)
    plat2sys = {pid: k for k, pid in mine.items()}
    cur_plats = ",".join(str(mine[k]) for k in CURRENT if k in mine)
    by_id, by_norm = owned_index(lib)
    games, shelves = {}, {k: [] for k, _ in SHELVES}
    def add(rows, shelf, keep, want=lambda e: True):
        n = 0
        for r in rows:
            if n >= keep: break
            if not r.get("id") or not (r.get("cover") or {}).get("image_id"): continue
            e = games.get(did(r["id"])) or discover_entry(r, plat2sys, by_id, by_norm, today)
            if not e["systems"] or not want(e): continue
            games[e["id"]] = e
            if shelf not in e["shelves"]: e["shelves"].append(shelf); shelves[shelf].append(e["id"])
            n += 1
    if cur_plats:
        new = igdb_query(ig, "games", GAME_FIELDS + " where first_release_date >= %d & first_release_date <= %d & platforms = (%s) & cover != null"
                         " & version_parent = null; sort first_release_date desc; limit 200;" % (today - 30 * DAY, today, cur_plats))
        new = [r for r in new if (r.get("hypes") or 0) + (r.get("total_rating_count") or 0) > 0]
        add(sorted(new, key=lambda r: -((r.get("hypes") or 0) + (r.get("total_rating_count") or 0))), "new", 48)
        soon = igdb_query(ig, "games", GAME_FIELDS + " where first_release_date > %d & first_release_date <= %d & platforms = (%s) & cover != null"
                          " & version_parent = null & hypes > 0; sort hypes desc; limit 120;" % (today, today + 120 * DAY, cur_plats))
        add(sorted(soon[:60], key=lambda r: r.get("first_release_date") or 0), "soon", 60)
        pop = igdb_query(ig, "popularity_primitives", "fields game_id,value; where popularity_type = 1; sort value desc; limit 200;")
        order = [p["game_id"] for p in pop if p.get("game_id")]
        if order:
            rows = igdb_query(ig, "games", GAME_FIELDS + " where id = (%s) & platforms = (%s); limit 200;" % (",".join(map(str, order)), cur_plats))
            rank = {gid: i for i, gid in enumerate(order)}
            add(sorted(rows, key=lambda r: rank.get(r["id"], 1 << 30)), "popular", 48)
    for k, pid in sorted(mine.items(), key=lambda kv: -next(s["count"] for s in lib["systems"] if s["key"] == kv[0])):
        rows = igdb_query(ig, "games", GAME_FIELDS + " where platforms = (%d) & total_rating_count >= 20 & cover != null & version_parent = null;"
                          " sort total_rating desc; limit 40;" % pid)
        add(rows, "notable", 10, want=lambda e: not e["owned"])
    wish = load_wishlist()
    ids = [int(w["igdb"]) for w in wish if w.get("igdb")]
    if ids:
        add(igdb_query(ig, "games", GAME_FIELDS + " where id = (%s); limit 500;" % ",".join(map(str, ids[:500]))), "wishlist", 500)
    covers_for(list(games.values()))
    order = [i for k, _ in SHELVES for i in shelves[k]]
    seen, flat = set(), []
    for i in order:
        if i not in seen: seen.add(i); flat.append(games[i])
    dsheets(flat)
    alerts = make_alerts(wish, {e["id"]: e["date"] for e in flat}, today)
    for e in flat: e.pop("cover_image", None)
    body = dict(version=1, made=time.strftime("%Y-%m-%d %H:%M:%S"), today=time.strftime("%Y-%m-%d", time.localtime(today)), alerts=alerts,
                shelves=[dict(key=k, label=l, ids=shelves[k]) for k, l in SHELVES], games=flat)
    etag = write_discover(body)
    print("discover: %s; %d alerts; etag %s" % (", ".join("%s %d" % (k, len(shelves[k])) for k, _ in SHELVES), len(alerts), etag), flush=True)

def cmd_alerts(a=None, today=None):
    """the alerts only (no IGDB): the wishlist against today and the dates in discover.json; rewrites discover.json only when they change"""
    p = os.path.join(LIB, "discover.json")
    try: body = json.load(open(p))
    except (OSError, ValueError): return False
    today = int(today or time.time())
    alerts = make_alerts(load_wishlist(), {e["id"]: e["date"] for e in body.get("games", [])}, today)
    if [x["key"] for x in alerts] == [x["key"] for x in body.get("alerts", [])]: return False
    body["alerts"] = alerts
    body["made"] = time.strftime("%Y-%m-%d %H:%M:%S")
    write_discover(body)
    print("%s: discover alerts now %s" % (time.strftime("%H:%M:%S"), ", ".join("%s %s" % (x["kind"], x["title"]) for x in alerts) or "none"), flush=True)
    return True

def discover_due(now=None):
    """refresh when discover.json is missing or older than 24 h, or imgserve asked (`discover.wanted`) and it is older than 24 h"""
    now = now or time.time()
    try: age = now - os.path.getmtime(os.path.join(LIB, "discover.json"))
    except OSError: return True
    return age > DAY

def covers_for(entries):
    """each Discover game's IGDB cover, fetched ONCE (state.jsonl, the same full/thumb store as the library)"""
    for d in ("full", "thumb", "dsheet"): os.makedirs(os.path.join(COV, d), exist_ok=True)
    st = load_state()
    for e in entries:
        if e["id"] in st or not e.get("cover_image"): continue
        url = "https://images.igdb.com/igdb/image/upload/t_cover_big/%s.jpg" % e["cover_image"]
        try: raw = http_get(url, timeout=30)
        except Exception: save_state(dict(id=e["id"], ok=False, source="igdb-discover", url=url, why="download failed")); continue
        r = make_images(e["id"], raw)
        if not r: save_state(dict(id=e["id"], ok=False, source="igdb-discover", url=url, why="unreadable image")); continue
        save_state(dict(id=e["id"], ok=True, source="igdb-discover", url=url, colour=r[0], w=r[1][0], h=r[1][1], matched=e["title"], at=int(time.time())))

def dsheets(entries):
    """Discover's own sprite sheets: 64 thumbs of 128 px a page, in Discover order; page/cell/colour go into each entry"""
    from PIL import Image
    st = load_state()
    per, cells = 64, 128
    pages = (len(entries) + per - 1) // per
    for p in range(pages):
        sheet = Image.new("RGB", (1024, 1024), (10, 10, 11))
        for i, e in enumerate(entries[p * per:(p + 1) * per]):
            s = st.get(e["id"], {})
            e.update(page=p, cell=i, colour=s.get("colour", ""), cover=bool(s.get("ok")))
            if not s.get("ok"): continue
            th = Image.open(os.path.join(COV, "thumb", e["id"] + ".jpg")).convert("RGB")
            th.thumbnail((cells, cells), Image.LANCZOS)
            sheet.paste(th, ((i % 8) * cells + (cells - th.width) // 2, (i // 8) * cells + (cells - th.height) // 2))
        tmp = os.path.join(COV, "dsheet", "%d.jpg.tmp" % p)
        sheet.save(tmp, format="JPEG", quality=85, optimize=True); os.replace(tmp, os.path.join(COV, "dsheet", "%d.jpg" % p))

# ---------------------------------------------------------------- serve (until imgserve has the same routes)
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *x): pass
    def do_GET(self):
        path = urllib.parse.unquote(self.path.split("?")[0])
        if path == "/library.json":
            f, ctype, cache = os.path.join(LIB, "library.json"), "application/json", "no-cache"
            etag = '"%s"' % open(os.path.join(LIB, "library.etag")).read().strip()
            if self.headers.get("If-None-Match") == etag:
                self.send_response(304); self.send_header("ETag", etag); self.end_headers(); return
        elif path == "/covers.json":
            f, ctype, cache, etag = os.path.join(COV, "covers.json"), "application/json", "no-cache", None
        else:
            m = re.fullmatch(r"/covers/(thumb|full|sheet)/([0-9a-f]{12}|\d+)\.jpg", path)
            if not m: self.send_response(404); self.end_headers(); return
            f, ctype, cache, etag = os.path.join(COV, m.group(1), m.group(2) + ".jpg"), "image/jpeg", "public, max-age=31536000, immutable", None
        if not os.path.exists(f): self.send_response(404); self.end_headers(); return
        data = open(f, "rb").read()
        self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache); self.send_header("Access-Control-Allow-Origin", "*")
        if etag: self.send_header("ETag", etag)
        self.end_headers(); self.wfile.write(data)

def cmd_serve(a):
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), H)
    print("library: http://0.0.0.0:%d/library.json  /covers.json  /covers/{thumb,full,sheet}/<id>.jpg" % a.port, flush=True)
    srv.serve_forever()

# ---------------------------------------------------------------- watch: the library follows the drives and the owned list by itself
def signature(owned):
    """cheap: the owned list's mtime, every game folder's listing (names + mtimes, one level), imgserve's /games.txt"""
    h = hashlib.sha256()
    try: h.update(str(os.path.getmtime(owned)).encode())
    except OSError: pass
    for drive in DRIVES:
        for rel, _ in DRIVE_DIRS + [("Retro Games", "")]:
            d = os.path.join(drive, rel)
            if not os.path.isdir(d): continue
            for name in sorted(os.listdir(d)):
                p = os.path.join(d, name)
                try: h.update(("%s|%d\n" % (p, int(os.path.getmtime(p)))).encode())
                except OSError: pass
    try:
        with urllib.request.urlopen(IMGSERVE + "/games.txt", timeout=15) as r: h.update(r.read())
    except Exception: pass
    return h.hexdigest()

def watch_discover(wanted_path=None, now=None):
    """one look at Discover from the watch loop: the alerts every time; a full refresh when it is due (older than 24 h) or imgserve
    left `discover.wanted` (the app asked and the file was old). Returns what it did ('refresh', 'alerts', '')."""
    wanted_path = wanted_path or os.path.join(LIB, "discover.wanted")
    asked = os.path.exists(wanted_path)
    if asked:
        try: os.remove(wanted_path)
        except OSError: pass
    if discover_due(now):
        try:
            print("%s: Discover refresh (%s)" % (time.strftime("%H:%M:%S"), "the app asked" if asked else "older than 24 h"), flush=True)
            cmd_discover(None)
            return "refresh"
        except Exception as ex:
            print("  Discover refresh failed: %s (next try in an hour)" % ex, flush=True)
            try: os.utime(os.path.join(LIB, "discover.json"), (time.time() - DAY + 3600,) * 2)   # retry in an hour, not every 15 s
            except OSError: pass
            return ""
    return "alerts" if cmd_alerts(None) else ""

def cmd_watch(a):
    last, next_full = None, 0.0
    while True:
        if time.time() >= next_full:
            sig = signature(a.owned)
            if sig != last:
                if last is not None: print("%s: the drives or the owned list changed: index, new covers, sheets" % time.strftime("%H:%M:%S"), flush=True)
                a.fast = False; cmd_index(a)
                a.only, a.limit = None, 0; cmd_covers(a)
                cmd_sheets(a)
                last = sig
            next_full = time.time() + a.every
            watch_discover()
        elif os.path.exists(os.path.join(LIB, "discover.wanted")):
            watch_discover()
        time.sleep(15)

def cmd_stats(a):
    lib = json.load(open(os.path.join(LIB, "library.json")))
    print("%d games, made %s; covers: %s" % (lib["count"], lib["made"], stats_line()))

def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("index"); p.add_argument("--owned", default=os.path.join(LIB, "D5-WHAT-MARKUS-HAD.md")); p.add_argument("--fast", action="store_true")
    p = sp.add_parser("covers"); p.add_argument("--only"); p.add_argument("--limit", type=int, default=0)
    sp.add_parser("sheets"); sp.add_parser("stats")
    p = sp.add_parser("watch"); p.add_argument("--owned", default=os.path.join(LIB, "D5-WHAT-MARKUS-HAD.md")); p.add_argument("--every", type=int, default=300)
    p = sp.add_parser("serve"); p.add_argument("--port", type=int, default=4453)
    sp.add_parser("discover"); sp.add_parser("alerts")
    a = ap.parse_args()
    dict(index=cmd_index, covers=cmd_covers, sheets=cmd_sheets, serve=cmd_serve, stats=cmd_stats, watch=cmd_watch, discover=cmd_discover,
         alerts=cmd_alerts)[a.cmd](a)

if __name__ == "__main__":
    main()
