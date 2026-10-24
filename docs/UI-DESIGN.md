# BlazingMount app: the native screens for the payload

Status: 24 Oct 2026, v0.1.0 (MVP). Markus asked for:

> "we need to make visible screens for the payload like some of the other payloads have. i want it to be native. the mounter
> should show our full library and i should also be able to choose what i want and what i dont want. this will be a good way
> to have a visual of whats installed, whats not installed, whats compressed/not, tells the file type. theres lots of other
> settings. between phat5 and digi5 apps and payloads use those as design teachers too. add what we should add."

He also asked that the app show changes on the drives (a game added, rebuilt or updated) by itself.

## 1. The decision: a native PS5 title, talking to the payload

The design has two parts.

- **blazingmountd** (the payload) stays the only thing that changes the console. It gains a small local API on `:8094`
  (`console/blazingmountd/src/bm_api.c`, section 6).
- **The BlazingMount app** (`app/`, title **PPSA99280**, its own tile) draws the screens with the GPU, reads the
  controller, and only talks to that API.

A PS5 title cannot read `/data`, so the app could not see the library by itself anyway. Splitting it this way also keeps
every change to the console inside the payload and its log: a crash is always one of two known things.

**The stack:** our own, from Multiverse Fighters.
- mihawk-99's PS5_VulkanTemplate (MIT) on PS5_Vulkan/RADV, built on the Dell WSL with the toolchain already in
  `/root/mf-ps5-build`.
- MF's UI pass: `ui_vk.cpp` is one Vulkan pipeline drawing a list of quads.
- A Barlow Condensed distance-field atlas, which we rebake with Latin-1 and Latin Extended-A so game titles keep ®, ™, ō and é.

**Why this stack, and not one of the others:**

| Option | Native? | Time to first screen | Why not (or why) |
|---|---|---|---|
| **MF's UI pass on PS5_VulkanTemplate** (chosen) | yes, GPU | a day | Toolchain, pipeline and font already proven on PHAT5 (MF 0.3+, 18 Oct). The same quads go through a software raster on the Mac, so every screen gets a picture and a rules check before any console is touched. ONE LIGHT is easy to hit exactly. |
| Orbit store's TV app (EGL + OpenGL 4.6, ps5-homebrew-ui) | yes | days | The best match for our architecture: a backend payload on 127.0.0.1 plus a native app. But it is a second graphics stack we have never built, and its upstream UI kit is gone (404). We copy its ideas, not its code. |
| mihawk's UI kit (`new-title.py --ui paper`, PS5_VKHomebrewUI) | yes | days | Proven, with Library and shelf designs, but its own look and no Mac preview. Worth a second look for the cover-shelf view. |
| SDL2 + RmlUi (jtplay) | yes, CPU-drawn | days | Plainer, and draws on the CPU. |
| A web page plus a `deeplinkUri` tile (pldmgr, SMPlusGui, ps5upload Minion) | no | hours | Not native: the PS5 browser with mouse-style focus. Markus said native. We serve one anyway as a fallback for phones (`GET /`). |

## 2. The teachers (both consoles, read-only survey, 24 Oct)

DIGI5 runs pldmgr 0.6.3 with 10 local and 30 repo payloads. PHAT5 runs pldmgr 0.6.3 with 19 local and 74 repo payloads.
Running on DIGI5: SMPlusGui, ps5upload-tile, web-file-mgr, ShadowMount+, pegasus_dl, SpectrumLibrary, PS5SX2 Helper,
pkgmgr and etaHEN's FPS sampler. Sources were found on GitHub with `gh`.

| Teacher | How it draws | What we take | What we avoid |
|---|---|---|---|
| **pldmgr** (itsPLK, :8084, React) | Web page. The payload installs its own tile (`deeplinkUri` http://127.0.0.1:8084) | One ELF carries its tile; an autoload list; a log view; toasts; a process list | Mouse-style focus on a TV |
| **SMPlusGui** (KarnerF, :7777, Mongoose) | Web page | An API shape: `/api/status`, `/api/sm/games`, mount/unmount per title, `/api/log`, config backup | A settings form, not a library |
| **PKG Manager** (PLK, :8844) | Web page | `/api/poll`, `/api/queue`, `/api/storage`, `/api/drives`, cache stats, leftovers; one poll endpoint drives every view | Desktop-first |
| **Orbit store** (saawant12; our fork `~/NYCT/orbit-blazing`) | **Native** EGL/GL app PPSA99177 + backend payload :34177 | **Our architecture**: Library with status, a Downloads list (active/failed/finished), filters, a game page, settings | Its app starts the payload through elfldr; we never do (payloads only, his autoload) |
| **Itemzflow** (LightningMods, PS4 public) | Native GLES2, coverflow | Daemon/UI split, cover cache, a game page with every fact | Old toolchain |
| **PS5SX2** (Swordpdf) | Native Vulkan cover-flow shelf | Covers as the main surface; a QR code to a phone settings page | |
| **etaHEN toolbox** | Hooks ShellUI's Debug Settings | Looks exactly like the system | Fragile, depends on FW |
| **Tailscale on PS5** (:8090) | Web page, password | Plain-words "what this does and does not" | |
| **ProsperoMgr**, **BFpilot / web-file-manager** | Web | Progress of background jobs, storage bars | |
| **BlazingArch** (PPSA99171, mihawk PS5_RetroArch) | Native Vulkan (RetroArch XMB) | Proof the Vulkan path is solid on both consoles | |

Not found as public source: spectrum-library, tile-launcher, Pegasus on PS5, the tsnet payload, PS5 Itemzflow.

**Rules we took from all of them:**
- One poll endpoint drives every view.
- The daemon does the work and the UI only asks.
- Every row says what it is in one word.
- Destructive actions confirm.
- A toast answers every press.

## 3. The screens (1920 × 1080, DESIGN-V2 "ONE LIGHT")

The colour tokens are from DESIGN-V2 section 2.5, made opaque over `bg`:
- `bg` #0A0A0B, `surface` #0E0A0C, `surfaceHi` #171012
- `line` #FF3D47 at 20%
- focus #FF3D47, focus fill #FF3D47 at 78%
- `good` #4CD48A (the one green), `warn` #FFB84D (toasts only)

Layout: 90 px gutters, nothing readable below y = 1010, a 16 px radius everywhere. Type is Barlow at HEAD 40, BODY 28, META 24
and WHISPER 21: one step up from V2, because the face is condensed.

**Top band (every tab).** BLAZINGMOUNT, then the tabs `L1 Library · Activity · Settings R1`, with the active tab marked by the
one 6 px accent bar. On the right are live chips:
- **BUILDING <game>** (solid red, only while it happens)
- **HOME (LAN)** in green, or **AWAY (TAILNET)** outlined red
- the free space where starters go (`/data` or the M.2 when `homebrew_dir` is `/mnt/ext1/...`)
- **DRIVES OFFLINE** when imgserve does not answer

### 3.1 Library

Every game on the drives, plus every game this console has that is no longer on them.

**Filter chips, `L2` / `R2`**, each with its count:
- All
- On this console
- Not installed
- Needs attention
- Hidden
- PS5
- PS4

**Sort, `OPTIONS`:** Title, Size, or Status (playing and building first, then what needs you).

**Each row:**
- a plate with the initials (cover art in v0.2)
- the title
- `PPSA02849 · PS5 · .ffpfsc compressed · 1.3 GB · FW 4.50`
- on the right, one status badge and a **PLAYED** badge when the game has a proof line

| Badge | Meaning | Tone |
|---|---|---|
| STREAMING | a starter of ours is on the console | green |
| FULL COPY / IMAGE | a full copy or an image file (BlazingMount leaves it alone) | green |
| PLAYING | its sandbox is up right now | solid red |
| BUILDING | the payload is making its starter now | solid red |
| QUEUED | will be built on the next pass | dim |
| AVAILABLE | on the drives; you pick it (when new games are set to "only the ones I pick") | dim |
| HIDDEN | you chose not to have it | dim |
| OLD IMAGE | the starter streams from an image no longer on the drives (v2.5 auto-update rebuilds it) | red outline |
| NOT ON DRIVES | a starter whose image is gone | red outline |
| NEEDS NEWER FW | the game needs a newer FW than this console (backport = later) | red outline |
| CAN'T STREAM / FAILED | the build refused or failed; the reason is in the detail panel | red outline |

**Detail panel (right side).**
- Title, badge, Title ID, platform and version.
- **On the drives:** the image's file name.
- **File type:** `.ffpfsc` (compressed) or `.exfat`/`.pkg`/folder (not compressed), plus the size.
- **Needs FW** against this console's FW.
- **On this console:** streaming starter, full copy, image, or not installed, with its path and a "pinned" note.
- **Streams over:** LAN or Tailnet.
- **Played:** the newest proof line: console, date and evidence.
- **Last build or Why:** the payload's own reason line.
- **Your choice:** wanted or hidden.

**Buttons:**
- `CROSS`: Install when the game is not on the console; Remove (with a confirm) when ours is.
- `TRIANGLE`: Rebuild (with a confirm).
- `SQUARE`: Hide, or Show again.
- `CIRCLE`: quits to the home screen.

### 3.2 Activity

- **NOW:** building <game> with the time since it started, or "Idle: every game is up to date". Under it: the payload
  version and fork, the drives' address and whether they answer, ShadowMount+ against the proven build (red when it is not),
  where starters go, and how new games are handled. `CROSS` checks the drives now.
- **WHAT YOU ASKED:** the last eight results of your presses, in the payload's words.
- **LOG:** the tail of `/data/blazing/blazingmountd.log`. FAILED, WARNING and refused lines show in red; ↑ and ↓ scroll.

### 3.3 Settings

- **New games on the drives:** "Build them all" (today's behaviour) or "Only the ones I pick".
- **Check the drives now.**
- Every `mount.conf` key with a sentence of help: host, port, homebrew_dir, max_starter_mb, big_starter_mb, big_free_mb,
  min_free_mb, disk_cache_mb, cache_mb_ram, conns, readahead_kb, tsnet, auto_update, poll_secs, smp_version.
- Numbers: `←`/`→` change the value, `CROSS` saves, `CIRCLE` undoes. On/off settings flip with `CROSS`. Text settings are
  shown and edited in `mount.conf` until v0.2 adds the on-screen keyboard.

The payload rewrites only that one line of `mount.conf` and keeps every other line and comment.

### 3.4 When the payload is not running

A full-screen note: "BlazingMount is not running", what to do (load blazingmountd in the Payload Manager), and a retry every
2 seconds. The app never starts a payload itself (Markus's rules: payloads only, his autoload).

**Pictures:** run `app/host/preview.sh`. It draws every screen from real API answers in `app/fixtures/` and checks the rules
on each one: text ≥ 20 px, nothing past x = 1830, no unknown button tokens, and within the PS5 pass's vertex budget.

## 4. Choose what you want

"What I want and what I don't" is a small file the app writes through the payload, `/data/blazing/choices.tsv`:

```
auto	1            1 = build every game on the drives (default, today's behaviour); 0 = only the games marked want
want	PPSA02849    built first, and built even with auto 0
skip	PPSA03671    never built (Hide / Remove)
```

The payload folds it in at the start of every pass:
- `skip` lines join `mount.conf`'s skip list.
- `want` lines join `first`.
- With auto 0, `want` becomes the `only` list.

`mount.conf` itself stays his hand-edited file.

**Remove** deletes the starter folder and its link cfg, then marks the game skip so the next pass does not rebuild it. Its
safeguards:
- It never touches a full copy or an image file.
- It refuses while the game runs (`/mnt/sandbox/<TID>_000`).

**Rebuild** deletes the starter and builds it again on the same pass. The v2.5 auto-update already rebuilds starters whose
image changed, and the app shows those as OLD IMAGE until it does.

## 5. Changes on the drives show up by themselves

- The API re-reads the host's `/games.txt` every 30 s. When it changes (a game added, rebuilt or removed), the API logs
  "the drives' game list changed", bumps `gen` and wakes the payload for a pass at once instead of after `poll_secs`.
- The app polls `/api/status` once a second. When `gen` moves, it reloads `/api/library`. So a new game appears on screen
  within about 30 s, and with auto 1 it is built with no press. `host/test_api.py` proves both.
- Image changes are handled by v2.5's auto-update (the payload agent's work). The app shows the state as it happens:
  OLD IMAGE → BUILDING → STREAMING.

## 6. The local API (bm_api.c, blazingmountd :8094)

HTTP/1.0, one request per connection, JSON out, `Access-Control-Allow-Origin: *`.

| Route | Answer |
|---|---|
| `GET /api/status` | `version, now, gen, busy{tid,title,since}, last_pass, queued, recent[8], host, port, route LAN/TAILNET, tsnet, smp, smp_expected, homebrew_dir, free_mb{data,ext0,ext1,usb0}, min_free_mb, fork` |
| `GET /api/library` | `gen, host, host_ok, auto, console_fw, games[]`; each game has `tid, title, platform, key, version, fw, size, format, compressed, on_drives, file, local (starter/full/image/none), local_path, state, choice, build, tries, reason, proof, pinned` |
| `GET /api/settings` | `auto, keys[]{key, type n/b/s, value, help}` |
| `GET /api/log?lines=N` | `lines[]`: the log's tail (N ≤ 2000) |
| `GET /api/icon/<TID>` | icon0.png: the local starter's, else a cached copy, else fetched once from the host (`/t/<key>/sce_sys/icon0.png`) into `/data/blazing/icons/` |
| `POST /api/action?do=…&tid=…` | `install`, `remove`, `rebuild`, `hide`, `unhide`, `sync`, `auto&value=0/1`, `set&key=K&value=V`. **Needs the header `X-BM-Request: 1`**: a web page on another site cannot send it, so there are no cross-site writes. Unknown actions, keys, title ids and newline values are refused. |
| `GET /` | a small status page for a phone or the PS5 browser |

**Threads.**
- The API thread only reads: state, choices, mount.conf, the homebrew folders, the log, and the host's lists through its own
  HTTP client (the daemon's `http_get` has a static buffer).
- Every change is queued. The main loop applies the queue in `bm_api_pass_begin()` as the only writer, so a request can never
  race a starter build.

**The wiring** is five hook lines in `blazingmountd.c`, scripted in `host/api_hooks.py`:
- `#include "bm_api.c"`
- `bm_api_pass_begin(&c)` after `read_conf`
- `bm_api_busy()` around both `build_game` calls
- `bm_api_start()`, plus `bm_api_wait(secs)` in place of `sleep`

**Test:** `python3 -I host/test_api.py` runs a fake imgserve and a fake console: 38 checks, ALL PASS (24 Oct).

## 7. What the host should add (the drives' side)

- **`/proven.tsv` on imgserve.** It would serve mac1's `~/.blazing/games-proven.tsv` read-only, so the PLAYED badge and the
  proof line show on the console. The API already asks for it; until it exists, "no proof yet" is shown, never "works".
- **`/library.json` on imgserve.** It would cover every game we own, not only the packed `.ffpfsc` images: folders, `.exfat`,
  PS4 pkgs, games **not packed yet** (as `games-truth.py` lists them), and **verified** yes/no from the compression
  verify. Then the Library can show "on the drives but not packed" and "packed but not verified".
- Cover art at 256 px for the grid (`/t/<key>/sce_sys/icon0.png` works today at 512 px, about 250 KB each).

## 8. Packaging: still payloads only

Markus's rule (23 Oct): nothing on the consoles but payloads, and every helper goes inside the BlazingMount payload. So the
app ships the way pldmgr and Orbit ship their tiles.

- **v0.2:** the app's title folder (eboot, about 26 MB, mostly RADV) is embedded in `blazingmountd.elf`, as the link fork is
  today. The payload writes it to `/data/homebrew/PPSA99280` only when that folder is missing or its version is older, and
  ShadowMount registers the tile. One payload in his Payload Manager; the tile comes and goes with it.
- **v0.1 (MVP test):** the title folder is copied once to DIGI5 under `ps5-claim`, with a TV notice. His autoload, pldmgr,
  etaHEN and kstuff are not touched. To remove it, delete `/data/homebrew/PPSA99280`.

## 9. Controller map

| Button | Library | Activity | Settings | Confirm |
|---|---|---|---|---|
| ↑ ↓ (stick too, repeat after 0.4 s) | move | scroll the log | move | |
| ← → | page up / down | | change a number | |
| CROSS | Install / Remove | check the drives now | toggle / save | yes |
| TRIANGLE | Rebuild | | | |
| SQUARE | Hide / Show | | | |
| CIRCLE | quit the app | | undo an edit | cancel |
| L1 / R1 | tabs | tabs | tabs | |
| L2 / R2 | filter | | | |
| OPTIONS | sort | | | |

## 10. What we should add (next versions, in order)

1. **Cover art.** A second texture in the UI pass: a 2048² RGBA atlas of 128 px covers decoded with stb_image, fed by
   `/api/icon`. Then a **cover grid** view (Itemzflow, PS5SX2) as a toggle next to the list.
2. **The tile inside the payload** (section 8).
3. **Build progress.** Files and bytes done for the game being built (the payload logs every 5000 files; expose counters
   in `busy`), plus a queue view with what is next.
4. **Storage view.** Each starter's size on the console, per drive, and the largest first: what to remove when space runs low.
5. **Per-game link settings:** conns, readahead and a pin against auto-update (`.blazingmount-pin` is already honoured by v2.5).
6. **On-screen keyboard** (SceImeDialog) for text settings such as host.
7. **Proof and verify from the host** (section 7): PLAYED, VERIFIED, and NOT PACKED YET as filters.
8. **Launch** from the app. Markus's rule: start games like a person, through the home screen tile. So the app's "Play"
   would only point to the tile; it never uses appmove.

## 11. Files

| Path | What |
|---|---|
| `console/blazingmountd/src/bm_api.c` | the API (section 6) |
| `host/api_hooks.py`, `host/test_api.py` | the wiring and its test |
| `app/src/bm_app.h` | the app: screens, controller, polling (pure C++) |
| `app/src/json.h` | the JSON reader |
| `app/src/ui/{draw,font,raster}.h` | MF's quad batch, SDF font and software raster (blazing-fighters f68b025) |
| `app/assets/ui/bm-font.sdf` | Barlow Condensed Bold, 336 glyphs (`app/tools/font`) |
| `app/host/preview.cpp`, `preview.sh` | the Mac pictures + rule checks |
| `app/fixtures/*.json` | real API answers (DIGI5's 40 folders against mac1's 44 images, 24 Oct) |
| `app/ps5/blazingmount.cpp` | the PS5 program: pad, worker thread, UI pass |
| `app/ps5/ui_vk.*`, `app/ps5/shaders/ui.*` | MF's UI pass (shader folder `blazingmount/`) |
| `app/ps5/build-on-dell-wsl.sh`, `dell-build.sh` | the build on the Dell (MF's toolchain, never rebuilt) |
