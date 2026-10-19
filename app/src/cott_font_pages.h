// cott - Pure3D TextureFonts and their pages in guest memory, as the GPU reads them. Shared by the
// button pictures (cott_prompt_icons.cpp) and the letters added for Portuguese (cott_language.cpp).
#pragma once

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>

namespace cott_font_pages {

constexpr uint32_t kTextureFontVtable = 0x82035594, kTextureVtable = 0x82034000;
constexpr uint32_t kFontPageCount = 60, kFontPages = 64, kFontGlyphCount = 72, kFontGlyphs = 76,
                   kFontCharacters = 84, kGlyphSize = 40;
constexpr uint32_t kTexturePddi = 24, kPddiD3D = 20, kPddiPalettes = 24, kPddiNextPalette = 88,
                   kD3DFetch = 28;
constexpr uint32_t kFormat8 = 2, kFormat8888 = 6;

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return rex::byte_swap(value);
}
inline uint16_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, 2);
  return rex::byte_swap(value);
}

// Heap and physical addresses (fonts live at 0xF7xxxxxx).
inline bool Plausible(uint32_t address) { return address >= 0x40000000 && address < 0xFFFF0000; }

inline uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// One font page as the GPU reads it.
struct Page {
  uint32_t width = 0, height = 0, pitch = 0;
  uint32_t pixels = 0, palette = 0;  // guest addresses (through the 0xA0000000 physical view), see below
  bool ok = false;                   // k_8, linear, 256x1 k_8_8_8_8 palette, sane sizes

  // The index rows only: the palette is filled after the load and can change.
  uint64_t Hash(const uint8_t* base) const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < height; ++y) h = Fnv(base + pixels + y * pitch, width, h);
    return h;
  }
};

inline uint32_t Physical(uint32_t fetch_word1) { return 0xA0000000 + (fetch_word1 & 0x1FFFF000); }

// Where the pixels the GPU shows are. Measured on the PC: 0x1000 past the address in the D3D texture
// header, for the indices and for the palette (whose slot is then the value at +88 itself): there the
// pictures are whole and upright, and a test pattern written at the header address showed up 16 rows
// lower. The cause is not known yet (the 4 KB offset of the 0xE0000000 range is the suspect), so the
// header address itself, with the slot before, is tried too; only an exact hash match is ever written.
constexpr uint32_t kPlacements[2] = {0x1000, 0};

inline Page ReadPage(const uint8_t* base, uint32_t texture, uint32_t shift = kPlacements[0]) {
  Page page;
  if (!Plausible(texture)) return page;
  const uint32_t pddi = Load32(base, texture + kTexturePddi);
  if (!Plausible(pddi)) return page;
  const uint32_t d3d = Load32(base, pddi + kPddiD3D);
  const uint32_t slot = (Load32(base, pddi + kPddiNextPalette) - (shift ? 0 : 1)) & 15;
  const uint32_t palette = Load32(base, pddi + kPddiPalettes + 4 * slot);
  if (!Plausible(d3d) || !Plausible(palette)) return page;
  uint32_t f[3], pf[3];
  for (int i = 0; i < 3; ++i) {
    f[i] = Load32(base, d3d + kD3DFetch + 4 * i);
    pf[i] = Load32(base, palette + kD3DFetch + 4 * i);
  }
  page.width = (f[2] & 0x1FFF) + 1;
  page.height = ((f[2] >> 13) & 0x1FFF) + 1;
  page.pitch = ((f[0] >> 22) & 0x1FF) * 32;
  page.pixels = Physical(f[1]) + shift;
  page.palette = Physical(pf[1]) + shift;
  const uint32_t palette_width = (pf[2] & 0x1FFF) + 1, palette_height = ((pf[2] >> 13) & 0x1FFF) + 1;
  page.ok = (f[1] & 63) == kFormat8 && !(f[0] >> 31) && page.width <= 512 && page.height <= 512 &&
            page.pitch >= page.width && (pf[1] & 63) == kFormat8888 && !(pf[0] >> 31) &&
            palette_width == 256 && palette_height == 1;
  return page;
}

// Writes guest memory after telling the memory watchers, as the game's own writes do.
inline void WriteGuest(uint8_t* base, uint32_t address, const uint8_t* data, uint32_t size) {
  if (auto* memory = rex::system::kernel_memory()) {
    memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(), address, size,
                                           true, false);
  }
  std::memcpy(base + address, data, size);
}

}  // namespace cott_font_pages
