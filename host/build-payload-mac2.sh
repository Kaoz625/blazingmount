#!/bin/zsh
# build-payload-mac2.sh <fork libSceAmpr.sprx> [out.elf] - build the console blazingmountd on mac2 (its ps5-payload-sdk + llvm@18 work;
# the MacBook's SDK copy lacks system headers) from THIS checkout's committed HEAD + src, with the given fork embedded.
# Prints the version and sha; copies the ELF to out.elf (default ~/.blazing/blazingmountd-<version>.elf).
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FORK=$1
R="NYCT/bm-build-$(date +%Y%m%d-%H%M%S)"
ssh mac2 "mkdir -p $R"
tar -C "$HERE" -cf - .git console/blazingmountd/src console/blazingmountd/build.sh | ssh mac2 "tar -C $R -xf -"
scp -q "$FORK" "mac2:$R/fork.sprx"
ssh mac2 "cd $R/console/blazingmountd && export PATH=/usr/local/opt/llvm@18/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin LLVM_CONFIG=/usr/local/opt/llvm@18/bin/llvm-config && mkdir -p out && FORK=\$HOME/$R/fork.sprx PS5_PAYLOAD_SDK=\$HOME/NYCT/ps5-linux/sdk/ps5-payload-sdk sh build.sh 2>&1 | tail -1"
V=$(ssh mac2 "strings $R/console/blazingmountd/out/blazingmountd.elf | grep -m1 '^2\.[0-9]*-'")
OUT=${2:-$HOME/.blazing/blazingmountd-$V.elf}
scp -q "mac2:$R/console/blazingmountd/out/blazingmountd.elf" "$OUT"
echo "$OUT  version $V  sha256 $(shasum -a 256 "$OUT" | cut -c1-16)"
