#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# dell-build.sh - build the BlazingMount app (PPSA99280) on the Dell's WSL "Blazing" from this MacBook checkout, then fetch the
# title zip into app/out/ps5/ (gitignored) and check its sha256. Uses MF's toolchain in /root/mf-ps5-build (never rebuilds it).
#   app/ps5/dell-build.sh
# The Windows ssh layer mangles quotes: every file travels on stdin (tar + base64), never inside the command line.
set -eu
cd "$(dirname "$0")/.."
LOCK="${BM_DELL_LOCK:-$HOME/NYCT/blazing-fighters/tools/dell-lock.sh}"
"$LOCK" acquire ps5-build blazingmount-app || exit 1
trap '"$LOCK" release ps5-build' EXIT
B=/root/mf-ps5-build
VERSION=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' ps5/blazingmount.cpp)
ZIP=BlazingMount-PPSA99280-v$VERSION.zip
SSH="ssh -o BatchMode=yes -o ServerAliveInterval=30 dell"
quiet() { grep -v 'post-quantum\|store now\|pq.html' || true; }
wsl() { $SSH "wsl -d Blazing -- bash -lc \"$1\""; }
COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -cf - ps5/blazingmount.cpp ps5/ui_vk.h ps5/ui_vk.cpp ps5/shaders ps5/build-on-dell-wsl.sh \
    src assets | base64 | wsl "rm -rf $B/bm-src && mkdir -p $B/bm-src && base64 -d | tar -C $B/bm-src -xf -" 2>&1 | quiet
out=$(mktemp)
wsl "set -o pipefail; BM_APP_VERSION=$VERSION bash $B/bm-src/ps5/build-on-dell-wsl.sh 2>&1 | tee -a $B/logs/bm-run.log && echo BM_BUILD_OK || echo BM_BUILD_FAILED" 2>&1 | quiet | tee "$out"
ok=$(grep -c '^BM_BUILD_OK' "$out" || true)
rm -f "$out"
[ "$ok" = 1 ] || { echo "dell-build: the PS5 build failed on the Dell: nothing fetched" >&2; exit 1; }
mkdir -p out/ps5
wsl "base64 -w0 $B/PS5_BlazingMount/dist/$ZIP" 2>/dev/null | base64 -d > "out/ps5/$ZIP"
remote=$(wsl "sha256sum $B/PS5_BlazingMount/dist/$ZIP" 2>/dev/null | cut -d' ' -f1)
wsl "cat $B/PS5_BlazingMount/dist/PPSA99280.sha256" 2>/dev/null > out/ps5/PPSA99280.files.sha256
local=$(shasum -a 256 "out/ps5/$ZIP" | cut -d' ' -f1)
[ "$local" = "$remote" ] || { echo "zip sha256 differs: MacBook $local, Dell $remote" >&2; exit 1; }
ls -la "out/ps5/$ZIP"
echo "sha256 $local"
