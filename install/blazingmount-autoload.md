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
