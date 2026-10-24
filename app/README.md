# BlazingMount app (PPSA99280)

The native PS5 screens for the BlazingMount payload: the whole library, what is on this console, what is compressed, the file
type, the proof it played, install / remove / rebuild / hide per game, the payload's settings and its log. It talks only to
blazingmountd's local API (`console/blazingmountd/src/bm_api.c`, 127.0.0.1:8094). Design, decisions and the API:
`docs/UI-DESIGN.md`.

    app/host/preview.sh            every screen as a PNG on the Mac (software raster = the PS5 shader maths) + rule checks
    app/ps5/dell-build.sh          the PS5 title on the Dell WSL (Multiverse Fighters' toolchain) -> app/out/ps5/*.zip

`src/ui/{draw,font,raster}.h`, `ps5/ui_vk.*` and `ps5/shaders/ui.*` come from blazing-fighters (f68b025; shader folder
renamed). The font is Barlow Condensed Bold (OFL, `assets/ui/Barlow-OFL.txt`), rebaked with Latin-1 and Latin Extended-A by
`tools/font` (stb_truetype, public domain / MIT: `third_party/stb/LICENSE`).
