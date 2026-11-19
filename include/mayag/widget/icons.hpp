#pragma once
// mayag::widget icons — a stroke icon set, drawn, not fonted
//
//   icon(c, Icon::folder)                    16 px, text colour
//   icon(c, Icon::git_branch, 14, t.accent)
//
// Zed's chrome is quiet because its icons are: one stroke weight, round
// caps, a 16-unit grid, no fills. These are drawn the same way, from the
// renderer's own analytic shapes -- capsule lines (round caps, so a
// polyline joins cleanly), arcs, rings and rounded-rect strokes -- so they
// are pixel-crisp at any size and any DPI, need no icon font installed, and
// take the theme's colour like text does.
//
// Every icon is a short program on a 16x16 grid: `L` line, `P` polyline,
// `A` arc, `C` circle (ring), `D` dot (filled circle), `R` rounded rect.

#include "control.hpp"
#include "../render/draw_list.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace mayag::dsl {

enum class Icon : std::uint8_t {
    // files and workspace
    folder, folder_open, file, file_code, files, search, replace, outline, list_tree,
    // git
    git_branch, git_commit, git_diff, git_pull, git_push, plus_square, minus_square,
    // terminal and remote
    terminal, server, link, globe, key, plug,
    // chrome
    chevron_down, chevron_right, chevron_left, chevron_up, close, plus, minus, check, more, more_vertical,
    menu, settings, command, filter, refresh, undo, external, copy, trash, pencil, eye, pin, star,
    // docks and layout
    panel_left, panel_right, panel_bottom, split_h, split_v, maximize, layout,
    // status
    circle_alert, circle_x, triangle_alert, info, bell, sparkles, clock, home, arrow_up, arrow_down,
    count_
};

namespace icons_detail {

enum Op : std::uint8_t { L, P, A, C, D, R, END };

/// One icon: numbers on a 16-unit grid, decoded by draw_icon.
///   L x0 y0 x1 y1           a line
///   P n x0 y0 ... xn yn     a polyline of n+1 points
///   A cx cy r start sweep   an arc, angles in turns, 0 = 3 o'clock, y-down
///   C cx cy r               a ring
///   D cx cy r               a filled dot
///   R x y w h r             a rounded-rect stroke
using Prog = std::initializer_list<float>;

// clang-format off
inline const Prog& program(Icon i) {
    static const Prog k[static_cast<int>(Icon::count_)] = {
        /* folder */      {R, 1.5f, 3.f, 13.f, 10.5f, 1.5f, P, 2, 1.5f, 5.5f, 6.f, 5.5f, 7.5f, 3.f, END},
        /* folder_open */ {P, 4, 1.5f, 12.f, 1.5f, 4.5f, 2.5f, 3.f, 6.f, 3.f, 7.5f, 4.5f, P, 4, 1.5f, 12.f, 4.f, 7.5f, 14.5f, 7.5f, 12.f, 13.f, 1.5f, 13.f, END},
        /* file */        {P, 5, 9.5f, 1.5f, 3.5f, 1.5f, 3.5f, 14.5f, 12.5f, 14.5f, 12.5f, 4.5f, 9.5f, 1.5f, P, 2, 9.5f, 1.5f, 9.5f, 4.5f, 12.5f, 4.5f, END},
        /* file_code */   {P, 5, 9.5f, 1.5f, 3.5f, 1.5f, 3.5f, 14.5f, 12.5f, 14.5f, 12.5f, 4.5f, 9.5f, 1.5f, P, 2, 7.f, 8.f, 5.5f, 9.75f, 7.f, 11.5f, P, 2, 9.f, 8.f, 10.5f, 9.75f, 9.f, 11.5f, END},
        /* files */       {P, 4, 6.5f, 3.5f, 6.5f, 1.5f, 11.5f, 1.5f, 14.5f, 4.5f, 14.5f, 11.5f, P, 1, 14.5f, 11.5f, 12.5f, 11.5f, R, 1.5f, 4.5f, 8.f, 10.f, 1.f, END},
        /* search */      {C, 7.f, 7.f, 4.75f, L, 10.5f, 10.5f, 14.f, 14.f, END},
        /* replace */     {P, 2, 2.f, 6.f, 2.f, 3.f, 11.f, 3.f, P, 2, 9.f, 1.f, 11.f, 3.f, 9.f, 5.f, P, 2, 14.f, 10.f, 14.f, 13.f, 5.f, 13.f, P, 2, 7.f, 11.f, 5.f, 13.f, 7.f, 15.f, END},
        /* outline */     {L, 2.f, 3.5f, 14.f, 3.5f, L, 5.f, 8.f, 14.f, 8.f, L, 5.f, 12.5f, 14.f, 12.5f, D, 2.5f, 8.f, 0.9f, D, 2.5f, 12.5f, 0.9f, END},
        /* list_tree */   {L, 2.f, 3.f, 6.f, 3.f, P, 2, 3.5f, 3.f, 3.5f, 12.f, 7.f, 12.f, L, 3.5f, 7.5f, 7.f, 7.5f, L, 9.f, 7.5f, 14.f, 7.5f, L, 9.f, 12.f, 14.f, 12.f, L, 8.f, 3.f, 14.f, 3.f, END},
        /* git_branch */  {C, 4.5f, 3.5f, 1.75f, C, 4.5f, 12.5f, 1.75f, C, 11.5f, 4.5f, 1.75f, L, 4.5f, 5.25f, 4.5f, 10.75f, P, 2, 11.5f, 6.25f, 11.5f, 7.f, 4.5f, 10.f, END},
        /* git_commit */  {C, 8.f, 8.f, 2.75f, L, 1.f, 8.f, 5.25f, 8.f, L, 10.75f, 8.f, 15.f, 8.f, END},
        /* git_diff */    {L, 4.5f, 2.f, 4.5f, 8.f, L, 1.5f, 5.f, 7.5f, 5.f, L, 8.5f, 12.f, 14.5f, 12.f, R, 9.f, 1.5f, 5.5f, 5.5f, 1.f, END},
        /* git_pull */    {C, 4.f, 3.5f, 1.75f, C, 4.f, 12.5f, 1.75f, L, 4.f, 5.25f, 4.f, 10.75f, C, 12.f, 12.5f, 1.75f, P, 2, 12.f, 10.75f, 12.f, 5.f, 8.f, 5.f, P, 2, 9.5f, 3.5f, 8.f, 5.f, 9.5f, 6.5f, END},
        /* git_push */    {L, 8.f, 14.f, 8.f, 3.f, P, 2, 4.f, 6.5f, 8.f, 2.5f, 12.f, 6.5f, L, 3.f, 14.f, 13.f, 14.f, END},
        /* plus_square */ {R, 2.f, 2.f, 12.f, 12.f, 2.f, L, 8.f, 5.f, 8.f, 11.f, L, 5.f, 8.f, 11.f, 8.f, END},
        /* minus_square */{R, 2.f, 2.f, 12.f, 12.f, 2.f, L, 5.f, 8.f, 11.f, 8.f, END},
        /* terminal */    {P, 2, 2.5f, 4.f, 6.f, 7.5f, 2.5f, 11.f, L, 8.f, 12.f, 13.5f, 12.f, END},
        /* server */      {R, 2.f, 2.f, 12.f, 5.f, 1.5f, R, 2.f, 9.f, 12.f, 5.f, 1.5f, D, 4.75f, 4.5f, 0.85f, D, 4.75f, 11.5f, 0.85f, END},
        /* link */        {A, 5.f, 11.f, 3.f, 0.125f, 0.75f, A, 11.f, 5.f, 3.f, 0.625f, 0.75f, L, 6.f, 10.f, 10.f, 6.f, END},
        /* globe */       {C, 8.f, 8.f, 6.5f, L, 1.5f, 8.f, 14.5f, 8.f, A, 2.f, 8.f, 7.f, 0.875f, 0.25f, A, 14.f, 8.f, 7.f, 0.375f, 0.25f, END},
        /* key */         {C, 5.f, 11.f, 3.f, L, 7.f, 9.f, 14.f, 2.f, L, 11.f, 5.f, 13.f, 7.f, END},
        /* plug */        {L, 6.f, 1.5f, 6.f, 4.5f, L, 10.f, 1.5f, 10.f, 4.5f, P, 4, 3.5f, 4.5f, 12.5f, 4.5f, 12.5f, 8.f, 8.f, 12.f, 3.5f, 8.f, L, 3.5f, 4.5f, 3.5f, 8.f, L, 8.f, 12.f, 8.f, 14.5f, END},
        /* chevron_down */{P, 2, 4.f, 6.f, 8.f, 10.f, 12.f, 6.f, END},
        /* chevron_right*/{P, 2, 6.f, 4.f, 10.f, 8.f, 6.f, 12.f, END},
        /* chevron_left */{P, 2, 10.f, 4.f, 6.f, 8.f, 10.f, 12.f, END},
        /* chevron_up */  {P, 2, 4.f, 10.f, 8.f, 6.f, 12.f, 10.f, END},
        /* close */       {L, 4.f, 4.f, 12.f, 12.f, L, 12.f, 4.f, 4.f, 12.f, END},
        /* plus */        {L, 8.f, 3.f, 8.f, 13.f, L, 3.f, 8.f, 13.f, 8.f, END},
        /* minus */       {L, 3.f, 8.f, 13.f, 8.f, END},
        /* check */       {P, 2, 3.f, 8.5f, 6.5f, 12.f, 13.f, 4.5f, END},
        /* more */        {D, 3.5f, 8.f, 1.1f, D, 8.f, 8.f, 1.1f, D, 12.5f, 8.f, 1.1f, END},
        /* more_vertical*/{D, 8.f, 3.5f, 1.1f, D, 8.f, 8.f, 1.1f, D, 8.f, 12.5f, 1.1f, END},
        /* menu */        {L, 2.5f, 4.f, 13.5f, 4.f, L, 2.5f, 8.f, 13.5f, 8.f, L, 2.5f, 12.f, 13.5f, 12.f, END},
        /* settings */    {C, 8.f, 8.f, 2.25f, C, 8.f, 8.f, 5.5f, L, 8.f, 1.f, 8.f, 2.5f, L, 8.f, 13.5f, 8.f, 15.f, L, 1.f, 8.f, 2.5f, 8.f, L, 13.5f, 8.f, 15.f, 8.f, L, 3.f, 3.f, 4.1f, 4.1f, L, 11.9f, 11.9f, 13.f, 13.f, L, 13.f, 3.f, 11.9f, 4.1f, L, 4.1f, 11.9f, 3.f, 13.f, END},
        /* command */     {R, 6.f, 6.f, 4.f, 4.f, 0.f, A, 4.f, 4.f, 2.f, 0.f, 0.75f, A, 12.f, 4.f, 2.f, 0.25f, 0.75f, A, 12.f, 12.f, 2.f, 0.5f, 0.75f, A, 4.f, 12.f, 2.f, 0.75f, 0.75f, END},
        /* filter */      {P, 3, 2.f, 3.f, 14.f, 3.f, 9.5f, 8.5f, 9.5f, 13.5f, P, 1, 6.5f, 8.5f, 6.5f, 12.f, L, 2.f, 3.f, 6.5f, 8.5f, L, 6.5f, 12.f, 9.5f, 13.5f, END},
        /* refresh */     {A, 8.f, 8.f, 5.5f, 0.0f, 0.8f, P, 2, 13.5f, 2.5f, 13.5f, 5.5f, 10.5f, 5.5f, END},
        /* undo */        {P, 2, 5.f, 2.5f, 2.f, 5.5f, 5.f, 8.5f, A, 9.f, 9.5f, 4.f, 0.75f, 0.5f, L, 2.f, 5.5f, 9.f, 5.5f, L, 9.f, 13.5f, 5.f, 13.5f, END},
        /* external */    {P, 4, 8.f, 3.f, 3.f, 3.f, 3.f, 13.f, 13.f, 13.f, 13.f, 8.f, L, 9.f, 2.f, 14.f, 2.f, L, 14.f, 2.f, 14.f, 7.f, L, 14.f, 2.f, 7.5f, 8.5f, END},
        /* copy */        {R, 5.5f, 5.5f, 9.f, 9.f, 1.5f, P, 2, 3.f, 10.5f, 1.5f, 10.5f, 1.5f, 1.5f, P, 1, 1.5f, 1.5f, 10.5f, 1.5f, L, 10.5f, 1.5f, 10.5f, 3.f, END},
        /* trash */       {L, 2.f, 4.f, 14.f, 4.f, P, 2, 6.f, 4.f, 6.f, 2.f, 10.f, 2.f, L, 10.f, 2.f, 10.f, 4.f, P, 3, 3.5f, 4.f, 4.5f, 14.5f, 11.5f, 14.5f, 12.5f, 4.f, END},
        /* pencil */      {P, 4, 11.f, 2.f, 14.f, 5.f, 5.5f, 13.5f, 2.f, 14.f, 2.5f, 10.5f, L, 2.5f, 10.5f, 11.f, 2.f, L, 9.f, 4.f, 12.f, 7.f, END},
        /* eye */         {A, 8.f, 13.f, 8.5f, 0.635f, 0.23f, A, 8.f, 3.f, 8.5f, 0.135f, 0.23f, C, 8.f, 8.f, 2.f, END},
        /* pin */         {P, 4, 6.f, 2.f, 10.f, 2.f, 9.5f, 6.f, 12.f, 9.f, 4.f, 9.f, P, 1, 4.f, 9.f, 6.5f, 6.f, L, 6.5f, 6.f, 6.f, 2.f, L, 8.f, 9.f, 8.f, 14.5f, END},
        /* star */        {P, 10, 8.f, 1.5f, 9.9f, 5.6f, 14.4f, 6.1f, 11.1f, 9.1f, 12.f, 13.6f, 8.f, 11.3f, 4.f, 13.6f, 4.9f, 9.1f, 1.6f, 6.1f, 6.1f, 5.6f, 8.f, 1.5f, END},
        /* panel_left */  {R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 6.f, 2.5f, 6.f, 13.5f, END},
        /* panel_right */ {R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 10.f, 2.5f, 10.f, 13.5f, END},
        /* panel_bottom */{R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 1.5f, 9.5f, 14.5f, 9.5f, END},
        /* split_h */     {R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 8.f, 2.5f, 8.f, 13.5f, END},
        /* split_v */     {R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 1.5f, 8.f, 14.5f, 8.f, END},
        /* maximize */    {P, 2, 2.f, 6.f, 2.f, 2.f, 6.f, 2.f, P, 2, 10.f, 2.f, 14.f, 2.f, 14.f, 6.f, P, 2, 14.f, 10.f, 14.f, 14.f, 10.f, 14.f, P, 2, 6.f, 14.f, 2.f, 14.f, 2.f, 10.f, END},
        /* layout */      {R, 1.5f, 2.5f, 13.f, 11.f, 1.5f, L, 6.f, 2.5f, 6.f, 13.5f, L, 6.f, 8.f, 14.5f, 8.f, END},
        /* circle_alert */{C, 8.f, 8.f, 6.5f, L, 8.f, 4.5f, 8.f, 8.5f, D, 8.f, 11.25f, 0.9f, END},
        /* circle_x */    {C, 8.f, 8.f, 6.5f, L, 5.75f, 5.75f, 10.25f, 10.25f, L, 10.25f, 5.75f, 5.75f, 10.25f, END},
        /* tri_alert */   {P, 3, 8.f, 2.f, 14.5f, 13.5f, 1.5f, 13.5f, 8.f, 2.f, L, 8.f, 6.5f, 8.f, 9.5f, D, 8.f, 11.75f, 0.85f, END},
        /* info */        {C, 8.f, 8.f, 6.5f, L, 8.f, 7.5f, 8.f, 11.5f, D, 8.f, 4.75f, 0.9f, END},
        /* bell */        {A, 8.f, 7.f, 4.f, 0.5f, 0.5f, L, 4.f, 7.f, 4.f, 10.f, L, 12.f, 7.f, 12.f, 10.f, P, 1, 2.5f, 12.f, 13.5f, 12.f, L, 4.f, 10.f, 2.5f, 12.f, L, 12.f, 10.f, 13.5f, 12.f, L, 6.75f, 14.5f, 9.25f, 14.5f, END},
        /* sparkles */    {P, 4, 6.f, 2.f, 7.2f, 5.8f, 11.f, 7.f, 7.2f, 8.2f, 6.f, 12.f, P, 3, 6.f, 12.f, 4.8f, 8.2f, 1.f, 7.f, 4.8f, 5.8f, L, 4.8f, 5.8f, 6.f, 2.f, L, 12.f, 10.f, 12.f, 14.f, L, 10.f, 12.f, 14.f, 12.f, END},
        /* clock */       {C, 8.f, 8.f, 6.5f, P, 2, 8.f, 4.5f, 8.f, 8.f, 10.5f, 9.5f, END},
        /* home */        {P, 3, 2.f, 7.5f, 8.f, 2.f, 14.f, 7.5f, 14.f, 7.5f, P, 3, 3.5f, 6.5f, 3.5f, 14.f, 12.5f, 14.f, 12.5f, 6.5f, END},
        /* arrow_up */    {L, 8.f, 14.f, 8.f, 2.5f, P, 2, 3.5f, 7.f, 8.f, 2.5f, 12.5f, 7.f, END},
        /* arrow_down */  {L, 8.f, 2.f, 8.f, 13.5f, P, 2, 3.5f, 9.f, 8.f, 13.5f, 12.5f, 9.f, END},
    };
    return k[static_cast<int>(i)];
}
// clang-format on

/// The icon rides in the canvas node's own Style, in fields a canvas
/// never paints and layout never rewrites: `flags` = icon id, text size =
/// stroke weight (grid units), text colour = colour.
inline void paint(DrawList& dl, const Rect& r, const void* user) {
    const Style& s = *static_cast<const Style*>(user);
    const auto which = static_cast<Icon>(s.flags);
    if (static_cast<int>(which) >= static_cast<int>(Icon::count_)) return;
    const float k = std::min(r.width(), r.height()) / 16.0f;
    const Vec2 o{r.origin.x + (r.width() - 16.0f * k) * 0.5f, r.origin.y + (r.height() - 16.0f * k) * 0.5f};
    const float w = std::max(s.text.size * k, 1.0f);   // never thinner than a device pixel
    const Color<Srgb> col = s.text.color;
    const auto at = [&](float x, float y) { return Vec2{o.x + x * k, o.y + y * k}; };
    const auto& prog = program(which);
    const float* p = prog.begin();
    const float* end = prog.end();
    constexpr float tau = 6.2831853f;
    while (p < end) {
        switch (static_cast<Op>(static_cast<int>(*p++))) {
            case L: dl.line(at(p[0], p[1]), at(p[2], p[3]), w, col); p += 4; break;
            case P: {
                const int n = static_cast<int>(*p++);
                for (int i = 0; i < n; ++i) dl.line(at(p[2 * i], p[2 * i + 1]), at(p[2 * i + 2], p[2 * i + 3]), w, col);
                p += 2 * (n + 1);
                break;
            }
            case A: {
                const float sweep = p[4] <= 0.0f ? 1.0f : p[4];
                dl.arc(at(p[0], p[1]), p[2] * k, w, p[3] * tau, sweep * tau, col);
                p += 5;
                break;
            }
            case C: dl.ring(at(p[0], p[1]), p[2] * k, w, col); p += 3; break;
            case D: dl.circle(at(p[0], p[1]), p[2] * k, col); p += 3; break;
            case R: {
                const Rect b{at(p[0], p[1]), Vec2{p[2] * k, p[3] * k}};
                dl.stroke_rect(b, w, col, Corners{p[4] * k}, StrokeAlign::center);
                p += 5;
                break;
            }
            case END: return;
        }
    }
}

}  // namespace icons_detail

/// An icon as a node: `size` px square, `col` (alpha 0 = inherit nothing:
/// the theme's secondary text colour), stroke weight in grid units (1.25 is
/// Zed's/Lucide's look at 16 px).
[[nodiscard]] inline Node icon_node(Icon which, float size, Color<Srgb> col, float weight = 1.3f) {
    Style s;
    s.layout.width = px(size);
    s.layout.height = px(size);
    s.flags = static_cast<std::uint8_t>(which);
    s.text.size = weight;
    s.text.color = col;
    s.text.set |= text_bits::size | text_bits::color;   // never inherited over
    s.layout.shrink = 0.0f;
    return Node::make_canvas(&icons_detail::paint, nullptr, s);
}

template <ControlContext C>
[[nodiscard]] inline Node icon(const C& c, Icon which, float size = 16.0f, Color<Srgb> col = {}) {
    return icon_node(which, size, col.a > 0.0f ? col : c.theme.text_secondary);
}

/// Every icon, for galleries and docs.
[[nodiscard]] inline std::span<const Icon> all_icons() {
    static const auto list = [] {
        std::array<Icon, static_cast<std::size_t>(Icon::count_)> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<Icon>(i);
        return a;
    }();
    return list;
}
[[nodiscard]] inline std::string_view icon_name(Icon i) {
    static constexpr std::string_view n[] = {
        "folder", "folder_open", "file", "file_code", "files", "search", "replace", "outline", "list_tree",
        "git_branch", "git_commit", "git_diff", "git_pull", "git_push", "plus_square", "minus_square",
        "terminal", "server", "link", "globe", "key", "plug",
        "chevron_down", "chevron_right", "chevron_left", "chevron_up", "close", "plus", "minus", "check", "more", "more_vertical",
        "menu", "settings", "command", "filter", "refresh", "undo", "external", "copy", "trash", "pencil", "eye", "pin", "star",
        "panel_left", "panel_right", "panel_bottom", "split_h", "split_v", "maximize", "layout",
        "circle_alert", "circle_x", "triangle_alert", "info", "bell", "sparkles", "clock", "home", "arrow_up", "arrow_down"};
    const auto k = static_cast<std::size_t>(i);
    return k < std::size(n) ? n[k] : "?";
}

}  // namespace mayag::dsl
