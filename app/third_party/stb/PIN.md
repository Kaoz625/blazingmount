# stb_truetype, pinned copy

Rasterises the game's font into a signed-distance-field atlas, ONCE, on the MacBook: `tools/ui/bake_font.cpp` makes
`assets/ui/mf-font.sdf`, which both consoles load (`src/ui/font.h`). No console build compiles this header; the host
tests do (`tests/ui_test.cpp` bakes the atlas again and requires the committed file byte for byte).

| | |
|---|---|
| taken from | https://github.com/nothings/stb, commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (15 Jul 2024, the newest commit that touches `stb_truetype.h`) |
| version | stb_truetype v1.26 |
| licence | public domain or MIT, your choice (`LICENSE`) |
| copied | 18 Oct 2026: `stb_truetype.h`, `LICENSE` |
| checked | git blob hashes equal upstream's tree at the commit: `stb_truetype.h` 90a5c2e2b3fe, `LICENSE` a77ae91f3ec0 |

stb_truetype trusts its input ("no range checking"): it only ever reads the pinned font below, blob-checked.
Do not edit files here. To move: take the files at a new commit, check the blob hashes, update this note, run
`tools/ui/bake_font.sh` and commit the new atlas with the tests green.

## stb_image_write (19 Oct 2026)

`stb_image_write.h` v1.16 from the same commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760`
(sha256 `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05`), unchanged. Only the model-pack converter
uses it (`tools/look/retarget/mfpack.cpp` writes the game models' textures as PNG); no game build compiles it.

## stb_image.h (BlazingMount app, 24 Oct 2026)

Same pinned commit as above (`6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760`, v2.29), fetched from
https://raw.githubusercontent.com/nothings/stb/<rev>/stb_image.h. Decodes the game icons (icon0.png) the payload serves at
/api/icon/<TID> into the app's cover atlas (`src/covers.h`), PNG only (STBI_ONLY_PNG), on the console and in the host preview.
