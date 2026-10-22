#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build.sh - build blazingmountd.elf with the PS5 payload SDK.
#
# REFERENCE build. blazingmountd is a plain POSIX C payload; it links against the PS5
# payload SDK the same way the link fork does (see ../link-fork/tools/build-fork.sh for the
# toolchain this project uses: llvm@18 / prospero-clang + ps5-payload-sdk).
#
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk sh build.sh
#
# Output: out/blazingmountd.elf
#
# Install (reboot-surviving): copy blazingmountd.elf into the console payload manager's
# payload dir and add ONE line to the jailbreak autoload list so it runs each boot. See
# ../../install/blazingmount-autoload.md - and read docs/REPORT-A-FIX.md first: a payload in a
# boot list must be safe to run unattended with no arguments (blazingmountd is: it loops and
# only writes under /data/homebrew, /data/blazing/link and /data/blz-stage).
set -e
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to your ps5-payload-sdk checkout}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/out"

CC="${CC:-${PS5_PAYLOAD_SDK}/bin/prospero-clang}"
"$CC" -O2 -Wall -Wextra -std=c11 \
    -o "$HERE/out/blazingmountd.elf" \
    "$HERE/src/blazingmountd.c"

echo "built: $HERE/out/blazingmountd.elf"
echo "NOTE: on-console test still pending - verify sockets + file I/O against your SDK version,"
echo "      then install per ../../install/blazingmount-autoload.md."
