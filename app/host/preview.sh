#!/bin/sh
# preview.sh - build app/host/preview.cpp and write every screen as PNG (default out: app/out/preview, gitignored).
#   app/host/preview.sh [outdir] [shot...]
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/out/preview}"; [ $# -gt 0 ] && shift
mkdir -p "$OUT"
BIN="${TMPDIR:-/tmp}/bm-app-preview"
${CXX:-clang++} -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -o "$BIN" "$HERE/host/preview.cpp"
"$BIN" "$HERE/assets/ui/bm-font.sdf" "$HERE/fixtures" "$OUT" "$@"
for p in "$OUT"/*.ppm; do sips -s format png "$p" --out "${p%.ppm}.png" >/dev/null && rm "$p"; done
echo "pictures: $OUT"
