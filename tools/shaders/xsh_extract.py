#!/usr/bin/env python3
"""Extracts the shaders the game used from the SDK's shader storage (<title>.xsh) into one file per shader.

    tools/shaders/xsh_extract.py <565507E4.xsh> <output dir>

The xenos plugin appends every shader the game loads (PM4 IM_LOAD) to <cache>/shaders/shareable/<title>.xsh,
so playing on the PC is the capture step. Vertex shaders are stored as they reach the ring, after the game's
Direct3D patched their vertex fetches, which is the form the native renderer sees too.

Format (sdk/src/graphics/vulkan/pipeline_cache.cpp, ShaderStoredHeader):
  'XESH' magic, version 0x20201219 byte-swapped; then per shader a packed 12-byte header
  {u64 XXH3 of the ucode bytes; u32 dword count : 31, type : 1 (0 vertex, 1 pixel)} and the ucode, raw guest
  memory (big-endian dwords).
Output: vs_<hash>.ucode / ps_<hash>.ucode (the naming MojoRecomp's ShaderTranslator expects), hash as 16 hex digits.
"""
import struct
import sys
from pathlib import Path

MAGIC = 0x48534558  # 'XESH'
VERSION = 0x20201219


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != MAGIC or struct.unpack(">I", struct.pack("<I", version))[0] != VERSION:
        sys.exit(f"not a shader storage file (magic {magic:08X}, version {version:08X})")
    pos, counts = 8, {"vs": 0, "ps": 0}
    while pos + 12 <= len(data):
        ucode_hash, bits = struct.unpack_from("<QI", data, pos)
        dwords, kind = bits & 0x7FFFFFFF, "ps" if bits >> 31 else "vs"
        ucode = data[pos + 12:pos + 12 + dwords * 4]
        if len(ucode) != dwords * 4:
            break  # truncated tail, as the SDK itself would stop
        (out / f"{kind}_{ucode_hash:016X}.ucode").write_bytes(ucode)
        counts[kind] += 1
        pos += 12 + dwords * 4
    print(f"{counts['vs']} vertex and {counts['ps']} pixel shaders -> {out}")


if __name__ == "__main__":
    main()
