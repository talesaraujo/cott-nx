// cott - the game's language, and the Brazilian Portuguese translation (cott_language.cpp).
#pragma once

#include <cstdint>

// Picks the game's language from cott_language; before the game starts (it sets user_language).
void CottLanguageConfigure();
// A pure3d::Language just loaded: when translating, its strings that have a translation point to it.
// Called before the Switch rewording (cott_switch_text.cpp), on the loader's thread.
void CottLanguageTranslate(uint8_t* base, uint32_t language);
// A live pure3d::TextureFont, every 8th frame on the game's thread (cott_prompt_icons.cpp): when
// translating, draws the letters Portuguese needs and the font lacks.
void CottLanguageCheckFont(uint8_t* base, uint32_t font);
