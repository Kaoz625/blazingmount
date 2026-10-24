#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build-on-dell-wsl.sh - the BlazingMount app (PPSA99280) as a PS5 title, built on the Dell's WSL "Blazing" with the toolchain
# Multiverse Fighters already keeps in /root/mf-ps5-build (PS5_VulkanTemplate 91179df + PS5_Vulkan f410e67 + RADV + SDK).
# It never builds that toolchain: when it is missing, run blazing-fighters src/platform/ps5/dell-build.sh toolchain first.
#
#   bash build-on-dell-wsl.sh            (dell-build.sh copies our files into $B/bm-src first and runs this)
#
# Steps: 1. the environment MF's build sets (ONE LLVM 18, meson 1.6.1, ninja -j12)  2. the title (new-title.py, once)
#        3. our program + the app core + the UI pass + font into it, prelude.h includes, ui_vk.cpp in CMake, shaders
#        4. RADV's hidden-weak archive (MF's, reused)  5. ps5/tools/build.sh  6. checks, sha256s, a reproducible zip
set -euo pipefail
B=/root/mf-ps5-build
S=$B/bm-src                     # our files, as dell-build.sh copies them (app/ of the blazingmount repo)
TITLE_ID=PPSA99280
TITLE_NAME="BlazingMount"
PID=blazingmount
VERSION=${BM_APP_VERSION:-0.1.0}
T=$B/PS5_BlazingMount
log() { echo "[$(date +%H:%M:%S)] $*"; }
fail() { log "FAIL: $*"; exit 1; }
export GIT_AUTHOR_NAME="Mark Anthony" GIT_AUTHOR_EMAIL="markususeche@gmail.com"
export GIT_COMMITTER_NAME="Mark Anthony" GIT_COMMITTER_EMAIL="markususeche@gmail.com"

# ---------------------------------------------------------------- 1. the environment (= MF's build-on-dell-wsl.sh step 1)
[[ -x $B/tools/bin/meson && -x $B/PS5_Vulkan/build/host/ps5-native-tool && -f $B/PS5_Vulkan/runtime/libc.prx ]] ||
    fail "the PS5 toolchain is not in $B: run blazing-fighters src/platform/ps5/dell-build.sh toolchain first"
printf '#!/bin/sh\nexec /usr/bin/ninja -j %s "$@"\n' "${JOBS:-12}" > "$B/tools/bin/ninja"; chmod +x "$B/tools/bin/ninja"
export LLVM_CONFIG=/usr/bin/llvm-config-18
export PATH="$B/tools/bin:$PATH"
export PS5_CLANG=/usr/bin/clang-18
log "start: BlazingMount app $VERSION ($TITLE_ID), toolchain $B"

# ---------------------------------------------------------------- 2. the title
cd "$B"
if [[ ! -f $T/ps5/sce_sys/param.json ]]; then
    python3 PS5_VulkanTemplate/ps5/tools/new-title.py "$T" --title-id "$TITLE_ID" --name "$TITLE_NAME" | tee "$B/logs/bm-new-title.log"
fi
[[ -f $T/examples/$PID/$PID.cpp ]] || fail "new-title.py made no examples/$PID/$PID.cpp"

# ---------------------------------------------------------------- 3. our files into the title
put() { cmp -s "$1" "$2" || cp "$1" "$2"; }
mirror() {
    local src=$1 dst=$2 f
    mkdir -p "$dst"
    (cd "$src" && find . -type f) | while read -r f; do mkdir -p "$(dirname "$dst/$f")"; put "$src/$f" "$dst/$f"; done
    (cd "$dst" && find . -type f) | while read -r f; do [[ -f $src/$f ]] || rm -f "$dst/$f"; done
}
for f in ps5/blazingmount.cpp ps5/ui_vk.h ps5/ui_vk.cpp ps5/shaders/ui.vert ps5/shaders/ui.frag src/bm_app.h src/json.h src/ui/draw.h \
         src/ui/font.h assets/ui/bm-font.sdf assets/ui/Barlow-OFL.txt; do
    [[ -f $S/$f ]] || fail "missing $S/$f (dell-build.sh copies app/ there)"
done
put "$S/ps5/blazingmount.cpp" "$T/examples/$PID/$PID.cpp"
put "$S/ps5/ui_vk.h" "$T/examples/$PID/ui_vk.h"
put "$S/ps5/ui_vk.cpp" "$T/examples/$PID/ui_vk.cpp"
# the program's and ui_vk's "../src/..." resolve from examples/<pid>/ to examples/src/
mirror "$S/src" "$T/examples/src"
# the template compiles the program inside a namespace (ps5/src/wrap.cpp.in): every header it needs is included first, at file scope
if ! grep -q 'BlazingMount app' "$T/ps5/src/prelude.h"; then
    cat >> "$T/ps5/src/prelude.h" <<'EOF'

// BlazingMount app: the system headers, the app core and the UI pass at file scope, before the program is included
// inside its namespace (the program's own #includes of them are then no-ops)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "platform.h"
#include "../../examples/src/bm_app.h"
#include "../../examples/blazingmount/ui_vk.h"
EOF
fi
if ! grep -q 'examples/blazingmount/ui_vk.cpp' "$T/ps5/CMakeLists.txt"; then
    python3 - "$T/ps5/CMakeLists.txt" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
anchor = "# The UI module, when the title has it"
assert text.count(anchor) == 1, "ps5/CMakeLists.txt changed: no single UI-module anchor"
block = '''# BlazingMount app: the UI pass (= Multiverse Fighters' ui_vk.cpp, its shader folder blazingmount/)
target_sources(samples PRIVATE ${ROOT}/examples/blazingmount/ui_vk.cpp)

'''
open(path, "w").write(text.replace(anchor, block + anchor))
PY
fi
rm -rf "$T/shaders/glsl/$PID"; mkdir -p "$T/shaders/glsl/$PID"
cp "$S/ps5/shaders/ui.vert" "$S/ps5/shaders/ui.frag" "$T/shaders/glsl/$PID/"
(cd "$T" && bash ps5/tools/compile-shaders.sh "$PID" > "$B/logs/bm-shaders.log") || { cat "$B/logs/bm-shaders.log"; fail "shaders"; }
mkdir -p "$T/ps5/assets/ui"
put "$S/assets/ui/bm-font.sdf" "$T/ps5/assets/ui/bm-font.sdf"
put "$S/assets/ui/Barlow-OFL.txt" "$T/ps5/assets/ui/Barlow-OFL.txt"
python3 - "$T/ps5/assets.json" <<'PY'
import json, sys
path = sys.argv[1]
m = json.load(open(path))
m["assets"] = [{
    "path": "ui/bm-font.sdf", "from": "file", "source": "ps5/assets/ui/bm-font.sdf",
    "title": "Barlow Condensed Bold, as a signed-distance-field atlas (ASCII, Latin-1, Latin Extended-A, marks)",
    "author": "The Barlow Project Authors (https://github.com/jpt/barlow)", "licence": "OFL-1.1",
    "url": "https://github.com/google/fonts/tree/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/barlowcondensed",
    "changes": "baked into a distance-field atlas with our button glyphs (blazingmount app/tools/font)",
    "licence_file": "assets/ui/Barlow-OFL.txt", "used_by": ["*"]}]
m["comment"] = "BlazingMount app: the UI font. Add each new asset here, with its licence."
open(path, "w").write(json.dumps(m, indent=2) + "\n")
PY
(cd "$T" && python3 ps5/tools/build-assets.py --notices > /dev/null)
if [[ -n $(git -C "$T" status --porcelain) ]]; then git -C "$T" add -A; git -C "$T" commit -q -m "BlazingMount app $VERSION"; fi
log "program in: $(wc -l < "$T/examples/$PID/$PID.cpp") lines + bm_app.h $(wc -l < "$T/examples/src/bm_app.h") lines, title commit $(git -C "$T" rev-parse --short HEAD)"

# ---------------------------------------------------------------- 4. RADV's hidden-weak archive (MF's build made it; reused as is)
hidden="$B/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.hidden-weak.a"
[[ -f $hidden ]] || fail "no $hidden: run blazing-fighters src/platform/ps5/dell-build.sh once (it makes it)"
export RADV_ARCHIVE="$hidden"

# ---------------------------------------------------------------- 5. the build
cd "$T"
log "build: ps5/tools/build.sh"
if ! PS5_VULKAN_DIR="$B/PS5_Vulkan" bash ps5/tools/build.sh > "$B/logs/bm-title-build.log" 2>&1; then
    grep -E "error|Error|FAILED" "$B/logs/bm-title-build.log" | head -40
    tail -30 "$B/logs/bm-title-build.log"
    fail "ps5/tools/build.sh"
fi
grep -E "^==>|warning:" "$B/logs/bm-title-build.log" | head -20 || true

# ---------------------------------------------------------------- 6. checks, sha256s, the zip
D="$T/dist/$TITLE_ID"
[[ -f $D/eboot.bin && -f $D/sce_sys/param.json && -f $D/sce_module/libc.prx && -f $D/assets/ui/bm-font.sdf ]] || fail "dist/$TITLE_ID is incomplete"
for s in "BlazingMount app %s: the library" "ui: font atlas" "action %s -> http %d"; do
    grep -q -a -F "$s" "$T/build/ps5/eboot.elf" || fail "eboot.elf lacks the string \"$s\""
done
cd "$T/dist"
zip_name="BlazingMount-$TITLE_ID-v$VERSION.zip"
python3 - "$TITLE_ID" "$zip_name" "$(git -C "$T" log -1 --format=%ct)" <<'PY'
import os, stat, sys, time, zipfile
tid, out, epoch = sys.argv[1], sys.argv[2], int(sys.argv[3])
when = time.gmtime(epoch)[:6]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, dirnames, filenames in os.walk(tid):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            info = zipfile.ZipInfo(p, date_time=when)
            info.external_attr = (stat.S_IFREG | 0o755) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as data:
                z.writestr(info, data.read(), compresslevel=9)
print(out, os.path.getsize(out))
PY
(cd "$D" && find . -type f -print0 | sort -z | xargs -0 sha256sum > "$T/dist/$TITLE_ID.sha256")
echo "== title folder: $(find "$D" -type f | wc -l) files, $(du -sb "$D" | cut -f1) bytes"
sha256sum "$D/eboot.bin" "$T/dist/$zip_name"
log "DONE"
