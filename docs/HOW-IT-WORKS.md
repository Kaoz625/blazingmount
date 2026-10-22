# How BlazingMount works

Three pieces: a **host** that serves your drives, a **starter** on the console, and an
in-process **link fork** that streams the big files.

## 1. The host is the "network drive"

The PS5 kernel has no network filesystem, so instead of mounting a share in the kernel, the
host runs small **read-only HTTP Range** file servers:

- `host/serve/imgserve` (C#/.NET) serves files *inside* `.ffpfsc`/PFS **images** without
  unpacking them.
- `host/serve/dirserve.py` serves game **folders**.

These expose only the files you point them at. They answer byte-range reads; they never push,
never write, never list your whole disk. This is the network drive.

## 2. The starter is what the console stores

A game's *starter* is the game's **small files** (its `sce_sys/param.json`, `icon0.png`,
`eboot.bin`, modules, manifests…) plus **0-byte placeholders** for the big paks, an
`ampr_emu.index` carrying the big files' **real sizes**, and a `fakelib/libSceAmpr.sprx`
(the link fork). For a 20–150 GB game the starter is only a few hundred MB.

`host/blazingmount-starter.py` builds it — natively, with **no downgrade/re-sign**, as long as
the console firmware is ≥ the game's required firmware (see `SUPPORTED-FORMATS.md`).

## 3. The tile — ShadowMount Plus

Drop a starter folder under `/data/homebrew/<name>/` and the resident **ShadowMount Plus**
daemon registers it as a real home-screen tile (it reads `sce_sys/param.json` + `icon0.png`,
mounts the folder, and overlays the folder's `fakelib/` so the game loads our link fork). This
is the same mechanism public backports use; BlazingMount just supplies the folder.

## 4. The play — the link fork streams on demand

When you launch the tile, the game asks for its paks. Our `libSceAmpr.sprx` (a fork of
`drakmor/ampr_emu`) sees those reads and, for any file listed in
`/data/blazing/link/<TID>.cfg`, fetches the needed byte ranges over HTTP from the host, on
demand, with a small in-RAM cache. The 0-byte placeholder on disk is never read; the bytes come
from the host. No config for a title = the fork is inert (stock behaviour).

## Reboot survival — nothing to re-run

Everything that makes a game work is on-disk and reloaded automatically:

- the starter folder, the fork and the link config all persist across reboot;
- ShadowMount Plus is already in the boot list and re-registers the tile each boot;
- the host just needs its file servers running (a login item / service).

No computer drives the console while you play. The optional `console/blazingmountd` daemon adds
auto-discovery of *new* games from the host catalog, so you never run a per-game step.

## Two ways to install

- **Host-assisted (proven today):** `install/blazingmount-install.py` — one command per game.
- **Fully autonomous (reference):** build `console/blazingmountd`, point it at a
  `blazingmount-host.py` catalog, and it installs everything by itself.
