#!/usr/bin/env python3
"""Turns a translated .po (tools/text/export_po.py) into the file the game reads.

    python3 tools/text/compile_po.py [--po translations/work/pt_BR.po] [--out translations/pt_BR.txt]

translations/pt_BR.txt: one line per translated text, "<key>\\t<translation>", the key being the
64-bit FNV-1a of the English text (tools/text/textbible.py, text_key), in hex. Line breaks are written
\\n, tabs \\t, backslashes \\\\; button pictures stay as {A}, {START}... . It holds no English, only the
translation, and is built into the executable (app/CMakeLists.txt), which loads it with
cott_language = pt-BR (app/src/cott_language.cpp). Untranslated and fuzzy entries are left out (the game
shows the English).

The other way, translations already in pt_BR.txt go into a new .po: export_po.py reads it.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textbible  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read_po_entries(path):
    """[(msgid, msgstr, fuzzy)] of a catalog."""
    from export_po import unquote
    entries, fields, current, fuzzy = [], {}, None, False

    def flush():
        msgid = unquote(fields.get('msgid', []))
        if msgid:
            entries.append((msgid, unquote(fields.get('msgstr', [])), fuzzy))
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip():
                flush()
                fields, current, fuzzy = {}, None, False
            elif line.startswith('#,'):
                fuzzy = fuzzy or 'fuzzy' in line
            elif line.startswith('#'):
                continue
            elif line.startswith('"'):
                if current:
                    fields[current].append(line)
            else:
                current, _, rest = line.partition(' ')
                fields[current] = [rest]
    flush()
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--po', default=os.path.join(ROOT, 'translations', 'work', 'pt_BR.po'))
    parser.add_argument('--out', default=os.path.join(ROOT, 'translations', 'pt_BR.txt'))
    args = parser.parse_args()

    lines, fuzzy, missing_tags = [], 0, []
    for msgid, msgstr, is_fuzzy in read_po_entries(args.po):
        if not msgstr:
            continue
        if is_fuzzy:
            fuzzy += 1
            continue
        tags = sorted(t for t in textbible.PICTURES if '{%s}' % t in msgid)
        if tags != sorted(t for t in textbible.PICTURES if '{%s}' % t in msgstr):
            missing_tags.append(msgid)
        key = textbible.text_key(textbible.from_tags(msgid))
        lines.append('%016X\t%s' % (key, textbible.escape(msgstr)))
    lines.sort()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Crash of the Titans (cott-nx), Brazilian Portuguese. Made by tools/text/compile_po.py from a .po\n'
                '# (tools/text/export_po.py). <FNV-1a 64 of the English text>\\t<translation>; {A}... = buttons.\n')
        f.write('\n'.join(lines) + '\n')
    print('%d translations -> %s (%d fuzzy left out)' % (len(lines), args.out, fuzzy))
    for msgid in missing_tags:
        print('warning: the button pictures differ from the English in: %s' % msgid[:70], file=sys.stderr)


if __name__ == '__main__':
    main()
