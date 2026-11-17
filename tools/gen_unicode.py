#!/usr/bin/env python3
"""Generate include/mayag/font/unicode_data.hpp from the Unicode Character Database.

Usage: python3 tools/gen_unicode.py <ucd-dir> <output.hpp>

stdlib only, deterministic output. See the header comment emitted below for the
table layout.
"""
import os
import re
import sys

MAXCP = 0x110000
BLOCK_SHIFT = 7
BLOCK = 1 << BLOCK_SHIFT


def lines(path):
    with open(path, encoding="utf-8") as f:
        for raw in f:
            yield raw.rstrip("\n")


def data_lines(path):
    for raw in lines(path):
        s = raw.split("#", 1)[0].strip()
        if s:
            yield [x.strip() for x in s.split(";")]


def missing_lines(path):
    for raw in lines(path):
        m = re.match(r"#\s*@missing:\s*([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(.+?)\s*$", raw)
        if m:
            yield int(m.group(1), 16), int(m.group(2), 16), m.group(3)


def rng(s):
    if ".." in s:
        a, b = s.split("..")
        return int(a, 16), int(b, 16)
    v = int(s, 16)
    return v, v


def ident(name):
    s = re.sub(r"[^0-9A-Za-z]+", "_", name.strip()).strip("_")
    if s and s[0].isdigit():
        s = "_" + s
    return s


def version_of(path):
    for raw in lines(path):
        m = re.search(r"-(\d+\.\d+\.\d+)\.txt", raw)
        if m:
            return m.group(1)
    return "unknown"


def esc_bytes(data, width=100):
    """Encode bytes as a sequence of C string-literal lines."""
    res = []
    cur = []
    curlen = 0
    n = len(data)
    for i, b in enumerate(data):
        c = chr(b)
        if 0x20 <= b < 0x7F and c not in '\\"?':
            piece = c
        else:
            o = format(b, "o")
            # an octal escape is greedy (up to 3 digits): pad it when an octal
            # digit follows in the same literal
            if i + 1 < n and 0x30 <= data[i + 1] <= 0x37:
                o = o.rjust(3, "0")
            piece = "\\" + o
        cur.append(piece)
        curlen += len(piece)
        if curlen >= width:
            res.append('    "' + "".join(cur) + '"')
            cur, curlen = [], 0
    if cur:
        res.append('    "' + "".join(cur) + '"')
    return "\n".join(res) if res else '    ""'

BIDI = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
        "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]
GC = ["Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe",
      "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"]
JT = ["U", "R", "L", "D", "C", "T"]
GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend",
       "SpacingMark", "L", "V", "T", "LV", "LVT"]


class Data:
    pass


def load(ucd):
    P = lambda n: os.path.join(ucd, n)
    d = Data()
    d.version = version_of(P("DerivedBidiClass.txt"))

    # --- property value aliases ---------------------------------------
    bidi_alias = {}
    sc_alias = {}  # long -> short
    for f in data_lines(P("PropertyValueAliases.txt")):
        if f[0] == "bc":
            for a in f[2:]:
                bidi_alias[a] = f[1]
            bidi_alias[f[1]] = f[1]
        elif f[0] == "sc":
            sc_alias[f[2]] = f[1]
    specials = [("Common", "Zyyy"), ("Inherited", "Zinh"), ("Unknown", "Zzzz")]
    rest = sorted((l, s) for l, s in sc_alias.items() if s not in ("Zyyy", "Zinh", "Zzzz"))
    d.scripts = specials + rest  # (long, short)
    sc_index = {}
    for i, (l, s) in enumerate(d.scripts):
        sc_index[l] = i
        sc_index[s] = i
    d.sc_index = sc_index

    # --- general category, ccc, decompositions ------------------------
    gc = bytearray([GC.index("Cn")]) * MAXCP
    ccc = bytearray(MAXCP)
    decomp = {}
    first = None
    for f in data_lines(P("UnicodeData.txt")):
        cp = int(f[0], 16)
        name = f[1]
        g = GC.index(f[2])
        if name.endswith(", First>"):
            first = cp
            continue
        if name.endswith(", Last>"):
            for c in range(first, cp + 1):
                gc[c] = g
                ccc[c] = int(f[3])
            continue
        gc[cp] = g
        ccc[cp] = int(f[3])
        if f[5] and not f[5].startswith("<"):
            parts = [int(x, 16) for x in f[5].split()]
            decomp[cp] = parts
    d.gc, d.ccc, d.decomp = gc, ccc, decomp

    # --- scripts ------------------------------------------------------
    script = bytearray([sc_index["Unknown"]]) * MAXCP
    for f in data_lines(P("Scripts.txt")):
        a, b = rng(f[0])
        v = sc_index[f[1]]
        for c in range(a, b + 1):
            script[c] = v
    d.script = script
    scx = {}
    for f in data_lines(P("ScriptExtensions.txt")):
        a, b = rng(f[0])
        vals = tuple(sorted(sc_index[x] for x in f[1].split()))
        for c in range(a, b + 1):
            scx[c] = vals
    d.scx = scx

    # --- bidi class ---------------------------------------------------
    bidi = bytearray(MAXCP)
    path = P("DerivedBidiClass.txt")
    for a, b, v in missing_lines(path):
        k = BIDI.index(bidi_alias[v])
        for c in range(a, b + 1):
            bidi[c] = k
    for f in data_lines(path):
        a, b = rng(f[0])
        k = BIDI.index(bidi_alias[f[1]])
        for c in range(a, b + 1):
            bidi[c] = k
    d.bidi = bidi
    d.mirror = {}
    for f in data_lines(P("BidiMirroring.txt")):
        d.mirror[int(f[0], 16)] = int(f[1], 16)
    d.brackets = {}
    for f in data_lines(P("BidiBrackets.txt")):
        d.brackets[int(f[0], 16)] = (int(f[1], 16), 1 if f[2] == "o" else 2)

    # --- joining ------------------------------------------------------
    jt = bytearray(MAXCP)
    for c in range(MAXCP):
        if GC[gc[c]] in ("Mn", "Me", "Cf"):
            jt[c] = JT.index("T")
    jg = bytearray(MAXCP)
    groups = set()
    rows = list(data_lines(P("ArabicShaping.txt")))
    for f in rows:
        if f[3] != "No_Joining_Group":
            groups.add(ident(f[3]))
    d.jgroups = ["none"] + sorted(groups)
    for f in rows:
        c = int(f[0], 16)
        jt[c] = JT.index(f[2])
        if f[3] != "No_Joining_Group":
            jg[c] = d.jgroups.index(ident(f[3]))
    d.jt, d.jg = jt, jg

    # --- indic --------------------------------------------------------
    def enum_prop(fname, default):
        vals = set()
        rows = list(data_lines(P(fname)))
        for f in rows:
            vals.add(f[1])
        vals.discard(default)
        names = [default] + sorted(vals)
        arr = bytearray(MAXCP)
        for f in rows:
            a, b = rng(f[0])
            k = names.index(f[1])
            for c in range(a, b + 1):
                arr[c] = k
        return names, arr

    d.isc_names, d.isc = enum_prop("IndicSyllabicCategory.txt", "Other")
    d.ipc_names, d.ipc = enum_prop("IndicPositionalCategory.txt", "Not_Applicable")

    gcb = bytearray(MAXCP)
    for f in data_lines(P("GraphemeBreakProperty.txt")):
        a, b = rng(f[0])
        k = GCB.index(f[1])
        for c in range(a, b + 1):
            gcb[c] = k
    d.gcb = gcb

    flags = bytearray(MAXCP)
    for f in data_lines(P("emoji-data.txt")):
        if f[1] == "Extended_Pictographic":
            a, b = rng(f[0])
            for c in range(a, b + 1):
                flags[c] |= 1
    for f in data_lines(P("DerivedCoreProperties.txt")):
        if f[1] == "Default_Ignorable_Code_Point":
            a, b = rng(f[0])
            for c in range(a, b + 1):
                flags[c] |= 2
    d.flags = flags

    fce = set()
    for f in data_lines(P("DerivedNormalizationProps.txt")):
        if f[1] == "Full_Composition_Exclusion":
            a, b = rng(f[0])
            fce.update(range(a, b + 1))
    d.comp = {}
    for cp, parts in d.decomp.items():
        if len(parts) == 2 and cp not in fce:
            d.comp[(parts[0], parts[1])] = cp
    return d

def build_tables(d):
    rec_index = {}
    recs = []
    per_cp = [0] * MAXCP
    cols = (d.script, d.bidi, d.gc, d.jt, d.jg, d.ccc, d.isc, d.ipc, d.gcb, d.flags)
    for c in range(MAXCP):
        r = bytes(col[c] for col in cols)
        k = rec_index.get(r)
        if k is None:
            k = len(recs)
            rec_index[r] = k
            recs.append(r)
        per_cp[c] = k
    assert len(recs) < 65536
    blocks = {}
    block_list = []
    stage1 = []
    for bi in range(MAXCP >> BLOCK_SHIFT):
        blk = tuple(per_cp[bi * BLOCK:(bi + 1) * BLOCK])
        k = blocks.get(blk)
        if k is None:
            k = len(block_list)
            blocks[blk] = k
            block_list.append(blk)
        stage1.append(k)
    return recs, stage1, block_list


def u16le(vals):
    out = bytearray()
    for v in vals:
        out += bytes((v & 0xFF, v >> 8))
    return bytes(out)


def u24le(vals):
    out = bytearray()
    for v in vals:
        out += bytes((v & 0xFF, (v >> 8) & 0xFF, v >> 16))
    return bytes(out)


def emit_str(name, data):
    return "inline constexpr char %s[] =\n%s;\n" % (name, esc_bytes(data))


def enum_body(names):
    out = []
    line = "   "
    for n in names:
        piece = " %s," % n
        if len(line) + len(piece) > 96:
            out.append(line)
            line = "   "
        line += piece
    out.append(line)
    return "\n".join(out)


def tag_u32(s):
    return "0x%08Xu" % ((ord(s[0]) << 24) | (ord(s[1]) << 16) | (ord(s[2]) << 8) | ord(s[3]))

RUNTIME = r'''[[nodiscard]] constexpr std::uint32_t rd16(const char* p, std::size_t i) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[i]))
         | static_cast<std::uint32_t>(static_cast<unsigned char>(p[i + 1])) << 8;
}
[[nodiscard]] constexpr std::uint32_t rd24(const char* p, std::size_t i) noexcept {
    return rd16(p, i) | static_cast<std::uint32_t>(static_cast<unsigned char>(p[i + 2])) << 16;
}

// Field k of the property record for cp.
[[nodiscard]] constexpr std::uint8_t prop(char32_t cp, std::size_t k) noexcept {
    if (cp > 0x10FFFF) cp = 0x10FFFF;  // noncharacter: Unknown / Cn defaults
    const std::uint32_t blk = rd16(stage1, (static_cast<std::size_t>(cp) >> 7) * 2);
    const std::uint32_t rec = rd16(stage2[blk / chunk_blocks],
                                   ((blk % chunk_blocks) * 128 + (cp & 127)) * 2);
    return static_cast<unsigned char>(records[rec * record_size + k]);
}

// Binary search over fixed-width records whose first field is a u24 key.
[[nodiscard]] constexpr std::size_t find(const char* tab, std::size_t n, std::size_t stride,
                                         std::uint32_t key) noexcept {
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const std::uint32_t k = rd24(tab, mid * stride);
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return n;
}

inline constexpr char32_t s_base = 0xAC00, l_base = 0x1100, v_base = 0x1161, t_base = 0x11A7;
inline constexpr char32_t l_count = 19, v_count = 21, t_count = 28;
inline constexpr char32_t n_count = v_count * t_count, s_count = l_count * n_count;

} // namespace detail

[[nodiscard]] constexpr Script script(char32_t cp) noexcept { return Script(detail::prop(cp, 0)); }

[[nodiscard]] constexpr std::uint32_t iso15924_tag(Script s) noexcept {
    return detail::script_tags[static_cast<std::size_t>(s)];
}

[[nodiscard]] constexpr std::span<const Script> script_extensions(char32_t cp) noexcept {
    std::size_t lo = 0, hi = detail::scx_count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const std::uint32_t a = detail::rd24(detail::scx, mid * 9);
        const std::uint32_t b = detail::rd24(detail::scx, mid * 9 + 3);
        if (cp < a) hi = mid;
        else if (cp > b) lo = mid + 1;
        else {
            const std::size_t off = detail::rd16(detail::scx, mid * 9 + 6);
            const std::size_t n = static_cast<unsigned char>(detail::scx[mid * 9 + 8]);
            return {detail::scx_sets + off, n};
        }
    }
    return {detail::all_scripts + detail::prop(cp, 0), 1};
}

[[nodiscard]] constexpr BidiClass bidi_class(char32_t cp) noexcept { return BidiClass(detail::prop(cp, 1)); }

[[nodiscard]] constexpr char32_t bidi_mirror(char32_t cp) noexcept {
    const std::size_t i = detail::find(detail::mirror, detail::mirror_count, 6, cp);
    return i == detail::mirror_count ? cp : char32_t(detail::rd24(detail::mirror, i * 6 + 3));
}

[[nodiscard]] constexpr Bracket bidi_bracket(char32_t cp) noexcept {
    const std::size_t i = detail::find(detail::bracket, detail::bracket_count, 7, cp);
    if (i == detail::bracket_count) return {0, BracketType::none};
    return {char32_t(detail::rd24(detail::bracket, i * 7 + 3)),
            BracketType(static_cast<unsigned char>(detail::bracket[i * 7 + 6]))};
}

[[nodiscard]] constexpr GeneralCategory general_category(char32_t cp) noexcept {
    return GeneralCategory(detail::prop(cp, 2));
}
[[nodiscard]] constexpr bool is_mark(char32_t cp) noexcept {
    const auto g = general_category(cp);
    return g == GeneralCategory::Mn || g == GeneralCategory::Mc || g == GeneralCategory::Me;
}
[[nodiscard]] constexpr JoiningType joining_type(char32_t cp) noexcept { return JoiningType(detail::prop(cp, 3)); }
[[nodiscard]] constexpr JoiningGroup joining_group(char32_t cp) noexcept { return JoiningGroup(detail::prop(cp, 4)); }
[[nodiscard]] constexpr std::uint8_t combining_class(char32_t cp) noexcept { return detail::prop(cp, 5); }
[[nodiscard]] constexpr IndicSyllabic indic_syllabic(char32_t cp) noexcept { return IndicSyllabic(detail::prop(cp, 6)); }
[[nodiscard]] constexpr IndicPositional indic_positional(char32_t cp) noexcept {
    return IndicPositional(detail::prop(cp, 7));
}
[[nodiscard]] constexpr GraphemeBreak grapheme_break(char32_t cp) noexcept { return GraphemeBreak(detail::prop(cp, 8)); }
[[nodiscard]] constexpr bool is_extended_pictographic(char32_t cp) noexcept { return (detail::prop(cp, 9) & 1) != 0; }
[[nodiscard]] constexpr bool is_default_ignorable(char32_t cp) noexcept { return (detail::prop(cp, 9) & 2) != 0; }

[[nodiscard]] constexpr Decomposition canonical_decomposition(char32_t cp) noexcept {
    using namespace detail;
    if (cp >= s_base && cp < s_base + s_count) {
        const char32_t si = cp - s_base;
        if (si % t_count == 0) return {l_base + si / n_count, v_base + (si % n_count) / t_count};
        return {cp - si % t_count, t_base + si % t_count};
    }
    const std::size_t i = find(decomp, decomp_count, 9, cp);
    if (i == decomp_count) return {};
    return {char32_t(rd24(decomp, i * 9 + 3)), char32_t(rd24(decomp, i * 9 + 6))};
}

[[nodiscard]] constexpr char32_t canonical_composition(char32_t a, char32_t b) noexcept {
    using namespace detail;
    if (a >= l_base && a < l_base + l_count && b >= v_base && b < v_base + v_count)
        return s_base + ((a - l_base) * v_count + (b - v_base)) * t_count;
    if (a >= s_base && a < s_base + s_count && (a - s_base) % t_count == 0 && b > t_base && b < t_base + t_count)
        return a + (b - t_base);
    std::size_t lo = 0, hi = comp_count;
    const std::uint64_t key = (std::uint64_t(a) << 24) | b;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const std::uint64_t k = (std::uint64_t(rd24(comp, mid * 9)) << 24) | rd24(comp, mid * 9 + 3);
        if (k == key) return char32_t(rd24(comp, mid * 9 + 6));
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return 0;
}

} // namespace mayag::typo::ucd
'''

# 128 blocks * 128 entries * 2 bytes = 32 KiB per literal. MSVC rejects a
# string literal of 65535 bytes or more INCLUDING its terminating NUL (C2026),
# so a 64 KiB chunk is one byte too many there; 32 KiB leaves wide margin.
CHUNK_BLOCKS = 128


def emit(d, out_path):
    recs, stage1, block_list = build_tables(d)
    o = []
    w = o.append
    w("// include/mayag/font/unicode_data.hpp — Unicode Character Database tables\n")
    w("//\n// GENERATED FILE — DO NOT EDIT. Unicode %s.\n" % d.version)
    w("// Regenerate: python3 tools/gen_unicode.py /tmp/ucd include/mayag/font/unicode_data.hpp\n")
    w("//   (the argument is a directory holding the UCD text files: UnicodeData.txt,\n")
    w("//    Scripts.txt, ScriptExtensions.txt, DerivedBidiClass.txt, ArabicShaping.txt, ...)\n")
    w("//\n// Layout. Every per-codepoint property lives in ONE two-stage table:\n")
    w("//   stage1[cp >> 7]               -> block id   (u16 LE)\n")
    w("//   stage2[block * 128 + (cp&127)] -> record id  (u16 LE)\n")
    w("//   records[id * 10 + k]          -> script, bidi, gc, jt, jg, ccc, isc, ipc, gcb, flags\n")
    w("// Identical 128-cp blocks are shared, so the 1.1M-codepoint space collapses to a few\n")
    w("// hundred blocks. Arrays are string literals (not brace-initialised integer arrays)\n")
    w("// because the front end swallows a string literal in one token: this header is\n")
    w("// included by most TUs and must stay cheap. Sparse properties (mirrors, brackets,\n")
    w("// decompositions, compositions, script extensions) are sorted fixed-width records\n")
    w("// (u24 LE fields) searched by binary search.\n\n")
    w("#pragma once\n\n#include <cstddef>\n#include <cstdint>\n#include <span>\n\n")
    w("namespace mayag::typo::ucd {\n\n")
    w("inline constexpr char unicode_version[] = \"%s\";\n\n" % d.version)

    w("// Common, Inherited, Unknown first; then every other script by long name.\n")
    w("enum class Script : std::uint8_t {\n%s\n};\n\n" % enum_body([l for l, _ in d.scripts]))
    w("enum class BidiClass : std::uint8_t {\n%s\n};\n\n" % enum_body(BIDI))
    w("enum class BracketType : std::uint8_t { none, open, close };\n")
    w("struct Bracket { char32_t pair; BracketType type; };\n\n")
    w("enum class JoiningType : std::uint8_t { U, R, L, D, C, T };\n\n")
    w("enum class JoiningGroup : std::uint8_t {\n%s\n};\n\n" % enum_body(d.jgroups))
    w("enum class GeneralCategory : std::uint8_t {\n%s\n};\n\n" % enum_body(GC))
    w("enum class IndicSyllabic : std::uint8_t {\n%s\n};\n\n" % enum_body(d.isc_names))
    w("enum class IndicPositional : std::uint8_t {\n%s\n};\n\n" % enum_body(d.ipc_names))
    w("enum class GraphemeBreak : std::uint8_t {\n%s\n};\n\n" % enum_body(GCB))
    w("struct Decomposition { char32_t a = 0, b = 0; };  // b==0 => singleton, a==0 => none\n\n")

    w("namespace detail {\n\n")
    w("inline constexpr std::size_t record_size = 10;\n")
    w("inline constexpr std::size_t chunk_blocks = %d;\n\n" % CHUNK_BLOCKS)
    w("inline constexpr std::uint32_t script_tags[] = {\n")
    for i in range(0, len(d.scripts), 8):
        w("    " + " ".join(tag_u32(s) + "," for _, s in d.scripts[i:i + 8]) + "\n")
    w("};\n\n")
    w("inline constexpr Script all_scripts[] = {\n")
    for i in range(0, len(d.scripts), 8):
        w("    " + " ".join("Script(%d)," % j for j in range(i, min(i + 8, len(d.scripts)))) + "\n")
    w("};\n\n")

    w(emit_str("stage1", u16le(stage1)))
    w("\n")
    flat = [v for blk in block_list for v in blk]
    nchunks = (len(block_list) + CHUNK_BLOCKS - 1) // CHUNK_BLOCKS
    for i in range(nchunks):
        part = flat[i * CHUNK_BLOCKS * BLOCK:(i + 1) * CHUNK_BLOCKS * BLOCK]
        w(emit_str("stage2_%d" % i, u16le(part)))
        w("\n")
    w("inline constexpr const char* stage2[] = { %s };\n\n"
      % ", ".join("stage2_%d" % i for i in range(nchunks)))
    w(emit_str("records", b"".join(recs)))
    w("\n")

    # script extensions: unique sets + ranges
    sets = sorted(set(d.scx.values()))
    set_idx = {}
    flat_sets = []
    w("inline constexpr Script scx_sets[] = {\n")
    for s in sets:
        set_idx[s] = (len(flat_sets), len(s))
        flat_sets.extend(s)
        w("    " + " ".join("Script(%d)," % v for v in s) + "\n")
    w("};\n")
    ranges = []
    for cp in sorted(d.scx):
        off, n = set_idx[d.scx[cp]]
        if ranges and ranges[-1][1] == cp - 1 and ranges[-1][2] == off and ranges[-1][3] == n:
            ranges[-1][1] = cp
        else:
            ranges.append([cp, cp, off, n])
    data = bytearray()
    for a, b, off, n in ranges:
        data += u24le([a, b]) + bytes((off & 0xFF, off >> 8, n))
    w("inline constexpr std::size_t scx_count = %d;  // records: lo u24, hi u24, off u16, n u8\n" % len(ranges))
    w(emit_str("scx", bytes(data)))
    w("\n")

    def emit_sparse(name, rows, comment):
        w("inline constexpr std::size_t %s_count = %d;  // %s\n" % (name, len(rows), comment))
        w(emit_str(name, b"".join(rows)))
        w("\n")

    emit_sparse("mirror", [u24le([k, v]) for k, v in sorted(d.mirror.items())], "cp u24, mirror u24")
    emit_sparse("bracket", [u24le([k, v[0]]) + bytes((v[1],)) for k, v in sorted(d.brackets.items())],
                "cp u24, pair u24, type u8")
    emit_sparse("decomp", [u24le([k, v[0], v[1] if len(v) > 1 else 0]) for k, v in sorted(d.decomp.items())],
                "cp u24, a u24, b u24")
    emit_sparse("comp", [u24le([a, b, c]) for (a, b), c in sorted(d.comp.items())], "a u24, b u24, composite u24")
    w("// @@RUNTIME@@\n")
    text = "".join(o).replace("// @@RUNTIME@@\n", RUNTIME)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    d = load(sys.argv[1])
    emit(d, sys.argv[2])


if __name__ == "__main__":
    main()
