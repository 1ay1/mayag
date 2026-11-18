#!/usr/bin/env python3
# Derive a flat font order that reproduces CoreText's per-codepoint fallback
# ("first font in the list whose cmap covers the codepoint"), from ct_all.txt.
import collections, json, sys
from fontTools.ttLib import TTFont, TTCollection
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[2] if len(sys.argv) > 2 else '/tmp/ct_all.txt', encoding='utf-8')]
# PingFang UI is stored in Apple's undocumented `hvgl` outline format, which
# nothing outside Apple reads (HarfBuzz/FreeType included). Its predecessor,
# Heiti (STHeiti SC Light, glyf), is the same sans design class and covers
# 98.4% of what CoreText draws from PingFang SC: put it in PingFang's slot.
SUBST = {'.PingFangUITextSC-Regular': ('/System/Library/Fonts/STHeiti Light.ttc', 1, 'STHeitiSC-Light'),
         '.PingFangUITextTC-Regular': ('/System/Library/Fonts/STHeiti Light.ttc', 0, 'STHeitiTC-Light'),
         '.PingFangUITextHK-Regular': ('/System/Library/Fonts/STHeiti Light.ttc', 0, 'STHeitiTC-Light')}
orig_pf = collections.defaultdict(set)
for r in rows:
    if len(r) == 4 and r[3] in SUBST: orig_pf[SUBST[r[3]][2]].add(int(r[0], 16))
rows = [[r[0], *SUBST[r[3]]] if len(r) == 4 and r[3] in SUBST else r for r in rows]
choice = {}
# CoreText reports a face index into the file's DESCRIPTOR list, which for a
# variable .ttc counts named instances. Map each (path, ps) to the real face
# whose PostScript family matches.
fam = lambda ps: ps.rsplit('-', 1)[0] if '-' in ps else ps
face_of = {}
def real_face(path, ps):
    key = (path, fam(ps))
    if key in face_of: return face_of[key]
    found = None
    try:
        if path.endswith(('.ttc', '.otc')):
            n = len(TTCollection(path, lazy=True).fonts)
        else:
            n = 1
        for i in range(n):
            t = TTFont(path, fontNumber=i, lazy=True) if n > 1 else TTFont(path, lazy=True)
            if fam(t['name'].getDebugName(6) or '') == fam(ps): found = i; break
    except Exception:
        pass
    face_of[key] = found
    return found
for cp, path, idx, ps in rows:
    if ps != 'LastResort' and path:
        ri = real_face(path, ps)
        if ri is not None: idx = ri
    choice[int(cp, 16)] = (path, int(idx), ps)
fonts = sorted({v for v in choice.values() if v[2] != 'LastResort' and v[0]})
cmaps = {}
for path, idx, ps in fonts:
    try:
        f = TTFont(path, fontNumber=idx, lazy=True) if path.endswith(('.ttc', '.otc')) else TTFont(path, lazy=True)
        cm = set()
        for t in f['cmap'].tables:
            if t.isUnicode() or t.platformID == 3: cm |= set(t.cmap.keys())
        cmaps[(path, idx, ps)] = cm
    except Exception as e:
        print('skip', path, idx, e, file=sys.stderr)
fonts = [f for f in fonts if f in cmaps]
# Faces with no outline format mayag reads (Apple's undocumented `hvgl`, e.g.
# PingFang UI) cannot be used; leave them out and let the order solve for
# the best renderable substitute.
renderable = {}
for f in fonts:
    path, idx, ps = f
    t = TTFont(path, fontNumber=idx, lazy=True) if path.endswith(('.ttc', '.otc')) else TTFont(path, lazy=True)
    renderable[f] = any(k in t for k in ('glyf', 'CFF ', 'CFF2', 'sbix', 'CBDT', 'COLR'))
unusable = [f[2] for f in fonts if not renderable[f]]
if unusable: print('unrenderable (left out):', ', '.join(unusable), file=sys.stderr)
fonts = [f for f in fonts if renderable[f]]
# A substitute may only serve what CoreText drew from PingFang.
for f in fonts:
    if f[2] in orig_pf: cmaps[f] = cmaps[f] & orig_pf[f[2]]
def ranges(cps):
    out = []
    for c in sorted(cps):
        if out and c == out[-1][1] + 1: out[-1][1] = c
        else: out.append([c, c])
    return out
json.dump({ps: ranges(c) for ps, c in orig_pf.items()}, open('/tmp/ct_limits.json', 'w'))
# wins[a][b] = # codepoints where CT chose a though b also covers it
wins = collections.defaultdict(collections.Counter)
for cp, a in choice.items():
    if a not in cmaps: continue
    for b in fonts:
        if b != a and cp in cmaps[b]: wins[a][b] += 1
# order: greedy -- repeatedly take the font with the fewest weighted losses to
# the remaining fonts (a weighted feedback-arc-set heuristic)
remaining = set(fonts); order = []
sf = next(f for f in fonts if f[2] == '.SFNS-Regular')
order.append(sf); remaining.remove(sf)
while remaining:
    best = min(remaining, key=lambda a: (sum(wins[b][a] for b in remaining if b != a) - sum(wins[a][b] for b in remaining if b != a)))
    order.append(best); remaining.remove(best)
# emulate
def score(order):
    ok = bad = 0; miss = collections.Counter()
    for cp, a in choice.items():
        got = next((f for f in order if cp in cmaps[f]), ('', 0, 'LastResort'))
        if got[2] == a[2]: ok += 1
        else: bad += 1; miss[(a[2], got[2])] += 1
    return ok, bad, miss
chosen = collections.Counter(a for a in choice.values() if a in cmaps)
for minwin in (0, 5, 20, 50):
    o = [f for f in order if chosen[f] >= minwin or f == sf]
    ok, bad, _ = score(o)
    print(f'min {minwin:3d} wins: {len(o):3d} fonts reproduce {ok}/{ok + bad} ({100 * ok / (ok + bad):.2f}%)', file=sys.stderr)
MIN = int(sys.argv[1]) if len(sys.argv) > 1 else 5
order = [f for f in order if chosen[f] >= MIN or f == sf]
# Local search: adjacent swaps, kept only if the EXACT score improves. A
# swap only changes codepoints both faces cover, so score just those.
both = {}
def pair_delta(a, b, prefix_cover):
    key = (a, b)
    if key not in both: both[key] = [cp for cp in cmaps[a] & cmaps[b] if cp in choice]
    d = 0
    for cp in both[key]:
        if cp in prefix_cover: continue
        want = choice[cp]
        d += (want == b) - (want == a)
    return d
improved = True; rounds = 0
while improved and rounds < 20:
    improved = False; rounds += 1
    cover = set()
    for i in range(1, len(order) - 1):
        cover |= cmaps[order[i - 1]] if i > 0 else set()
        a, b = order[i], order[i + 1]
        if pair_delta(a, b, cover) > 0:
            order[i], order[i + 1] = b, a; improved = True
print(f'local search: {rounds} rounds', file=sys.stderr)
ok, bad, miss = score(order)
# Exceptions: codepoint ranges whose CoreText choice differs from the flat
# order's (and is a face in the order). Contiguous runs sharing a face merge.
idx_of = {f: i for i, f in enumerate(order)}
exc = []
for cp in sorted(choice):
    want = choice[cp]
    if want not in idx_of: continue
    got = next((f for f in order if cp in cmaps[f]), None)
    if got == want or cp not in cmaps[want]: continue
    i = idx_of[want]
    if exc and exc[-1][2] == i and exc[-1][1] + 1 == cp: exc[-1][1] = cp
    elif exc and exc[-1][2] == i and all(choice.get(c, want) == want or c not in cmaps[want] for c in range(exc[-1][1] + 1, cp)) and cp - exc[-1][1] < 64:
        exc[-1][1] = cp
    else: exc.append([cp, cp, i])
json.dump(exc, open('/tmp/ct_exceptions.json', 'w'))
fixed = sum(1 for cp, want in choice.items() if want in idx_of and cp in cmaps[want]
            and next((f for f in order if cp in cmaps[f]), None) != want)
print(f'{len(exc)} exception ranges fix {fixed} more codepoints -> {100 * (ok + fixed) / (ok + bad):.2f}%', file=sys.stderr)
print(f'flat order reproduces {ok}/{ok + bad} ({100 * ok / (ok + bad):.2f}%)', file=sys.stderr)
for (a, g), n in miss.most_common(15): print(f'  {n:6d} CT {a:36s} order {g}', file=sys.stderr)

for p, i, s in order: print(f'{s}\t{i}\t{p}')
