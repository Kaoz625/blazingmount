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

Executable modules (`.prx`, `.sprx`, `.elf`, `.self`) are always kept as real files in the starter, wherever they sit in the game:
the game's own loader opens them straight from `/app0`, and a 0-byte placeholder there kills the launch at once
(`rtld self_load_shared_object: size 0`, EA SPORTS FC 27, 22 Oct 2026). Their names are listed as `kept_local` in the manifest.

`--check-only` builds nothing and prints one JSON line with the same verdict a real build would reach
(`ok`, `refusal`, `starter_bytes`, `remote_bytes`, ...). `--max-starter-mb N` refuses a game whose starter
(what the console would actually store) is bigger than N MB - a starter that is most of the game is not a
streaming starter; `blazingmount-host.py build` passes it through.

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

`build` writes a catalog of exactly the games named in that call. Add `--merge` to keep the games
already in the catalog and add (or replace, by title id) only the ones you name - the way to grow
one catalog a game at a time, or from two tools at once (the catalog is locked while it is read and
rewritten, and every file is renamed into place so a console polling mid-build never sees half of one).

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

## Network sources, console profiles, remote wake (blazingmountd 2.12)

```sh
python3 host/bm-console.py state                         # PHAT5 / DIGI5: awake, rest mode or silent (discovery only - never wakes)
python3 host/bm-console.py wake DIGI5                    # a console in REST MODE: the Remote Play wake packet; refuses an awake/silent one
python3 host/bm-console.py profile PHAT5 set --target m2 --source mac1-ftp --user Peace_Maker --name PHAT5
python3 host/bm-console.py src PHAT5 add mac1-ftp ftp 192.168.1.130:2121 /pkgs --user u --pass-env MY_FTP_PW
python3 host/bm-console.py src PHAT5 ls mac1-ftp          # browse; split X.pkg.N sets show as ONE X.pkg
python3 host/bm-console.py install PHAT5 mac1-ftp "Apps/Cool App.pkg"     # live %, MB/s, ETA; Ctrl-C cancels
```

A source is FTP (`/dir`), SMB (`share[/dir]`) or WebDAV (`/dav`, plain http); the payload reads the bytes itself and hands Sony's installer the
127.0.0.1 proxy, so nothing is copied to the console first. The login is saved ON THE CONSOLE in `/data/blazing/sources.conf` (mode 0600) and is
never listed, logged or committed. `GET /src/<name>/<path>` on :8094 is a Range gateway to a source's file (a game image from FTP/SMB/WebDAV).
Tests: `test_netsrc.py` (our own FTP + WebDAV servers, rclone's if installed, a real SMB share via `BM_TEST_SMB`), `test_wake.py` (fake console).
