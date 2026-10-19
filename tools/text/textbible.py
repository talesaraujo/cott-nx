"""Reads the game's texts from default.rcf: the Pure3D TextBibles and their Languages.

THE ARCHIVE
  default.rcf ("ATG CORE CEMENT LIBRARY"): at 0x24, big-endian, the directory's offset and size and the
  names' offset and size. The directory is (hash, offset, size) per file, 12 bytes each, sorted by hash;
  the names come in the order of the files' offsets (u32 time, u32 alignment, u32 0, u32 length with the
  ending 0, the name, 3 bytes of padding; after an 8-byte header).

THE TEXTS
  A TextBible (chunk 0x1800D) is one text bank: bootup, frontend, persistent, shared, ingame, save_load.
  Several packages carry their own copy of a bank, and the copies are not all the same: every level's
  package has its own "ingame", some carry the Wii's or the PS2's messages, some older wordings. Inside
  a bank, one Language (chunk 0x1800E) per language, each with its strings: a hash per string (unique
  only inside its bank), offsets into one UTF-16 buffer, every string ended by a 0 character.

  Chunk header (little-endian): id, size of the chunk's own data (header included), size with its
  sub-chunks. TextBible data: the name (a length byte, then the name, 0-padded). Language data: the name
  the same way, the language code (one byte: E, F, G, I, S, ...), the string count, one more u32, the
  buffer size in bytes, the hashes, the offsets in bytes, the buffer.

  So a translation is found by the English text itself (text_key), as gettext does: whatever copy the
  game loads, the same English gets the same translation.

These are the game's own texts: whatever is read here stays out of the repository.
"""
import bisect
import mmap
import struct

TEXT_BIBLE = 0x1800D
LANGUAGE = 0x1800E

# The button pictures of the game's fonts (U+00B1..U+00BE), as tags for translators. The game draws them
# as the Switch's buttons (app/src/cott_prompt_icons.cpp).
PICTURES = ['A', 'B', 'X', 'Y', 'BACK', 'START', 'RT', 'RB', 'LSTICK', 'LEFT', 'RIGHT', 'ROTATE', 'LT', 'LB']
FIRST_PICTURE = 0xB1


def to_tags(text):
    """The game's string with its button pictures as {A}, {START}... ."""
    out = []
    for c in text:
        k = ord(c) - FIRST_PICTURE
        out.append('{%s}' % PICTURES[k] if 0 <= k < len(PICTURES) else c)
    return ''.join(out)


def from_tags(text):
    """The other way: {A} -> the picture character."""
    for k, name in enumerate(PICTURES):
        text = text.replace('{%s}' % name, chr(FIRST_PICTURE + k))
    return text


def text_key(text):
    """64-bit FNV-1a of the string's UTF-16 code units, as the game holds it (pictures as characters).
    app/src/cott_language.cpp computes the same."""
    h = 0xCBF29CE484222325
    for unit in struct.unpack('<%dH' % (len(text.encode('utf-16-le')) // 2), text.encode('utf-16-le')):
        h ^= unit
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def _name(m, p):
    n = m[p]
    return m[p + 1:p + 1 + n].split(b'\0')[0].decode('latin-1'), p + 1 + n


def files(m):
    """[(name, offset, size)] of the archive, by offset."""
    dir_off, dir_size, names_off, names_size = struct.unpack_from('>4I', m, 0x24)
    count = dir_size // 12
    entries = sorted((struct.unpack_from('>III', m, dir_off + 12 * i) for i in range(count)),
                     key=lambda e: e[1])
    names, p = [], names_off + 8
    while len(names) < count:
        length = struct.unpack_from('<I', m, p + 12)[0]
        names.append(m[p + 16:p + 15 + length].decode('latin-1'))
        p += 16 + length + 3
    return [(name, offset, size) for name, (_, offset, size) in zip(names, entries)]


def read_strings(rcf_path):
    """Every string of every bank: [(package, bank, hash, language code, text)], in the archive's order."""
    out = []
    with open(rcf_path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        listing = files(m)
        starts = [offset for _, offset, _ in listing]
        pattern = struct.pack('<I', TEXT_BIBLE)
        i = m.find(pattern)
        while i >= 0:
            try:
                data, total = struct.unpack_from('<II', m, i + 4)
                if 12 <= data < 4096 and data <= total < 50_000_000:
                    k = bisect.bisect_right(starts, i) - 1
                    package = listing[k][0] if k >= 0 else '?'
                    bank, _ = _name(m, i + 12)
                    j, end = i + data, i + total
                    while j + 12 <= end:
                        chunk, _, chunk_total = struct.unpack_from('<III', m, j)
                        if chunk != LANGUAGE or chunk_total < 12:
                            break
                        _, p = _name(m, j + 12)
                        code = chr(m[p])
                        count, _, size = struct.unpack_from('<III', m, p + 1)
                        p += 13
                        hashes = struct.unpack_from('<%dI' % count, m, p)
                        offsets = struct.unpack_from('<%dI' % count, m, p + 4 * count)
                        buffer = m[p + 8 * count:p + 8 * count + size]
                        for h, o in zip(hashes, offsets):
                            e = o
                            while e + 1 < len(buffer) and (buffer[e] or buffer[e + 1]):
                                e += 2
                            out.append((package, bank, h, code, buffer[o:e].decode('utf-16-le', 'replace')))
                        j += chunk_total
            except struct.error:
                pass
            i = m.find(pattern, i + 1)
    return out
