#!/bin/bash
# catalog-keeper.sh - run by a user LaunchAgent at login and every 60 s. When the catalog server is not
# answering on 127.0.0.1:4460, it OPENS the Terminal window (it never runs the server itself: a launchd
# process cannot reach the LAN on macOS). Mirrors the proven imgserve-keeper pattern.
DIR="$(cd "$(dirname "$0")" && pwd)"
/usr/bin/curl -s -m 3 -o /dev/null http://127.0.0.1:4460/catalog.txt && exit 0
sleep 8
/usr/bin/curl -s -m 3 -o /dev/null http://127.0.0.1:4460/catalog.txt && exit 0
echo "$(date '+%F %T') keeper: :4460 did not answer - opening its window" >> "$DIR/serve.log"
/usr/bin/open "$DIR/serve-catalog.command"
