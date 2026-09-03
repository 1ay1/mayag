#pragma once
// mayag::dsl widgets — components built entirely from the public DSL
//
// Nothing here is privileged. Every widget below is an ordinary function that
// composes `v`/`h`/`text`/`box` with modifiers, and returns an Elem whose type
// still carries its capabilities — so the CALLER can keep piping:
//
//     button<"Deploy">(theme) | grow() | id<"deploy-btn">
//
// That is the test of whether a DSL is actually good: user widgets and
// built-in widgets are indistinguishable, because they are the same thing.

#include "dsl.hpp"
#include "../style/theme.hpp"

namespace mayag::dsl {

// ── primitives ──────────────────────────────────────────────────────────

/// Internal helper: a one-child hstack, declared up front so the primitives
/// below can use it before the layout-helper section.
template <Element... Kids>
[[nodiscard]] constexpr auto h_(Kids... kids) { return h(kids...); }

/// A horizontal rule.
[[nodiscard]] constexpr auto divider(const Theme& t, float thickness = 1.0f) {
    return box() | height(thickness) | width(pct(100)) | bg(t.border);
}

/// A coloured dot — status indicators, legend swatches, unread badges.
[[nodiscard]] constexpr auto dot(Color<Srgb> c, float d = 8.0f) {
    return box() | size(d, d) | bg(c) | radius(d * 0.5f);
}

/// A filled progress track.
[[nodiscard]] constexpr auto progress(const Theme& t, float fraction, float w = 200.0f,
                                      float h = 6.0f) {
    return h_(box() | size(w * num::saturate(fraction), h) | bg(t.accent) | radius(h * 0.5f))
         | size(w, h) | bg(t.border) | radius(h * 0.5f) | clip;
}

// ── surfaces ────────────────────────────────────────────────────────────

/// A card: the workhorse container. Note it returns a *container*, so the
/// caller can still add children conceptually via composition and still use
/// `gap`, `center`, etc.
template <Element... Kids>
[[nodiscard]] constexpr auto card(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(2))
         | gap(t.space(1))
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_medium)
         | elevation(6.0f);
}

/// A panel — flatter than a card, for sidebars and toolbars.
template <Element... Kids>
[[nodiscard]] constexpr auto panel(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(1.5f))
         | gap(t.space(1))
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_small);
}

/// Frosted-glass overlay — modals, command palettes, popovers.
template <Element... Kids>
[[nodiscard]] constexpr auto glass(const Theme& t, Kids... kids) {
    return v(kids...)
         | pad(t.space(2))
         | gap(t.space(1))
         | bg(t.overlay.fade(0.72f))
         | border(1, t.border_strong.fade(0.6f))
         | radius(t.radius_large)
         | backdrop_blur(24.0f)
         | elevation(20.0f);
}

// ── typography ──────────────────────────────────────────────────────────

template <fixed_string S>
[[nodiscard]] constexpr auto title(const Theme& t) {
    return text<S> | font(t.font_size * 1.6f) | bold | fg(t.text_primary) | tracking(-0.3f);
}

template <fixed_string S>
[[nodiscard]] constexpr auto heading(const Theme& t) {
    return text<S> | font(t.font_size * 1.15f) | semibold | fg(t.text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto body(const Theme& t) {
    return text<S> | font(t.font_size) | fg(t.text_primary);
}

template <fixed_string S>
[[nodiscard]] constexpr auto caption(const Theme& t) {
    return text<S> | font(t.font_size * 0.85f) | fg(t.text_secondary);
}

// ── controls ────────────────────────────────────────────────────────────

enum class ButtonVariant { primary, secondary, ghost, danger };

/// A button. All four variants share one body; the variant only chooses
/// colours, which is exactly the property that keeps a design system coherent.
template <fixed_string Label>
[[nodiscard]] constexpr auto button(const Theme& t, ButtonVariant variant = ButtonVariant::primary) {
    const bool primary_ = (variant == ButtonVariant::primary);
    const bool danger_  = (variant == ButtonVariant::danger);
    const bool ghost_   = (variant == ButtonVariant::ghost);

    const Color<Srgb> face =
        primary_ ? t.accent : danger_ ? t.danger : ghost_ ? t.surface.fade(0.0f) : t.surface_raised;
    const Color<Srgb> label_color =
        primary_ ? t.on_accent : danger_ ? readable_on(t.danger) : t.text_primary;
    const Color<Srgb> edge = ghost_ ? t.border : (primary_ || danger_) ? face : t.border_strong;

    return h(text<Label> | font(t.font_size) | semibold | fg(label_color))
         | center
         | pad(t.space(1.25f), t.space(2))
         | bg(face)
         | border(1, edge)
         | radius(t.radius_small)
         | when(primary_ || danger_, elevation(3.0f));
}

/// A pill-shaped label. `tone` drives the whole appearance.
template <fixed_string Label>
[[nodiscard]] constexpr auto badge(const Theme& t, Color<Srgb> tone) {
    return h(text<Label> | font(t.font_size * 0.8f) | semibold | fg(tone))
         | center
         | pad(3.0f, 9.0f)
         | bg(tone.fade(0.16f))
         | border(1, tone.fade(0.35f))
         | pill;
}

/// A key cap — for shortcut hints.
template <fixed_string Key>
[[nodiscard]] constexpr auto kbd(const Theme& t) {
    return h(text<Key> | font(t.font_size * 0.82f) | fg(t.text_secondary))
         | center
         | pad(2.0f, 7.0f)
         | bg(t.background)
         | border(1, t.border_strong)
         | radius(4.0f)
         | inner_shadow(2.0f, colors::black.fade(0.25f), {0.0f, -1.0f});
}

/// A toggle switch, drawn declaratively: the knob is an absolutely positioned
/// child whose x offset is the animation parameter.
[[nodiscard]] constexpr auto toggle(const Theme& t, bool on, float w = 40.0f, float h = 22.0f) {
    const float knob = h - 6.0f;
    const float x    = on ? (w - knob - 3.0f) : 3.0f;
    return z(box() | size(knob, knob)
                   | bg(on ? t.on_accent : t.text_secondary)
                   | radius(knob * 0.5f)
                   | absolute(x, 3.0f)
                   | elevation(2.0f))
         | size(w, h)
         | bg(on ? t.accent : t.surface_raised)
         | border(1, on ? t.accent : t.border_strong)
         | radius(h * 0.5f);
}

/// An avatar circle with an accent ring.
[[nodiscard]] constexpr auto avatar(const Theme& t, Color<Srgb> tint, float d = 36.0f) {
    return box() | size(d, d)
                 | bg(tint)
                 | radius(d * 0.5f)
                 | border(2, t.surface, StrokeAlign::outside);
}

// ── layout helpers ──────────────────────────────────────────────────────

/// A horizontal row whose children are pushed to opposite ends — the single
/// most common bar layout (title on the left, actions on the right).
template <Element... Kids>
[[nodiscard]] constexpr auto split(Kids... kids) {
    return h(kids...) | justify(Justify::space_between) | align(Align::center);
}

/// A centred stack that fills whatever space it is given.
template <Element... Kids>
[[nodiscard]] constexpr auto centered(Kids... kids) {
    return v(kids...) | center | width(pct(100)) | height(pct(100));
}

}  // namespace mayag::dsl
