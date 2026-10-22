# blazingmountd — the self-contained discovery/register daemon

This is the **autoloadable payload** that makes BlazingMount run with no computer driving
the console: at boot it reads a catalog from your host, installs any games it hasn't seen yet
(fetch bundle → extract → drop the fork → write the link config), and lets ShadowMount Plus
register the tiles. Then the games stream from your host when launched.

> **Status: reference implementation, on-console test pending.** The source is complete and
> deliberately small/POSIX-plain so other devs can build and fix it. Until it's verified on a
> console, use the proven host-assisted path (`install/blazingmount-install.py`), which does
> the same steps from the host.

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
/data/blazing/mount.conf          one line:  192.168.1.130 4460
/data/blazing/fork/libSceAmpr.sprx   the link fork (from console/link-fork/prebuilt or your build)
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
