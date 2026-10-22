#!/bin/zsh
# BlazingMount host catalog server. Serves catalog.txt + starter bundles that blazingmountd on each
# PS5 pulls every 10 min. VISIBLE Terminal on purpose: a server started over ssh or from a launchd
# agent loses local-network access on macOS, so it must run in a real login (GUI) session. Closing
# this window stops it. Point your consoles' /data/blazing/mount.conf at "<this host's LAN ip> 4460".
cd "$(dirname "$0")"
echo "blazingmount-host: serving catalog + bundles on :4460 (read-only) from OUT/. Close this window to stop."
/usr/bin/python3 -I host/blazingmount-host.py serve OUT --port 4460 2>&1 | tee -a serve.log &
pid=$!
trap "kill $pid 2>/dev/null; pkill -f 'blazingmount-host.py serve'; exit 0" HUP INT TERM
wait $pid
