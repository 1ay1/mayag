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
#include "kernel.hpp"   // KIND_* / FLAG_*: the one definition shader and CPU share

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace mayag {

/// Pixels an app hands to the renderer: photos, icons, decoded video frames.
///
/// Deliberately OUTSIDE Cmd<Msg>. Nested inside, every Msg type would get its
/// own incompatible ImageData, and lifting a child component's upload into a
/// parent would need a cast between two identical layouts. Images do not
/// depend on the message type, so they should not be parameterised by it.
///
/// Held by shared_ptr and moved by POINTER, never copied. A 1080p frame is
/// 8 MB; a player producing 60 a second would otherwise burn half a gigabyte
/// per second copying frames into messages that are themselves copied through
/// the queue. Sharing also lets a decoder hand over a buffer it already owns.
/// A texture the renderer can sample, whatever produced it.
///
/// Lives here rather than with Buffer because it is a TEXTURE identity, not a
/// buffer one: `DrawList` indexes textures by id and this is that id, so an
/// imported buffer and an uploaded image are the same currency at draw time.
/// Opaque so the backend can decide whether the slot means a VkImage, an
/// MTLTexture, or an index into a software array.
struct TextureHandle {
    std::uint32_t slot = 0;
    [[nodiscard]] bool valid() const noexcept { return slot != 0; }
    [[nodiscard]] friend constexpr bool operator==(TextureHandle,
                                                   TextureHandle) = default;
};

/// Decoded pixels the renderer can upload.
///
/// `rgba` is straight (not premultiplied), sRGB-encoded RGBA8 -- what every
/// decoder and image loader produces. The shading kernel decodes it.
struct ImageData {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};


/// What the shader should compute for this instance. Kept in the instance data
/// rather than in pipeline state, so different shapes still batch together.
enum class ShapeKind : std::uint32_t {
    rounded_box   = kernel::KIND_ROUNDED_BOX,
    circle        = kernel::KIND_CIRCLE,
    ring          = kernel::KIND_RING,   ///< annulus; `param0` is the ring width
    line          = kernel::KIND_LINE,   ///< capsule between two points
    glyph         = kernel::KIND_GLYPH,   ///< samples the glyph atlas as coverage
    texture       = kernel::KIND_TEXTURE,   ///< samples an image
    shadow        = kernel::KIND_SHADOW,   ///< blurred box, analytic
    arc           = kernel::KIND_ARC,   ///< partial ring; param0/param1 are start/sweep radians
    color_glyph   = kernel::KIND_COLOR_GLYPH,   ///< samples an RGBA colour-glyph atlas (emoji), untinted
    backdrop      = kernel::KIND_BACKDROP,   ///< blurs the pixels already behind it (frosted glass)
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
    //   backdrop    : (blur_radius, saturation, brightness, _)
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
inline constexpr std::uint32_t gradient          = kernel::FLAG_GRADIENT;
inline constexpr std::uint32_t gradient_radial   = kernel::FLAG_RADIAL;
inline constexpr std::uint32_t gradient_angular  = kernel::FLAG_ANGULAR;
inline constexpr std::uint32_t gradient_srgb     = kernel::FLAG_SRGB;
inline constexpr std::uint32_t stroke_only       = kernel::FLAG_STROKE;   ///< hollow: border, no fill
inline constexpr std::uint32_t inset             = kernel::FLAG_INSET;   ///< inner shadow
inline constexpr std::uint32_t dashed            = kernel::FLAG_DASHED;
/// This glyph's atlas entry is a distance field, not coverage.
inline constexpr std::uint32_t glyph_sdf         = kernel::FLAG_GLYPH_SDF;
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
        text_backdrop_ = Vec4{};
        backdrop_since_ = 0;
        covers_.clear();
        cover_bound_ = Rect{};
    }

    void reserve(std::size_t n) {
        instances_.reserve(n);
        batches_.reserve(n / 8 + 4);
    }

    [[nodiscard]] const std::vector<Instance>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<Batch>& batches() const noexcept { return batches_; }
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }
    [[nodiscard]] bool empty() const noexcept { return instances_.empty(); }

    /// Byte-identical instances and batches: the frame would draw the same
    /// pixels (given the same textures). Instances and Batch are plain data.
    [[nodiscard]] bool same_as(const DrawList& o) const noexcept {
        static_assert(std::is_trivially_copyable_v<Instance>);
        if (instances_.size() != o.instances_.size() || batches_.size() != o.batches_.size()) return false;
        // Instances are value-initialised (padding zeroed), so bytes compare.
        // (Empty: data() may be null, and memcmp on null is UB even for 0 bytes.)
        if (!instances_.empty() &&
            std::memcmp(instances_.data(), o.instances_.data(), instances_.size() * sizeof(Instance)) != 0)
            return false;
        for (std::size_t i = 0; i < batches_.size(); ++i) {
            const Batch& a = batches_[i];
            const Batch& b = o.batches_[i];
            if (a.first != b.first || a.count != b.count || a.texture != b.texture || a.blend != b.blend ||
                a.clip.origin.x != b.clip.origin.x || a.clip.origin.y != b.clip.origin.y ||
                a.clip.size.x != b.clip.size.x || a.clip.size.y != b.clip.size.y)
                return false;
        }
        return true;
    }
    /// Copy `o`'s commands into this list, reusing its storage.
    void assign_from(const DrawList& o) {
        instances_.assign(o.instances_.begin(), o.instances_.end());
        batches_.assign(o.batches_.begin(), o.batches_.end());
    }

    /// The instances pushed since `first`, with the texture and blend each
    /// was pushed with (a recording a PaintCache can replay).
    struct Recorded { Instance inst; Rect clip; std::uint32_t texture; BlendMode blend; bool outer = false; };
    void record_since(std::size_t first, std::vector<Recorded>& out) const {
        out.clear();
        for (const Batch& b : batches_) {
            const std::size_t hi = static_cast<std::size_t>(b.first) + b.count;
            for (std::size_t i = std::max<std::size_t>(b.first, first); i < hi; ++i)
                out.push_back({instances_[i], b.clip, b.texture, b.blend});
        }
    }
    /// Push a recording back: the same instances under the same clips,
    /// batched as push() would. The clip state afterwards is as before.
    void replay(const std::vector<Recorded>& rec) {
        const Rect saved = current_clip_;
        for (const auto& r : rec) { current_clip_ = r.clip; push(r.inst, r.texture, r.blend); }
        current_clip_ = saved;
    }

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

    // ── text backdrop ───────────────────────────────────────────────────
    //
    // The opaque colour text is drawn on, when the painter knows it. GPU
    // backends without framebuffer fetch blend glyphs in linear light and
    // reshape coverage to imitate the encoded-space blend
    // (kernel::apply_text_gamma); against the real backdrop that is exact,
    // against the black/white design backdrops only close. Glyphs carry it
    // in their otherwise unused color2 (w = 1 known, 0 unknown).
    void set_text_backdrop(Color<Srgb> c) noexcept {
        const auto p = premultiplied(c);
        text_backdrop_ = c.a >= 0.999f ? Vec4{p.x, p.y, p.z, 1.0f} : Vec4{};
        // The fill that set it is the instance just pushed; anything drawn
        // after it that overlaps a glyph invalidates it for that glyph.
        backdrop_since_ = instances_.size();
        covers_.clear();
        cover_bound_ = Rect{};
    }
    void clear_text_backdrop() noexcept { text_backdrop_ = Vec4{}; }
    struct BackdropState { Vec4 colour; std::size_t since; };
    [[nodiscard]] BackdropState text_backdrop() const noexcept { return {text_backdrop_, backdrop_since_}; }
    void restore_text_backdrop(BackdropState s) noexcept {
        text_backdrop_ = s.colour;
        if (s.since != backdrop_since_) {   // a different window: rebuild its covers from the list
            backdrop_since_ = s.since;
            covers_.clear();
            cover_bound_ = Rect{};
            for (std::size_t k = backdrop_since_; k < instances_.size(); ++k) note_cover(instances_[k]);
        }
    }

    // ── emission ────────────────────────────────────────────────────────

    void push(const Instance& inst, std::uint32_t texture = 0,
              BlendMode blend = BlendMode::normal) {
        // Merge into the tail batch when render state matches; otherwise open
        // a new one. This is the entire batching algorithm.
        //
        // Backdrops are the exception: a frosted-glass panel must be drawn in
        // its own batch, because a GPU backend renders it in a SEPARATE pass
        // after snapshotting the framebuffer behind it. So a backdrop never
        // merges with a neighbour, and nothing merges onto a backdrop batch.
        const bool is_backdrop =
            static_cast<ShapeKind>(inst.kind) == ShapeKind::backdrop;
        if (!is_backdrop && !batches_.empty()) {
            Batch& tail = batches_.back();
            const bool tail_is_backdrop = tail.count > 0 &&
                static_cast<ShapeKind>(instances_[tail.first].kind) == ShapeKind::backdrop;
            if (!tail_is_backdrop && tail.texture == texture &&
                tail.blend == blend && tail.clip == current_clip_) {
                note_cover(inst);
                instances_.push_back(inst);
                ++tail.count;
                return;
            }
        }
        batches_.push_back(Batch{static_cast<std::uint32_t>(instances_.size()), 1,
                                 current_clip_, texture, blend});
        note_cover(inst);
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
        i.color  = premultiplied(sh.color);
        if (sh.inset) {
            // Inner: the quad IS the shape (it is clipped to it); the
            // shadow box's offset and spread travel in params.
            i.rect   = {r.origin.x, r.origin.y, r.size.x, r.size.y};
            i.radii  = radii.clamp_to(r.size).as_vec4();
            i.params = {num::max(sh.blur, 0.01f), sh.spread, sh.offset.x, sh.offset.y};
            i.flags |= instance_flags::inset;
            push(i);
            return;
        }
        const Rect b = r.translate(sh.offset).expand(sh.spread);
        i.rect   = {b.origin.x, b.origin.y, b.size.x, b.size.y};
        i.radii  = radii.clamp_to(b.size).as_vec4();
        i.params = {num::max(sh.blur, 0.01f), sh.spread, 0.0f, 0.0f};
        push(i);
    }

    /// Frosted glass. Blurs the pixels ALREADY drawn behind `r` by `blur`
    /// pixels, optionally shifting saturation and brightness, clipped to the
    /// rounded rect. Unlike every other primitive this reads the framebuffer,
    /// so it must be emitted BEFORE the fill that sits on top of it — the
    /// painter does that. `radii` rounds the glass to match the panel.
    void backdrop(const Rect& r, float blur, float saturation, float brightness,
                  Corners radii = {}) {
        Instance i{};
        i.kind   = static_cast<std::uint32_t>(ShapeKind::backdrop);
        i.rect   = {r.origin.x, r.origin.y, r.size.x, r.size.y};
        i.radii  = radii.clamp_to(r.size).as_vec4();
        i.color  = {0.0f, 0.0f, 0.0f, 1.0f};
        i.params = {num::max(blur, 0.0f), saturation, brightness, 0.0f};
        push(i);
    }

    /// The glyph atlas occupies a RESERVED, permanently-bound texture slot.
    ///
    /// That is what lets text batch with everything else. If glyphs were
    /// treated like an ordinary texture, a UI that alternates labels and
    /// boxes (which is every UI) would break the batch on every switch and
    /// turn one draw call into dozens. Instead the atlas is always bound to
    /// its own sampler, the shader reads it when `kind == glyph`, and the
    /// batch key never changes.
    static constexpr std::uint32_t atlas_slot = 0xFFFF'FFFFu;

    /// The colour-glyph atlas rides in a distinct slot so the sampler knows to
    /// return RGBA rather than coverage.
    static constexpr std::uint32_t color_atlas_slot = 0xFFFF'FFFEu;

    /// `sdf` selects how the sampler decodes the atlas texel: a bitmap entry
    /// IS coverage and must be used as-is, while a distance-field entry has
    /// to be thresholded back into coverage.
    ///
    /// This has to travel per INSTANCE, not per batch or per font: in hybrid
    /// mode a single frame mixes both kinds (small text rasterised per size,
    /// large text sharing one scale-free field), and decoding a bitmap entry
    /// as if it were a distance field snaps every partial pixel to 0 or 1 —
    /// which throws away the antialiasing and makes small text look eroded
    /// and ragged. That was a real bug.
    void glyph(const Rect& dst, const Rect& atlas_uv, Color<Srgb> c, bool sdf = false,
               float sdf_edge = 0.0f, float sdf_grow = 0.0f) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::glyph);
        i.rect  = {dst.origin.x, dst.origin.y, dst.size.x, dst.size.y};
        i.uv    = {atlas_uv.left(), atlas_uv.top(), atlas_uv.right(), atlas_uv.bottom()};
        i.color = premultiplied(c);
        i.color2 = backdrop_for(dst);
        i.texture_slot = atlas_slot;
        // params.x carries the SDF antialiasing half-band in normalised field
        // units. 0 => the renderer falls back to its constant default. The
        // caller sets it so the band tracks ~1 display pixel no matter how far
        // the glyph is scaled from its raster size (otherwise magnified SDF
        // text blurs and shrunk text aliases). params.y moves the threshold
        // outward by that many field units: font smoothing grows the glyph
        // by a fixed number of DISPLAY pixels, and one shared field serves
        // every size, so the growth can only be applied per draw.
        // params.zw: the atlas uv step of one display pixel (x, y), so the
        // kernel can sample the field's gradient one pixel away.
        const float uv_dx = dst.size.x > 0.0f ? atlas_uv.width() / dst.size.x : 0.0f;
        const float uv_dy = dst.size.y > 0.0f ? atlas_uv.height() / dst.size.y : 0.0f;
        i.params = {sdf_edge, sdf_grow, sdf ? uv_dx : 0.0f, sdf ? uv_dy : 0.0f};
        if (sdf) i.flags |= instance_flags::glyph_sdf;
        // Texture 0 for batching purposes: the atlas is already bound.
        push(i, 0);
    }

    /// A glyph whose coverage came from the OS rasteriser (DirectWrite):
    /// composited with the OS's own grayscale alpha correction
    /// (kernel::dwrite_text_alpha) so it matches native text exactly.
    /// `gamma_ratios` and `contrast` are the user's DirectWrite parameters.
    void glyph_native(const Rect& dst, const Rect& atlas_uv, Color<Srgb> c, Vec4 gamma_ratios, float contrast) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::glyph);
        i.rect  = {dst.origin.x, dst.origin.y, dst.size.x, dst.size.y};
        i.uv    = {atlas_uv.left(), atlas_uv.top(), atlas_uv.right(), atlas_uv.bottom()};
        i.color = premultiplied(c);
        i.color2 = backdrop_for(dst);
        i.texture_slot = atlas_slot;
        i.params = gamma_ratios;
        i.axis = {contrast, 0.0f, 0.0f, 0.0f};
        i.flags |= kernel::FLAG_GLYPH_DWRITE;
        push(i, 0);
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

    /// Draw a buffer imported with `Device::import`.
    ///
    /// A compositor needs this for the path that must ALWAYS work. A layer
    /// that wins a hardware plane never reaches the GPU at all, but planes
    /// are few and most layers lose: they have to be composited, and
    /// compositing an imported buffer was impossible without this. Import
    /// returns a TextureHandle, DrawList indexes textures by id, and nothing
    /// joined the two -- so zero-copy import could feed direct scanout and
    /// nothing else, which is exactly backwards. The fallback is the case
    /// that must never be missing.
    ///
    /// A TextureHandle's slot IS a texture id; they were always the same
    /// currency. Taking the handle rather than the raw slot keeps that an
    /// implementation detail and stops a caller inventing an id by hand.
    void imported(const Rect& dst, TextureHandle handle,
                  const Rect& src_uv = Rect{{0, 0}, {1, 1}},
                  Color<Srgb> tint = colors::white) {
        if (!handle.valid()) return;
        textured(dst, src_uv, handle.slot, tint);
    }

    /// A colour glyph (emoji). Unlike `glyph`, which samples a coverage atlas
    /// and is tinted by the text colour, this samples an RGBA colour-glyph
    /// atlas and takes its pixels VERBATIM — a 😀 is yellow no matter the
    /// text colour. `opacity` still multiplies through, so it fades with the
    /// run's opacity like everything else.
    void color_glyph(const Rect& dst, const Rect& atlas_uv, float opacity = 1.0f) {
        Instance i{};
        i.kind  = static_cast<std::uint32_t>(ShapeKind::color_glyph);
        i.rect  = {dst.origin.x, dst.origin.y, dst.size.x, dst.size.y};
        i.uv    = {atlas_uv.left(), atlas_uv.top(), atlas_uv.right(), atlas_uv.bottom()};
        // color.w carries opacity; rgb is unused (the atlas supplies colour).
        i.color = {1.0f, 1.0f, 1.0f, num::saturate(opacity)};
        i.texture_slot = color_atlas_slot;
        push(i, 0);
    }

  private:
    /// Convert to linear premultiplied — the only correct form to blend in.
    /// Doing this once on the CPU means the shader never has to, and means an
    /// sRGB value can never reach the blender un-linearised.
    [[nodiscard]] Vec4 premultiplied(Color<Srgb> c) noexcept {
        // Every glyph of a text run converts the same colour; skip the three
        // pow()s when it repeats.
        if (c == last_srgb_) return last_premul_;
        const auto l = c.to<Linear>();
        last_srgb_ = c;
        last_premul_ = {l.c0 * l.a, l.c1 * l.a, l.c2 * l.a, l.a};
        return last_premul_;
    }

    std::vector<Instance> instances_;
    Color<Srgb>           last_srgb_{0.0f, 0.0f, 0.0f, 0.0f};
    Vec4                  last_premul_{0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<Batch>    batches_;
    std::vector<Rect>     clip_stack_;
    Rect                  current_clip_{Vec2{}, Vec2{num::inf, num::inf}};
    Vec4                  text_backdrop_{};   // see set_text_backdrop
    std::size_t           backdrop_since_ = 0;
    // The non-glyph rects pushed since backdrop_since_ (what can hide the
    // backdrop from a glyph), and their bounding box. backdrop_for asks these
    // instead of rescanning every instance since the backdrop for every glyph:
    // that scan was O(glyphs x instances), the single largest cost of
    // painting a screen of text (a terminal under a flood: 8 ms a frame).
    std::vector<Rect>     covers_;
    Rect                  cover_bound_{};

    void note_cover(const Instance& o) {
        if (o.kind == static_cast<std::uint32_t>(ShapeKind::glyph)) return;
        const Rect r{o.rect.x, o.rect.y, o.rect.z, o.rect.w};
        cover_bound_ = covers_.empty() ? r : cover_bound_.unite(r);
        covers_.push_back(r);
    }

    /// The backdrop under a glyph at `dst`, or unknown if any non-glyph
    /// instance drawn after the backdrop's fill overlaps it (a sibling card,
    /// an overlay in a z-stack). Glyph-on-glyph overlap is ignored: text
    /// never sits on other text's ink.
    [[nodiscard]] Vec4 backdrop_for(const Rect& dst) const noexcept {
        if (text_backdrop_.w < 0.5f) return Vec4{};
        if (covers_.empty() || !cover_bound_.intersects(dst)) return text_backdrop_;   // the common case: O(1)
        for (const Rect& r : covers_)
            if (r.intersects(dst)) return Vec4{};
        return text_backdrop_;
    }
};

}  // namespace mayag
