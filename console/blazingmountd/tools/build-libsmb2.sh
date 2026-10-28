#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build-libsmb2.sh [ps5|host] - libsmb2 (SMB2/3 client, LGPL-2.1-or-later) as a static archive for blazingmountd's SMB network sources.
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk sh tools/build-libsmb2.sh ps5   -> out/libsmb2-ps5/{lib/libsmb2.a,include/smb2}
#   sh tools/build-libsmb2.sh host                                           -> out/libsmb2-host/...   (the fake-console tests)
# Pinned to the master commit itsPLK/ps5-pkg-manager runs on the PS5, by the archive's sha256; no Kerberos, no GSSAPI, no libdcerpc.
# The PS5 libc's getaddrinfo resolves nothing for a payload-linked library, so libsmb2 is built asking bm_smb_getaddrinfo (bm_src.c:
# literal IPv4 only - the source's host is always an address). LIBSMB2_ARCHIVE=<file> uses a copy you already have (digest still checked).
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-ps5}"
REV=68f1136e7f6e7d136fd08893dc0028faeb2a120b
DIGEST=0006174d6045ff21a565879faaa24f1615f3189f8a5d1b3f03c900239b8ecc4a
OUT="$HERE/out/libsmb2-$MODE"
if [ -f "$OUT/.stamp" ] && [ "$(cat "$OUT/.stamp")" = "$REV $MODE" ]; then echo "$OUT"; exit 0; fi
ARC="${LIBSMB2_ARCHIVE:-$HERE/out/libsmb2-$REV.tar.gz}"
if [ ! -f "$ARC" ]; then curl -fsSL --retry 3 "https://github.com/sahlberg/libsmb2/archive/$REV.tar.gz" -o "$ARC"; fi
GOT="$(shasum -a 256 "$ARC" | cut -c1-64)"
[ "$GOT" = "$DIGEST" ] || { echo "libsmb2 archive digest mismatch ($GOT)" >&2; exit 1; }
B="$HERE/out/libsmb2-build-$MODE"; rm -rf "$B"; mkdir -p "$B/src"
tar -xzf "$ARC" -C "$B/src" --strip-components=1
FLAGS="-fPIC -ffunction-sections -fdata-sections"
TC=""
if [ "$MODE" = ps5 ]; then
  : "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK}"
  export PS5_PAYLOAD_SDK PS5_CLANG="${PS5_CLANG:-/usr/bin/clang}"
  TC="-DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake"
  FLAGS="$FLAGS -march=znver2 -Dgetaddrinfo=bm_smb_getaddrinfo -Dfreeaddrinfo=bm_smb_freeaddrinfo"
fi
cmake -S "$B/src" -B "$B/cmake" $TC -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$FLAGS" -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF \
  -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF -DENABLE_EXAMPLES=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log" >&2; exit 1; }
cmake --build "$B/cmake" --target smb2 -j 6 > "$B/build.log" 2>&1 || { grep -E "error" "$B/build.log" | head -20 >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/include"
cp "$(find "$B/cmake" -name libsmb2.a | head -1)" "$OUT/lib/"
cp -r "$B/src/include/smb2" "$OUT/include/"
echo "$REV $MODE" > "$OUT/.stamp"
echo "$OUT"
