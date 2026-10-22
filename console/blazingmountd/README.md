# blazingmountd — the self-contained discovery/register daemon

This is the **autoloadable payload** that makes BlazingMount run with no computer driving
the console: at boot it reads a catalog from your host, installs any games it hasn't seen yet
(fetch bundle → extract → drop the fork → write the link config), and lets ShadowMount Plus
register the tiles. Then the games stream from your host when launched.

> **Status: built and proven on-console.** Built with the PS5 payload SDK and run on a jailbroken
> PS5 (DIGI5, FW 13.60): it read `/data/blazing/mount.conf`, fetched a host catalog over HTTP,
> installed a bundle (untar → `/data/homebrew/<name>/`), dropped the fork into the game's
> `fakelib/`, and wrote `/data/blazing/link/<TID>.cfg` — the fully-autonomous "no setup script"
> path (see `docs/PROOF-DIGI5.md`). Run it one-shot with `blazingmountd once`, or resident by
> adding the autoload line (your call). The host-assisted `install/blazingmount-install.py`
> remains the per-game alternative. The source is deliberately small/POSIX-plain so other devs
> can build and improve it.

## How it works

```
host (your drives)                                 console (blazingmountd, resident)
  blazingmount-host.py build  ─────────────────▶   read /data/blazing/mount.conf ("<host-ip> <port>")
    catalog.txt                                     GET /catalog.txt
    bundles/<TID>.starter.tar                       for each new game:
    cfg/<TID>.cfg                                     GET bundle → untar → /data/homebrew/<NAME>/
  blazingmount-host.py serve  ◀──── HTTP ─────       copy fork → <NAME>/fakelib/libSceAmpr.sprx
                                                      GET cfg → /data/blazing/link/<TID>.cfg
                                                    ShadowMount Plus registers the tile
```

## Configure the console (once)

```
/data/blazing/mount.conf          one line:  192.168.1.50 4460
/data/blazing/fork/libSceAmpr.sprx   the link fork (build it: console/link-fork/tools/build-fork.sh)
```

## Build

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk sh build.sh
```

## Install (reboot-surviving)

Copy `out/blazingmountd.elf` to your payload manager's payload directory and add one line to
the jailbreak autoload list. See `../../install/blazingmount-autoload.md`.

**Boot-list safety:** anything in a boot list must be safe to run unattended with no arguments
(a one-shot tool that opens a browser or expects args will crash the boot). `blazingmountd`
qualifies: it loops, needs no arguments, and only writes under `/data/homebrew`,
`/data/blazing/link`, and `/data/blz-stage`. After adding it, re-read the autoload list and
count it (one entry).

## What to test / fix first (see docs/REPORT-A-FIX.md)

- sockets + `inet_pton`/`connect` against your SDK (some SDKs need an explicit libnet init).
- the ustar extractor against a real bundle (`tar tvf bundles/<TID>.starter.tar`).
- staging/rename across `/data/blz-stage` → `/data/homebrew` on your firmware.
