#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build-fork.sh - build the libSceAmpr.sprx link fork from source.
#
# Clones drakmor/ampr_emu at the pinned commit, overlays console/link-fork/module/ onto it with
# ampr_link_apply.py, and builds the result with the PS5 payload SDK. The fork is a GPL-3 derivative
# of ampr_emu (see ../README.md and ../../../CREDITS.md).
#
#   AMPR_EMU_REPO=<drakmor/ampr_emu git url> PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk sh build-fork.sh
#
# Output: ../build/libSceAmpr.sprx
#   To use the installers' default --fork, copy it to ../prebuilt/libSceAmpr.sprx
#   (the binary is .gitignored on purpose - BlazingMount ships source + this recipe, never a prebuilt).
set -e
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to your ps5-payload-sdk checkout}"
: "${AMPR_EMU_REPO:?set AMPR_EMU_REPO to drakmor/ampr_emu's git URL (the GPL-3 upstream this fork derives from)}"

PIN=cfa85df379f6eeeb165d7badf9b648e266fe77b7     # drakmor/ampr_emu master, 2026-09-06 (4 commits after tag 0.4.2.1)
HERE="$(cd "$(dirname "$0")" && pwd)"
FORK="$(cd "$HERE/.." && pwd)"                    # console/link-fork
WORK="${WORK:-${TMPDIR:-/tmp}/blazingmount-fork}"

rm -rf "$WORK"; mkdir -p "$WORK"
git clone -q "$AMPR_EMU_REPO" "$WORK/ampr_emu"
git -C "$WORK/ampr_emu" checkout -q "$PIN"
python3 -I "$HERE/ampr_link_apply.py" "$WORK/ampr_emu" "$FORK/module"

# AMPR_PROCESS_SYNC_READ_INTERCEPT=1 hooks pread/read; AMPR_PROCESS_AIO_INTERCEPT=1 also hooks the
# game's own async reads (needed by games that do their own AIO). This is the proven variant.
make -C "$WORK/ampr_emu" -j4 PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" PYTHON=python3 \
    AMPR_PROCESS_SYNC_READ_INTERCEPT=1 AMPR_PROCESS_AIO_INTERCEPT=1 "$@"

mkdir -p "$FORK/build"
cp "$WORK/ampr_emu/out/payload-sdk/libSceAmpr.sprx" "$FORK/build/"
echo "built: $FORK/build/libSceAmpr.sprx"
echo "to use the installers' default, run: cp \"$FORK/build/libSceAmpr.sprx\" \"$FORK/prebuilt/libSceAmpr.sprx\""
