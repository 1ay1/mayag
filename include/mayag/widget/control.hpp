#pragma once
// mayag::widget control — what every interactive widget shares
//
// The rules the controls in this folder follow:
//
//  1. NO HIDDEN STATE. A control draws a value it is GIVEN and names the node
//     the user will interact with. What a click or drag MEANS is decided in
//     the program's update(), so every value lives in the model where it can
//     be saved, undone and asserted on. Each control documents its
//     interaction contract in its own header.
//
//  2. INTERACTION FEEDBACK IS READ, NOT STORED. Hover, press and focus come
//     from the frame context (`c.hovered(id)` ...), which the host derives
//     from the laid-out tree. Controls are templated on that context so the
//     widget layer does not depend on the app layer; mayag::Ctx satisfies it,
//     and an app can pass its own (e.g. one carrying a different theme).
//
// Ids: a control named "font" owns node_id("font"); its parts are
// part_id("font", i), matched in update() with `is(e, "font", i)`.

#include "../dsl/dsl.hpp"
#include "../scene/memo.hpp"
#include "../style/theme.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mayag::dsl {

/// What a control reads from a frame context. mayag::Ctx models it.
template <class C>
concept ControlContext = requires(const C& c, std::uint64_t id) {
    { c.theme } -> std::convertible_to<const Theme&>;
    { c.hovered(id) } -> std::same_as<bool>;
    { c.pressed(id) } -> std::same_as<bool>;
    { c.focused(id) } -> std::same_as<bool>;
};

/// The frame's own clock (Ctx::time, seconds), from any control context (a
/// wrapper exposing the Ctx as `.c` too); 0 where there is none. The ONLY
/// clock a widget animates by: it takes no `time` parameter, so an app can't
/// hand it a model clock that moves only while some subscription runs (hand
/// did, three times: the tab spinner, the sticky header, the dialog froze or
/// lagged whenever nothing else animated). Read it through stepper /
/// ambient_time, which book the next frame (docs/motion.md).
template <class C>
[[nodiscard]] double frame_time(const C& c) noexcept {
    if constexpr (requires { { c.time } -> std::convertible_to<double>; }) return c.time;
    else if constexpr (requires { { c.c.time } -> std::convertible_to<double>; }) return c.c.time;
    else return 0.0;
}

/// Build through the context's memo cache when it has one (a live window's
/// Ctx), so a heavy widget reuses last frame's subtree while its inputs
/// (folded into `key`) are unchanged. Any other context just builds.
template <class C, class F>
[[nodiscard]] Node memo_widget(const C& c, std::uint64_t key, F&& build) {
    if constexpr (requires { { c.memoize(key, build) } -> std::convertible_to<Node>; })
        return c.memoize(key, std::forward<F>(build));
    else
        return Node(build());
}

/// The part ids of a compound control: `part_id("theme", 2)` is the third
/// segment of the "theme" segmented control, the third row of a radio group,
/// the third option of a dropdown.
[[nodiscard]] constexpr std::uint64_t part_id(std::string_view base, std::size_t i) noexcept {
    return node_id(base, i);
}
/// Parts that are not an option index (a stepper's buttons, a dropdown's
/// menu). Far above any option count, so they can never collide.
inline constexpr std::size_t part_dec  = 1u << 20;
inline constexpr std::size_t part_inc  = part_dec + 1;
inline constexpr std::size_t part_menu = part_dec + 2;

namespace detail {

/// The edge a control wears: the focus ring when focused (a settings form is
/// used with both keyboard and mouse, and the ring is the honest answer to
/// "what will Space toggle?"), a stronger border on hover, else `rest`.
[[nodiscard]] inline Color<Srgb> edge(const Theme& t, bool focused, bool hovered,
                                      Color<Srgb> rest) {
    if (focused) return t.focus_ring;
    if (hovered) return t.border_strong;
    return rest;
}

/// Advance of `s` at `size` in the frame's real font, when the context can
/// measure (mayag::Ctx, or a wrapper exposing it as `.c`); a 0.6 em
/// monospace estimate otherwise.
template <class C>
[[nodiscard]] float text_advance(const C& c, std::string_view s, float size, FontFamily family = FontFamily::ui) {
    TextStyle st;
    st.size = size;
    st.family = family;
    if constexpr (requires { c.c.measurer(); }) return c.c.measurer().advance(s, st);
    else if constexpr (requires { c.measurer(); }) return c.measurer().advance(s, st);
    else return static_cast<float>(s.size()) * size * 0.6f;
}

/// `name` as it fits in `room` px, shortened in the MIDDLE: the start (what
/// it is) and the end (its extension, its number) both stay readable --
/// "compile_comm…ands.json", not "compile_comma…". Whole UTF-8 characters
/// only. Returns `name` itself when it fits.
template <class C>
[[nodiscard]] std::string fit_middle(const C& c, std::string_view name, float room, float size,
                                     FontFamily family = FontFamily::ui) {
    if (text_advance(c, name, size, family) <= room) return std::string{name};
    // character boundaries
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < name.size(); ++i)
        if ((static_cast<unsigned char>(name[i]) & 0xC0) != 0x80) at.push_back(i);
    const std::size_t n = at.size();
    if (n <= 2) return std::string{name};
    // the tail keeps the extension (up to 8 chars of it) and a little before
    const std::size_t dot = name.rfind('.');
    const auto make = [&](std::size_t keep) {
        // keep `keep` characters: ~60% head, the rest tail, tail >= the extension when it can
        std::size_t tail = keep * 2 / 5;
        if (dot != std::string_view::npos && dot > 0) {
            std::size_t ext = 0;
            for (std::size_t k = 0; k < n; ++k) if (at[k] >= dot) ++ext;
            if (ext <= 8 && ext + 3 <= keep) tail = std::max(tail, ext + 2);
        }
        tail = std::clamp<std::size_t>(tail, 1, keep);
        const std::size_t head = keep - tail;
        std::string out{name.substr(0, at[head])};
        out += "\u2026";
        out += name.substr(at[n - tail]);
        return out;
    };
    std::size_t lo = 1, hi = n - 1, best = 1;   // the most characters that fit
    while (lo <= hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (text_advance(c, make(mid), size, family) <= room) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return make(best);
}

/// Previews are pictures, not controls: strip node ids so a click lands on
/// the card around them (hit testing picks the innermost NAMED node).
inline void make_inert(Node& n) {
    n.style().id = 0;
    for (auto& k : n.children()) make_inert(k);
}

}  // namespace detail

}  // namespace mayag::dsl
