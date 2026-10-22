# host/ — the machine that holds your drives

Run these on the computer that has your game drives. Together they are the "network drive"
the PS5 streams from, plus the tools that turn a game into a console starter.

## The file servers (the network drive)

Serve your drives read-only over HTTP Range. Point them at **your** directories.

```sh
# images (.ffpfsc / PFS): needs .NET
dotnet run --project serve/imgserve -- --port 4451 /path/to/your/image-drive

# game folders: needs python3
python3 serve/dirserve.py /path/to/your/folders --port 4452
```

Keep them running (a login item / launchd / systemd service) so games stream after a reboot —
see `nothing dies on reboot` in the main docs. They are read-only: they answer byte ranges and
never write or expose anything outside the directories you name.

## Build a starter from one of your games

```sh
python3 blazingmount-starter.py \
    "The Mound - Omen of Cthulhu (PPSA26223).ffpfsc" \
    /tmp/mound-out \
    --fork ../console/link-fork/prebuilt/libSceAmpr.sprx \
    --console-fw 13.60
```

Produces `/tmp/mound-out/starter/<Name (TID)>/` (the console folder) and `<TID>.cfg` (the link
config). It **refuses** a game whose required firmware is newer than `--console-fw`.

Normally you don't call this directly — `install/blazingmount-install.py` runs it for you and
pushes the result to the console.

## Build a catalog for the autonomous daemon

```sh
python3 blazingmount-host.py build /srv/blazingmount \
    --host-ip 192.168.1.50 --console-fw 13.60 \
    --game "The Mound - Omen of Cthulhu (PPSA26223).ffpfsc" \
    --game "Street Fighter 6 (PPSA02632).ffpfsc"
python3 blazingmount-host.py serve /srv/blazingmount --port 4460
```

`blazingmountd` on the console reads `catalog.txt` + the bundles from here and installs every
game by itself.

## engine/

`mkgame.py`, `amprindex.py`, `mkstub.py`, `pconsole.py` — the streaming engine's host helpers
that the builders reuse (sizes/index/stub logic). You don't call them directly;
`blazingmount-starter.py` imports them from here automatically.

**Adjust for your host:** `engine/mkgame.py` has a few constants from the reference setup —
`IMGSERVE` (default `http://127.0.0.1:4451`, the local image server), `DRIVE5_DL` /
`DIRSERVE_ROOT` (where your drives are mounted), and `MAC1_LAN` (the server IP written into a
cfg — the installer overrides this with `--host-ip`, so it only matters if you build cfgs by
hand). Point them at your own paths. The backport-related constants are unused on the native
path this tool uses (it refuses a game that needs newer firmware than the console).
