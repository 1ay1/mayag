#pragma once
// mayag::widget term_dialog — a confirmation that looks like it belongs in a terminal
//
//   ┌─[ PASTE ]──────────────────────────────────────── 3 lines · 84 chars ─┐
//   │ Paste 3 lines into fish?                                              │
//   │ Two of them look dangerous.                                           │
//   │ ┌──────────────────────────────────────────────────────────────────┐ │
//   │ │ 1  sudo pacman -Syu                                    ⚠ root    │ │
//   │ │ 2  rm -rf build                                        ⚠ deletes │ │
//   │ │ 3  cmake --build build                                           │ │
//   │ └──────────────────────────────────────────────────────────────────┘ │
//   │ [ ] don't ask for multi-line pastes        cancel  one line  ▌paste │
//   └───────────────────────────────────────────────────────────────────────┘
//
// Monospace, square-ish, a coloured tag, a preview with line numbers where
// flagged lines are tinted and annotated, and N actions with their keys; the
// focused one is filled. `appear` (0..1) drives the entrance: it fades, rises
// and settles from a slight scale. Pure view -- the app owns focus, keys and
// the overlay (so it can animate the exit too).
//
// Interaction: Clicked on part_id(id, i) runs action i; part_id(id, 100)
// toggles the remember box.

#include "control.hpp"
#include "icons.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mayag::dsl {

struct DialogLine {
    std::string text;
    bool        flag = false;      ///< tinted and marked
    std::string note;              ///< why, at the right ("root", "deletes")
};

struct DialogAction {
    enum class Tone : std::uint8_t { normal, primary, danger };
    std::string label;
    std::string key;               ///< "⏎", "esc", "⇧⏎"
    Tone        tone = Tone::normal;
};

struct TermDialogSpec {
    std::string tag;               ///< "PASTE", "CLOSE", "QUIT"
    Color<Srgb> tone{};            ///< the tag / frame colour (alpha 0: accent)
    Icon        icon = Icon::count_;   ///< the badge glyph; count_: picked from the tag
    std::string title;
    std::string body;
    std::string meta;              ///< top-right of the frame ("3 lines · 84 chars")
    std::vector<DialogLine> preview;
    int         preview_more = 0;  ///< lines not shown
    std::vector<DialogAction> actions;
    int         focused = 0;
    std::string remember;          ///< a checkbox label ("" = none)
    bool        remember_on = false;
    float       width = 580.0f;
    float       appear = 1.0f;     ///< 0..1, the entrance
    double      time = 0.0;        ///< for the flagged-line pulse
};

namespace term_dialog_detail {
/// A scheme colour made fit to be a large surface. Terminal palettes carry
/// raw primaries (Clu's red is #FF0000) that are right for a glyph and
/// garish as a button: in Oklch the chroma is capped and the lightness kept
/// in a band that reads on this background, the hue untouched -- so it is
/// still the scheme's red, just one a designer would have picked.
[[nodiscard]] inline Color<Srgb> tame(Color<Srgb> c, Color<Srgb> paper) {
    auto l = c.to<Oklch>();
    const bool dark = luminance(paper) < 0.4f;
    l.c1 = std::min(l.c1, 0.165f);
    l.c0 = dark ? std::clamp(l.c0, 0.60f, 0.72f) : std::clamp(l.c0, 0.50f, 0.60f);
    auto out = l.to<Srgb>();
    out.c0 = std::clamp(out.c0, 0.0f, 1.0f); out.c1 = std::clamp(out.c1, 0.0f, 1.0f); out.c2 = std::clamp(out.c2, 0.0f, 1.0f);
    out.a = c.a;
    return out;
}
[[nodiscard]] inline Icon icon_for(const TermDialogSpec& s) {
    if (s.icon != Icon::count_) return s.icon;
    if (s.tag == "QUIT") return Icon::power;
    if (s.tag == "CLOSE") return Icon::circle_x;
    if (s.tag == "PASTE" || s.tag == "CAREFUL") return Icon::triangle_alert;
    return Icon::info;
}
}  // namespace term_dialog_detail

template <ControlContext C>
[[nodiscard]] inline Node term_dialog(const C& c, std::string_view id, const TermDialogSpec& s) {
    namespace d = term_dialog_detail;
    const Theme& t = c.theme;
    const float fs = t.font_size * 0.95f;
    const auto paper = t.background, ink = t.text_primary;
    // Neutral greys from the scheme's own fg/bg: the dialog is quiet, and
    // colour lands only where it means something (the badge, the action).
    const auto dim = mix(ink, paper, 0.40f), hair = mix(paper, ink, 0.10f);
    const auto tone = d::tame(s.tone.a > 0.0f ? s.tone : t.accent, paper);
    const auto danger = d::tame(t.danger, paper);
    const float r = t.rounded(6);
    const auto mono = [&](std::string str, float size, Color<Srgb> col) {
        return text_owned(std::move(str)) | font(size) | monospace | fg(col);
    };

    // ── head: a tinted badge, an eyebrow + title + body, the meta ───────
    auto badge = h(node(icon_node(d::icon_for(s), 18.0f, tone, 1.5f)))
               | size(38.0f, 38.0f) | center | radius(t.rounded(10.0f))
               | bg(tone.fade(0.13f)) | border(1.0f, tone.fade(0.28f));
    std::vector<Node> text_col;
    text_col.push_back((h(mono(s.tag, fs * 0.72f, tone) | semibold | tracking(1.2f), box() | grow(),
                          mono(s.meta, fs * 0.78f, dim))
                        | align(Align::center) | width(pct(100))).build());
    text_col.push_back((mono(s.title, fs * 1.18f, ink) | semibold).build());
    if (!s.body.empty())
        text_col.push_back((text_owned(s.body) | font(fs * 0.9f) | fg(dim) | wrap_text | line_height(1.45f) | width(pct(100))).build());
    auto head = h(std::move(badge), v(std::move(text_col)) | gap(5.0f) | grow() | min_size(0, 0))
              | gap(14.0f) | align(Align::start) | width(pct(100));

    std::vector<Node> rows;
    rows.push_back(head.build());

    // ── the preview: numbered, flagged lines tinted ──────────────────────
    if (!s.preview.empty()) {
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(s.time * 4.0));
        const int digits = static_cast<int>(std::to_string(s.preview.size() + static_cast<std::size_t>(s.preview_more)).size());
        std::vector<Node> lines;
        for (std::size_t i = 0; i < s.preview.size(); ++i) {
            const auto& l = s.preview[i];
            std::string num = std::to_string(i + 1);
            num = std::string(static_cast<std::size_t>(std::max(digits - static_cast<int>(num.size()), 0)), ' ') + num;
            lines.push_back((h(mono(l.flag ? "\u25B6" : " ", fs * 0.7f, danger.fade(0.6f + 0.4f * pulse)) | width(10.0f),
                               mono(num, fs * 0.85f, l.flag ? danger : mix(paper, ink, 0.3f)),
                               mono(l.text.empty() ? std::string{" "} : l.text, fs * 0.92f, l.flag ? ink : mix(ink, paper, 0.15f))
                                   | grow() | ellipsis | min_size(0, 0),
                               mono(l.note.empty() ? std::string{} : "\u26A0 " + l.note, fs * 0.8f, danger))
                             | gap(8.0f) | align(Align::center) | height(std::round(fs * 1.55f)) | pad(0, 8.0f, 0, 4.0f)
                             | bg(l.flag ? danger.fade(0.055f + 0.025f * pulse) : paper.fade(0.0f))
                             | when(l.flag, inner_shadow(0.0f, danger.fade(0.8f), {2.0f, 0.0f}))).build());
        }
        if (s.preview_more > 0)
            lines.push_back((h(mono("\u22EF " + std::to_string(s.preview_more) + " more line" + (s.preview_more == 1 ? "" : "s"),
                                    fs * 0.85f, dim))
                             | height(std::round(fs * 1.5f)) | pad(0, 22.0f)).build());
        rows.push_back((v(std::move(lines)) | pad(4.0f, 0) | width(pct(100))
                        | bg(luminance(paper) < 0.4f ? mix(paper, colors::black, 0.18f) : mix(paper, ink, 0.03f))
                        | border(1.0f, hair) | radius(r) | clip).build());
    }

    // ── the actions, in a footer band; the focused one is filled ─────────
    std::vector<Node> foot;
    if (!s.remember.empty()) {
        const auto rid = part_id(id, 100);
        const bool hov = c.hovered(rid);
        Node mark = s.remember_on ? icon_node(Icon::check, 11.0f, readable_on(tone), 2.0f) : box().build();
        foot.push_back((h(h(node(std::move(mark)))
                              | size(15.0f, 15.0f) | center | radius(std::min(r, 4.0f))
                              | bg(s.remember_on ? tone : paper.fade(0.0f)) | border(1.0f, s.remember_on ? tone : hov ? dim : mix(paper, ink, 0.24f)),
                          mono(s.remember, fs * 0.82f, hov ? ink : dim))
                        | gap(8.0f) | align(Align::center) | pointer_cursor | id_of(id, 100)).build());
    }
    foot.push_back((box() | grow()).build());
    for (std::size_t i = 0; i < s.actions.size(); ++i) {
        const auto& a = s.actions[i];
        const auto pid = part_id(id, i);
        const bool hov = c.hovered(pid), focus = static_cast<int>(i) == s.focused;
        const bool coloured = a.tone != DialogAction::Tone::normal;
        const auto face = a.tone == DialogAction::Tone::danger ? danger : tone;
        // Coloured actions: filled when focused, a tinted ghost otherwise.
        // Neutral ones: a raised neutral chip when focused, a hairline ghost
        // otherwise. The key rides along as a small keycap.
        Color<Srgb> fill, edge, label_col;
        if (coloured) {
            fill = focus ? (hov ? mix(face, ink, 0.08f) : face) : face.fade(hov ? 0.22f : 0.12f);
            edge = focus ? face : face.fade(0.35f);
            label_col = focus ? readable_on(face) : face;
        } else {
            fill = focus ? mix(paper, ink, hov ? 0.16f : 0.12f) : hov ? mix(paper, ink, 0.07f) : paper.fade(0.0f);
            edge = focus ? mix(paper, ink, 0.28f) : mix(paper, ink, 0.16f);
            label_col = focus || hov ? ink : mix(ink, paper, 0.2f);
        }
        auto keycap = h(mono(a.key, fs * 0.72f, label_col.fade(0.7f))) | height(17.0f) | pad(0, 5.0f) | center
                    | radius(4.0f) | bg(label_col.fade(0.10f));
        foot.push_back((h(mono(a.label, fs * 0.92f, label_col) | semibold, std::move(keycap))
                        | gap(8.0f) | align(Align::center) | height(32.0f) | pad(0, 8.0f, 0, 14.0f) | radius(t.rounded(7.0f))
                        | bg(fill) | border(1.0f, edge)
                        | when(focus && coloured, shadow(14.0f, face.fade(0.30f), {0.0f, 4.0f}))
                        | pointer_cursor | role(a11y::Role::button) | label(a.label) | id_of(id, i)).build());
    }

    // the entrance: fade, rise 14px, settle from 96%
    const float e = std::clamp(s.appear, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);
    auto body = v(std::move(rows)) | gap(16.0f) | pad(20.0f, 22.0f, 20.0f, 22.0f) | width(pct(100));
    // The footer band: a step DOWN on a dark scheme, a whisper of ink on a
    // light one (darkening a light paper reads as a grey slab that swallows
    // the neutral button's outline).
    const bool dark_paper = luminance(paper) < 0.4f;
    auto footer = h(std::move(foot)) | gap(10.0f) | align(Align::center) | width(pct(100))
                | pad(12.0f, 14.0f, 12.0f, 22.0f) | bg(dark_paper ? mix(paper, colors::black, 0.14f) : mix(paper, ink, 0.025f))
                | inner_shadow(0.0f, hair, {0.0f, 1.0f});
    return (v(std::move(body), std::move(footer)) | width(s.width)
            | bg(dark_paper ? mix(paper, ink, 0.035f) : paper) | border(1.0f, mix(paper, ink, 0.14f)) | radius(t.rounded(12.0f)) | clip
            | shadow(48.0f, colors::black.fade(0.6f * ease), {0.0f, 20.0f})
            | opacity(ease) | translate(0.0f, (1.0f - ease) * 14.0f) | scale(0.96f + 0.04f * ease)
            | role(a11y::Role::dialog) | label(s.title)).build();
}

}  // namespace mayag::dsl
