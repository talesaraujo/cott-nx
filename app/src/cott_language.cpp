// cott - the game's language, and the Brazilian Portuguese translation.
//
// HOW THE GAME PICKS ITS LANGUAGE (read in the generated code, 2026-10-19)
//   A letter: E, F, G, I or S from XGetLanguage (user_language: 1 English, 3 German, 4 French, 5 Spanish,
//   6 Italian), or D, U, A, H, N, W from the console's country or a menu. The engine knows Portuguese
//   ('P'), but this game has no texts for it and turns it into English.
//
// cott_language = auto (default) | en | fr | de | es | it | pt-BR
//   auto: the Switch's system language (Portuguese -> pt-BR; English for the languages the game does
//   not have); on the PC, whatever user_language says.
//   pt-BR: the game runs in English (its voices too, the original dub) and every string with a
//   translation in translations/pt_BR.txt (built into the executable) shows it instead.
//
// THE TRANSLATION
//   translations/pt_BR.txt: one line per English text, "<key>\t<Portuguese>", the key being the 64-bit
//   FNV-1a of the English text's UTF-16 code units (tools/text/textbible.py, text_key): the game keeps
//   several differing copies of its text banks, so a string is known by its English. {A}, {START}...
//   are the button pictures, \n a line break. Made from a .po by tools/text/compile_po.py.
//   All the translations go once into one block of guest memory (the system heap), held as the game
//   holds texts: big-endian UTF-16 ended by 0. When an English pure3d::Language loads, each string with
//   a translation gets its offset pointed into the block (the block address minus the Language's text,
//   modulo 2^32). The Language's own buffer is untouched, so the game only ever frees its own memory, and
//   a translation can be longer than the English.
//
// THE LETTERS THE FONTS LACK
//   The two menu fonts (pure3d::TextureFont, 237 glyphs) have every Portuguese letter but ã, õ, Ã, Õ, Ê,
//   Ô: ã Ã Ê Ô are there with no picture (width 0), õ Õ not at all. Every glyph cell of a font has the
//   same height, so each one is made of a letter and the accent of another: ã = a + the tilde of ñ,
//   õ = o + the tilde of ñ; the capitals with an accent are drawn smaller, so Ã = Á without its accent +
//   the tilde of Ñ, Õ = Ó + Ñ's, Ê = É + Â's circumflex, Ô = Ó + Â's. The accent ends at the first row
//   with no solid pixel (the outlines touch). Each is drawn over the cell of a Cyrillic letter (never
//   shown with the English texts) and the glyph is pointed at it. õ and Õ take over the glyphs of ø and Ø (unused in
//   Portuguese): the translation is loaded with ø and Ø in their place. A glyph's cell is rows
//   [v0, v1) x page height of its page (as cott_font_pages.h reads it), the glyph upside down in it;
//   pixels are matched through the pages' palettes (white letters with a grey outline).

#include "cott_language.h"

#include "cott_font_pages.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_STRING(cott_language, "auto", "Game",
                      "Language of the game's texts: auto (the Switch's system language; Portuguese gives "
                      "pt-BR, the languages the game does not have give English), en, fr, de, es, it, or pt-BR (Brazilian Portuguese, translated by the port: "
                      "texts in Portuguese, voices in English)")
    .allowed({"auto", "en", "fr", "de", "es", "it", "pt-BR"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using namespace cott_font_pages;

// translations/pt_BR.txt as bytes (kPortuguese, kPortugueseSize), written by app/CMakeLists.txt.
#include "cott_translation_pt_BR.inc"

constexpr uint32_t kLanguageVtable = 0x82033324;
constexpr uint32_t kLanguageCode = 16, kLanguageCount = 20, kLanguageOffsets = 28, kLanguageText = 32;
constexpr char16_t kFirstPicture = 0xB1;
constexpr const char* kPictures[] = {"A",  "B",      "X",    "Y",     "BACK",   "START", "RT",
                                     "RB", "LSTICK", "LEFT", "RIGHT", "ROTATE", "LT",    "LB"};

bool g_translate = false;

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + address, &value, 4);
}

// --- The language -------------------------------------------------------------------------------------

// The game's language for the Switch's system language: "fr", "de", "es", "it", "pt-BR", or "en" for
// English and every language the game does not have. "" elsewhere (the PC: user_language decides).
std::string SystemLanguage() {
#if REX_PLATFORM_SWITCH
  std::string out = "en";
  if (R_SUCCEEDED(setInitialize())) {
    u64 code = 0;
    SetLanguage language;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language))) {
      switch (language) {
        case SetLanguage_ENUS:
        case SetLanguage_ENGB:
          out = "en";
          break;
        case SetLanguage_FR:
        case SetLanguage_FRCA:
          out = "fr";
          break;
        case SetLanguage_DE:
          out = "de";
          break;
        case SetLanguage_IT:
          out = "it";
          break;
        case SetLanguage_ES:
        case SetLanguage_ES419:
          out = "es";
          break;
        case SetLanguage_PT:
        case SetLanguage_PTBR:
          out = "pt-BR";
          break;
        default:
          break;
      }
    }
    setExit();
  }
  return out;
#else
  return "";
#endif
}

// --- The translation ----------------------------------------------------------------------------------

std::u16string Utf16(std::string_view utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t code = c, extra = 0;
    if (c >= 0xF0) code = c & 7, extra = 3;
    else if (c >= 0xE0) code = c & 15, extra = 2;
    else if (c >= 0xC0) code = c & 31, extra = 1;
    ++i;
    for (; extra && i < utf8.size(); --extra, ++i) code = code << 6 | (uint8_t(utf8[i]) & 63);
    out += code < 0x10000 ? char16_t(code) : u'?';
  }
  return out;
}

// One line's text as the game shows it: escapes undone, button pictures, ø/Ø for õ/Õ.
std::u16string GameText(std::string_view line) {
  std::string text;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      const char c = line[++i];
      text += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else if (line[i] == '{') {
      const size_t end = line.find('}', i);
      const std::string_view name = end == std::string_view::npos ? "" : line.substr(i + 1, end - i - 1);
      const auto* it = std::find(std::begin(kPictures), std::end(kPictures), name);
      if (it != std::end(kPictures)) {
        // U+00B1..U+00BE in UTF-8.
        const char16_t c = char16_t(kFirstPicture + (it - std::begin(kPictures)));
        text += char(0xC0 | c >> 6);
        text += char(0x80 | (c & 63));
        i = end;
      } else {
        text += '{';
      }
    } else {
      text += line[i];
    }
  }
  std::u16string out = Utf16(text);
  for (char16_t& c : out) c = c == u'õ' ? u'ø' : c == u'Õ' ? u'Ø' : c;
  return out;
}

struct Table {
  std::unordered_map<uint64_t, uint32_t> address;  // the English text's key -> the translation (guest)
};

const Table& GetTable(uint8_t* base) {
  static Table table;
  static std::once_flag once;
  std::call_once(once, [base] {
    std::vector<std::pair<uint64_t, std::u16string>> entries;
    const std::string_view file(reinterpret_cast<const char*>(kPortuguese), kPortugueseSize);
    size_t bytes = 0;
    for (size_t at = 0; at < file.size();) {
      size_t end = file.find('\n', at);
      if (end == std::string_view::npos) end = file.size();
      std::string_view line = file.substr(at, end - at);
      at = end + 1;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      const size_t tab = line.find('\t');
      if (line.empty() || line[0] == '#' || tab != 16) continue;
      uint64_t key = 0;
      if (std::from_chars(line.data(), line.data() + 16, key, 16).ec != std::errc()) continue;
      std::u16string text = GameText(line.substr(17));
      if (text.empty()) continue;
      bytes += (text.size() + 1) * 2;
      entries.emplace_back(key, std::move(text));
    }
    auto* memory = rex::system::kernel_memory();
    const uint32_t block = memory && bytes ? memory->SystemHeapAlloc(uint32_t(bytes), 16) : 0;
    if (!block) {
      REXLOG_WARN("[language] no Portuguese texts ({} in the table, {} bytes)", entries.size(), bytes);
      return;
    }
    uint32_t at = block;
    for (const auto& [key, text] : entries) {
      table.address.emplace(key, at);
      for (const char16_t c : text) {
        base[at] = uint8_t(c >> 8);
        base[at + 1] = uint8_t(c);
        at += 2;
      }
      base[at] = base[at + 1] = 0;
      at += 2;
    }
    REXLOG_INFO("[language] {} Portuguese texts at {:08X} ({} KB)", table.address.size(), block, bytes >> 10);
  });
  return table;
}

// --- The letters --------------------------------------------------------------------------------------

// pure3d::TextureFont glyph in memory (40 bytes): metrics, character << 16 | flags, page << 1 | 1 for
// a picture glyph (cott_prompt_icons.cpp), texture coordinates of its cell.
struct Glyph {
  float left, right, width, advance;
  uint32_t tag, page;
  float u0, v0, u1, v1;
};

Glyph ReadGlyph(const uint8_t* base, uint32_t address) {
  uint32_t w[10];
  for (int i = 0; i < 10; ++i) w[i] = Load32(base, address + 4 * i);
  return {std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1]), std::bit_cast<float>(w[2]),
          std::bit_cast<float>(w[3]), w[4], w[5], std::bit_cast<float>(w[6]), std::bit_cast<float>(w[7]),
          std::bit_cast<float>(w[8]), std::bit_cast<float>(w[9])};
}

void WriteGlyph(uint8_t* base, uint32_t address, const Glyph& g) {
  const uint32_t w[10] = {std::bit_cast<uint32_t>(g.left), std::bit_cast<uint32_t>(g.right),
                          std::bit_cast<uint32_t>(g.width), std::bit_cast<uint32_t>(g.advance), g.tag, g.page,
                          std::bit_cast<uint32_t>(g.u0), std::bit_cast<uint32_t>(g.v0),
                          std::bit_cast<uint32_t>(g.u1), std::bit_cast<uint32_t>(g.v1)};
  for (int i = 0; i < 10; ++i) Store32(base, address + 4 * i, w[i]);
}

// A glyph's picture: its page and its rectangle in texture pixels (y from the top).
struct Cell {
  Page page;
  uint32_t texture = 0, glyph = 0;  // the glyph's address (0 for free space)
  uint32_t page_index = 0;
  float v0 = 0, v1 = 0;
  int x = 0, y = 0, w = 0, h = 0;
  bool ok = false;

  // Row cy from the glyph's top: the cell is rows [y, y + h) of the page, the glyph upside down in it.
  uint32_t Row(int cy) const { return page.pixels + uint32_t(y + h - 1 - cy) * page.pitch + uint32_t(x); }
  uint32_t Argb(const uint8_t* base, int cx, int cy) const {
    return Load32(base, page.palette + 4u * base[Row(cy) + uint32_t(cx)]);  // A, R, G, B
  }
};

struct Font {
  uint32_t font, glyph_array, characters, glyphs, page_array, pages;

  // The glyph of character c (its own word 4 says which: character << 16 | flags), or 0. The sorted
  // characters at +84 do not run parallel to the glyphs.
  uint32_t Find(const uint8_t* base, char16_t c) const {
    for (uint32_t i = 0; i < glyphs; ++i) {
      if (Load32(base, glyph_array + kGlyphSize * i + 16) >> 16 == c) return glyph_array + kGlyphSize * i;
    }
    return 0;
  }

  Cell CellOf(const uint8_t* base, uint32_t glyph) const {
    Cell cell;
    if (!glyph) return cell;
    const Glyph g = ReadGlyph(base, glyph);
    if ((g.page >> 1) >= pages) return cell;
    cell.glyph = glyph;
    cell.page_index = g.page >> 1;
    cell.v0 = g.v0;
    cell.v1 = g.v1;
    cell.texture = Load32(base, page_array + 4 * cell.page_index);
    cell.page = ReadPage(base, cell.texture);
    if (!cell.page.ok) return cell;
    const float W = float(cell.page.width), H = float(cell.page.height);
    cell.x = int(std::lround(g.u0 * W));
    cell.y = int(std::lround(g.v0 * H));
    cell.w = int(std::lround(g.u1 * W)) - cell.x;
    cell.h = int(std::lround(g.v1 * H)) - cell.y;
    cell.ok = cell.w > 0 && cell.h > 0 && cell.x >= 0 && cell.y >= 0 && cell.x + cell.w <= int(cell.page.width) &&
              cell.y + cell.h <= int(cell.page.height);
    return cell;
  }
};

// Where an accent ends: the first row with no solid pixel after the first solid ones (the outlines of
// the accent and the letter touch, their solid parts do not).
int AccentEnd(const uint8_t* base, const Cell& cell) {
  auto solid = [&](int y) {
    for (int x = 0; x < cell.w; ++x)
      if ((cell.Argb(base, x, y) >> 24) > 128) return true;
    return false;
  };
  int y = 0;
  while (y < cell.h && !solid(y)) ++y;
  while (y < cell.h && solid(y)) ++y;
  return y;
}

// The ink's horizontal middle, in rows [y0, y1).
float Middle(const uint8_t* base, const Cell& cell, int y0, int y1) {
  int lo = cell.w, hi = -1;
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < cell.w; ++x)
      if (cell.Argb(base, x, y) >> 24) lo = std::min(lo, x), hi = std::max(hi, x);
  return hi < 0 ? -1.0f : (lo + hi) / 2.0f;
}

// The palette index of `page` closest to the color (alpha counts double).
uint8_t Nearest(const uint8_t* base, const Page& page, uint32_t argb) {
  int best = 0, best_distance = INT32_MAX;
  for (int i = 0; i < 256; ++i) {
    const uint32_t p = Load32(base, page.palette + 4u * i);
    int distance = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const int d = int(p >> shift & 255) - int(argb >> shift & 255);
      distance += d * d * (shift == 24 ? 4 : 1);
    }
    if (distance < best_distance) best = i, best_distance = distance;
  }
  return uint8_t(best);
}

bool PaletteFilled(const uint8_t* base, const Page& page) {
  for (int i = 0; i < 256; ++i)
    if (Load32(base, page.palette + 4u * i) >> 24) return true;
  return false;
}

// Places to draw the new letters: in the rows of a page where cells of height h sit, the stretches no
// glyph covers and whose pixels are all transparent (1 pixel kept clear on each side). The widest first.
std::vector<Cell> FreeSpace(const uint8_t* base, const Font& font, int h) {
  std::vector<Cell> cells;
  for (uint32_t i = 0; i < font.glyphs; ++i) {
    const Cell cell = font.CellOf(base, font.glyph_array + kGlyphSize * i);
    if (cell.ok) cells.push_back(cell);
  }
  std::vector<Cell> free;
  std::vector<std::pair<uint32_t, int>> bands;  // page, row
  for (const Cell& band : cells) {
    if (band.h != h || std::find(bands.begin(), bands.end(), std::make_pair(band.page_index, band.y)) != bands.end()) {
      continue;
    }
    bands.emplace_back(band.page_index, band.y);
    std::vector<std::pair<int, int>> used;
    for (const Cell& c : cells) {
      if (c.page_index == band.page_index && c.y < band.y + h && band.y < c.y + c.h) used.emplace_back(c.x, c.x + c.w);
    }
    std::sort(used.begin(), used.end());
    int x = 0;
    used.emplace_back(int(band.page.width), int(band.page.width));
    for (const auto& [from, to] : used) {
      // [x, from) is not covered by any glyph: keep its transparent columns.
      int start = x;
      for (int cx = x; cx <= from; ++cx) {
        bool clear = cx < from;
        for (int cy = 0; clear && cy < h; ++cy) {
          clear = (Load32(base, band.page.palette + 4u * base[band.page.pixels + uint32_t(band.y + cy) * band.page.pitch +
                                                               uint32_t(cx)]) >> 24) == 0;
        }
        if (!clear) {
          if (cx - 1 - (start + 1) >= 8) {
            Cell space = band;
            space.glyph = 0;
            space.x = start + 1;
            space.w = cx - 1 - space.x;
            free.push_back(space);
          }
          start = cx + 1;
        }
      }
      x = std::max(x, to);
    }
  }
  std::sort(free.begin(), free.end(), [](const Cell& a, const Cell& b) { return a.w > b.w; });
  return free;
}

struct Letter {
  char16_t target;  // the glyph that gets the picture
  char16_t letter;  // the letter it is drawn from
  bool cut;         // that letter has an accent of its own, dropped (capitals with an accent are smaller)
  char16_t accent;  // the letter whose accent it gets
};
constexpr Letter kLetters[] = {
    {u'ã', u'a', false, u'ñ'}, {u'ø', u'o', false, u'ñ'},  // ø shows õ
    {u'Ã', u'Á', true, u'Ñ'},  {u'Ø', u'Ó', true, u'Ñ'},   // Ø shows Õ
    {u'Ê', u'É', true, u'Â'},  {u'Ô', u'Ó', true, u'Â'},
};

struct FontState {
  uint32_t glyph_array = 0;
  uint64_t hash = 0;  // of the cells drawn, to notice the game drawing over them
  std::vector<std::pair<uint32_t, Cell>> drawn;  // the cells written (page texture, rectangle)
};
std::unordered_map<uint32_t, FontState> g_fonts;

uint64_t HashCells(const uint8_t* base, const std::vector<std::pair<uint32_t, Cell>>& cells) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const auto& [texture, cell] : cells) {
    for (int y = 0; y < cell.h; ++y) h = Fnv(base + cell.Row(y), uint32_t(cell.w), h);
  }
  return h;
}

// Draws the letters kLetters into one font. False when it cannot yet (palettes not filled).
bool DrawLetters(uint8_t* base, const Font& font, FontState& state) {
  // Cyrillic cells, the widest first, to draw over.
  // Where to draw: free space on the pages, then the cells of Cyrillic letters.
  const Cell any = font.CellOf(base, font.Find(base, u'a'));
  if (!any.ok) return true;
  std::vector<Cell> donors = FreeSpace(base, font, any.h);
  for (char16_t c = 0x410; c <= 0x44F; ++c) {
    const Cell cell = font.CellOf(base, font.Find(base, c));
    if (cell.ok) donors.push_back(cell);
  }
  std::vector<std::pair<uint32_t, Cell>> drawn;
  std::string made;
  for (const Letter& l : kLetters) {
    const uint32_t target = font.Find(base, l.target);
    const Cell letter = font.CellOf(base, font.Find(base, l.letter));
    const Cell accent = font.CellOf(base, font.Find(base, l.accent));
    if (!target || !letter.ok || !accent.ok || letter.h != accent.h) {
      REXLOG_INFO("[language] font {:08X} {:04X}: target {:08X}, cells {}x{} {} / {}x{} {} (glyph {:08X}: {:08X})",
                font.font, uint32_t(l.target), target, letter.w, letter.h, letter.ok, accent.w, accent.h, accent.ok,
                accent.glyph, accent.glyph ? Load32(base, accent.glyph + 16) : 0);
      continue;
    }
    if (!PaletteFilled(base, letter.page) || !PaletteFilled(base, accent.page)) return false;
    // The letter's rows below its own accent, the accent's rows above its letter.
    const int letter_from = l.cut ? AccentEnd(base, letter) : 0;
    const int accent_rows = AccentEnd(base, accent);
    const float accent_middle = Middle(base, accent, 0, accent_rows);
    const float letter_middle = Middle(base, letter, letter_from, letter.h);
    if (accent_middle < 0 || letter_middle < 0) {
      REXLOG_INFO("[language] font {:08X} {:04X}: no accent above row {} or no letter below row {}", font.font,
                  uint32_t(l.target), accent_rows, letter_from);
      continue;
    }
    const int dx = int(std::lround(letter_middle - accent_middle));
    std::vector<uint32_t> picture(size_t(letter.w) * letter.h, 0);
    for (int y = letter_from; y < letter.h; ++y)
      for (int x = 0; x < letter.w; ++x) picture[size_t(y) * letter.w + x] = letter.Argb(base, x, y);
    for (int y = 0; y < accent_rows && y < letter.h; ++y) {
      for (int x = 0; x < accent.w; ++x) {
        const int tx = x + dx;
        if (tx < 0 || tx >= letter.w) continue;
        const uint32_t a = accent.Argb(base, x, y);
        uint32_t& out = picture[size_t(y) * letter.w + tx];
        if ((a >> 24) > (out >> 24)) out = a;
      }
    }
    // A Cyrillic cell wide enough, of the same height.
    auto donor = std::find_if(donors.begin(), donors.end(),
                              [&](const Cell& d) { return d.w >= letter.w && d.h == letter.h; });
    if (donor == donors.end() || !PaletteFilled(base, donor->page)) {
      REXLOG_INFO("[language] font {:08X} {:04X}: no cell for {}x{} among {}", font.font, uint32_t(l.target),
                  letter.w, letter.h, donors.size());
      continue;
    }
    Cell cell = *donor;
    if (donor->glyph) {
      donors.erase(donor);
    } else {
      // Free space: take what the letter needs, keep the rest.
      cell.w = letter.w;
      donor->x += letter.w + 2;
      donor->w -= letter.w + 2;
      if (donor->w < 8) donors.erase(donor);
    }
    std::vector<uint8_t> row(size_t(cell.w));
    const uint8_t clear = Nearest(base, cell.page, 0);
    std::unordered_map<uint32_t, uint8_t> nearest;
    for (int y = 0; y < cell.h; ++y) {
      for (int x = 0; x < cell.w; ++x) {
        if (x >= letter.w) {
          row[size_t(x)] = clear;
          continue;
        }
        const uint32_t argb = picture[size_t(y) * letter.w + x];
        auto it = nearest.find(argb);
        if (it == nearest.end()) it = nearest.emplace(argb, Nearest(base, cell.page, argb)).first;
        row[size_t(x)] = it->second;
      }
      WriteGuest(base, cell.Row(y), row.data(), uint32_t(cell.w));
    }
    // The target glyph: the base letter's metrics, the new cell.
    const Glyph base_glyph = ReadGlyph(base, letter.glyph);
    Glyph g = ReadGlyph(base, target);
    g.left = base_glyph.left;
    g.right = base_glyph.right;
    g.width = base_glyph.width;
    g.advance = base_glyph.advance;
    g.page = cell.page_index << 1;
    g.u0 = float(cell.x) / float(cell.page.width);
    g.v0 = cell.v0;
    g.v1 = cell.v1;
    g.u1 = float(cell.x + letter.w) / float(cell.page.width);
    WriteGlyph(base, target, g);
    Cell written = cell;
    written.w = letter.w;
    drawn.emplace_back(cell.texture, written);
    if (l.target == u'ø' || l.target == u'Ø') {
      made += l.target == u'ø' ? "õ" : "Õ";
    } else {
      made += char(0xC0 | l.target >> 6);
      made += char(0x80 | (l.target & 63));
    }
    made += fmt::format(" (page {} at {},{}) ", cell.page_index, cell.x, cell.y);
  }
  state.drawn = std::move(drawn);
  state.hash = HashCells(base, state.drawn);
  REXLOG_INFO("[language] font {:08X}: {}drawn", font.font, made.empty() ? "nothing " : made);
  return true;
}

}  // namespace

void CottLanguageConfigure() {
  const std::string asked = REXCVAR_GET(cott_language);
  std::string choice = asked == "auto" ? SystemLanguage() : asked;
  g_translate = choice == "pt-BR";
  // XGetLanguage's numbers. pt-BR: the English texts, translated as they load.
  uint32_t xbox = 0;
  if (choice == "en" || choice == "pt-BR") xbox = 1;
  else if (choice == "de") xbox = 3;
  else if (choice == "fr") xbox = 4;
  else if (choice == "es") xbox = 5;
  else if (choice == "it") xbox = 6;
  if (xbox && !rex::cvar::HasNonDefaultValue("user_language")) {
    rex::cvar::SetFlagByName("user_language", std::to_string(xbox));
  }
  REXLOG_INFO("[language] cott_language {}: {} (user_language {})", asked,
              choice.empty() ? std::string("the game's own choice") : choice, REXCVAR_GET(user_language));
  if (g_translate && REXCVAR_GET(user_language) != 1) {
    REXLOG_WARN("[language] pt-BR translates the English texts, but user_language is {}: not translated",
                REXCVAR_GET(user_language));
  }
}

void CottLanguageTranslate(uint8_t* base, uint32_t language) {
  if (!g_translate || !Plausible(language) || Load32(base, language) != kLanguageVtable ||
      base[language + kLanguageCode] != 'E') {
    return;
  }
  const Table& table = GetTable(base);
  const uint32_t count = Load32(base, language + kLanguageCount);
  const uint32_t offsets = Load32(base, language + kLanguageOffsets), text = Load32(base, language + kLanguageText);
  if (table.address.empty() || count > 100000 || !Plausible(offsets) || !Plausible(text)) return;
  uint32_t translated = 0;
  for (uint32_t i = 0; i < count; ++i) {
    // The English text's key, as tools/text/textbible.py text_key.
    uint64_t key = 0xcbf29ce484222325ull;
    uint32_t at = text + Load32(base, offsets + 4 * i);
    for (uint32_t n = 0; n < 4096; ++n, at += 2) {
      const uint16_t c = Load16(base, at);
      if (!c) break;
      key = (key ^ c) * 0x100000001b3ull;
    }
    const auto it = table.address.find(key);
    if (it == table.address.end()) continue;
    Store32(base, offsets + 4 * i, it->second - text);
    ++translated;
  }
  REXLOG_INFO("[language] {} of {} strings in Portuguese", translated, count);
}

void CottLanguageCheckFont(uint8_t* base, uint32_t font) {
  if (!g_translate) return;
  const Font f = {font, Load32(base, font + kFontGlyphs), Load32(base, font + kFontCharacters),
                  Load32(base, font + kFontGlyphCount), Load32(base, font + kFontPages),
                  Load32(base, font + kFontPageCount)};
  if (f.glyphs > 4096 || f.pages > 256 || !Plausible(f.glyph_array) || !Plausible(f.characters) ||
      !Plausible(f.page_array)) {
    return;
  }
  FontState& state = g_fonts[font];
  if (state.glyph_array == f.glyph_array && HashCells(base, state.drawn) == state.hash) return;
  if (state.glyph_array == f.glyph_array && !state.drawn.empty()) {
    REXLOG_INFO("[language] font {:08X}: the letters were drawn over; again", font);
  }
  if (DrawLetters(base, f, state)) state.glyph_array = f.glyph_array;
}
