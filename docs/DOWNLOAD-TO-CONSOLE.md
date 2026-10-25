# Download to console: a full local copy of a streamed game

Status: 24 Oct 2026, design for payload **v2.8**, app **0.5.0** and the mac1 tool `host/download-to-console.py`.

Markus, 24 Oct, with PHAT5 away on a phone hotspot: *"is there a way to download some games to my internal drive from the
blazing mount?"* He also could not tell in the app which games are **on the console** and which **stream**.

## 1. What was measured first (PHAT5, 24 Oct, 19:4x-20:0x, away on a cellular hotspot)

- **The link is the cap, not the latency.** FTP into PHAT5 over the tailnet: 0.86 MB/s on one stream, 0.78 MB/s **in total**
  on four streams (RTT 53 ms at the time; 485-972 ms earlier in the evening). imgserve streaming to PHAT5 the same evening:
  0.5-1.5 MB/s per minute. A 30 GB game takes about 9 hours away from home, a 125 GB one about a day and a half, and it
  uses the same number of GB of the hotspot's data. At home the same copy takes minutes.
- **Parallel streams cannot write one file here.** PHAT5's ftpsrv 1.16 refuses `REST` past the end of a file
  (`551 Restart point beyond EOF`), and `REST n` + `STOR` in the middle of a file **truncates it at n + the bytes sent**.
  So one image arrives on ONE stream, appended in order. Resume = `REST <verified bytes>` + `STOR`, and the truncation then
  conveniently drops any unverified tail.
- **`SITE CPFR <src>` + `SITE CPTO <dst>`** copies a file on the console, in the background (`250 Copy started in
  background`; poll the size).
- **Never send `MTRW`** to ftpsrv 1.16: it remounts `/system`, `/system_ex` and `/preinst` read-write.
- **Some games are already on the console in full.** Wolverine (PPSA03671) had a 101.6 GB pkg install from 7 Oct in
  `/mnt/ext1/user/app/PPSA03671/app.pkg` under its streaming starter, and four images sat parked in
  `/mnt/ext1/blz-stage/parked` and `/data/blz-parked`. For those, "download" is a switch that takes seconds and sends 0 bytes.
- **Wolverine was switched by hand at 19:47 and proven at 20:04**: `hide` through the API (so the payload never rebuilds the
  starter), the starter renamed to `/mnt/ext1/blz-stage/stub/<name>-off-<stamp>`, the link cfg renamed to
  `.cfg.off-<stamp>`. ShadowMount logged `source removed` + `removed stale mount link`, and Markus's next launch ran the pkg's
  own `app0/fakelib` with **0 imgserve requests**.
- **Our link fork with no `/data/blazing/link/<TID>.cfg` is the stock AMPR emulator** (`blz_link_game.cpp`: "No config = the
  library behaves exactly like the stock emulator"). So a starter's own real files (fakelib + `ampr_emu.index` + any
  backported eboot) are a correct `backports/<TID>` overlay for the same game's full image.

## 2. The parts

| Part | File(s) | Job |
|---|---|---|
| mac1 tool + worker | `host/download-to-console.py` | moves the bytes (FTP), verifies them (on-console SHA-256), switches the game to local, removes a local copy, runs as a worker in a visible mac1 Terminal and answers the app's requests |
| payload v2.8 | `console/blazingmountd/src/bm_api.c`, `blazingmountd.c`, `build.sh` | records the app's requests, shows every copy a console has (pkg installs, parked images), passes the worker's progress through, hashes byte ranges of staged files |
| app 0.5.0 | `app/src/bm_app.h`, `app/fixtures/*`, `app/host/preview.cpp` | ON CONSOLE / STREAMS at a glance, an options list per game, the download confirm with size + free space + time, progress, Remove local copy |

The payload never moves game bytes and never talks to mac1 for this. mac1 reaches the console (it already does for FTP and
the API); the console only keeps two small files.

## 3. Console files

Both live in `/data/blazing/`. Lines are tab-separated; `#` lines and unknown lines are ignored.

**`downloads.tsv`: the app's requests.** Written ONLY by the payload's main loop when the app asks. One line per title
(the newest request replaces the older one); at most 256 lines.

```
# BlazingMount download requests (written by the payload; read by mac1's download-to-console.py)
download	PPSA03671	1792886400
remove_local	PPSA20397	1792886500
cancel	PPSA01603	1792886600
```

**`dl-status.tsv`: the worker's progress.** Written ONLY by mac1 (`STOR dl-status.tsv.tmp` + rename, at most every 30 s
while a job runs). The payload reads it, never writes it. Max 64 KB.

```
# written by mac1 download-to-console.py; read by blazingmountd (/api/downloads, /api/library, /api/status)
worker	1792886410	0.5.0
rate	1012345	1792886000	TAILNET
job	PPSA03671	sending	46300000000	125041901568	93500	1792886410	piece 44 of 117 verified
```

- `worker <unix time> <tool version>`: when the worker last wrote. Older than 120 s = the helper is not running.
- `rate <bytes/s> <unix time measured> <LAN|TAILNET>`: the last measured mac1 -> console speed.
- `job <TID> <state> <done bytes> <total bytes> <eta seconds> <unix time> <message>`; states:
  `queued`, `waiting` (game running / no space / payload too old to verify), `sending`, `verifying`, `switching`, `done`,
  `failed`, `cancelled`, `removing`, `removed`. The message is one line, no tabs, at most 200 bytes.

## 4. The payload (v2.8)

### 4.1 What a console has (`scan_local`)

Today a row has one `local` of `starter | full | image | none`, and only `homebrew` folders are looked at. v2.8 also finds:

- **pkg installs**: `/user/app/<TID>/app.pkg`, `/mnt/ext0/user/app/<TID>/app.pkg`, `/mnt/ext1/user/app/<TID>/app.pkg` with a
  size > 0 -> kind `pkg`, bytes = the app.pkg size, path = the folder.
- **parked copies**: every entry carrying a title id in `<drive>/blz-stage/parked/` for `/data`, `/mnt/ext0`, `/mnt/ext1`,
  `/mnt/usb0`, `/mnt/usb1`, and in `/data/blz-parked/` -> kind `parked` (bytes = file size, 0 for a folder).

The ACTIVE copy is the one ShadowMount launches: a homebrew entry (starter, full, image) wins over a pkg install; a parked
copy is never active.

### 4.2 `/api/library`, new fields per game (old fields unchanged)

| Field | Meaning |
|---|---|
| `local` | the ACTIVE copy's kind: `starter`, `full`, `image`, `pkg` (new) or `none` |
| `on_console` | `true` when the active copy plays without the network: `full`, `image` or `pkg` |
| `local_bytes` | bytes of the active copy (an image's or a pkg's size; 0 for a folder) |
| `copies` | every copy found: `[{"kind":"starter|full|image|pkg|parked","path":"...","bytes":N}]` |
| `request`, `request_at` | the newest line for this title in `downloads.tsv` (`download`, `remove_local`, `cancel`; `""`, 0 when none) |
| `dl_state`, `dl_done`, `dl_total`, `dl_eta`, `dl_at`, `dl_msg` | this title's `job` line from `dl-status.tsv` (`""` / 0 when none) |

`state` for a title with an active pkg install is `installed` (before v2.8 Wolverine read `hidden`).

### 4.3 New routes

| Route | Answer |
|---|---|
| `GET /api/downloads` | `{"version", "now", "requests":[{"what","tid","at"}], "worker_at", "rate", "rate_at", "rate_route", "jobs":[{"tid","state","done","total","eta","at","msg"}], "free_mb":{"data","ext0","ext1","usb0"}, "min_free_mb", "homebrew_dir", "route"}` - small; mac1's worker polls it every 20 s |
| `GET /api/sha256?path=P&off=O&len=L` | `{"path","off","len","size","sha256"}`: SHA-256 of bytes `[O, O+L)` of a regular file. `L` defaults to "to the end", and is capped at 1 GiB (1073741824) per call. `P` must start with `/data/blz-stage/incoming/`, `/mnt/ext0/blz-stage/incoming/`, `/mnt/ext1/blz-stage/incoming/`, `/mnt/usb0/blz-stage/incoming/` or `/mnt/usb1/blz-stage/incoming/`, and must not contain `..`, `//`, `/./` or a control character. 400 bad input, 403 path not allowed, 404 missing. Runs in the API thread (about 2-5 s per GiB on a console); the app's 1 s status poll waits that long at worst. |
| `POST /api/action?do=download&tid=T` | (needs `X-BM-Request: 1`, like every action) queued; the main loop writes the line to `downloads.tsv` |
| `POST /api/action?do=remove_local&tid=T` | same |
| `POST /api/action?do=cancel_dl&tid=T` | same; the line's word is `cancel` |

`/api/status` gains `dl_rate`, `dl_rate_at`, `dl_worker_at` and `dl_active` (jobs in `queued`, `waiting`, `sending`,
`verifying`, `switching`, `removing`).

### 4.4 The builder

`scan_present` (`blazingmountd.c`) also counts pkg installs (same three `user/app` roots, app.pkg size > 0): a title the
console has installed must never get a second tile from a starter. (The tool also marks a switched title `skip`; this is the
second guard.)

`build.sh` labels the payload `2.8-<sha>`.

## 5. The mac1 tool: `host/download-to-console.py`

Stdlib only, Python 3.9+, run with `python3 -I`. A console is a name (PHAT5, DIGI5: found by identity through blazing-ps5's
`tools/ps5where.py`, `$BLAZING_PS5_TOOLS` or `~/NYCT/blazing-ps5/tools`) or an IP. Ports: FTP 2121, websrv 8080, API 8094.

```
download-to-console.py plan     CONSOLE TID...            size, what the console already has, free space, time; changes nothing   [done]
download-to-console.py get      CONSOLE TID... [--notice]  copy (or reuse a full copy already there), verify, switch; resumes     [done]
download-to-console.py verify   CONSOLE TID...            only the piece check of what was sent (after a payload update)          [done]
download-to-console.py switch   CONSOLE TID               only the switch (everything sent and verified)                          [done]
download-to-console.py cancel   CONSOLE TID               stop wanting it; deletes ONLY its staged files in incoming/              [done]
download-to-console.py status                             the journal, human-readable                                               [done]
download-to-console.py remove-local CONSOLE TID --confirm  back to streaming; deletes the local image (frees the space)           [done 25 Oct]
download-to-console.py serve    [CONSOLE...]              the worker: answers the app (downloads.tsv), writes dl-status.tsv         [done]
download-to-console.py prove    CONSOLE TID... [--minutes 5] waits for the title to run, then counts imgserve requests for it      [done 25 Oct]
```

Built 24 Oct evening with the first three games (Smurfs 2, Samson, Galactic Racer) already copying, so it differs from the plan
above in these ways:

- **State**: one file per job, `~/.blazing/download-local/jobs/<console>-<TID>.json` (pieces, checks, every rename for undo),
  plus `log`. Two runs on different titles never overwrite each other; an `flock` on `jobs/<console>-<TID>.lock` keeps a second
  run off a title that one is already working on. `touch ~/.blazing/download-local/STOP` stops every run cleanly.
- **Several streams = several titles at once.** On a 270 ms hotspot ONE FTP stream was window-bound at 0.2-0.3 MB/s and two
  streams gave ~0.8 MB/s together (24 Oct 20:4x). ftpsrv truncates on `REST n` + `STOR`, so one file cannot take two streams:
  run one `get` per title (one Terminal each, `~/.blazing/download-local/download-local*.command`).
- **Folder games** (dirserve :4452, a link cfg with `server ... 4452` and `file <size> <path>` lines = 0-byte placeholders in the
  starter): each file goes to `<drive>/blz-stage/incoming/<TID>-files/<path>.part`, is checked piece by piece the same way,
  and the switch moves each placeholder to `<drive>/blz-stage/stub/<TID>-placeholders-<stamp>/<path>`, the real file into its
  place, and the cfg off. The starter folder is then the whole game (no ShadowMount change).
- **A full copy on ANOTHER drive of the console** (Galactic Racer: `/data/blz-parked`): ftpsrv `SITE CPFR`/`SITE CPTO` copies it
  ON the console into `<target drive>/blz-stage/incoming/<name>.part` (0 network bytes), only while no game runs
  (`--while-playing` overrides); mac1 hashes its own copy meanwhile, then the console's pieces are checked like a sent file.
  The other-drive copy is left where it is: deleting it is Markus's call.
- **The overlay** is restored only when the game needs a newer FW than the console (`games.json` `fw` > console FW):
  Smurfs 2 (7.00) on PHAT5 (10.20) gets none. Restoring = the newest `<drive>/blz-disabled/<TID>-overlay-*`; with none kept the
  switch stops BEFORE the starter is parked (copying the starter's real files as the overlay is not built yet).
- **Yield**: while imgserve logged a request in the last 45 s (a streamed game is playing), sending pauses. dirserve and SMB
  traffic are not seen by this check.
- **Console address**: Remote Play discovery does not answer during some games, so a console name falls back to its last proven
  (`ps5where` cache) or Tailscale address when websrv answers there.
- Python 3.14 refuses a tuple inside an `except` tuple: the tool keeps one flat `NET_ERRORS`.

### 5.1 Which image, which file

- The title's images come from imgserve's `GET /games.json` (`http://127.0.0.1:4451`, `$BM_IMGSERVE` overrides): `key`,
  `name`, `size`. With several images of one title, the one whose key the console's starter streams (`/api/library` `key`)
  wins, then a name containing the console's name (`... PHAT5.ffpfsc`), then the first.
- The file is found by its exact name under the imgserve roots (`/Volumes/Untitled`, `/Volumes/My Book`, depth <= 4;
  `$BM_DL_ROOTS` = `:`-separated overrides). Its size must equal the key's bytes.
- The reference hash, when there is one: `<file>.verified.json` `imageSha256`, else `<file>.hash.json` `sha256`.

### 5.2 Where on the console

- Target drive = the drive of the payload's `homebrew_dir` (`/api/status`): `/mnt/ext1` on PHAT5.
- **Never `/data` on a console that has an M.2** (`free_mb.ext1` > 1): refused. A console without one (DIGI5) uses `/data`.
- Staging: `<drive>/blz-stage/incoming/<image name>.part` (three levels deep: ShadowMount never scans it).
- Space kept free: `min_free_mb` from `/api/status` (default 51200), and at least 55 GiB on `/data`
  (memory ps5-data-stops-writes-near-52gib). A job that does not fit is `failed` with the numbers, before a byte is sent.

### 5.3 The transfer

- One FTP stream, `TYPE I`, 4 MiB reads from the drive, 1 MiB sends.
- **Pieces of 1 GiB.** mac1 hashes each piece (SHA-256) as it reads it. After a piece lands, the tool asks the console
  for the same range (`/api/sha256`) and compares. A match is written to the journal at once.
- **Resume** after any drop (Wi-Fi, mac1 restart, console sleep) = the end of the last VERIFIED piece: `REST <that offset>`
  + `STOR` (ftpsrv truncates the unverified tail there). A piece that does not match is sent again the same way; three
  failures in a row on one piece = `failed`.
- **The whole file**: mac1's full-file SHA-256 (computed while reading) must equal the reference hash when there is one.
  Every byte on the console was compared piece by piece, so the console never has to read the whole image twice and
  the hotspot never carries it twice.
- **No verify route** (payload older than v2.8): pieces are sent but stay unverified, the job waits with
  `waiting: payload 2.8 needed to verify` and finishes by itself once the payload is updated. `--readback` (LAN only, refused
  when `/api/status` route is TAILNET) reads the pieces back over websrv instead.
- Speed: measured over the last 60 s; written to `dl-status.tsv` `rate` and the journal. While ANY title runs on that console
  the tool keeps the speed under `--playing-mbps` (default 25 MB/s): a game on the same M.2 must not stutter (memory
  ps5-background-io-yields-to-games). The hotspot is far below that anyway.

### 5.4 The switch to local (the Wolverine steps, made general)

Never while THIS title runs (`/mnt/sandbox/<TID>_000` + its eboot in pldmgr `:8084`): the job waits. Another title running is
fine (ShadowMount applies the change after it). When `ps5-claim` is on PATH the tool claims the console for the switch only
(seconds), never for the transfer, and releases it after.

1. `POST /api/action?do=hide&tid=T` first, so the payload can never rebuild a starter in between.
2. **Overlay** `<drive>/homebrew/backports/<TID>/`:
   - a proven one moved out earlier (`<drive>/blz-disabled/<TID>-overlay-*`, the newest) is renamed back;
   - else, when the active starter has `fakelib/`, the starter's REAL files (every file NOT listed as a streamed placeholder
     in its link cfg's `file` lines: fakelib, `ampr_emu.index`, eboot, sce_module, ...) are copied there on the console
     (`SITE CPFR/CPTO`, sizes checked);
   - an existing `backports/<TID>` that the tool did not make = stop and ask (`failed`).
3. Park the starter: `<drive>/homebrew/<starter>` -> `<drive>/blz-stage/stub/<starter>-off-<stamp>` (rename, same drive).
4. The local copy into homebrew: `incoming/<name>.part` -> `<drive>/homebrew/<name>`; or a parked image
   `<drive>/blz-stage/parked/<name>` -> `<drive>/homebrew/<name>`; a pkg install needs no move. A rename never crosses drives
   (a cross-device rename panics the kernel: ps5upload's note); a parked copy on another drive than the target is refused.
5. The link cfg `/data/blazing/link/<TID>.cfg` -> `.cfg.off-<stamp>`.
6. Wait up to 3 minutes for ShadowMount (`/data/shadowmount/debug.log`): `source removed` for the starter, then the image
   registered/remounted, and `/user/app/<TID>/mount.lnk` pointing at the new copy (or gone, for a pkg install).
7. Journal + `~/.blazing/phat5-games/PARKED.log`-style line in the tool's log; `dl-status` `done`; a TV notice: "<Title> is
   on your console now: it plays without the network."

### 5.5 Remove local copy

Only for a copy the tool itself switched in, or an image in `homebrew` that matches the drives' image by name and size.
**A pkg install is never deleted** (`failed`: "an installed game: remove it in the PS5's Storage settings").

1. Not while the title runs (waits).
2. The image `homebrew/<name>` -> `<drive>/blz-stage/incoming/<name>` (rename) and the overlay -> `<drive>/blz-disabled/
   <TID>-overlay-<stamp>`.
3. Wait for ShadowMount's `source removed`.
4. Streaming back: the parked starter from the journal (or the newest `<drive>/blz-stage/stub/<starter>-off-*`) renamed into
   homebrew + its cfg renamed back; `POST do=unhide`. With no parked starter, `unhide` alone: the payload builds a new one
   on its next pass.
5. `DELE` the image from `incoming/`. **The tool deletes only inside `<drive>/blz-stage/incoming/`.**

As built (25 Oct 2026):

- **The steps come from the switch's own journal** (`undo` in `jobs/<console>-<TID>.json`): each recorded rename is sorted into
  OUT (the image -> `incoming/<name>.remove-<stamp>`, whatever it came from: sent, parked or copied on the console; the overlay
  the switch restored -> its old `blz-disabled/` name; a folder game's real files -> their `.part` in `incoming/`) and BACK (cfg,
  parked starter, 0-byte placeholders). An undo step it does not recognise = refused before any change. The plan is saved in the
  job (`removal`) before the first rename, so a cut-off run resumes the same plan; every rename checks first (source there,
  target not = do it; the reverse = done before; anything else = stop).
- **No journal**: only an image in homebrew that IS the drives' image (name + size), with the newest parked starter
  (`stub/<starter>-off-<stamp>`) and newest `cfg.off-*` brought back; with no parked starter the payload gets `install`
  (it builds a new starter even with auto off) instead of `unhide`. Refused with the reason, nothing changed: a pkg install
  ("an installed game: remove it in the PS5's Storage settings"), a folder copy with no cfg ("a full copy the download helper
  did not make"), an image that is not the drives' copy, a title that still streams.
- **ShadowMount first.** It keeps the FIRST copy it sees of a title, and it does nothing while a game runs (PHAT5, 25 Oct: the
  RE2 switch at 09:50 was applied at 10:01:19, when 007 First Light closed). So the starter comes back only after a NEW
  `[CACHE] source removed: <TID>` line (or `/user/app/<TID>/mount.lnk` gone). That wait has no limit while a game runs; after
  10 idle minutes (`BM_DL_SMP_GONE_WAIT`) with no sign the OUT renames are reversed: the copy is back in homebrew and still
  plays, state `failed` with the reason, the journal kept (`get` still sees it as on the console; asking again resumes).
- An image game is `hide`-den while it changes (the payload must not build a starter in between), then `unhide`.
- The job ends `removed` with `files`, `undo`, `switched_at` cleared (a later `download` is a fresh copy) and the old undo in
  `history`. TV notice + hub line. `serve` runs it for the app's `remove_local` (`--confirm`: the app's own dialog asked).
- `claim` (switch and remove alike) waits up to 30 min for another agent's lock instead of failing the run at its last step.

### 5.5.1 `prove` (as built)

Passive: it never launches anything. Waits (up to `--wait-hours`, default 12) until one of the titles runs (`ps5-status`),
then counts imgserve.log + dirserve.log lines naming the title from the look before the launch until `--minutes` after.
imgserve.log carries no client address, so another console running the same title in that window (checked every poll) makes
the result INCONCLUSIVE (exit 3). 0 requests = PROVEN LOCAL: a line in `~/.blazing/games-proven.tsv`, `proven_at` in the job,
hub line (exit 0). Any request = NOT LOCAL (exit 1). A title gone within a minute is a failed launch: it waits for the next one.

### 5.6 The worker (`serve`)

- Runs in a visible mac1 Terminal (`window` writes `~/.blazing/download-local/download-local.command` and opens it).
- Every 20 s per console: `GET /api/downloads`. A request is new when `(tid, at)` is not in the journal yet. Jobs run in
  request order, one per console; `cancel` stops a running transfer at the next block.
- Writes `dl-status.tsv` when something changed, at most every 30 s while sending, and a `worker` heartbeat every 60 s.
- Measures the speed once when it starts on a console and again when the console's `route` changes (`measure`, ~15 s).
- A console that does not answer is "not reachable from here", retried every minute; nothing is assumed about it.
- Hub line per job start / done / fail when `$BM_HUB_FILE` is set (house: `~/.claude-team/chat/roku-channels.md`).

### 5.7 Proof (`prove`)

Passive only: it never launches anything. It waits until the title runs, then counts imgserve log lines `GET <TID>@`
(`~/.blazing/imgserve/imgserve.log`, `$BM_IMGSERVE_LOG`) from the launch for `--minutes`: 0 = **PROVEN LOCAL**.

## 6. The app (0.5.0)

- **At a glance:** every grid tile and list row carries **ON CONSOLE** (green) or **STREAMS** (dim outline), or
  **DOWNLOADING 37%** while a job runs, **DOWNLOAD FAILED** (red outline) after one fails.
- **Show (OPTIONS):** "On this console" now means `on_console` (it used to include starters); new "Streams" (`local` = starter).
- **Detail panel, "This console":** "On this console: installed game, 101.6 GB (plays without the network)", "Streams from
  mac1", or "Downloading: 37% - 46.3 of 125.0 GB - 1 d 2 h left at 1.0 MB/s". "Also here: ..." lists other copies.
- **CROSS opens the game's options** (a short list, up/down, CROSS picks, CIRCLE closes): Download to console, Remove local
  copy, Cancel download, Install, Remove starter, Rebuild, Hide / Show again - only the ones that apply. TRIANGLE (Rebuild)
  and SQUARE (Hide) keep their shortcuts.
- **Download confirm** shows, before anything starts: the size; the target drive's free space now and after; the time at
  the last measured speed ("speed not measured yet" when there is none); "uses about N GB of your internet connection"
  when the route is TAILNET; "a full copy is already on this console: switching takes seconds, nothing is downloaded" when
  `copies` has a pkg or parked copy. It refuses (toast) when the space after would be below `min_free_mb`, and warns when
  `dl_worker_at` is older than 120 s ("mac1's download helper is not running: the request waits for it").
- **Remove local copy confirm:** "Frees N GB on the M.2. The game streams from mac1 again."
- **Activity tab:** a Downloads section on top while any job exists (title, state, %, time left).

## 7. Tests

- `host/test_api.py` (fake console, HOST build): the new kinds and fields, downloads.tsv writes, dl-status passthrough,
  `/api/sha256` against Python's hashlib (whole file, a range, the 1 GiB cap, every refused path), `scan_present` counting
  a pkg install.
- `host/test_download_to_console.py`: the tool against a fake console (a small ftpsrv with 1.16's REST rules, a websrv
  listing, a fake API): resume at a verified piece after a cut, a corrupt piece sent again, refusal of `/data` with an M.2,
  space refusal, switch renames + overlay from the starter's real files, remove-local deleting only in `incoming/`, a pkg
  install never deleted, cancel.
  Since 25 Oct also remove-local (image, folder game, overlay back, ShadowMount silent -> put back, pkg + stranger's copy
  refused, through `serve`, downloaded again after; every DELE checked to be in `incoming/`) and prove (proven, not local,
  inconclusive, never ran): 55 checks.
- `host/test_download_e2e.py` (NOT BUILT YET): the tool against the REAL host-built payload (`BM_ROOT`) + the fake ftpsrv/websrv
  over the same root: app request -> worker -> transfer -> `/api/sha256` verify -> switch -> `/api/library` shows `on_console`.

## 8. Rollout

1. Payload: `build.sh` on mac2 (cross-compile), then on each console when no game runs: slot ELF swapped (old kept `.bak`),
   old process stopped by `/data/blazing/blazingmountd.pid`, new one loaded once (the v2.5 / v2.7 procedure).
2. App 0.5.0: `app/ps5/dell-build.sh`, then put-tree to `/data/homebrew/PPSA99280` when the app is not running.
3. Worker: `download-to-console.py window` on mac1.
4. Both consoles (cross-platform rule). DIGI5 has no M.2: it uses `/data` with the 55 GiB reserve.
