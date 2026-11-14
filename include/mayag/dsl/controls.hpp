#pragma once
// mayag::dsl controls — the form vocabulary a settings screen is made of
//
//   switch_control  checkbox  radio_group  segmented  slider  stepper
//   dropdown        swatch    keybind      setting_row section nav_item
//
// Same rule as widgets.hpp: nothing here is privileged, every control is the
// public DSL composed. Two further rules make them usable in a real app:
//
//  1. NO HIDDEN STATE. A control draws a value it is GIVEN and names the node
//     the user will interact with. What a click or drag MEANS is decided in
//     the program's update(), so every value lives in the model where it can
//     be saved, undone and asserted on. The interaction contract of each
//     control is written next to it.
//
//  2. INTERACTION FEEDBACK IS READ, NOT STORED. Hover, press and focus come
//     from the context (`c.hovered(id)` ...), which the host derives from the
//     laid-out tree. The controls are templated on that context so this
//     header does not depend on the app layer; mayag::Ctx satisfies it.
//
// Ids: a control named "font" owns node_id("font"); its parts are
// node_id("font", i) — `is(e, "font", i)` style matching in update(), with
// no strings assembled at runtime.

#include "dsl.hpp"
#include "widgets.hpp"
#include "../app/event.hpp"
#include "../scene/overlay.hpp"
#include "../style/theme.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

/// What the controls read from a frame context. mayag::Ctx models it.
template <class C>
concept ControlContext = requires(const C& c, std::uint64_t id) {
    { c.theme } -> std::convertible_to<const Theme&>;
    { c.hovered(id) } -> std::same_as<bool>;
    { c.pressed(id) } -> std::same_as<bool>;
    { c.focused(id) } -> std::same_as<bool>;
};

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

/// The ring a focused control wears. Keyboard users need to see where they
/// are; mouse users never see it because clicking does not move focus rings
/// onto things they did not tab to... except it does here, deliberately:
/// a settings form is used with both, and a consistent ring is the honest
/// answer to "what will Space toggle?".
[[nodiscard]] inline Color<Srgb> edge(const Theme& t, bool focused, bool hovered,
                                      Color<Srgb> rest) {
    if (focused) return t.focus_ring;
    if (hovered) return t.border_strong;
    return rest;
}

}  // namespace detail

// ── switch ──────────────────────────────────────────────────────────────
//
// Interaction: Clicked / Activated on node_id(id) -> flip the bool.

template <ControlContext C>
[[nodiscard]] inline auto switch_control(const C& c, std::string_view id, bool on) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    constexpr float w = 38.0f, hgt = 22.0f, knob = 16.0f;
    const bool hov = c.hovered(nid);
    return z(box() | size(knob, knob)
                   | bg(on ? t.on_accent : (hov ? t.text_primary : t.text_secondary))
                   | radius(knob * 0.5f)
                   | absolute(on ? w - knob - 3.0f : 3.0f, 3.0f)
                   | elevation(1.5f))
         | size(w, hgt)
         | bg(on ? (c.pressed(nid) ? t.accent_active : hov ? t.accent_hover : t.accent)
                 : t.surface_raised)
         | border(1.5f, detail::edge(t, c.focused(nid), hov, on ? t.accent : t.border_strong))
         | radius(hgt * 0.5f)
         | pointer_cursor
         | role(a11y::Role::checkbox) | checked(on) | label(id)
         | id_of(id);
}

// ── checkbox ────────────────────────────────────────────────────────────
//
// Interaction: Clicked / Activated on node_id(id) -> flip the bool. The
// label is part of the hit target, as it is on every native platform.

template <ControlContext C>
[[nodiscard]] inline auto checkbox(const C& c, std::string_view id, bool on, std::string_view caption_text) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid);
    return h(z(text<"\u2713"> | font(12) | bold | fg(t.on_accent.fade(on ? 1.0f : 0.0f)))
                 | size(18, 18) | center
                 | bg(on ? t.accent : t.surface)
                 | border(1.5f, detail::edge(t, c.focused(nid), hov, on ? t.accent : t.border_strong))
                 | radius(5),
             text_owned(std::string{caption_text}) | font(t.font_size) | fg(t.text_primary))
         | gap(10) | align(Align::center)
         | pointer_cursor
         | role(a11y::Role::checkbox) | checked(on) | label(caption_text)
         | id_of(id);
}

// ── radio group ─────────────────────────────────────────────────────────
//
// Interaction: Clicked / Activated on part_id(id, i) -> select option i.

template <ControlContext C>
[[nodiscard]] inline auto radio_group(const C& c, std::string_view id,
                                      const std::vector<std::string_view>& options, int chosen) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    rows.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto pid = part_id(id, i);
        const bool on  = static_cast<int>(i) == chosen;
        const bool hov = c.hovered(pid);
        rows.push_back((h(z(box() | size(8, 8) | bg(t.on_accent.fade(on ? 1.0f : 0.0f)) | radius(4))
                              | size(18, 18) | center
                              | bg(on ? t.accent : t.surface)
                              | border(1.5f, detail::edge(t, c.focused(pid), hov,
                                                          on ? t.accent : t.border_strong))
                              | radius(9),
                            text_owned(std::string{options[i]}) | font(t.font_size)
                              | fg(t.text_primary))
                        | gap(10) | align(Align::center)
                        | pointer_cursor
                        | role(a11y::Role::radio) | checked(on) | label(options[i])
                        | id_of(id, i)).build());
    }
    return v(v_list(std::move(rows), 10.0f)) | role(a11y::Role::group) | label(id);
}

// ── segmented control ───────────────────────────────────────────────────
//
// The compact one-of-N for 2-5 short options (cursor shape, bell mode).
// Interaction: Clicked / Activated on part_id(id, i) -> select option i.

template <ControlContext C>
[[nodiscard]] inline auto segmented(const C& c, std::string_view id,
                                    const std::vector<std::string_view>& options, int chosen) {
    const Theme& t = c.theme;
    std::vector<Node> segs;
    segs.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto pid = part_id(id, i);
        const bool on  = static_cast<int>(i) == chosen;
        const bool hov = c.hovered(pid);
        segs.push_back((h(text_owned(std::string{options[i]})
                            | font(t.font_size * 0.92f)
                            | (on ? semibold : weight(FontWeight::regular))
                            | fg(on ? t.text_primary : hov ? t.text_primary : t.text_secondary))
                        | pad(5, 12) | center
                        | bg(on ? t.surface_raised : (hov ? t.surface_raised.fade(0.45f)
                                                          : t.surface_raised.fade(0.0f)))
                        | border(1, c.focused(pid) ? t.focus_ring
                                                   : (on ? t.border : t.border.fade(0.0f)))
                        | radius(t.radius_small - 2.0f)
                        | elevation(on ? 1.0f : 0.0f)
                        | pointer_cursor
                        | role(a11y::Role::radio) | checked(on) | label(options[i])
                        | id_of(id, i)).build());
    }
    return h(h_list(std::move(segs), 2.0f))
         | pad(3)
         | bg(t.surface)
         | border(1, t.border)
         | radius(t.radius_small)
         | role(a11y::Role::group) | label(id);
}

// ── slider ──────────────────────────────────────────────────────────────
//
// `fraction` is 0..1; the program owns the real range and units.
// Interaction: Pressed / Dragged on node_id(id) carry `local.x`; the value
// is `clamp(local.x / width, 0, 1)` (pointer capture keeps a drag reporting
// after it leaves the track). Focused + Left/Right should nudge it.
// The hit target is the full 24 px row, not the 4 px track: a slider you
// have to aim at is a slider nobody can use.

template <ControlContext C>
[[nodiscard]] inline auto slider(const C& c, std::string_view id, float fraction, float track_w) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    const bool hov = c.hovered(nid), held = c.pressed(nid), foc = c.focused(nid);
    const float knob = held ? 18.0f : 16.0f;
    const float x = f * track_w;
    return z(box() | size(track_w, 4) | bg(t.surface_raised) | radius(2) | absolute(0, 10),
             box() | size(std::max(x, 0.0f), 4) | bg(t.accent) | radius(2) | absolute(0, 10),
             box() | size(knob, knob)
                   | bg(t.text_primary)
                   | border(foc ? 3.0f : (hov || held ? 2.0f : 0.0f), foc ? t.focus_ring : t.accent)
                   | radius(knob * 0.5f)
                   | elevation(held ? 3.0f : 1.5f)
                   | absolute(x - knob * 0.5f, 12.0f - knob * 0.5f))
         | size(track_w, 24)
         | pointer_cursor
         | role(a11y::Role::slider) | a11y_value(f) | label(id)
         | id_of(id);
}

// ── stepper ─────────────────────────────────────────────────────────────
//
// A number you change one unit at a time (font size, scrollback).
// Interaction: Clicked on part_id(id, part_dec) / part_id(id, part_inc).
// `at_min` / `at_max` dim the button that would do nothing.

template <ControlContext C>
[[nodiscard]] inline auto stepper(const C& c, std::string_view id, std::string value,
                                  bool at_min = false, bool at_max = false) {
    const Theme& t = c.theme;
    const auto button_part = [&](std::size_t part, const char* glyph, bool off) {
        const auto pid = part_id(id, part);
        const bool hov = !off && c.hovered(pid);
        return (h(text_owned(glyph) | font(t.font_size * 1.1f)
                    | fg(off ? t.text_disabled : t.text_primary))
                | size(28, 28) | center
                | bg(c.pressed(pid) && !off ? t.surface_raised
                                            : (hov ? t.surface_raised.fade(0.6f) : t.surface.fade(0.0f)))
                | border(1, c.focused(pid) ? t.focus_ring : t.border.fade(0.0f))
                | radius(t.radius_small - 2.0f)
                | cursor(off ? CursorShape::arrow : CursorShape::pointer)
                | role(a11y::Role::button) | disabled(off)
                | label(part == part_dec ? "decrease" : "increase")
                | id_of(id, part)).build();
    };
    return h(node(button_part(part_dec, "\u2212", at_min)),
             text_owned(std::move(value)) | font(t.font_size) | semibold | fg(t.text_primary)
                 | text_align(TextAlign::center) | width(46),
             node(button_part(part_inc, "+", at_max)))
         | align(Align::center) | pad(2)
         | bg(t.surface) | border(1, t.border) | radius(t.radius_small);
}

// ── dropdown ────────────────────────────────────────────────────────────
//
// The trigger draws in place; when `open`, the option list is posted as an
// OVERLAY, so it escapes a scrolling form's clip and paints above it.
// Interaction:
//   Clicked on node_id(id)            -> toggle open
//   Clicked on part_id(id, i)         -> select i, close
//   Left    on part_id(id, part_menu) -> the click-outside dismissal: close
// The Ctx must be able to post overlays (mayag::Ctx can).

template <ControlContext C>
[[nodiscard]] inline auto dropdown(const C& c, std::string_view id,
                                   const std::vector<std::string>& options, int chosen,
                                   bool open, float width_px = 200.0f) {
    const Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid);
    const std::string current = chosen >= 0 && chosen < static_cast<int>(options.size())
                                    ? options[static_cast<std::size_t>(chosen)]
                                    : std::string{"\u2014"};
    if (open) {
        std::vector<Node> rows;
        rows.reserve(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            const auto pid = part_id(id, i);
            const bool on = static_cast<int>(i) == chosen;
            rows.push_back((h(text_owned(on ? "\u2713" : " ") | font(t.font_size * 0.9f)
                                  | fg(t.accent) | width(16),
                              text_owned(options[i]) | font(t.font_size)
                                  | fg(t.text_primary) | grow())
                            | align(Align::center) | pad(7, 10) | width(pct(100))
                            | bg(c.hovered(pid) ? t.accent.fade(0.18f) : t.overlay.fade(0.0f))
                            | radius(t.radius_small - 2.0f)
                            | pointer_cursor
                            | role(a11y::Role::menu_item) | selected(on)
                            | id_of(id, i)).build());
        }
        c.overlay(Overlay{
            .content = (v(v_list(std::move(rows), 1.0f))
                        | pad(4) | width(width_px)
                        | bg(t.overlay) | border(1, t.border_strong)
                        | radius(t.radius_medium) | elevation(12)
                        | role(a11y::Role::menu)).build(),
            .id = part_id(id, part_menu),
            .anchor_id = nid,
            .placement = Placement::below_start,
            .dismiss = Dismiss::on_click_outside,
        });
    }
    return h(text_owned(current) | font(t.font_size) | fg(t.text_primary) | grow(),
             text<"\u25BE"> | font(t.font_size * 0.85f) | fg(t.text_secondary))
         | align(Align::center) | gap(8) | pad(7, 11) | width(width_px)
         | bg(hov || open ? t.surface_raised : t.surface)
         | border(1, detail::edge(t, c.focused(nid) || open, hov, t.border))
         | radius(t.radius_small)
         | pointer_cursor
         | role(a11y::Role::button) | label(id)
         | id_of(id);
}

// ── colour swatch ───────────────────────────────────────────────────────
//
// Interaction: Clicked / Activated on node_id(base, index) -> select it.
// The selected ring is drawn OUTSIDE the swatch so the colour itself is
// never covered by the indicator.

template <ControlContext C>
[[nodiscard]] inline auto swatch(const C& c, std::string_view base, std::size_t index,
                                 Color<Srgb> colour, bool selected_now, float d = 28.0f) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    return box() | size(d, d)
                 | bg(colour)
                 | border(selected_now ? 2.5f : (hov ? 2.0f : 1.0f),
                          selected_now ? t.text_primary
                                       : (c.focused(pid) ? t.focus_ring
                                                         : (hov ? t.border_strong : t.border)),
                          selected_now ? StrokeAlign::outside : StrokeAlign::inside)
                 | radius(t.radius_small)
                 | pointer_cursor
                 | role(a11y::Role::radio) | checked(selected_now)
                 | id_of(base, index);
}

/// `#RRGGBB` for a colour, the spelling every terminal config file uses.
[[nodiscard]] inline std::string hex(Color<Srgb> c) {
    const auto b = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", b(c.c0), b(c.c1), b(c.c2));
    return buf;
}

// ── keyboard shortcuts ──────────────────────────────────────────────────

/// A recorded shortcut: a key and the modifiers held with it.
struct Chord {
    Key  key = Key::unknown;
    Mods mods{};
    friend constexpr bool operator==(const Chord&, const Chord&) = default;
    [[nodiscard]] constexpr bool empty() const noexcept { return key == Key::unknown; }
};

/// How a key is printed on a keycap.
[[nodiscard]] inline std::string key_label(Key k) {
    switch (k) {
        case Key::enter: return "\u21A9";        case Key::tab: return "\u21E5";
        case Key::backspace: return "\u232B";    case Key::del: return "\u2326";
        case Key::escape: return "Esc";          case Key::space: return "Space";
        case Key::left: return "\u2190";         case Key::right: return "\u2192";
        case Key::up: return "\u2191";           case Key::down: return "\u2193";
        case Key::home: return "Home";           case Key::end: return "End";
        case Key::page_up: return "PgUp";        case Key::page_down: return "PgDn";
        case Key::insert: return "Ins";
        case Key::minus: return "-";             case Key::equal: return "=";
        case Key::bracket_left: return "[";      case Key::bracket_right: return "]";
        case Key::backslash: return "\\";        case Key::semicolon: return ";";
        case Key::quote: return "'";             case Key::comma: return ",";
        case Key::period: return ".";            case Key::slash: return "/";
        case Key::grave: return "`";
        default: break;
    }
    const auto n = static_cast<int>(k);
    if (k >= Key::a && k <= Key::z) return std::string(1, static_cast<char>('A' + (n - static_cast<int>(Key::a))));
    if (k >= Key::n0 && k <= Key::n9) return std::string(1, static_cast<char>('0' + (n - static_cast<int>(Key::n0))));
    if (k >= Key::f1 && k <= Key::f12) return "F" + std::to_string(n - static_cast<int>(Key::f1) + 1);
    return "?";
}

/// True for keys that only make sense held with another (a shortcut cannot
/// BE Shift). The recorder keeps listening past them.
[[nodiscard]] constexpr bool is_modifier_key(Key k) noexcept {
    return k == Key::shift || k == Key::ctrl || k == Key::alt || k == Key::super ||
           k == Key::caps_lock;
}

/// The keycaps of a chord, in the platform's conventional modifier order.
[[nodiscard]] inline std::vector<std::string> chord_caps(Chord ch) {
    std::vector<std::string> caps_;
#if defined(__APPLE__)
    if (ch.mods.ctrl)  caps_.push_back("\u2303");
    if (ch.mods.alt)   caps_.push_back("\u2325");
    if (ch.mods.shift) caps_.push_back("\u21E7");
    if (ch.mods.super) caps_.push_back("\u2318");
#else
    if (ch.mods.ctrl)  caps_.push_back("Ctrl");
    if (ch.mods.alt)   caps_.push_back("Alt");
    if (ch.mods.shift) caps_.push_back("Shift");
    if (ch.mods.super) caps_.push_back("Super");
#endif
    if (!ch.empty()) caps_.push_back(key_label(ch.key));
    return caps_;
}

/// A shortcut field: keycaps at rest, a live prompt while recording.
/// Interaction: Clicked on node_id(base, index) -> start recording that row;
/// while recording, the next non-modifier KeyEvent is the chord, Escape
/// cancels, Backspace clears. `conflict` paints the field as an error.
template <ControlContext C>
[[nodiscard]] inline auto keybind(const C& c, std::string_view base, std::size_t index, Chord ch,
                                  bool recording, bool conflict = false) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    std::vector<Node> caps_;
    if (recording) {
        caps_.push_back((text<"Press a shortcut\u2026"> | font(t.font_size * 0.9f)
                         | fg(t.accent)).build());
    } else if (ch.empty()) {
        caps_.push_back((text<"Unassigned"> | font(t.font_size * 0.9f) | italic
                         | fg(t.text_disabled)).build());
    } else {
        for (auto& cap : chord_caps(ch)) {
            caps_.push_back((h(text_owned(std::move(cap)) | font(t.font_size * 0.85f)
                                 | semibold | fg(t.text_primary))
                             | pad(2, 7)
                             | bg(t.surface_raised) | border(1, t.border_strong)
                             | radius(4)).build());
        }
    }
    return h(h_list(std::move(caps_), 4.0f))
         | align(Align::center) | justify(Justify::end)
         | pad(5, 8) | min_size(150, 32)
         | bg(recording ? t.accent.fade(0.12f) : (hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f)))
         | border(1, recording ? t.accent
                               : conflict ? t.danger
                               : detail::edge(t, c.focused(pid), hov, t.border.fade(0.0f)))
         | radius(t.radius_small)
         | pointer_cursor
         | role(a11y::Role::button) | label("shortcut")
         | id_of(base, index);
}

// ── form layout ─────────────────────────────────────────────────────────

/// One setting: a title, an optional explanation, and the control, which is
/// right-aligned and keeps its own size. The text column takes the rest and
/// wraps its description rather than squeezing the control.
template <ControlContext C, Element Control>
[[nodiscard]] inline Node setting_row(const C& c, std::string_view title_text,
                                      std::string_view description_text, Control control) {
    const Theme& t = c.theme;
    return (h(v(text_owned(std::string{title_text}) | font(t.font_size) | fg(t.text_primary),
                text_owned(std::string{description_text}) | font(t.font_size * 0.86f)
                    | fg(t.text_secondary) | wrap_text | line_height(1.35f))
                | gap(3) | grow() | min_size(0, 0),
              v(control) | align(Align::end))
            | gap(24) | align(Align::center) | pad(14, 18) | width(pct(100))).build();
}

/// A titled card of setting rows with hairlines between them.
template <ControlContext C>
[[nodiscard]] inline Node section(const C& c, std::string_view title_text, std::vector<Node> rows) {
    const Theme& t = c.theme;
    std::vector<Node> body;
    body.reserve(rows.size() * 2);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) body.push_back((box() | height(1) | width(pct(100)) | bg(t.border.fade(0.7f))).build());
        body.push_back(std::move(rows[i]));
    }
    return (v(text_owned(std::string{title_text}) | font(t.font_size * 0.78f) | semibold
                  | tracking(0.8f) | fg(t.text_secondary) | pad(0, 4),
              v(v_list(std::move(body)))
                  | width(pct(100))
                  | bg(t.surface) | border(1, t.border) | radius(t.radius_medium))
            | gap(8) | width(pct(100))).build();
}

/// A sidebar destination. Interaction: Clicked / Activated on node_id(base, index).
template <ControlContext C>
[[nodiscard]] inline Node nav_item(const C& c, std::string_view base, std::size_t index,
                                   std::string_view icon, std::string_view name, bool active) {
    const Theme& t = c.theme;
    const auto pid = part_id(base, index);
    const bool hov = c.hovered(pid);
    return (h(text_owned(std::string{icon}) | font(t.font_size) | fg(active ? t.accent : t.text_secondary)
                  | text_align(TextAlign::center) | width(18),
              text_owned(std::string{name}) | font(t.font_size)
                  | (active ? semibold : weight(FontWeight::regular))
                  | fg(active || hov ? t.text_primary : t.text_secondary))
            | gap(10) | align(Align::center) | pad(7, 10) | width(pct(100))
            | bg(active ? t.surface_raised : (hov ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f)))
            | border(1, c.focused(pid) ? t.focus_ring : t.border.fade(0.0f))
            | radius(t.radius_small)
            | pointer_cursor
            | role(a11y::Role::tab) | selected(active) | label(name)
            | id_of(base, index)).build();
}

}  // namespace mayag::dsl
