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
#include "../render/sdf.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
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
        std::uint8_t* dst = out.data();

        for (const Vec4& p : pixels_) {
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

  private:
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

        switch (kind) {
            case ShapeKind::rounded_box: {
                float d = sdf::rounded_box(local, half, inst.radii);
                if (inst.flags & instance_flags::stroke_only) {
                    // A border is the outline of the same field. Offsetting by
                    // half the width first keeps an inside stroke inside.
                    d = sdf::outline(d + inst.params.x * 0.5f, inst.params.x);
                }
                return sdf::coverage_smooth(d);
            }
            case ShapeKind::circle:
                return sdf::coverage_smooth(sdf::circle(local, num::min(half.x, half.y)));

            case ShapeKind::ring:
                return sdf::coverage_smooth(
                    sdf::ring(local, num::min(half.x, half.y), inst.params.x));

            case ShapeKind::arc:
                return sdf::coverage_smooth(
                    sdf::arc(local, num::min(half.x, half.y), inst.params.x,
                             inst.params.y, inst.params.z));

            case ShapeKind::line:
                return sdf::coverage_smooth(
                    sdf::segment(p, Vec2{inst.axis.x, inst.axis.y},
                                 Vec2{inst.axis.z, inst.axis.w}, inst.params.x));

            case ShapeKind::shadow: {
                const float d = sdf::rounded_box(local, half, inst.radii);
                const float c = sdf::shadow_coverage(d, num::max(inst.params.x, 0.01f));
                return (inst.flags & instance_flags::inset) ? 1.0f - c : c;
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
                return sampler->sample(inst.texture_slot, u, v);
            }

            case ShapeKind::texture:
                return sdf::coverage_smooth(sdf::rounded_box(local, half, inst.radii));
        }
        return 0.0f;
    }

  public:
    /// Interpolate two LINEAR premultiplied colours through Ok**LCH**.
    ///
    /// This is the gradient claim made real, and the choice of Oklch over
    /// Oklab is the entire point. Oklab is CARTESIAN: a straight line between
    /// two roughly-opposite hues passes close to the neutral axis, so an
    /// Oklab ramp desaturates in the middle almost as badly as an RGB one.
    /// Oklch is POLAR — it carries chroma and hue as separate coordinates, so
    /// chroma is interpolated directly and the ramp stays vivid end to end.
    ///
    /// Hue takes the shorter way around the wheel, so red -> magenta does not
    /// detour through green.
    ///
    /// Same constants as color.hpp, so CPU and shader agree; the shader
    /// version lives in shader_source.hpp.
    [[nodiscard]] static Vec4 lerp_oklch(Vec4 a, Vec4 b, float t) noexcept {
        // Un-premultiply: interpolating premultiplied chroma is meaningless.
        const auto unpre = [](Vec4 c) -> Vec4 {
            const float inv = c.w > 1e-6f ? 1.0f / c.w : 0.0f;
            return {c.x * inv, c.y * inv, c.z * inv, c.w};
        };
        const Vec4 ca = unpre(a), cb = unpre(b);

        // Linear sRGB -> LMS -> cube root -> Oklab. The cube root is the trick.
        const auto to_lab = [](Vec4 c) -> Vec4 {
            const float l = 0.4122214708f * c.x + 0.5363325363f * c.y + 0.0514459929f * c.z;
            const float m = 0.2119034982f * c.x + 0.6806995451f * c.y + 0.1073969566f * c.z;
            const float s = 0.0883024619f * c.x + 0.2817188376f * c.y + 0.6299787005f * c.z;
            const float l_ = num::cbrt(l), m_ = num::cbrt(m), s_ = num::cbrt(s);
            return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                    1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                    0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
                    c.w};
        };

        const Vec4 la = to_lab(ca), lb = to_lab(cb);

        // -> polar
        const float ca_chroma = Vec2{la.y, la.z}.length();
        const float cb_chroma = Vec2{lb.y, lb.z}.length();
        const float ha = num::atan2(la.z, la.y);
        float       hb = num::atan2(lb.z, lb.y);

        // Shortest path around the hue circle.
        float dh = hb - ha;
        if (dh >  num::pi) dh -= num::tau;
        if (dh < -num::pi) dh += num::tau;

        const float L      = num::lerp(la.x, lb.x, t);
        const float chroma = num::lerp(ca_chroma, cb_chroma, t);
        const float hue    = ha + dh * t;
        const float alpha  = num::lerp(la.w, lb.w, t);

        // -> back to Oklab -> linear sRGB
        const float A = chroma * num::cos(hue);
        const float B = chroma * num::sin(hue);

        const float l_ = L + 0.3963377774f * A + 0.2158037573f * B;
        const float m_ = L - 0.1055613458f * A - 0.0638541728f * B;
        const float s_ = L - 0.0894841775f * A - 1.2914855480f * B;
        const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;

        const float r  = num::max( 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, 0.0f);
        const float g  = num::max(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s, 0.0f);
        const float bl = num::max(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s, 0.0f);

        return {r * alpha, g * alpha, bl * alpha, alpha};
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
