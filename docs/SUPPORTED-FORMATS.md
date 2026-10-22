# Supported content and limits

BlazingMount is content-neutral: you bring your own legally-owned games and homebrew. It does
not fetch, crack, sign, decrypt, or convert anything.

## Game sources on the host

| Source | Served by | Notes |
|--------|-----------|-------|
| `.ffpfsc` (compressed PFS image) | `imgserve` | files are streamed from *inside* the image, no unpack |
| game **folder** | `dirserve.py` | a dumped game laid out as files on disk |

Plain (uncompressed) PFS images are not read by `imgserve` yet — use a folder or a `.ffpfsc`.

## Firmware rule (important)

This tool plays your games **natively**. It does **not** downgrade or re-sign a game, so:

> **The console firmware must be ≥ the game's required firmware.**

`blazingmount-starter.py` checks this and **refuses** a game that needs newer firmware than the
console you named (`--console-fw`). (Playing a game whose required FW is *higher* than the
console would need a separate backport/re-sign step, which is out of scope for this tool.)

## What streams vs. what stays local

- Files the game reads through APR (its big `.pak`/`.ucas`/`.utoc`, movies, etc.) stream from
  the host. The starter keeps them as 0-byte placeholders; `ampr_emu.index` carries their real
  sizes so the game stats them correctly.
- Everything small (the executable, config, certs, icons) stays on the console in the starter.
- Up to a large number of big files per game may be streamed; a game that does its own async
  I/O is handled by the AIO-intercept build of the fork (the default prebuilt binary).

## Requirements on the console

- A jailbroken PS5 with **ShadowMount Plus** (for tiles) and an FTP server (`ftpsrv`) + web file
  server (`websrv`) running — standard on the usual homebrew stacks.
- Space for the starters (a few hundred MB each), not the full images.

## Requirements on the host

- `imgserve` needs **.NET** to build/run; `dirserve.py` and all the builders need **python3**.
- The host and console must be on the same LAN (the console streams from the host's LAN IP;
  userspace VPN addresses on the console are not reachable by the game process).
