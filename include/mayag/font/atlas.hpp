#pragma once
// mayag::typo::Atlas — the glyph cache
//
// Glyphs are rasterised once and live in a shared texture. Everything about
// this file exists to keep that texture SMALL and the draw call count at ONE:
//
//   * skyline bottom-left packing — near-optimal for the tall-thin shapes
//     glyphs actually are, at O(width) per insert with no backtracking
//   * an SDF cache keyed by (face, glyph, size-bucket), so a heading and body
//     text at different sizes SHARE one entry when the field is scale-free
//   * generation-based LRU eviction, so a long-running app that renders every
//     CJK codepoint eventually reclaims space instead of growing forever
//   * dirty-rect tracking, so only the changed sub-rectangle is uploaded to
//     the GPU each frame rather than the whole atlas
//
// The atlas is CPU-side. `render/draw_list.hpp` references it by texture slot
// and the backend uploads the dirty region; nothing here talks to a GPU.

#include "raster.hpp"
#include "../core/geometry.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// Glyph key
// ════════════════════════════════════════════════════════════════════════

/// Identifies a rasterised glyph.
///
/// `size_bucket` is quantised rather than exact. SDF entries are scale-free,
/// so every size maps to bucket 0 and one entry serves all of them — that is
/// the entire reason mayag can render 8 px captions and 96 px display type
/// from a single atlas. Bitmap entries bucket to the nearest integer pixel
/// size, because those genuinely differ.
struct GlyphKey {
    std::uint32_t face_id     = 0;
    std::uint16_t glyph_id    = 0;
    std::uint16_t size_bucket = 0;
    std::uint8_t  flags       = 0;   ///< see GlyphFlags

    friend constexpr bool operator==(const GlyphKey&, const GlyphKey&) = default;

    [[nodiscard]] constexpr std::uint64_t hash() const noexcept {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](std::uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(face_id);
        mix(static_cast<std::uint64_t>(glyph_id) << 16 | size_bucket);
        mix(flags);
        return h;
    }
};

namespace glyph_flags {
inline constexpr std::uint8_t none      = 0;
inline constexpr std::uint8_t sdf       = 1 << 0;   ///< distance field, not coverage
inline constexpr std::uint8_t subpixel  = 1 << 1;   ///< RGB subpixel positioned
inline constexpr std::uint8_t synthetic_bold   = 1 << 2;
inline constexpr std::uint8_t synthetic_italic = 1 << 3;
}  // namespace glyph_flags

}  // namespace mayag::typo

namespace std {
template <>
struct hash<mayag::typo::GlyphKey> {
    [[nodiscard]] std::size_t operator()(const mayag::typo::GlyphKey& k) const noexcept {
        return static_cast<std::size_t>(k.hash());
    }
};
}  // namespace std

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// Cached glyph
// ════════════════════════════════════════════════════════════════════════

/// A glyph's placement in the atlas plus the geometry needed to draw it.
struct CachedGlyph {
    /// Pixel rect within the atlas texture.
    Rect atlas_rect{};
    /// Normalised UV rect, precomputed so the render path does no division.
    Rect uv{};
    /// Offset from the pen position to the bitmap's top-left, in pixels at
    /// the size this entry was rasterised for.
    Vec2 bearing{};
    /// Bitmap size in pixels.
    Vec2 size{};
    /// Advance width in pixels.
    float advance = 0.0f;
    /// The scale this entry was rasterised at, so an SDF entry shared across
    /// sizes can be rescaled at draw time.
    float raster_scale = 1.0f;
    /// SDF spread in pixels; 0 for coverage bitmaps.
    float spread = 0.0f;

    bool  is_sdf = false;
    bool  valid  = false;
    /// True when this entry is a colour glyph (emoji): its pixels live in the
    /// FontStack's separate RGBA colour atlas, not this coverage atlas, and
    /// `uv` indexes that atlas instead.
    bool  is_color = false;

    /// Last frame this glyph was drawn — drives LRU eviction.
    std::uint64_t last_used = 0;
};

// ════════════════════════════════════════════════════════════════════════
// Skyline packer
// ════════════════════════════════════════════════════════════════════════

/// Skyline bottom-left rectangle packing.
///
/// The skyline is the upper contour of everything placed so far, stored as a
/// small run-length list. To place a rect we scan for the position whose
/// resulting top edge is lowest — a good heuristic that runs in O(nodes) and,
/// unlike guillotine packing, never fragments the free space into slivers.
class SkylinePacker {
  public:
    SkylinePacker() = default;
    SkylinePacker(int width, int height) { reset(width, height); }

    void reset(int width, int height) {
        width_  = std::max(width, 1);
        height_ = std::max(height, 1);
        nodes_.clear();
        nodes_.push_back(Node{0, 0, width_});
        used_area_ = 0;
    }

    /// Find a home for a `w`x`h` rect. Returns false when the atlas is full.
    [[nodiscard]] bool pack(int w, int h, int& out_x, int& out_y) {
        if (w <= 0 || h <= 0 || w > width_ || h > height_) return false;

        int best_index = -1;
        int best_y = height_ + 1;
        int best_x = 0;
        int best_waste = width_ * height_;

        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            int y = 0, waste = 0;
            if (!fits(i, w, y, waste)) continue;

            // Prefer the lowest top edge; break ties by the least wasted
            // space underneath, which keeps the skyline from developing
            // deep unusable notches.
            if (y + h < best_y || (y + h == best_y && waste < best_waste)) {
                best_index = static_cast<int>(i);
                best_y     = y + h;
                best_x     = nodes_[i].x;
                best_waste = waste;
            }
        }

        if (best_index < 0 || best_y > height_) return false;

        out_x = best_x;
        out_y = best_y - h;
        insert(static_cast<std::size_t>(best_index), best_x, best_y, w);
        used_area_ += w * h;
        return true;
    }

    [[nodiscard]] int  width()  const noexcept { return width_; }
    [[nodiscard]] int  height() const noexcept { return height_; }
    /// Fraction of the atlas consumed — used to decide when to grow.
    [[nodiscard]] float occupancy() const noexcept {
        return static_cast<float>(used_area_) / static_cast<float>(width_ * height_);
    }

  private:
    struct Node { int x, y, width; };

    /// Can a `w`-wide rect start at node `i`? Computes the resulting top edge
    /// and the dead space trapped beneath it.
    [[nodiscard]] bool fits(std::size_t i, int w, int& out_y, int& out_waste) const {
        const int x = nodes_[i].x;
        if (x + w > width_) return false;

        int y = nodes_[i].y;
        int remaining = w;
        int waste = 0;

        for (std::size_t k = i; remaining > 0; ++k) {
            if (k >= nodes_.size()) return false;
            const int span = std::min(remaining, nodes_[k].width);
            if (nodes_[k].y > y) {
                // A taller node forces us up; everything already counted
                // becomes wasted space.
                waste += (nodes_[k].y - y) * (w - remaining);
                y = nodes_[k].y;
            } else {
                waste += (y - nodes_[k].y) * span;
            }
            remaining -= span;
        }

        out_y = y;
        out_waste = waste;
        return true;
    }

    void insert(std::size_t i, int x, int top, int w) {
        nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(i), Node{x, top, w});

        // Trim the nodes the new one now covers.
        for (std::size_t k = i + 1; k < nodes_.size();) {
            const int prev_right = nodes_[k - 1].x + nodes_[k - 1].width;
            if (nodes_[k].x >= prev_right) break;

            const int shrink = prev_right - nodes_[k].x;
            nodes_[k].x     += shrink;
            nodes_[k].width -= shrink;
            if (nodes_[k].width > 0) break;
            nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(k));
        }

        // Merge adjacent nodes at the same height, so the list stays short.
        for (std::size_t k = 0; k + 1 < nodes_.size();) {
            if (nodes_[k].y == nodes_[k + 1].y) {
                nodes_[k].width += nodes_[k + 1].width;
                nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(k) + 1);
            } else {
                ++k;
            }
        }
    }

    int               width_  = 0;
    int               height_ = 0;
    long long         used_area_ = 0;
    std::vector<Node> nodes_;
};

// ════════════════════════════════════════════════════════════════════════
// Atlas
// ════════════════════════════════════════════════════════════════════════

/// A single-channel glyph texture with a residency cache.
class Atlas {
  public:
    explicit Atlas(int size = 1024) { resize(size, size); }

    void resize(int w, int h) {
        width_  = std::max(w, 64);
        height_ = std::max(h, 64);
        pixels_.assign(static_cast<std::size_t>(width_) * height_, 0);
        packer_.reset(width_, height_);
        entries_.clear();
        // 1px is reserved so `uv` never samples row 0 / column 0, which some
        // GPUs treat as a clamp boundary.
        dirty_ = Rect{};
        ++generation_;
    }

    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::size_t glyph_count() const noexcept { return entries_.size(); }
    [[nodiscard]] float occupancy() const noexcept { return packer_.occupancy(); }
    /// Bumped whenever the texture is reallocated, so a backend knows to
    /// re-upload everything rather than a dirty rect.
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

    /// Region changed since the last `clear_dirty()`. Empty when nothing moved.
    [[nodiscard]] const Rect& dirty_region() const noexcept { return dirty_; }
    void clear_dirty() noexcept { dirty_ = Rect{}; }

    void set_frame(std::uint64_t frame) noexcept { frame_ = frame; }

    /// Look up a glyph. Returns nullptr when it is not resident.
    [[nodiscard]] const CachedGlyph* find(const GlyphKey& key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        it->second.last_used = frame_;
        return &it->second;
    }

    /// Insert a rasterised bitmap. Returns nullptr when the atlas is full and
    /// eviction could not free enough room — the caller should grow or split.
    const CachedGlyph* insert(const GlyphKey& key, const Bitmap& bmp,
                              Vec2 bearing, float advance,
                              float raster_scale, float spread, bool is_sdf) {
        CachedGlyph g;
        g.bearing      = bearing;
        g.advance      = advance;
        g.raster_scale = raster_scale;
        g.spread       = spread;
        g.is_sdf       = is_sdf;
        g.last_used    = frame_;
        g.valid        = true;

        // A blank glyph (space) still needs a cache entry so we do not
        // re-rasterise it every frame; it just occupies no atlas space.
        if (bmp.empty()) {
            g.size = Vec2{};
            auto [it, _] = entries_.insert_or_assign(key, g);
            return &it->second;
        }

        // 1px gutter prevents bilinear filtering from bleeding a neighbour's
        // ink into this glyph's edge — the classic atlas artefact.
        constexpr int gutter = 1;
        const int w = bmp.width + gutter * 2;
        const int h = bmp.height + gutter * 2;

        int x = 0, y = 0;
        if (!packer_.pack(w, h, x, y)) {
            if (!evict_and_repack(w, h, x, y)) return nullptr;
        }

        blit(bmp, x + gutter, y + gutter);

        g.atlas_rect = Rect{static_cast<float>(x + gutter), static_cast<float>(y + gutter),
                            static_cast<float>(bmp.width), static_cast<float>(bmp.height)};
        g.size = Vec2{static_cast<float>(bmp.width), static_cast<float>(bmp.height)};
        g.uv = Rect{g.atlas_rect.left()  / static_cast<float>(width_),
                    g.atlas_rect.top()   / static_cast<float>(height_),
                    g.atlas_rect.width() / static_cast<float>(width_),
                    g.atlas_rect.height()/ static_cast<float>(height_)};

        auto [it, _] = entries_.insert_or_assign(key, g);
        return &it->second;
    }

    /// Insert a COLOUR glyph (emoji). Its RGBA pixels are packed into a second
    /// plane that shares this atlas's packer and dimensions, so colour and
    /// coverage glyphs are cached, packed and evicted through one path. The
    /// returned entry is marked `is_color`; its `uv` indexes the colour plane.
    const CachedGlyph* insert_color(const GlyphKey& key,
                                    const std::uint8_t* rgba, int bw, int bh,
                                    Vec2 bearing, float advance) {
        CachedGlyph g;
        g.bearing   = bearing;
        g.advance   = advance;
        g.is_color  = true;
        g.last_used = frame_;
        g.valid     = true;
        if (bw <= 0 || bh <= 0 || rgba == nullptr) {
            g.size = Vec2{};
            auto [it, _] = entries_.insert_or_assign(key, g);
            return &it->second;
        }

        constexpr int gutter = 1;
        const int w = bw + gutter * 2;
        const int h = bh + gutter * 2;
        int x = 0, y = 0;
        if (!packer_.pack(w, h, x, y)) {
            if (!evict_and_repack(w, h, x, y)) return nullptr;
        }

        ensure_color_plane();
        for (int row = 0; row < bh; ++row) {
            const std::uint8_t* src = rgba + static_cast<std::size_t>(row) * bw * 4;
            std::uint8_t* dst = color_.data() +
                (static_cast<std::size_t>(y + gutter + row) * width_ + (x + gutter)) * 4;
            std::memcpy(dst, src, static_cast<std::size_t>(bw) * 4);
        }

        g.atlas_rect = Rect{static_cast<float>(x + gutter), static_cast<float>(y + gutter),
                            static_cast<float>(bw), static_cast<float>(bh)};
        g.size = Vec2{static_cast<float>(bw), static_cast<float>(bh)};
        g.uv = Rect{g.atlas_rect.left()  / static_cast<float>(width_),
                    g.atlas_rect.top()   / static_cast<float>(height_),
                    g.atlas_rect.width() / static_cast<float>(width_),
                    g.atlas_rect.height()/ static_cast<float>(height_)};
        const Rect touched{static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(w), static_cast<float>(h)};
        color_dirty_ = color_dirty_.empty() ? touched : color_dirty_.unite(touched);
        auto [it, _] = entries_.insert_or_assign(key, g);
        return &it->second;
    }

    /// Sample the RGBA colour plane at normalised (u,v), straight (not
    /// premultiplied), nearest-neighbour. Emoji strikes are authored at a
    /// fixed ppem and drawn near 1:1, so nearest keeps them crisp and avoids
    /// bleeding the transparent gutter into the edge.
    [[nodiscard]] Vec4 sample_color(float u, float v) const noexcept {
        if (color_.empty()) return Vec4{1.0f, 1.0f, 1.0f, 0.0f};
        const int x = num::clamp(static_cast<int>(u * static_cast<float>(width_)),
                                 0, width_ - 1);
        const int y = num::clamp(static_cast<int>(v * static_cast<float>(height_)),
                                 0, height_ - 1);
        const std::size_t i = (static_cast<std::size_t>(y) * width_ + x) * 4;
        return Vec4{color_[i] / 255.0f, color_[i + 1] / 255.0f,
                    color_[i + 2] / 255.0f, color_[i + 3] / 255.0f};
    }

    /// Sample the atlas in normalised coordinates — the software backend's
    /// glyph path calls this.
    [[nodiscard]] float sample(float u, float v) const noexcept {
        if (pixels_.empty()) return 0.0f;
        const float fx = num::clamp(u * static_cast<float>(width_)  - 0.5f, 0.0f,
                                    static_cast<float>(width_  - 1));
        const float fy = num::clamp(v * static_cast<float>(height_) - 0.5f, 0.0f,
                                    static_cast<float>(height_ - 1));
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width_  - 1);
        const int y1 = std::min(y0 + 1, height_ - 1);
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);

        const auto px = [&](int x, int y) -> float {
            return pixels_[static_cast<std::size_t>(y) * width_ + x] / 255.0f;
        };
        return num::lerp(num::lerp(px(x0, y0), px(x1, y0), tx),
                         num::lerp(px(x0, y1), px(x1, y1), tx), ty);
    }

    /// Drop glyphs untouched for `max_age` frames. Called opportunistically;
    /// a UI that stops showing CJK text should not keep paying for it.
    std::size_t collect(std::uint64_t max_age) {
        std::size_t removed = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (frame_ > it->second.last_used + max_age) {
                it = entries_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

  private:
    void blit(const Bitmap& bmp, int dx, int dy) {
        for (int y = 0; y < bmp.height; ++y) {
            const int ty = dy + y;
            if (ty < 0 || ty >= height_) continue;
            for (int x = 0; x < bmp.width; ++x) {
                const int tx = dx + x;
                if (tx < 0 || tx >= width_) continue;
                pixels_[static_cast<std::size_t>(ty) * width_ + tx] = bmp.at(x, y);
            }
        }
        const Rect touched{static_cast<float>(dx), static_cast<float>(dy),
                           static_cast<float>(bmp.width), static_cast<float>(bmp.height)};
        dirty_ = dirty_.empty() ? touched : dirty_.unite(touched);
    }

    /// When the atlas fills up: drop everything not used recently and rebuild
    /// the skyline. A full repack is expensive but happens rarely, and the
    /// alternative (fragmenting forever) is worse.
    bool evict_and_repack(int w, int h, int& x, int& y) {
        const std::uint64_t cutoff = frame_ > 60 ? frame_ - 60 : 0;

        std::vector<std::pair<GlyphKey, CachedGlyph>> survivors;
        survivors.reserve(entries_.size());
        for (const auto& [k, g] : entries_) {
            if (g.last_used >= cutoff) survivors.emplace_back(k, g);
        }
        if (survivors.size() == entries_.size()) return false;   // nothing to gain

        // Repack survivors tallest-first, which is what makes skyline
        // packing efficient on a second pass.
        std::sort(survivors.begin(), survivors.end(),
                  [](const auto& a, const auto& b) { return a.second.size.y > b.second.size.y; });

        std::vector<std::uint8_t> old_pixels = std::move(pixels_);
        const int old_w = width_;

        pixels_.assign(static_cast<std::size_t>(width_) * height_, 0);
        packer_.reset(width_, height_);
        entries_.clear();

        constexpr int gutter = 1;
        for (auto& [k, g] : survivors) {
            const int gw = static_cast<int>(g.size.x) + gutter * 2;
            const int gh = static_cast<int>(g.size.y) + gutter * 2;
            int nx = 0, ny = 0;
            if (!packer_.pack(gw, gh, nx, ny)) continue;

            // Copy the pixels from their old home.
            for (int row = 0; row < static_cast<int>(g.size.y); ++row) {
                for (int col = 0; col < static_cast<int>(g.size.x); ++col) {
                    const int sx = static_cast<int>(g.atlas_rect.left()) + col;
                    const int sy = static_cast<int>(g.atlas_rect.top())  + row;
                    const std::size_t si = static_cast<std::size_t>(sy) * old_w + sx;
                    if (si >= old_pixels.size()) continue;
                    const int tx = nx + gutter + col, ty = ny + gutter + row;
                    pixels_[static_cast<std::size_t>(ty) * width_ + tx] = old_pixels[si];
                }
            }

            g.atlas_rect = Rect{static_cast<float>(nx + gutter), static_cast<float>(ny + gutter),
                                g.size.x, g.size.y};
            g.uv = Rect{g.atlas_rect.left()  / static_cast<float>(width_),
                        g.atlas_rect.top()   / static_cast<float>(height_),
                        g.atlas_rect.width() / static_cast<float>(width_),
                        g.atlas_rect.height()/ static_cast<float>(height_)};
            entries_.insert_or_assign(k, g);
        }

        // The whole texture moved.
        dirty_ = Rect{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
        return packer_.pack(w, h, x, y);
    }

    void ensure_color_plane() {
        if (color_.empty()) {
            color_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
        }
    }

    int                       width_  = 0;
    int                       height_ = 0;
    std::vector<std::uint8_t> pixels_;
    /// RGBA colour plane for emoji glyphs, allocated lazily on first colour
    /// glyph. Same dimensions and packer as the coverage plane.
    std::vector<std::uint8_t> color_;
    Rect                      color_dirty_{};
    SkylinePacker             packer_;
    std::unordered_map<GlyphKey, CachedGlyph> entries_;
    Rect                      dirty_{};
    std::uint64_t             frame_ = 0;
    std::uint32_t             generation_ = 0;
};

}  // namespace mayag::typo
