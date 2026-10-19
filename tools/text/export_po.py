#!/usr/bin/env python3
"""Writes the game's English texts as a gettext catalog (.po) to translate them.

    python3 tools/text/export_po.py [--rcf assets/game_root/default.rcf] [--lang pt_BR]
                                    [--out translations/work/pt_BR.po]

One entry per distinct English text (msgid), translated in msgstr: the game finds the translation by its
English text (tools/text/textbible.py), so a text repeated in several banks is translated once. The
"#:" lines say where it is (bank:hash), "#. es:" gives the Spanish text for context. Button pictures are
written as {A}, {B}, {START}...; keep them. Any .po editor works (Poedit, Lokalize, Weblate...). Texts
with no letters (a lone button picture) are left out.

If the output exists, its translations are kept (by msgid); texts no longer in the game are dropped and
counted.

The .po holds the game's English text, so it is game data: translations/work/ is in .gitignore.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textbible  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def quote(text):
    text = text.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
    lines = text.split('\n')
    if len(lines) == 1:
        return '"%s"' % lines[0]
    return '""\n' + '\n'.join('"%s\\n"' % line for line in lines[:-1]) + '\n"%s"' % lines[-1]


def unquote(lines):
    text = ''.join(line.strip()[1:-1] for line in lines)
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text):
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\'}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def read_po(path):
    """{msgid: msgstr} of an existing catalog."""
    entries = {}
    if not os.path.exists(path):
        return entries
    fields, current = {}, None

    def flush():
        msgid = unquote(fields.get('msgid', []))
        if msgid:
            entries[msgid] = unquote(fields.get('msgstr', []))
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip():
                flush()
                fields, current = {}, None
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
    parser.add_argument('--rcf', default=os.path.join(ROOT, 'assets', 'game_root', 'default.rcf'))
    parser.add_argument('--lang', default='pt_BR')
    parser.add_argument('--out')
    args = parser.parse_args()
    out = args.out or os.path.join(ROOT, 'translations', 'work', args.lang + '.po')

    strings = textbible.read_strings(args.rcf)
    spanish = {(package, bank, h): text for package, bank, h, code, text in strings if code == 'S'}
    entries = {}  # English text -> ([bank:hash], Spanish)
    for package, bank, h, code, text in strings:
        if code != 'E' or not any(c.isalpha() for c in text):
            continue
        where, es = entries.setdefault(text, ([], spanish.get((package, bank, h))))
        if '%s:%04X' % (bank, h) not in where:
            where.append('%s:%04X' % (bank, h))
    old = read_po(out)

    os.makedirs(os.path.dirname(out), exist_ok=True)
    kept = 0
    with open(out, 'w', encoding='utf-8') as f:
        f.write('# Crash of the Titans (cott-nx): the game\'s texts, to translate. Made by tools/text/export_po.py.\n'
                '# Game data (the English text): never commit this file.\n'
                'msgid ""\nmsgstr ""\n"Project-Id-Version: cott-nx\\n"\n"MIME-Version: 1.0\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Content-Transfer-Encoding: 8bit\\n"\n"Language: %s\\n"\n\n' % args.lang)
        for text, (where, es) in entries.items():
            msgid = textbible.to_tags(text)
            msgstr = old.get(msgid, '')
            kept += bool(msgstr)
            f.write('#: %s\n' % ' '.join(where[:6] + (['...'] if len(where) > 6 else [])))
            if es and es != text:
                for line in textbible.to_tags(es).split('\n'):
                    f.write('#. es: %s\n' % line)
            f.write('msgid %s\nmsgstr %s\n\n' % (quote(msgid), quote(msgstr)))
    words = sum(len(text.split()) for text in entries)
    print('%d texts (%d words) -> %s' % (len(entries), words, out))
    if old:
        dropped = sum(1 for msgid, msgstr in old.items() if msgstr and textbible.from_tags(msgid) not in entries)
        print('%d translations kept; %d dropped (their English is no longer in the game)' % (kept, dropped))


if __name__ == '__main__':
    main()
