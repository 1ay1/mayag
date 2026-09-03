#pragma once
// mayag::backend::Tiled — the parallel rasteriser
//
// Same shading kernel as `Software`, but organised so a modern machine can
// actually use its cores. Three ideas, in order of how much they win:
//
//   1. TILING + BINNING. The screen is cut into fixed tiles; each instance is
//      appended to the tiles its bounding box touches. A tile then renders
//      only the instances that can possibly affect it, which turns the
//      "background rect covers everything" case from O(instances x pixels)
//      into O(instances_in_tile x tile_pixels).
//
//   2. PARALLELISM. Tiles are disjoint in memory, so they render with no
//      locking and no false sharing. Work is claimed dynamically because UI
//      tiles vary wildly in cost.
//
//   3. OCCLUSION. Within a tile, if an opaque instance completely covers the
//      tile, everything queued beneath it is dropped before a single pixel is
//      touched. A typical UI is mostly opaque panels stacked on an opaque
//      background, so this deletes a large fraction of the work outright.
//
// Output is bit-identical to `Software`. That is asserted by a test, and it
// is what lets the fast path be the default without risk.

#include "simd.hpp"
#include "software.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace mayag::backend {

/// Tile size in pixels.
///
/// 64x64 is 16 KB of float4 accumulation, which sits inside L1 on every CPU
/// mayag targets, and is large enough that per-tile overhead stays negligible
/// against the pixel work. Smaller tiles bin better but pay more overhead;
/// larger ones spill L1 and reduce parallel granularity.
inline constexpr int tile_size = 64;

struct TiledStats {
    int  tiles = 0;
    int  tiles_skipped = 0;      ///< had no instances at all
    long instance_refs = 0;      ///< total (tile, instance) pairs after binning
    long instances_culled = 0;   ///< dropped by occlusion
    unsigned threads = 1;
};

class Tiled {
  public:
    /// Render `dl` into `fb`. `pool` may be null for single-threaded output.
    static TiledStats render(const DrawList& dl, Framebuffer& fb,
                             const CoverageSampler* sampler = nullptr,
                             ThreadPool* pool = &shared_pool(),
                             Color<Srgb> clear = {}) {
        TiledStats stats;

        const int tiles_x = (fb.width()  + tile_size - 1) / tile_size;
        const int tiles_y = (fb.height() + tile_size - 1) / tile_size;
        const int n_tiles = tiles_x * tiles_y;
        if (n_tiles <= 0) return stats;

        stats.tiles = n_tiles;
        stats.threads = pool ? static_cast<unsigned>(pool->worker_count() + 1) : 1u;

        // ── bin ─────────────────────────────────────────────────────────
        //
        // One pass over the draw list, appending each instance index to every
        // tile its (scissored) bounds overlap. Bins are plain vectors of
        // u32; a frame's binning is a few microseconds.
        std::vector<std::vector<std::uint32_t>> bins(static_cast<std::size_t>(n_tiles));

        const auto bin_instance = [&](std::uint32_t global_index,
                                      const Instance& inst, const Rect& scissor) {
            const Rect bounds = instance_bounds(inst).intersect(scissor);
            if (bounds.empty()) return;

            const int x0 = num::max(static_cast<int>(bounds.left())   / tile_size, 0);
            const int y0 = num::max(static_cast<int>(bounds.top())    / tile_size, 0);
            const int x1 = num::min(static_cast<int>(bounds.right())  / tile_size, tiles_x - 1);
            const int y1 = num::min(static_cast<int>(bounds.bottom()) / tile_size, tiles_y - 1);

            for (int ty = y0; ty <= y1; ++ty) {
                for (int tx = x0; tx <= x1; ++tx) {
                    bins[static_cast<std::size_t>(ty) * tiles_x + tx].push_back(global_index);
                    ++stats.instance_refs;
                }
            }
        };

        // Flatten batches into a single indexed list so a tile can replay
        // instances in submission order regardless of which batch they came
        // from. `owner` records each instance's batch for its render state.
        std::vector<std::uint32_t> owner(dl.instances().size(), 0);
        for (std::uint32_t b = 0; b < dl.batches().size(); ++b) {
            const Batch& batch = dl.batches()[b];
            const Rect scissor = clamp_scissor(batch.clip, fb);
            for (std::uint32_t k = 0; k < batch.count; ++k) {
                const std::uint32_t gi = batch.first + k;
                if (gi >= dl.instances().size()) break;
                owner[gi] = b;
                if (scissor.empty()) continue;
                bin_instance(gi, dl.instances()[gi], scissor);
            }
        }

        // ── render tiles ────────────────────────────────────────────────
        std::atomic<long> culled{0};

        const auto render_tile = [&](std::size_t index) {
            const int tx = static_cast<int>(index) % tiles_x;
            const int ty = static_cast<int>(index) / tiles_x;

            const Rect tile{
                static_cast<float>(tx * tile_size), static_cast<float>(ty * tile_size),
                static_cast<float>(num::min(tile_size, fb.width()  - tx * tile_size)),
                static_cast<float>(num::min(tile_size, fb.height() - ty * tile_size))};

            auto& list = bins[index];

            // Clear this tile. Doing it here rather than in a separate full
            // -framebuffer pass keeps the pixels in cache for the shading
            // that immediately follows.
            fill_tile(fb, tile, clear);

            if (list.empty()) return;

            // Occlusion: scan backwards for the last instance that covers the
            // whole tile opaquely, and drop everything before it.
            std::size_t start = 0;
            for (std::size_t k = list.size(); k-- > 0;) {
                if (covers_opaque(dl.instances()[list[k]], tile, dl.batches()[owner[list[k]]])) {
                    start = k;
                    break;
                }
            }
            if (start > 0) culled.fetch_add(static_cast<long>(start), std::memory_order_relaxed);

            for (std::size_t k = start; k < list.size(); ++k) {
                const std::uint32_t gi = list[k];
                const Batch& batch = dl.batches()[owner[gi]];
                const Rect scissor = clamp_scissor(batch.clip, fb).intersect(tile);
                if (scissor.empty()) continue;

                Software::draw_instance_public(dl.instances()[gi], fb, scissor,
                                               batch.texture, batch.blend, sampler);
            }
        };

        if (pool != nullptr && n_tiles > 1) {
            pool->parallel_for(static_cast<std::size_t>(n_tiles), render_tile);
        } else {
            for (int i = 0; i < n_tiles; ++i) render_tile(static_cast<std::size_t>(i));
        }

        for (const auto& b : bins) if (b.empty()) ++stats.tiles_skipped;
        stats.instances_culled = culled.load(std::memory_order_relaxed);
        return stats;
    }

    /// Encode straight to RGBA8, in parallel, tile by tile.
    ///
    /// The serial encode was 10.5 ms at 2x — as expensive as the rendering
    /// itself once that was parallelised, and a textbook case of Amdahl's
    /// law capping the win. Rows are independent, so this is a pure split.
    static void encode_parallel(const Framebuffer& fb, std::vector<std::uint8_t>& out,
                                ThreadPool* pool) {
        const std::size_t n = static_cast<std::size_t>(fb.width()) * fb.height();
        out.resize(n * 4);

        if (pool == nullptr || pool->worker_count() == 0 || fb.height() < 64) {
            Framebuffer::encode_span(&fb.at(0, 0), out.data(), n);
            return;
        }

        // One chunk per worker plus the caller; bigger chunks amortise the
        // atomic claim, and rows are contiguous so each stays cache-friendly.
        const std::size_t chunks = pool->worker_count() + 1;
        const int rows_per = (fb.height() + static_cast<int>(chunks) - 1) /
                             static_cast<int>(chunks);

        pool->parallel_for(chunks, [&](std::size_t c) {
            const int y0 = static_cast<int>(c) * rows_per;
            const int y1 = num::min(y0 + rows_per, fb.height());
            if (y0 >= y1) return;
            const std::size_t offset = static_cast<std::size_t>(y0) * fb.width();
            const std::size_t count  = static_cast<std::size_t>(y1 - y0) * fb.width();
            Framebuffer::encode_span(&fb.at(0, y0), out.data() + offset * 4, count);
        });
    }

  private:
    [[nodiscard]] static Rect clamp_scissor(const Rect& clip, const Framebuffer& fb) {
        const Rect screen{0.0f, 0.0f, static_cast<float>(fb.width()),
                          static_cast<float>(fb.height())};
        return clip.intersect(screen).pixel_snap_out().intersect(screen);
    }

    /// Screen-space extent an instance can touch, including blur falloff.
    [[nodiscard]] static Rect instance_bounds(const Instance& inst) {
        const Rect shape{inst.rect.x, inst.rect.y, inst.rect.z, inst.rect.w};
        const auto kind = static_cast<ShapeKind>(inst.kind);
        float pad = 1.0f;
        if (kind == ShapeKind::shadow) pad = inst.params.x * 3.0f + 2.0f;
        return shape.expand(pad);
    }

    /// True when this instance paints the ENTIRE tile at full opacity, so
    /// anything already queued beneath it cannot show through.
    ///
    /// Conservative on purpose: only axis-aligned opaque fills qualify, and
    /// the tile must sit strictly inside the shape's corner-radius inset. A
    /// false positive here would erase visible content.
    [[nodiscard]] static bool covers_opaque(const Instance& inst, const Rect& tile,
                                            const Batch& batch) {
        if (static_cast<ShapeKind>(inst.kind) != ShapeKind::rounded_box) return false;
        if (batch.blend != BlendMode::normal) return false;
        if (inst.flags & (instance_flags::stroke_only | instance_flags::gradient)) return false;
        if (inst.color.w < 1.0f) return false;
        if (!batch.clip.intersect(tile).contains(tile.min())) {
            // The clip must fully contain the tile, else part of it is
            // untouched by this instance.
            if (!contains_rect(batch.clip, tile)) return false;
        }

        const Rect shape{inst.rect.x, inst.rect.y, inst.rect.z, inst.rect.w};
        const float r = num::max(num::max(inst.radii.x, inst.radii.y),
                                 num::max(inst.radii.z, inst.radii.w));
        return contains_rect(shape.inset(r), tile);
    }

    [[nodiscard]] static bool contains_rect(const Rect& outer, const Rect& inner) noexcept {
        return inner.left()  >= outer.left()  && inner.right()  <= outer.right() &&
               inner.top()   >= outer.top()   && inner.bottom() <= outer.bottom();
    }

    /// Clear one tile to `c`, premultiplied linear.
    static void fill_tile(Framebuffer& fb, const Rect& tile, Color<Srgb> c) {
        const auto l = c.to<Linear>();
        const Vec4 pre{l.c0 * l.a, l.c1 * l.a, l.c2 * l.a, l.a};

        const int x0 = static_cast<int>(tile.left());
        const int y0 = static_cast<int>(tile.top());
        const int x1 = static_cast<int>(tile.right());
        const int y1 = static_cast<int>(tile.bottom());

        for (int y = y0; y < y1; ++y) {
            Vec4* row = &fb.at(x0, y);
            for (int x = 0; x < x1 - x0; ++x) row[x] = pre;
        }
    }
};

}  // namespace mayag::backend
