# prebuilt/ — where your built fork goes (not shipped)

BlazingMount does **not** ship a prebuilt `libSceAmpr.sprx`. It ships the source
(`../module/`) and the build recipe (`../tools/build-fork.sh`); you build the fork yourself.
This keeps the project a clean GPL-3 **source** distribution with no binary-redistribution
question.

Build it, then drop the result here:

```sh
AMPR_EMU_REPO=<drakmor/ampr_emu git url> PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk \
    sh ../tools/build-fork.sh
cp ../build/libSceAmpr.sprx ./libSceAmpr.sprx
```

`libSceAmpr.sprx` is `.gitignored`, so a built binary placed here is never committed. The
installers (`install/blazingmount-install.py`, `host/blazingmount-host.py`) use
`prebuilt/libSceAmpr.sprx` as the default `--fork`, so once you build + copy it here they work
with no extra flags.
