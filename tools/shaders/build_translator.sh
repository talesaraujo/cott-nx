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
# The sources are patched below for the variant asked for (COTT_UBO), so start from MojoRecomp's version.
git -C "$X" checkout -q XenosRecomp/shader_common.h XenosRecomp/shader_recompiler.cpp
git -C "$X" apply "$REFS/MojoRecomp/patches/xenosrecomp-mojorecomp-runtime.patch"
# CotT: constants through uniform buffers. MojoRecomp's SPIR-V reads every constant, boolean and texture
# index through a buffer device address, a memory load per use; on the Switch's GPU those loads made the
# menu's 2D shader and the world shader 5-10x slower than their DXIL path, which reads cbuffers (Maxwell's
# constant banks). COTT_UBO_CONSTANTS selects that cbuffer path for SPIR-V too: set 4, binding 0 the vertex
# constants, 1 the pixel constants, 2 the shared constants (same layout as the BDA blocks). The renderer
# binds both, so libraries built either way work.
# It is the default (COTT_UBO=0 builds the buffer-address variant): with the linearized shaders it cut
# the level's draws on the Switch from 57.7 to 40.7 ms per frame. Its first test, before linearization,
# was 3x slower: cbuffer loads can be reordered, so the compilers hoisted them out of the
# while(switch(pc)) loop and the live registers doubled.
# Either way, texture and sampler indices are constants in the shader (the renderer's table is the
# identity, slot N is index N): one memory load less before every texture fetch.
python3 - "$X/XenosRecomp/shader_common.h" "$X/XenosRecomp/shader_recompiler.cpp" \
    "$M/gpu/shader_translator.cpp" "$OUT/gen/shader_translator.cpp" "${COTT_UBO:-1}" <<'PY'
import sys
common, recompiler, translator_in, translator_out, ubo = sys.argv[1:6]
ubo = ubo == '1'
s = open(recompiler).read()
for old, new in (('println("#define {}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",\n'
                  '                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);',
                  'println("#define {}_Texture{}DescriptorIndex {}u",\n'
                  '                    constantName, TEXTURE_DIMENSIONS[j], constantInfo->registerIndex.get());'),
                 ('println("#define {}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",\n'
                  '                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);',
                  'println("#define {}_SamplerDescriptorIndex {}u",\n'
                  '                constantName, constantInfo->registerIndex.get());')):
    assert old in s, 'shader_recompiler.cpp changed upstream'
    s = s.replace(old, new, 1)
# loop_end with a predicated break leaves the loop when p0 equals its condition (Xenia's interpreter and
# DXBC translator). XenosRecomp ignored it, so every pixel ran all iterations: the level's motion blur
# (ps_C5AB76F04A3EE0C2) samples along the velocity until p0 says the step left the blur length.
for old, new in (('            case ControlFlowOpcode::LoopEnd:\n                if (simpleControlFlow)\n                {\n'
                  '                    --indentation;\n                    indent();\n                    out += "}\\n";\n',
                  '            case ControlFlowOpcode::LoopEnd:\n                if (simpleControlFlow)\n                {\n'
                  '                    if (cfInstr.loopEnd.isPredicatedBreak)\n'
                  '                    {\n'
                  '                        indent();\n'
                  '                        println("if ({}p0) break;", cfInstr.loopEnd.condition ? "" : "!");\n'
                  '                    }\n'
                  '                    --indentation;\n                    indent();\n                    out += "}\\n";\n'),
                 ('                    println("\\t\\t\\tif (aL < g_LoopCount({}))", uint32_t(cfInstr.loopEnd.loopId));\n',
                  '                    if (cfInstr.loopEnd.isPredicatedBreak)\n'
                  '                        println("\\t\\t\\tif (aL < g_LoopCount({}) && {}p0)", uint32_t(cfInstr.loopEnd.loopId), cfInstr.loopEnd.condition ? "!" : "");\n'
                  '                    else\n'
                  '                        println("\\t\\t\\tif (aL < g_LoopCount({}))", uint32_t(cfInstr.loopEnd.loopId));\n')):
    assert old in s, 'shader_recompiler.cpp changed upstream: ' + old[:40]
    s = s.replace(old, new, 1)
open(recompiler, 'w').write(s)
# Linearize every shader whose jumps all go forward, not only the one MojoRecomp lists: XenosRecomp
# emits shaders with jumps as a while (true) { switch (pc) } state machine that the GPU compiler cannot
# optimize across, and the menu's 2D shader (ps_DD3269CF5A041FA2) is one. MojoRecomp's rewrite turns
# each case into "if (pc <= N)"; it only needed the case count and the shader name generalized.
t = open(translator_in).read()
for old, new in (('    if (name != "ps_1bfff23dcd5c2d84")\n        return false;\n', ''),
                 ('    std::array<size_t, 40> caseLines{};\n    caseLines.fill(SIZE_MAX);\n',
                  '    std::vector<size_t> caseLines;\n'),
                 ('            if (end && *end == \':\' && value < caseLines.size())\n                caseLines[value] = i;\n',
                  '            if (!end || *end != \':\' || value != caseLines.size())\n                return false;\n            caseLines.push_back(i);\n'),
                 ('    if (whileLine == SIZE_MAX)\n        return false;\n',
                  '    if (whileLine == SIZE_MAX || caseLines.size() < 2)\n        return false;\n')):
    assert old in t, 'shader_translator.cpp changed upstream: ' + old[:40]
    t = t.replace(old, new, 1)
open(translator_out, 'w').write(t)
translator_in = translator_out
if not ubo:
    sys.exit(0)
s = open(common).read()
if 'COTT_UBO_CONSTANTS' not in s:
    s = s.replace('#ifdef __spirv__\n\nstruct PushConstants',
                  '#define COTT_UBO_CONSTANTS\n\n#if defined(__spirv__) && !defined(COTT_UBO_CONSTANTS)\n\nstruct PushConstants', 1)
    s = s.replace('    uint4 g_LoopConstants[8] : packoffset(c24);',
                  '    uint4 g_LoopConstants[8] : packoffset(c24); \\\n    uint4 g_BoolFile[2] : packoffset(c32);', 1)
    s = s.replace('uint g_SpecConstants();',
                  '#ifdef __spirv__\n[[vk::constant_id(0)]] const uint g_SpecConstants = 0;\n#define g_SpecConstants() g_SpecConstants\n'
                  '#else\nuint g_SpecConstants();\n#endif', 1)
    assert s.count('COTT_UBO_CONSTANTS') == 2 and 'g_BoolFile' in s, 'shader_common.h changed upstream'
    open(common, 'w').write(s)
s = open(recompiler).read()
if 'COTT_UBO_CONSTANTS' not in s:
    old = 'out += "#ifdef __spirv__\\n\\n";\n\n#ifdef UNLEASHED_RECOMP'
    assert old in s, 'shader_recompiler.cpp changed upstream'
    s = s.replace(old, 'out += "#if defined(__spirv__) && !defined(COTT_UBO_CONSTANTS)\\n\\n";\n\n#ifdef UNLEASHED_RECOMP', 1)
    # The cbuffer arrays get a suffix: shaders with jumps keep their program counter in a local "pc",
    # which hides the pixel constants array of the same name.
    for old, new in (('print("\\tfloat4 {}", constantName);\n\n            if (constantInfo->registerCount > 1)\n'
                      '                print("[{}]", constantInfo->registerCount.get());',
                      'print("\\tfloat4 {}", constantName);\n\n            if (constantInfo->registerCount > 1)\n'
                      '                print("_cb[{}]", constantInfo->registerCount.get());'),
                     ('println("#define {0}(INDEX) select((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)"',
                      'println("#define {0}(INDEX) select((INDEX) < {1}, {0}_cb[min(INDEX, {2})], 0.0)"')):
        assert old in s, 'shader_recompiler.cpp changed upstream'
        s = s.replace(old, new, 1)
    # DXC wants the shared cbuffer's members in increasing offsets; they came grouped by sampler.
    old = ('            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)\n'
           '            {\n'
           '                println("\\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",\n'
           '                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);\n'
           '            }\n'
           '\n'
           '            println("\\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",\n'
           '                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);\n'
           '        }\n'
           '    }\n'
           '\n'
           '    out += "\\tDEFINE_SHARED_CONSTANTS();\\n";')
    new = ('            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)\n'
           '            {\n'
           '                cottMembers.emplace_back(uint32_t(j * 64 + constantInfo->registerIndex * 4), fmt::format("\\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});\\n",\n'
           '                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]));\n'
           '            }\n'
           '\n'
           '            cottMembers.emplace_back(uint32_t(std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4), fmt::format("\\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});\\n",\n'
           '                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]));\n'
           '        }\n'
           '    }\n'
           '    std::sort(cottMembers.begin(), cottMembers.end());\n'
           '    for (auto& member : cottMembers)\n'
           '        out += member.second;\n'
           '\n'
           '    out += "\\tDEFINE_SHARED_CONSTANTS();\\n";')
    assert old in s, 'shader_recompiler.cpp changed upstream'
    s = s.replace(old, new, 1)
    old = '    out += "cbuffer SharedConstants : register(b2, space4)\\n";\n'
    assert old in s, 'shader_recompiler.cpp changed upstream'
    s = s.replace(old, old + '    std::vector<std::pair<uint32_t, std::string>> cottMembers;\n', 1)
    open(recompiler, 'w').write(s)
s = open(translator_in).read()
old = ('"\\n#define MOJORECOMP_BOOL_CONSTANT(INDEX) "\n'
       '        "((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 512 + "\n'
       '        "((INDEX) >> 5) * 4) & (1u << ((INDEX) & 31))) != 0)\\n"\n'
       '        "#ifdef __spirv__\\n"')
assert old in s, 'shader_translator.cpp changed upstream'
s = s.replace(old,
    '"\\n#ifdef COTT_UBO_CONSTANTS\\n"\n'
    '        "#define MOJORECOMP_BOOL_CONSTANT(INDEX) "\n'
    '        "((g_BoolFile[(INDEX) >> 7][((INDEX) >> 5) & 3] & (1u << ((INDEX) & 31))) != 0)\\n"\n'
    '        "#else\\n"\n'
    '        "#define MOJORECOMP_BOOL_CONSTANT(INDEX) "\n'
    '        "((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 512 + "\n'
    '        "((INDEX) >> 5) * 4) & (1u << ((INDEX) & 31))) != 0)\\n"\n'
    '        "#endif\\n"\n'
    '        "#if defined(__spirv__) && !defined(COTT_UBO_CONSTANTS)\\n"', 1)
open(translator_out, 'w').write(s)
PY
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
"${CXX[@]}" "${INC[@]}" -I"$M/gpu" -c "$OUT/gen/shader_translator.cpp" -o st.o
"${CXX[@]}" "${INC[@]}" -c "$M/host/host_paths.cpp" -o hp.o
"${CXX[@]}" "${INC[@]}" -include "$M/gpu/xenos_pch.h" -c "$X/XenosRecomp/shader_recompiler.cpp" -o sr.o
"${CXX[@]}" "${INC[@]}" -c "$ROOT/tools/shaders/mojo_xlate_main.cpp" -o main.o
clang++ st.o hp.o sr.o main.o -o mojo_xlate -ldl -lpthread
cp "$X/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$X/thirdparty/dxc-bin/lib/x64/libdxil.so" .
echo "$OUT/mojo_xlate"
