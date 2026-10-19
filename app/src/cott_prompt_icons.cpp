// cott - Switch button pictures in the game's prompts.
//
// WHERE THE PICTURES ARE (found on the PC build with the probe below; same engine and layout as Mind over
// Mutant, Mind over Recomp docs/findings/37, whose approach this follows)
//   The game draws its button prompts with picture glyphs of Pure3D TextureFonts. Two fonts are loaded
//   (32 and 20 pages), each with the same 14 pictures: U+00B1..U+00BE = A, B, X, Y, Back, Start, RT, RB,
//   left stick, the menus' red left and right arrows, "rotate", LT, LB. Found in this XEX through MSVC
//   RTTI (.?AVTextureFontLoader@pure3d@@):
//   - pure3d::TextureFontLoader vtable 0x82035620, slot 5 LoadObject = sub_82339970: builds the font and
//     stores it through its r4 argument (stw r31,0([r4]) at the end).
//   - pure3d::TextureFont vtable 0x82035594: +60 page count, +64 pure3d::Texture*[pages], +72 glyph
//     count, +76 glyphs (40 bytes), +84 the sorted u16 characters.
//   - pure3d::Texture +24 -> its pddi texture: +20 the D3D texture of the indices, +24 a ring of 16
//     palette textures, +88 the slot in use. A D3D texture's fetch constant is at +28 (6 words).
//   - Every picture page: format k_8, linear, pitch 256 bytes, 64x64, 64x128, 128x64 or 128x128, rows
//     stored bottom up; the palette is 256x1 k_8_8_8_8 with the bytes A, R, G, B. The palettes are filled
//     after the load, not by LoadObject. Both are 0x1000 past the address in their fetch constant (see
//     kPlacements).
//   - Two button pictures are textures of their own, in the same layout: FE_360_b_button_64.tga (the B
//     over things Crash can act on; same indices as the font's B page, but shown upside down) and
//     HUD_360_nopack_icon.tga (a B with a red "no" sign, in every level's HUD): the entries of
//     cott_prompt_icons_data.h with character 0. pure3d::TextureLoader vtable 0x82034028, slot 5
//     LoadObject = sub_82312458, stores the pure3d::Texture (vtable 0x82034000) through r4 like the font's.
//
// WHAT THIS DOES
//   Fonts are remembered when they load; at the end of every 8th frame (the D3D Present, sub_82274080, on
//   the game's thread), each picture page of each live font is hashed (its rows, then its palette): a
//   page that hashes to the game's own picture of a 360 button gets the Switch picture of the button in
//   the same position (cott_prompt_icons_data.h, made by tools/prompt_icons.py from Xelu's CC0 set):
//   A -> B, B -> A, X -> Y, Y -> X, Back -> Minus, Start -> Plus, RT/RB/LT/LB -> ZR/R/ZL/L and the left
//   stick. The red arrows and "rotate" stay. Every texture loaded with the size of a picture is hashed
//   too for its first checks (its pixels may come after the load): one that shows a button is then
//   checked like a font page, the others are forgotten. A page that hashes to anything else is left
//   alone, so
//   nothing is written into memory that is no longer a font page; if the game writes its picture again
//   (a reload, a palette upload), the next check puts ours back. The renderer sees the change by itself
//   (it hashes small textures every frame), and the memory watchers are told first, as for the game's
//   own writes. The game's texts and layouts do not change.
//
// cott_button_icons = switch (default) | xbox (the game's own pictures).
// cott_prompt_icons_probe=true: every font loaded is logged with its picture glyphs and pages, again 15 s
// later (when the palettes are there); with cott_prompt_icons_dump=<folder> each page is written to a file
// (indices + palette). PC only: those files are the game's data and stay out of the repo.

#include "cott_font_pages.h"
#include "cott_language.h"
#include "cott_prompt_icons_data.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include <rex/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(cott_button_icons, "switch", "Input",
                      "Button pictures in the game's prompts: switch (by position) or xbox (the game's own)")
    .allowed({"switch", "xbox"});
REXCVAR_DEFINE_BOOL(cott_prompt_icons_probe, false, "Debug",
                    "Log every Pure3D TextureFont the game loads, with its picture glyphs and pages");
REXCVAR_DEFINE_STRING(cott_prompt_icons_dump, "", "Debug",
                      "With the probe: folder where every 8-bit font page is written (indices + palette)");

namespace {

using namespace cott_font_pages;

constexpr uint32_t kCheckEvery = 8;    // frames
constexpr uint32_t kTextureTries = 16;  // checks a new texture gets to show a button picture

std::mutex g_mutex;
std::vector<uint32_t> g_fonts;
struct Watched {
  uint32_t texture;
  uint32_t tries;  // checks left before it is forgotten; 0: it showed a button, kept while it does
};
std::vector<Watched> g_textures;
uint32_t g_frames = 0;

bool PictureSize(const Page& page) {
  return std::any_of(std::begin(cott_prompt_icons_data::kPages), std::end(cott_prompt_icons_data::kPages),
                     [&](const auto& k) { return k.width == page.width && k.height == page.height; });
}

// Puts our picture into one page if it shows the game's picture of a button we replace, or our picture
// with another palette. False: neither (wrong placement, another picture, or no longer a font page).
// font and p: the font and its page, or the texture and ~0u.
bool CheckPage(uint8_t* base, uint32_t font, uint32_t p, const Page& page) {
  const uint64_t hash = page.Hash(base);
  for (const auto& k : cott_prompt_icons_data::kPages) {
    if (k.width != page.width || k.height != page.height || (k.character == 0) != (p == ~0u)) continue;
    if (hash == k.game_hash) {
      for (uint32_t y = 0; y < k.height; ++y) {
        WriteGuest(base, page.pixels + y * page.pitch, k.indices + y * k.width, k.width);
      }
      WriteGuest(base, page.palette, k.palette, 1024);
      REXLOG_INFO("[prompt icons] {} {:08X}{}: the 360's {} -> the Switch picture (at {:08X}){}",
                  p == ~0u ? "texture" : "font", font, p == ~0u ? "" : fmt::format(" page {}", p), k.button,
                  page.pixels, page.Hash(base) == k.our_hash ? "" : " DIFFERENT HASH after writing");
      return true;
    }
    if (hash == k.our_hash) {
      if (std::memcmp(base + page.palette, k.palette, 1024) != 0) {
        // Our picture with another palette: the game filled or uploaded the palette again.
        WriteGuest(base, page.palette, k.palette, 1024);
        REXLOG_INFO("[prompt icons] {:08X} {} ({}): palette at {:08X} put back", font,
                    p == ~0u ? std::string("texture") : fmt::format("page {}", p), k.button, page.palette);
      }
      return true;
    }
  }
  return false;
}

// buttons: put the Switch pictures in (cott_button_icons). Every live font also goes to the letters added
// for Portuguese (cott_language.cpp).
void CheckFonts(uint8_t* base, bool buttons) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_fonts.begin(); it != g_fonts.end();) {
    const uint32_t font = *it;
    if (Load32(base, font) != kTextureFontVtable) {  // freed
      it = g_fonts.erase(it);
      continue;
    }
    ++it;
    CottLanguageCheckFont(base, font);
    if (!buttons) continue;
    const uint32_t pages = Load32(base, font + kFontPageCount), page_array = Load32(base, font + kFontPages);
    if (pages > 256 || !Plausible(page_array)) continue;
    for (uint32_t p = 0; p < pages; ++p) {
      const uint32_t texture = Load32(base, page_array + 4 * p);
      for (const uint32_t shift : kPlacements) {
        const Page page = ReadPage(base, texture, shift);
        if (!page.ok || !PictureSize(page)) break;  // not a picture page
        if (CheckPage(base, font, p, page)) break;
      }
    }
  }
  for (auto it = g_textures.begin(); buttons && it != g_textures.end();) {
    bool shows = false;
    if (Load32(base, it->texture) == kTextureVtable) {
      for (const uint32_t shift : kPlacements) {
        const Page page = ReadPage(base, it->texture, shift);
        if (!page.ok || !PictureSize(page)) break;
        if ((shows = CheckPage(base, it->texture, ~0u, page))) break;
      }
    }
    if (shows) {
      it->tries = 0;
      ++it;
    } else if (it->tries > 1) {
      --it->tries;
      ++it;
    } else {
      it = g_textures.erase(it);  // never showed a button, no longer does, or freed
    }
  }
}

// --- Probe (diagnostic) ------------------------------------------------------------------------------------

void Probe(const uint8_t* base, uint32_t font) {
  const uint32_t pages = Load32(base, font + kFontPageCount), page_array = Load32(base, font + kFontPages);
  const uint32_t glyphs = Load32(base, font + kFontGlyphCount), glyph_array = Load32(base, font + kFontGlyphs);
  const uint32_t characters = Load32(base, font + kFontCharacters);
  REXLOG_INFO("[prompt probe] font {:08X}: {} pages at {:08X}, {} glyphs at {:08X}, characters at {:08X}", font,
              pages, page_array, glyphs, glyph_array, characters);
  if (glyphs > 4096 || pages > 256 || !Plausible(characters) || !Plausible(glyph_array) ||
      !Plausible(page_array)) {
    return;
  }
  for (uint32_t i = 0; i < glyphs; ++i) {
    const uint16_t c = Load16(base, characters + 2 * i);
    if (c < 0xA5 || c > 0xBE) continue;
    std::string words;
    for (int w = 0; w < 10; ++w) words += fmt::format(" {:08X}", Load32(base, glyph_array + kGlyphSize * i + 4 * w));
    REXLOG_INFO("[prompt probe]   glyph {:04X}:{}", c, words);
  }
  const std::string folder = REXCVAR_GET(cott_prompt_icons_dump);
  for (uint32_t p = 0; p < pages; ++p) {
    const Page page = ReadPage(base, Load32(base, page_array + 4 * p));
    if (!page.ok) {
      REXLOG_INFO("[prompt probe]   page {}: not an 8-bit linear page", p);
      continue;
    }
    REXLOG_INFO("[prompt probe]   page {} {}x{} pitch {} pixels {:08X} palette {:08X}, hash {:016X}", p, page.width,
                page.height, page.pitch, page.pixels, page.palette, page.Hash(base));
    if (folder.empty()) continue;
    const std::string path = fmt::format("{}/font_{:08X}_page_{:02}.bin", folder, font, p);
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      const uint32_t header[3] = {page.width, page.height, page.pitch};
      std::fwrite(header, 4, 3, file);
      std::fwrite(base + page.pixels, 1, size_t(page.pitch) * page.height, file);
      std::fwrite(base + page.palette, 1, 1024, file);
      std::fclose(file);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82339970);
REX_HOOK_RAW(sub_82339970) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82339970(ctx, base);
  const uint32_t font = Plausible(out) ? Load32(base, out) : 0;
  if (!Plausible(font) || Load32(base, font) != kTextureFontVtable) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (std::find(g_fonts.begin(), g_fonts.end(), font) == g_fonts.end()) g_fonts.push_back(font);
  }
  if (REXCVAR_GET(cott_prompt_icons_probe)) {
    Probe(base, font);
    std::thread([base, font] {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      REXLOG_INFO("[prompt probe] font {:08X} again, 15 s later", font);
      if (Load32(base, font) == kTextureFontVtable) Probe(base, font);
    }).detach();
  }
}

REX_EXTERN(__imp__sub_82312458);
REX_HOOK_RAW(sub_82312458) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82312458(ctx, base);
  const uint32_t texture = Plausible(out) ? Load32(base, out) : 0;
  if (!Plausible(texture) || Load32(base, texture) != kTextureVtable) return;
  const Page page = ReadPage(base, texture);
  if (!page.ok || !PictureSize(page)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (std::none_of(g_textures.begin(), g_textures.end(), [&](const Watched& w) { return w.texture == texture; })) {
    g_textures.push_back({texture, kTextureTries});
  }
}

// The D3D Present, once per frame on the game's thread.
REX_EXTERN(__imp__sub_82274080);
REX_HOOK_RAW(sub_82274080) {
  __imp__sub_82274080(ctx, base);
  if (++g_frames % kCheckEvery != 0) return;
  CheckFonts(base, REXCVAR_GET(cott_button_icons) == "switch");
}
