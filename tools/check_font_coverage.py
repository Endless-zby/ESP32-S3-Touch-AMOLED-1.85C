#!/usr/bin/env python3
"""Check which characters of the given strings are covered by an LVGL font .c file."""
import re, sys

def covered_set(path):
    src = open(path, encoding='utf-8', errors='ignore').read()
    lists = {}
    for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};', src, re.S):
        vals = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(2))]
        lists[m.group(1)] = vals
    cov = set()
    cm = re.search(r'cmaps\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not cm:
        raise SystemExit('no cmaps found')
    for ent in re.finditer(r'\{\s*\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*\.glyph_id_start\s*=\s*(\d+),\s*\.unicode_list\s*=\s*([A-Za-z0-9_]+),\s*\.glyph_id_ofs_list\s*=\s*([A-Za-z0-9_]+),\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*([A-Z_0-9]+)', cm.group(1)):
        rs, rl, gs, ul, gol, ll, typ = ent.groups()
        rs, rl = int(rs), int(rl)
        if 'SPARSE' in typ:
            for v in lists.get(ul, []):
                cov.add(rs + v)
        else:
            for i in range(rl):
                cov.add(rs + i)
    return cov

if __name__ == '__main__':
    font = sys.argv[1]
    cov = covered_set(font)
    print('glyphs covered:', len(cov))
    missing = set()
    for s in sys.argv[2:]:
        for ch in s:
            if ord(ch) > 127 and ord(ch) not in cov:
                missing.add(ch)
    print('missing:', ''.join(sorted(missing)) if missing else '(none)')
