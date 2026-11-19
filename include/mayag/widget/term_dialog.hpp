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

template <ControlContext C>
[[nodiscard]] inline Node term_dialog(const C& c, std::string_view id, const TermDialogSpec& s) {
    const Theme& t = c.theme;
    const float fs = t.font_size * 0.95f;
    const auto paper = t.background, ink = t.text_primary;
    const auto dim = mix(ink, paper, 0.45f), faint = mix(paper, ink, 0.13f);
    const auto tone = s.tone.a > 0.0f ? s.tone : t.accent;
    const float r = t.rounded(6);
    const auto mono = [&](std::string str, float size, Color<Srgb> col) {
        return text_owned(std::move(str)) | font(size) | monospace | fg(col);
    };

    // ── the frame's top edge: a tag, the title rule, the meta ────────────
    auto head = h(h(mono(s.tag, fs * 0.82f, paper) | semibold) | height(22.0f) | pad(0, 8.0f) | center | bg(tone),
                  box() | height(1.0f) | grow() | bg(mix(paper, tone, 0.45f)),
                  mono(s.meta, fs * 0.8f, dim))
              | gap(10.0f) | align(Align::center) | width(pct(100));

    std::vector<Node> rows;
    rows.push_back(head.build());
    rows.push_back((mono(s.title, fs * 1.12f, ink) | semibold).build());
    if (!s.body.empty()) rows.push_back((text_owned(s.body) | font(fs * 0.9f) | fg(dim) | wrap_text | line_height(1.4f)).build());

    // ── the preview: numbered, flagged lines tinted ──────────────────────
    if (!s.preview.empty()) {
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(s.time * 4.0));
        const int digits = static_cast<int>(std::to_string(s.preview.size() + static_cast<std::size_t>(s.preview_more)).size());
        std::vector<Node> lines;
        for (std::size_t i = 0; i < s.preview.size(); ++i) {
            const auto& l = s.preview[i];
            std::string num = std::to_string(i + 1);
            num = std::string(static_cast<std::size_t>(std::max(digits - static_cast<int>(num.size()), 0)), ' ') + num;
            lines.push_back((h(mono(l.flag ? "\u25B6" : " ", fs * 0.7f, t.danger.fade(0.6f + 0.4f * pulse)) | width(10.0f),
                               mono(num, fs * 0.85f, l.flag ? t.danger : faint.fade(1.0f)),
                               mono(l.text.empty() ? std::string{" "} : l.text, fs * 0.92f, l.flag ? ink : mix(ink, paper, 0.15f))
                                   | grow() | ellipsis | min_size(0, 0),
                               mono(l.note.empty() ? std::string{} : "\u26A0 " + l.note, fs * 0.8f, t.danger))
                             | gap(8.0f) | align(Align::center) | height(std::round(fs * 1.55f)) | pad(0, 8.0f, 0, 4.0f)
                             | bg(l.flag ? t.danger.fade(0.10f + 0.05f * pulse) : paper.fade(0.0f))).build());
        }
        if (s.preview_more > 0)
            lines.push_back((h(mono("\u22EF " + std::to_string(s.preview_more) + " more line" + (s.preview_more == 1 ? "" : "s"),
                                    fs * 0.85f, dim))
                             | height(std::round(fs * 1.5f)) | pad(0, 22.0f)).build());
        rows.push_back((v(std::move(lines)) | pad(4.0f, 0) | width(pct(100)) | bg(mix(paper, ink, 0.025f))
                        | border(1.0f, faint) | radius(r) | clip).build());
    }

    // ── the actions, with their keys; the focused one is filled ──────────
    std::vector<Node> foot;
    if (!s.remember.empty()) {
        const auto rid = part_id(id, 100);
        const bool hov = c.hovered(rid);
        foot.push_back((h(h(mono(s.remember_on ? "\u2713" : "", fs * 0.8f, paper) | semibold)
                              | size(16.0f, 16.0f) | center | radius(std::min(r, 3.0f))
                              | bg(s.remember_on ? tone : paper) | border(1.0f, s.remember_on ? tone : hov ? dim : faint),
                          mono(s.remember, fs * 0.82f, hov ? ink : dim))
                        | gap(8.0f) | align(Align::center) | pointer_cursor | id_of(id, 100)).build());
    }
    foot.push_back((box() | grow()).build());
    for (std::size_t i = 0; i < s.actions.size(); ++i) {
        const auto& a = s.actions[i];
        const auto pid = part_id(id, i);
        const bool hov = c.hovered(pid), focus = static_cast<int>(i) == s.focused;
        const auto face = a.tone == DialogAction::Tone::danger ? t.danger : a.tone == DialogAction::Tone::primary ? tone : dim;
        const bool filled = focus || hov;
        foot.push_back((h(mono(a.label, fs * 0.92f, filled ? paper : face) | semibold,
                          mono(a.key, fs * 0.75f, filled ? paper.fade(0.75f) : faint.fade(1.0f)))
                        | gap(7.0f) | align(Align::center) | height(30.0f) | pad(0, 12.0f) | radius(std::min(r, 5.0f))
                        | bg(filled ? (hov && !focus ? face.fade(0.85f) : face) : paper.fade(0.0f))
                        | border(1.0f, filled ? face : mix(paper, face, 0.5f))
                        | pointer_cursor | role(a11y::Role::button) | label(a.label) | id_of(id, i)).build());
    }
    rows.push_back((h(std::move(foot)) | gap(8.0f) | align(Align::center) | width(pct(100))).build());

    // the entrance: fade, rise 14px, settle from 96%
    const float e = std::clamp(s.appear, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);
    return (v(std::move(rows)) | gap(12.0f) | pad(14.0f, 18.0f, 16.0f, 18.0f) | width(s.width)
            | bg(mix(paper, ink, 0.04f)) | border(1.0f, mix(paper, tone, 0.55f)) | radius(r)
            | shadow(36.0f, colors::black.fade(0.55f * ease), {0.0f, 14.0f})
            | opacity(ease) | translate(0.0f, (1.0f - ease) * 14.0f) | scale(0.96f + 0.04f * ease)
            | role(a11y::Role::dialog) | label(s.title)).build();
}

}  // namespace mayag::dsl
