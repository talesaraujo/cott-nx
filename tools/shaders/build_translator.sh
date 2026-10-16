#!/bin/bash
# Builds mojo_xlate, the PC tool that turns the game's shader microcode into SPIR-V (Phase 3, M1).
#
#   tools/shaders/build_translator.sh            -> build/shaders/mojo_xlate/mojo_xlate
#   tools/shaders/xsh_extract.py <title>.xsh build/shaders/ucode
#   MOJORECOMP_DXC_LIB=build/shaders/mojo_xlate/libdxcompiler.so \
#     build/shaders/mojo_xlate/mojo_xlate build/shaders/ucode build/shaders/spv
#
# The translator is MojoRecomp's (runtime/gpu/shader_translator.cpp, ISC): it rebuilds a shader container from
# the microcode the game sends to the ring, translates it with XenosRecomp (hedge-dev, MIT) and its CotT patch,
# and compiles the HLSL with DXC. MojoRecomp's checkout comes from refs/MojoRecomp (REFS overrides it);
# XenosRecomp is cloned at the commit MojoRecomp pins, and DXC is the Linux build inside XenosRecomp's dxc-bin.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REFS=${REFS:-$ROOT/../refs}
M=$REFS/MojoRecomp/runtime
OUT=$ROOT/build/shaders/mojo_xlate
X=$ROOT/build/shaders/deps/XenosRecomp
XENOS_COMMIT=990d03b28a27b50277ee5d8d942e1c5f873869d1   # thirdparty/XenosRecomp in MojoRecomp

if [ ! -d "$X/.git" ]; then
  git clone -q https://github.com/hedge-dev/XenosRecomp.git "$X"
  git -C "$X" checkout -q "$XENOS_COMMIT"
  git -C "$X" submodule update --init --depth 1 thirdparty/dxc-bin thirdparty/fmt thirdparty/xxHash
  git -C "$X" apply "$REFS/MojoRecomp/patches/xenosrecomp-mojorecomp-runtime.patch"
fi
mkdir -p "$OUT/gen"
python3 - "$X/XenosRecomp/shader_common.h" "$OUT/gen/mojorecomp_shader_common_embedded.h" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'w').write("#pragma once\ninline constexpr unsigned char g_mojoShaderCommonH[] = {"
    + ",".join(f"0x{b:02x}" for b in d) + "};\n"
    "inline constexpr unsigned long g_mojoShaderCommonHSize = sizeof(g_mojoShaderCommonH);\n")
PY
INC=(-I"$M" -I"$X/XenosRecomp" -I"$X/thirdparty/dxc-bin/inc" -I"$X/thirdparty/fmt/include" -I"$OUT/gen")
# -fms-extensions: DXC's WinAdapter.h declares its COM interfaces with __declspec(uuid).
CXX=(clang++ -std=c++20 -O2 -w -fms-extensions -DMOJORECOMP_XENOS_BOOL_EXEC -DFMT_HEADER_ONLY)
cd "$OUT"
"${CXX[@]}" "${INC[@]}" -c "$M/gpu/shader_translator.cpp" -o st.o
"${CXX[@]}" "${INC[@]}" -c "$M/host/host_paths.cpp" -o hp.o
"${CXX[@]}" "${INC[@]}" -include "$M/gpu/xenos_pch.h" -c "$X/XenosRecomp/shader_recompiler.cpp" -o sr.o
"${CXX[@]}" "${INC[@]}" -c "$ROOT/tools/shaders/mojo_xlate_main.cpp" -o main.o
clang++ st.o hp.o sr.o main.o -o mojo_xlate -ldl -lpthread
cp "$X/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$X/thirdparty/dxc-bin/lib/x64/libdxil.so" .
echo "$OUT/mojo_xlate"
