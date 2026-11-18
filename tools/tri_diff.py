#!/usr/bin/env python3
# tri_diff.py a.png b.png [c.png...] : crop each to its ink box (non-white),
# align top-left (with a +-2 px search), report pairwise mean |dRGB|.
import sys, zlib, struct, itertools, os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'color_diff.py')).read().split('w, h, A = load')[0])
def crop(p):
    w, h, R = load(p)
    pts = [(x, y) for y in range(h) for x in range(w) if min(R[y][x]) < 245]
    x0 = min(x for x, _ in pts); y0 = min(y for _, y in pts)
    x1 = max(x for x, _ in pts); y1 = max(y for _, y in pts)
    return [row[x0:x1 + 1] for row in R[y0:y1 + 1]]
imgs = {p: crop(p) for p in sys.argv[1:]}
def diff(A, B):
    best = None
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            ds = []
            for y in range(len(A)):
                yb = y + dy
                if not 0 <= yb < len(B): continue
                for x in range(len(A[0])):
                    xb = x + dx
                    if not 0 <= xb < len(B[0]): continue
                    ds.append(sum(abs(p - q) for p, q in zip(A[y][x], B[yb][xb])) / 3)
            m = sum(ds) / max(len(ds), 1)
            if best is None or m < best[0]: best = (m, dx, dy)
    return best
for a, b in itertools.combinations(sys.argv[1:], 2):
    m, dx, dy = diff(imgs[a], imgs[b])
    print(f'{a.split("/")[-1]:>8} vs {b.split("/")[-1]:<8} mean |dRGB| {m:6.2f}  (shift {dx},{dy})  sizes {len(imgs[a][0])}x{len(imgs[a])} {len(imgs[b][0])}x{len(imgs[b])}')
