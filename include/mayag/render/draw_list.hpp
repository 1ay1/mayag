#pragma once
// mayag::DrawList — the GPU command buffer
//
// The whole renderer rests on one idea: **every UI primitive is a rounded-box
// signed distance field**. A button, a card, a divider, a circle, a pill, a
// ring, a shadow, even a glyph quad — all the same instance struct, all the
// same shader, all one draw call.
//
// That is why mayag has no path rasteriser, no tessellator, no stencil passes.
// A frame is: build an array of `Instance`, upload, draw N quads. The CPU work
// per widget is a struct write.
//
//     struct Instance { rect, radii, colors, params }   // 96 bytes
//
// Batching rule: consecutive instances sharing a clip rect and texture merge
// into one draw. A typical app frame is 1-3 draws.

#include "../core/color.hpp"
#include "../core/geometry.hpp"
#include "../style/style.hpp"

#include <cstdint>
#include <vector>

namespace mayag {

/// What the shader should compute for this instance. Kept in the instance data
/// rather than in pipeline state, so different shapes still batch together.
enum class ShapeKind : std::uint32_t {
    rounded_box   = 0,
    circle        = 1,
    ring          = 2,   ///< annulus; `param0` is the ring width
    line          = 3,   ///< capsule between two points
    glyph         = 4,   ///< samples the glyph atlas as coverage
    texture       = 5,   ///< samples an image
    shadow        = 6,   ///< blurred box, analytic
    arc           = 7,   ///< partial ring; param0/param1 are start/sweep radians
};

/// One GPU instance. Layout is deliberately 16-byte-aligned vec4 groups so the
/// same struct maps 1:1 onto an HLSL/MSL/GLSL/WGSL instance buffer with no
/// padding surprises across backends.
struct alignas(16) Instance {
    // vec4 0: destination rect (x, y, w, h) in physical pixels
    Vec4 rect{};
    // vec4 1: corner radii (tl, tr, br, bl)
    Vec4 radii{};
    // vec4 2: primary colour, linear premultiplied
    Vec4 color{};
    // vec4 3: secondary colour (gradient end / border colour)
    Vec4 color2{};
    // vec4 4: gradient axis (from.xy, to.xy) in local normalised coords
    Vec4 axis{};
    // vec4 5: uv rect for atlas sampling (u0, v0, u1, v1)
    Vec4 uv{};
    // vec4 6: shape params — meaning depends on `kind`
    //   rounded_box : (border_width, softness, _, _)
    //   ring        : (thickness, _, _, _)
    //   shadow      : (blur, spread, _, _)
    //   line        : (thickness, _, _, _)
    //   arc         : (thickness, start_rad, sweep_rad, _)
    Vec4 params{};
    // vec4 7: (kind, flags, blend, texture_slot)
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    std::uint32_t blend = 0;
    std::uint32_t texture_slot = 0;
};

static_assert(sizeof(Instance) == 128, "Instance must stay a tidy multiple of 16 bytes");
static_assert(std::is_trivially_copyable_v<Instance>);

namespace instance_flags {
inline constexpr std::uint32_t none              = 0;
inline constexpr std::uint32_t gradient          = 1u << 0;
inline constexpr std::uint32_t gradient_radial   = 1u << 1;
inline constexpr std::uint32_t gradient_angular  = 1u << 2;
inline constexpr std::uint32_t gradient_srgb     = 1u << 3;
inline constexpr std::uint32_t stroke_only       = 1u << 4;   ///< hollow: border, no fill
inline constexpr std::uint32_t inset             = 1u << 5;   ///< inner shadow
inline constexpr std::uint32_t dashed            = 1u << 6;
}  // namespace instance_flags

/// A contiguous run of instances sharing render state.
struct Batch {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    Rect          clip{};
    std::uint32_t texture = 0;
    BlendMode     blend = BlendMode::normal;
};

/// The per-frame command buffer. Cleared and refilled each frame — the
/// allocation is amortised, so steady-state frames do zero heap work.
class DrawList {
  public:
    void clear() noexcept {
        instances_.clear();
        batches_.clear();
        clip_stack_.clear();
        current_clip_ = Rect{Vec2{}, Vec2{num::inf, num::inf}};
    }

    void reserve(std::size_t n) {
        instances_.reserve(n);
        batches_.reserve(n / 8 + 4);
    }

    [[nodiscard]] const std::vector<Instance>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<Batch>& batches() const noexcept { return batches_; }
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }
    [[nodiscard]] bool empty() const noexcept { return instances_.empty(); }

    // ── clipping ────────────────────────────────────────────────────────

    /// Push an intersected clip rect. Intersecting (rather than replacing) is
    /// what makes nested scroll views behave.
    void push_clip(const Rect& r) {
        clip_stack_.push_back(current_clip_);
        current_clip_ = current_clip_.intersect(r);
    }

    void pop_clip() {
        if (clip_stack_.empty()) return;
        current_clip_ = clip_stack_.back();
        clip_stack_.pop_back();
    }

    [[nodiscard]] const Rect& clip() const noexcept { return current_clip_; }

    // ── emission ────────────────────────────────────────────────────────

    void push(const Instance& inst, std::uint32_t texture = 0,
              BlendMode blend = BlendMode::normal) {
        // Merge into the tail batch when render state matches; otherwise open
        // a new one. This is the entire batching algorithm.
        if (!batches_.empty()) {
            Batch& tail = batches_.back();
            if (tail.texture == texture && tail.blend == blend && tail.clip == current_clip_) {
                instances_.push_back(inst);
                ++tail.count;
                return;
            }
        }
        batches_.push_back(Batch{static_cast<std::uint32_t>(instances_.size()), 1,
                                 current_clip_, texture, blend});
        instances_.push_back(inst);
    }

    // ── convenience primitives ──────────────────────────────────────────
    //
    // These are what a `canvas()` callback uses, and what the painter builds
    // on. Everything funnels into `push`.

    void fill_rect(const Rect& r, Color<Srgb> c, Corners radii = {}) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::rounded_box);
        i.rect  = {r.origin.x, r.origin.y, r.size.x, r.size.y};
        i.radii = radii.clamp_to(r.size).as_vec4();
        i.color = premultiplied(c);
        i.params = {0.0f, 1.0f, 0.0f, 0.0f};
        push(i);
    }

    void stroke_rect(const Rect& r, float width, Color<Srgb> c, Corners radii = {},
                     StrokeAlign align = StrokeAlign::inside) {
        // Normalise every alignment to an inside stroke on an adjusted rect,
        // so the shader only ever implements one case.
        Rect  rr = r;
        Corners cr = radii;
        if (align == StrokeAlign::outside) {
            rr = r.expand(width);
            cr = Corners{radii.tl + width, radii.tr + width, radii.br + width, radii.bl + width};
        } else if (align == StrokeAlign::center) {
            const float half = width * 0.5f;
            rr = r.expand(half);
            cr = Corners{radii.tl + half, radii.tr + half, radii.br + half, radii.bl + half};
        }

        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::rounded_box);
        i.rect  = {rr.origin.x, rr.origin.y, rr.size.x, rr.size.y};
        i.radii = cr.clamp_to(rr.size).as_vec4();
        i.color = premultiplied(c);
        i.params = {width, 1.0f, 0.0f, 0.0f};
        i.flags = instance_flags::stroke_only;
        push(i);
    }

    void fill_gradient(const Rect& r, const Fill& f, Corners radii = {}) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::rounded_box);
        i.rect  = {r.origin.x, r.origin.y, r.size.x, r.size.y};
        i.radii = radii.clamp_to(r.size).as_vec4();

        const auto first = f.stop_count ? f.stops[0].color : f.color;
        const auto last  = f.stop_count ? f.stops[f.stop_count - 1].color : f.color;
        i.color  = premultiplied(first);
        i.color2 = premultiplied(last);
        i.axis   = {f.from.x, f.from.y, f.to.x, f.to.y};
        i.params = {0.0f, 1.0f, f.radius, 0.0f};

        i.flags = instance_flags::gradient;
        if (f.kind == FillKind::radial_gradient)  i.flags |= instance_flags::gradient_radial;
        if (f.kind == FillKind::angular_gradient) i.flags |= instance_flags::gradient_angular;
        if (f.interpolate_srgb)                   i.flags |= instance_flags::gradient_srgb;
        push(i);
    }

    void circle(Vec2 center, float r, Color<Srgb> c) {
        fill_rect(Rect::centered(center, Vec2{r * 2.0f}), c, Corners{r});
    }

    void ring(Vec2 center, float r, float thickness, Color<Srgb> c) {
        Instance i{};
        i.kind   = static_cast<std::uint32_t>(ShapeKind::ring);
        const Rect b = Rect::centered(center, Vec2{r * 2.0f});
        i.rect   = {b.origin.x, b.origin.y, b.size.x, b.size.y};
        i.radii  = Vec4{r, r, r, r};
        i.color  = premultiplied(c);
        i.params = {thickness, 1.0f, 0.0f, 0.0f};
        push(i);
    }

    void arc(Vec2 center, float r, float thickness, float start_rad, float sweep_rad,
             Color<Srgb> c) {
        Instance i{};
        i.kind   = static_cast<std::uint32_t>(ShapeKind::arc);
        const Rect b = Rect::centered(center, Vec2{r * 2.0f});
        i.rect   = {b.origin.x, b.origin.y, b.size.x, b.size.y};
        i.radii  = Vec4{r, r, r, r};
        i.color  = premultiplied(c);
        i.params = {thickness, start_rad, sweep_rad, 0.0f};
        push(i);
    }

    /// A line is a capsule: a rect rotated to the segment with fully rounded
    /// caps. Encoded as endpoints so the shader can do the exact SDF.
    void line(Vec2 a, Vec2 b, float thickness, Color<Srgb> c) {
        Instance i{};
        i.kind   = static_cast<std::uint32_t>(ShapeKind::line);
        const Rect bounds = Rect::from_bounds(a, b).expand(thickness);
        i.rect   = {bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y};
        i.axis   = {a.x, a.y, b.x, b.y};
        i.color  = premultiplied(c);
        i.params = {thickness, 1.0f, 0.0f, 0.0f};
        push(i);
    }

    void shadow(const Rect& r, const Shadow& sh, Corners radii) {
        Instance i{};
        i.kind   = static_cast<std::uint32_t>(ShapeKind::shadow);
        const Rect b = r.translate(sh.offset).expand(sh.spread);
        i.rect   = {b.origin.x, b.origin.y, b.size.x, b.size.y};
        i.radii  = radii.clamp_to(b.size).as_vec4();
        i.color  = premultiplied(sh.color);
        i.params = {num::max(sh.blur, 0.01f), sh.spread, 0.0f, 0.0f};
        if (sh.inset) i.flags |= instance_flags::inset;
        push(i);
    }

    void glyph(const Rect& dst, const Rect& atlas_uv, Color<Srgb> c, std::uint32_t atlas) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::glyph);
        i.rect  = {dst.origin.x, dst.origin.y, dst.size.x, dst.size.y};
        i.uv    = {atlas_uv.left(), atlas_uv.top(), atlas_uv.right(), atlas_uv.bottom()};
        i.color = premultiplied(c);
        push(i, atlas);
    }

    void textured(const Rect& dst, const Rect& src_uv, std::uint32_t texture,
                  Color<Srgb> tint = colors::white) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::texture);
        i.rect  = {dst.origin.x, dst.origin.y, dst.size.x, dst.size.y};
        i.uv    = {src_uv.left(), src_uv.top(), src_uv.right(), src_uv.bottom()};
        i.color = premultiplied(tint);
        push(i, texture);
    }

  private:
    /// Convert to linear premultiplied — the only correct form to blend in.
    /// Doing this once on the CPU means the shader never has to, and means an
    /// sRGB value can never reach the blender un-linearised.
    [[nodiscard]] static Vec4 premultiplied(Color<Srgb> c) noexcept {
        const auto l = c.to<Linear>();
        return {l.c0 * l.a, l.c1 * l.a, l.c2 * l.a, l.a};
    }

    std::vector<Instance> instances_;
    std::vector<Batch>    batches_;
    std::vector<Rect>     clip_stack_;
    Rect                  current_clip_{Vec2{}, Vec2{num::inf, num::inf}};
};

}  // namespace mayag
