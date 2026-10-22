# Credits, dependencies, and license review

BlazingMount is glue around several open components. Credit where due, and the
license position for each. **The checklist at the bottom must be cleared before any
public release.**

## Upstream components

| Component | Role in BlazingMount | Where | License |
|-----------|----------------------|-------|---------|
| **drakmor/ampr_emu** | the base of the `libSceAmpr.sprx` link fork (the demand-paging streaming engine). We overlay our `console/link-fork/module/` onto a clean checkout pinned at commit `cfa85df379f6eeeb165d7badf9b648e266fe77b7` (master, 2026-09-06, 4 commits after tag 0.4.2.1). | `console/link-fork/` | **GPL-3.0** |
| **ShadowMount Plus** | registers a starter folder as a real PS5 home-screen tile (reads `sce_sys/param.json` + `icon0.png`, overlays the folder's `fakelib/` with our fork). BlazingMount does not vendor it; it is already public homebrew and is expected to be installed on the console. | external | author's terms — verify |
| **idlesauce homebrew launcher** | auto-scans `/data/homebrew` for homebrew tiles (alternative to ShadowMount for homebrew folders). Referenced, not vendored. | external | author's terms — verify |
| **PS5 payload SDK** | builds the link fork and `blazingmountd`. Referenced by the build scripts, not vendored. | external | its own terms |

## BlazingMount's own code (GPL-3.0-or-later)

- `console/link-fork/module/` — the SMB/HTTP demand-paging link (`blz_http`, `blz_link_*`).
- `host/blazingmount-starter.py`, `host/blazingmount-host.py`, `host/engine/*`
  (`mkgame`, `amprindex`, `mkstub`, `pconsole`) — the host-side starter/index/catalog builders.
- `host/serve/imgserve` (C#), `host/serve/dirserve.py` — the read-only HTTP Range file servers.
- `install/blazingmount-install.py` — the host-assisted installer.
- `console/blazingmountd/` — the on-console discovery/register daemon (reference).

## No bundled content

This repository contains **no game or homebrew content** of any kind — no images,
no packages, no decrypted files, no fetchers for commercial titles. The only binary
is `console/link-fork/prebuilt/libSceAmpr.sprx`, which is a build of the GPL-3 link
fork and carries no game data. `.gitignore` additionally blocks common game-content
extensions from ever being committed.

## License-review checklist (DO BEFORE PUBLIC RELEASE)

- [ ] Vendor verbatim `COPYING` (GPL v3) at the repo root.
- [ ] Confirm drakmor/ampr_emu's GPL-3 terms and add the required upstream
      attribution/source-offer in `console/link-fork/README.md`.
- [ ] Confirm the redistribution terms for the **prebuilt** `libSceAmpr.sprx`
      (GPL-3 requires the corresponding source — we ship the source + build recipe,
      so this is satisfiable; verify the exact wording).
- [ ] State ShadowMount Plus and the homebrew launcher as external runtime
      dependencies with their authors' names and links (do not re-host their binaries
      without permission).
- [ ] Add per-file SPDX headers (`SPDX-License-Identifier: GPL-3.0-or-later`).
- [ ] Final read-through for anything host-specific (IPs, drive names) that should be
      an example rather than hard-coded.
