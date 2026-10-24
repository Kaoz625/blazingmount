#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_library.py - the whole-library rules of library.py, on a made-up owned list and a fake drive (no mac1, no network).

    python3 -I host/library/test_library.py
"""
import importlib.util, json, os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("library", os.path.join(HERE, "library.py"))
L = importlib.util.module_from_spec(spec); spec.loader.exec_module(L)

fails = 0
def check(cond, what):
    global fails
    print(("PASS " if cond else "FAIL ") + what); fails += 0 if cond else 1

# titles: what people say, and one key per game across regions/dumps
check(L.display("Legend of Zelda, The - Twilight Princess (USA).nkit") == "The Legend of Zelda - Twilight Princess", "article first, tags and .nkit gone")
check(L.display("Banjo-Kazooie Rev 1 (1998)(Nintendo - Rare)(US)") == "Banjo-Kazooie", "TOSEC dates/publisher and Rev gone")
check(L.display("Assassin's Creed® Mirage (PPSA07230)") == "Assassin's Creed® Mirage", "title id gone, mark kept for the screen")
check(L.norm("The Legend of Zelda: Twilight Princess") == L.norm("Legend of Zelda, The - Twilight Princess (USA).nkit"), "same key across naming styles")
check(L.norm("Contra III - The Alien Wars (1992)(Konami)(US)") == L.norm("Contra III: The Alien Wars"), "TOSEC name meets the owned list")
check(L.norm("God of War II (Asia)") == L.norm("God of War II (USA)"), "regions group")
check(L.eid("ps2", "God of War II (Asia)") == L.eid("ps2", "God of War II (USA)") != L.eid("ps3", "God of War II"), "ids: same per system, differ across")

work = tempfile.mkdtemp(prefix="bm-lib-")
try:
    owned = os.path.join(work, "owned.md")
    open(owned, "w").write("""# list
## PS2

| Game | ID | Platform | His? | Licence on DIGI5 | Played (profiles) | File Type | Size |
|---|---|---|---|---|---|---|---|
| God of War II (Asia) | SCAJ-20200 | PS2 | OWNED - in his PSN library | - | - | ZIP | ZIP 4.0 GB |
| God of War II (USA) | SCUS-97481 | PS2 | OWNED - in his PSN library | - | - | ZIP | ZIP 4.0 GB |

## PS5

| Game | ID | Platform | His? | Licence on DIGI5 | Played (profiles) | File Type | Size |
|---|---|---|---|---|---|---|---|
| Alan Wake 2 | PPSA02571 | PS5 | PS Plus (his while Plus is active) | 1522a9d8 | - | - | - |
| Gran Turismo 7 | PPSA01317 | PS5 | OWNED - in his PSN library | - | - | - | - |

## Nintendo

| Game | ID | Platform | His? | Licence on DIGI5 | Played (profiles) | File Type | Size |
|---|---|---|---|---|---|---|---|
| Super Mario Sunshine | - | GameCube | OWNED - Markus, 15 Oct 2026 | - | - | - | - |
""")
    rows = L.owned_rows(owned)
    check(len(rows) == 5 and {r["system"] for r in rows} == {"ps2", "ps5", "gc"}, "owned list: rows and systems")
    check(next(r for r in rows if r["title"] == "Alan Wake 2")["how"] == "ps_plus", "PS Plus is told apart from owned")
    drive = os.path.join(work, "My Book")
    for rel in ("PS5 Games - not packed yet/Gran Turismo 7 (PPSA01317)/sce_sys", "Retro Games/GameCube", "Retro Games/PSP", "PS2 Games"):
        os.makedirs(os.path.join(drive, rel))
    open(os.path.join(drive, "PS5 Games - not packed yet/Gran Turismo 7 (PPSA01317)/sce_sys/icon0.png"), "wb").write(b"\x89PNG")
    open(os.path.join(drive, "Retro Games/GameCube/Super Mario Sunshine (USA).rvz"), "wb").write(b"x" * 10)
    open(os.path.join(drive, "Retro Games/PSP/settings.bin"), "wb").write(b"x")
    open(os.path.join(drive, "PS2 Games/God of War II (USA).iso"), "wb").write(b"x" * 20)
    open(os.path.join(drive, "PS2 Games/God of War II (USA).iso.verified.json"), "w").write("{}")
    L.DRIVES = [drive]
    L.IMGSERVE = "http://127.0.0.1:9"        # nothing answers: no streamable games
    L.LIB = os.path.join(work, "lib")
    class A: pass
    a = A(); a.owned = owned; a.fast = False
    L.cmd_index(a)
    lib = json.load(open(os.path.join(L.LIB, "library.json")))
    by = {(g["system"], g["title"]): g for g in lib["games"]}
    gow = by.get(("ps2", "God of War II"))
    check(gow is not None and len(gow["variants"]) == 2 and len(gow["files"]) == 1, "PS2 regions grouped into one game with its file")
    check(gow and gow["files"][0]["verified"] and gow["files"][0]["type"] == "iso" and gow["files"][0]["size"] == 20, "file type, size, verified")
    gt = by.get(("ps5", "Gran Turismo 7"))
    check(gt and gt["files"] and gt["files"][0]["icon"].endswith("icon0.png") and gt["how"] == "owned", "a not-packed PS5 folder joins its owned row by title id, icon0 found")
    check(gt and gt["plays"] == L.PS5_NOT_PACKED, "a not-packed PS5 folder does not claim it streams: 'once it is packed'")
    check(by[("ps5", "Alan Wake 2")]["plays"] == L.PS5_NOT_ON_DRIVES, "an owned PS5 game with no copy says it is not on the drives")
    sms = by.get(("gc", "Super Mario Sunshine"))
    check(sms and sms["files"] and sms["files"][0]["type"] == "rvz" and "Dolphin" in sms["plays"], "retro file joins the owned row; plays on")
    check(not any(g["title"] == "settings" for g in lib["games"]), "junk files (settings.bin) are not games")
    check(by[("ps5", "Alan Wake 2")]["how"] == "ps_plus" and not by[("ps5", "Alan Wake 2")]["files"], "owned-only game listed without files")
    check(os.path.exists(os.path.join(L.LIB, "library.etag")), "ETag written")
finally:
    shutil.rmtree(work, ignore_errors=True)
print("ALL PASS" if not fails else "%d FAIL" % fails)
sys.exit(1 if fails else 0)
