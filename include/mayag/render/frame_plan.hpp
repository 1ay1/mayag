#pragma once
// mayag::render::FramePlanner — the order a GPU draws one DrawList in, when
// glyphs and shapes blend differently.
//
// WHY
// ---
// Shapes composite in LINEAR light (an _SRGB colour target). Text does not:
// CoreGraphics, and therefore every mac app and mayag's software reference,
// draws a glyph as a lerp of sRGB-ENCODED values (kernel::encoded_text_source).
// A fragment shader cannot see the destination, so a GPU gets that exact
// blend only by rendering glyphs through a UNORM view of the same image —
// a different render pass. The planner decides which instances go in which
// pass so the frame still looks exactly as if drawn in list order.
//
// HOW
// ---
// Every instance gets a LAYER: even layers draw shapes, odd layers draw text.
// An instance must land above every EARLIER instance it overlaps — on the
// same layer if it is the same class (a layer keeps list order), on a later
// one otherwise — and instances that don't overlap commute freely. So a
// whole screen of labels on their cards needs 2 passes, not one per label:
// layer 0 every card, layer 1 every label. Overlap is tested on exact,
// conservative pixel bounds (the quad's antialiasing skirt, a shadow's blur,
// the batch clip) through a coarse tile grid, so the cost is linear in the
// instances for real UIs.
//
// A backdrop (frosted glass) reads everything drawn before it, so it splits
// the frame into segments planned independently, with a capture between.
//
// Pure C++, no GPU: tests/test_vulkan.cpp checks it headless.

#include "draw_list.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mayag::render {

/// One CmdDraw: `count` consecutive instances from `first`, under the clip
/// of batch `batch`.
struct DrawRun {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t batch = 0;
};

/// One step of the frame: a pass of shapes, a pass of text (drawn through
/// the encoded view), or a backdrop capture of the region `capture_instance`
/// reads.
struct PassStep {
    enum class Kind : std::uint8_t { shapes, text, capture };
    Kind          kind = Kind::shapes;
    std::uint32_t runs_begin = 0, runs_end = 0;
    std::uint32_t capture_instance = 0;
};

struct FramePlan {
    std::vector<PassStep> steps;
    std::vector<DrawRun>  runs;
    void clear() { steps.clear(); runs.clear(); }
    [[nodiscard]] std::size_t passes() const noexcept {
        std::size_t n = 0;
        for (const auto& s : steps) n += s.kind != PassStep::Kind::capture;
        return n;
    }
};

class FramePlanner {
  public:
    /// Highest layer a glyph may take. Past it a glyph draws with the shapes
    /// (the coverage-reshaping approximation): a pathological frame of text
    /// stacked over shapes over text costs at most ~this many passes.
    static constexpr std::uint16_t max_text_layer = 15;

    /// Plan `dl` for a `vw` x `vh` target. `encoded_text` false: no text
    /// passes at all (the device has no UNORM view), which is exactly the
    /// list's own batches in order.
    const FramePlan& plan(const DrawList& dl, std::uint32_t vw, std::uint32_t vh, bool encoded_text) {
        plan_.clear();
        const auto& insts = dl.instances();
        const auto& batches = dl.batches();
        const std::size_t n = insts.size();
        if (n == 0 || batches.empty()) return plan_;

        batch_of_.assign(n, no_batch);
        for (std::uint32_t b = 0; b < batches.size(); ++b)
            for (std::uint32_t k = 0; k < batches[b].count && batches[b].first + k < n; ++k)
                batch_of_[batches[b].first + k] = b;

        // Segments split at each backdrop batch (a backdrop never merges, so
        // its batch is that one instance).
        std::uint32_t seg_begin = 0;
        for (std::uint32_t b = 0; b < batches.size(); ++b) {
            const Batch& bt = batches[b];
            if (bt.count == 0 || static_cast<ShapeKind>(insts[bt.first].kind) != ShapeKind::backdrop) continue;
            plan_segment(dl, seg_begin, bt.first, vw, vh, encoded_text);
            PassStep cap;
            cap.kind = PassStep::Kind::capture;
            cap.capture_instance = bt.first;
            plan_.steps.push_back(cap);
            seg_begin = bt.first;   // the backdrop itself opens the next segment
        }
        plan_segment(dl, seg_begin, static_cast<std::uint32_t>(n), vw, vh, encoded_text);
        return plan_;
    }

  private:
    static constexpr std::uint32_t no_batch = 0xFFFFFFFFu;
    static constexpr int tile = 32;

    struct Box { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

    /// Pixels an instance can touch, conservatively: the quad the vertex
    /// shader emits (1 px antialiasing skirt, a drop shadow's blur reach),
    /// intersected with its batch clip and the target.
    [[nodiscard]] static Box bounds(const Instance& in, const Batch& b, std::uint32_t vw, std::uint32_t vh) noexcept {
        const auto kind = static_cast<ShapeKind>(in.kind);
        float pad = 1.0f;
        if (kind == ShapeKind::shadow && (in.flags & instance_flags::inset) == 0)
            pad = in.params.x * 3.0f + 2.0f;
        if (kind == ShapeKind::backdrop) pad = 1.5f * std::max(in.params.x, 0.0f) + 4.0f;
        Box r{in.rect.x - pad, in.rect.y - pad, in.rect.x + in.rect.z + pad, in.rect.y + in.rect.w + pad};
        const Rect& c = b.clip;
        if (c.size.x < 1e8f && c.size.y < 1e8f && c.size.x > 0.0f && c.size.y > 0.0f) {
            r.x0 = std::max(r.x0, c.left());  r.y0 = std::max(r.y0, c.top());
            r.x1 = std::min(r.x1, c.right()); r.y1 = std::min(r.y1, c.bottom());
        }
        r.x0 = std::max(r.x0, 0.0f); r.y0 = std::max(r.y0, 0.0f);
        r.x1 = std::min(r.x1, static_cast<float>(vw)); r.y1 = std::min(r.y1, static_cast<float>(vh));
        return r;
    }

    [[nodiscard]] static bool is_text(const Instance& in, const Batch& b) noexcept {
        return static_cast<ShapeKind>(in.kind) == ShapeKind::glyph && b.blend == BlendMode::normal &&
               (in.flags & instance_flags::gradient) == 0;
    }

    void plan_segment(const DrawList& dl, std::uint32_t begin, std::uint32_t end,
                      std::uint32_t vw, std::uint32_t vh, bool encoded_text) {
        if (begin >= end) return;
        const auto& insts = dl.instances();
        const auto& batches = dl.batches();

        bool any_text = false;
        if (encoded_text) {
            for (std::uint32_t i = begin; i < end && !any_text; ++i)
                any_text = batch_of_[i] != no_batch && is_text(insts[i], batches[batch_of_[i]]);
        }
        layer_.assign(end - begin, 0);
        std::uint16_t top = 0;

        if (any_text) {
            gw_ = static_cast<int>((vw + tile - 1) / tile);
            gh_ = static_cast<int>((vh + tile - 1) / tile);
            if (grid_.size() < static_cast<std::size_t>(gw_) * gh_) grid_.resize(static_cast<std::size_t>(gw_) * gh_);
            entries_.clear();
            for (std::uint32_t i = begin; i < end; ++i) {
                if (batch_of_[i] == no_batch) continue;
                const Batch& bt = batches[batch_of_[i]];
                const Box r = bounds(insts[i], bt, vw, vh);
                const bool text = is_text(insts[i], bt);
                if (r.x1 <= r.x0 || r.y1 <= r.y0) { layer_[i - begin] = text ? 1 : 0; continue; }
                const int tx0 = static_cast<int>(r.x0) / tile, ty0 = static_cast<int>(r.y0) / tile;
                const int tx1 = std::min(gw_ - 1, static_cast<int>(r.x1) / tile);
                const int ty1 = std::min(gh_ - 1, static_cast<int>(r.y1) / tile);

                // Lowest layer above everything earlier it overlaps. An
                // entry's class is its layer's parity, so "same class: its
                // layer, other class: one above" always has i's parity.
                const auto place = [&](bool as_text) {
                    std::uint16_t need = as_text ? 1 : 0;
                    for (int ty = ty0; ty <= ty1; ++ty)
                        for (int tx = tx0; tx <= tx1; ++tx)
                            for (std::uint32_t e : grid_[static_cast<std::size_t>(ty) * gw_ + tx]) {
                                const Entry& en = entries_[e];
                                if (en.b.x0 >= r.x1 || en.b.x1 <= r.x0 || en.b.y0 >= r.y1 || en.b.y1 <= r.y0) continue;
                                const bool same = ((en.layer & 1u) != 0) == as_text;
                                need = std::max<std::uint16_t>(need, static_cast<std::uint16_t>(en.layer + (same ? 0 : 1)));
                            }
                    return need;
                };
                std::uint16_t L = place(text);
                if (text && L > max_text_layer) L = place(false);   // draw with the shapes

                layer_[i - begin] = L;
                top = std::max(top, L);
                const auto id = static_cast<std::uint32_t>(entries_.size());
                entries_.push_back(Entry{r, L});
                for (int ty = ty0; ty <= ty1; ++ty)
                    for (int tx = tx0; tx <= tx1; ++tx) {
                        auto& cell = grid_[static_cast<std::size_t>(ty) * gw_ + tx];
                        if (cell.empty()) touched_.push_back(static_cast<std::uint32_t>(ty * gw_ + tx));
                        cell.push_back(id);
                    }
            }
            for (std::uint32_t t : touched_) grid_[t].clear();
            touched_.clear();
        }

        // Emit layers bottom-up, each in list order, as runs of consecutive
        // instances of one batch. Adjacent layers of the same kind (an empty
        // layer between them) share a pass.
        for (std::uint16_t L = 0; L <= top; ++L) {
            const auto kind = (L & 1u) ? PassStep::Kind::text : PassStep::Kind::shapes;
            const auto runs_begin = static_cast<std::uint32_t>(plan_.runs.size());
            for (std::uint32_t i = begin; i < end; ++i) {
                if (layer_[i - begin] != L || batch_of_[i] == no_batch) continue;
                if (plan_.runs.size() > runs_begin) {
                    DrawRun& last = plan_.runs.back();
                    if (last.batch == batch_of_[i] && last.first + last.count == i) { ++last.count; continue; }
                }
                plan_.runs.push_back(DrawRun{i, 1, batch_of_[i]});
            }
            const auto runs_end = static_cast<std::uint32_t>(plan_.runs.size());
            if (runs_end == runs_begin) continue;
            if (!plan_.steps.empty() && plan_.steps.back().kind == kind &&
                plan_.steps.back().runs_end == runs_begin) {
                plan_.steps.back().runs_end = runs_end;
            } else {
                PassStep s;
                s.kind = kind; s.runs_begin = runs_begin; s.runs_end = runs_end;
                plan_.steps.push_back(s);
            }
        }
    }

    struct Entry { Box b; std::uint16_t layer; };

    FramePlan plan_;
    std::vector<std::uint32_t> batch_of_;
    std::vector<std::uint16_t> layer_;
    std::vector<Entry> entries_;
    std::vector<std::vector<std::uint32_t>> grid_;
    std::vector<std::uint32_t> touched_;
    int gw_ = 0, gh_ = 0;
};

}  // namespace mayag::render
