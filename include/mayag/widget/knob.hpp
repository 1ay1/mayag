#pragma once
// mayag::widget knob — precision controls that look like hardware
//
//   range_knob(c, "font.size", fraction, 220, {.active = selected})
//       | on_slide([](Vec2 p) { return Set{range_knob_fraction(p.x, 220)}; })
//
//        ╭──╮
//   ━━━━━┥●┝───────────────────────────
//   ╵    ╵ ╰──╯    ╵         ╵         ╵
//
//   switch_knob(c, "bell.visual", on, {.active = selected})   ( ●) / (● )
//
// range_knob: a recessed groove, an accent fill that brightens toward the
// thumb, tick marks, and a thumb with an accent ring that grows and glows
// when the row is active, hovered or held. The groove is INSET by half a
// thumb, so 0% and 100% sit fully inside the control -- map the pointer
// with range_knob_fraction(), never local.x / width. The hit target is the
// full 28 px row.
//
// switch_knob: a 40×22 pill; off is a recessed groove with a muted knob,
// on is an accent track with a bright knob. Draw-only; attach on_click.
//
// ui_glyph(t, nerd, plain): a Nerd Font icon when the theme says the font
// has them (Theme::icons), else a plain character every mono font carries.
// Never emit an icon codepoint a face may not have: tofu reads as broken.

#include "control.hpp"
#include "../app/handlers.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

[[nodiscard]] inline std::string_view ui_glyph(const Theme& t, std::string_view nerd, std::string_view plain) {
    return t.icons == IconSet::nerd ? nerd : plain;
}

struct RangeKnobProps {
    bool active = false;   ///< selected by the keyboard: lifted, glowing
    int  ticks = 0;        ///< tick marks under the groove (off by default: they pull the eye below centre)
};

namespace detail {
inline constexpr float kKnobRow = 28.0f, kKnobGroove = 4.0f, kKnobThumbMax = 18.0f;
inline constexpr float kKnobInset = kKnobThumbMax * 0.5f + 2.0f;
}  // namespace detail

/// Pointer x (local to the knob) → 0..1, accounting for the inset groove.
[[nodiscard]] constexpr float range_knob_fraction(float local_x, float track_w) noexcept {
    const float span = track_w - 2.0f * detail::kKnobInset;
    return span > 0.0f ? std::clamp((local_x - detail::kKnobInset) / span, 0.0f, 1.0f) : 0.0f;
}

template <ControlContext C>
[[nodiscard]] inline Node range_knob(const C& c, std::string_view id, float fraction, float track_w,
                                     const RangeKnobProps& p = {}) {
    using namespace detail;
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool grab = c.pressed(nid);
    const bool hot = p.active || grab || c.hovered(nid);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    const float span = std::max(track_w - 2.0f * kKnobInset, 1.0f);
    const float x = kKnobInset + f * span;
    const float gy = (kKnobRow - kKnobGroove) * 0.5f;
    const float thumb = grab ? kKnobThumbMax : hot ? 16.0f : 14.0f;

    std::vector<Node> layers;
    // groove: recessed into the surface
    layers.push_back((box() | size(span, kKnobGroove) | absolute(kKnobInset, gy) | radius(kKnobGroove * 0.5f)
                      | bg(mix(t.background, t.text_primary, 0.10f))
                      | inner_shadow(2.0f, colors::black.fade(0.45f), {0.0f, 1.0f})).build());
    // fill: from a quiet accent to the full accent at the thumb
    if (f > 0.0f)
        layers.push_back((box() | size(f * span, kKnobGroove) | absolute(kKnobInset, gy) | radius(kKnobGroove * 0.5f)
                          | linear_gradient(t.accent.fade(0.45f), t.accent, {0.0f, 0.5f}, {1.0f, 0.5f})).build());
    // ticks: hairlines under the groove, lit where the fill has reached
    for (int i = 0; p.ticks > 1 && i < p.ticks; ++i) {
        const float tf = static_cast<float>(i) / static_cast<float>(p.ticks - 1);
        layers.push_back((box() | size(1.0f, 4.0f) | absolute(kKnobInset + tf * span - 0.5f, gy + kKnobGroove + 4.0f)
                          | bg(tf <= f + 1e-4f ? t.accent.fade(0.7f) : t.text_disabled.fade(0.5f))).build());
    }
    // halo
    if (hot) {
        const float halo = thumb + 14.0f;
        layers.push_back((box() | size(halo, halo) | radius(halo * 0.5f)
                          | absolute(x - halo * 0.5f, (kKnobRow - halo) * 0.5f)
                          | bg(t.accent.fade(grab ? 0.24f : 0.12f))).build());
    }
    // thumb: a bright cap with an accent ring
    layers.push_back((box() | size(thumb, thumb) | radius(thumb * 0.5f)
                      | absolute(x - thumb * 0.5f, (kKnobRow - thumb) * 0.5f)
                      | linear_gradient(mix(t.text_primary, colors::white, 0.6f), mix(t.text_primary, t.surface, 0.1f))
                      | border(hot ? 3.0f : 2.0f, t.accent)
                      | shadow(hot ? 10.0f : 5.0f, hot ? t.accent.fade(0.5f) : colors::black.fade(0.5f), {0.0f, 1.0f})).build());

    return (z(box() | size(track_w, kKnobRow), std::move(layers)) | size(track_w, kKnobRow) | pointer_cursor
            | role(a11y::Role::slider) | a11y_value(f) | label(id) | id_of(id)).build();
}

struct SwitchKnobProps {
    bool active = false;
};

template <ControlContext C>
[[nodiscard]] inline Node switch_knob(const C& c, std::string_view id, bool on, const SwitchKnobProps& p = {}) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hot = p.active || c.hovered(nid);
    constexpr float w = 40.0f, hgt = 22.0f, knob = 16.0f, inset = 3.0f;
    const float kx = on ? w - knob - inset : inset;
    auto track = box() | size(w, hgt) | radius(hgt * 0.5f)
               | border(t.stroke(1), on ? t.accent : (hot ? t.border_strong : t.border));
    const auto off = mix(t.background, t.text_primary, 0.06f);
    auto lit = std::move(track) | linear_gradient(on ? mix(t.accent, colors::white, 0.12f) : off, on ? t.accent : off);
    return (z(std::move(lit) | inner_shadow(on ? 0.0f : 3.0f, colors::black.fade(on ? 0.0f : 0.4f), {0.0f, 1.0f}),
              box() | size(knob, knob) | radius(knob * 0.5f) | absolute(kx, inset)
                  | bg(on ? colors::white : mix(t.text_secondary, t.surface, 0.15f))
                  | shadow(on ? 6.0f : 3.0f, colors::black.fade(0.45f), {0.0f, 1.0f}))
            | size(w, hgt)
            | shadow(hot && on ? 12.0f : 0.0f, t.accent.fade(hot && on ? 0.45f : 0.0f), {0.0f, 0.0f})
            | radius(hgt * 0.5f) | pointer_cursor
            | role(a11y::Role::checkbox) | checked(on) | label(id) | id_of(id)).build();
}

}  // namespace mayag::dsl

// ── terminal-style controls ───────────────────────────────────────────────────────
//
//   bar_knob    ▌▌▌▌▌▌▌█▌▌▌▌▌▌▌   a level meter you can drag: lit cells ramp up to a
//                                bright head cell that glows when held
//   tui_check   [■] on  /  [ ] off
//   tui_choice   block │ beam │ underline    the chosen one in inverse video

namespace mayag::dsl {

struct BarKnobProps {
    bool active = false;
    int  cells = 26;
};
inline constexpr float kBarSegW = 5.0f, kBarSegGap = 2.0f, kBarRowH = 24.0f;
[[nodiscard]] constexpr float bar_knob_width(int cells) noexcept {
    return static_cast<float>(cells) * (kBarSegW + kBarSegGap) - kBarSegGap;
}
/// Pointer x (local) → 0..1.
[[nodiscard]] constexpr float bar_knob_fraction(float local_x, int cells) noexcept {
    return std::clamp(local_x / bar_knob_width(cells), 0.0f, 1.0f);
}

template <ControlContext C>
[[nodiscard]] inline Node bar_knob(const C& c, std::string_view id, float fraction, const BarKnobProps& p = {}) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool grab = c.pressed(nid);
    const bool hot = p.active || grab || c.hovered(nid);
    const int n = std::max(p.cells, 2);
    const int lit = static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * static_cast<float>(n)));
    const auto off = mix(t.background, t.text_primary, 0.13f);
    std::vector<Node> segs;
    segs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const bool on = i < lit, head = on && i == lit - 1;
        const float k = static_cast<float>(i) / static_cast<float>(n - 1);
        const auto col = head ? (hot ? mix(t.accent, colors::white, 0.45f) : mix(t.accent, colors::white, 0.2f))
                       : on ? mix(mix(t.accent, t.background, 0.55f), t.accent, k) : off;
        segs.push_back((box() | size(kBarSegW, head ? 18.0f : 10.0f) | radius(1.0f) | bg(col)
                        | shadow(head && hot ? 10.0f : 0.0f, t.accent.fade(head && hot ? 0.7f : 0.0f), {0.0f, 0.0f})).build());
    }
    return (h(std::move(segs)) | gap(kBarSegGap) | align(Align::center) | height(kBarRowH)
            | width(bar_knob_width(n)) | pointer_cursor
            | role(a11y::Role::slider) | a11y_value(fraction) | label(id) | id_of(id)).build();
}

template <ControlContext C>
[[nodiscard]] inline Node tui_check(const C& c, std::string_view id, bool on, bool active = false) {
    const Theme& t = c.theme;
    const bool hot = active || c.hovered(node_id(id));
    auto mark = h(box() | size(8.0f, 8.0f) | radius(1.0f) | bg(on ? t.success : t.success.fade(0.0f)))
              | size(16.0f, 16.0f) | center | radius(3.0f)
              | border(t.stroke(1), on ? t.success : hot ? t.text_secondary : t.border_strong)
              | shadow(on && hot ? 8.0f : 0.0f, t.success.fade(on && hot ? 0.5f : 0.0f), {0.0f, 0.0f});
    return (h(std::move(mark),
              text_owned(on ? "on" : "off") | font(t.font_size * 0.92f) | monospace
                  | fg(on ? t.success : t.text_disabled) | width(30.0f))
            | gap(8.0f) | align(Align::center) | height(kBarRowH) | pointer_cursor
            | role(a11y::Role::checkbox) | checked(on) | label(id) | id_of(id)).build();
}

template <ControlContext C, class Pick>
[[nodiscard]] inline Node tui_choice(const C& c, std::string_view id, const std::vector<std::string>& opts, int chosen,
                                     Pick pick) {
    const Theme& t = c.theme;
    std::vector<Node> segs;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        const bool on = static_cast<int>(i) == chosen;
        const auto pid = part_id(id, i);
        const bool hov = c.hovered(pid);
        segs.push_back((h(text_owned(opts[i]) | font(t.font_size * 0.9f) | monospace
                              | fg(on ? t.background : hov ? t.text_primary : t.text_secondary)
                              | (on ? semibold : weight(FontWeight::regular)))
                        | height(22.0f) | pad(0, 9.0f) | center | radius(2.0f)
                        | bg(on ? t.accent : hov ? t.text_primary.fade(0.07f) : t.accent.fade(0.0f))
                        | pointer_cursor | role(a11y::Role::radio) | checked(on) | label(opts[i]) | id_of(id, i)
                        | on_click([m = pick(i)] { return m; })).build());
    }
    return (h(std::move(segs)) | gap(2.0f) | pad(2.0f) | radius(3.0f) | border(t.stroke(1), t.border)
            | role(a11y::Role::group) | label(id)).build();
}

}  // namespace mayag::dsl
