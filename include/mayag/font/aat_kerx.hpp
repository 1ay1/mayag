#pragma once
// mayag::typo::aat — the `kerx` table (Apple extended kerning) + `ankr`
//
// HarfBuzz applies `kerx` instead of GPOS when a font has kerx and lacks
// GSUB+GPOS together (hb_ot_shape_plan: apply_kerx). KerxTable::apply walks
// every horizontal subtable: skip it unless some buffer glyph is in its
// "left set", chain every glyph for the first cross-stream subtable, reverse
// the buffer when Backwards differs from the direction, and dispatch:
//
//   0  sorted pairs (u32 binsearch header)              hb_kern_machine_t
//   1  state machine, pushes glyphs + pops kern actions  (Push/DontAdvance/Reset)
//   2  left/right class Lookups summed into a value array hb_kern_machine_t
//   4  state machine placing marks on anchors (ankr / control points)
//   6  row/column index Lookups (short or long values)   hb_kern_machine_t
//
// Values are in font units (mayag shapes at upem, so HarfBuzz's em_scale is
// the identity). A port of HarfBuzz 15.1.0 hb-aat-layout-kerx-table.hh and
// hb-aat-layout-ankr-table.hh (Old MIT; see THIRD_PARTY_NOTICES.md).

#include "aat_morx.hpp"
#include "ot_layout.hpp"

#include <cstdint>
#include <span>

namespace mayag::typo::aat {

namespace kerx_detail {

inline constexpr std::uint32_t cov_vertical = 0x80000000u, cov_cross_stream = 0x40000000u,
                               cov_backwards = 0x10000000u, cov_type = 0x000000FFu;

struct Sub {
    std::size_t off = 0;     ///< subtable start (base of its offsets)
    std::size_t limit = 0;   ///< readable end: the subtable, or the table for the last one
    std::uint32_t coverage = 0, tuple_count = 0;
    [[nodiscard]] unsigned type() const { return coverage & cov_type; }
    [[nodiscard]] bool cross_stream() const { return (coverage & cov_cross_stream) != 0; }
};

/// kerxTupleKern: with tuples, `value` is an offset from `base` to the first tuple.
[[nodiscard]] inline int tuple_kern(const ot::Reader& r, int value, std::uint32_t tuple_count, std::size_t base,
                                    std::size_t limit) {
    if (!tuple_count) return value;
    const std::size_t p = base + static_cast<std::uint32_t>(value);
    if (p + 2 * std::size_t{tuple_count} > limit) return 0;
    return r.i16_at(p);
}

// ── pair formats: left-set membership and values ─────────────────────────

[[nodiscard]] inline bool f0_has_left(const ot::Reader& r, const Sub& s, std::uint16_t g) {
    const std::size_t first = s.off + 28;
    std::size_t n = r.u32_at(s.off + 12);
    if (first + n * 6 > s.limit) n = s.limit > first ? (s.limit - first) / 6 : 0;
    std::size_t lo = 0, hi = n;
    while (lo < hi) {   // lower bound of (g, 0)
        const std::size_t mid = (lo + hi) / 2;
        if (r.u16_at(first + mid * 6) < g) lo = mid + 1; else hi = mid;
    }
    return lo < n && r.u16_at(first + lo * 6) == g;
}

[[nodiscard]] inline int f0_value(const ot::Reader& r, const Sub& s, std::uint16_t left, std::uint16_t right) {
    const std::size_t first = s.off + 28;
    std::size_t lo = 0, hi = r.u32_at(s.off + 12);
    if (first + hi * 6 > s.limit) hi = s.limit > first ? (s.limit - first) / 6 : 0;
    const std::uint32_t key = std::uint32_t{left} << 16 | right;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const std::uint32_t k = r.u32_at(first + mid * 6);
        if (k == key) return tuple_kern(r, r.i16_at(first + mid * 6 + 4), s.tuple_count, s.off, s.limit);
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return 0;
}

[[nodiscard]] inline bool f2_left(const ot::Reader& r, const Sub& s, std::uint16_t g, unsigned ng) {
    return lookup_covers<2>(r, s.off + r.u32_at(s.off + 16), g, ng);
}
[[nodiscard]] inline bool f2_right(const ot::Reader& r, const Sub& s, std::uint16_t g, unsigned ng) {
    return lookup_covers<2>(r, s.off + r.u32_at(s.off + 20), g, ng);
}
[[nodiscard]] inline int f2_value(const ot::Reader& r, const Sub& s, std::uint16_t left, std::uint16_t right,
                                  unsigned ng) {
    const unsigned l = lookup<2>(r, s.off + r.u32_at(s.off + 16), left, ng, false).value_or(0);
    const unsigned rr = lookup<2>(r, s.off + r.u32_at(s.off + 20), right, ng, false).value_or(0);
    // The class values are pre-multiplied BYTE offsets (rows by the row
    // width, columns by 2): their sum / 2 is the value index (HarfBuzz 15.1).
    const std::size_t p = s.off + r.u32_at(s.off + 24) + 2 * std::size_t{(l + rr) / 2};
    if (p + 2 > s.limit) return 0;
    return tuple_kern(r, r.i16_at(p), s.tuple_count, s.off, s.limit);
}

[[nodiscard]] inline bool f6_long(const ot::Reader& r, const Sub& s) { return (r.u32_at(s.off + 12) & 1) != 0; }
[[nodiscard]] inline bool f6_covers(const ot::Reader& r, const Sub& s, std::size_t field, std::uint16_t g, unsigned ng) {
    const std::size_t t = s.off + r.u32_at(s.off + field);
    return f6_long(r, s) ? lookup_covers<4>(r, t, g, ng) : lookup_covers<2>(r, t, g, ng);
}
[[nodiscard]] inline int f6_value(const ot::Reader& r, const Sub& s, std::uint16_t left, std::uint16_t right,
                                  unsigned ng) {
    const std::size_t rows = s.off + r.u32_at(s.off + 20), cols = s.off + r.u32_at(s.off + 24);
    const std::size_t array = s.off + r.u32_at(s.off + 28), vector = s.off + r.u32_at(s.off + 32);
    if (f6_long(r, s)) {
        const std::uint32_t l = lookup<4>(r, rows, left, ng).value_or(0);
        const std::uint32_t rr = lookup<4>(r, cols, right, ng).value_or(0);
        const std::uint32_t offset = l + rr;
        if (offset < l || offset > 0x3FFFFFFFu) return 0;
        const std::size_t p = array + 4 * std::size_t{offset};
        if (p + 4 > s.limit) return 0;
        return tuple_kern(r, static_cast<std::int32_t>(r.u32_at(p)), s.tuple_count, vector, s.limit);
    }
    const std::uint32_t l = lookup<2>(r, rows, left, ng).value_or(0);
    const std::uint32_t rr = lookup<2>(r, cols, right, ng).value_or(0);
    const std::size_t p = array + 2 * std::size_t{l + rr};
    if (p + 2 > s.limit) return 0;
    return tuple_kern(r, r.i16_at(p), s.tuple_count, vector, s.limit);
}

// ── state-machine formats ────────────────────────────────────────────────

/// StateTable::collect_initial_glyphs membership: is `g` in a class that the
/// start state does something with? `acts(entry)` = initiable || actionable.
template <class Acts>
[[nodiscard]] inline bool machine_initial(const StateTable& m, std::uint16_t g, Acts&& acts) {
    const auto filter = [&](unsigned klass) {
        const std::size_t e = m.entry(0, klass);
        return !(m.new_state(e) == 0 && !acts(e));
    };
    if (m.n_classes > 512) return g != deleted_glyph && lookup16(m.r, m.class_table, g, m.num_glyphs, false).has_value();
    if (g == deleted_glyph) return filter(class_deleted);
    const auto v = lookup16(m.r, m.class_table, g, m.num_glyphs, false);
    return v && *v < m.n_classes && filter(*v);
}

/// KerxSubTableFormat1::driver_context_t (extended types), as HarfBuzz 15.1
/// matches native CoreText: an eight-slot CIRCULAR history (unused slots are
/// glyph 0) that every action list walks anew from the newest push, Reset
/// ignored, lists ended by a lone 0xFFFF (no odd-bit terminator), and the
/// action index read as an index, not a byte offset.
struct Format1 {
    static constexpr bool in_place = true;
    const ot::Reader& r;
    std::size_t kern_action = 0, limit = 0;
    std::uint32_t tuple_count = 0, kern_mask = 0;
    bool cross_stream = false;
    std::size_t stack[8]{};
    unsigned depth = 0, stack_pos = 0;

    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        const unsigned flags = m.flags(e);
        if (flags & 0x8000) {            // Push
            stack[stack_pos] = b.idx;
            stack_pos = (stack_pos + 1) % 8;
            depth = std::min(depth + 1, 8u);
        }
        const unsigned action = m.data(e, 0);
        if (action == 0xFFFF || !depth) return;
        const std::uint32_t tc = tuple_count ? tuple_count : 1;
        std::size_t p = kern_action + 2 * std::size_t{action};
        unsigned cursor = stack_pos;
        for (;;) {
            if (b.max_ops-- <= 0) { b.successful = false; return; }
            if (p + 2 > limit) { depth = 0; return; }
            const int v = r.i16_at(p);
            if (v == -1) break;
            if (tc > 1 && p + 2 * std::size_t{tc} > limit) { depth = 0; return; }
            cursor = (cursor + 7) % 8;
            const std::size_t idx = stack[cursor];
            p += 2 * std::size_t{tc};
            if (idx >= b.len()) continue;
            ot::GlyphPos& o = b.pos[idx];
            if (cross_stream) {
                if (v == -0x8000) { o.attach_type = 0; o.attach_chain = 0; o.y_offset = 0; }
                else if (o.attach_type) { o.y_offset += static_cast<float>(v); b.has_attachment = true; }
            } else if (b.info[idx].mask & kern_mask) {
                o.x_advance += static_cast<float>(v);
                o.x_offset += static_cast<float>(v);
            }
        }
    }
};

/// ankr::get_anchor: anchor `i` of glyph `g`, or (0, 0).
inline void ankr_anchor(std::span<const std::uint8_t> ankr, std::uint16_t g, unsigned i, unsigned ng, int& x, int& y) {
    x = y = 0;
    if (ankr.size() < 12) return;
    const ot::Reader a{ankr};
    if (a.u16_at(0) != 0) return;
    const std::uint32_t lookup_off = a.u32_at(4);
    if (!lookup_off) return;
    const auto off = lookup16(a, lookup_off, g, ng, false);
    if (!off) return;
    const std::size_t anchors = std::size_t{a.u32_at(8)} + *off;
    if (anchors + 4 > ankr.size() || i >= a.u32_at(anchors)) return;
    const std::size_t p = anchors + 4 + 4 * std::size_t{i};
    if (p + 4 > ankr.size()) return;
    x = a.i16_at(p);
    y = a.i16_at(p + 2);
}

/// KerxSubTableFormat4::driver_context_t.
struct Format4 {
    static constexpr bool in_place = true;
    const ot::Reader& r;
    std::span<const std::uint8_t> ankr;
    std::size_t ankr_data = 0, limit = 0;
    unsigned action_type = 0, num_glyphs = 0;
    bool reversed = false;   ///< the buffer is reversed from its original order
    bool mark_set = false;
    std::size_t mark = 0;

    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        const unsigned action = m.data(e, 0);
        if (mark_set && action != 0xFFFF && b.idx < b.len()) {
            ot::GlyphPos& o = b.pos[b.idx];
            // Chains are kept in the ORIGINAL buffer order (HarfBuzz 15).
            long chain = static_cast<long>(mark) - static_cast<long>(b.idx);
            if (reversed) chain = -chain;
            if (chain != static_cast<std::int16_t>(chain)) { o.attach_chain = 0; goto set_mark; }
            switch (action_type) {
            case 0:   // control points: hb-ot-font has no contour points, so HarfBuzz bails
                return;
            case 1: {   // anchor points via `ankr`
                const std::size_t p = ankr_data + 4 * std::size_t{action};
                if (p + 4 > limit) return;
                int mx, my, cx, cy;
                ankr_anchor(ankr, b.info[mark].gid, r.u16_at(p), num_glyphs, mx, my);
                ankr_anchor(ankr, b.info[b.idx].gid, r.u16_at(p + 2), num_glyphs, cx, cy);
                o.x_offset = static_cast<float>(mx - cx);
                o.y_offset = static_cast<float>(my - cy);
                break;
            }
            case 2: {   // control point coordinates
                const std::size_t p = ankr_data + 8 * std::size_t{action};
                if (p + 8 > limit) return;
                o.x_offset = static_cast<float>(r.i16_at(p) - r.i16_at(p + 4));
                o.y_offset = static_cast<float>(r.i16_at(p + 2) - r.i16_at(p + 6));
                break;
            }
            default: break;
            }
            o.y_offset += ot::detail::resolve_cross_offset(b.pos, mark, reversed);
            o.attach_type = 1;   // ATTACH_TYPE_MARK
            o.attach_chain = static_cast<std::int16_t>(chain);
            b.has_attachment = true;
        }
    set_mark:
        if (m.flags(e) & 0x8000) { mark_set = true; mark = b.idx; }
    }
};

}  // namespace kerx_detail

/// hb_aat_layout_position: apply every horizontal `kerx` subtable to a
/// horizontal buffer (logical order; `rtl` direction runs reversed).
/// `next(i)` is the kern skippy (next non-mark glyph honouring kern_mask),
/// or npos; `ankr` may be empty.
template <class Next>
inline void apply_kerx(std::span<const std::uint8_t> data, std::span<const std::uint8_t> ankr, ot::Buffer& b,
                       std::uint32_t kern_mask, unsigned num_glyphs, Next&& next) {
    using namespace kerx_detail;
    if (data.size() < 8) return;
    const ot::Reader r{data};
    if (r.u16_at(0) == 0) return;   // has_data
    const bool requested = kern_mask != 0;
    const std::uint32_t count = r.u32_at(4);
    bool seen_cross = false, reversed = false;
    std::size_t pos = 8;
    for (std::uint32_t i = 0; i < count && pos + 12 <= data.size(); ++i) {
        Sub s;
        s.off = pos;
        const std::uint32_t length = r.u32_at(pos);
        s.coverage = r.u32_at(pos + 4);
        s.tuple_count = r.u32_at(pos + 8);
        s.limit = i + 1 < count ? std::min<std::size_t>(data.size(), pos + length) : data.size();
        if (length < 12) break;
        pos += length;
        if (s.coverage & cov_vertical) continue;   // horizontal buffers only
        const unsigned type = s.type();
        if (type != 0 && type != 1 && type != 2 && type != 4 && type != 6) continue;

        // The machine glyph set: the subtable's left set; skip when no
        // buffer glyph is in it.
        std::optional<StateTable> machine;
        if (type == 1 || type == 4) machine.emplace(r, s.off + 12, 6, num_glyphs);
        const auto in_left = [&](std::uint16_t g) -> bool {
            switch (type) {
            case 0: return f0_has_left(r, s, g);
            case 2: return f2_left(r, s, g, num_glyphs);
            case 6: return f6_covers(r, s, 20, g, num_glyphs);
            default: return machine_initial(*machine, g, [&](std::size_t e) {   // 1, 4: Push/Mark or an action
                         return (machine->flags(e) & 0x8000) || machine->data(e, 0) != 0xFFFF; });
            }
        };
        bool hit = false;
        for (const auto& g : b.info) if (in_left(g.gid)) { hit = true; break; }
        if (!hit) continue;

        const bool reverse = ((s.coverage & cov_backwards) != 0) != (b.direction == ot::Direction::rtl);
        if (!seen_cross && s.cross_stream()) {
            seen_cross = true;
            for (auto& p : b.pos) {
                p.attach_type = 2;   // cursive chain
                p.attach_chain = static_cast<std::int16_t>(b.direction == ot::Direction::ltr ? -1 : 1);
            }
        }
        if (reverse != reversed) { b.reverse(); reversed = !reversed; }
        switch (type) {
        case 1:
            if (requested || s.cross_stream()) {
                Format1 c{r, s.off + 12 + r.u32_at(s.off + 28), s.limit, s.tuple_count, kern_mask, s.cross_stream()};
                drive(*machine, b, c);
            }
            break;
        case 4: {
            const std::uint32_t flags = r.u32_at(s.off + 28);
            Format4 c{r, ankr, s.off + 12 + (flags & 0x00FFFFFFu), s.limit, flags >> 30, num_glyphs, reversed};
            drive(*machine, b, c);
            break;
        }
        default: {   // 0, 2, 6: hb_kern_machine_t
            if (!requested || (s.coverage & cov_backwards)) break;
            const auto value = [&](std::uint16_t a, std::uint16_t c) -> int {
                switch (type) {
                case 0: return f0_value(r, s, a, c);
                case 2: return f2_left(r, s, a, num_glyphs) && f2_right(r, s, c, num_glyphs)
                               ? f2_value(r, s, a, c, num_glyphs) : 0;
                default: return f6_covers(r, s, 20, a, num_glyphs) && f6_covers(r, s, 24, c, num_glyphs)
                                ? f6_value(r, s, a, c, num_glyphs) : 0;
                }
            };
            for (std::size_t idx = 0; idx < b.len();) {
                if (!(b.info[idx].mask & kern_mask)) { ++idx; continue; }
                const std::size_t j = next(idx);
                if (j >= b.len()) { ++idx; continue; }
                const int kern = value(b.info[idx].gid, b.info[j].gid);
                if (kern) {
                    if (s.cross_stream()) {
                        b.pos[j].y_offset = static_cast<float>(kern);
                        b.has_attachment = true;
                    } else {
                        const int k1 = kern >> 1, k2 = kern - k1;
                        b.pos[idx].x_advance += static_cast<float>(k1);
                        b.pos[j].x_advance += static_cast<float>(k2);
                        b.pos[j].x_offset += static_cast<float>(k2);
                    }
                }
                idx = j;
            }
            break;
        }
        }
        if (!b.successful) break;
    }
    if (reversed) b.reverse();
}

}  // namespace mayag::typo::aat
