#!/usr/bin/env python3
# ink_cols.py img.png : per connected column-run (glyph), ink x-range and
# y-range (rows with non-background pixels). Colour-aware (any channel).
import sys, zlib, struct
def load(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; c = d[i+8:i+8+n]
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT': idat += c
        i += 12 + n
    raw = zlib.decompress(idat); ch = {6: 4, 2: 3, 0: 1, 4: 2}[ct]; stride = w * ch
    rows = []; prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1 + stride
        for x in range(stride):
            a = line[x-ch] if x >= ch else 0; b = prev[x]; c = prev[x-ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([min(line[x*ch:x*ch+3]) < 200 for x in range(w)]); prev = line
    return w, h, rows
w, h, R = load(sys.argv[1])
cols = [any(R[y][x] for y in range(h)) for x in range(w)]
x = 0; out = []
while x < w:
    if not cols[x]: x += 1; continue
    x0 = x
    while x < w and cols[x]: x += 1
    ys = [y for y in range(h) if any(R[y][xx] for xx in range(x0, x))]
    out.append(f'[{x0}-{x-1} y{min(ys)}-{max(ys)}]')
print(' '.join(out))
