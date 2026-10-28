#!/bin/zsh
# deploy-payload.sh <PHAT5|DIGI5> <blazingmountd.elf> <who> - put a new blazingmountd on a console and make it LIVE now.
#   1. refuses unless <who> holds (or can take) the console's claim (ps5-claim)
#   2. payload-slot-swap.py: staged, read back by sha256, old ELF kept in /data/blz-stage/backup/, renamed into the slot
#   3. stops the running blazingmountd by its pid file (Payload Manager process_kill) and loads the slot ELF (loadpayload)
#   4. checks /api/status reports the new version
# A running game is never touched: blazingmountd itself waits for a game to close before it swaps that game's fork.
set -u
CON=$1 ELF=$2 WHO=$3
SLOT=/data/pldmgr/payloads/local/blazingmountd
ps5-claim "$CON" "$WHO" "install blazingmountd $(strings "$ELF" | grep -m1 '^2\.[0-9]*-') (payload only)" | grep -q '^CLAIMED\|^RENEWED' || { echo "REFUSED: $CON is held by another agent"; exit 1; }
IP=$(cd ~/NYCT/blazing-ps5/tools && python3 -c "import ps5where,sys; print(ps5where.arg(sys.argv[1]))" "$CON") || exit 1
python3 ~/NYCT/blazing-ps5/tools/payload-slot-swap.py "$CON" "blazingmountd-$(date +%H%M)" "$SLOT" --new "$ELF=blazingmountd.elf" | tail -2 || exit 1
PID=$(curl -s --max-time 10 "http://$IP:8080/fs/data/blazing/blazingmountd.pid" | tr -d '\n ')
[ -n "$PID" ] && curl -s --max-time 15 -X POST -H 'X-PLDMGR-Request: 1' "http://$IP:8084/process_kill?pid=$PID"; echo
for i in {1..10}; do curl -s --max-time 3 "http://$IP:8094/api/status" >/dev/null 2>&1 || break; sleep 1; done
curl -s --max-time 15 -X POST -H 'X-PLDMGR-Request: 1' "http://$IP:8084/loadpayload:$SLOT/blazingmountd.elf"; echo
WANT=$(strings "$ELF" | grep -m1 '^2\.[0-9]*-')
for i in {1..20}; do V=$(curl -s --max-time 4 "http://$IP:8094/api/status" | grep -o '"version":"[^"]*"' | cut -d'"' -f4); [ "$V" = "$WANT" ] && break; sleep 2; done
echo "$CON blazingmountd now: ${V:-not answering} (want $WANT)"
[ "$V" = "$WANT" ]
