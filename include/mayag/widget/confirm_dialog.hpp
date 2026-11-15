#pragma once
// mayag::widget confirm_dialog — the modal questions a terminal asks
//
//   confirm_dialog(c, "paste", {.title = "Paste 12 lines?", .body = ..., .preview = text,
//                               .confirm = "Paste", .danger = false})
//
// Paste protection (multi-line or containing control characters), closing a
// tab with a running process, quitting with several. A modal overlay with a
// scrim, an optional monospace preview (what is about to be pasted, which
// processes will be killed), and the destructive button on the right.
//
// Interaction: Clicked on part_id(id, 0) cancel, part_id(id, 1) confirm,
// part_id(id, 2) the checkbox ("don't ask again"). Enter = confirm, Esc =
// cancel (app keymap).

#include "control.hpp"
#include "../scene/overlay.hpp"

#include <string>

namespace mayag::dsl {

struct ConfirmSpec {
    std::string title;
    std::string body;
    std::string preview;                 ///< monospace block, clipped to ~8 lines
    std::string confirm = "OK";
    std::string cancel = "Cancel";
    bool        danger = false;
    std::string remember;                ///< checkbox label, empty for none
    bool        remember_on = false;
};

template <ControlContext C>
inline void confirm_dialog(const C& c, std::string_view id, const ConfirmSpec& s) {
    const Theme& t = c.theme;
    std::string preview = s.preview;
    {   // at most 8 lines, the rest summarised
        int lines = 0;
        std::size_t cut = std::string::npos;
        for (std::size_t i = 0; i < preview.size(); ++i)
            if (preview[i] == '\n' && ++lines == 8) { cut = i; break; }
        if (cut != std::string::npos) {
            int rest = 0;
            for (std::size_t i = cut; i < preview.size(); ++i) rest += preview[i] == '\n';
            preview = preview.substr(0, cut) + "\n\u22EF " + std::to_string(rest) + " more lines";
        }
    }
    const auto button_ = [&](std::size_t part, const std::string& text_, bool primary) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        const Color<Srgb> face = primary ? (s.danger ? t.danger : t.accent) : (hov ? t.surface_raised : t.surface);
        return (h(text_owned(text_) | font(t.font_size) | semibold | fg(primary ? t.on_accent : t.text_primary))
                | pad(7, 16) | radius(t.radius_small) | bg(primary && hov ? mix(face, colors::white, 0.1f) : face)
                | border(1, primary ? face : t.border) | pointer_cursor | role(a11y::Role::button) | id_of(id, part)).build();
    };
    Node pv = preview.empty() ? Node{}
        : (v(text_owned(preview) | font(t.font_size * 0.92f) | fg(t.text_secondary) | no_ligatures | wrap_text | line_height(1.4f))
           | pad(10, 12) | width(pct(100)) | bg(t.background) | border(1, t.border) | radius(t.radius_small)).build();
    Node rem = s.remember.empty() ? Node{}
        : (h(h(text_owned(s.remember_on ? "\u2713" : "") | font(11) | bold | fg(t.on_accent)) | size(16, 16) | center | radius(4)
                 | bg(s.remember_on ? t.accent : t.surface) | border(1, t.border_strong),
             text_owned(s.remember) | font(t.font_size * 0.9f) | fg(t.text_secondary))
           | gap(8) | align(Align::center) | pointer_cursor | id_of(id, 2)).build();
    c.overlay(Overlay{
        .content = (v(text_owned(s.title) | font(t.font_size * 1.15f) | semibold | fg(t.text_primary),
                      text_owned(s.body) | font(t.font_size) | fg(t.text_secondary) | wrap_text | line_height(1.45f),
                      node(std::move(pv)),
                      h(node(std::move(rem)), spacer(), node(button_(0, s.cancel, false)), node(button_(1, s.confirm, true)))
                          | gap(8) | align(Align::center) | width(pct(100)))
                    | gap(12) | pad(18, 20) | width(460)
                    | bg(t.overlay) | border(1, t.border_strong) | radius(t.radius_medium) | elevation(24)
                    | role(a11y::Role::dialog) | label(s.title)).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::centered,
        .dismiss = Dismiss::modal,
        .layer = 30,
        .scrim = 0.45f,
    });
}

}  // namespace mayag::dsl
