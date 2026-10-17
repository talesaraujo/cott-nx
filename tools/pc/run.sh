#!/bin/bash
# Runs the Linux PC build of the port against the extracted disc.
#   tools/pc/run.sh [extra --cvar=value ...]
# Logs go to logs/pc/run_<date>.log.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/app/out/linux}
mkdir -p "$ROOT/logs/pc"
LOG="$ROOT/logs/pc/run_$(date +%Y%m%d_%H%M%S).log"
# The GPU plugin is built into sdk/out/linux-amd64 and only copied next to the executable when the
# executable itself relinks, so a change limited to the plugin would run the old copy.
cp -u "$ROOT"/sdk/out/linux-amd64/librexgpu-*.so "$BUILD"/ 2>/dev/null || true
export LD_LIBRARY_PATH="$BUILD:$ROOT/sdk/out/linux-amd64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
echo "log: $LOG"
exec "$BUILD/cott" --game_data_root="$ROOT/assets/game_root" --gpu_plugin=xenos --mnk_mode=true --render_target_path_vulkan=fsi --native_shader_path="$ROOT/build/shaders/spv" --native_shader_capture_path="$ROOT/build/shaders/ucode" --log_file="$LOG" "$@"
