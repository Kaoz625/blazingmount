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
