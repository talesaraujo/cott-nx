#!/usr/bin/env python3
"""Builds app/src/cott_prompt_icons_data.h: the Switch button pictures as the game's font pages want them.

    python3 tools/prompt_icons.py        (needs Pillow)

Input: app/prompts/switch/*.png, from Xelu's Free Controller & Key Prompts (CC0, public domain; see
app/prompts/README.md). Output, for every Xbox 360 button glyph we replace (U+00B1..U+00BE, docs in
app/src/cott_prompt_icons.cpp):
  - the page size and the FNV-1a hash of the game's own page's index rows (not its palette, which the game
    fills after the load) as the PC probe measured it: the runtime only writes into a page that hashes to
    this;
  - our picture: 8-bit indices, rows from the bottom up like the game's pages, entry 0 transparent, and a
    256-entry palette in the guest's byte order (A, R, G, B);
  - the FNV-1a hash of our own index rows, to know when a page already shows our picture.
Only these hashes come from the game, nothing of its data.

Two pages are not font pages but textures of their own (pure3d::Texture, same 8-bit layout):
FE_360_b_button_64.tga, the B over things Crash can act on, has the same indices as the font's B page but
the game shows it upside down (its B looks the same both ways), so its entry stores the rows top down;
HUD_360_nopack_icon.tga (a B with a red "no" sign) gets the Switch A with the same sign, drawn here.
Entries with character 0 are for textures, the others for font pages.

The pictures go BY POSITION (the port runs with input_xbox_layout: the bottom button is the 360's A):
A -> Switch B, B -> A, X -> Y, Y -> X, Back -> Minus, Start -> Plus, RT/RB/LT/LB -> ZR/R/ZL/L, left stick
-> the Switch left stick. The menus' red arrows and the "rotate" arrow keep the game's pictures.
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, 'app', 'prompts', 'switch')
OUT = os.path.join(ROOT, 'app', 'src', 'cott_prompt_icons_data.h')

# character, 360 button, picture, page width, height, visible rows from the top of the picture (the stick
# glyph shows v 0.25..1: the top 3/4 of the picture, the rows stored last), the game page's hash
# (its index rows only: the palette is filled later).
BUTTONS = [
    (0xB1, 'A', 'Switch_B.png', 64, 64, 64, 0x4A248A036CD6DD1B),
    (0xB2, 'B', 'Switch_A.png', 64, 64, 64, 0xC562FABCB4A7B319),
    (0xB3, 'X', 'Switch_Y.png', 64, 64, 64, 0xBDCA032EC464E094),
    (0xB4, 'Y', 'Switch_X.png', 64, 64, 64, 0x6279ABAAF7B1E45D),
    (0xB5, 'Back', 'Switch_Minus.png', 64, 64, 64, 0x85B66E3B35EFEF1A),
    (0xB6, 'Start', 'Switch_Plus.png', 64, 64, 64, 0xCB467228BB3CF754),
    (0xB7, 'RT', 'Switch_RT.png', 64, 128, 128, 0x6FF591006A875E31),
    (0xB8, 'RB', 'Switch_RB.png', 128, 64, 64, 0xD5B8B459E55BE7FA),
    (0xB9, 'left stick', 'Switch_Left_Stick.png', 128, 128, 96, 0x70DE2A1DEF89E3E5),
    (0xBD, 'LT', 'Switch_LT.png', 64, 128, 128, 0x898CE01114F5FC9A),
    (0xBE, 'LB', 'Switch_LB.png', 128, 64, 64, 0xB284B53D38CC97B2),
    # Not glyphs (character 0): the textures FE_360_b_button_64.tga and HUD_360_nopack_icon.tga.
    (0x00, 'B (over things)', 'Switch_A.png', 64, 64, 64, 0xC562FABCB4A7B319, 'top down'),
    (0x00, 'B (no pack)', 'Switch_A.png', 64, 64, 64, 0xA97137023352EBBC, 'no'),
]
BORDER = 6  # Xelu's pictures have 6 px of empty border on their 100 px


def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def no_sign(canvas):
    """A red ring and a slash from the top left over the whole page, with a dark edge like the game's."""
    k = 4
    big = Image.new('RGBA', (canvas.width * k, canvas.height * k), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    w, h = big.size
    r = min(w, h) / 2 - 2 * k
    cx, cy = w / 2, h / 2
    d = r * 0.7071
    for color, grow in (((20, 0, 0, 255), 2 * k), ((220, 0, 0, 255), 0)):
        draw.ellipse((cx - r - grow / 2, cy - r - grow / 2, cx + r + grow / 2, cy + r + grow / 2),
                     outline=color, width=int(7 * k + grow))
        draw.line((cx - d, cy - d, cx + d, cy + d), fill=color, width=int(7 * k + grow))
    canvas.alpha_composite(big.resize(canvas.size, Image.LANCZOS))
    return canvas


def page(picture, width, height, visible_h):
    """The picture fitted (aspect kept, centered) in the visible top rows of a width x height page."""
    src = Image.open(os.path.join(ICONS, picture)).convert('RGBA')
    src = src.crop((BORDER, BORDER, src.width - BORDER, src.height - BORDER))
    side = min(width, visible_h)
    icon = src.resize((side, side), Image.BOX)
    canvas = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    canvas.alpha_composite(icon, ((width - side) // 2, (visible_h - side) // 2))
    return canvas


def quantize(img):
    """255 colors for the visible pixels, index 0 for the fully transparent ones."""
    rgba = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    visible = [p for p in rgba if p[3] > 0]
    small = Image.new('RGBA', (len(visible), 1))
    small.putdata(visible)
    q = small.quantize(colors=255, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')[:255 * 4] if hasattr(q, 'getpalette') else None
    used = list(q.get_flattened_data() if hasattr(q, "get_flattened_data") else q.getdata())
    indices, k = [], 0
    for p in rgba:
        if p[3] == 0:
            indices.append(0)
        else:
            indices.append(used[k] + 1)
            k += 1
    palette = [(0, 0, 0, 0)] + [tuple(pal[i * 4:i * 4 + 4]) for i in range(255)]
    return indices, palette


def main():
    lines = ['// Generated by tools/prompt_icons.py from app/prompts/switch (Xelu, CC0). Do not edit.',
             '#pragma once', '#include <cstdint>', '', 'namespace cott_prompt_icons_data {', '',
             'struct Page {', '  uint16_t character;  // 0: a texture of its own, not a font glyph', '  const char* button;', '  uint32_t width, height;',
             '  uint64_t game_hash, our_hash;', '  const uint8_t* indices;  // width x height, bottom row first',
             '  const uint8_t* palette;  // 256 x (A, R, G, B)', '};', '']
    entries = []
    for char, button, picture, width, height, visible_h, game_hash, *extra in BUTTONS:
        img = page(picture, width, height, visible_h)
        if 'no' in extra:
            img = no_sign(img)
        indices, palette = quantize(img)
        rows = [indices[y * width:(y + 1) * width] for y in range(height)]
        if 'top down' not in extra:
            rows.reverse()  # the game's pages are stored bottom up
        flat = [b for row in rows for b in row]
        pal = [c for r, g, b, a in palette for c in (a, r, g, b)]
        our_hash = fnv(bytes(flat))
        name = 'k%02X' % char if char else 'k' + ''.join(c for c in button.title() if c.isalnum())
        lines.append('inline constexpr uint8_t %sIndices[%d] = {%s};' % (name, len(flat), ','.join(map(str, flat))))
        lines.append('inline constexpr uint8_t %sPalette[1024] = {%s};' % (name, ','.join(map(str, pal))))
        entries.append('    {0x%02X, "%s", %d, %d, 0x%016XULL, 0x%016XULL, %sIndices, %sPalette},' %
                       (char, button, width, height, game_hash, our_hash, name, name))
        print('%04X %-10s <- %-22s %dx%d' % (char, button, picture, width, height))
    lines += ['', 'inline constexpr Page kPages[] = {'] + entries + ['};', '', '}  // namespace cott_prompt_icons_data', '']
    open(OUT, 'w').write('\n'.join(lines))
    print('->', OUT)


if __name__ == '__main__':
    main()
