#!/bin/bash
# Builds the NVK Vulkan driver (Mesa for Horizon) for the Switch NRO on a Linux host, inside a rootless
# podman container, and installs it as the SDK folder the app links against.
#
#   tools/switch/build_mesa_linux.sh <mesa-switch checkout> [<sdk output dir>]
#
# The checkout must be danfromtico/mesa-switch at 1a8c1a66d6f with nfsmw-nx's mesa/mesa-switch-nfsmw.patch
# applied. The default output is <checkout>/mesa-linux-install; pass
# <output>/opt/devkitpro/portlibs/switch to the app as -DREXGLUE_SWITCH_NVK_SDK.
#
# Why a script of our own: at that commit mesa-switch's build-unified.sh (which installs the SDK layout)
# only runs under MSYS2 on Windows, and build-switch.sh (Linux, Docker) only builds the archives. This
# combines the container and host-tool steps of build-switch.sh with the Vulkan part of build-unified.sh's
# configuration and its `meson install`. OpenGL/EGL are left out: the port only needs Vulkan.
set -euo pipefail

MESA=$(cd "${1:?usage: build_mesa_linux.sh <mesa-switch checkout> [<sdk output dir>]}" && pwd)
OUT=${2:-$MESA/mesa-linux-install}
IMAGE=${IMAGE:-devkitpro-mesa-rust}
JOBS=${JOBS:-8}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

if ! podman image exists "$IMAGE"; then
  echo "== building image $IMAGE (Docker.rust from the checkout)"
  podman build -f "$MESA/Docker.rust" -t "$IMAGE" "$MESA"
fi

podman run --rm -v "$MESA:/project:Z" -v "$OUT:/install:Z" -w /project \
  -e JOBS="$JOBS" "$IMAGE" bash -lc '
set -euo pipefail
export PATH="/root/.cargo/bin:$PATH"
# The rustc wrapper only cross-compiles (ELF AArch64) when this is set; build-unified.sh sets it too.
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
rustup target add aarch64-unknown-linux-gnu >/dev/null

echo "== cross wrappers"
mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp /project/bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
MESON_BIN="$(command -v meson)"
[ "$MESON_BIN" = /usr/local/bin/meson ] || ln -sf "$MESON_BIN" /usr/local/bin/meson
cp /project/src/gallium/winsys/nouveau/drm/nouveau.h /opt/devkitpro/portlibs/switch/include/
cp /project/src/nouveau/headers/nv_device_info.h /opt/devkitpro/portlibs/switch/include/
[ -d /usr/lib/llvm-15/lib/clang/15/include ] || \
  ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include
# Rust std wants -lrt -ldl -lutil; src/nouveau/vulkan/rust_switch_stubs.c provides the symbols.
if [ ! -f /opt/devkitpro/portlibs/switch/lib/libdl.a ]; then
  cd /tmp && echo "void __mesa_switch_stub_lib(void) {}" > stub_posix.c
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c stub_posix.c -o stub_posix.o
  for L in dl rt util; do
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs /opt/devkitpro/portlibs/switch/lib/lib${L}.a stub_posix.o
  done
  cd /project
fi

echo "== native host tools (mesa_clc, vtn_bindgen2)"
if [ ! -x builddir-native/src/compiler/clc/mesa_clc ]; then
  meson setup builddir-native --wipe -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=enabled \
    -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dtools=[] -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
  ninja -C builddir-native -j "$JOBS" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
fi
export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"

echo "== cross configure (Vulkan only)"
setup_mode=()
[ -f builddir-switch-vk/meson-private/coredata.dat ] && setup_mode+=(--reconfigure)
meson setup builddir-switch-vk "${setup_mode[@]}" \
  --cross-file switch_cross_file.txt \
  --default-library=static --prefix=/opt/devkitpro/portlibs/switch --libdir=lib \
  --buildtype=release -Doptimization=2 -Db_lto=false -Db_ndebug=true \
  -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dgallium-rusticl=false \
  -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Dvideo-codecs= -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled -Dtools=[] \
  -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dbuild-tests=false

echo "== build"
ninja -C builddir-switch-vk -j "$JOBS"

echo "== install"
rm -rf /install/opt
meson install -C builddir-switch-vk --destdir /install
test -f /install/opt/devkitpro/portlibs/switch/lib/libvulkan.a
'
echo "NVK SDK: $OUT/opt/devkitpro/portlibs/switch"
ls -la "$OUT/opt/devkitpro/portlibs/switch/lib" | head -20
