#!/usr/bin/env python3
# color_diff.py a.png b.png : per-pixel RGB difference over ink (either image
# differs from the background), with a +-1 px shift search.
import sys, zlib, struct
def load(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; c = d[i+8:i+8+n]
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT': idat += c
        i += 12 + n
    raw = zlib.decompress(idat); ch = {6: 4, 2: 3}[ct]; st = w * ch; rows = []; prev = bytearray(st); p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p+1:p+1+st]); p += 1 + st
        for x in range(st):
            a = line[x-ch] if x >= ch else 0; b = prev[x]; c = prev[x-ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x*ch:x*ch+3]) for x in range(w)]); prev = line
    return w, h, rows
w, h, A = load(sys.argv[1]); _, _, B = load(sys.argv[2])
xmax = int(sys.argv[3]) if len(sys.argv) > 3 else w
best = None
for dy in (-1, 0, 1):
    for dx in (-1, 0, 1):
        ds = []
        for y in range(1, h - 1):
            for x in range(1, min(w, xmax) - 1):
                a = A[y][x]; b = B[y + dy][x + dx]
                if min(a) < 245 or min(b) < 245: ds.append(sum(abs(p - q) for p, q in zip(a, b)) / 3)
        if not ds: continue
        m = sum(ds) / len(ds)
        if best is None or m < best[0]:
            ds.sort(); best = (m, dx, dy, ds[len(ds) // 2], ds[int(len(ds) * 0.95)], len(ds))
m, dx, dy, p50, p95, n = best
print(f'shift ({dx},{dy}) ink px {n} mean |dRGB| {m:.2f} p50 {p50:.1f} p95 {p95:.1f}')
