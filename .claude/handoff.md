# BlazingMount handoff (BLZ-0141)

Working on: DIGI5 "can't see my games" — surface the games + a browsable games section + blazingmountd autoload.
Last action (digi5-games, 22 Oct 2026):
  - DIAGNOSED (read-only app.db): NOT a DB fault. 7 drive games + Blazing Games (PPSA99191) + BlazingArch
    (PPSA99171) are all registered visible=1, categoryType=0 (Games), psgm:play deeplink, per-user icon rows
    for all 3 users. Cause of "not showing" = stale SceShellUI cache; surfaces via the native Game Library
    (reads app.db live) and fully on a reboot/re-JB. See blazing-ps5 docs/GAMES-SECTION-DIGI5.md.
  - ACTIVATED blazingmountd autoload on DIGI5 (reversible, verified):
      * pldmgr local slot /data/pldmgr/payloads/local/blazingmountd/ (blazingmountd.elf 111504 + meta.json)
      * /data/blazing/mount.conf = "192.168.1.130 4460"
      * /data/pldmgr/autoload.txt 25 -> 26 lines, sha d2b00a20..., "local/blazingmountd" last (after ShadowMount)
      * backups: on-console *.bak-digi5games-20261022-1122 + local scratchpad copies
  - HOST catalog server up on mac1 (192.168.1.130:4460), LAN-reachable (200): visible-Terminal
    serve-catalog.command + catalog-keeper.sh + LaunchAgent com.nyctailblazers.blazingmount-host (loaded).
    Catalog is header-only (empty of game rows) for now — daemon skips installed games via exists().
Next step:
  1. Markus: reboot + re-jailbreak DIGI5 -> confirm the 7 games show on the Games row and in Game Library,
     and that `local/blazingmountd` loads (check /data/blazing/blazingmountd.log for "blazingmountd start").
  2. To auto-add NEW games to every console's section: on mac1 run
     `python3 ~/.blazing/blazingmount-host/host/blazingmount-host.py build OUT --host-ip 192.168.1.130 --console-fw 13.60 --game "<path>"`
  3. PHAT5 parity (FW 10.20): stage blazingmountd.elf + fork in /data/blazing first (DIGI5 had them; PHAT5 may
     not), then same slot + mount.conf + one autoload line AFTER ShadowMount. Coordinate with Tokon/smb-link lanes.
Key files: blazingmount install/host-autostart/*, install/blazingmount-autoload.md; blazing-ps5 docs/GAMES-SECTION-DIGI5.md
Blockers: on-TV visual proof needs Markus's reboot (RP menu capture is blocked); autoload exercises at his next boot.

---
## add-from-drives lane (22 Oct 2026 ~17:3x) - what changed in THIS repo (host/)
- `blazingmount-host.py build ... --merge`: keep the games already in catalog.json, upsert by title id; read-modify-write under a flock; every file renamed into place (a polling console never reads half a catalog/bundle). Without --merge a build still rewrites the catalog to only its own games. (cf6755d)
- `blazingmount-starter.py --check-only` (one JSON line, same verdict a build reaches, builds nothing) and `--max-starter-mb N` (refuse a "starter" that is most of the game: Ghost of Tsushima / SW Outlaws starters are ~70 GB); `build` passes the cap through. (a82c779)
- executable modules (.prx/.sprx/.elf/.self, any folder) stay REAL files in the starter: FC27 on DIGI5 died in rtld ("self_load_shared_object: size 0") when two were streamed as 0-byte placeholders (found by digi5-convert). Manifest + --check-only list them as `kept_local`. (605364e)
- tests: host/test_catalog_merge.py (8), host/test_starter_checks.py (10); `python3 -I host/test_*.py`.
- KNOWN GAP in this public repo: host/engine lacks mkbackport.py (mkgame.elf_needs imports it) and decrypt_fself comes from outside, so the starter only runs on mac1 with `SMBLINK_TOOLS=~/.blazing/smblink/tools` (what blazing-ps5 tools/add-from-drives.py sets). Fixing that for other devs = vendor an ELF dynamic-section reader and review decrypt_fself's licence first.
- The house-only site button + worker that USE these live in blazing-games / blazing (add-on) / blazing-ps5 tools/add-from-drives.py; see roku channels .claude/handoff.md "add-from-drives" and memory add-from-my-drives-button.

---
## blazingmount-ui-agent (24 Oct 2026 ~10:20) - the native BlazingMount app
Working on: native PS5 screens for the payload (Markus 24 Oct) - app PPSA99280 + blazingmountd local API :8094.
Last action: app v0.1.1 built on the Dell (zip sha d59c0764, app/out/ps5/, gitignored), pushed 23c5ec7; design doc docs/UI-DESIGN.md (e82b2a9).
Done + tested: bm_api.c (host/test_api.py 38/38 ALL PASS), app screens (app/host/preview.sh ALL SHOTS OK, PNGs in app/out/preview),
  payload with the 5 hooks cross-compiles for PS5 (mac2 ~/NYCT/bm-ui-build, NOT installed).
NOT done: hooks are NOT in blazingmountd.c yet (posted to hub, applying at 11:00 unless payload-agent objects);
  nothing installed on any console; DIGI5 held by digi5-test-agent.
Next step (exact):
  1. python3 -I host/api_hooks.py console/blazingmountd/src/blazingmountd.c && python3 -I host/test_api.py && commit + push
  2. when DIGI5 is free: ps5-claim DIGI5 blazingmount-ui-agent "app MVP test"; ps5 notice DIGI5 "..."; swap the slot ELF
     (keep v2.5 .bak), stop old daemon by /data/blazing/blazingmountd.pid, load once via POST :8084/loadpayload:<slot elf>;
     curl http://192.168.1.39:8094/api/status; put-tree.py DIGI5 <unzipped>/PPSA99280 /data/homebrew/PPSA99280 --stage --tid PPSA99280;
     play.py up --console DIGI5 --backend chiaki; open the tile with Cross; shots; CIRCLE quits; play.py down; ps5-release DIGI5.
Key files: docs/UI-DESIGN.md, console/blazingmountd/src/bm_api.c, host/api_hooks.py, host/test_api.py, app/src/bm_app.h, app/ps5/*.
Blockers: DIGI5 claim (digi5-test-agent); hooks wait for 11:00 hub ack window.

### blazingmount-ui-agent update ~10:45 (24 Oct) - SCOPE: everything Markus owns, every system, with covers
- app 0.3.0 (31b5252): whole library grid/list, system chips L2/R2, show modes OPTIONS, TOUCH PAD grid/list, covers via sheets + focus full cover. Dell zip 0f8515ad.
- host/library/library.py (+ test_library.py ALL PASS): mac1 ~/.blazing/library/library.json = 18,809 games / 56 systems.
  COVER RUN in a visible mac1 Terminal: ~/.blazing/library/covers-run.command (resumable; log run.log). When it says DONE:
  `open -a Terminal ~/.blazing/library/watch.command` (follows drives + owned list every 5 min).
- imgserve library routes: blazing-fighters 8ffc032, built mac1 ~/.blazing/imgserve/bin.next, NOT deployed (needs a quiet :4451 restart; asked on hub).
- bm_api passes catalog/covers through (12a4091). Hooks still NOT in blazingmountd.c (11:00 plan). PS5 payload with hooks built mac2 ~/NYCT/bm-ui-build (not installed).

---
## blazing-live-speed lane (24 Oct 2026, 21:0x) - full log: ~/.claude-team/handoffs/live-speed.md
Working on: PHAT5 away (Verizon hotspot) streaming speed - live monitor + measured tuning.
Last action: measured 8 vs 16 link connections and 1 MiB requests (n=10): no significant gain -> no link fork change; the hotspot pipe (~1-2.3 MB/s, lossy) is the limit.
Next step: when Markus next streams a game away, `tail -f ~/.claude-team/ai-notes/away-speed.tsv` (new_conns should be ~0), then pull /data/blazing/link/<TID>.log from PHAT5 and compare "remote http ... avg us" with 1.2-2.3 s per request before the 20:35 forward fix.
Key files: ~/.blazing/live-speed/ (monitor.py, bench_*.py, backup/), PHAT5 /data/blazing/mount.conf (+ proxy 127.0.0.1 14451), /data/tailscale/config.json (forward 14451), mac1 /Library/LaunchDaemons/com.nyctailblazers.tcp-tuning.plist
Blockers: DIGI5 wedged (claude-nyc-info) - needs the same forward + mount.conf line later; upstream ps5-tailscale MaxIdleConnsPerHost issue and the MacBook's 2.5 MB/s background download both need Markus's OK.

### dl-local lane (download to console), 25 Oct 2026 ~11:45
Working on: games on PHAT5's own M.2 (play without the network) + the app's Download to console / Remove local copy.
Last action: pushed remove-local + prove + app 0.5.1 (cfb2f48); built payload 2.8-38fe482.fdefd666f (mac2) + app 0.5.1 zip (Dell), NOT installed. Requiem PPSA31246 switched to local 11:37; passive prove running.
Next step: when mac1 load < 10 run the independent review (handoff section 4), then install on both consoles when idle (section 3).
Key files: host/download-to-console.py, host/test_download_to_console.py, docs/DOWNLOAD-TO-CONSOLE.md, app/src/bm_app.h, console/blazingmountd/src/bm_api.c
Blockers: review helper refused by the mac1 load cap; consoles busy. Full state: ~/.claude-team/handoffs/download-local.md

---
## bm-installer lane (claude-nyc-main, 27 Oct 2026 ~16:40-17:58) - BlazingMount installs PKGs itself (step 1 + 2 DONE)
Working on: the app's INSTALL tab really installing + install from a PC with live %/ETA/cancel.
Last action: payload 2.10-ccb1e37 + app 0.7.0 (eboot 4c8da613, zip sha 3b5058fc) on BOTH consoles; PHAT5 real install PPSA99051 done at ~30 MB/s.
What exists now (all pushed, main):
  - console/blazingmountd/src/bm_install.c: /api/install (path=<imgserve /pkgs.json path> | url=http://ip:port/x.pkg, name=), /api/install/cancel,
    /api/install/pkgs. Sony's installer (sceAppInstUtilInstallByPackage) reads http://127.0.0.1:8095/i/<job>-<unixtime>.pkg = a proxy in the payload
    that Range-reads the host. Progress = GetInstallStatus capped by bytes served; rate/ETA; cancel = proxy stops; stall 300 s = failed.
  - every install runs in a FRESH helper process (bm_inst_helper.c, embedded, launched via elfldr 127.0.0.1:9021, IPC 127.0.0.1:8096, bm_ai.h).
    Helper log: /data/blazing/inst-helper.log. Fallbacks: no helper -> in-payload call (1 per payload start) -> PKG Manager (drive pkgs only).
  - host/install-pkg.py <console ip> <file.pkg|file.pkg.0> : this computer serves the pkg, live %/MB/s/ETA, Ctrl-C cancels. PC must be on the console LAN.
  - app 0.7.0: 8TB entries go to /api/install; USB entries stay PKG Manager; store pkgs -> own installer.
ROOT CAUSES found: (1) one AppInstUtil install per PROCESS (2nd = 0x80B2116F) -> helper per install; (2) 0x80B224xx = HTTP status the PS5 got from
  the URL (0x80B22404 = 404: proxy must ignore Sony's ?product=0284&serverIpAddr=... query); (3) PKG Manager itself DID process jobs on re-test
  (DIGI5 MOUS21425 base done in 10 s); its PHAT5 DLC (PPSA28997 Deluxe upgrade) streamed 100% then "timed out during finalization" (Sony never
  reported playable). The original "queued but never runs" case was NOT reproduced: PKG Manager's queue only advances while its installer is idle,
  so one stuck finalisation (up to 5 min) holds every later job.
Next step: drive the app's INSTALL tab on the TV once (press CROSS on an 8TB pkg, watch %/ETA, SQUARE cancel) - only with Markus/RP free.
Key files: console/blazingmountd/src/{bm_install.c,bm_inst_helper.c,bm_ai.h,bm_store.c}, host/install-pkg.py, host/test_install.py (40 checks),
  app/src/bm_app.h; build: mac2 ~/NYCT/bm-ccb1e37 (FORK = bm-76ab657/fork.sprx defd666f); payload install script pattern in this lane's report.
Blockers / leftovers:
  - DIGI5: 2 hung helper processes (pids 165, 167, from a bad intermediate build) ignore pldmgr process_kill; gone at the next reboot.
  - DIGI5 got test installs PS5Library PPSA99051 + PS5-SHOP MOUS21425 (73 MB, DIGI5 space rule says tests on PHAT5) - remove on Markus's word
    (SMP uninstall via /api/smp/uninstall refuses MOUS21425 as a "bad title id").
  - PHAT5: a failed PKG Manager DLC try (PPSA28997 Deluxe upgrade, 1 MB) may sit in the PS5 Downloads list.
  - 17:10 I restarted blazingmountd on PHAT5 while np-universal held the claim (my script ignored REFUSED); reported on hub + relay #20434.

## bm-netsrc lane (claude-nyc-main, 28 Oct 2026 ~00:30-01:35) - network sources (FTP/SMB/WebDAV) + console profiles + remote wake
Working on: Markus's last two PlayBridge-style features in BlazingMount (approved 28 Oct).
Last action: payload 2.12-520a827 + app 0.8.0 LIVE on PHAT5 and proven with real installs; DIGI5 NOT touched (claim held by another claude-nyc-main agent: MK1 relaunch test).
Commits (all pushed, main): ac7096d (payload 2.12: bm_src.c, bm_prof.c, host/bm-console.py, tests), 27a2ba8 (app 0.8.0), 520a827 (SMB parallel-session fix), + preview BM_LIVE.
What exists:
  - console/blazingmountd/src/bm_src.c: sources in /data/blazing/sources.conf (0600; logins never listed/logged), FTP (PASV/REST/RETR, MLSD else LIST),
    WebDAV (Range GET, PROPFIND, Basic, http only), SMB2/3 via libsmb2 (pinned 68f1136e, static, tools/build-libsmb2.sh; build.sh runs it; NO_SMB=1 skips).
    API: GET /api/src, POST /api/src/add|del, GET /api/src/ls|test, POST /api/install?src=&path= (bm_install.c reads the source itself; split X.pkg.N = one file),
    GET /src/<name>/<path> = Range gateway (a game image from a source; NOT wired to a game build yet).
  - bm_prof.c: GET /api/profile, POST /api/profile/set (name, install_target auto|internal|m2 -> homebrew_dir, default_source, user) in /data/blazing/profile.conf.
  - host/bm-console.py (symlink ~/.local/bin/bm-console on the MacBook): state, wake, profile, src list|add|ls|test|del, install. Wake = DDP WAKEUP (UDP 9302) with the
    Game Lab pairing credential, sent from where the profile lives (this computer or mac2 over ssh); refuses an awake or silent console; never sleeps one.
  - app 0.8.0: INSTALL tab walks network sources (L2/R2 with the store + drives), folders (CROSS in, CIRCLE up), CROSS on a pkg = confirm -> install with live %/MB/s/ETA/SQUARE cancel;
    Settings tab has 4 profile rows (name, user = shown; install target + default source = cycled). The profile's default source opens first.
  - Tests: host/test_netsrc.py (own FTP+WebDAV servers, rclone's if installed, REAL SMB with BM_TEST_SMB=<ip[:port]>:<share/dir> + BM_TEST_SMB_USER/PASS env; 6 parallel SMB sessions),
    host/test_wake.py (fake console), test_install/test_api/test_store/test_pm/test_pull still pass; app/host/preview.sh (+ BM_LIVE=<console ip> = screens from a real console's API).
ROOT CAUSES found: (1) libsmb2 on the PS5 has NO strong RNG (config.h) -> random() reseeded per context -> parallel sessions (Sony reads on several connections) got the
  same client GUID and the server dropped the older ones ("Timeout expired and no connection exists"); fixed: own GUID per session, init/destroy under a lock, one handshake at a time.
  (2) macOS smbd "guest" does not work with libsmb2 (0xC000006E/6D); the 8TB share needs the login (user blazing, password in mac1 ~/.credentials/game-shares-smb/mac1.pass).
  (3) mac2's smb-pkg :4446 share (impacket) lists fine but Tools/ links dangle (the Dell 5TB is not mounted) and its listing crashes on them - not our bug.
Console state: PHAT5 sources mac2-ftp (rclone :8021), mac2-dav (rclone :8077) are TEST sources (rclone on mac2 under ~/bm-netsrc-live, stopped at the end), mac1-8tb = smb login (kept).
  PHAT5 profile: name PHAT5, target m2 (= homebrew_dir it already had), user Dappa (a label I chose: change it), default source set to mac1-8tb at the end.
  PS5Library PPSA99051 was reinstalled 4x on PHAT5 (same pkg, same title); one cancelled mid-way (the PS5 Downloads list may show a failed task: delete it there).
Next step (exact): when DIGI5 is free (ps5-status DIGI5; ps5-claim DIGI5 bm-netsrc "..."):
  zsh host/deploy-payload.sh DIGI5 ~/.blazing/blazingmountd-2.12-520a827-fork15533a6e.elf bm-netsrc   (fork 15533a6e = DIGI5's current)
  then the app: python3 -I ~/NYCT/blazing-ps5/tools/put-tree.py DIGI5 ~/NYCT/bm-netsrc-work/app080/PPSA99280 /data/homebrew/PPSA99280 --stage --tid PPSA99280
  after moving the old folder to /data/blz-stage/backup/PPSA99280-app-0.7.0-<ts> over FTP (:1337 on DIGI5), then ps5-release.
  Rebuild if needed: zsh host/build-payload-mac2.sh <fork> (builds the COMMITTED HEAD on mac2); app: sh app/ps5/dell-build.sh (zip sha 8b45bf93).
Untested / not done: LIVE WAKE (both consoles were awake: the code path is proven against a fake console + the real credential lookup on mac2 by --dry-run, but no packet has
  gone to a resting console - first rest-mode moment: `bm-console wake PHAT5`); app screens not driven on the TV (preview + BM_LIVE shots only); "stream a game image from a source"
  = only the byte gateway; wake from Blazing Games (a web page cannot send UDP; needs a local helper); WebDAV over https, FTPS, SMB over the tailnet proxy (away from home);
  the 1500-entry listing cap is silent; /src/<name>/<path> is readable by anything on the LAN that reaches :8094 (like the rest of the API).
Key files: console/blazingmountd/src/{bm_src.c,bm_prof.c,bm_install.c}, console/blazingmountd/tools/build-libsmb2.sh, host/{bm-console.py,test_netsrc.py,test_wake.py},
  app/src/bm_app.h, app/fixtures/{src,src-ls,profile}.json.
Blockers: DIGI5 claim held by another agent (not stale: renewed).
NOTE: I accidentally printed two SMB passwords (mac1 game-shares-smb + macbook-smb) into my session output on 28 Oct (a bad sed mask); nothing was written to a file or the hub.
