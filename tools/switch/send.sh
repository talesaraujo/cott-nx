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
DEST=${DEST:-switch/cott-nx/cott.nro}

mkdir -p "$ROOT/logs/switch"
LOG="$ROOT/logs/switch/nxlink_$(date +%Y%m%d_%H%M%S).log"
echo "sending $NRO -> sdmc:/$DEST ($SWITCH_IP); log: $LOG"
# --network host: nxlink -s listens on 28771 for the game's connection back to the PC.
podman run --rm -i --network host -v "$(dirname "$NRO"):/nro:ro,Z" "$IMAGE" \
  /opt/devkitpro/tools/bin/nxlink -s -a "$SWITCH_IP" -p "$DEST" "/nro/$(basename "$NRO")" "$@" \
  2>&1 | tee "$LOG"
