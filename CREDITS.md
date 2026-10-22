<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Credits, dependencies, and license

BlazingMount is GPL-3.0-or-later. It is glue around a few open components; this file credits
them and records the license position. The pre-release checklist at the bottom is **cleared**.

## Upstream components

| Component | Role | How we use it | License |
|-----------|------|---------------|---------|
| **drakmor/ampr_emu** | base of the `libSceAmpr.sprx` link fork (the demand-paging streaming engine) | we ship **our patches** (`console/link-fork/module/`) and a build recipe that clones ampr_emu at pinned commit `cfa85df379f6eeeb165d7badf9b648e266fe77b7` and overlays them. We do **not** redistribute ampr_emu's source or any built binary. | **GPL-3.0** |
| **ShadowMount Plus** | registers a starter folder as a home-screen tile and overlays its `fakelib/`. External runtime dependency — **not vendored, not re-hosted**. Install it yourself from its author's release. | runtime | author's terms |
| **PS5 homebrew launcher** (idlesauce) | auto-scans `/data/homebrew` for homebrew tiles. External, referenced only. | runtime | author's terms |
| **ps-neighborhood** `PS5PKGTool.Ffpfsc` / `Ufs2` | imgserve's on-demand `.ffpfsc` + exFAT/UFS2 decoder. **External build dependency — not vendored**; obtain a checkout and set `FFPFSC_SRC` (dirserve for game *folders* needs none of this). | build (imgserve) | **GPL-3.0** |
| **PS5 payload SDK** | builds the link fork and `blazingmountd`. Referenced by the build scripts; not vendored. | build | its own terms |

> **Attribution note for packagers:** when you distribute a *built* `libSceAmpr.sprx`, you are
> distributing a GPL-3 binary and must provide its complete corresponding source (our
> `console/link-fork/` plus ampr_emu @ `cfa85df`). This repo distributes **source only**, so that
> obligation is already met for anyone building from here. Keep a mirror of ampr_emu @ `cfa85df`
> (the pinned commit) so the corresponding source stays available if upstream moves.

## BlazingMount's own code (GPL-3.0-or-later)

`console/link-fork/module/` (the link), `host/blazingmount-starter.py`, `host/blazingmount-host.py`,
`host/engine/*`, `host/serve/imgserve` + `host/serve/dirserve.py`, `install/blazingmount-install.py`,
and `console/blazingmountd/`. Every source file carries an SPDX header.

## No bundled content, no secrets

This repository contains **no game or homebrew content** — no images, packages, decrypted files,
or fetchers — and **no prebuilt binary**. `.gitignore` blocks game-content extensions and `*.sprx`
so neither can be committed by accident. A secret scan of all tracked files is clean (the only
matches are a parser *token* in the fork and path-traversal **test fixtures** in
`host/serve/test_dirserve.py` that assert the server refuses `/etc/passwd`-style escapes).

## Pre-release license checklist — CLEARED

- [x] **Verbatim `COPYING` (GPL v3) at the repo root.** Added (674 lines, from gnu.org).
- [x] **drakmor/ampr_emu attribution + offer-of-source.** Credited above with the pinned commit
      and license; we ship source only, so the corresponding-source obligation is met; packagers
      are told what applies if they distribute a built binary.
- [x] **Prebuilt `libSceAmpr.sprx` redistribution question — removed.** We do **not** ship the
      prebuilt binary. Source + `build-fork.sh` only; `console/link-fork/prebuilt/` is a
      placeholder and `*.sprx` is gitignored.
- [x] **ShadowMount Plus + homebrew launcher named as external runtime deps**, with authors, and a
      "do not re-host their binaries" note. They are not vendored.
- [x] **Per-file SPDX headers** (`SPDX-License-Identifier: GPL-3.0-or-later`) on all 23 source files.
- [x] **Host-specific read-through.** Internal IPs replaced with placeholder LAN addresses
      (`192.168.1.50/.60`); internal paths/codenames removed from `host/engine/mkgame.py` (now
      env-configurable), `console/link-fork/tools/build-fork.sh` (generic local build, no internal
      ssh/paths), and `host/serve/imgserve/build.sh` + `imgserve.csproj` (FFPFSC_SRC is now an external
      dependency, no internal paths). *Harmless labels that remain by design:* the authors' rig is named
      in a few vendored code comments and in `docs/PROOF-DIGI5.md` (e.g. `mac1`, `DIGI5`, `PHAT5`) — these
      are plain host/console labels, not secrets, IPs, or paths.
- [x] **Secret + game-content scan** of all tracked files — clean (see above).

> **Confirm before flipping public:** the exact `drakmor/ampr_emu` upstream URL (the build script
> takes it as `$AMPR_EMU_REPO`; the pinned commit `cfa85df` is the anchor). Fill it into the README
> links once verified.
