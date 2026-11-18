#pragma once
// A one-face shaping driver with hb-shape's conventions, shared by the
// hb-shape comparison tool (tools/mg_shape.cpp) and the HarfBuzz conformance
// test (tests/test_shaping.cpp): clusters are character indices, positions
// are font units, the output string is hb-shape --no-glyph-names format.
#include <mayag/font/font.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mayag::typo::hbcompat {

[[nodiscard]] inline std::uint32_t tag4(const std::string& s) {
    char t[4] = {' ', ' ', ' ', ' '};
    for (std::size_t i = 0; i < 4 && i < s.size(); ++i) t[i] = s[i];
    return ot::make_tag(t[0], t[1], t[2], t[3]);
}

struct Options {
    std::string script, direction, language;
    std::vector<ot::Feature> features;
    std::vector<std::pair<std::uint32_t, float>> variations;
    bool bot = false;
};

/// Parse hb-shape style arguments ("--features=+smcp,-liga", "--direction=rtl",
/// "--script=Arab", "--variations=wght=700").
inline Options parse_args(const std::vector<std::string>& args) {
    Options o;
    for (const std::string& a : args) {
        const auto val = [&](const char* k) { return a.rfind(k, 0) == 0 ? a.substr(std::strlen(k)) : std::string{}; };
        if (a == "--bot") { o.bot = true; continue; }
        if (auto v = val("--script="); !v.empty()) o.script = v;
        else if (auto d = val("--direction="); !d.empty()) o.direction = d;
        else if (auto lg = val("--language="); !lg.empty()) o.language = lg;
        else if (auto fs = val("--features="); !fs.empty()) {
            std::size_t p = 0;
            while (p < fs.size()) {
                std::size_t e = fs.find(',', p);
                if (e == std::string::npos) e = fs.size();
                std::string f = fs.substr(p, e - p);
                p = e + 1;
                ot::Feature ft;
                if (!f.empty() && f[0] == '-') { ft.value = 0; f = f.substr(1); }
                else if (!f.empty() && f[0] == '+') f = f.substr(1);
                if (auto eq = f.find('='); eq != std::string::npos) {
                    ft.value = static_cast<std::uint32_t>(std::stoul(f.substr(eq + 1)));
                    f = f.substr(0, eq);
                }
                ft.tag = tag4(f);
                o.features.push_back(ft);
            }
        } else if (auto vs = val("--variations="); !vs.empty()) {
            std::size_t p = 0;
            while (p < vs.size()) {
                std::size_t e = vs.find(',', p);
                if (e == std::string::npos) e = vs.size();
                const std::string f = vs.substr(p, e - p);
                p = e + 1;
                const auto eq = f.find('=');
                if (eq == std::string::npos) continue;
                o.variations.push_back({tag4(f.substr(0, eq)), std::stof(f.substr(eq + 1))});
            }
        }
    }
    return o;
}

/// Shape `text` with `face` and return hb-shape's output string.
inline std::string shape(Face& face, const std::string& text, const Options& o) {
    if (!o.variations.empty()) face.set_variations(o.variations);
    ot::Buffer b;
    for (std::size_t i = 0; i < text.size();) {
        ot::GlyphInfo g;
        g.codepoint = utf8::decode(text, i);
        g.cluster = static_cast<std::uint32_t>(b.info.size());
        b.info.push_back(g);
    }
    // Script: explicit, else the first strong one (hb_buffer_guess_segment_properties).
    ucd::Script script = ucd::Script::Common;
    if (!o.script.empty()) {
        const std::uint32_t want = tag4(o.script);
        for (unsigned s = 0; s < 256; ++s)
            if (ucd::iso15924_tag(static_cast<ucd::Script>(s)) == want) { script = static_cast<ucd::Script>(s); break; }
    } else {
        for (const auto& g : b.info) {
            const auto s = ucd::script(g.codepoint);
            if (s != ucd::Script::Common && s != ucd::Script::Inherited && s != ucd::Script::Unknown) { script = s; break; }
        }
    }
    b.direction = o.direction == "rtl" ? ot::Direction::rtl
                : o.direction == "ltr" ? ot::Direction::ltr
                                       : ot::script_direction(script);

    const ot::FontFile& file = face.file();
    const ot::Layout& layout = face.layout();
    ot::FontFuncs ff;
    ff.nominal = [&](char32_t c, std::uint16_t& g) { g = file.glyph_for(c); return g != 0; };
    ff.num_glyphs = file.num_glyphs();
    ff.advance = [&](std::uint16_t g) { return face.advance_units(g); };
    ff.upem = file.units_per_em();
    ff.coords = face.coords();
    const KernTable& kern = face.legacy_kern();
    if (kern.has_legacy()) ff.legacy_kern = [&](std::uint16_t a, std::uint16_t c) { return int(kern.legacy_lookup(a, c)); };
    if (const auto kt = file.table(ot::tag("kern")); aat::has_kern(kt)) { ff.kern_table = kt; ff.num_glyphs = file.num_glyphs(); }
    ff.kerx_table = file.table(ot::tag("kerx"));
    ff.ankr_table = file.table(ot::tag("ankr"));
    // hb-shape applies `trak` at CoreText's default 12 pt when the font has STAT.
    if (file.has_table(ot::tag("STAT"))) ff.tracking = face.track_table().at_exact(12.0f);
    ff.extents = [&](std::uint16_t g, ot::FontFuncs::Extents& e) {
        const Outline ol = face.outline(g);
        if (ol.empty()) { e = {}; return true; }
        const Rect r = ol.control_bounds();
        e.x_bearing = static_cast<int>(std::lround(r.origin.x));
        e.width = static_cast<int>(std::lround(r.origin.x + r.size.x)) - e.x_bearing;
        e.y_bearing = static_cast<int>(std::lround(r.origin.y + r.size.y));
        e.height = static_cast<int>(std::lround(r.origin.y)) - e.y_bearing;
        return true;
    };
    const std::vector<std::uint32_t> langs = ot::ot_language_tags(o.language);
    const auto plan = ot::compile_plan(layout, script, b.direction, langs, o.features, ff, kern.has_legacy() || !ff.kern_table.empty(),
                                       file.has_table(ot::tag("kerx")), file.table(ot::tag("morx")));
    ot::shape(plan, layout, ff, b, o.features, o.bot);

    std::string out = "[";
    for (std::size_t i = 0; i < b.len(); ++i) {
        const auto& g = b.info[i];
        const auto& p = b.pos[i];
        char buf[96];
        const long xo = std::lround(p.x_offset), yo = std::lround(p.y_offset), xa = std::lround(p.x_advance);
        std::snprintf(buf, sizeof buf, "%s%u=%u", i ? "|" : "", g.gid, g.cluster);
        out += buf;
        if (xo || yo) { std::snprintf(buf, sizeof buf, "@%ld,%ld", xo, yo); out += buf; }
        std::snprintf(buf, sizeof buf, "+%ld", xa);
        out += buf;
    }
    return out + "]";
}

}  // namespace mayag::typo::hbcompat
