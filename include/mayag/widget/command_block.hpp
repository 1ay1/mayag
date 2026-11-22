#pragma once
// mayag::widget command_block — one command and its output, Warp-style
//
//   command_block(c, "block", i, info, output_view)
//
// With shell integration (OSC 133) a terminal knows where each command's
// prompt, input and output begin. This draws one such block: the prompt
// context (cwd, git branch), the command, its exit status and duration, the
// output (any Node — usually a terminal_view over those rows), and hover
// actions: copy command, copy output, rerun, collapse. A failed command gets
// a red edge; a running one an animated-free "running" pill.
//
// Interaction (parts under part_id(id, i)'s base, i = block index):
//   Clicked on part_id(id, i)                  -> select block i
//   Clicked on part_id(id, block_part(i, k))   -> action k (see block_action)

#include "control.hpp"

#include <string>

namespace mayag::dsl {

namespace block_action {
inline constexpr std::size_t copy_command = 0, copy_output = 1, rerun = 2, collapse = 3, share = 4, count = 5;
}
[[nodiscard]] constexpr std::size_t block_part(std::size_t block, std::size_t action) noexcept {
    return part_dec + 8192 + block * 8 + action;
}

struct BlockInfo {
    std::string cwd;
    std::string branch;
    std::string command;
    int         exit_code = -1;      ///< -1 running
    double      seconds = 0.0;       ///< duration (or elapsed while running)
    std::string started;             ///< "14:02:11"
    bool        selected = false;
    bool        collapsed = false;
    int         output_lines = 0;
};

[[nodiscard]] inline std::string format_duration(double s) {
    char buf[32];
    if (s < 1.0) std::snprintf(buf, sizeof buf, "%dms", static_cast<int>(s * 1000.0));
    else if (s < 60.0) std::snprintf(buf, sizeof buf, "%.1fs", s);
    else std::snprintf(buf, sizeof buf, "%dm %02ds", static_cast<int>(s / 60.0), static_cast<int>(s) % 60);
    return buf;
}

template <ControlContext C>
[[nodiscard]] inline Node command_block(const C& c, std::string_view id, std::size_t index, const BlockInfo& b,
                                        Node output) {
    const Theme& t = c.theme;
    const auto pid = part_id(id, index);
    const bool hov = c.hovered(pid) || b.selected;
    const bool failed = b.exit_code > 0, running = b.exit_code < 0;
    const Color<Srgb> edge = failed ? t.danger : running ? t.info : b.selected ? t.accent : t.border.fade(0.0f);

    std::vector<Node> actions;
    if (hov) {
        const auto action = [&](std::size_t k, const char* glyph, const char* what) {
            const auto aid = part_id(id, block_part(index, k));
            actions.push_back((h(text_owned(glyph) | font(t.type(Tier::body)) | fg(c.hovered(aid) ? t.text_primary : t.text_secondary))
                               | size(24, 20) | center | radius(t.rounded(4)) | bg(c.hovered(aid) ? t.surface_raised : t.surface.fade(0.0f))
                               | pointer_cursor | label(what) | id_of(id, block_part(index, k))).build());
        };
        action(block_action::copy_command, "\u2398", "copy command");
        action(block_action::copy_output, "\u2263", "copy output");
        action(block_action::rerun, "\u21BB", "rerun");
        action(block_action::collapse, b.collapsed ? "\u203A" : "\u2304", b.collapsed ? "expand" : "collapse");
    }
    Node status = running
        ? (h(text<"running"> | font(t.type(Tier::small)) | fg(var::info)) | pad(t.spaced(1), t.spaced(7)) | radius(t.rounded(9)) | bg(t.tint(t.info, 0.15f))).build()
        : failed ? (h(text_owned("exit " + std::to_string(b.exit_code)) | font(t.type(Tier::small)) | fg(var::danger))
                    | pad(t.spaced(1), t.spaced(7)) | radius(t.rounded(9)) | bg(t.tint(t.danger, 0.15f))).build()
                 : (text<"\u2713"> | font(t.type(Tier::small)) | fg(var::success)).build();

    auto header = h(text_owned(b.cwd) | font(t.type(Tier::small)) | fg(var::info),
                    text_owned(b.branch.empty() ? "" : " \u2387 " + b.branch) | font(t.type(Tier::small)) | fg(var::text_disabled),
                    spacer(),
                    h(h_list(std::move(actions), 2.0f)),
                    text_owned(b.started) | font(t.type(Tier::small)) | fg(var::text_disabled),
                    text_owned(running ? "" : format_duration(b.seconds)) | font(t.type(Tier::small)) | fg(var::text_disabled),
                    node(std::move(status)))
                | gap(t.spaced(10)) | align(Align::center) | width(pct(100)) | height(20);
    auto cmd = h(text<"\u276F"> | font(t.type(Tier::body)) | bold | fg(failed ? t.danger : t.success),
                 text_owned(b.command) | font(t.type(Tier::body)) | semibold | fg(var::text_primary) | no_ligatures | ellipsis
                     | grow() | min_size(0, 0))
             | gap(t.spaced(8)) | align(Align::center) | width(pct(100));
    Node body = b.collapsed
        ? (text_owned("\u22EF " + std::to_string(b.output_lines) + " lines hidden") | font(t.type(Tier::small))
           | fg(var::text_disabled)).build()
        : std::move(output);
    return (h(box() | width(3) | bg(edge),
              v(std::move(header), std::move(cmd), v(node(std::move(body))) | width(pct(100)))
                  | gap(t.spaced(4)) | pad(t.spaced(8), t.spaced(12)) | grow() | min_size(0, 0))
            | width(pct(100)) | align(Align::stretch)
            | bg(b.selected ? t.tint(t.accent, 0.08f) : hov ? t.surface.fade(0.6f) : t.background.fade(0.0f))
            | border(t.stroke(1), b.selected ? t.accent.fade(0.4f) : t.border.fade(0.5f))
            | role(a11y::Role::list_item) | label(b.command)
            | id_of(id, index)).build();
}

}  // namespace mayag::dsl
