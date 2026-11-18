#pragma once
// mayag::typo::aat — the `kern` table, both dialects, as HarfBuzz applies it
//
// `kern` comes in Microsoft's header (version 0, u16 counts, 6-byte
// subtable headers) and Apple's (version 1.0, u32 counts, 8-byte headers
// with different coverage bits). HarfBuzz's KernTable::apply walks every
// subtable: skip variation and vertical ones, reverse an RTL buffer unless
// the subtable runs Backwards, chain every glyph for the first cross-stream
// subtable, and run the pair machine (hb_kern_machine_t) with the
// subtable's lookup. Formats supported here, as in HarfBuzz:
//
//   0  sorted pairs          (both dialects)
//   2  left/right class tables, byte-offset indexed array (Apple)
//   3  compact class indices (Apple)
//   1  state machine (Apple) — in aat_morx's driver; see apply_format1
//
// A port of HarfBuzz's hb-ot-kern-table.hh / hb-aat-layout-kerx-table.hh
// (Old MIT; see THIRD_PARTY_NOTICES.md).

#include "ot_layout.hpp"

#include <cstdint>
#include <span>

namespace mayag::typo::aat {

struct KernSub {
    std::size_t off = 0, body = 0, end = 0;   ///< subtable start, data after header, end
    std::uint8_t format = 0;
    bool horizontal = true, cross_stream = false, variation = false, backwards = false;
};

/// Enumerate the subtables of a `kern` table.
template <class F>
inline void for_each_kern_subtable(std::span<const std::uint8_t> data, F&& f) {
    const ot::Reader r{data};
    if (data.size() < 4) return;
    const bool apple = r.u16_at(0) == 1;
    std::size_t pos = apple ? 8 : 4;
    const std::uint32_t n = apple ? r.u32_at(4) : r.u16_at(2);
    for (std::uint32_t t = 0; t < n && t < 256 && pos + 6 <= data.size(); ++t) {
        KernSub s;
        s.off = pos;
        std::uint32_t length = 0;
        if (apple) {
            length = r.u32_at(pos);
            const std::uint8_t cov = r.u8_at(pos + 4);
            s.format = r.u8_at(pos + 5);
            s.horizontal = (cov & 0x80) == 0;
            s.cross_stream = (cov & 0x40) != 0;
            s.variation = (cov & 0x20) != 0;
            s.body = pos + 8;
        } else {
            length = r.u16_at(pos + 2);
            s.format = r.u8_at(pos + 4);
            const std::uint8_t cov = r.u8_at(pos + 5);
            s.horizontal = (cov & 0x01) != 0;
            s.cross_stream = (cov & 0x04) != 0;
            s.body = pos + 6;
            // OT subtables are capped at 64 KiB by their u16 length; the last
            // one may overflow it (HarfBuzz: treat it as running to the end).
            if (t + 1 == n) length = static_cast<std::uint32_t>(data.size() - pos);
        }
        if (length < 6) break;
        s.end = std::min<std::size_t>(data.size(), pos + length);
        f(s);
        pos += length;
    }
}

/// Pair value in a subtable (0 = no kerning).
[[nodiscard]] inline int kern_value(const ot::Reader& r, const KernSub& s, std::uint16_t left,
                                    std::uint16_t right, unsigned num_glyphs) noexcept {
    switch (s.format) {
        case 0: {   // nPairs, searchRange, entrySelector, rangeShift, pairs[]
            std::size_t lo = 0, hi = r.u16_at(s.body);
            const std::size_t first = s.body + 8;
            if (first + hi * 6 > s.end) hi = (s.end > first) ? (s.end - first) / 6 : 0;
            const std::uint32_t key = std::uint32_t{left} << 16 | right;
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                const std::uint32_t k = r.u32_at(first + mid * 6);
                if (k == key) return r.i16_at(first + mid * 6 + 4);
                if (k < key) lo = mid + 1; else hi = mid;
            }
            return 0;
        }
        case 2: {   // rowWidth, leftClassTable, rightClassTable, array (offsets from subtable start)
            const auto class_of = [&](std::size_t table, std::uint16_t g) -> unsigned {
                const std::uint16_t first = r.u16_at(table), count = r.u16_at(table + 2);
                const unsigned i = static_cast<unsigned>(g) - first;
                return (g >= first && i < count) ? r.u16_at(table + 4 + std::size_t{i} * 2) : 0u;
            };
            const std::size_t lt = s.off + r.u16_at(s.body + 2), rt = s.off + r.u16_at(s.body + 4);
            const std::size_t arr = s.off + r.u16_at(s.body + 6);
            const std::size_t at = s.off + class_of(lt, left) + class_of(rt, right);
            if (at < arr || at + 2 > s.end) return 0;   // offsetToIndex's guard
            return r.i16_at(at);
        }
        case 3: {   // glyphCount, kernValueCount, leftClassCount, rightClassCount, flags, values, classes, indices
            const std::uint16_t glyphs = r.u16_at(s.body);
            const std::uint8_t nv = r.u8_at(s.body + 2), nl = r.u8_at(s.body + 3), nr = r.u8_at(s.body + 4);
            const std::size_t values = s.body + 6;
            const std::size_t lc = values + std::size_t{nv} * 2, rc = lc + glyphs, idx = rc + glyphs;
            if (left >= glyphs || right >= glyphs) return 0;
            const unsigned l = r.u8_at(lc + left), rr = r.u8_at(rc + right);
            if (l >= nl || rr >= nr) return 0;
            const unsigned vi = r.u8_at(idx + std::size_t{l} * nr + rr);
            return vi < nv ? r.i16_at(values + std::size_t{vi} * 2) : 0;
        }
        default: (void)num_glyphs; return 0;
    }
}

/// True if the table has anything this applier handles.
[[nodiscard]] inline bool has_kern(std::span<const std::uint8_t> data) {
    bool any = false;
    for_each_kern_subtable(data, [&](const KernSub& s) { any |= s.format <= 3; });
    return any;
}

/// hb_ot_layout_has_machine_kerning: any format-1 (state machine) subtable.
[[nodiscard]] inline bool has_machine_kern(std::span<const std::uint8_t> data) {
    bool any = false;
    for_each_kern_subtable(data, [&](const KernSub& s) { any |= s.format == 1; });
    return any;
}

/// hb_ot_layout_has_cross_kerning: any cross-stream subtable.
[[nodiscard]] inline bool has_cross_kern(std::span<const std::uint8_t> data) {
    bool any = false;
    for_each_kern_subtable(data, [&](const KernSub& s) { any |= s.cross_stream; });
    return any;
}

/// Format 1: an Apple state machine over "obsolete" (16-bit) state tables
/// that pushes glyphs and pops kerning actions (KerxSubTableFormat1 with
/// ObsoleteTypes). Header after the subtable header: nClasses u16,
/// classTable u16, stateArray u16, entryTable u16 (offsets from the state
/// table start), then valueTable (kernAction) u16. Entries are {newState
/// as a byte offset into the state array, flags: Push 0x8000, DontAdvance
/// 0x4000, Offset 0x3FFF = byte offset of the action list}.
inline void apply_format1(const ot::Reader& r, const KernSub& s, ot::Buffer& b, std::uint32_t kern_mask) {
    const std::size_t st = s.body;   // the StateTable starts right after the header
    const unsigned n_classes = r.u16_at(st);
    const std::size_t class_table = st + r.u16_at(st + 2);
    const std::size_t state_array = st + r.u16_at(st + 4);
    const std::size_t entry_table = st + r.u16_at(st + 6);
    const std::size_t kern_action = st + r.u16_at(st + 8);
    if (n_classes == 0) return;
    const auto get_class = [&](std::uint16_t g) -> unsigned {
        if (g == 0xFFFF) return 2;   // deleted glyph
        const std::uint16_t first = r.u16_at(class_table), count = r.u16_at(class_table + 2);
        const unsigned i = static_cast<unsigned>(g) - first;
        return (g >= first && i < count) ? r.u8_at(class_table + 4 + i) : 1u;   // 1 = out of bounds
    };
    const auto entry_at = [&](unsigned state, unsigned klass) -> std::size_t {
        if (klass >= n_classes) klass = 1;
        const unsigned e = r.u8_at(state_array + std::size_t{state} * n_classes + klass);
        return entry_table + std::size_t{e} * 4;
    };
    const auto new_state = [&](std::size_t e) -> unsigned {   // byte offset -> state index
        const int ns = r.u16_at(e);
        const int v = (ns - static_cast<int>(state_array - st)) / static_cast<int>(n_classes);
        return v < 0 ? 0u : static_cast<unsigned>(v);
    };
    std::size_t stack[8];
    unsigned depth = 0;
    unsigned state = 0;
    for (b.idx = 0; b.successful;) {
        const unsigned klass = b.idx < b.len() ? get_class(b.cur().gid) : 0u;
        const std::size_t e = entry_at(state, klass);
        const unsigned flags = r.u16_at(e + 2);
        const unsigned next = new_state(e);
        // transition
        if (flags & 0x8000) {
            if (depth < 8) stack[depth++] = b.idx;
            else depth = 0;
        }
        if ((flags & 0x3FFF) && depth) {
            const std::size_t actions0 = st + (flags & 0x3FFF);
            if (actions0 < kern_action) {   // byteOffsetToIndex guard
                depth = 0;
            } else {
                std::size_t at = actions0;
                bool last = false;
                while (!last && depth) {
                    const std::size_t idx = stack[--depth];
                    int v = r.i16_at(at);
                    at += 2;
                    if (idx >= b.len()) continue;
                    last = (v & 1) != 0;
                    v &= ~1;
                    ot::GlyphPos& o = b.pos[idx];
                    if (s.cross_stream) {
                        if (v == -0x8000) { o.attach_type = 0; o.attach_chain = 0; o.y_offset = 0; }
                        else if (o.attach_type) { o.y_offset += static_cast<float>(v); b.has_attachment = true; }
                    } else if (b.info[idx].mask & kern_mask) {
                        o.x_advance += static_cast<float>(v);
                        o.x_offset += static_cast<float>(v);
                    }
                }
            }
        }
        state = next;
        if (b.idx == b.len() || !b.successful) break;
        if (!(flags & 0x4000) || b.max_ops-- <= 0) ++b.idx;
    }
    b.idx = 0;
}

/// KernTable::apply for a horizontal buffer (logical order, `rtl` direction).
/// `next(i)` returns the next non-mark glyph after i (the skippy iterator,
/// ignoring marks, honouring `kern_mask`), or npos.
template <class Next>
inline void apply_kern(std::span<const std::uint8_t> data, ot::Buffer& b, std::uint32_t kern_mask,
                       unsigned num_glyphs, Next&& next) {
    const ot::Reader r{data};
    bool seen_cross = false;
    for_each_kern_subtable(data, [&](const KernSub& s) {
        if (s.variation || !s.horizontal) return;
        if (s.format > 3) return;
        const bool reverse = s.backwards != (b.direction == ot::Direction::rtl);
        if (!seen_cross && s.cross_stream) {
            seen_cross = true;
            for (auto& p : b.pos) {
                p.attach_type = 2;   // cursive chain, like HarfBuzz
                p.attach_chain = static_cast<std::int16_t>(b.direction == ot::Direction::ltr ? -1 : 1);
            }
        }
        if (reverse) b.reverse();
        if (s.format == 1) {
            apply_format1(r, s, b, kern_mask);
            if (reverse) b.reverse();
            return;
        }
        if (s.backwards) { if (reverse) b.reverse(); return; }   // formats 0/2/3 ignore Backwards
        for (std::size_t idx = 0; idx < b.len();) {
            if (!(b.info[idx].mask & kern_mask)) { ++idx; continue; }
            const std::size_t j = next(idx);
            if (j >= b.len()) { ++idx; continue; }
            const int kern = kern_value(r, s, b.info[idx].gid, b.info[j].gid, num_glyphs);
            if (kern) {
                if (s.cross_stream) {
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
        if (reverse) b.reverse();
    });
}

}  // namespace mayag::typo::aat
