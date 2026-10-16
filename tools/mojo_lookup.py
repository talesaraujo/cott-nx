#!/usr/bin/env python3
"""Looks up guest addresses in MojoRecomp's function map (research reference, ISC licensed).

    python3 tools/mojo_lookup.py 0x823F5880 [...]
    python3 tools/mojo_lookup.py --missing app/generated/default   # Mojo starts that our codegen lacks

MojoRecomp's config lists ~19,800 function starts and sizes for the same default.xex (SHA-256
e76824b2...6373). It is not shipped here: point --map at refs/MojoRecomp/config/CrashOfTheTitans.toml.
"""
import argparse, bisect, os, re, sys

DEFAULT_MAP = os.path.join(os.path.dirname(__file__), '..', '..', 'refs', 'MojoRecomp', 'config', 'CrashOfTheTitans.toml')


def load(path):
    text = open(path).read()
    return sorted((int(a, 16), int(s, 16)) for a, s in
                  re.findall(r'address = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', text))


def ours(gen_dir):
    starts = set()
    for name in os.listdir(gen_dir):
        if name.endswith('.cpp'):
            for line in open(os.path.join(gen_dir, name), errors='replace'):
                m = re.match(r'DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)', line)
                if m:
                    starts.add(int(m.group(1), 16))
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='*')
    ap.add_argument('--map', default=DEFAULT_MAP)
    ap.add_argument('--missing', metavar='GEN_DIR')
    a = ap.parse_args()
    fs = load(a.map)
    starts = [f[0] for f in fs]
    if a.missing:
        have = ours(a.missing)
        miss = [f for f in fs if f[0] not in have]
        print(f'{len(fs)} Mojo starts, {len(have)} ours, {len(miss)} Mojo starts missing in ours')
        for addr, size in miss:
            print(f'0x{addr:08X} size 0x{size:X}')
        return
    for s in a.addrs:
        t = int(s, 16)
        i = bisect.bisect_right(starts, t) - 1
        if i >= 0 and fs[i][0] <= t < fs[i][0] + fs[i][1]:
            addr, size = fs[i]
            where = 'starts a function' if addr == t else f'inside 0x{addr:08X} (+0x{t - addr:X})'
            print(f'0x{t:08X}: {where}, size 0x{size:X}')
        else:
            print(f'0x{t:08X}: not in the map')


if __name__ == '__main__':
    sys.exit(main())
