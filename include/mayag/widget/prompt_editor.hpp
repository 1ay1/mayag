#pragma once
// mayag::widget prompt_editor — write the message to an agent like a human
//
//   prompt_editor(c, "prompt", m.draft, {.target = "claude · tab 2", .files = ..., ...})
//
// Agent TUIs read their input line by line in a pty, so long prompts are
// miserable to type in place. This is a real multi-line editor that floats
// over the bottom of the pane: wrap, caret, selection, @-file chips, a
// target (which pane / agent receives it), and send — the app pastes it into
// that pty as ONE bracketed paste plus Enter, which every agent CLI accepts.
// Also used for broadcasting one command to several panes.
//
// Interaction: part_id(id, 0) field, 1 send, 2 cancel, 3 target menu,
//              part_id(id, 64 + k) remove attachment k.

#include "control.hpp"
#include "key_hint.hpp"
#include "../core/text_edit.hpp"
#include "../scene/overlay.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace mayag::dsl {

struct PromptEditorSpec {
    std::string              target;          ///< "claude · tab 2 · fix/cell-px"
    std::vector<std::string> attachments;     ///< @-mentioned files / selections
    bool                     broadcast = false;   ///< send to several panes
    int                      broadcast_count = 0;
    bool                     cursor_on = true;
    std::uint64_t            anchor = 0;      ///< the pane node to float over (0: window bottom)
};

template <ControlContext C>
inline void prompt_editor(const C& c, std::string_view id, const TextEditState& in, const PromptEditorSpec& s) {
    const Theme& t = c.theme;
    const float fs = t.type(Tier::body);
    const float lh = std::round(fs * 1.5f);
    std::vector<std::string> lines;
    for (std::size_t st = 0, i = 0; i <= in.text.size(); ++i)
        if (i == in.text.size() || in.text[i] == '\n') { lines.push_back(in.text.substr(st, i - st)); st = i + 1; }
    std::vector<Node> rows;
    std::size_t off = 0;
    for (const auto& l : lines) {
        const std::size_t a = off, b = off + l.size();
        std::vector<Node> layers;
        if (in.has_selection() && in.selection_start() <= b && in.selection_end() >= a) {
            const std::size_t s0 = std::max(in.selection_start(), a), s1 = std::min(in.selection_end(), b);
            const float x0 = detail::text_advance(c, l.substr(0, s0 - a), fs);
            const float x1 = in.selection_end() > b ? detail::text_advance(c, l, fs) + fs * 0.4f : detail::text_advance(c, l.substr(0, s1 - a), fs);
            if (x1 > x0) layers.push_back((box() | size(x1 - x0, lh) | bg(t.tint(t.accent, 0.35f)) | radius(t.rounded(2)) | absolute(x0, 0)).build());
        }
        layers.push_back((h(text_owned(l.empty() ? std::string{" "} : l) | font(fs) | fg(var::text_primary) | clip_text)
                          | height(lh) | align(Align::center) | absolute(0, 0)).build());
        if (s.cursor_on && in.caret >= a && in.caret <= b)
            layers.push_back((box() | size(2, lh - 6) | bg(var::accent) | radius(t.rounded(1))
                              | absolute(detail::text_advance(c, l.substr(0, in.caret - a), fs), 3)).build());
        rows.push_back((z(h_list(std::move(layers))) | height(lh) | width(pct(100))).build());
        off = b + 1;
    }
    if (in.text.empty())
        rows = {(z(text<"Write to the agent\u2026  @ to attach a file, \u21E7\u23CE newline"> | font(fs) | fg(var::text_disabled) | absolute(0, 4),
                   box() | size(2, lh - 6) | bg(t.accent.fade(s.cursor_on ? 1.0f : 0.0f)) | absolute(0, 3))
                 | height(lh) | width(pct(100))).build()};
    std::vector<Node> chips;
    for (std::size_t k = 0; k < s.attachments.size(); ++k) {
        const auto xid = part_id(id, 64 + k);
        chips.push_back((h(text<"@"> | font(t.type(Tier::small)) | fg(var::info),
                           text_owned(s.attachments[k]) | font(t.type(Tier::small)) | fg(var::text_secondary),
                           h(text<"\u00D7"> | font(t.type(Tier::small)) | fg(c.hovered(xid) ? t.text_primary : t.text_disabled)) | pointer_cursor | id_of(id, 64 + k))
                         | gap(t.spaced(4)) | align(Align::center) | pad(t.spaced(1), t.spaced(6)) | radius(t.rounded(4)) | bg(t.tint(t.info, 0.12f))).build());
    }
    const auto send = part_id(id, 1), tgt = part_id(id, 3);
    const bool can = !in.text.empty();
    c.overlay(Overlay{
        .content = (v(h(text<"\u2192"> | font(t.type(Tier::small)) | fg(var::text_disabled),
                        h(text_owned(s.broadcast ? "broadcast to " + std::to_string(s.broadcast_count) + " panes" : s.target)
                              | font(t.type(Tier::small)) | fg(s.broadcast ? t.danger : t.text_secondary),
                          text<" \u2304"> | font(t.type(Tier::fine)) | fg(var::text_disabled))
                            | pad(t.spaced(1), t.spaced(6)) | radius(t.rounded(4)) | bg(c.hovered(tgt) ? t.surface_raised : t.surface.fade(0.0f)) | pointer_cursor | id_of(id, 3),
                        spacer(),
                        node(key_hint(t, "esc", "cancel").build()))
                        | gap(t.spaced(6)) | align(Align::center) | width(pct(100)),
                      node(chips.empty() ? Node{} : (h_list(std::move(chips), 6.0f) | wrap | gap(t.spaced(6))).build()),
                      v(v_list(std::move(rows))) | width(pct(100)) | max_size(1e6f, lh * 12.0f) | clip | cursor(CursorShape::text) | id_of(id, 0),
                      h(text_owned(std::to_string(in.text.size()) + " chars") | font(t.type(Tier::fine)) | fg(var::text_disabled),
                        spacer(),
                        h(text<"Send"> | font(t.type(Tier::small)) | semibold | fg(can ? t.on_accent : t.text_disabled),
                          text<"\u2318\u23CE"> | font(t.type(Tier::fine)) | fg(can ? t.on_accent.fade(0.7f) : t.text_disabled))
                            | gap(t.spaced(6)) | pad(t.spaced(4), t.spaced(12)) | radius(t.radius_small)
                            | bg(can ? (c.hovered(send) ? t.accent_hover : (s.broadcast ? t.danger : t.accent)) : t.surface_raised)
                            | cursor(can ? CursorShape::pointer : CursorShape::arrow) | id_of(id, 1))
                        | align(Align::center) | width(pct(100)))
                    | gap(t.spaced(8)) | pad(t.spaced(10), t.spaced(12)) | width(640)
                    | bg(var::overlay) | border(t.stroke(1), s.broadcast ? t.danger : t.accent.fade(0.6f)) | radius(t.radius_medium) | elevation(t.lift(20))
                    | role(a11y::Role::dialog) | label("prompt editor")).build(),
        .id = part_id(id, part_menu),
        .anchor_id = s.anchor,
        .placement = s.anchor != 0 ? Placement::above_end : Placement::centered,
        .dismiss = Dismiss::never,
        .offset = -12.0f,
        .layer = 18,
    });
}

}  // namespace mayag::dsl
