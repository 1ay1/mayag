#pragma once
// mayag::backend::Software — the reference rasteriser
//
// Every mayag backend must produce the same pixels. This one is the standard
// they are measured against: it evaluates the exact same `sdf::` functions the
// shaders do, in linear light, on the CPU.
//
// It exists for three reasons, all load-bearing:
//   1. mayag runs with NO GPU at all — CI, servers, containers, SSH sessions.
//   2. Golden-image tests compare GPU output against this, so a shader bug in
//      one backend is caught mechanically instead of by eye.
//   3. It is the executable specification of the shading model. If the prose
//      and this file disagree, this file is right.
//
// It is scalar and single-threaded by default but tiles trivially; a 1080p
// frame of typical UI runs in a few milliseconds because the per-instance
// bounding box means most pixels are never visited.

#include "../core/color.hpp"
#include "../render/draw_list.hpp"
#include "../render/kernel.hpp"
#include "../render/sdf.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <span>
#include <vector>

namespace mayag::backend {

namespace detail {

/// Linear -> sRGB 8-bit, as a table.
///
/// The exact conversion needs a `pow(x, 1/2.4)` per channel, which is ~2.7
/// MILLION calls for one 1020x880 frame and measured at 70 ms — nine times
/// the cost of actually rendering that frame. Since the output is only 8
/// bits, the whole function has at most 256 distinguishable results, so a
/// 4096-entry table is both exact to the last bit and ~200x faster.
///
/// 4096 entries (12-bit input) keeps the darkest steps accurate, where sRGB's
/// curve is steepest and banding would otherwise be visible.
struct SrgbEncodeTable {
    static constexpr std::size_t size = 4096;
    std::array<std::uint8_t, size> lut{};

    constexpr SrgbEncodeTable() {
        for (std::size_t i = 0; i < size; ++i) {
            const float linear = static_cast<float>(i) / static_cast<float>(size - 1);
            const float encoded = linear <= 0.0031308f
                ? linear * 12.92f
                : 1.055f * num::pow(linear, 1.0f / 2.4f) - 0.055f;
            lut[i] = static_cast<std::uint8_t>(num::saturate(encoded) * 255.0f + 0.5f);
        }
    }

    [[nodiscard]] constexpr std::uint8_t operator()(float linear) const noexcept {
        const float c = num::saturate(linear);
        return lut[static_cast<std::size_t>(c * static_cast<float>(size - 1) + 0.5f)];
    }
};

/// Built once at namespace scope. `constinit` guarantees it is computed at
/// compile time rather than during static initialisation.
constinit inline const SrgbEncodeTable srgb_encode{};

}  // namespace detail

/// A linear-light RGBA framebuffer. Compositing happens here, in linear space,
/// premultiplied — the encode to sRGB is a single final pass.
class Framebuffer {
  public:
    Framebuffer(int w, int h) : width_{w}, height_{h}, pixels_(static_cast<std::size_t>(w) * h) {}

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    void clear(Color<Srgb> c) {
        const auto l = c.to<Linear>();
        const Vec4 pre{l.c0 * l.a, l.c1 * l.a, l.c2 * l.a, l.a};
        for (auto& p : pixels_) p = pre;
    }

    [[nodiscard]] Vec4& at(int x, int y) noexcept {
        return pixels_[static_cast<std::size_t>(y) * width_ + x];
    }
    [[nodiscard]] const Vec4& at(int x, int y) const noexcept {
        return pixels_[static_cast<std::size_t>(y) * width_ + x];
    }

    /// Source-over with premultiplied operands — one multiply-add per channel.
    void blend(int x, int y, Vec4 src_premul, float cov) noexcept {
        if (cov <= 0.0f) return;
        Vec4& dst = at(x, y);
        const Vec4 s = src_premul * cov;
        // Opaque source replaces the destination outright; that is the common
        // case for backgrounds and panels, and it skips three multiplies.
        if (s.w >= 1.0f) { dst = s; return; }
        const float ia = 1.0f - s.w;
        dst = Vec4{s.x + dst.x * ia, s.y + dst.y * ia, s.z + dst.z * ia, s.w + dst.w * ia};
    }

    /// Un-premultiply, encode to sRGB, pack to 8-bit RGBA.
    [[nodiscard]] std::vector<std::uint8_t> to_rgba8() const {
        std::vector<std::uint8_t> out(pixels_.size() * 4);
        write_rgba8(out);
        return out;
    }

    /// Same conversion, straight into caller-owned storage.
    ///
    /// The windowed path calls this every frame into the buffer its cached
    /// CGBitmapContext already points at — returning a fresh vector instead
    /// would allocate and free several megabytes per frame.
    void write_rgba8(std::vector<std::uint8_t>& out) const {
        out.resize(pixels_.size() * 4);
        encode_span(pixels_.data(), out.data(), pixels_.size());
    }

    /// Encode a run of pixels. Public and static so the tiled renderer can
    /// encode
    /// tiles in parallel, on the thread that just rendered them and still has
    /// them in cache.
    static void encode_span(const Vec4* src, std::uint8_t* dst, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const Vec4& p = src[i];
            const float a = num::saturate(p.w);

            // Fully opaque is the overwhelmingly common case in a UI (the
            // background covers everything), and it skips the reciprocal.
            if (a >= 1.0f) {
                dst[0] = detail::srgb_encode(p.x);
                dst[1] = detail::srgb_encode(p.y);
                dst[2] = detail::srgb_encode(p.z);
                dst[3] = 255;
            } else if (a <= 0.0f) {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            } else {
                const float inv = 1.0f / a;
                dst[0] = detail::srgb_encode(p.x * inv);
                dst[1] = detail::srgb_encode(p.y * inv);
                dst[2] = detail::srgb_encode(p.z * inv);
                dst[3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
            }
            dst += 4;
        }
    }

  private:
    int width_, height_;
    std::vector<Vec4> pixels_;
};

/// Coverage source for glyph instances — the software path's tiny font hook.
class CoverageSampler {
  public:
    virtual ~CoverageSampler() = default;
    /// Coverage of texture `slot` at normalised (u,v).
    [[nodiscard]] virtual float sample(std::uint32_t slot, float u, float v) const = 0;

    /// RGBA of a COLOUR-glyph atlas at normalised (u,v), straight (not
    /// premultiplied). Only `color_glyph` instances call this; the default
    /// returns opaque white so a sampler that has no colour atlas degrades to
    /// a blank quad rather than misbehaving.
    [[nodiscard]] virtual Vec4 sample_rgba(std::uint32_t /*slot*/, float /*u*/,
                                           float /*v*/) const {
        return Vec4{1.0f, 1.0f, 1.0f, 0.0f};
    }

    /// RGBA of a USER IMAGE at normalised (u,v), straight (not premultiplied),
    /// sRGB-encoded. Only `texture` instances call this. The default returns
    /// opaque white, which multiplies through as "no image" rather than
    /// drawing a black rectangle when nothing is bound.
    [[nodiscard]] virtual Vec4 sample_texture(float /*u*/, float /*v*/) const {
        return Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    }
};

/// A user image the software path can sample for KIND_TEXTURE instances.
///
/// The GPU keeps this as a bound sampler; the CPU keeps it as a span. Both
/// exist so `image()` renders identically on either backend rather than being
/// a GPU-only feature -- a DSL element that works on one renderer and silently
/// draws nothing on the other is worse than no element at all.
struct TextureSource {
    const std::uint8_t* rgba = nullptr;   ///< straight, sRGB-encoded RGBA8
    int width = 0, height = 0;

    [[nodiscard]] bool valid() const noexcept {
        return rgba != nullptr && width > 0 && height > 0;
    }

    /// Bilinear sample, matching what a GPU sampler does. Nearest-neighbour
    /// here would make the two backends disagree along every image edge.
    [[nodiscard]] Vec4 sample(float u, float v) const noexcept {
        if (!valid()) return Vec4{1, 1, 1, 1};
        const float fx = num::clamp(u * static_cast<float>(width)  - 0.5f, 0.0f,
                                    static_cast<float>(width  - 1));
        const float fy = num::clamp(v * static_cast<float>(height) - 0.5f, 0.0f,
                                    static_cast<float>(height - 1));
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width  - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        const auto tap = [&](int x, int y, int c) {
            return static_cast<float>(
                rgba[(static_cast<std::size_t>(y) * width + x) * 4 + c]) / 255.0f;
        };
        Vec4 out{};
        float* o = &out.x;
        for (int c = 0; c < 4; ++c) {
            o[c] = num::lerp(num::lerp(tap(x0, y0, c), tap(x1, y0, c), tx),
                             num::lerp(tap(x0, y1, c), tap(x1, y1, c), tx), ty);
        }
        return out;
    }
};

/// Uploaded user images, and the CoverageSampler wrapper that lets the
/// software rasteriser see them.
///
/// A software window already has a CoverageSampler for glyphs (the font
/// stack). Images are a SECOND source, so rather than make every window
/// juggle two samplers this decorates the font one: glyph calls pass straight
/// through, `sample_texture` reads whichever image is bound. That keeps the
/// rasteriser's interface at one sampler and means a window gains image
/// support by owning this instead of by growing a code path.
class ImageStore final : public CoverageSampler {
  public:
    void set_glyphs(const CoverageSampler* s) noexcept { glyphs_ = s; }

    /// Take SHARED OWNERSHIP of the pixels. Never copies them.
    ///
    /// This used to do `slot.rgba = img.rgba`, which copied the whole buffer
    /// on every upload. For a video player that is 8 MB per frame at 1080p --
    /// about 500 MB/s of memcpy at 60fps, moving bytes that already existed
    /// and had not changed. The Cmd carries a shared_ptr precisely so the
    /// pixels can travel from decoder to renderer without being duplicated;
    /// copying here silently threw that away.
    void upload(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        if (!img) return;
        images_[id] = std::move(img);
        bound_ = id;
    }

    void release(std::uint32_t id) {
        images_.erase(id);
        if (bound_ == id) bound_ = 0;
    }

    /// Which image `sample_texture` reads. mayag binds ONE image at a time
    /// (see VulkanDevice::upload_texture for why), so the most recent upload
    /// wins -- exactly the video case, where one id is refreshed per frame.
    void bind(std::uint32_t id) noexcept { bound_ = id; }

    [[nodiscard]] bool empty() const noexcept { return images_.empty(); }

    // ── CoverageSampler ─────────────────────────────────────────────
    [[nodiscard]] float sample(std::uint32_t slot, float u, float v) const override {
        return glyphs_ != nullptr ? glyphs_->sample(slot, u, v) : 0.0f;
    }
    [[nodiscard]] Vec4 sample_rgba(std::uint32_t slot, float u, float v) const override {
        return glyphs_ != nullptr ? glyphs_->sample_rgba(slot, u, v)
                                  : Vec4{1.0f, 1.0f, 1.0f, 0.0f};
    }
    [[nodiscard]] Vec4 sample_texture(float u, float v) const override {
        const auto it = images_.find(bound_);
        if (it == images_.end() || !it->second) return Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        const ImageData& img = *it->second;
        TextureSource src{img.rgba.data(), img.width, img.height};
        return src.sample(u, v);
    }

  private:
    std::unordered_map<std::uint32_t, std::shared_ptr<const ImageData>> images_;
    std::uint32_t bound_ = 0;
    const CoverageSampler* glyphs_ = nullptr;
};

class Software {
  public:
    /// Execute a draw list into `fb`. Instances are processed in submission
    /// order (back to front); each is clipped to its batch's scissor rect.
    static void render(const DrawList& dl, Framebuffer& fb,
                       const CoverageSampler* sampler = nullptr) {
        for (const Batch& batch : dl.batches()) {
            const Rect scissor = clamp_scissor(batch.clip, fb);
            if (scissor.empty()) continue;

            for (std::uint32_t i = 0; i < batch.count; ++i) {
                draw_instance(dl.instances()[batch.first + i], fb, scissor,
                              batch.texture, batch.blend, sampler);
            }
        }
    }

    /// Render one instance into `fb`, clipped to `scissor`.
    ///
    /// Public so `Tiled` can drive the SAME shading kernel per tile. Sharing
    /// this rather than duplicating it is what keeps the tiled output
    /// bit-identical to the reference path.
    static void draw_instance_public(const Instance& inst, Framebuffer& fb,
                                     const Rect& scissor, std::uint32_t texture,
                                     BlendMode blend, const CoverageSampler* sampler) {
        draw_instance(inst, fb, scissor, texture, blend, sampler);
    }

  private:
    // Perceptual glyph correction (stem darkening) and the SDF threshold band
    // both live in render/kernel.hpp, the single source of truth every backend
    // shares. They used to live HERE, which is exactly why GPU text could never
    // match this rasteriser: no shader had them.
    static constexpr float sdf_edge = kernel::sdf_edge_fallback;

    [[nodiscard]] static float apply_text_gamma(float cov, Vec4 premul_color) noexcept {
        return kernel::apply_text_gamma(cov, premul_color);
    }

    [[nodiscard]] static Rect clamp_scissor(const Rect& clip, const Framebuffer& fb) {
        const Rect screen{0.0f, 0.0f, static_cast<float>(fb.width()),
                          static_cast<float>(fb.height())};
        return clip.intersect(screen).pixel_snap_out().intersect(screen);
    }

    static void draw_instance(const Instance& inst, Framebuffer& fb, const Rect& scissor,
                              std::uint32_t texture, BlendMode blend,
                              const CoverageSampler* sampler) {
        // A fully transparent instance cannot change any pixel, so skip it
        // before touching its bounding box. UI trees are full of these:
        // hover overlays at rest, faded-out panels, `opacity(0)` branches
        // that a view returns rather than conditionally omitting.
        if (inst.color.w <= 0.001f && inst.color2.w <= 0.001f) return;

        const auto kind = static_cast<ShapeKind>(inst.kind);
        const Rect shape{inst.rect.x, inst.rect.y, inst.rect.z, inst.rect.w};

        // Grow the iteration bounds for blurred shapes so the falloff is not
        // cut off at the geometric edge.
        float pad = 1.0f;
        if (kind == ShapeKind::shadow) pad = inst.params.x * 3.0f + 2.0f;

        const Rect bounds = shape.expand(pad).intersect(scissor).pixel_snap_out();
        if (bounds.empty()) return;

        const int x0 = static_cast<int>(bounds.left());
        const int y0 = static_cast<int>(bounds.top());
        const int x1 = static_cast<int>(bounds.right());
        const int y1 = static_cast<int>(bounds.bottom());

        const Vec2 center = shape.center();
        const Vec2 half   = shape.half();

        // Hoist the solid-colour case out of the pixel loop: most instances
        // are a flat fill, and re-deciding that per pixel is pure overhead.
        const bool is_gradient = (inst.flags & instance_flags::gradient) != 0;
        const bool simple_blend = (blend == BlendMode::normal);

        // ── interior fast path ──────────────────────────────────────────
        //
        // A page background or a panel is mostly INTERIOR: thousands of
        // pixels where coverage is exactly 1 and the SDF tells us nothing we
        // did not already know. Evaluating `rounded_box` for each of them
        // costs a length(), a few selects, and a smoothstep — and measured at
        // 15 ms for a single full-viewport fill.
        //
        // So compute the largest axis-aligned rect strictly inside the shape
        // (shrunk by the corner radius, which is where the boundary can
        // curve) and blit it directly. Only the thin border region actually
        // needs the distance field.
        const bool plain_fill =
            kind == ShapeKind::rounded_box &&
            (inst.flags & (instance_flags::stroke_only | instance_flags::gradient)) == 0 &&
            simple_blend;

        Rect interior{};
        if (plain_fill) {
            const float r = num::max(num::max(inst.radii.x, inst.radii.y),
                                     num::max(inst.radii.z, inst.radii.w));
            // 1 px inset keeps the antialiased edge on the slow path.
            const float inset = r + 1.0f;
            if (shape.width() > inset * 2.0f && shape.height() > inset * 2.0f) {
                interior = shape.inset(inset).intersect(bounds);
            }
        }

        const bool has_interior = !interior.empty() && interior.width() >= 2.0f &&
                                  interior.height() >= 2.0f;
        const int ix0 = has_interior ? static_cast<int>(std::ceil(interior.left()))   : 0;
        const int iy0 = has_interior ? static_cast<int>(std::ceil(interior.top()))    : 0;
        const int ix1 = has_interior ? static_cast<int>(interior.right())  : 0;
        const int iy1 = has_interior ? static_cast<int>(interior.bottom()) : 0;

        // ── colour glyph (emoji) ────────────────────────────────────────
        //
        // Sample the RGBA colour-glyph atlas per pixel and blend it verbatim:
        // the atlas supplies the colour, the instance supplies only opacity
        // (in color.w). No SDF, no tint — a 😀 keeps its own colours.
        if (kind == ShapeKind::color_glyph && sampler != nullptr) {
            const float op = inst.color.w;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                    const float u = (p.x - shape.left()) / shape.width();
                    const float v = (p.y - shape.top())  / shape.height();
                    const float su = inst.uv.x + u * (inst.uv.z - inst.uv.x);
                    const float sv = inst.uv.y + v * (inst.uv.w - inst.uv.y);
                    const Vec4 texel = sampler->sample_rgba(inst.texture_slot, su, sv);
                    const float a = texel.w * op;
                    if (a <= 0.001f) continue;
                    // Straight -> premultiplied, honouring opacity.
                    fb.blend(x, y, Vec4{texel.x * a, texel.y * a, texel.z * a, a}, 1.0f);
                }
            }
            return;
        }

        // ── user image ───────────────────────────────────────────
        //
        // Same shading the GPU does for KIND_TEXTURE, kept in step with
        // render/kernel.hpp: sample straight sRGB RGBA, DECODE to linear, then
        // modulate by the instance colour (so `image(id) | fg(c)` tints and
        // `| opacity(a)` fades) and premultiply. Skipping the decode is a
        // gamma error that makes every tinted image too bright.
        if (kind == ShapeKind::texture && sampler != nullptr) {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const Vec2 p{static_cast<float>(x) + 0.5f,
                                 static_cast<float>(y) + 0.5f};
                    const float cov = coverage_at(inst, kind, p, center, half,
                                                  texture, sampler);
                    if (cov <= 0.001f) continue;
                    const float u = (p.x - shape.left()) / shape.width();
                    const float v = (p.y - shape.top())  / shape.height();
                    const Vec4 t = sampler->sample_texture(num::saturate(u),
                                                           num::saturate(v));
                    const Vec4 lin{kernel::srgb_to_linear(t.x),
                                   kernel::srgb_to_linear(t.y),
                                   kernel::srgb_to_linear(t.z), t.w};
                    const Vec4 c{inst.color.x * lin.x, inst.color.y * lin.y,
                                 inst.color.z * lin.z, inst.color.w * lin.w};
                    fb.blend(x, y, c, cov);
                }
            }
            return;
        }

        // ── backdrop (frosted glass) ──────────────────────────────────
        //
        // Read the pixels ALREADY on the framebuffer under this rect, blur
        // them (separable box blur, a couple of passes approximating a
        // Gaussian), tweak saturation/brightness, and write the result back
        // inside the rounded-rect coverage. Because instances render in emit
        // order and the painter emits this before the panel's fill, the input
        // is the fully-composited background — the vibrancy look, no offscreen
        // pass.
        if (kind == ShapeKind::backdrop) {
            blur_backdrop(inst, fb, x0, y0, x1, y1, center, half);
            return;
        }

        for (int y = y0; y < y1; ++y) {
            const bool interior_row = has_interior && y >= iy0 && y < iy1;

            for (int x = x0; x < x1; ++x) {
                if (interior_row && x >= ix0 && x < ix1) {
                    // Fully covered: blend once, then skip the whole span.
                    for (; x < ix1; ++x) fb.blend(x, y, inst.color, 1.0f);
                    --x;
                    continue;
                }

                // Sample at the pixel centre — the half-pixel offset that
                // makes a 1px line land ON a pixel instead of straddling two.
                const Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};

                const float cov = coverage_at(inst, kind, p, center, half, texture, sampler);
                if (cov <= 0.001f) continue;

                if (!is_gradient && simple_blend) {
                    fb.blend(x, y, inst.color, cov);
                    continue;
                }

                Vec4 color = shade(inst, kind, p, shape);
                if (!simple_blend) color = apply_blend(color, fb.at(x, y), blend);
                fb.blend(x, y, color, cov);
            }
        }
    }

    [[nodiscard]] static float coverage_at(const Instance& inst, ShapeKind kind, Vec2 p,
                                           Vec2 center, Vec2 half, std::uint32_t texture,
                                           const CoverageSampler* sampler) {
        const Vec2 local = p - center;

        // Geometric kinds are decided by the shared kernel, so this rasteriser
        // and every shader evaluate byte-identical maths. Only the kinds that
        // need a CPU-side resource (the glyph atlas via CoverageSampler) are
        // handled here; the rest fall through to kernel::shape_coverage.
        switch (kind) {
            case ShapeKind::rounded_box:
            case ShapeKind::circle:
            case ShapeKind::ring:
            case ShapeKind::arc:
            case ShapeKind::line:
            case ShapeKind::shadow:
            case ShapeKind::texture: {
                kernel::Fragment f;
                f.local  = local;
                f.frag   = p;
                f.half   = half;
                f.radii  = inst.radii;
                f.axis   = inst.axis;
                f.params = inst.params;
                f.px     = 1.0f;
                f.kind   = inst.kind;
                f.flags  = inst.flags;
                return kernel::shape_coverage(f);
            }

            case ShapeKind::glyph: {
                if (sampler == nullptr) return 0.0f;
                const float u = num::lerp(inst.uv.x, inst.uv.z,
                                          num::saturate((p.x - (center.x - half.x)) / (half.x * 2.0f)));
                const float v = num::lerp(inst.uv.y, inst.uv.w,
                                          num::saturate((p.y - (center.y - half.y)) / (half.y * 2.0f)));
                // The atlas slot rides in the INSTANCE, not the batch, so
                // glyphs and shapes share one batch. See DrawList::glyph.
                (void)texture;
                const float raw = sampler->sample(inst.texture_slot, u, v);

                // A bitmap entry IS coverage; a distance-field entry has to be
                // thresholded. Which one this is travels per instance, because
                // hybrid mode mixes both within a single frame. params.x is
                // the scale-correct half-band the emitter computed for THIS
                // glyph (so magnified text stays crisp); fall back to the
                // constant when it wasn't set.
                const float band = inst.params.x > 0.0f ? inst.params.x : sdf_edge;
                const float cov = (inst.flags & instance_flags::glyph_sdf) == 0
                    ? raw
                    : num::smoothstep(0.5f - band, 0.5f + band, raw);

                // ── stem darkening ──────────────────────────────────────
                //
                // Compositing in linear light is physically correct, but for
                // TEXT it is perceptually wrong in a specific, measurable
                // way: a 50%-covered pixel of white-on-dark lands ~0.19 L*
                // lighter than a human reads as "half", so light text on a
                // dark background looks thin and washed out. (Measured with
                // Oklch against the linear blend.) Dark-on-light has the
                // opposite, milder error.
                //
                // Every serious text renderer corrects for this — FreeType
                // calls it stem darkening, Apple and Skia apply a
                // contrast-dependent gamma. mayag raises coverage to a power
                // chosen by the text/background polarity, which thickens
                // light-on-dark and leaves dark-on-light essentially alone.
                //
                // Applied to COVERAGE, not colour, so it cannot shift hue.
                return apply_text_gamma(cov, inst.color);
            }

            case ShapeKind::color_glyph:
                // Colour glyphs are blended per-pixel from the RGBA atlas in
                // draw_instance before this coverage path runs, so control
                // never reaches here; the case exists for switch exhaustivity.
                return 0.0f;

            case ShapeKind::backdrop:
                // Backdrops read + blur the framebuffer in draw_instance before
                // this path; never reached here.
                return 0.0f;
        }
        return 0.0f;
    }

  public:
    /// Interpolate two LINEAR premultiplied colours through Ok**LCH**.
    ///
    /// The implementation lives in render/kernel.hpp so the shader and this
    /// rasteriser cannot diverge — the shader is transpiled from that file.
    /// Kept here as a named entry point because tests and the gradient path
    /// call it directly.
    ///
    /// Why Oklch and not Oklab: Oklab is CARTESIAN, so a straight line between
    /// two roughly-opposite hues passes close to the neutral axis and the ramp
    /// desaturates in the middle almost as badly as an RGB one. Oklch is POLAR,
    /// so chroma is interpolated directly and the ramp stays vivid end to end.
    /// Hue takes the shorter way around the wheel.
    [[nodiscard]] static Vec4 lerp_oklch(Vec4 a, Vec4 b, float t) noexcept {
        return kernel::mix_oklch(a, b, t);
    }

  private:
    [[nodiscard]] static Vec4 shade(const Instance& inst, ShapeKind kind, Vec2 p,
                                    const Rect& shape) {
        if ((inst.flags & instance_flags::gradient) == 0) return inst.color;

        // Normalised position inside the shape, so gradients follow resizes.
        const Vec2 n{shape.size.x > 0.0f ? (p.x - shape.left()) / shape.size.x : 0.0f,
                     shape.size.y > 0.0f ? (p.y - shape.top()) / shape.size.y : 0.0f};

        float t;
        if (inst.flags & instance_flags::gradient_radial) {
            const Vec2 c{inst.axis.x, inst.axis.y};
            t = num::saturate((n - c).length() / num::max(inst.params.z, 1e-4f));
        } else if (inst.flags & instance_flags::gradient_angular) {
            const Vec2 c{inst.axis.x, inst.axis.y};
            const Vec2 d = n - c;
            t = num::saturate((num::atan2(d.y, d.x) + num::pi) / num::tau);
        } else {
            // Linear: project onto the gradient axis.
            const Vec2  a{inst.axis.x, inst.axis.y};
            const Vec2  b{inst.axis.z, inst.axis.w};
            const Vec2  ab = b - a;
            const float len2 = dot(ab, ab);
            t = len2 <= 0.0f ? 0.0f : num::saturate(dot(n - a, ab) / len2);
        }

        (void)kind;
        // Perceptual by default; `srgb_interpolation` opts into the naive ramp
        // for designs that were authored against a CSS gradient.
        return (inst.flags & instance_flags::gradient_srgb)
             ? lerp(inst.color, inst.color2, t)
             : lerp_oklch(inst.color, inst.color2, t);
    }

    /// Frosted glass: separable box blur of the framebuffer region under a
    /// backdrop instance, with saturation/brightness, written back inside the
    /// rounded-rect coverage. Operates on the premultiplied-linear buffer.
    static void blur_backdrop(const Instance& inst, Framebuffer& fb,
                              int x0, int y0, int x1, int y1,
                              Vec2 center, Vec2 half) {
        if (x1 <= x0 || y1 <= y0) return;
        const int rw = x1 - x0, rh = y1 - y0;
        const int radius = static_cast<int>(num::clamp(inst.params.x, 0.0f, 64.0f));
        const float saturation = inst.params.y;
        const float brightness = inst.params.z;

        // Copy the region out (straight, un-premultiplied, so blur/sat/bright
        // are correct on colour rather than on premultiplied values).
        std::vector<Vec4> buf(static_cast<std::size_t>(rw) * rh);
        for (int y = 0; y < rh; ++y) {
            for (int x = 0; x < rw; ++x) {
                const Vec4& p = fb.at(x0 + x, y0 + y);
                const float a = p.w;
                const float inv = a > 1e-4f ? 1.0f / a : 0.0f;
                buf[static_cast<std::size_t>(y) * rw + x] =
                    Vec4{p.x * inv, p.y * inv, p.z * inv, a};
            }
        }

        // Separable box blur, run twice — two box passes approximate a Gaussian
        // closely and cheaply. Clamped at the region edge (the panel is small
        // relative to the frame, so edge bias is invisible under the fill).
        if (radius > 0) {
            std::vector<Vec4> tmp(buf.size());
            const float norm = 1.0f / static_cast<float>(2 * radius + 1);
            for (int pass = 0; pass < 2; ++pass) {
                // Horizontal.
                for (int y = 0; y < rh; ++y) {
                    for (int x = 0; x < rw; ++x) {
                        Vec4 s{};
                        for (int k = -radius; k <= radius; ++k) {
                            const int sx = num::clamp(x + k, 0, rw - 1);
                            s = s + buf[static_cast<std::size_t>(y) * rw + sx];
                        }
                        tmp[static_cast<std::size_t>(y) * rw + x] = s * norm;
                    }
                }
                // Vertical.
                for (int y = 0; y < rh; ++y) {
                    for (int x = 0; x < rw; ++x) {
                        Vec4 s{};
                        for (int k = -radius; k <= radius; ++k) {
                            const int sy = num::clamp(y + k, 0, rh - 1);
                            s = s + tmp[static_cast<std::size_t>(sy) * rw + x];
                        }
                        buf[static_cast<std::size_t>(y) * rw + x] = s * norm;
                    }
                }
            }
        }

        // Write back inside the rounded-rect coverage, applying sat/brightness.
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const Vec2 local{static_cast<float>(x) + 0.5f - center.x,
                                 static_cast<float>(y) + 0.5f - center.y};
                const float d = sdf::rounded_box(local, half, inst.radii);
                const float cov = sdf::coverage(d);
                if (cov <= 0.0f) continue;
                Vec4 c = buf[static_cast<std::size_t>(y - y0) * rw + (x - x0)];
                // Saturation: mix toward Rec.709 luma. Brightness: scale.
                if (saturation != 1.0f) {
                    const float luma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
                    c.x = luma + (c.x - luma) * saturation;
                    c.y = luma + (c.y - luma) * saturation;
                    c.z = luma + (c.z - luma) * saturation;
                }
                c.x *= brightness; c.y *= brightness; c.z *= brightness;
                c.x = num::max(c.x, 0.0f); c.y = num::max(c.y, 0.0f); c.z = num::max(c.z, 0.0f);
                // Re-premultiply and replace the pixel by coverage.
                Vec4& dst = fb.at(x, y);
                const Vec4 blurred{c.x * c.w, c.y * c.w, c.z * c.w, c.w};
                dst = Vec4{dst.x * (1 - cov) + blurred.x * cov,
                           dst.y * (1 - cov) + blurred.y * cov,
                           dst.z * (1 - cov) + blurred.z * cov,
                           dst.w * (1 - cov) + blurred.w * cov};
            }
        }
    }

    [[nodiscard]] static Vec4 apply_blend(Vec4 src, Vec4 dst, BlendMode mode) {
        switch (mode) {
            case BlendMode::normal:     return src;
            case BlendMode::multiply:   return Vec4{src.x * dst.x, src.y * dst.y, src.z * dst.z, src.w};
            case BlendMode::screen:     return Vec4{1 - (1 - src.x) * (1 - dst.x),
                                                    1 - (1 - src.y) * (1 - dst.y),
                                                    1 - (1 - src.z) * (1 - dst.z), src.w};
            case BlendMode::additive:   return Vec4{src.x + dst.x, src.y + dst.y, src.z + dst.z, src.w};
            case BlendMode::subtract:   return Vec4{num::max(dst.x - src.x, 0.0f),
                                                    num::max(dst.y - src.y, 0.0f),
                                                    num::max(dst.z - src.z, 0.0f), src.w};
            case BlendMode::difference: return Vec4{num::abs(dst.x - src.x),
                                                    num::abs(dst.y - src.y),
                                                    num::abs(dst.z - src.z), src.w};
            case BlendMode::overlay: {
                const auto ov = [](float s, float d) {
                    return d < 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
                };
                return Vec4{ov(src.x, dst.x), ov(src.y, dst.y), ov(src.z, dst.z), src.w};
            }
        }
        return src;
    }
};

}  // namespace mayag::backend
