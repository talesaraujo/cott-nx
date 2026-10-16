#!/bin/bash
# Builds cott.nro (Nintendo Switch) inside the official devkitA64 container (rootless podman), so nothing
# has to be installed on the host.
#
#   NVK_SDK=<mesa sdk>/opt/devkitpro/portlibs/switch tools/switch/build_nro.sh [cmake --build args...]
#
# Needs: the generated code (app/generated, from rexglue codegen on the PC) and the NVK SDK from
# tools/switch/build_mesa_linux.sh. The repository and the SDK are mounted at the same absolute paths as on
# the host, so paths recorded by CMake and GCC (and later PGO profiles) are the same inside and outside.
# Output: app/out/switch/cott.nro
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NVK_SDK=${NVK_SDK:?set NVK_SDK to <mesa sdk>/opt/devkitpro/portlibs/switch}
NVK_SDK=$(cd "$NVK_SDK" && pwd)
IMAGE=${IMAGE:-docker.io/devkitpro/devkita64:latest}
BUILD=${BUILD:-$ROOT/app/out/switch}
JOBS=${JOBS:-6}

podman run --rm \
  -v "$ROOT:$ROOT:Z" -v "$NVK_SDK:$NVK_SDK:ro,Z" -w "$ROOT" \
  -e ROOT="$ROOT" -e NVK_SDK="$NVK_SDK" -e BUILD="$BUILD" -e JOBS="$JOBS" \
  "$IMAGE" bash -lc '
set -euo pipefail
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$ROOT/app" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXSDK_DIR="$ROOT/sdk" \
    -DREXGLUE_SWITCH_NVK_SDK="$NVK_SDK"
fi
cmake --build "$BUILD" -j "$JOBS" '"$*"'
'
ls -la "$BUILD"/*.nro
