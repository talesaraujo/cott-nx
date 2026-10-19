// cott - "Nintendo Switch" instead of "Xbox 360" in the game's texts.
//
// WHERE THE TEXTS ARE
//   One Pure3D TextBible per text bank (chunk 0x1800D in the packages of default.rcf; every platform's
//   strings in one bank), with one Language per language (sub-chunk 0x1800E). Only the game's language is
//   kept: the loader of a Language returns 0 for the others. Found in this XEX through MSVC RTTI
//   (.?AVTextBibleLoader@pure3d@@, vtable 0x82034CBC, slot 5 LoadObject = sub_82327368), which reads each
//   language with sub_823271F8. That one returns the pure3d::Language (36 bytes, vtable 0x82033324):
//   +16 the language's code, +20 the string count, +24 the strings' hashes, +28 their offsets, +32 the
//   UTF-16 big-endian characters, each string ended by a 0 character.
//
// WHAT THIS DOES
//   Right after a Language loads, every string that names the 360 (the save messages' "do not turn off
//   the Xbox 360 console", "reconnect the Xbox 360 controller", "press the Xbox Guide button", in the
//   11 languages) is reworded IN PLACE, the rest of the string moved back and 0 characters up to the old
//   end: the new text is never longer, so every offset stays as it was (Mind over Recomp rewords its save
//   messages the same way, saves/save_screen_waits.cpp). The game's own text is never copied here: only
//   the phrases to look for and ours. The other consoles' strings ("Wii Remote", "MEMORY CARD") are never
//   shown on this build and stay.
//
// cott_switch_text = true (default) | false.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

REXCVAR_DEFINE_BOOL(cott_switch_text, true, "Input",
                    "Name the Nintendo Switch instead of the Xbox 360 in the game's texts");

namespace {

constexpr uint32_t kLanguageVtable = 0x82033324;
constexpr uint32_t kLanguageCode = 16, kLanguageCount = 20, kLanguageOffsets = 28, kLanguageText = 32;

// The phrase as the game writes it -> ours, never longer. In order: the longer, more specific phrases
// first; the last one catches anything left.
struct Rule {
  std::u16string_view from, to;
};
constexpr Rule kRules[] = {
    // English
    {u"the Xbox 360 console", u"the Nintendo Switch"},
    {u"the Xbox 360 controller", u"the controller"},
    {u"the Xbox Guide button", u"the HOME Button"},
    // Dutch
    {u"de Xbox 360-console", u"de Nintendo Switch"},
    {u"de Xbox 360 controller", u"de controller"},
    {u"de Xbox Guide-knop", u"de HOME-knop"},
    // French, Italian
    {u"la console Xbox 360", u"la Nintendo Switch"},
    {u"la manette Xbox 360", u"la manette"},
    {u"la touche Xbox Guide", u"le bouton HOME"},
    {u"il controller Xbox 360", u"il controller"},
    {u"il pulsante Guida Xbox", u"il pulsante HOME"},
    // German
    {u"die Xbox 360 Konsole", u"die Nintendo Switch"},
    {u"den Xbox 360 Controller", u"den Controller"},
    {u"die Xbox Guide-Taste", u"die HOME-Taste"},
    // Finnish
    {u"Xbox 360 -konsolia", u"Nintendo Switchiä"},
    {u"Xbox 360 -ohjain", u"ohjain"},
    {u"Xbox-opaspainiketta", u"HOME-näppäintä"},
    // Spanish
    {u"tu consola Xbox 360", u"tu Nintendo Switch"},
    {u"la consola Xbox 360", u"la Nintendo Switch"},
    {u"el Mando Xbox 360", u"el mando"},
    {u"el Botón Guía Xbox", u"el botón HOME"},
    // Swedish, Danish, Norwegian
    {u"din Xbox 360-konsol", u"din Nintendo Switch"},
    {u"Xbox 360-konsollen", u"Nintendo Switch"},
    {u"Xbox 360-konsolen", u"Nintendo Switch"},
    {u"Xbox 360-controlleren", u"controlleren"},
    {u"Xbox 360-handkontrollen", u"handkontrollen"},
    {u"Xbox 360 håndkontroller", u"håndkontrolleren"},
    {u"Xbox Guide-knappen", u"HOME-knappen"},
    // Russian
    {u"консоль Xbox 360", u"Nintendo Switch"},
    {u"контроллер Xbox 360", u"контроллер"},
    // Anything else
    {u"Xbox 360", u"Switch"},
};

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return rex::byte_swap(value);
}

bool Plausible(uint32_t address) { return address >= 0x40000000 && address < 0xFFFF0000; }

char16_t CharAt(const uint8_t* p) { return char16_t(p[0] << 8 | p[1]); }

std::string Utf8(std::u16string_view text) {
  std::string out;
  for (const char16_t c : text) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 63));
    } else {
      out += char(0xE0 | c >> 12);
      out += char(0x80 | (c >> 6 & 63));
      out += char(0x80 | (c & 63));
    }
  }
  return out;
}

// Rewords one string at `p` (big-endian UTF-16, ended by 0). True if it changed.
bool Reword(uint8_t* p) {
  std::u16string text;
  for (const uint8_t* q = p; CharAt(q) != 0 && text.size() < 4096; q += 2) text += CharAt(q);
  if (text.find(u"Xbox") == std::u16string::npos) return false;
  std::u16string ours = text;
  for (const Rule& rule : kRules) {
    for (size_t at = 0; (at = ours.find(rule.from, at)) != std::u16string::npos; at += rule.to.size()) {
      ours.replace(at, rule.from.size(), rule.to);
    }
  }
  if (ours == text || ours.size() > text.size()) return false;
  REXLOG_DEBUG("[switch text] \"{}\"", Utf8(ours));
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = i < ours.size() ? ours[i] : 0;
    p[2 * i] = uint8_t(c >> 8);
    p[2 * i + 1] = uint8_t(c);
  }
  return true;
}

}  // namespace

// pure3d::TextBibleLoader's reading of one Language (r4: the chunk reader). Returns the Language, or 0 for
// a language the game doesn't keep.
REX_EXTERN(__imp__sub_823271F8);
REX_HOOK_RAW(sub_823271F8) {
  __imp__sub_823271F8(ctx, base);
  const uint32_t language = ctx.r3.u32;
  if (!REXCVAR_GET(cott_switch_text) || !Plausible(language) || Load32(base, language) != kLanguageVtable) return;
  const uint32_t count = Load32(base, language + kLanguageCount);
  const uint32_t offsets = Load32(base, language + kLanguageOffsets), text = Load32(base, language + kLanguageText);
  if (count > 100000 || !Plausible(offsets) || !Plausible(text)) return;
  uint32_t changed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    changed += Reword(base + text + Load32(base, offsets + 4 * i)) ? 1 : 0;
  }
  REXLOG_INFO("[switch text] language {}: {} strings, {} name the Nintendo Switch now",
              int(int8_t(base[language + kLanguageCode])), count, changed);
}
