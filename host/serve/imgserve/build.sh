#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build imgserve (framework-dependent, .NET 10 + ASP.NET Core runtime).
#
# imgserve decodes .ffpfsc PFSC images + their inner exFAT/UFS2 on demand using ps-neighborhood's
# PS5PKGTool.Ffpfsc + Ufs2 (GPL-3.0). That decoder is an EXTERNAL build dependency and is NOT
# vendored here - obtain a checkout and point FFPFSC_SRC at it (see ../../../CREDITS.md).
#
# (If you only serve game FOLDERS you do not need imgserve at all - use dirserve.py, which is
#  self-contained and has no external dependency.)
#
#   FFPFSC_SRC=/path/to/ps-neighborhood/vendor  OUT=./out  sh build.sh
set -e
: "${FFPFSC_SRC:?set FFPFSC_SRC to a checkout of ps-neighborhood's PS5PKGTool.Ffpfsc (GPL-3; see CREDITS.md)}"
OUT=${OUT:-./out}
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet publish imgserve.csproj -c Release \
    -p:FfpfscProj="$FFPFSC_SRC/PS5PKGTool.Ffpfsc/PS5PKGTool.Ffpfsc.csproj" -o "$OUT"
echo "built: $OUT   run: dotnet $OUT/imgserve.dll <folder with .ffpfsc images> [--port 4451] [--bind 0.0.0.0]"
