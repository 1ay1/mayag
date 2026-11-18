#!/usr/bin/env python3
# cascade_sweep.py : compare the font mayag resolves vs CoreText's cascade for
# a sample of codepoints from every block in use. Prints mismatches grouped.
# Needs /tmp/ct_cascade (clang tools/ct_cascade.c -framework CoreText
# -framework CoreFoundation) and /tmp/mg_resolve (a FontStack::resolve dump).
import subprocess, collections, unicodedata, sys
cps = []
blocks = [(0x20, 0x24F, 7), (0x250, 0x2FF, 9), (0x370, 0x3FF, 5), (0x400, 0x52F, 9), (0x531, 0x58F, 5),
          (0x591, 0x5FF, 5), (0x600, 0x6FF, 5), (0x700, 0x74F, 7), (0x780, 0x7BF, 7), (0x900, 0xDFF, 6),
          (0xE01, 0xEFF, 5), (0xF00, 0xFFF, 17), (0x1000, 0x109F, 9), (0x10A0, 0x10FF, 9), (0x1100, 0x11FF, 23),
          (0x1200, 0x139F, 31), (0x13A0, 0x13FF, 11), (0x1400, 0x167F, 53), (0x1780, 0x17FF, 9), (0x1800, 0x18AF, 11),
          (0x1E00, 0x1FFF, 13), (0x2000, 0x2BFF, 7), (0x2E80, 0x2FDF, 17), (0x3000, 0x30FF, 5), (0x3100, 0x31FF, 11),
          (0x3200, 0x33FF, 13), (0x3400, 0x4DBF, 211), (0x4E00, 0x9FFF, 311), (0xA000, 0xA4CF, 41), (0xAC00, 0xD7AF, 257),
          (0xF900, 0xFAFF, 31), (0xFB00, 0xFDFF, 9), (0xFE30, 0xFE4F, 5), (0xFF00, 0xFFEF, 7), (0x1D400, 0x1D7FF, 37),
          (0x1F300, 0x1FAFF, 41), (0x20000, 0x2A6DF, 997)]
for lo, hi, step in blocks:
    for c in range(lo, hi + 1, step):
        ch = chr(c)
        if unicodedata.category(ch) in ('Cn', 'Co', 'Cs', 'Cc', 'Zl', 'Zp'): continue
        cps.append(c)
hexs = ['%X' % c for c in cps]
def run(cmd):
    out = {}
    for i in range(0, len(hexs), 400):
        r = subprocess.run(cmd + hexs[i:i + 400], capture_output=True, text=True).stdout
        for line in r.splitlines():
            p = line.split('\t')
            if len(p) >= 2: out[int(p[0], 16)] = p[1:]
    return out
ct = run(['/tmp/ct_cascade'])
mg = run(['/tmp/mg_resolve'])
norm = lambda s: s.lower().replace(' ', '').replace('.', '').replace('ui', '')
same = 0; subst = 0; diff = collections.Counter(); examples = {}
for c in cps:
    cf = ct.get(c, ['?', '?'])[1]; mf = mg.get(c, ['?'])[0]
    if cf in ('.LastResort', 'LastResort'):
        if mf == '(none)': same += 1; continue
    if norm(cf) == norm(mf) or (cf == '.AppleSystemUIFont' and mf == 'System Font'): same += 1; continue
    if cf.startswith('.PingFang') and mf.startswith('Heiti'): subst += 1; continue
    key = (cf, mf); diff[key] += 1; examples.setdefault(key, []).append(c)
total = same + subst + sum(diff.values())
print(f'{same}/{total} codepoints resolve to the same family as CoreText, {subst} more to Heiti in place of PingFang (unreadable hvgl)')
for (cf, mf), n in diff.most_common(int(sys.argv[1]) if len(sys.argv) > 1 else 40):
    ex = ' '.join('%X' % c for c in examples[(cf, mf)][:4])
    print(f'{n:5d}  CT {cf:38s} mg {mf:32s} e.g. {ex}')
