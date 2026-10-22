# Reboot survival & autoload

BlazingMount is designed so that **nothing has to be re-run after a reboot**. Here is exactly
what persists and what (optionally) goes in the boot list.

## What already survives a reboot (no boot-list change)

For every game installed with `blazingmount-install.py`:

- the **starter folder** under `/data/homebrew/<name>/` — on disk, persists;
- the **link fork** in that folder's `fakelib/libSceAmpr.sprx` — on disk, persists;
- the **link config** `/data/blazing/link/<TID>.cfg` — on disk, persists;
- **ShadowMount Plus** is already in the jailbreak's boot list and re-registers every starter
  tile on each boot.

So after the one-time install, the game keeps its tile and keeps streaming across reboots. The
only moving part is the **host**: keep `imgserve`/`dirserve` running there (a login item /
launchd / systemd service), the same as any always-on network drive.

## Optional: autoload `blazingmountd` (full auto-discovery)

If you build `console/blazingmountd`, add it to the boot list so it discovers and installs new
games from the host catalog on every boot, with no per-game step.

1. Put the fork where the daemon copies it from:
   `/data/blazing/fork/libSceAmpr.sprx` (build it: `console/link-fork/tools/build-fork.sh`).
2. Write the host line:
   `echo "192.168.1.50 4460" > /data/blazing/mount.conf`
3. Copy `blazingmountd.elf` into your payload manager's payload directory.
4. Add **one** line to the jailbreak autoload list that loads it **after** the core servers and
   ShadowMount. The exact file depends on your stack — common ones are
   `/data/pldmgr/autoload.txt`, `/data/ps5_autoloader/autoload.txt`,
   `/mnt/usb*/ps5_autoloader/`, and `/data/etaHEN/config.ini`. **Change only one list and say
   which**, then re-read it and count the entries.

### Boot-list safety (read this)

Anything in a boot list runs **unattended, with no arguments, while the system is still coming
up**. A one-shot tool that opens a browser, writes a test file, or expects arguments will crash
or hang the boot. `blazingmountd` is written to be boot-safe: it loops, takes no arguments, and
only writes under `/data/homebrew`, `/data/blazing/link`, and `/data/blz-stage`. After any
change to a boot list, re-read it and confirm the count — a stray or duplicated entry is the
usual cause of a "boot loaded nothing" or a system-software-error on startup.

`blazingmountd` is also safe to list your whole library in the catalog: before it fetches a
bundle it does `if (exists(/data/homebrew/<NAME>)) return;`, so a game you already have is
**skipped** — it never re-downloads on every boot/poll. The catalog is therefore a plain list
of *everything you want available*, and each console pulls only what it is missing.

## Worked example — activated on a Payload-Manager stack (DIGI5, FW 13.60, 22 Oct 2026)

This is the exact, reversible activation used on a digital-edition PS5 running
itsPLK/ps5-payload-manager. Adapt the one IP and the slot name to your stack.

1. **Stage** (already done by the build): `blazingmountd.elf` and `fork/libSceAmpr.sprx` live in
   `/data/blazing/`.
2. **Back up every boot list first**, to the console and to your machine, and record the sha256
   and line count:
   `curl http://<ps5>:8080/fs/data/pldmgr/autoload.txt` → save as `autoload.txt.bak-<who>-<date>`;
   push it back with `curl -T ... ftp://<ps5>:2121/data/pldmgr/autoload.txt.bak-<who>-<date>`.
3. **Make a Payload-Manager *local slot*** (its autoload lines are slot refs, not raw paths):
   `MKD /data/pldmgr/payloads/local/blazingmountd`, then upload `blazingmountd.elf` and a
   `meta.json` (`{"schema":2,"kind":"local","filename":"blazingmountd.elf", ...}`, copy the shape
   from an existing `local/*/meta.json`).
4. **Write the host line:** `printf '192.168.1.130 4460\n'` → `/data/blazing/mount.conf`
   (the IP/port of your catalog server — see host-autostart below).
5. **Add exactly one autoload line, last** (so it loads after the core servers and ShadowMount):
   append `local/blazingmountd` to `/data/pldmgr/autoload.txt`. **Read it back, confirm the
   sha256 and that the count went up by exactly one** (here 25 → 26).
6. **Host side:** run the catalog server so the daemon has something to pull. Templates are in
   [`host-autostart/`](host-autostart/): `serve-catalog.command` (a *visible Terminal* server —
   a launchd/ssh process cannot reach the LAN on macOS), `catalog-keeper.sh` + the LaunchAgent
   plist to reopen it after a reboot, and a `catalog.txt` starter. Mirror of the imgserve pattern.

Reverse it by removing the `local/blazingmountd` line (restore the `.bak`), deleting the slot
folder, and deleting `/data/blazing/mount.conf`. Nothing else is touched.

**What survives the reboot after this:** the on-disk starters + ShadowMount keep every
already-installed game's tile (so the games you set up still appear in the Game Library and on
the Games row); `blazingmountd` additionally re-pulls any *new* catalog games on each boot.
