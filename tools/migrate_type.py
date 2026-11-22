#!/usr/bin/env python3
"""tools/migrate_type.py -- move widget text sizes onto the type scale.

One-off migration (kept for the record): every `X * k` text size in
include/mayag/widget becomes a tier of style/type.hpp, by its effective
multiple of the face's body size. Layout arithmetic (round(), pad(), gap(),
width(), height(), size(), min_size()) is left alone.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "include" / "mayag"

def tier_of(m):
    if m < 0.77: return "fine"
    if m < 0.90: return "small"
    if m < 1.10: return "body"
    if m < 1.32: return "large"
    if m < 1.60: return "title"
    return "display"

LAYOUT = ("round", "pad", "gap", "width", "height", "size", "min_size", "max_size",
          "radius", "capsule", "term_cell_width", "cell_width", "add", "node_id", "translate",
          "absolute", "offset", "ceil", "floor", "max", "min", "clamp")

UI_BASES = {"t.font_size": 1.0, "t.font_size * t.chrome_type": 1.0}
HANDED = ("b.font_size", "o.font_size", "p.font_size", "po.font_size")

def size_of(base, mult):
    """The tier expression for `mult` x `base`."""
    tier = tier_of(mult)
    if base == "ui": return f"t.type(Tier::{tier})"
    return f"t.type_at({base}, Tier::{tier})"

def enclosing_call(text, pos):
    """Name of the innermost call whose parenthesis encloses pos."""
    depth = 0
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == ')': depth += 1
        elif c == '(':
            if depth == 0:
                m = re.search(r"([A-Za-z_][A-Za-z_0-9:]*)\s*$", text[:i])
                return m.group(1).split("::")[-1] if m else ""
            depth -= 1
        elif c in ";{}" and depth == 0:
            return ""
        i -= 1
    return ""

NUM = r"([0-9]+(?:\.[0-9]*)?)f?"

def migrate(path):
    src = path.read_text()
    out = src
    vars_ = {}   # name -> (base, mult)

    # 1. size variables: `const float NAME = BASE [* K];`
    def var_def(m):
        name, expr = m.group(2), m.group(3).strip()
        base, mult = None, 1.0
        mm = re.fullmatch(r"(t\.font_size(?: \* t\.chrome_type)?)(?: \* " + NUM + r")?", expr)
        if mm:
            base, mult = "ui", float(mm.group(2) or 1.0)
        else:
            mm = re.fullmatch(r"(" + "|".join(re.escape(h) for h in HANDED) + r")(?: \* " + NUM + r")?", expr)
            if mm: base, mult = mm.group(1), float(mm.group(2) or 1.0)
        if base is None: return m.group(0)
        vars_[name] = (base, mult)
        return f"{m.group(1)}{name} = {size_of(base, mult)};"
    out = re.sub(r"((?:const )?float )([a-z_][a-z_0-9]*) = ([^;]*\bfont_size\b[^;]*);", var_def, out)

    # 2. products: BASE * K and VAR * K in text-size positions
    bases = {"t.font_size * t.chrome_type": ("ui", 1.0), "t.font_size": ("ui", 1.0)}
    for h in HANDED: bases[h] = (h, 1.0)
    for v, (b, k) in vars_.items(): bases[v] = (b, k)
    names = sorted(bases, key=len, reverse=True)
    pat = re.compile(r"(?<![A-Za-z_0-9.])(" + "|".join(re.escape(n) for n in names) + r")(?: \* " + NUM + r")?(?![A-Za-z_0-9(])")

    res, last = [], 0
    for m in pat.finditer(out):
        name, k = m.group(1), m.group(2)
        call = enclosing_call(out, m.start())
        if call in LAYOUT: continue
        # a bare variable already is a tier now; a bare base outside font() is layout
        if k is None:
            if name in vars_: continue
            if call != "font": continue
        base, mult = bases[name]
        total = mult * float(k or 1.0)
        if total > 1.9 or total < 0.55: continue   # not a text size
        res.append(out[last:m.start()]); res.append(size_of(base, total)); last = m.end()
    res.append(out[last:])
    out = "".join(res)
    if out != src:
        path.write_text(out)
        return True
    return False

if __name__ == "__main__":
    files = [pathlib.Path(p) for p in sys.argv[1:]] or sorted((ROOT / "widget").rglob("*.hpp"))
    for f in files:
        if migrate(f): print("migrated", f)
