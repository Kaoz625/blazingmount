# BlazingMount

**A general network-drive mount-and-play tool for jailbroken PlayStation 5.**

BlazingMount lets a jailbroken PS5 **read games and homebrew off a drive you share
over your own network, show them as normal tiles on the home screen, and play them
by streaming the big files on demand** — so the console only ever stores a few hundred
megabytes per title instead of the whole 20–150 GB image.

It is content-neutral plumbing, the same category of tool as the already-public
**PS4Mount** and **ShadowMount**: *bring your own legally-owned games and your own
homebrew.* BlazingMount does not fetch, crack, sign, rip, decrypt, or bundle any game.
Your setup is your setup. You point it at drives **you** own holding content **you** own.

> This is normal open homebrew meant for other developers to test and improve.
> There is **no game content in this repository** — only the tool.

---

## What problem it solves

A jailbroken PS5 has no network filesystem in the kernel, and copying a modern
20–150 GB game onto the console just to try it is slow and fills the disk. BlazingMount
solves both:

1. **Mount** — a tiny read-only HTTP file server on the machine that holds your drives
   exposes your game images/folders to the LAN (this is the "network drive").
2. **Discover + show** — the console gets a small *starter* for each game (its small
   files, plus 0-byte placeholders for the big ones) and a well-known homebrew tile
   daemon (ShadowMount Plus) registers it as a real home-screen tile.
3. **Play (stream)** — when you launch the tile, an in-process link fork
   (`libSceAmpr.sprx`) serves the game's big files over HTTP Range from your server,
   on demand, as the game reads them. The console never stores the full image.

Everything survives a reboot and **no computer drives the console while you play** —
your server just answers file reads, exactly like any network drive.

```
   your machine (the drives)                         your jailbroken PS5
 ┌───────────────────────────┐   HTTP Range     ┌──────────────────────────────┐
 │ imgserve  (.ffpfsc images)│◀────────────────▶│ libSceAmpr.sprx link fork    │
 │ dirserve  (game folders)  │  on-demand reads │  (streams big files)         │
 │ blazingmount-host catalog │                  │ ShadowMount Plus → home tile │
 └───────────────────────────┘                  │ starter folder (small files) │
        the "network drive"                      └──────────────────────────────┘
```

---

## Repository layout

| Path | What it is |
|------|------------|
| `host/` | runs on the machine that **holds your drives** (the "network drive" side) |
| `host/serve/` | `imgserve` (serve `.ffpfsc`/PFS images) + `dirserve.py` (serve game folders), read-only HTTP Range |
| `host/blazingmount-starter.py` | build a **native streaming starter** from one of your games (proven) |
| `host/blazingmount-host.py` | build a `catalog.json` + per-game starter bundles for the on-console daemon |
| `host/engine/` | the streaming engine's host helpers (`mkgame`, `amprindex`, `mkstub`, `pconsole`) |
| `console/link-fork/` | the demand-paging **streaming engine**: `libSceAmpr.sprx` source + build recipe + a prebuilt binary |
| `console/blazingmountd/` | the self-contained **discovery/register daemon** (the autoloadable payload) — reference source + `build.sh` |
| `install/blazingmount-install.py` | **one command** to mount a host game onto a console (the working path today) |
| `install/blazingmount-autoload.md` | how reboot-survival is wired |
| `docs/` | how it works, supported formats, how to report a fix, and the first proof run |

---

## Quick start

See `docs/HOW-IT-WORKS.md` for the full picture. In short:

1. **On the machine with your drives** — start the servers and build a starter:
   ```sh
   # serve your image/folder drives, read-only, over HTTP Range
   dotnet run --project host/serve/imgserve -- --port 4451 /path/to/your/drive
   python3 host/serve/dirserve.py /path/to/your/folders --port 4452
   ```
2. **Install one game onto the console** (reboot-surviving, host-assisted):
   ```sh
   python3 install/blazingmount-install.py \
       --console 192.168.1.39 --host-ip 192.168.1.130 \
       --game "The Mound - Omen of Cthulhu (PPSA26223).ffpfsc"
   ```
   The game appears as a tile within ~15 s and plays, streamed from your server.
3. **(optional) Make it fully autonomous** — build and autoload `console/blazingmountd/`
   so the console discovers and registers every game in the host catalog by itself,
   with no per-game step. See `console/blazingmountd/README.md`.

---

## Supported content you bring

- **PS5 game images** you own, as `.ffpfsc` (compressed PFS) — served by `imgserve`.
- **PS5 game folders** you own — served by `dirserve`.
- **Homebrew** — any folder the homebrew launcher or ShadowMount already understands.

The console firmware must be **>=** the game's required firmware (this tool does not
downgrade/re-sign a game; it plays your games natively). See `docs/SUPPORTED-FORMATS.md`.

---

## Status

- **Proven**: the host starter builder, the `libSceAmpr.sprx` link fork, and the
  host-assisted installer. First end-to-end proof is in `docs/PROOF-DIGI5.md`
  (a game registered + played, streamed with 0 errors, on a stock-firmware console).
- **Reference, build pending**: `console/blazingmountd` (the fully-autonomous
  on-console discovery daemon) — source + build recipe are here; it is built with the
  PS5 payload SDK and still wants an on-console test. The installer is the working path
  meanwhile.

## License & credits

GPL-3.0-or-later (the link fork derives from GPL-3 `drakmor/ampr_emu`). See `LICENSE`
and `CREDITS.md`. **Run the license review in `CREDITS.md` before any public release.**

## Reporting a bug or a fix

See `docs/REPORT-A-FIX.md`.
