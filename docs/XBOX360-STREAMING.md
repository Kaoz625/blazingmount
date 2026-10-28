# Xbox 360 games streamed from the drives (PS5X360) — plan

Status: **plan, waiting for Markus's OK** (28 Oct 2026). Nothing built, nothing on a console.

Markus, 28 Oct: games on PHAT5's M.2 "now, then build a streaming network". The 10 games went to
`/mnt/ext1/xbox360` (roku channels `tools/ps5/x360-to-phat5.py`). This is the "then": every Xbox 360
game plays from mac1's 8TB with nothing but small placeholders on the console, the same as PS5 starters.

## What PS5X360 does with a game (checked in its source, v1.0.2)

- **Library scan:** `std::filesystem::directory_iterator` over each game folder (`/mnt/ext1/xbox360` is a
  built-in root). It finds `default.xex`, `*.iso`, or a GOD/STFS header file (first 4 bytes `LIVE`/`PIRS`/`CON `).
- **Reading a game:** on the PS5 it never `mmap`s. `PosixMappedMemory::OpenUnmapped` = `open` + `fstat`, then
  every read is `pread` at an offset (`patches/canary/xbox360ps5.patch`, `XE_PLATFORM_PS5` → `from_file = true`).
  Extracted games are plain `open`/`read`/`lseek`.
- So every call it makes is one the link already serves: open, stat, fstat, pread, read, lseek, close, getdents
  (`console/link-fork/module/blz_link_game.cpp`, the hook-slot log line). No `mmap` = no 007/HITMAN problem.

## The design

1. **One plain copy on the 8TB.** The 360 games are zip/rar/7z today. Unpack each ONCE to
   `/Volumes/My Book/Xbox Games/Xbox 360/<Title>/` (ISO, extracted folder or GOD tree, the same layouts the
   M.2 has now). Unpack on mac2, never on mac1 (quiet boot after the 28 Oct freezes). dirserve :4452
   already serves it with Range.
2. **Placeholders on the console.** `/mnt/ext1/xbox360/<Title>/` keeps the real names, but each big file is
   a 0-byte placeholder listed in the link index with its real size (exactly how a PS5 starter carries its
   paks). The library scan sees the real tree; small files (default.xex, headers) stay real so the scan
   and covers work without the network.
3. **The link inside PS5X360.** Reads of a listed file go to mac1 dirserve (8 connections, read-ahead,
   no growing cache, per the 24 Oct rules). Unlisted files and saves stay local.
4. **BlazingMount builds it.** blazingmountd lists `Xbox Games/Xbox 360` from the host like it lists PS5
   images, writes the placeholders and the index, and removes a game's full local copy only after a
   streamed launch is proven (the DIGI5 full-image → starter rule: never delete the only copy).

## The one open decision: how the link gets into PS5X360

The link lives in our `fakelib/libSceAmpr.sprx`; PS5 games load it because they import libSceAmpr.
PS5X360 does not import it, so it would never load. Two ways:

- **A. A separate starter title** ("PS5X360 Stream", its own title ID): our copy of PS5X360's small files
  with the link module added to what its eboot loads. Markus's installed PPSA50011 is never touched.
  Cost: the copy must follow PS5X360 updates (BlazingMount already rebuilds starters when a source changes).
- **B. blazingmountd loads the link into the running PS5X360** when it starts (the payload already has
  kernel access). Nothing copied, works with every PS5X360 update, but it is the bigger and riskier build
  (injecting into a live process; a mistake crashes the emulator, never the console's games).

Recommended: **A** first (it is the mechanism every streamed PS5 game already proves), B later if
PS5X360 updates make the copy a chore.

## Proof (what "done" means)

One ISO game (MK vs DC) and one extracted game (BlazBlue CSE) load AND are played on PHAT5 streamed,
with the M.2 copy moved aside (not deleted), imgserve/dirserve logs showing the 206 range reads, then
DIGI5. Per the payload rule, the plan line goes to Markus before any build reaches a console.
