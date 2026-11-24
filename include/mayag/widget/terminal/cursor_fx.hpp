#pragma once
// mayag::widget terminal/cursor_fx — the cursor's movement and the little shows around it
//
//   movement   none · glide (eased) · spring (overshoots, settles) · smear (the
//              leading edge jumps, the tail stretches after it)
//   effects    ripple · sonicboom · wireframe · pixiedust · railgun · torpedo ·
//              fireworks (on Enter) -- a tiny particle system
//   trails     comet (fading ghosts) · smear (tapered copies)
//
// Pure model (State) + physics (emit / step / move_*) + drawing (draw /
// comet / smear: absolute nodes, laid over a terminal_view in its own
// coordinates). The app owns a State, steps it on frame ticks and emits on
// typed cursor moves.

#include "../control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mayag::dsl {

/// How the drawn cursor travels between cells.
enum class CursorMotion : std::uint8_t { none, glide, spring, smear };
/// A little show at the cursor as you type.
enum class CursorFx : std::uint8_t { none, ripple, sonicboom, wireframe, pixiedust, railgun, torpedo, fireworks };

namespace cursor_fx {

using CursorAnim = CursorMotion;

struct Particle {
    float x = 0, y = 0, vx = 0, vy = 0, g = 0;   ///< px, px/s, px/s²
    float life = 0, max = 1;                     ///< s
    float size = 2, w = 0, h = 0;
    std::uint8_t kind = 0;                       ///< 0 dot, 1 ring, 2 disc, 3 frame
    std::uint32_t rgb = 0xFFFFFF;
};

struct State {
    std::vector<Particle> ps;
    std::uint32_t seed = 0x9E3779B9u;
    int  last_col = -1, last_row = -1;
    double typed_at = -10.0, enter_at = -10.0;   ///< app clock
    float vx = 0.0f, vy = 0.0f;                  ///< spring velocity, cells/s
    float tx = 0.0f, ty = 0.0f;                  ///< the tail: lags the head, cells
    bool  tail_known = false;
    [[nodiscard]] bool busy() const noexcept { return !ps.empty(); }
};

inline bool cursor_settled(const State& s, CursorAnim anim, float x, float y, float tx, float ty);
inline bool tail_settled(const State& s, float hx, float hy, float cw, float ch);

namespace detail {

constexpr std::size_t kMaxParticles = 500;

inline float rnd(State& s, float lo, float hi) {
    s.seed = s.seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * static_cast<float>(s.seed >> 8) / static_cast<float>(1u << 24);
}

template <class Pal>
std::uint32_t pick_colour(State& s, const Pal& p) {
    static constexpr int slots[] = {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14};
    return p.ansi[slots[static_cast<int>(rnd(s, 0.0f, 11.999f))]];
}

inline void push(State& s, const Particle& p) {
    if (s.ps.size() >= kMaxParticles) s.ps.erase(s.ps.begin());
    s.ps.push_back(p);
}

inline Color<Srgb> col(std::uint32_t v, float a) {
    return rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)).fade(a);
}

}  // namespace detail

/// The cursor moved after typing: emit `fx` at the new cell's centre (cx,cy px).
/// `pal` is any palette with `.cursor` and `.ansi[16]` (0xRRGGBB).
template <class Pal>
void emit(State& s, CursorFx fx, float cx, float cy, float dirx, float diry, float cw, float ch, bool enter,
          const Pal& pal) {
    const std::uint32_t cur = pal.cursor;
    switch (fx) {
        case CursorFx::none: break;
        case CursorFx::ripple:
            detail::push(s, {.x = cx, .y = cy, .life = 0.45f, .max = 0.45f, .size = ch * 1.5f, .kind = 1, .rgb = cur});
            break;
        case CursorFx::sonicboom:
            detail::push(s, {.x = cx, .y = cy, .life = 0.38f, .max = 0.38f, .size = ch * 2.0f, .kind = 2, .rgb = cur});
            break;
        case CursorFx::wireframe:
            detail::push(s, {.x = cx, .y = cy, .life = 0.35f, .max = 0.35f, .w = cw, .h = ch, .kind = 3, .rgb = cur});
            break;
        case CursorFx::pixiedust:
            for (int i = 0; i < 9; ++i) {
                const float life = detail::rnd(s, 0.45f, 0.9f);
                detail::push(s, {.x = cx + detail::rnd(s, -cw * 0.5f, cw * 0.5f), .y = cy + detail::rnd(s, -ch * 0.4f, ch * 0.4f),
                         .vx = detail::rnd(s, -45.0f, 45.0f), .vy = detail::rnd(s, -110.0f, -25.0f), .g = 170.0f,
                         .life = life, .max = life, .size = detail::rnd(s, 1.8f, 3.6f), .kind = 0, .rgb = detail::pick_colour(s, pal)});
            }
            break;
        case CursorFx::railgun: {
            const float len = std::max(std::hypot(dirx, diry), 0.001f);
            const float ux = -dirx / len, uy = -diry / len;   // shoot out the back
            for (int i = 0; i < 14; ++i) {
                const float sp = detail::rnd(s, 180.0f, 520.0f), side = detail::rnd(s, -70.0f, 70.0f);
                const float life = detail::rnd(s, 0.18f, 0.42f);
                detail::push(s, {.x = cx, .y = cy, .vx = ux * sp - uy * side, .vy = uy * sp + ux * side,
                         .life = life, .max = life, .size = detail::rnd(s, 1.5f, 2.6f), .kind = 0, .rgb = cur});
            }
            break;
        }
        case CursorFx::torpedo:
            for (int i = 0; i < 5; ++i)
                detail::push(s, {.x = cx, .y = cy, .vx = detail::rnd(s, -30.0f, 30.0f), .vy = detail::rnd(s, -30.0f, 30.0f),
                         .life = 0.3f, .max = 0.3f, .size = detail::rnd(s, 2.0f, 4.0f), .kind = 0, .rgb = cur});
            break;
        case CursorFx::fireworks:
            if (!enter) {   // typing: a spark; Enter: the show
                detail::push(s, {.x = cx, .y = cy, .vx = detail::rnd(s, -20.0f, 20.0f), .vy = detail::rnd(s, -60.0f, -20.0f), .g = 120.0f,
                         .life = 0.35f, .max = 0.35f, .size = 2.0f, .kind = 0, .rgb = detail::pick_colour(s, pal)});
                break;
            }
            for (int burst = 0; burst < 3; ++burst) {
                const float bx = cx + detail::rnd(s, 0.0f, cw * 14.0f), by = cy - detail::rnd(s, ch * 1.0f, ch * 4.0f);
                const std::uint32_t c = detail::pick_colour(s, pal);
                for (int i = 0; i < 22; ++i) {
                    const float a = 6.2831853f * static_cast<float>(i) / 22.0f + detail::rnd(s, -0.1f, 0.1f);
                    const float sp = detail::rnd(s, 90.0f, 210.0f), life = detail::rnd(s, 0.6f, 1.05f);
                    detail::push(s, {.x = bx, .y = by, .vx = std::cos(a) * sp, .vy = std::sin(a) * sp, .g = 210.0f,
                             .life = life, .max = life, .size = detail::rnd(s, 1.8f, 3.0f), .kind = 0, .rgb = c});
                }
            }
            break;
    }
}

/// While the drawn cursor travels (torpedo): a puff where it is now.
template <class Pal>
void travel(State& s, CursorFx fx, float cx, float cy, float ch, const Pal& pal) {
    if (fx != CursorFx::torpedo) return;
    for (int i = 0; i < 2; ++i)
        detail::push(s, {.x = cx + detail::rnd(s, -2.0f, 2.0f), .y = cy + detail::rnd(s, -ch * 0.3f, ch * 0.3f),
                 .vx = detail::rnd(s, -12.0f, 12.0f), .vy = detail::rnd(s, -12.0f, 12.0f),
                 .life = 0.4f, .max = 0.4f, .size = detail::rnd(s, 3.5f, 6.0f), .kind = 0,
                 .rgb = detail::rnd(s, 0.0f, 1.0f) < 0.5f ? pal.cursor : pal.ansi[4]});
}

inline void step(State& s, float dt) {
    for (auto& p : s.ps) {
        p.vy += p.g * dt;
        p.vx *= std::exp(-dt * 1.5f);   // a touch of drag
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life -= dt;
    }
    std::erase_if(s.ps, [](const Particle& p) { return p.life <= 0.0f; });
}

inline std::vector<Node> draw(const State& s) {
    std::vector<Node> out;
    out.reserve(s.ps.size());
    for (const auto& p : s.ps) {
        const float a = std::clamp(p.life / p.max, 0.0f, 1.0f);
        const float e = 1.0f - a;                                   // 0 -> 1 over its life
        const float out3 = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);   // ease-out
        switch (p.kind) {
            case 1: {   // ring
                const float r = std::max(p.size * out3, 1.0f);
                out.push_back((box() | size(2 * r, 2 * r) | radius(r) | border(1.5f, detail::col(p.rgb, 0.9f * a))
                               | absolute(p.x - r, p.y - r)).build());
                break;
            }
            case 2: {   // disc
                const float r = std::max(p.size * out3, 1.0f);
                out.push_back((box() | size(2 * r, 2 * r) | radius(r) | bg(detail::col(p.rgb, 0.32f * a))
                               | absolute(p.x - r, p.y - r)).build());
                break;
            }
            case 3: {   // frame
                const float k = 1.0f + 1.7f * out3;
                const float w = p.w * k, h = p.h * k;
                out.push_back((box() | size(w, h) | border(1.2f, detail::col(p.rgb, a)) | absolute(p.x - w * 0.5f, p.y - h * 0.5f)).build());
                break;
            }
            default: {   // dot
                const float d = std::max(p.size * (0.35f + 0.65f * a), 0.8f);
                out.push_back((box() | size(d, d) | radius(d * 0.5f) | bg(detail::col(p.rgb, a)) | absolute(p.x - d * 0.5f, p.y - d * 0.5f)).build());
            }
        }
    }
    return out;
}

inline std::vector<Node> smear(float lx, float ly, float tx, float ty, float cw, float ch, std::uint32_t rgb) {
    std::vector<Node> out;
    const float dist = std::hypot(tx - lx, ty - ly);
    if (dist < 1.0f) return out;
    const int n = std::clamp(static_cast<int>(dist / (std::min(cw, ch) * 0.35f)), 2, 48);
    for (int k = n; k >= 1; --k) {   // from the tail (thin, faint) to just behind the head
        const float f = static_cast<float>(k) / static_cast<float>(n);
        const float x = tx + (lx - tx) * f, y = ty + (ly - ty) * f;
        const float sh = 1.0f - 0.55f * f;
        const float w = cw * sh, h = ch * sh;
        out.push_back((box() | size(w, h) | radius(1.0f) | absolute(x + (cw - w) * 0.5f, y + (ch - h) * 0.5f)
                       | bg(detail::col(rgb, 0.85f * (1.0f - f) + 0.08f))).build());
    }
    return out;
}

inline void move_cursor(State& s, CursorAnim anim, float& x, float& y, float tx, float ty, float dt) {
    switch (anim) {
        case CursorAnim::none: x = tx; y = ty; s.vx = s.vy = 0.0f; return;
        case CursorAnim::glide: case CursorAnim::smear: {
            const float k = 1.0f - std::exp(-dt * (anim == CursorAnim::smear ? 20.0f : 26.0f));
            x += (tx - x) * k;
            y += (ty - y) * k;
            break;
        }
        case CursorAnim::spring: {   // underdamped: overshoots, then settles (ζ ≈ 0.45)
            constexpr float K = 560.0f, C = 21.0f;
            for (float left = dt; left > 0.0f; left -= 1.0f / 240.0f) {
                const float h = std::min(left, 1.0f / 240.0f);
                s.vx += (K * (tx - x) - C * s.vx) * h;
                s.vy += (K * (ty - y) - C * s.vy) * h;
                x += s.vx * h;
                y += s.vy * h;
            }
            break;
        }
    }
    if (cursor_settled(s, anim, x, y, tx, ty)) { x = tx; y = ty; s.vx = s.vy = 0.0f; }
}

inline void move_tail(State& s, float hx, float hy, float length, float dt, float cw = 0.0f, float ch = 0.0f) {
    if (!s.tail_known || length <= 0.0f) { s.tx = hx; s.ty = hy; s.tail_known = true; return; }
    // the longer the tail, the slower its end follows: 1x trails ~120 ms, 4x ~450 ms
    const float rate = 26.0f / (0.25f + 0.75f * length);
    const float k = 1.0f - std::exp(-dt * rate);
    s.tx += (hx - s.tx) * k;
    s.ty += (hy - s.ty) * k;
    if (tail_settled(s, hx, hy, cw, ch)) { s.tx = hx; s.ty = hy; }
}

/// Settled once the tail is closer than comet/smear DRAW (under 1 px): in
/// cells that is 1/cw, not a fixed 0.02 -- at 20 px cells 0.02 is 0.4 px, and
/// the tail kept frames coming for an invisible 0.6 px of travel. Without the
/// cell size (cw = 0) the old fixed fraction stands.
inline bool tail_settled(const State& s, float hx, float hy, float cw = 0.0f, float ch = 0.0f) {
    const float ex = cw > 0.0f ? 1.0f / cw : 0.02f, ey = ch > 0.0f ? 1.0f / ch : 0.02f;
    return std::fabs(hx - s.tx) < ex && std::fabs(hy - s.ty) < ey;
}

inline std::vector<Node> comet(float hx, float hy, float tx, float ty, float cw, float ch, std::uint32_t rgb, float strength) {
    std::vector<Node> out;
    const float dist = std::hypot(tx - hx, ty - hy);
    if (dist < 1.5f) return out;
    const int n = std::clamp(static_cast<int>(dist / (std::min(cw, ch) * 0.45f)), 3, 64);
    for (int k = n; k >= 1; --k) {   // from the tail end (small, faint) to just behind the head
        const float f = static_cast<float>(k) / static_cast<float>(n);
        const float x = hx + (tx - hx) * f, y = hy + (ty - hy) * f;
        const float sh = 0.92f - 0.62f * f;
        const float w = cw * sh, h = ch * sh;
        const float a = std::clamp(strength, 0.2f, 1.0f) * (0.55f * (1.0f - f) * (1.0f - f) + 0.04f);
        out.push_back((box() | size(w, h) | radius(std::min(w, h) * 0.3f) | absolute(x + (cw - w) * 0.5f, y + (ch - h) * 0.5f)
                       | bg(detail::col(rgb, a))).build());
    }
    return out;
}

inline bool cursor_settled(const State& s, CursorAnim anim, float x, float y, float tx, float ty) {
    const bool near = std::fabs(tx - x) < 0.01f && std::fabs(ty - y) < 0.01f;
    return anim == CursorAnim::spring ? near && std::fabs(s.vx) < 0.05f && std::fabs(s.vy) < 0.05f : near;
}

}  // namespace cursor_fx
}  // namespace mayag::dsl
