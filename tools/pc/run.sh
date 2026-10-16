#!/bin/bash
# Runs the Linux PC build of the port against the extracted disc.
#   tools/pc/run.sh [extra --cvar=value ...]
# Logs go to logs/pc/run_<date>.log.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/app/out/linux}
mkdir -p "$ROOT/logs/pc"
LOG="$ROOT/logs/pc/run_$(date +%Y%m%d_%H%M%S).log"
export LD_LIBRARY_PATH="$BUILD:$ROOT/sdk/out/linux-amd64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
echo "log: $LOG"
exec "$BUILD/cott" --game_data_root="$ROOT/assets/game_root" --gpu_plugin=xenos --log_file="$LOG" "$@"
