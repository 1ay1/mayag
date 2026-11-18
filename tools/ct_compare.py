#!/usr/bin/env python3
# ct_compare.py CASEFILE [MGS] : compare mayag (absolute positions) vs CoreText.
# Build the oracle once:
#   clang -O1 -framework CoreText -framework CoreFoundation -framework CoreGraphics \
#         tools/ct_shape.c -o /tmp/ct_shape        (or set CT_SHAPE)
# and mg_shape (tools/mg_shape.cpp) to /tmp/mgs. Lines marked
# '[CT used fallback font]' are not comparable (CoreText swapped the face).
import subprocess, sys, re, os
CT_SHAPE = os.environ.get('CT_SHAPE', '/tmp/ct_shape')
cases = sys.argv[1]
mgs = sys.argv[2] if len(sys.argv) > 2 else '/tmp/mgs'
tol = 1.5
import struct
def upem_of(p, idx):
    d = open(p, 'rb').read(128 * 1024)
    o = struct.unpack('>I', d[12 + 4 * idx:16 + 4 * idx])[0] if d[:4] == b'ttcf' else 0
    d = open(p, 'rb').read(o + 12 + 16 * 64)
    n = struct.unpack('>H', d[o + 4:o + 6])[0]
    for i in range(n):
        t, c, off, l = struct.unpack('>4sIII', d[o + 12 + 16 * i:o + 28 + 16 * i])
        if t == b'head':
            with open(p, 'rb') as f:
                f.seek(off + 18); return struct.unpack('>H', f.read(2))[0]
    return 1000
def mg_abs(out):
    out = out.strip()
    if not out.startswith('['): return None, out
    pen = 0; glyphs = []
    for item in out[1:-1].split('|'):
        m = re.match(r'(\d+)=(\d+)(?:@(-?\d+),(-?\d+))?\+(-?\d+)', item)
        g, _, xo, yo, adv = m.groups()
        xo = int(xo or 0); yo = int(yo or 0)
        glyphs.append((int(g), pen + xo, yo)); pen += int(adv)
    return glyphs, pen
def ct_abs(out):
    out = out.strip()
    body, w = out.rsplit('|', 1)
    fb = '[fallback]' in body
    glyphs = []
    for tok in body.replace('[fallback]', '').split():
        g, xy = tok.split('@'); x, y = xy.split(',')
        glyphs.append((int(g), float(x), float(y)))
    return glyphs, float(w), fb
npass = nfail = 0
for line in open(cases, encoding='utf-8'):
    line = line.rstrip('\n')
    if not line or line.startswith('#'): continue
    font, text, extra = (line.split('|') + ['', ''])[:3]
    if extra.strip(): continue  # CT can't take hb feature args
    path, idx = (font.rsplit(':', 1) + ['0'])[:2] if re.search(r':\d+$', font) else (font, '0')
    ct = subprocess.run([CT_SHAPE, path, idx, text], capture_output=True, text=True).stdout
    mg_args = [mgs, f'{path}:{idx}', text]
    mg = subprocess.run(mg_args, capture_output=True, text=True).stdout
    try:
        cg, cw, fb = ct_abs(ct)
    except Exception:
        print('CTERR', font, text, ct); continue
    mgl, mw = mg_abs(mg)
    # Drop CT's deleted/placeholder glyphs (0xFFFF) — they have no ink.
    cg = [x for x in cg if x[0] != 0xFFFF]
    same = lambda a, b: a[0] == b[0] and abs(a[1] - b[1]) <= tol and abs(a[2] - b[2]) <= tol
    pos_ok = mgl is not None and len(cg) == len(mgl) and all(same(a, b) for a, b in zip(cg, mgl))
    # CoreText draws uncomposed marks in INPUT order; mayag in canonical
    # order (Unicode C6). Accept the same glyph placements in either order.
    if not pos_ok and mgl is not None and len(cg) == len(mgl):
        rest = list(mgl)
        for a in cg:
            k = next((i for i, b in enumerate(rest) if same(a, b)), None)
            if k is None: break
            rest.pop(k)
        pos_ok = not rest
    # CTLine's width counts a trailing mark's overhang; the pen does not.
    trailing_mark = mg.strip().endswith('+0]')
    ok = pos_ok and (abs(cw - mw) <= tol * 2 or (trailing_mark and cw > mw))
    if ok: npass += 1; continue
    npass += 0; nfail += 1
    print(f'FAIL {font} | {text}' + ('  [CT used fallback font]' if fb else ''))
    print('  ct:', ' '.join(f'{g}@{x:.0f},{y:.0f}' for g, x, y in cg), '|', f'{cw:.0f}')
    print('  mg:', ' '.join(f'{g}@{x},{y}' for g, x, y in (mgl or [])), '|', mw)
print(f'pass {npass} fail {nfail}')
