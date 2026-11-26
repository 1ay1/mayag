#pragma once
// mayag::widget scrollback_bar — the scrollback as a minimap
//
//   scrollback_bar(c, "sb", info, height_px)
//
//      ┃▬▬▬ ▬▬▬▬▬            ←  history, drawn as its own coloured text runs
//      ┃▬▬▬▬▬▬ ▬▬            (one bar per line, or per bucket of lines once
//    ──┼──────────────       the history is taller than the strip)
//      ┃▬▬ ▬▬▬▬▬▬▬▬▬▬▬▬    ▬ ←  search hits on the right edge
//     ┌┃──────────────────┐
//     │┃▬▬▬▬▬ ▬▬▬         │  ←  the viewport: a frame with corner ticks
//     └┃──────────────────┘
//      ┃▬▬▬▬▬▬▬▬▬
//      ^ command spans: green ok, red failed, blue running; a hairline at each prompt
//
// Rows are abstract line indices (0 = oldest kept line), so it works with
// any scrollback store; the app summarises its lines into `runs`. At rest it
// is a slim strip; `expanded` (the app sets it while the pointer is near)
// draws the full minimap. The history always spans the full height.
//
// Interaction: Pressed / Dragged on node_id(id) carry local.y; the first line
// to show is scrollback_line_at(info, local.y, height).

#include "control.hpp"
#include "../render/draw_list.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mayag::dsl {

struct ScrollbackMark {
    enum class Kind : std::uint8_t { prompt, error, match, current_match, bookmark };
    int  line = 0;
    Kind kind = Kind::prompt;
};

/// A coloured run of text on one minimap row (x in 0..1 of the line width).
struct ScrollbackRun {
    int           row = 0;            ///< minimap row (bucket), 0 = top
    float         x0 = 0.0f, x1 = 0.0f;
    std::uint32_t rgb = 0xFFFFFF;
};

/// A command's lines, for the status stripe.
struct ScrollbackSpan {
    enum class Kind : std::uint8_t { ok, error, running };
    int  first = 0, last = 0;         ///< lines, inclusive
    Kind kind = Kind::ok;
};

struct ScrollbackInfo {
    int total = 0;      ///< history + screen rows
    int first = 0;      ///< first visible line
    int visible = 24;   ///< rows on screen
    std::vector<ScrollbackMark> marks;
    // the minimap proper (optional): runs per bucket, command spans
    int                          buckets = 0;   ///< rows the runs were binned into (0: no runs)
    std::vector<ScrollbackRun>   runs;
    /// Changes whenever `runs` does (0 = unknown: the run layer is rebuilt
    /// every frame). With it, the hundreds of run bars are built once per
    /// content change, not per frame -- the bar redraws on hover and scroll.
    std::uint64_t runs_version = 0;
    std::vector<ScrollbackSpan>  spans;
    bool  expanded = false;
    float width = 84.0f;     ///< when expanded
    float rest_width = 14.0f;
    float line_px = 2.0f;    ///< max px per line
};

/// The scale: px per line. The history always spans the whole strip, like a
/// scrollbar -- a short one is spread out, a long one binned.
[[nodiscard]] inline float scrollback_scale(const ScrollbackInfo& s, float height) {
    return s.total > 0 ? height / static_cast<float>(s.total) : height;
}

[[nodiscard]] inline int scrollback_line_at(const ScrollbackInfo& s, float y, float height) {
    if (s.total <= 0 || height <= 0) return 0;
    const float k = scrollback_scale(s, height);
    return std::clamp(static_cast<int>(y / k) - s.visible / 2, 0, std::max(0, s.total - s.visible));
}

/// The same, unrounded: the exact (fractional) first line under `y`. A drag
/// must use this -- a whole line per step jerks wherever a line is more than
/// a pixel or two of the strip, and stalls between steps where it is less.
[[nodiscard]] inline double scrollback_pos_at(const ScrollbackInfo& s, float y, float height) {
    if (s.total <= 0 || height <= 0) return 0.0;
    const double k = static_cast<double>(scrollback_scale(s, height));
    return std::clamp(static_cast<double>(y) / k - static_cast<double>(s.visible) / 2.0, 0.0,
                      static_cast<double>(std::max(0, s.total - s.visible)));
}

/// How many minimap rows to bin the history into for a strip `height` tall.
[[nodiscard]] inline int scrollback_buckets(int total, float height, float line_px = 2.0f) {
    return std::max(1, std::min(total, static_cast<int>(height / line_px)));
}

template <ControlContext C>
[[nodiscard]] inline Node scrollback_bar(const C& c, std::string_view id, const ScrollbackInfo& s, float height_px) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool held = c.pressed(nid);
    const bool open = s.expanded || held;
    const float w = open ? s.width : s.rest_width;
    const float k = scrollback_scale(s, height_px);                 // px per line
    const float used = std::min(height_px, k * static_cast<float>(std::max(s.total, 1)));
    const auto ink = t.text_primary, paper = t.background;
    const auto rgb = [](std::uint32_t v, float a) {
        return rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)).fade(a);
    };
    const float vy = static_cast<float>(s.first) * k;
    const float vh = std::max(static_cast<float>(s.visible) * k, 6.0f);
    const bool scrolled = s.first + s.visible < s.total;

    std::vector<Node> parts;
    // the body: a hairline down the left, the paper behind when open
    parts.push_back((box() | size(w, height_px) | bg(open ? mix(paper, ink, 0.035f).fade(0.97f) : paper.fade(0.0f))).build());
    parts.push_back((box() | size(1.0f, height_px) | absolute(0.0f, 0.0f)
                     | bg(mix(paper, ink, open ? 0.16f : 0.07f))).build());

    // text runs: one bar per minimap row -- hundreds of nodes. Built once per
    // content change (memo), not every frame: scrolling and hovering redraw
    // the frame and marks over the same layer.
    const float x_l = open ? 7.0f : 4.0f, x_r = w - (open ? 6.0f : 3.0f);
    if (s.buckets > 0) {
        const float bh = used / static_cast<float>(s.buckets);
        // a line is a thin bar however spread out the history is: text, not blocks
        const float row_h = std::clamp(bh - (bh >= 2.0f ? 0.6f : 0.0f), 0.7f, 2.5f);
        const int vis0 = static_cast<int>(vy / std::max(bh, 0.001f)), vis1 = static_cast<int>((vy + vh) / std::max(bh, 0.001f));
        // One canvas node that owns its bars, not a box node per bar: while
        // output streams the runs change every frame, and hundreds of nodes
        // (each a style allocation, then cascade, layout and paint) were most
        // of the view's cost. The bars are the same instances either way.
        const auto build_runs = [&] {
            struct Bars {
                float w = 0, h = 0;                         // logical size the bars are laid out in
                std::vector<std::pair<Rect, Color<Srgb>>> bars;
            };
            auto data = std::make_shared<Bars>();
            data->w = w; data->h = height_px;
            data->bars.reserve(s.runs.size());
            for (const auto& r : s.runs) {
                const bool in_view = r.row >= vis0 && r.row <= vis1;
                const float a = open ? (in_view ? 0.95f : 0.55f) : (in_view ? 0.55f : 0.22f);
                const float x0 = x_l + r.x0 * (x_r - x_l), x1 = std::max(x_l + r.x1 * (x_r - x_l), x0 + 1.0f);
                data->bars.emplace_back(Rect{x0, static_cast<float>(r.row) * bh, x1 - x0, row_h}, rgb(r.rgb, a));
            }
            Style st;
            st.layout.width = px(w);
            st.layout.height = px(height_px);
            const CanvasFn paint = [](DrawList& dl, const Rect& f, const void* u) {
                const auto& d = *static_cast<const Bars*>(u);
                const float sx = d.w > 0 ? f.width() / d.w : 1.0f, sy = d.h > 0 ? f.height() / d.h : 1.0f;
                for (const auto& [r, col] : d.bars)
                    dl.fill_rect(Rect{f.left() + r.left() * sx, f.top() + r.top() * sy, r.width() * sx, r.height() * sy}, col);
            };
            return (z(Node::make_canvas(paint, std::shared_ptr<const void>(std::move(data)), std::move(st)))
                    | size(w, height_px) | absolute(0.0f, 0.0f)).build();
        };
        if constexpr (requires { c.memoize(std::uint64_t{}, std::uint64_t{}, build_runs); }) {
            if (s.runs_version != 0) {
                std::uint64_t key = memo_mix(s.runs_version, static_cast<std::uint64_t>(vis0) << 32 | static_cast<std::uint32_t>(vis1));
                std::uint32_t u;
                for (const float f : {w, height_px, bh, row_h}) { std::memcpy(&u, &f, 4); key = memo_mix(key, u); }
                key = memo_mix(key, (open ? 1u : 0u) | static_cast<std::uint64_t>(s.runs.size()) << 1);
                // the site: while output streams the key changes every frame,
                // and the cache stops paying to keep what it can't reuse
                parts.push_back(c.memoize(memo_mix(node_id(id), 0x5C0B), memo_mix(key, node_id(id)), build_runs));
            } else {
                parts.push_back(build_runs());
            }
        } else {
            parts.push_back(build_runs());
        }
    }
    // command spans: a status stripe down the left edge
    for (const auto& sp : s.spans) {
        using K = ScrollbackSpan::Kind;
        const auto col = sp.kind == K::error ? t.danger : sp.kind == K::running ? t.accent : t.success;
        const float y0 = static_cast<float>(sp.first) * k, y1 = static_cast<float>(sp.last + 1) * k;
        parts.push_back((box() | size(2.0f, std::max(y1 - y0, 1.0f)) | absolute(1.0f, y0)
                         | bg(col.fade(sp.kind == K::ok ? (open ? 0.55f : 0.35f) : 0.9f))).build());
    }
    // marks: prompts as hairlines across, errors as a red notch, search hits on the right edge
    for (const auto& m : s.marks) {
        using K = ScrollbackMark::Kind;
        const float y = static_cast<float>(m.line) * k;
        switch (m.kind) {
            case K::prompt:
                if (open) parts.push_back((box() | size(w - 4.0f, 1.0f) | absolute(3.0f, y) | bg(mix(paper, ink, 0.22f))).build());
                break;
            case K::error:
                parts.push_back((box() | size(open ? 6.0f : 4.0f, 2.0f) | absolute(1.0f, y) | bg(t.danger)).build());
                break;
            case K::match: case K::current_match: {
                const bool cur = m.kind == K::current_match;
                const float mw = cur ? (open ? 10.0f : 6.0f) : (open ? 6.0f : 4.0f);
                parts.push_back((box() | size(mw, 2.0f) | absolute(w - mw, y)
                                 | bg(cur ? t.warning : t.warning.fade(0.6f))).build());
                break;
            }
            case K::bookmark:
                parts.push_back((box() | size(w - 2.0f, 1.0f) | absolute(1.0f, y) | bg(t.accent)).build());
                break;
        }
    }
    // the viewport: a frame with corner ticks (always when open; else only when scrolled back)
    if (open || scrolled) {
        const float fy = std::min(vy, std::max(height_px - vh, 0.0f));
        const auto edge = mix(paper, ink, held ? 0.75f : open ? 0.5f : 0.35f);
        parts.push_back((box() | size(w - 2.0f, vh) | absolute(1.0f, fy)
                         | bg(ink.fade(open ? 0.07f : 0.05f)) | border(1.0f, edge.fade(open ? 1.0f : 0.8f))).build());
        if (open) {   // corner ticks: a frame you can read at a glance
            const float tk = 5.0f;
            for (float cx : {1.0f, w - 1.0f - tk})
                for (float cy : {fy - 1.0f, fy + vh - 1.0f})
                    parts.push_back((box() | size(tk, 2.0f) | absolute(cx, cy) | bg(ink.fade(0.85f))).build());
        }
    }
    return (z(std::move(parts)) | size(w, height_px) | clip | pointer_cursor
            | role(a11y::Role::slider) | label("scrollback") | id_of(id)).build();
}

/// The bar docked on the right edge of a `width`-wide view (a terminal's minimap).
template <ControlContext C>
[[nodiscard]] inline Node scrollback_dock(const C& c, std::string_view id, const ScrollbackInfo& s, Vec2 view) {
    return (node(scrollback_bar(c, id, s, view.y)) | absolute(view.x - s.width, 0.0f)).build();
}

}  // namespace mayag::dsl
