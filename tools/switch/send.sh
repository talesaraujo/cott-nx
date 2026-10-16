#!/bin/bash
# Sends cott.nro to the Switch over the network and runs it, without taking the SD card out or rebooting.
# The game's log then shows up here, live, until the game closes (Ctrl+C stops listening).
#
#   SWITCH_IP=192.168.x.y tools/switch/send.sh [game arguments, e.g. --log_level=debug]
#
# On the Switch: open hbmenu by holding R on a game (full memory) and press Y (netloader).
# hbmenu writes the file to sdmc:/switch/cott-nx/cott.nro, over the previous one, so game_root/ next to
# it is found as usual. The PC must accept TCP 28771 from the Switch for the log (nxlink -s); the
# netloader's own transfer goes the other way, to the Switch's port 28280.
# Each run's log is also kept in logs/switch/nxlink_<date>.log.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SWITCH_IP=${SWITCH_IP:?set SWITCH_IP to the address hbmenu shows after pressing Y}
NRO=${NRO:-$ROOT/app/out/switch/cott.nro}
IMAGE=${IMAGE:-docker.io/devkitpro/devkita64:latest}
DEST=${DEST:-cott-nx/cott.nro}  # relative to sdmc:/switch (hbmenu's root)

# Wait for the netloader with nxlink's own UDP ping ("nxboot" -> "bootnx"). Never probe its TCP port:
# hbmenu accepts a single connection, and an empty one ends the netloader with "Error getting name length".
# The socket is bound to 28771, the port the firewall already lets in, so the reply arrives either way.
echo "waiting for the netloader on $SWITCH_IP (hbmenu, Y)..."
python3 -I - "$SWITCH_IP" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 28771))
s.settimeout(0.5)
while True:
    s.sendto(b"nxboot", (sys.argv[1], 28280))
    try:
        data, _ = s.recvfrom(256)
        if data.startswith(b"bootnx"):
            break
    except socket.timeout:
        pass
PY

mkdir -p "$ROOT/logs/switch"
LOG="$ROOT/logs/switch/nxlink_$(date +%Y%m%d_%H%M%S).log"
echo "sending $NRO -> sdmc:/switch/$DEST ($SWITCH_IP); log: $LOG"
# --network host: nxlink -s listens on 28771 for the game's connection back to the PC.
# stdbuf -oL: nxlink's stdout is a pipe here, so glibc would hold the game's log in 4 KB blocks and a
# hang would look like it happened earlier than it did (first Switch run: 1 min "stuck" mid-line).
podman run --rm -i --network host -v "$(dirname "$NRO"):/nro:ro,Z" "$IMAGE" \
  stdbuf -oL -eL /opt/devkitpro/tools/bin/nxlink -s -a "$SWITCH_IP" -p "$DEST" "/nro/$(basename "$NRO")" "$@" \
  2>&1 | tee "$LOG"
