#!/bin/sh
# Build imgserve (framework-dependent, .NET 10 + ASP.NET Core runtime, both on mac1). Needs the vendored ps-neighborhood PS5PKGTool.Ffpfsc + Ufs2 sources
# (GPL-3.0, not in this repo; mac1 ~/archive-ssd-20261012/.blazing/pkg-to-image/src/converter/vendor, mac2 ~/.blazing/pkg-to-image/...).
#   FFPFSC_SRC=/path/to/vendor sh tools/imgserve/build.sh   -> $OUT (default ~/.blazing/imgserve/bin)
set -e
SRC=${FFPFSC_SRC:-$HOME/archive-ssd-20261012/.blazing/pkg-to-image/src/converter/vendor}
OUT=${OUT:-$HOME/.blazing/imgserve/bin}
cd "$(dirname "$0")"
export PATH="$PATH:/usr/local/share/dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet publish imgserve.csproj -c Release -p:FfpfscProj="$SRC/PS5PKGTool.Ffpfsc/PS5PKGTool.Ffpfsc.csproj" -o "$OUT"
echo "built: $OUT   run: dotnet $OUT/imgserve.dll <folder with .ffpfsc images> [--port 4451] [--bind 0.0.0.0]"
