// aat_morx.hpp — Apple Advanced Typography `morx` glyph substitution.
//
// Port of HarfBuzz hb-aat-layout-common.hh, hb-aat-layout-morx-table.hh and
// hb-aat-layout.cc (Old MIT license, see THIRD_PARTY_NOTICES.md). Extended
// (morx) tables only. Feature selection uses each chain's defaultFlags.
#pragma once

#include <mayag/font/opentype.hpp>
#include <mayag/font/ot_layout.hpp>
#include <mayag/font/unicode_data.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace mayag::typo::aat {

namespace ot = mayag::typo::ot;

inline constexpr std::uint16_t deleted_glyph = 0xFFFF;   ///< HarfBuzz DELETED_GLYPH
inline constexpr std::uint8_t uflag_aat_deleted = 0x80;  ///< UPROPS_MASK_Cf_AAT_DELETED

/// AAT Lookup<T>::get_value (formats 0/2/4/6/8) and, when `allow_format10`,
/// get_value_or_null's format 10. `W` is sizeof(T): 2 (HBUINT16) or 4 (HBUINT32).
template <unsigned W>
inline std::optional<std::uint32_t> lookup(const ot::Reader& r, std::size_t t, std::uint16_t gid,
                                           unsigned num_glyphs, bool allow_format10 = true) {
    static_assert(W == 2 || W == 4);
    const auto value_at = [&](std::size_t p) -> std::uint32_t { return W == 2 ? r.u16_at(p) : r.u32_at(p); };
    const std::uint16_t format = r.u16_at(t);
    switch (format) {
    case 0:
        if (gid >= num_glyphs) return std::nullopt;
        return value_at(t + 2 + std::size_t{W} * gid);
    case 2: case 4: case 6: {
        const std::size_t unit = r.u16_at(t + 2);
        std::size_t n = r.u16_at(t + 4);
        const std::size_t base = t + 12;
        if (unit < (format == 6 ? 2u + W : format == 2 ? 4u + W : 6u)) return std::nullopt;
        // last_is_terminator: 0xFFFF in the first 1 (single) or 2 (segment) words.
        if (n) {
            const std::size_t last = base + (n - 1) * unit;
            bool term = r.u16_at(last) == 0xFFFF;
            if (format != 6) term = term && r.u16_at(last + 2) == 0xFFFF;
            if (term) --n;
        }
        std::size_t lo = 0, hi = n;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const std::size_t u = base + mid * unit;
            int c;
            if (format == 6) {
                const std::uint16_t g = r.u16_at(u);
                c = gid < g ? -1 : gid > g ? 1 : 0;
            } else {
                const std::uint16_t lastg = r.u16_at(u), first = r.u16_at(u + 2);
                c = gid < first ? -1 : gid <= lastg ? 0 : 1;
            }
            if (c < 0) hi = mid;
            else if (c > 0) lo = mid + 1;
            else {
                if (format == 6) return value_at(u + 2);
                if (format == 2) return value_at(u + 4);
                const std::uint16_t first = r.u16_at(u + 2);
                return value_at(t + r.u16_at(u + 4) + std::size_t{W} * (gid - first));
            }
        }
        return std::nullopt;
    }
    case 8: {
        const std::uint16_t first = r.u16_at(t + 2), count = r.u16_at(t + 4);
        if (first <= gid && gid - first < count) return value_at(t + 6 + std::size_t{W} * (gid - first));
        return std::nullopt;
    }
    case 10: {
        if (!allow_format10) return std::nullopt;
        const std::uint16_t vs = r.u16_at(t + 2), first = r.u16_at(t + 4), count = r.u16_at(t + 6);
        if (!(first <= gid && gid - first < count)) return std::uint32_t{0};
        std::uint32_t v = 0;
        const std::size_t p = t + 8 + std::size_t(gid - first) * vs;
        for (unsigned i = 0; i < vs && i < 4; ++i) v = (v << 8) | r.u8_at(p + i);
        return v;
    }
    default: return std::nullopt;
    }
}

inline std::optional<std::uint16_t> lookup16(const ot::Reader& r, std::size_t t, std::uint16_t gid,
                                             unsigned num_glyphs, bool allow_format10 = true) {
    const auto v = lookup<2>(r, t, gid, num_glyphs, allow_format10);
    if (!v) return std::nullopt;
    return static_cast<std::uint16_t>(*v);
}

/// Lookup::collect_glyphs membership: does the lookup cover `gid`?
template <unsigned W>
[[nodiscard]] inline bool lookup_covers(const ot::Reader& r, std::size_t t, std::uint16_t gid, unsigned num_glyphs) {
    if (r.u16_at(t) == 10) {
        const std::uint16_t first = r.u16_at(t + 4), count = r.u16_at(t + 6);
        return first <= gid && gid - first < count;
    }
    return lookup<W>(r, t, gid, num_glyphs, false).has_value();
}

inline constexpr unsigned class_end_of_text = 0, class_out_of_bounds = 1, class_deleted = 2;
inline constexpr std::uint16_t flag_dont_advance = 0x4000;

/// Extended StateTable<ExtendedTypes, Extra>; `entry_size` = 4 + sizeof(Extra).
struct StateTable {
    ot::Reader r;
    std::size_t base = 0;
    std::uint32_t n_classes = 0;
    std::size_t class_table = 0, state_array = 0, entry_table = 0;
    std::size_t entry_size = 4;
    unsigned num_glyphs = 0;

    StateTable(const ot::Reader& rr, std::size_t b, std::size_t esize, unsigned ng)
        : r(rr), base(b), n_classes(rr.u32_at(b)), class_table(b + rr.u32_at(b + 4)),
          state_array(b + rr.u32_at(b + 8)), entry_table(b + rr.u32_at(b + 12)), entry_size(esize),
          num_glyphs(ng) {}

    [[nodiscard]] unsigned get_class(std::uint16_t gid) const {
        if (gid == deleted_glyph) return class_deleted;
        const auto v = lookup16(r, class_table, gid, num_glyphs, false);
        return v ? *v : class_out_of_bounds;
    }
    /// Byte offset of the entry for (state, klass).
    [[nodiscard]] std::size_t entry(unsigned state, unsigned klass) const {
        if (klass >= n_classes) klass = class_out_of_bounds;
        const std::size_t e = r.u16_at(state_array + 2 * (std::size_t(state) * n_classes + klass));
        return entry_table + e * entry_size;
    }
    [[nodiscard]] std::uint16_t new_state(std::size_t e) const { return r.u16_at(e); }
    [[nodiscard]] std::uint16_t flags(std::size_t e) const { return r.u16_at(e + 2); }
    [[nodiscard]] std::uint16_t data(std::size_t e, unsigned i) const { return r.u16_at(e + 4 + 2 * i); }
};

/// StateTableDriver::drive. Ctx provides `static constexpr bool in_place`
/// and `void transition(Buffer&, const StateTable&, std::size_t entry)`.
template <class Ctx>
void drive(const StateTable& m, ot::Buffer& b, Ctx& c) {
    if (!Ctx::in_place) b.clear_output();
    unsigned state = 0;
    for (b.idx = 0; b.successful;) {
        const unsigned klass = b.idx < b.len() ? m.get_class(b.cur().gid) : class_end_of_text;
        const std::size_t e = m.entry(state, klass);
        const unsigned next = m.new_state(e);
        c.transition(b, m, e);
        state = next;
        if (b.idx == b.len() || !b.successful) break;
        if (!(m.flags(e) & flag_dont_advance) || b.max_ops-- <= 0) b.next_glyph();
    }
    if (!Ctx::in_place) b.sync();
}

namespace detail {

inline constexpr unsigned max_context = 64;   // HB_MAX_CONTEXT_LENGTH

inline void mark_deleted(ot::GlyphInfo& g) {
    g.gc = static_cast<std::uint8_t>(ucd::GeneralCategory::Cf);
    g.uflags = static_cast<std::uint8_t>(g.uflags | uflag_aat_deleted | ot::uflag::hidden);
}

/// replace_glyph_inplace: a lookup that maps to the deleted glyph deletes it.
inline void replace_inplace(ot::GlyphInfo& g, std::uint16_t v) {
    g.gid = v;
    if (v == deleted_glyph) mark_deleted(g);
}

struct Rearrangement {
    static constexpr bool in_place = true;
    std::size_t start = 0, end = 0;
    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        const unsigned flags = m.flags(e);
        if (flags & 0x8000) start = b.idx;
        if (flags & 0x2000) end = std::min(b.idx + 1, b.len());
        if ((flags & 0xF) && start < end) {
            static constexpr unsigned char map[16] = {0x00, 0x10, 0x01, 0x11, 0x20, 0x30, 0x02, 0x03,
                                                      0x12, 0x13, 0x21, 0x31, 0x22, 0x32, 0x23, 0x33};
            const unsigned mm = map[flags & 0xF];
            const unsigned l = std::min(2u, mm >> 4), r = std::min(2u, mm & 0x0Fu);
            const bool rev_l = 3 == (mm >> 4), rev_r = 3 == (mm & 0x0F);
            if (end - start >= l + r && end - start <= max_context) {
                b.merge_clusters(start, std::min(b.idx + 1, b.len()));
                b.merge_clusters(start, end);
                auto& info = b.info;
                ot::GlyphInfo buf[4];
                for (unsigned i = 0; i < l; ++i) buf[i] = info[start + i];
                for (unsigned i = 0; i < r; ++i) buf[2 + i] = info[end - r + i];
                if (l != r) {
                    const auto first = info.begin() + static_cast<std::ptrdiff_t>(start + l);
                    const auto last = info.begin() + static_cast<std::ptrdiff_t>(end - r);
                    if (r > l) std::move_backward(first, last, last + (r - l));
                    else std::move(first, last, first - (l - r));
                }
                for (unsigned i = 0; i < r; ++i) info[start + i] = buf[2 + i];
                for (unsigned i = 0; i < l; ++i) info[end - l + i] = buf[i];
                if (rev_l) std::swap(info[end - 1], info[end - 2]);
                if (rev_r) std::swap(info[start], info[start + 1]);
            }
        }
    }
};

struct Contextual {
    static constexpr bool in_place = true;
    std::size_t subs = 0;   // absolute offset of the substitution-table offset list
    bool mark_set = false;
    std::size_t mark = 0;
    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        if (b.idx == b.len() && !mark_set) return;
        const std::uint16_t mark_index = m.data(e, 0), cur_index = m.data(e, 1);
        if (mark_index != 0xFFFF && mark < b.len()) {
            const std::size_t lk = subs + m.r.u32_at(subs + 4u * mark_index);
            if (auto v = lookup16(m.r, lk, b.info[mark].gid, m.num_glyphs, false)) replace_inplace(b.info[mark], *v);
        }
        const std::size_t i = std::min(b.idx, b.len() - 1);
        if (cur_index != 0xFFFF) {
            const std::size_t lk = subs + m.r.u32_at(subs + 4u * cur_index);
            if (auto v = lookup16(m.r, lk, b.info[i].gid, m.num_glyphs, false)) replace_inplace(b.info[i], *v);
        }
        if (m.flags(e) & 0x8000) { mark_set = true; mark = b.idx; }
    }
};

inline bool in_range(const ot::Reader& r, std::size_t off, std::size_t n) { return off + n <= r.size(); }

// hb_aat_apply_context_t::output_glyphs (cur() at end of text: the last output glyph).
inline void output_glyphs(ot::Buffer& b, const ot::Reader& r, std::size_t at, unsigned count) {
    for (unsigned i = 0; i < count && b.successful; ++i) {
        const std::uint16_t g = r.u16_at(at + 2u * i);
        if (b.idx < b.len()) {
            if (g == deleted_glyph) mark_deleted(b.cur());
            b.output_glyph(g);
        } else {
            if (b.out.empty()) return;
            if (b.max_len && b.out.size() >= b.max_len) { b.successful = false; return; }
            b.out.push_back(b.out.back());
            b.out.back().gid = g;
            if (g == deleted_glyph) mark_deleted(b.out.back());
        }
    }
}

struct Ligature {
    static constexpr bool in_place = false;
    std::size_t lig_action = 0, component = 0, ligature = 0;
    unsigned match_positions[max_context] = {};
    unsigned match_length = 0;
    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        const ot::Reader& r = m.r;
        const unsigned flags = m.flags(e);
        const auto out_len = static_cast<unsigned>(b.out_len());
        if (flags & 0x8000) {
            if (match_length && match_positions[(match_length - 1u) % max_context] == out_len) match_length--;
            match_positions[match_length++ % max_context] = out_len;
        }
        if (!(flags & 0x2000)) return;
        const unsigned end = out_len;
        if (!match_length) return;
        if (b.idx >= b.len()) return;
        unsigned cursor = match_length;
        std::size_t action_at = lig_action + 4u * m.data(e, 0);
        std::uint32_t ligature_idx = 0;
        std::uint32_t action = 0;
        do {
            if (!cursor) { match_length = 0; break; }
            if (!b.move_to(match_positions[--cursor % max_context])) return;
            if (!in_range(r, action_at, 4)) break;
            action = r.u32_at(action_at);
            std::uint32_t uoffset = action & 0x3FFFFFFFu;
            if (uoffset & 0x20000000u) uoffset |= 0xC0000000u;
            const std::uint32_t component_idx = b.cur().gid + uoffset;
            const std::size_t cat = component + 2u * std::size_t(component_idx);
            if (std::size_t(component_idx) * 2u >= r.size() || !in_range(r, cat, 2)) break;
            ligature_idx += r.u16_at(cat);
            if (action & 0xC0000000u) {
                const std::size_t lat = ligature + 2u * std::size_t(ligature_idx);
                if (std::size_t(ligature_idx) * 2u >= r.size() || !in_range(r, lat, 2)) break;
                const std::uint16_t lig = r.u16_at(lat);
                if (b.idx >= b.len()) return;
                b.replace_glyph(lig);
                const unsigned lig_end = match_positions[(match_length - 1u) % max_context] + 1u;
                while (match_length - 1u > cursor) {
                    if (!b.move_to(match_positions[--match_length % max_context])) return;
                    if (b.idx >= b.len()) return;
                    mark_deleted(b.cur());
                    b.replace_glyph(deleted_glyph);
                }
                if (!b.move_to(lig_end)) return;
                b.merge_out_clusters(match_positions[cursor % max_context], b.out_len());
            }
            action_at += 4;
        } while (!(action & 0x80000000u));
        b.move_to(end);
    }
};

struct Insertion {
    static constexpr bool in_place = false;
    std::size_t actions = 0;
    std::size_t mark = 0;
    void insert(ot::Buffer& b, const ot::Reader& r, unsigned index, unsigned count, bool before) {
        std::size_t at = actions + 2u * index;
        if (!in_range(r, at, 2u * count)) count = 0;
        if (b.idx < b.len() && !before) b.out.push_back(b.info[b.idx]);   // copy_glyph
        output_glyphs(b, r, at, count);
        if (b.idx < b.len() && !before) b.skip_glyph();
    }
    void transition(ot::Buffer& b, const StateTable& m, std::size_t e) {
        const unsigned flags = m.flags(e);
        const std::size_t mark_loc = b.out_len();
        const std::uint16_t cur_index = m.data(e, 0), marked_index = m.data(e, 1);
        if (marked_index != 0xFFFF) {
            const unsigned count = flags & 0x001F;
            if ((b.max_ops -= count) <= 0) return;
            const std::size_t end = b.out_len();
            if (!b.move_to(mark)) return;
            insert(b, m.r, marked_index, count, flags & 0x0400);
            if (!b.successful || !b.move_to(end + count)) return;
        }
        if (flags & 0x8000) mark = mark_loc;
        if (cur_index != 0xFFFF) {
            const unsigned count = (flags & 0x03E0) >> 5;
            if ((b.max_ops -= count) <= 0) return;
            const std::size_t end = b.out_len();
            insert(b, m.r, cur_index, count, flags & 0x0800);
            if (!b.successful) return;
            b.move_to((flags & flag_dont_advance) ? end : end + count);
        }
    }
};

}  // namespace detail

/// Apply every morx chain (defaultFlags) to `b` (gids from cmap).
inline void substitute(std::span<const std::uint8_t> morx, ot::Buffer& b, unsigned num_glyphs) {
    const ot::Reader r{morx};
    if (r.u16_at(0) < 2) return;
    ot::set_budget(b);
    const bool backward = b.direction == ot::Direction::rtl;
    std::size_t chain = 8;
    const std::uint32_t n_chains = r.u32_at(4);
    for (std::uint32_t ci = 0; ci < n_chains && chain + 16 <= r.size(); ++ci) {
        const std::uint32_t flags = r.u32_at(chain);
        const std::uint32_t chain_len = r.u32_at(chain + 4);
        const std::uint32_t n_features = r.u32_at(chain + 8), n_subtables = r.u32_at(chain + 12);
        std::size_t st = chain + 16 + 12u * std::size_t(n_features);
        for (std::uint32_t si = 0; si < n_subtables && st + 12 <= r.size(); ++si) {
            const std::uint32_t len = r.u32_at(st), coverage_raw = r.u32_at(st + 4);
            const std::uint32_t sub_flags = r.u32_at(st + 8);
            const unsigned coverage = coverage_raw >> 24, type = coverage_raw & 0xFF;
            const std::size_t body = st + 12;
            if (len < 12) break;
            if ((sub_flags & flags) && ((coverage & 0x20) || !(coverage & 0x80))) {
                const bool reverse = (coverage & 0x10) ? bool(coverage & 0x40) : bool(coverage & 0x40) != backward;
                if (reverse) b.reverse();
                switch (type) {
                case 0: {
                    StateTable m{r, body, 4, num_glyphs};
                    detail::Rearrangement c;
                    drive(m, b, c);
                    break;
                }
                case 1: {
                    StateTable m{r, body, 8, num_glyphs};
                    detail::Contextual c;
                    c.subs = body + r.u32_at(body + 16);
                    drive(m, b, c);
                    break;
                }
                case 2: {
                    StateTable m{r, body, 6, num_glyphs};
                    detail::Ligature c;
                    c.lig_action = body + r.u32_at(body + 16);
                    c.component = body + r.u32_at(body + 20);
                    c.ligature = body + r.u32_at(body + 24);
                    drive(m, b, c);
                    break;
                }
                case 4:
                    for (auto& g : b.info)
                        if (auto v = lookup16(r, body, g.gid, num_glyphs, false)) detail::replace_inplace(g, *v);
                    break;
                case 5: {
                    StateTable m{r, body, 8, num_glyphs};
                    detail::Insertion c;
                    c.actions = body + r.u32_at(body + 16);
                    drive(m, b, c);
                    break;
                }
                default: break;
                }
                if (reverse) b.reverse();
                if (!b.successful) return;
            }
            st += len;
        }
        if (chain_len < 16) break;
        chain += chain_len;
    }
}

/// hb_aat_layout_remove_deleted_glyphs (hb_buffer_t::delete_glyphs_inplace).
inline void remove_deleted_glyphs(ot::Buffer& b) {
    auto& info = b.info;
    const bool has_pos = b.pos.size() == info.size();
    std::size_t j = 0;
    const std::size_t count = info.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (info[i].uflags & uflag_aat_deleted) {
            const std::uint32_t cluster = info[i].cluster;
            if (i + 1 < count && cluster == info[i + 1].cluster) continue;
            if (j) {
                if (cluster < info[j - 1].cluster) {
                    const std::uint32_t old = info[j - 1].cluster;
                    for (std::size_t k = j; k && info[k - 1].cluster == old; --k) info[k - 1].cluster = cluster;
                }
                continue;
            }
            if (i + 1 < count) b.merge_clusters(i, i + 2);
            continue;
        }
        if (j != i) {
            info[j] = info[i];
            if (has_pos) b.pos[j] = b.pos[i];
        }
        ++j;
    }
    info.resize(j);
    if (has_pos) b.pos.resize(j);
}

}  // namespace mayag::typo::aat
