#!/usr/bin/env python3
"""Makes the homebrew menu icon (tools/switch/icon/cott_icon.jpg, 256x256) from the game's own art.

    python3 tools/switch/make_icon.py [assets/game_root/default.rcf]      (needs Pillow)

The title logo (the sprite FE_Title_Logo.tga, 480x317 in six PNG pieces) goes over a darkened, blurred
square of the menu's jungle concept art (the sprite FE_Concept_04.tga, 640x480 in eight pieces). Both are read from default.rcf, so the
icon is game data: tools/switch/icon/ is in .gitignore and the icon is never committed. Without it the NRO
gets libnx's default icon (app/CMakeLists.txt).
"""
import io
import mmap
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RCF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'assets', 'game_root', 'default.rcf')
OUT = os.path.join(ROOT, 'tools', 'switch', 'icon', 'cott_icon.jpg')
SPRITE = 0x19005  # Pure3D chunk id
SIZE = 256


def sprite(m, name):
    """The picture of the Pure3D sprite `name`, put together from its PNG pieces."""
    key = name.encode() + b'\0'
    i = m.find(key)
    while i >= 0:
        # The chunk header (id, data size, total size) is followed by the name's length byte and the name,
        # then the sprite's size, its content's size (x256) and more; then the pieces, in rows.
        start = i - 1 - 12
        if start >= 0 and struct.unpack_from('<I', m, start)[0] == SPRITE:
            end = start + struct.unpack_from('<I', m, start + 8)[0]
            content_w, content_h = (v // 256 for v in struct.unpack_from('<2I', m, i + m[i - 1] + 8))
            pieces, j = [], m.find(b'\x89PNG', start, end)
            while j >= 0:
                e = m.find(b'IEND', j, end) + 8
                pieces.append(Image.open(io.BytesIO(m[j:e])).convert('RGBA'))
                j = m.find(b'\x89PNG', e, end)
            # Every row has the same widths: the shortest repeating run of widths that covers the content
            # (the last pieces of a row or the last row can stick out; they are cut).
            widths = [p.width for p in pieces]
            per_row = next(n for n in range(1, len(pieces) + 1)
                           if len(pieces) % n == 0 and widths == widths[:n] * (len(pieces) // n) and
                           sum(widths[:n]) >= content_w)
            picture = Image.new('RGBA', (content_w, content_h), (0, 0, 0, 0))
            y = 0
            for r in range(0, len(pieces), per_row):
                x = 0
                for p in pieces[r:r + per_row]:
                    picture.paste(p, (x, y))
                    x += p.width
                y += pieces[r].height
            return picture
        i = m.find(key, i + 1)
    sys.exit('%s not found in %s' % (name, RCF))


def main():
    with open(RCF, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        logo = sprite(m, 'FE_Title_Logo.tga')
        jungle = sprite(m, 'FE_Concept_04.tga')
    logo = logo.crop(logo.getbbox())

    # The background: a square of the concept art (640x480) inside its frame, around the big tree, blurred,
    # darker, with a vignette.
    bg = jungle.crop((84, 40, 484, 440)).resize((SIZE, SIZE), Image.LANCZOS)
    bg = ImageEnhance.Brightness(bg.filter(ImageFilter.GaussianBlur(1.5))).enhance(0.8)
    mask = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse((-60, -60, SIZE + 60, SIZE + 60), fill=255)
    bg = Image.composite(bg, Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 255)), mask.filter(ImageFilter.GaussianBlur(40)))

    # The logo with a soft shadow.
    w = SIZE - 20
    h = round(logo.height * w / logo.width)
    logo = logo.resize((w, h), Image.LANCZOS)
    shadow = Image.new('RGBA', logo.size, (0, 0, 0, 0))
    shadow.putalpha(logo.getchannel('A').point(lambda a: int(a * 0.7)))
    x, y = (SIZE - w) // 2, (SIZE - h) // 2 + 4
    bg.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(5)), (x + 3, y + 5))
    bg.alpha_composite(logo, (x, y))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bg.convert('RGB').save(OUT, quality=92)
    print('->', OUT)


if __name__ == '__main__':
    main()
