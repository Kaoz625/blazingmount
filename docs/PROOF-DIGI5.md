# Proof run — a game registered and played, streamed, on stock firmware

This is the first end-to-end proof that BlazingMount works, captured 2026-10-22 on a
digital-edition PS5 ("DIGI5") running firmware **13.60**, jailbroken, with ShadowMount Plus
already installed. The game image lived on a separate host machine (firmware-neutral); the
console stored only a 327 MB starter and streamed the rest.

**Game:** *The Mound: Omen of Cthulhu* (`PPSA26223`), required FW **13.2** (so it runs
**native** on 13.60 — no downgrade, no re-sign). 1 big pak of 24.5 GB plus 5 more streamed
files; **327 MB** starter on the console.

### 1. Installed (host-assisted path)

- `blazingmount-starter.py` built the native starter (small files + 0-byte placeholders for the
  6 big files + `ampr_emu.index` with the real sizes + `fakelib/libSceAmpr.sprx`).
- 51 files pushed to `/data/homebrew/...`, **every file read back and verified by sha256**.
- Link config written to `/data/blazing/link/PPSA26223.cfg` (`mode remote`, `server <host> 4451`).

### 2. Registered as a tile (ShadowMount Plus)

```
[ACTION] Installing: The Mound: Omen of Cthulhu (PPSA26223)
[LINK] nullfs mounted: /data/homebrew/The Mound ... -> /system_ex/app/PPSA26223
[LINK] mount.lnk created: /user/app/PPSA26223/mount.lnk
[SHELLCORE] AppInstallAll bridge consumed: PPSA26223 result=0x00000000
NOTIFY: installed game PPSA26223
```

### 3. Played, streamed from the host (0 errors)

Launched from the tile. The link fork loaded and streamed:

```
[AMPR_EMU] name=libSceAmpr version=0.4.2.1 (c) Drakmor
[AMPR_EMU] [blz_link] fork loaded: SMB link build ...
[AMPR_EMU] [blz_link] title PPSA26223, config /data/blazing/link/PPSA26223.cfg
[AMPR_EMU] [blz_link] ACTIVE mode=remote server=<host>:4451 files=6 conns=4 async=on
[AMPR_EMU] [blz_link] selftest OK: 4096 bytes of pakchunk0-ps5.ucas ... (first bytes 01 00 00 00)
[blz_link] stats t=35.5 s  remote http 2090 req 569 MiB  errs 0  | cache hit 454 miss 118 | verify err 0
```

- Console status: `RUNNING PPSA26223 ... native=1`, eboot resident **5.1 GB**.
- Host file server (`imgserve`) log: hundreds of thousands of `206` partial-content responses
  for the streamed paks, e.g.
  `GET PPSA26223@.../pakchunk1-ps5.ucas range=... -> 206 262144 B 3 ms`.

**Result:** the console showed the game and played it, streamed on demand from the host,
with the full image never copied to the console and **zero stream errors**.

### 4. The autonomous daemon installed a game by itself (blazingmountd)

`blazingmountd` was built with the PS5 payload SDK and run on the same console in one-shot mode
(`blazingmountd once`) against a small host catalog. It read `/data/blazing/mount.conf` and acted
on its own — the host's server log shows the console fetching by itself:

```
192.168.1.60 - GET /catalog.txt HTTP/1.0 200
192.168.1.60 - GET /bundles/<TID>.starter.tar HTTP/1.0 200
192.168.1.60 - GET /cfg/<TID>.cfg HTTP/1.0 200
```

Its log (`/data/blazing/blazingmountd.log`):

```
blazingmountd once
installing <name> (<TID>)
  done: <name> -> tile (ShadowMount) + link /data/blazing/link/<TID>.cfg
```

Afterwards, on the console: the bundle was untarred to `/data/homebrew/<name>/`, the fork was in
`<name>/fakelib/libSceAmpr.sprx`, and `/data/blazing/link/<TID>.cfg` was written — with **no
computer driving the console**. Combined with the ShadowMount registration and streaming proven
above, this is the fully-autonomous "no setup script" path.

> Reproduce with your own legally-owned games:
> - per game: `python3 install/blazingmount-install.py --console <ip> --host-ip <ip> --game "<your image>" --console-fw <fw>`
> - autonomous: build a catalog with `host/blazingmount-host.py`, point `blazingmountd` at it, and it installs everything by itself.
