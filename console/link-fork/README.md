# console/link-fork — the demand-paging streaming engine (`libSceAmpr.sprx`)

This is what streams a game's big files from the host while it runs. It is a **fork of
`drakmor/ampr_emu`** (GPL-3): we overlay our `module/` onto a clean checkout pinned at commit
`cfa85df379f6eeeb165d7badf9b648e266fe77b7` (master, 2026-09-06, 4 commits after tag 0.4.2.1).

> **License:** derivative of GPL-3 `ampr_emu`, so this and everything that links it is
> GPL-3.0-or-later. Credit and the pre-release license checklist are in `../../CREDITS.md`.

## What it does

The game reads its paks through APR / the AIO path. For any file listed in
`/data/blazing/link/<TID>.cfg`, the fork fetches the needed byte ranges over HTTP from the host,
on demand, into a small in-RAM cache, and serves them to the game — the 0-byte placeholder on
disk is never read. With no config for a title, the fork is inert (stock `ampr_emu` behaviour),
so it is safe to leave installed globally.

## Layout

```
module/            OUR code (overlaid onto ampr_emu)
  blz_http.cpp/.h       HTTP/1.1 Range client (non-blocking sockets, poll timeouts)
  blz_link_core.cpp     engine: config, block cache, modes, fd table, virtual AIO, tracing
  blz_link_game.cpp     PRX glue: lazy start at first real open(), kernel symbols by name
  blz_link.h, blz_link_hooks.h
  DEMAND-PAGING.md      the mmap demand-paging notes
  test_link.cpp/.py     host tests against a mock Range server with fault routes
  probe.cpp             host throughput probe
tools/
  ampr_link_apply.py    overlays module/ onto a clean ampr_emu checkout (exact-string patches)
  build-fork.sh         builds the fork with the PS5 payload SDK
prebuilt/
  libSceAmpr.sprx       a build of the fork (no game data) + VERSION.txt
```

## Build

`build-fork.sh` clones `ampr_emu` at the pinned commit, applies `ampr_link_apply.py`, and builds
with the PS5 payload SDK (this project uses llvm@18 / prospero-clang). Read the script header for
the exact toolchain and the `AMPR_PROCESS_AIO_INTERCEPT` / `AMPR_PROCESS_SYNC_READ_INTERCEPT`
build flags (the prebuilt binary is the AIO-intercept variant, which also handles games that do
their own async reads).

Per the "pin build deps to a commit and vendor" rule: the exact upstream commit is recorded
above; keep a mirror of `ampr_emu` at that commit so a yanked upstream release can't break the
build.

## Config format

See `module/` and the host builders; a cfg looks like:

```
mode remote
server 192.168.1.130 4451
base /t/<TID>@<image-bytes>
file <size> <path relative to /app0>
block_kb 256  cache_mb 32  conns 4  async 1  timeout_ms 8000
```

## Host tests (no console needed)

```sh
python3 module/test_link.py      # engine against a mock Range server with injected faults
```
