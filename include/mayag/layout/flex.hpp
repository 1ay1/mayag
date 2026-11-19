#pragma once
// mayag::layout — flexbox
//
// A from-scratch flex engine, ~400 lines, no Yoga dependency. It implements
// the parts of CSS flexbox that a GPU UI actually needs:
//
//   * main/cross axis with grow, shrink, and basis
//   * justify (6 modes) and align (5 modes) including stretch and baseline
//   * gap, padding, margin
//   * min/max clamping applied where the spec says (after flexing, before
//     positioning) rather than where it is convenient
//   * absolute/fixed children resolved against the padding box, out of flow
//   * wrapping into lines
//
// The pass is: measure (bottom-up intrinsic sizes) then arrange (top-down
// final rects). Both are pure functions over the tree; nothing global.

#include "../scene/node.hpp"
#include "../scene/cascade.hpp"
#include "../style/style.hpp"
#include "text_metrics.hpp"

#include <span>
#include <vector>

namespace mayag::layout {

/// Available space for a measurement. An "unbounded" axis (infinite) means
/// "size to content", which is how intrinsic sizing is requested.
struct Constraints {
    float max_width  = num::inf;
    float max_height = num::inf;

    [[nodiscard]] constexpr float axis_max(Axis a) const noexcept {
        return a == Axis::horizontal ? max_width : max_height;
    }
    [[nodiscard]] static constexpr Constraints tight(Vec2 s) noexcept { return {s.x, s.y}; }
    [[nodiscard]] static constexpr Constraints unbounded() noexcept { return {}; }
};

namespace detail {

[[nodiscard]] constexpr float main_of(Vec2 v, Axis a) noexcept {
    return a == Axis::horizontal ? v.x : v.y;
}
[[nodiscard]] constexpr float cross_of(Vec2 v, Axis a) noexcept {
    return a == Axis::horizontal ? v.y : v.x;
}
[[nodiscard]] constexpr Vec2 compose(Axis a, float main, float cross) noexcept {
    return a == Axis::horizontal ? Vec2{main, cross} : Vec2{cross, main};
}

/// Clamp against min/max, resolving percentages against the parent extent.
[[nodiscard]] constexpr float clamp_axis(float value, Length min_len, Length max_len,
                                         float parent_extent) noexcept {
    const float lo = min_len.resolve(parent_extent, 0.0f);
    const float hi = max_len.resolve(parent_extent, num::inf);
    return num::clamp(value, lo, num::max(lo, hi));
}

[[nodiscard]] inline bool in_flow(const Node& n) noexcept {
    return n.style().layout.position == Positioning::flow;
}

}  // namespace detail

// ── measurement ────────────────────────────────────────────────────────────────────────

namespace detail {
/// One layout pass. measure() is pure for a fixed tree, and arrange
/// re-measures subtrees its parents already measured, so layout_tree() wraps
/// the caller's measurer in this and measure() memoises per node, keyed on
/// the pass's generation. Plain measurers (no pass) skip the memo.
class PassMeasurer final : public TextMeasurer {
  public:
    PassMeasurer(const TextMeasurer& inner, std::uint32_t gen) noexcept : inner_{inner}, gen_{gen} {}
    [[nodiscard]] Vec2 measure(std::string_view s, const TextStyle& st, float w) const override {
        return inner_.measure(s, st, w);
    }
    [[nodiscard]] float advance(std::string_view s, const TextStyle& st) const override {
        return inner_.advance(s, st);
    }
    [[nodiscard]] float ascent(const TextStyle& st) const override { return inner_.ascent(st); }
    [[nodiscard]] std::uint32_t layout_pass() const noexcept override { return gen_; }

  private:
    const TextMeasurer& inner_;
    std::uint32_t       gen_;
};
[[nodiscard]] Vec2 measure_uncached(const Node& node, Constraints c, const TextMeasurer& tm);
}  // namespace detail

/// Intrinsic size of a subtree under `c`, ignoring grow/shrink.
/// Pure; memoised per node during a layout_tree() pass.
[[nodiscard]] inline Vec2 measure(const Node& node, Constraints c, const TextMeasurer& tm) {
    // A memo root keeps its sizes across frames: its subtree doesn't change
    // while it's reused, so neither do they.
    if (node.memo_key() != 0) {
        if (const auto* ms = node.memo_size(c.max_width, c.max_height); ms != nullptr && node.memo_reused())
            return ms->out;
    }
    const std::uint32_t g = tm.layout_pass();
    if (g == 0) return detail::measure_uncached(node, c, tm);
    auto& m = node.measure_memo();
    for (int i = 0; i < 2; ++i)
        if (m.gen[i] == g && m.tm[i] == &tm && m.cw[i] == c.max_width && m.ch[i] == c.max_height)
            return m.out[i];
    const Vec2 r = detail::measure_uncached(node, c, tm);
    const int slot = m.next;
    m.next = static_cast<std::uint8_t>(slot ^ 1);
    m.gen[slot] = g;
    m.tm[slot] = &tm;
    m.cw[slot] = c.max_width;
    m.ch[slot] = c.max_height;
    m.out[slot] = r;
    if (node.memo_key() != 0) node.add_memo_size(c.max_width, c.max_height, r);
    return r;
}

[[nodiscard]] inline Vec2 detail::measure_uncached(const Node& node, Constraints c, const TextMeasurer& tm) {
    const Style& st = node.style();
    const LayoutStyle& L = st.layout;

    // An explicit size short-circuits everything below it, but percentages
    // still need the parent extent, hence the resolve-with-fallback dance.
    const float explicit_w = L.width.resolve(c.max_width, num::inf);
    const float explicit_h = L.height.resolve(c.max_height, num::inf);

    Vec2 content{};

    switch (node.kind()) {
        case NodeKind::text: {
            const float avail = num::isfinite_or(explicit_w, c.max_width) - L.padding.horizontal();
            content = tm.measure(node.text(), st.text, avail);
            break;
        }
        case NodeKind::image: {
            content = {64.0f, 64.0f};   // replaced by the real texture extent when bound
            break;
        }
        case NodeKind::spacer:
        case NodeKind::canvas: {
            content = {0.0f, 0.0f};
            break;
        }
        case NodeKind::box: {
            const Axis  ax  = L.axis;
            const float pad_main  = detail::main_of(L.padding.total(), ax);
            const float pad_cross = detail::cross_of(L.padding.total(), ax);

            const float avail_main =
                num::isfinite_or(detail::main_of({explicit_w, explicit_h}, ax),
                                 detail::main_of({c.max_width, c.max_height}, ax)) - pad_main;

            Constraints child_c = c;
            if (num::is_inf(explicit_w)) child_c.max_width  -= L.padding.horizontal();
            else                         child_c.max_width   = explicit_w - L.padding.horizontal();
            if (num::is_inf(explicit_h)) child_c.max_height -= L.padding.vertical();
            else                         child_c.max_height  = explicit_h - L.padding.vertical();

            float sum_main = 0.0f;
            float max_cross = 0.0f;
            int   flow_kids = 0;

            // A wrapping container with a bounded main axis measures as the
            // LINES arrange() will break it into (same greedy rule): main =
            // the longest line, cross = every line stacked with the gap.
            // Measuring it as one line made its frame one line tall while
            // arrange() placed later lines below it -- outside the parent's
            // frame, where the hit test (which prunes by frame) never looks,
            // so every control past the first line was dead to the pointer.
            const bool wrapping = L.wrap && num::isfinite_or(avail_main, -1.0f) > 0.0f;
            float line_main = 0.0f, line_cross = 0.0f, lines_cross = 0.0f, longest = 0.0f;
            int   in_line = 0, lines = 0;

            for (const auto& kid : node.children()) {
                if (!detail::in_flow(kid)) continue;   // out of flow: no contribution
                const Vec2 ks = measure(kid, child_c, tm);
                const Insets km = kid.style().layout.margin;
                const float km_main = detail::main_of(ks + km.total(), ax);
                const float km_cross = detail::cross_of(ks + km.total(), ax);
                sum_main  += km_main;
                max_cross  = num::max(max_cross, km_cross);
                ++flow_kids;
                if (wrapping) {
                    const float with_gap = (in_line > 0 ? L.gap : 0.0f) + km_main;
                    if (in_line > 0 && line_main + with_gap > avail_main + 0.01f) {
                        longest = num::max(longest, line_main);
                        lines_cross += line_cross + (lines > 0 ? L.gap : 0.0f);
                        ++lines;
                        line_main = km_main;
                        line_cross = km_cross;
                        in_line = 1;
                    } else {
                        line_main += with_gap;
                        line_cross = num::max(line_cross, km_cross);
                        ++in_line;
                    }
                }
            }
            if (flow_kids > 1) sum_main += L.gap * static_cast<float>(flow_kids - 1);

            if (wrapping && flow_kids > 0) {
                longest = num::max(longest, line_main);
                lines_cross += line_cross + (lines > 0 ? L.gap : 0.0f);
                content = detail::compose(ax, longest + pad_main, lines_cross + pad_cross);
            } else {
                content = detail::compose(ax, sum_main + pad_main, max_cross + pad_cross);
            }
            break;
        }
    }

    // Text/image add their own padding; box already included it.
    if (node.kind() != NodeKind::box) {
        content = content + L.padding.total();
    }

    float w = num::is_inf(explicit_w) ? content.x : explicit_w;
    float h = num::is_inf(explicit_h) ? content.y : explicit_h;

    w = detail::clamp_axis(w, L.min_width,  L.max_width,  c.max_width);
    h = detail::clamp_axis(h, L.min_height, L.max_height, c.max_height);
    return {w, h};
}

// ── arrangement ─────────────────────────────────────────────────────────

namespace detail {

/// One wrap line: a half-open child index range plus its accumulated extents.
struct Line {
    std::size_t begin = 0, end = 0;
    float main_size = 0.0f;
    float cross_size = 0.0f;
    float total_grow = 0.0f;
    float total_shrink = 0.0f;
};

/// Distribute leftover space per justify mode. Returns the leading offset and
/// the extra spacing to insert between adjacent items.
struct Distribution { float lead = 0.0f, between = 0.0f; };

[[nodiscard]] constexpr Distribution distribute(Justify j, float free, std::size_t count) noexcept {
    if (free <= 0.0f || count == 0) return {};
    const auto n = static_cast<float>(count);
    switch (j) {
        case Justify::start:         return {0.0f, 0.0f};
        case Justify::center:        return {free * 0.5f, 0.0f};
        case Justify::end:           return {free, 0.0f};
        case Justify::space_between: return {0.0f, count > 1 ? free / (n - 1.0f) : 0.0f};
        case Justify::space_around:  { const float u = free / n; return {u * 0.5f, u}; }
        case Justify::space_evenly:  { const float u = free / (n + 1.0f); return {u, u}; }
    }
    return {};
}

[[nodiscard]] constexpr float align_offset(Align a, float free) noexcept {
    switch (a) {
        case Align::start:
        case Align::stretch:
        case Align::baseline: return 0.0f;
        case Align::center:   return free * 0.5f;
        case Align::end:      return free;
    }
    return 0.0f;
}

/// The distance from a laid-out child's top to its text baseline, used by
/// Align::baseline. For a text node it's the font ascent at its style; for a
/// non-text box there is no baseline, so we use its full height (its bottom
/// sits on the row baseline, the CSS "last-line / bottom" convention). This is
/// what lets a big heading and small caption in one row share a baseline.
[[nodiscard]] inline float ascent_of(const Node& n, const TextMeasurer& tm,
                                     float laid_out_height) noexcept {
    if (n.kind() == NodeKind::text) return tm.ascent(n.style().text);
    return laid_out_height;
}

}  // namespace detail

/// Assign final rects to `node` and its whole subtree. `frame` is the absolute
/// rect the node occupies, including its own padding but not its margin.
inline void arrange(Node& node, const Rect& frame, const TextMeasurer& tm);

namespace detail {

inline void arrange_children(Node& node, const Rect& content, const TextMeasurer& tm) {
    const LayoutStyle& L = node.style().layout;
    const Axis ax = L.axis;

    const float avail_main  = main_of(content.size, ax);
    const float avail_cross = cross_of(content.size, ax);

    auto& kids = node.children();

    // ---- pass 1: measure flow children, collect flex factors -----------
    struct Item {
        std::size_t index;
        Vec2   base;         ///< hypothetical size
        float  main;         ///< working main size (mutated by flex)
        Insets margin;

        /// Leading margin as a Vec2 in (main, cross) order for `ax`, so the
        /// positioning loop stays axis-agnostic instead of branching twice.
        [[nodiscard]] constexpr Vec2 margin_lead(Axis ax) const noexcept {
            return ax == Axis::horizontal ? Vec2{margin.left, margin.top}
                                          : Vec2{margin.top, margin.left};
        }
        [[nodiscard]] constexpr Vec2 margin_trail(Axis ax) const noexcept {
            return ax == Axis::horizontal ? Vec2{margin.right, margin.bottom}
                                          : Vec2{margin.bottom, margin.right};
        }
    };
    // Up to 32 children live on the stack; bigger containers use the heap.
    // Lines below are spans into this one buffer, so nothing is copied.
    Item inline_items[32];
    std::vector<Item> heap_items;
    Item* store = inline_items;
    if (kids.size() > 32) { heap_items.resize(kids.size()); store = heap_items.data(); }
    std::size_t n_items = 0;

    const Constraints cc = Constraints::tight(content.size);

    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (!in_flow(kids[i])) continue;
        const Vec2 base = measure(kids[i], cc, tm);
        store[n_items++] = Item{i, base, main_of(base, ax), kids[i].style().layout.margin};
    }
    const std::span<Item> all_items{store, n_items};

    if (!all_items.empty()) {
        // Partition into lines. Without wrap that's one line holding every item;
        // with wrap, greedily break when the next item would overflow the main
        // axis. Each line then runs the full grow/shrink/justify/align machinery
        // independently and stacks along the cross axis.
        struct Slice { std::size_t lo, hi; };
        Slice inline_lines[8];
        std::vector<Slice> heap_lines;
        std::size_t n_lines = 0;
        const auto add_line = [&](Slice s) {
            if (n_lines < 8) inline_lines[n_lines] = s;
            else {
                if (heap_lines.empty()) heap_lines.assign(inline_lines, inline_lines + 8);
                heap_lines.push_back(s);
            }
            ++n_lines;
        };
        if (L.wrap && avail_main > 0.0f) {
            std::size_t start = 0;
            float line_main = 0.0f;
            for (std::size_t i = 0; i < all_items.size(); ++i) {
                const float w = all_items[i].main + main_of(all_items[i].margin.total(), ax);
                const float with_gap = (i > start ? L.gap : 0.0f) + w;
                if (i > start && line_main + with_gap > avail_main + 0.01f) {
                    add_line({start, i});
                    start = i;
                    line_main = w;
                } else {
                    line_main += with_gap;
                }
            }
            add_line({start, all_items.size()});
        } else {
            add_line({0, all_items.size()});
        }
        const std::span<const Slice> lines = n_lines <= 8
            ? std::span<const Slice>{inline_lines, n_lines}
            : std::span<const Slice>{heap_lines};

        float cross_cursor = 0.0f;   // accumulated cross offset across lines
        for (const Slice& slice : lines) {
        const std::span<Item> items = all_items.subspan(slice.lo, slice.hi - slice.lo);
        float line_cross = 0.0f;   // tallest child in this line (cross axis)
        const float gaps = L.gap * static_cast<float>(items.size() - 1);
        float used = gaps;
        float grow_sum = 0.0f, shrink_sum = 0.0f;
        for (const auto& it : items) {
            used += it.main + main_of(it.margin.total(), ax);
            grow_sum   += kids[it.index].style().layout.grow;
            shrink_sum += kids[it.index].style().layout.shrink;
        }

        // ---- pass 2: resolve flexible lengths --------------------------
        const float free = avail_main - used;

        if (free > 0.0f && grow_sum > 0.0f) {
            for (auto& it : items) {
                const float g = kids[it.index].style().layout.grow;
                if (g > 0.0f) it.main += free * (g / grow_sum);
            }
        } else if (free < 0.0f && shrink_sum > 0.0f) {
            // Weight shrinkage by base size, per spec: a big item absorbs more
            // of the overflow than a small one with the same shrink factor.
            float weighted = 0.0f;
            for (const auto& it : items)
                weighted += kids[it.index].style().layout.shrink * it.main;
            if (weighted > 0.0f) {
                for (auto& it : items) {
                    const float s = kids[it.index].style().layout.shrink;
                    it.main = num::max(it.main + free * (s * it.main / weighted), 0.0f);
                }
            }
        }

        // ---- pass 3: clamp after flexing (spec order) ------------------
        for (auto& it : items) {
            const LayoutStyle& kl = kids[it.index].style().layout;
            it.main = (ax == Axis::horizontal)
                ? clamp_axis(it.main, kl.min_width,  kl.max_width,  avail_main)
                : clamp_axis(it.main, kl.min_height, kl.max_height, avail_main);
        }

        // ---- pass 4: position ------------------------------------------
        float consumed = gaps;
        for (const auto& it : items) consumed += it.main + main_of(it.margin.total(), ax);
        const auto dist = distribute(L.justify, avail_main - consumed, items.size());

        float cursor = main_of(content.origin, ax) + dist.lead;

        // Baseline alignment (horizontal rows): find the row's baseline as the
        // deepest ascent among the children, so every child's text baseline
        // lands on the same line regardless of its font size.
        float row_baseline = 0.0f;
        const bool do_baseline = (L.align == Align::baseline && ax == Axis::horizontal);
        if (do_baseline) {
            for (const auto& it : items)
                row_baseline = num::max(row_baseline,
                                        detail::ascent_of(kids[it.index], tm, cross_of(it.base, ax)));
        }

        // The cross extent children stretch and align within. One line: the
        // container's. Wrapped: THIS line's -- its tallest item -- as CSS
        // `align-content: flex-start` with `align-items: stretch` does.
        // Stretching every line to the whole container made each card in a
        // wrapped grid as tall as the grid, pushing line two off the end.
        float line_avail_cross = avail_cross;
        if (lines.size() > 1) {
            line_avail_cross = 0.0f;
            for (const auto& it : items)
                line_avail_cross = num::max(line_avail_cross,
                                            cross_of(it.base, ax) + cross_of(it.margin.total(), ax));
        }

        for (std::size_t k = 0; k < items.size(); ++k) {
            auto& it = items[k];
            Node& kid = kids[it.index];
            const LayoutStyle& kl = kid.style().layout;

            cursor += main_of(it.margin_lead(ax), ax);

            // Cross-axis sizing: stretch fills, everything else uses the
            // measured size clamped to the container.
            const float kid_margin_cross = cross_of(it.margin.total(), ax);
            float kid_cross = cross_of(it.base, ax);

            const Length cross_explicit = (ax == Axis::horizontal) ? kl.height : kl.width;
            const bool cross_is_auto = cross_explicit.is_auto();

            if (L.align == Align::stretch && cross_is_auto) {
                kid_cross = num::max(line_avail_cross - kid_margin_cross, 0.0f);
            }
            kid_cross = (ax == Axis::horizontal)
                ? clamp_axis(kid_cross, kl.min_height, kl.max_height, avail_cross)
                : clamp_axis(kid_cross, kl.min_width,  kl.max_width,  avail_cross);

            const float cross_free = line_avail_cross - kid_cross - kid_margin_cross;
            // In baseline mode the child's own ascent decides its offset (so
            // its baseline meets the row's); otherwise the usual align rule.
            const float cross_align = do_baseline
                ? (row_baseline - detail::ascent_of(kid, tm, kid_cross))
                : align_offset(L.align, cross_free);
            const float cross_pos  = cross_of(content.origin, ax)
                                   + cross_of(it.margin_lead(ax), ax)
                                   + cross_align + cross_cursor;

            const Vec2 origin = compose(ax, cursor, cross_pos) + kl.offset;
            const Vec2 sz     = compose(ax, it.main, kid_cross);

            arrange(kid, Rect{origin, sz}, tm);

            line_cross = num::max(line_cross, kid_cross + kid_margin_cross);
            cursor += it.main + main_of(it.margin_trail(ax), ax);
            if (k + 1 < items.size()) cursor += L.gap + dist.between;
        }
        // Stack the next wrapped line below this one (plus the cross gap).
        cross_cursor += line_cross + (line_cross > 0.0f ? L.gap : 0.0f);
        }
    }

    // ---- out-of-flow children ------------------------------------------
    // Absolute children resolve against the padding box; `offset` is their
    // top-left, and auto sizes fall back to their intrinsic measurement.
    for (auto& kid : kids) {
        if (in_flow(kid)) continue;
        const LayoutStyle& kl = kid.style().layout;
        const Vec2 base = measure(kid, Constraints::tight(content.size), tm);
        const float w = kl.width.resolve(content.size.x, base.x);
        const float hgt = kl.height.resolve(content.size.y, base.y);
        arrange(kid, Rect{content.origin + kl.offset, Vec2{w, hgt}}, tm);
    }
}

}  // namespace detail

inline void arrange(Node& node, const Rect& frame, const TextMeasurer& tm) {
    // A reused memo subtree at the same size only needs moving; at a new
    // size it's relaid out from its resolved (pre-layout) copy. Moving adds
    // the offset to every frame, which can round 1 ulp away from a fresh
    // layout: at most 1/255 on an antialiased edge.
    if (node.memo_reused()) {
        const Rect old = node.frame();
        if (old.size.x == frame.size.x && old.size.y == frame.size.y) {
            if (old.origin.x != frame.origin.x || old.origin.y != frame.origin.y)
                node.translate(frame.origin - old.origin);
            return;
        }
        if (const Node* fresh = node.memo_cascaded()) {
            const std::uint64_t key = node.memo_key(), path = node.memo_path();
            node = *fresh;
            node.set_memo_key(key);
            node.set_memo_path(path);
            node.set_memo_reused(false);
        }
    }
    node.set_frame(frame);
    // Corner radii are clamped now that the real size is known, so `pill` /
    // `radius_full` become an actual pill -- for EVERY node, leaves and
    // scroll viewports included (they return early below). Transitions run
    // after layout and must see real radii, not 1e9.
    {
        const Corners& c = std::as_const(node).style().corners;
        const Corners clamped = c.clamp_to(frame.size);
        if (!(clamped == c)) node.style().corners = clamped;   // writes allocate
    }
    if (node.children().empty()) return;

    // ── scroll viewports ────────────────────────────────────────────────
    //
    // Children are laid out at their NATURAL size in an unbounded box, then
    // shifted by the scroll offset. Doing it this way (rather than shrinking
    // the children to fit) is what makes a scroll view show a slice of a
    // larger document instead of a squashed version of it.
    if (const ScrollState* sc = node.style().layout.scroll; sc != nullptr) {
        const Rect content_box = deflate(frame, node.style().layout.padding);

        // Measure the CHILDREN, not the node.
        //
        // `measure(node, ...)` would short-circuit on the viewport's own
        // explicit size — which is exactly the 200px we are scrolling inside —
        // and report that the content fits perfectly. The whole point is that
        // the content is BIGGER than the viewport, so the scrolling axis must
        // be unbounded and the viewport's own size must not participate.
        Constraints cc = Constraints::tight(content_box.size);
        if (sc->axis != ScrollAxis::horizontal) cc.max_height = num::inf;
        if (sc->axis != ScrollAxis::vertical)   cc.max_width  = num::inf;

        const Axis ax = node.style().layout.axis;
        Vec2 natural{};
        int  flow_kids = 0;
        for (const auto& kid : node.children()) {
            if (!detail::in_flow(kid)) continue;
            const Vec2 ks = measure(kid, cc, tm) + kid.style().layout.margin.total();
            if (ax == Axis::vertical) {
                natural.y += ks.y;
                natural.x  = num::max(natural.x, ks.x);
            } else {
                natural.x += ks.x;
                natural.y  = num::max(natural.y, ks.y);
            }
            ++flow_kids;
        }
        if (flow_kids > 1) {
            const float gaps = node.style().layout.gap * static_cast<float>(flow_kids - 1);
            (ax == Axis::vertical ? natural.y : natural.x) += gaps;
        }

        sc->measured(content_box.size, natural);

        // Lay the children out at natural size, then translate.
        const Rect inner{content_box.origin - sc->offset,
                         Vec2{num::max(natural.x, content_box.size.x),
                              num::max(natural.y, content_box.size.y)}};
        detail::arrange_children(node, inner, tm);

        // A viewport always clips: content outside it is not merely hidden,
        // it must not receive clicks either.
        node.style().clip = true;
        return;
    }

    // Children live inside the padding box.
    const Rect content = deflate(frame, node.style().layout.padding);
    detail::arrange_children(node, content, tm);
}

/// Full layout of a root node into a viewport. Returns the root's rect.
///
/// A tree that has not been through the cascade yet (a test, a one-shot
/// render, a tool) is resolved first against the default theme, at rest:
/// theme tokens (`var::space(2)`, `var::radius_md`, `var::accent`) are
/// sentinels until then, and laying out a sentinel padding is garbage. The
/// host cascades with the real theme and interaction state before calling
/// this, so for an app it is a no-op.
inline Rect layout_tree(Node& root, Vec2 viewport, const TextMeasurer& outer) {
    if (!root.cascaded()) cascade_static(root, themes::midnight, viewport);
    // Every node in this tree gets this pass's stamp the first time it's
    // measured; a node coming from an older tree (copied, reused) carries an
    // older stamp, so its memo misses. The stamp is the root's own counter,
    // bumped per pass, so no shared state is involved.
    const detail::PassMeasurer tm{outer, root.next_layout_pass()};
    const LayoutStyle& L = root.style().layout;
    const Vec2 intrinsic = measure(root, Constraints::tight(viewport), tm);
    const float w = L.width.is_auto()  ? viewport.x : L.width.resolve(viewport.x, intrinsic.x);
    const float h = L.height.is_auto() ? viewport.y : L.height.resolve(viewport.y, intrinsic.y);
    const Rect frame{Vec2{}, Vec2{w, h}};
    arrange(root, frame, tm);
    return frame;
}

}  // namespace mayag::layout
