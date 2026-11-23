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

#include "backend.hpp"   // Device / AtlasView / register_backend
#include "simd.hpp"
#include "software.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    int  tiles_reused = 0;       ///< unchanged since the last frame: not touched
    long instance_refs = 0;      ///< total (tile, instance) pairs after binning
    long instances_culled = 0;   ///< dropped by occlusion
    unsigned threads = 1;
};

namespace tile_hash {
/// 64-bit mix (wyhash-style multiply-fold). Cheap next to shading even one
/// glyph, and good enough that two different tiles colliding is not a
/// practical concern (2^-64 per tile per frame).
[[nodiscard]] inline std::uint64_t mix(std::uint64_t a, std::uint64_t b) noexcept {
    const __uint128_t r = static_cast<__uint128_t>(a ^ 0xA0761D6478BD642Full) * (b ^ 0xE7037ED1A0B428DBull);
    return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
}
inline void bytes(std::uint64_t& h, const void* p, std::size_t n) noexcept {
    const auto* c = static_cast<const unsigned char*>(p);
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        std::uint64_t a, b;
        std::memcpy(&a, c + i, 8);
        std::memcpy(&b, c + i + 8, 8);
        h = mix(h ^ a, b ^ 0x8EBC6AF09C88C6E3ull);
    }
    for (; i + 8 <= n; i += 8) { std::uint64_t a; std::memcpy(&a, c + i, 8); h = mix(h, a); }
    for (; i < n; ++i) h = mix(h, c[i]);
}
}  // namespace tile_hash

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
        // One slot per tile, summed after the join: each tile writes only its
        // own slot, so the count needs no atomic.
        std::vector<long> culled(static_cast<std::size_t>(n_tiles), 0);

        // A backdrop (frosted glass) blurs pixels from AROUND its own rect,
        // which other tiles are drawing at the same moment. Per tile it would
        // blur only that tile's 64 px -- a visible mosaic. So a frame with
        // backdrops renders in PHASES split at each one: every tile draws the
        // instances before it, the backdrop is applied once over its whole
        // region (exactly as the reference rasteriser does), then tiles carry
        // on. Frames without backdrops take the single-phase path unchanged.
        std::vector<std::uint32_t> backdrops;
        for (std::uint32_t gi = 0; gi < dl.instances().size(); ++gi)
            if (static_cast<ShapeKind>(dl.instances()[gi].kind) == ShapeKind::backdrop) backdrops.push_back(gi);
        if (!backdrops.empty()) {
            const auto run_phase = [&](std::uint32_t lo, std::uint32_t hi, bool clear_first) {
                const auto tile_phase = [&](std::size_t index) {
                    const int tx = static_cast<int>(index) % tiles_x;
                    const int ty = static_cast<int>(index) / tiles_x;
                    const Rect tile{
                        static_cast<float>(tx * tile_size), static_cast<float>(ty * tile_size),
                        static_cast<float>(num::min(tile_size, fb.width()  - tx * tile_size)),
                        static_cast<float>(num::min(tile_size, fb.height() - ty * tile_size))};
                    if (clear_first) fill_tile(fb, tile, clear);
                    const auto& list = bins[index];
                    auto it = std::lower_bound(list.begin(), list.end(), lo);
                    for (; it != list.end() && *it < hi; ++it) {
                        const std::uint32_t gi = *it;
                        const Batch& batch = dl.batches()[owner[gi]];
                        const Rect scissor = clamp_scissor(batch.clip, fb).intersect(tile);
                        if (scissor.empty()) continue;
                        Software::draw_instance_public(dl.instances()[gi], fb, scissor,
                                                       batch.texture, batch.blend, sampler);
                    }
                };
                if (pool != nullptr && n_tiles > 1) pool->parallel_for(static_cast<std::size_t>(n_tiles), tile_phase);
                else for (int i = 0; i < n_tiles; ++i) tile_phase(static_cast<std::size_t>(i));
            };
            std::uint32_t lo = 0;
            bool first = true;
            for (const std::uint32_t b : backdrops) {
                run_phase(lo, b, first);
                first = false;
                const Batch& batch = dl.batches()[owner[b]];
                const Rect scissor = clamp_scissor(batch.clip, fb);
                if (!scissor.empty())
                    Software::draw_instance_public(dl.instances()[b], fb, scissor, batch.texture, batch.blend, sampler);
                lo = b + 1;
            }
            run_phase(lo, static_cast<std::uint32_t>(dl.instances().size()), first);
            for (const auto& b : bins) if (b.empty()) ++stats.tiles_skipped;
            fb.invalidate_tiles();   // a backdrop reads across tiles: no per-tile reuse this frame
            return stats;
        }

        // ── damage ───────────────────────────────────────────────────────────
        //
        // A tile's pixels are a pure function of: the clear colour, the
        // instances that reach it (from the occluder on), each one's scissor
        // within the tile, texture and blend, and what the sampler returns.
        // Hash exactly that; a tile whose hash matches last frame's already
        // HOLDS the right pixels -- no clear, no shading, no encode. Typing
        // one character re-shades the one or two tiles under the cursor,
        // not the whole window. Bit-identical by construction: either the
        // same inputs, or a full re-shade.
        auto& mem = fb.tile_memory();
        if (mem.tiles_x != tiles_x || mem.tiles_y != tiles_y || mem.keys.size() != static_cast<std::size_t>(n_tiles)) {
            mem.keys.assign(static_cast<std::size_t>(n_tiles), 0);
            mem.tiles_x = tiles_x;
            mem.tiles_y = tiles_y;
        }
        mem.dirty.assign(static_cast<std::size_t>(n_tiles), 1);
        const std::uint64_t sver = sampler != nullptr ? sampler->version() : 1;
        const bool reuse_ok = sver != 0;   // a sampler that can't version: always re-shade
        std::uint64_t frame_seed = 0x9E3779B97F4A7C15ull;
        {
            const auto l = clear.to<Linear>();
            const float cc[4] = {l.c0, l.c1, l.c2, l.a};
            tile_hash::bytes(frame_seed, cc, sizeof cc);
            frame_seed = tile_hash::mix(frame_seed, sver);
            frame_seed = tile_hash::mix(frame_seed, static_cast<std::uint64_t>(fb.width()) << 32 | static_cast<std::uint32_t>(fb.height()));
        }
        std::vector<int> reused(static_cast<std::size_t>(n_tiles), 0);

        const auto render_tile = [&](std::size_t index) {
            const int tx = static_cast<int>(index) % tiles_x;
            const int ty = static_cast<int>(index) / tiles_x;

            const Rect tile{
                static_cast<float>(tx * tile_size), static_cast<float>(ty * tile_size),
                static_cast<float>(num::min(tile_size, fb.width()  - tx * tile_size)),
                static_cast<float>(num::min(tile_size, fb.height() - ty * tile_size))};

            auto& list = bins[index];

            // Occlusion: scan backwards for the last instance that covers the
            // whole tile opaquely, and drop everything before it.
            std::size_t start = 0;
            for (std::size_t k = list.size(); k-- > 0;) {
                if (covers_opaque(dl.instances()[list[k]], tile, dl.batches()[owner[list[k]]])) {
                    start = k;
                    break;
                }
            }
            if (start > 0) culled[index] = static_cast<long>(start);

            // what decides this tile's pixels, and is it what it was?
            std::uint64_t key = tile_hash::mix(frame_seed, index);
            for (std::size_t k = start; k < list.size(); ++k) {
                const std::uint32_t gi = list[k];
                const Batch& batch = dl.batches()[owner[gi]];
                const Rect scissor = clamp_scissor(batch.clip, fb).intersect(tile);
                if (scissor.empty()) continue;
                tile_hash::bytes(key, &dl.instances()[gi], sizeof(Instance));
                const float sc[4] = {scissor.origin.x, scissor.origin.y, scissor.size.x, scissor.size.y};
                tile_hash::bytes(key, sc, sizeof sc);
                key = tile_hash::mix(key, (static_cast<std::uint64_t>(batch.texture) << 8) | static_cast<std::uint64_t>(batch.blend));
            }
            key |= 1;   // never 0 (0 = unknown)
            if (reuse_ok && mem.keys[index] == key) {
                mem.dirty[index] = 0;
                reused[index] = 1;
                return;
            }
            mem.keys[index] = reuse_ok ? key : 0;

            // Clear this tile. Doing it here rather than in a separate full
            // -framebuffer pass keeps the pixels in cache for the shading
            // that immediately follows.
            fill_tile(fb, tile, clear);

            if (list.empty()) return;

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
        for (const long c : culled) stats.instances_culled += c;
        for (const int r : reused) stats.tiles_reused += r;
        mem.valid = true;
        // MAYAG_DAMAGE_MAP=1: the re-shaded tiles of every frame as a text map
        // on stderr (# re-shaded, . reused, space empty) -- where damage leaks.
        static const bool map = [] { const char* e = std::getenv("MAYAG_DAMAGE_MAP"); return e && *e && *e != '0'; }();
        if (map) {
            std::string s;
            for (int y = 0; y < tiles_y; ++y) {
                for (int x = 0; x < tiles_x; ++x) {
                    const auto i = static_cast<std::size_t>(y * tiles_x + x);
                    s += bins[i].empty() ? ' ' : reused[i] ? '.' : '#';
                }
                s += '\n';
            }
            std::fprintf(stderr, "damage %d/%d\n%s", n_tiles - stats.tiles_reused, n_tiles, s.c_str());
        }
        return stats;
    }

    /// Encode only the tiles the last render() changed, into `out`, which
    /// must hold this framebuffer's previous encode (same size). Falls back
    /// to a full encode when it can't know. What a window that keeps its
    /// pixel buffer between frames calls instead of encode_parallel.
    static void encode_damaged(const Framebuffer& fb, std::vector<std::uint8_t>& out, ThreadPool* pool) {
        const std::size_t n = static_cast<std::size_t>(fb.width()) * fb.height();
        const auto& mem = fb.tile_memory();
        if (!mem.valid || out.size() != n * 4 || mem.dirty.size() != static_cast<std::size_t>(mem.tiles_x) * mem.tiles_y) {
            encode_parallel(fb, out, pool);
            return;
        }
        std::vector<std::uint32_t> todo;
        for (std::uint32_t i = 0; i < mem.dirty.size(); ++i) if (mem.dirty[i]) todo.push_back(i);
        if (todo.empty()) return;
        const auto encode_tile = [&](std::size_t k) {
            const int tx = static_cast<int>(todo[k]) % mem.tiles_x, ty = static_cast<int>(todo[k]) / mem.tiles_x;
            const int x0 = tx * tile_size, y0 = ty * tile_size;
            const int w = num::min(tile_size, fb.width() - x0), y1 = num::min(y0 + tile_size, fb.height());
            for (int y = y0; y < y1; ++y)
                Framebuffer::encode_span(&fb.at(x0, y), out.data() + (static_cast<std::size_t>(y) * fb.width() + x0) * 4,
                                         static_cast<std::size_t>(w));
        };
        if (pool != nullptr && todo.size() > 4) pool->parallel_for(todo.size(), encode_tile);
        else for (std::size_t k = 0; k < todo.size(); ++k) encode_tile(k);
    }

    /// encode_damaged, but BGRA, into caller-owned memory with an explicit
    /// row stride: a GDI DIB section's bits, so the encode lands in the very
    /// buffer the window blits from. `full` forces every tile (a new or
    /// resized target). Returns the damaged rows [y0, y1); y0 == y1 when
    /// nothing changed.
    static std::pair<int, int> encode_damaged_bgra(const Framebuffer& fb, std::uint8_t* out, std::size_t stride,
                                                   bool full, ThreadPool* pool) {
        const auto& mem = fb.tile_memory();
        const int W = fb.width(), H = fb.height();
        const bool known = mem.valid && mem.dirty.size() == static_cast<std::size_t>(mem.tiles_x) * mem.tiles_y;
        if (full || !known) {
            const auto row = [&](std::size_t y) {
                Framebuffer::encode_span_bgra(&fb.at(0, static_cast<int>(y)), out + y * stride,
                                              static_cast<std::size_t>(W));
            };
            if (pool != nullptr && H > 64) pool->parallel_for(static_cast<std::size_t>(H), row);
            else for (int y = 0; y < H; ++y) row(static_cast<std::size_t>(y));
            return {0, H};
        }
        std::vector<std::uint32_t> todo;
        int top = mem.tiles_y, bot = -1;
        for (std::uint32_t i = 0; i < mem.dirty.size(); ++i)
            if (mem.dirty[i]) {
                todo.push_back(i);
                const int ty = static_cast<int>(i) / mem.tiles_x;
                top = num::min(top, ty);
                bot = num::max(bot, ty);
            }
        if (todo.empty()) return {0, 0};
        const auto encode_tile = [&](std::size_t k) {
            const int tx = static_cast<int>(todo[k]) % mem.tiles_x, ty = static_cast<int>(todo[k]) / mem.tiles_x;
            const int x0 = tx * tile_size, y0 = ty * tile_size;
            const int w = num::min(tile_size, W - x0), y1 = num::min(y0 + tile_size, H);
            for (int y = y0; y < y1; ++y)
                Framebuffer::encode_span_bgra(&fb.at(x0, y), out + static_cast<std::size_t>(y) * stride + x0 * 4u,
                                              static_cast<std::size_t>(w));
        };
        if (pool != nullptr && todo.size() > 4) pool->parallel_for(todo.size(), encode_tile);
        else for (std::size_t k = 0; k < todo.size(); ++k) encode_tile(k);
        return {top * tile_size, num::min(H, (bot + 1) * tile_size)};
    }

    /// Whether the last render() changed any pixel (false: the frame is the
    /// one already on screen -- a host may skip presenting it).
    [[nodiscard]] static bool any_damage(const Framebuffer& fb) noexcept {
        const auto& mem = fb.tile_memory();
        if (!mem.valid) return true;
        for (const auto d : mem.dirty) if (d) return true;
        return false;
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

        // Four chunks per thread: big enough to amortise the claim, small
        // enough that a helper that wakes late doesn't hold up the call (the
        // threads already running take its share). Rows stay contiguous.
        const std::size_t chunks = (pool->worker_count() + 1) * 4;
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
        return shape.expand(kernel::quad_pad(inst.kind, inst.flags, inst.params.x));
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

// ── the Device adapter ──────────────────────────────────────────────
//
// The software rasteriser is a PEER of every GPU backend, not a special case:
// it implements the same backend::Device and registers itself the same way.
// That is the load-bearing test of the interface — if `Device` had leaked GPU
// assumptions (command buffers, descriptor sets, a swapchain), a CPU renderer
// could not implement it, and every caller would need a "but if software..."
// branch. None do.
//
// It registers LAST in preference order and never fails to initialise, which
// is what makes "a missing driver degrades to a slower frame" true.

/// Samples the glyph atlas out of an AtlasView, so the software path gets
/// glyphs through the same type-erased handle the GPU backends use.
///
/// Filtering is BILINEAR with a half-texel offset, which is what a GPU sampler
/// does. Nearest-neighbour here would make the software backend disagree with
/// every GPU backend along glyph edges — the abstraction has to preserve the
/// filtering, not just the bytes.
class AtlasViewSampler final : public CoverageSampler {
  public:
    explicit AtlasViewSampler(const AtlasView& v) : v_{v} {}

    [[nodiscard]] float sample(std::uint32_t, float u, float v) const override {
        if (v_.coverage == nullptr || v_.width <= 0 || v_.height <= 0) return 0.0f;
        Bilinear b{v_, u, v};
        const auto px = [&](int x, int y) -> float {
            return static_cast<float>(
                v_.coverage[static_cast<std::size_t>(y) * v_.width + x]) / 255.0f;
        };
        return b.blend(px);
    }

    [[nodiscard]] Vec4 sample_rgba(std::uint32_t, float u, float v) const override {
        if (!v_.has_color || v_.color == nullptr) return Vec4{1, 1, 1, 0};
        Bilinear b{v_, u, v};
        const auto chan = [&](int c) {
            return b.blend([&](int x, int y) -> float {
                const std::size_t i = (static_cast<std::size_t>(y) * v_.width + x) * 4;
                return static_cast<float>(v_.color[i + static_cast<std::size_t>(c)]) / 255.0f;
            });
        };
        return Vec4{chan(0), chan(1), chan(2), chan(3)};
    }

  private:
    /// The four taps and weights, computed once per sample.
    struct Bilinear {
        int x0, y0, x1, y1;
        float tx, ty;
        Bilinear(const AtlasView& v, float u, float vv) {
            const float fx = num::clamp(u  * static_cast<float>(v.width)  - 0.5f, 0.0f,
                                        static_cast<float>(v.width  - 1));
            const float fy = num::clamp(vv * static_cast<float>(v.height) - 0.5f, 0.0f,
                                        static_cast<float>(v.height - 1));
            x0 = static_cast<int>(fx); y0 = static_cast<int>(fy);
            x1 = std::min(x0 + 1, v.width  - 1);
            y1 = std::min(y0 + 1, v.height - 1);
            tx = fx - static_cast<float>(x0);
            ty = fy - static_cast<float>(y0);
        }
        template <typename F>
        [[nodiscard]] float blend(F&& px) const {
            return num::lerp(num::lerp(px(x0, y0), px(x1, y0), tx),
                             num::lerp(px(x0, y1), px(x1, y1), tx), ty);
        }
    };

    AtlasView v_;
};

class SoftwareBackend final : public Device {
  public:
    /// Accepts any surface, including none: there is nothing to attach to.
    /// Presenting is the caller's job (an shm buffer, a blit, a PNG), because
    /// how CPU pixels reach a screen is a platform question, not a renderer
    /// one. `render_offscreen` is the portable way to get them.
    [[nodiscard]] static std::unique_ptr<Device> create(const SurfaceHandle&,
                                                        Vec2 logical, float dpi) {
        auto self = std::make_unique<SoftwareBackend>();
        self->resize(logical, dpi);
        return self;
    }

    [[nodiscard]] static std::unique_ptr<Device> create_offscreen() {
        return std::make_unique<SoftwareBackend>();
    }

    [[nodiscard]] DeviceInfo info() const override {
        DeviceInfo i;
        i.api = Api::software;
        i.adapter = "CPU";
        return i;
    }

    void resize(Vec2 logical, float dpi) override {
        w_ = static_cast<int>(logical.x * dpi);
        h_ = static_cast<int>(logical.y * dpi);
    }

    FrameStats submit(const DrawList& list, Color<Srgb> clear) override {
        std::vector<std::uint8_t> px;
        (void)render_offscreen(list, w_, h_, clear, px);
        last_frame_ = std::move(px);
        ++frames_;
        FrameStats st;
        st.instances  = static_cast<std::uint32_t>(list.instances().size());
        st.draw_calls = static_cast<std::uint32_t>(list.batches().size());
        return st;
    }

    void sync_atlas(const AtlasView& v) override { atlas_ = v; }

    void upload_image(std::uint32_t id,
                      std::shared_ptr<const ImageData> img) override {
        images_.upload(id, std::move(img));
    }
    void release_image(std::uint32_t id) override { images_.release(id); }

    [[nodiscard]] bool render_offscreen(const DrawList& list, int w, int h,
                                        Color<Srgb> clear,
                                        std::vector<std::uint8_t>& out) override {
        if (w <= 0 || h <= 0) return false;
        Framebuffer fb{w, h};
        AtlasViewSampler sampler{atlas_};
        images_.set_glyphs(atlas_.coverage != nullptr ? &sampler : nullptr);
        Tiled::render(list, fb, &images_, &shared_pool(), clear);
        Tiled::encode_parallel(fb, out, &shared_pool());
        return true;
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept override { return frames_; }

    /// The most recent frame, for platforms that present CPU pixels themselves.
    [[nodiscard]] const std::vector<std::uint8_t>& last_frame() const noexcept {
        return last_frame_;
    }

  private:
    int           w_ = 0, h_ = 0;
    AtlasView     atlas_{};
    ImageStore    images_;
    std::vector<std::uint8_t> last_frame_;
    std::uint64_t frames_ = 0;
};

inline const bool software_registered = [] {
    register_backend({Api::software, &SoftwareBackend::create,
                      &SoftwareBackend::create_offscreen});
    return true;
}();

}  // namespace mayag::backend
