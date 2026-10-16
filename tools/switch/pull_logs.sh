#!/bin/bash
# Copies the game's logs and Atmosphère's crash reports from the Switch to logs/switch/<date>/, over FTP
# (ftpd or sys-ftpd running on the Switch). No reboot, no SD card in the PC.
#
#   SWITCH_IP=192.168.x.y [FTP_PORT=5000] [FTP_USER=... FTP_PASS=...] tools/switch/pull_logs.sh
#
# Only the newest files of each folder are copied (N=5 by default): crash_reports grows forever.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SWITCH_IP=${SWITCH_IP:?set SWITCH_IP}
FTP_PORT=${FTP_PORT:-5000}
N=${N:-5}
AUTH=()
[ -n "${FTP_USER:-}" ] && AUTH=(--user "$FTP_USER:${FTP_PASS:-}")
BASE="ftp://$SWITCH_IP:$FTP_PORT"
OUT="$ROOT/logs/switch/$(date +%Y%m%d_%H%M%S)"

# Folders on the SD. logs/rex holds rex_stderr.log and the crash dumps written by switch_crash_hooks.c.
for dir in switch/cott-nx/logs switch/cott-nx/logs/rex atmosphere/crash_reports atmosphere/fatal_reports; do
  names=$(curl -s "${AUTH[@]}" --list-only "$BASE/$dir/" | tr -d '\r' | grep -v '/$' | grep '\.' | sort -V | tail -n "$N" || true)
  [ -z "$names" ] && continue
  mkdir -p "$OUT/$dir"
  while read -r f; do
    curl -s "${AUTH[@]}" -o "$OUT/$dir/$f" "$BASE/$dir/$f" && echo "$dir/$f"
  done <<< "$names"
done
echo "-> $OUT"
