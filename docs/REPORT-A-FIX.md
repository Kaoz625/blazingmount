# Reporting a bug or sending a fix

BlazingMount is meant for other developers to test and improve. Here is what makes a report
useful and how to send a change.

## Before you file

1. Confirm the basics are up: on the host, `imgserve`/`dirserve` answer a range request; on the
   console, `ftpsrv` (:2121) and `websrv` (:8080) are running and **ShadowMount Plus** is loaded.
2. Confirm the firmware rule: the console FW is >= the game's required FW
   (`blazingmount-starter.py` prints both and refuses a game that needs newer FW).

## What to include in a bug report

- **Console** firmware and the homebrew stack (ShadowMount Plus version, jailbreak).
- **Game**: title id + required firmware (not the game files - never attach content).
- **Where it broke**: tile didn't appear / tile appeared but launch failed / launched but no
  stream / stream errors.
- **Logs** (these carry no game content):
  - ShadowMount: `/data/shadowmount/debug.log` (tile registration).
  - The link fork: `/data/blazing/link/<TID>.log` and the `[AMPR_EMU] [blz_link]` lines in klog.
  - The host file server's request log (status codes, `206` = good).
  - `blazingmountd`: `/data/blazing/blazingmountd.log`.

## Common failures and where to look

| Symptom | Likely cause |
|---------|--------------|
| tile never appears | starter missing `sce_sys/param.json` or `icon0.png`; folder nested too deep for the scan depth; ShadowMount not running |
| tile appears, launch fails immediately | game's required FW > console FW (native run needs FW >= game) |
| launches but hangs/black with no stream | fork not overlaid (ShadowMount `backport_fakelib` off, or fork not in the starter's `fakelib/`); no `/data/blazing/link/<TID>.cfg` |
| stream errors in the fork log | host file server unreachable from the console's LAN IP, or the cfg points at an address the game process can't route to |

## Sending a change

- Keep the tool content-neutral: no bundled game content, no fetchers/rippers/signers for
  content the user doesn't own. PRs that add those are out of scope.
- Respect the licenses in `CREDITS.md` (GPL-3.0-or-later; the fork derives from GPL-3
  `drakmor/ampr_emu`).
- The on-console daemon (`blazingmountd`) is the most-wanted area to harden - see its README
  for the first things to verify against your SDK and firmware.
