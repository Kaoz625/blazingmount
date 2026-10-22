#!/bin/sh
# build-fork.sh - build OUR fork of the AMPR emulator (drakmor/ampr_emu cfa85df + tools/smblink/module) on mac2. OFF-CONSOLE: nothing here touches a console.
#   sh tools/smblink/tools/build-fork.sh [extra make args]
# Output: assets-local/smblink/fork/<stamp>/libSceAmpr.sprx (+ .prx, SHA256SUMS.txt, build.log, VARIANT.txt = the extra make args, passed to build AND verify). Hashes differ per build (__DATE__/__TIME__ in the source): compare sizes.
# The build uses AMPR_PROCESS_SYNC_READ_INTERCEPT=1 so the emulator also hooks pread/read (the default build only hooks open/stat/close/fstat/lseek).
set -e
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
REMOTE="NYCT/tokon-ampr-build"
ssh mac2 "mkdir -p $REMOTE/smblink-$STAMP/module $REMOTE/smblink-$STAMP/tools"
scp -q "$REPO"/tools/smblink/module/blz_http.* "$REPO"/tools/smblink/module/blz_link.h "$REPO"/tools/smblink/module/blz_link_core.cpp "$REPO"/tools/smblink/module/blz_link_game.cpp "$REPO"/tools/smblink/module/blz_link_hooks.h mac2:$REMOTE/smblink-$STAMP/module/
scp -q "$REPO"/tools/smblink/tools/ampr_link_apply.py mac2:$REMOTE/smblink-$STAMP/tools/
OUT="$REPO/assets-local/smblink/fork/$STAMP"
mkdir -p "$OUT"
echo "extra make args: ${*:-none} (AMPR_PROCESS_AIO_INTERCEPT=1 = the game's own AIO calls are hooked too, e.g. Samson)" > "$OUT/VARIANT.txt"
ssh mac2 "set -e
export PATH=/usr/local/opt/llvm@18/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin LLVM_CONFIG=/usr/local/opt/llvm@18/bin/llvm-config
cd \$HOME/$REMOTE
rm -rf smblink-fork-$STAMP && git clone -q ampr_emu smblink-fork-$STAMP && git -C smblink-fork-$STAMP checkout -q cfa85df379f6eeeb165d7badf9b648e266fe77b7
python3 -I smblink-$STAMP/tools/ampr_link_apply.py smblink-fork-$STAMP smblink-$STAMP/module
cd smblink-fork-$STAMP
nice -n 10 make -j6 PS5_PAYLOAD_SDK=\$HOME/NYCT/ps5-linux/sdk/ps5-payload-sdk PYTHON=python3 AMPR_PROCESS_SYNC_READ_INTERCEPT=1 $* 2>&1 | tail -40
ls -la out/payload-sdk/libSceAmpr.sprx out/payload-sdk/libSceAmpr.prx
nice -n 10 make verify PS5_PAYLOAD_SDK=\$HOME/NYCT/ps5-linux/sdk/ps5-payload-sdk PYTHON=python3 AMPR_PROCESS_SYNC_READ_INTERCEPT=1 $* 2>&1 | tail -8
shasum -a 256 out/payload-sdk/libSceAmpr.sprx out/payload-sdk/libSceAmpr.prx > SHA256SUMS.txt; cat SHA256SUMS.txt" 2>&1 | tee "$OUT/build.log"
scp -q mac2:$REMOTE/smblink-fork-$STAMP/out/payload-sdk/libSceAmpr.sprx mac2:$REMOTE/smblink-fork-$STAMP/out/payload-sdk/libSceAmpr.prx mac2:$REMOTE/smblink-fork-$STAMP/SHA256SUMS.txt "$OUT/"
echo "copied to $OUT"; ls -la "$OUT"
