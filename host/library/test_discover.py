#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_discover.py - DISCOVER (library.py discover / alerts / watch_discover) against a fake IGDB and fake cover downloads.
No mac1, no network, no credentials.

    python3 -I host/library/test_discover.py

Checks: the shelves (new / soon / popular / notable / wishlist) and their limits, the join against library.json (owned on the same
system, owned by title on another, on my drives, not owned), notable keeps only games he does NOT own, each cover is fetched ONCE,
the dsheet sprite sheets + page/cell, the wishlist alerts (out, moved, too old, not yet), alerts-only rewrites only on a change,
the ETag ignores `made`, and the refresh rule (missing / older than 24 h / the app asked).
"""
import importlib.util, io, json, os, shutil, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("library", os.path.join(HERE, "library.py"))
L = importlib.util.module_from_spec(spec); spec.loader.exec_module(L)

fails = 0
def check(cond, what):
    global fails
    print(("PASS " if cond else "FAIL ") + what); fails += 0 if cond else 1

DAY = 86400
TODAY = int(time.mktime(time.strptime("2026-10-24 12:00", "%Y-%m-%d %H:%M")))

def g(i, name, plats, date, hypes=0, rating=0, votes=0, cover=True):
    r = dict(id=i, name=name, platforms=plats, first_release_date=date, hypes=hypes, total_rating=rating, total_rating_count=votes,
             summary="A game about %s. " % name * 30, release_dates=[dict(date=date, platform=p) for p in plats])
    if cover: r["cover"] = dict(image_id="img%d" % i)
    return r

NEW = [g(101, "Fresh Owned Game", [167], TODAY - 5 * DAY, hypes=50), g(102, "Fresh Unknown", [167, 6], TODAY - 2 * DAY, hypes=5),
       g(103, "Zero Interest", [167], TODAY - 1 * DAY), g(104, "No Cover", [167], TODAY - 1 * DAY, hypes=9, cover=False),
       g(105, "Xbox Only", [999], TODAY - 1 * DAY, hypes=99)]
SOON = [g(201, "Later Big", [167], TODAY + 60 * DAY, hypes=300), g(202, "Sooner Small", [48], TODAY + 10 * DAY, hypes=3)]
POP = [dict(game_id=301, value=9.0), dict(game_id=102, value=8.0)]
POPGAMES = [g(102, "Fresh Unknown", [167, 6], TODAY - 2 * DAY, hypes=5), g(301, "Popular Thing", [167], TODAY - 400 * DAY, votes=900)]
NOTABLE = {167: [g(401, "Great PS5 Game", [167], TODAY - 900 * DAY, rating=95, votes=500), g(101, "Fresh Owned Game", [167], TODAY - 5 * DAY)],
           8: [g(402, "God of War II", [8], TODAY - 6000 * DAY, rating=93, votes=800), g(403, "Shadow of the Colossus", [8], TODAY - 6000 * DAY, rating=92, votes=900)],
           19: [g(404, "Chrono Trigger", [19], TODAY - 9000 * DAY, rating=96, votes=700)]}
CALLS = []

def fake_query(ig, endpoint, body):
    CALLS.append((endpoint, body))
    if endpoint == "popularity_primitives": return POP
    if "first_release_date >=" in body: return NEW
    if "first_release_date >" in body and "hypes > 0" in body: return SOON
    if "total_rating_count >= 20" in body:
        pid = int(body.split("platforms = (")[1].split(")")[0])
        return NOTABLE.get(pid, [])
    if "where id = (" in body:
        ids = [int(x) for x in body.split("where id = (")[1].split(")")[0].split(",")]
        every = {r["id"]: r for r in NEW + SOON + POPGAMES + [x for v in NOTABLE.values() for x in v] + [WISHED]}
        return [every[i] for i in ids if i in every]
    return []

WISHED = g(501, "Wished Game", [167], TODAY - 1 * DAY, hypes=40)      # came out yesterday: an "out" alert

def jpeg():
    from PIL import Image
    b = io.BytesIO(); Image.new("RGB", (264, 374), (200, 30, 40)).save(b, format="JPEG"); return b.getvalue()

DOWNLOADS = []
def fake_http_get(url, data=None, headers=None, timeout=30):
    DOWNLOADS.append(url); return jpeg()

work = tempfile.mkdtemp(prefix="bm-disc-")
try:
    L.LIB = os.path.join(work, "lib"); L.COV = os.path.join(work, "cov")
    os.makedirs(L.LIB); os.makedirs(L.COV)
    L.igdb_query = fake_query; L.http_get = fake_http_get
    games = [dict(id=L.eid("ps5", "Fresh Owned Game"), title="Fresh Owned Game", system="ps5", how="owned", files=[]),
             dict(id=L.eid("ps2", "God of War II"), title="God of War II", system="ps2", how="owned", files=[dict(path="/x/gow2.iso")]),
             dict(id=L.eid("ps4", "Popular Thing"), title="Popular Thing", system="ps4", how="ps_plus", files=[])]
    lib = dict(systems=[dict(key="ps5", count=5), dict(key="ps4", count=9), dict(key="ps2", count=4), dict(key="snes", count=3),
                        dict(key="a5200", count=2), dict(key="pc", count=3)], games=games)
    json.dump(lib, open(os.path.join(L.LIB, "library.json"), "w"))
    json.dump(dict(version=1, items=[dict(id=L.did(501), igdb=501, title="Wished Game", date=TODAY - 1 * DAY),
                                     dict(id=L.did(201), igdb=201, title="Later Big", date=TODAY + 30 * DAY)]),
              open(os.path.join(L.LIB, "wishlist.json"), "w"))

    check(L.discover_due(TODAY), "refresh is due when discover.json is missing")

    class Ig: last = 0.0
    L.cmd_discover(None, ig=Ig(), today=TODAY)
    d = json.load(open(os.path.join(L.LIB, "discover.json")))
    sh = {s["key"]: s["ids"] for s in d["shelves"]}
    by = {e["id"]: e for e in d["games"]}
    T = lambda i: by[i]["title"] if i in by else "?"

    check([T(i) for i in sh["new"]] == ["Fresh Owned Game", "Fresh Unknown"], "new: interest first, zero-interest / no cover / not his system dropped")
    check([T(i) for i in sh["soon"]] == ["Sooner Small", "Later Big"], "soon: by date")
    check([T(i) for i in sh["popular"]] == ["Popular Thing", "Fresh Unknown"], "popular: IGDB's order, his systems only")
    check(set(T(i) for i in sh["notable"]) == {"Great PS5 Game", "Shadow of the Colossus", "Chrono Trigger"},
          "notable: best-rated per system he plays, ONLY the ones he does not own (God of War II + Fresh Owned Game left out)")
    check(not any("platforms = (%d)" % 66 in b for _, b in CALLS), "notable skips a system with fewer than 3 games (Atari 5200)")
    check(set(T(i) for i in sh["wishlist"]) == {"Wished Game", "Later Big"}, "wishlist shelf: every wished game re-read by id")

    fo = by[L.did(101)]
    check(fo["owned"] == "owned" and not fo["on_drives"] and fo["lib_id"] == L.eid("ps5", "Fresh Owned Game"), "owned on the same system (eid join)")
    pt = by[L.did(301)]
    check(pt["owned"] == "ps_plus", "owned by title on another of his systems (PS4 via PS Plus) for a PS5 game")
    check(by[L.did(102)]["owned"] == "" and by[L.did(102)]["sys_labels"] == "PS5, PC", "not owned; systems labelled")
    check(by[L.did(102)]["date_text"] and len(by[L.did(102)]["summary"]) <= 300, "date text; summary capped at 300")

    check(all(e.get("cover") and e["page"] >= 0 and 0 <= e["cell"] < 64 for e in d["games"]), "every game has a cover cell in the dsheets")
    check(os.path.exists(os.path.join(L.COV, "dsheet", "0.jpg")) and os.path.exists(os.path.join(L.COV, "thumb", L.did(102) + ".jpg")), "dsheet + thumb written")
    n_first = len(DOWNLOADS)
    check(n_first == len(d["games"]), "one cover download per game (%d)" % n_first)

    al = {a["title"]: a for a in d["alerts"]}
    check(set(al) == {"Wished Game", "Later Big"} and al["Wished Game"]["kind"] == "out" and al["Later Big"]["kind"] == "moved",
          "alerts: out (date arrived) + moved (IGDB date differs from the date when added)")
    check(al["Wished Game"]["key"] == "%s:out:%d" % (L.did(501), TODAY - DAY), "alert key = id:kind:date (what the console remembers)")

    etag1 = open(os.path.join(L.LIB, "discover.etag")).read().strip()
    L.cmd_discover(None, ig=Ig(), today=TODAY)
    check(len(DOWNLOADS) == n_first, "second refresh downloads no cover again")
    check(open(os.path.join(L.LIB, "discover.etag")).read().strip() == etag1, "same data = same ETag (made is not part of it)")

    check(L.cmd_alerts(None, today=TODAY) is False, "alerts-only: nothing changed -> no rewrite")
    w = json.load(open(os.path.join(L.LIB, "wishlist.json")))
    w["items"].append(dict(id=L.did(102), igdb=102, title="Fresh Unknown", date=TODAY - 2 * DAY)); json.dump(w, open(os.path.join(L.LIB, "wishlist.json"), "w"))
    check(L.cmd_alerts(None, today=TODAY) is True, "alerts-only: a newly wished game that is out -> rewritten")
    d2 = json.load(open(os.path.join(L.LIB, "discover.json")))
    check(any(a["title"] == "Fresh Unknown" and a["kind"] == "out" for a in d2["alerts"]) and open(os.path.join(L.LIB, "discover.etag")).read().strip() != etag1,
          "the new alert is in discover.json and the ETag moved (consoles download it once)")
    check(not L.make_alerts([dict(id="a", title="Old", date=TODAY - 90 * DAY)], {}, TODAY), "an out date older than 60 days raises nothing")
    check(not L.make_alerts([dict(id="b", title="Not yet", date=TODAY + 5 * DAY)], {"b": TODAY + 5 * DAY}, TODAY), "not out yet, same date -> nothing")

    check(not L.discover_due(TODAY), "fresh file -> not due")
    os.utime(os.path.join(L.LIB, "discover.json"), (time.time() - 2 * DAY,) * 2)
    check(L.discover_due(), "older than 24 h -> due")
    os.utime(os.path.join(L.LIB, "discover.json"), None)
    open(os.path.join(L.LIB, "discover.wanted"), "w").close()
    check(L.watch_discover() in ("", "alerts") and not os.path.exists(os.path.join(L.LIB, "discover.wanted")),
          "the app asked but the file is fresh -> the marker is cleared, no IGDB refresh")
    calls = len(CALLS)
    os.utime(os.path.join(L.LIB, "discover.json"), (time.time() - 2 * DAY,) * 2)
    L.Igdb = lambda: Ig()
    check(L.watch_discover() == "refresh" and len(CALLS) > calls, "older than 24 h -> the watch loop refreshes")
finally:
    shutil.rmtree(work, ignore_errors=True)

print("ALL PASS" if not fails else "%d FAIL" % fails)
sys.exit(1 if fails else 0)
