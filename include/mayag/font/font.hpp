#pragma once
// mayag::typo::Face / FontStack — the public font API
//
// `Face`      one font file, one style. Owns its bytes, its outline source,
//             its kerning table, and a glyph cache.
// `FontStack` an ordered chain of faces plus a shared atlas. Shapes text by
//             asking each face in turn, so a string mixing Latin, CJK, and
//             emoji renders correctly from three different files.
//
// This is also where the engine meets the rest of mayag: `FontStack` supplies
// a `layout::TextMeasurer` (so flexbox can size text), a
// `render::GlyphRenderer` (so the painter can emit glyph quads), and a
// `backend::CoverageSampler` (so the software rasteriser can read the atlas).
// Those three interfaces already existed; the font engine just implements them.

#include "atlas.hpp"
#include "color_bitmap.hpp"
#include "colr.hpp"
#include "mapped_file.hpp"
#include "opentype.hpp"
#include "outline.hpp"
#include "raster.hpp"
#include "shape.hpp"

#include "../layout/text_metrics.hpp"
#include "../render/painter.hpp"
#include "../backend/software.hpp"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mayag::typo {

// ════════════════════════════════════════════════════════════════════════
// Metrics
// ════════════════════════════════════════════════════════════════════════

/// Vertical metrics in pixels at a given size.
struct FaceMetrics {
    float ascent      = 0.0f;   ///< above the baseline, positive
    float descent     = 0.0f;   ///< below the baseline, positive
    float line_gap    = 0.0f;
    float line_height = 0.0f;   ///< ascent + descent + gap
    float x_height    = 0.0f;
    float cap_height  = 0.0f;
    float underline_position  = 0.0f;
    float underline_thickness = 0.0f;
};

// ════════════════════════════════════════════════════════════════════════
// Face
// ════════════════════════════════════════════════════════════════════════

/// One font file at one style. Immutable after construction except for its
/// glyph cache, which is guarded so a face can be shared across threads.
class Face {
  public:
    /// Load from memory. The bytes are COPIED, because a face outlives the
    /// caller's buffer more often than not and a dangling span here would be
    /// a use-after-free inside the rasteriser.
    [[nodiscard]] static std::shared_ptr<Face> from_memory(std::vector<std::uint8_t> data,
                                                           std::uint32_t face_index = 0,
                                                           ot::Error* err = nullptr) {
        auto face = std::shared_ptr<Face>(new Face);
        face->data_ = std::move(data);
        return face->init(face->data_, face_index, err) ? face : nullptr;
    }

    /// Load from a file, mapped: pages fault in only where the parser and
    /// rasteriser read, and the kernel shares them across processes.
    [[nodiscard]] static std::shared_ptr<Face> from_file(const std::string& path,
                                                         std::uint32_t face_index = 0,
                                                         ot::Error* err = nullptr) {
        auto face = std::shared_ptr<Face>(new Face);
        face->mapped_ = MappedFile{path};
        if (face->mapped_.empty()) {
            if (err != nullptr) *err = ot::Error::too_small;
            return nullptr;
        }
        return face->init(face->mapped_.data(), face_index, err) ? face : nullptr;
    }

  private:
    bool init(std::span<const std::uint8_t> bytes, std::uint32_t face_index, ot::Error* err) {
        Face* face = this;
        auto parsed = ot::FontFile::parse(bytes, face_index, err);
        if (!parsed) return false;

        face->file_    = std::move(*parsed);
        face->glyphs_  = GlyphSource{face->file_};
        face->kerning_ = KernTable{face->file_};
        face->ligatures_ = LigatureTable{face->file_};
        face->id_      = next_id();
        face->base_id_ = face->id_;

        // ── variable-font setup ──────────────────────────────────────────
        // Parse the fvar axis table and start the face at every axis's
        // default, so an untouched variable font renders exactly like its
        // default instance. set_variation() later moves along the axes.
        face->axes_ = var::parse_fvar(face->file_.table(ot::tags::fvar));
        face->avar_ = face->file_.table(ot::tags::avar);
        if (!face->axes_.empty()) {
            // Empty user list => every axis normalises to its default (0.0).
            face->coords_ = var::normalize_all(face->axes_, face->avar_, {});
            face->user_.reserve(face->axes_.size());
            for (const auto& ax : face->axes_) face->user_.push_back(ax.def);
            face->hvar_ = var::parse_hvar(face->file_.table(ot::tags::hvar));
            face->glyphs_.set_coords(&face->coords_);
        }

        // ── vector colour glyphs (COLR/CPAL) ─────────────────────────────
        face->colr_ = colr::parse_colr(face->file_.table(ot::tags::colr));
        face->cpal_ = colr::parse_cpal(face->file_.table(ot::tags::cpal));

        // A face is usable if it can place glyphs at all. Vector faces need a
        // working glyph source; colour/bitmap faces (emoji, icons) have none
        // and render blank-but-spaced here — but they still carry a real cmap,
        // so they belong in the fallback chain for coverage detection rather
        // than being dropped, which is exactly what turned emoji into tofu.
        if (!face->glyphs_.valid() && !face->file_.has_color()) {
            if (err != nullptr) *err = ot::Error::unsupported_format;
            return false;
        }
        return true;
    }

  public:

    // ── identity ────────────────────────────────────────────────────────

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& family() const noexcept { return file_.family(); }
    [[nodiscard]] const std::string& subfamily() const noexcept { return file_.subfamily(); }
    [[nodiscard]] std::uint16_t weight() const noexcept { return file_.weight(); }
    [[nodiscard]] bool is_italic() const noexcept { return file_.is_italic(); }
    [[nodiscard]] bool is_bold() const noexcept { return file_.is_bold(); }

    /// True when the face has vector outlines that this engine can rasterise.
    /// A colour/bitmap-only face (emoji) is false here: it is measurable and
    /// keeps text from collapsing, but its glyphs draw blank.
    [[nodiscard]] bool has_outlines() const noexcept { return file_.has_outlines(); }
    /// True when the face carries colour or bitmap glyphs.
    [[nodiscard]] bool is_color() const noexcept { return file_.has_color(); }
    [[nodiscard]] std::uint16_t num_glyphs() const noexcept { return file_.num_glyphs(); }
    [[nodiscard]] std::uint16_t units_per_em() const noexcept { return file_.units_per_em(); }
    [[nodiscard]] const ot::FontFile& file() const noexcept { return file_; }

    /// Pixels per font unit at `size_px`.
    [[nodiscard]] float scale_for(float size_px) const noexcept {
        return size_px / static_cast<float>(file_.units_per_em());
    }

    [[nodiscard]] bool has_glyph(std::uint32_t codepoint) const noexcept {
        return file_.glyph_for(codepoint) != 0;
    }
    [[nodiscard]] std::uint16_t glyph_for(std::uint32_t codepoint) const noexcept {
        return file_.glyph_for(codepoint);
    }

    // ── metrics ─────────────────────────────────────────────────────────

    /// Vertical metrics at `size_px`.
    ///
    /// Which fields a font's line height should come from is a genuine mess:
    /// `hhea` is what Apple uses, `OS/2.usWin*` is what old Windows used, and
    /// `OS/2.sTypo*` is what the spec says — but only when the
    /// USE_TYPO_METRICS bit is set. Following that rule exactly is why mayag's
    /// line spacing matches the browser's.
    [[nodiscard]] FaceMetrics metrics(float size_px) const noexcept {
        const float s = scale_for(size_px);
        const auto& os2 = file_.os2();
        const auto& hhea = file_.hhea();

        FaceMetrics m;
        if (os2.present && os2.use_typo_metrics() && os2.typo_ascender != 0) {
            m.ascent   =  static_cast<float>(os2.typo_ascender)  * s;
            m.descent  = -static_cast<float>(os2.typo_descender) * s;
            m.line_gap =  static_cast<float>(os2.typo_line_gap)  * s;
        } else if (hhea.ascender != 0) {
            m.ascent   =  static_cast<float>(hhea.ascender)  * s;
            m.descent  = -static_cast<float>(hhea.descender) * s;
            m.line_gap =  static_cast<float>(hhea.line_gap)  * s;
        } else if (os2.present) {
            m.ascent  = static_cast<float>(os2.win_ascent)  * s;
            m.descent = static_cast<float>(os2.win_descent) * s;
        }

        // A font with zero metrics would collapse every line onto the same
        // baseline; fall back to a conventional 80/20 split of the em.
        if (m.ascent + m.descent <= 0.0f) {
            m.ascent  = size_px * 0.8f;
            m.descent = size_px * 0.2f;
        }

        m.line_height = m.ascent + m.descent + m.line_gap;
        m.x_height    = os2.x_height   != 0 ? static_cast<float>(os2.x_height)   * s : m.ascent * 0.52f;
        m.cap_height  = os2.cap_height != 0 ? static_cast<float>(os2.cap_height) * s : m.ascent * 0.72f;
        m.underline_position  = -m.descent * 0.5f;
        m.underline_thickness = num::max(size_px * 0.06f, 1.0f);
        return m;
    }

    /// Advance width of a glyph in pixels. For a variable font this includes
    /// the HVAR advance-width delta at the current axis position, so heavy and
    /// light instances space text correctly rather than reusing the default.
    [[nodiscard]] float advance(std::uint16_t gid, float size_px) const noexcept {
        float units = static_cast<float>(file_.advance(gid));
        if (hvar_.ok) {
            std::lock_guard lock{mutex_};
            units += var::hvar_advance_delta(hvar_, gid, coords_);
        }
        return units * scale_for(size_px);
    }

    /// Kerning between two glyphs in pixels.
    [[nodiscard]] float kern(std::uint16_t left, std::uint16_t right, float size_px) const noexcept {
        return kerning_.lookup(left, right) * scale_for(size_px);
    }

    [[nodiscard]] bool has_kerning() const noexcept { return !kerning_.empty(); }

    /// Whether this face carries GSUB ligatures we can apply.
    [[nodiscard]] bool has_ligatures() const noexcept { return !ligatures_.empty(); }

    /// Longest ligature substitution starting at gids[i]; 0 if none. On a hit
    /// `consumed` is how many glyphs the returned ligature glyph replaces.
    [[nodiscard]] std::uint16_t ligature_at(const std::vector<std::uint16_t>& gids,
                                            std::size_t i, std::size_t& consumed) const {
        return ligatures_.match(gids, i, consumed);
    }

    // ── outlines ────────────────────────────────────────────────────────

    [[nodiscard]] Outline outline(std::uint16_t gid) const {
        std::lock_guard lock{mutex_};
        auto it = outline_cache_.find(gid);
        if (it != outline_cache_.end()) return it->second;

        Outline o = glyphs_.load(gid);
        // Bound the cache: a CJK face has 20k glyphs and caching every
        // outline forever would dwarf the atlas it feeds.
        if (outline_cache_.size() < 2048) outline_cache_.emplace(gid, o);
        return o;
    }

    // VARIATION_API_ANCHOR

    // == vector colour glyphs (COLR) ==
    /// True when `gid` has a COLR (layered/vector) colour representation.
    [[nodiscard]] bool has_colr(std::uint16_t gid) const noexcept {
        return colr::has_color_glyph(colr_, gid);
    }

    /// Render a COLR colour glyph to a straight-RGBA bitmap at `size_px`. The
    /// `text_color` fills palette index 0xFFFF ("use foreground"). Returns an
    /// invalid ColorBitmap if `gid` has no usable COLR glyph.
    [[nodiscard]] ColorBitmap render_colr(std::uint16_t gid, float size_px,
                                          colr::Rgba text_color) const {
        colr::RenderContext ctx;
        ctx.palette = cpal_.ok() ? &cpal_ : nullptr;
        ctx.text_color = text_color;
        ctx.upem = units_per_em();
        ctx.outline_of = [this](std::uint16_t g) { return glyphs_.load(g); };
        return colr::render_glyph(colr_, gid, size_px, ctx);
    }

    /// True when this face carries fvar axes we can drive.
    [[nodiscard]] bool is_variable() const noexcept { return !axes_.empty(); }

    /// The design axes (tag + min/def/max in user units); empty if static.
    [[nodiscard]] const std::vector<var::Axis>& axes() const noexcept { return axes_; }

    /// True when this face can express italic through its OWN design space (a
    /// slnt or ital axis). If false, `italic` must be synthesised by shearing.
    [[nodiscard]] bool has_slant_axis() const noexcept {
        for (const auto& a : axes_)
            if (a.tag == var::tag_slnt || a.tag == var::tag_ital) return true;
        return false;
    }

    /// True when the face can actually reach USER weight `w` on its own wght
    /// axis. A static font (no wght axis) never can; a variable font can only
    /// up to its axis max. When false, heavier text must be synthesised by
    /// emboldening the coverage.
    [[nodiscard]] bool can_reach_weight(float w) const noexcept {
        for (const auto& a : axes_)
            if (a.tag == var::tag_wght) return w <= a.max + 1.0f;
        // A static face reaches its own weight (Menlo Bold IS weight 700):
        // emboldening a real Bold again is what produced smeared headings.
        return w <= static_cast<float>(weight()) + 150.0f;
    }

    /// Current value of one axis in USER units, or NaN if the tag is unknown.
    [[nodiscard]] float variation(std::uint32_t tag) const noexcept {
        for (std::size_t i = 0; i < axes_.size(); ++i)
            if (axes_[i].tag == tag)
                return i < user_.size() ? user_[i] : axes_[i].def;
        return std::numeric_limits<float>::quiet_NaN();
    }

    /// Move several axes at once (USER units, e.g. {{tag_wght, 700}}). Axes
    /// not listed keep their current value; unknown tags are ignored.
    void set_variations(std::span<const std::pair<std::uint32_t, float>> vals) {
        if (axes_.empty()) return;
        apply_user_coords(vals);
    }

    /// Move one axis to `value` (USER units), leaving the others put.
    void set_variation(std::uint32_t tag, float value) {
        const std::pair<std::uint32_t, float> one{tag, value};
        set_variations({&one, 1});
    }

    /// Snap back to the default instance (every axis at its fvar default).
    void reset_variations() {
        std::vector<std::pair<std::uint32_t, float>> defs;
        defs.reserve(axes_.size());
        for (const auto& a : axes_) defs.emplace_back(a.tag, a.def);
        set_variations(defs);
    }


  private:
    Face() = default;

    // Re-derive the normalised coords from a set of user-unit axis values,
    // then invalidate everything downstream that baked in the old instance.
    void apply_user_coords(std::span<const std::pair<std::uint32_t, float>> vals) {
        // Seed from the current per-axis user values (defaults on first call)
        // so axes the caller doesn't mention keep exactly where they were.
        std::vector<float> next_user(axes_.size());
        std::vector<std::pair<std::uint32_t, float>> user;
        user.reserve(axes_.size());
        for (std::size_t i = 0; i < axes_.size(); ++i) {
            const var::Axis& ax = axes_[i];
            float u = (i < user_.size()) ? user_[i] : ax.def;
            for (const auto& [tag, v] : vals)
                if (tag == ax.tag) u = v;
            u = std::clamp(u, ax.min, ax.max);
            next_user[i] = u;
            user.emplace_back(ax.tag, u);
        }
        var::Coords next = var::normalize_all(axes_, avar_, user);

        std::lock_guard lock{mutex_};
        if (next == coords_) return;   // no-op move: geometry unchanged
        coords_ = std::move(next);
        user_   = std::move(next_user);
        glyphs_.set_coords(&coords_);
        outline_cache_.clear();
        // Derive the id from the position, not a running counter, so returning
        // to a weight we've drawn before reuses its atlas entries instead of
        // re-rasterising every glyph again.
        id_ = variation_id(base_id_, coords_);
    }

    // Stable id for a base face at a given normalised position. FNV-1a over
    // the coords, mixed with the base id; collisions across different faces
    // are astronomically unlikely and only cost a redundant re-raster.
    [[nodiscard]] static std::uint32_t variation_id(std::uint32_t base,
                                                    const var::Coords& c) noexcept {
        std::uint32_t h = 2166136261u ^ base;
        for (float v : c) {
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(v));
            std::memcpy(&bits, &v, sizeof(bits));
            h = (h ^ bits) * 16777619u;
        }
        // Keep it out of the low range next_id() hands out for base faces so a
        // varied instance never aliases a different base face's id.
        return h | 0x80000000u;
    }

    [[nodiscard]] static std::uint32_t next_id() {
        static std::atomic<std::uint32_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::uint8_t> data_;     ///< owned bytes (from_memory)
    MappedFile                mapped_;   ///< or a mapping (from_file)
    ot::FontFile              file_;
    GlyphSource               glyphs_;
    KernTable                 kerning_;
    LigatureTable             ligatures_;
    std::uint32_t             id_ = 0;
    std::uint32_t             base_id_ = 0;   // stable id ignoring variation

    // ── variable-font state ─────────────────────────────────────────────
    std::vector<var::Axis>        axes_;      // fvar axes (empty ⇒ static font)
    std::span<const std::uint8_t> avar_{};    // optional axis remap table
    var::Coords                   coords_;    // current normalised position
    std::vector<float>            user_;      // current value per axis, USER units
    var::HvarTable                hvar_;      // advance-width variations (optional)

    colr::ColrTable               colr_;      // vector colour glyphs (optional)
    colr::Palette                 cpal_;      // colour palette for COLR

    mutable std::mutex                                 mutex_;
    mutable std::unordered_map<std::uint16_t, Outline> outline_cache_;
};

// ════════════════════════════════════════════════════════════════════════
// Rendering mode
// ════════════════════════════════════════════════════════════════════════

enum class RenderMode : std::uint8_t {
    /// Rasterise per size. Sharpest possible small text, one atlas entry per
    /// (glyph, size). The right choice for body copy.
    bitmap,
    /// One scale-free distance field per glyph, shaded by the same SDF kernel
    /// as every other mayag primitive. Slightly softer at tiny sizes, but a
    /// heading and a caption share one entry and text batches with everything
    /// else into a single draw call.
    sdf,
    /// SDF above a threshold, bitmap below it. The default: crisp where
    /// crispness is visible, scale-free where it pays.
    hybrid,
};

struct FontConfig {
    RenderMode mode = RenderMode::hybrid;

    /// DEVICE pixel size at or above which hybrid mode switches to SDF.
    ///
    /// Set high on purpose. A distance field is resampled by construction,
    /// so it is never as sharp as a bitmap rasterised at the exact size it
    /// is drawn at — it wins only when one entry has to serve many sizes,
    /// i.e. large display type. At UI sizes the sharper bitmap is worth the
    /// extra atlas entries.
    ///
    /// This is compared against DEVICE pixels (logical size x DPI), so a
    /// 12 pt label is a 24 px bitmap on Retina rather than being pushed
    /// into SDF purely because the display is dense. Making the mode depend
    /// on the monitor was a real bug: the same UI rendered differently on
    /// different screens.
    float      sdf_threshold = 56.0f;
    /// SDF is generated at this pixel size and scaled; larger means better
    /// fidelity at large display sizes and a bigger atlas footprint.
    float      sdf_raster_size = 48.0f;
    float      sdf_spread      = 6.0f;
    int        atlas_size      = 1024;
    /// Glyphs untouched for this many frames become eviction candidates.
    std::uint64_t glyph_ttl_frames = 600;
};

// ════════════════════════════════════════════════════════════════════════
// FontStack
// ════════════════════════════════════════════════════════════════════════

/// An ordered chain of faces sharing one atlas.
///
/// Fallback is per-CODEPOINT, not per-string: a label reading "CPU 温度 🔥"
/// resolves the Latin from the UI font, the CJK from a Han face, and the
/// emoji from the colour font, and they all land in one atlas and one draw
/// call. That is the behaviour users expect and the thing naive font stacks
/// get wrong by picking one face for the whole run.
class FontStack : public Shaper {
  public:
    explicit FontStack(FontConfig cfg = {})
        : config_{cfg}, atlas_{cfg.atlas_size} {}

    /// Append a face. The first added is primary; later ones are fallbacks.
    void add(std::shared_ptr<Face> face) {
        if (face != nullptr) faces_.push_back(std::move(face));
        clear_shape_cache();
    }

    /// Register another weight/style of an already-added face (Menlo Bold for
    /// Menlo Regular). It never takes part in fallback: shaping swaps it in
    /// for `of` when a run asks for a weight closer to its own, which is what
    /// a real typesetter does before it ever reaches for synthetic bold.
    void add_sibling(std::size_t of, std::shared_ptr<Face> face, std::uint16_t weight,
                     bool italic) {
        if (face == nullptr || of >= faces_.size()) return;
        const std::size_t at = faces_.size();
        faces_.push_back(std::move(face));
        siblings_.push_back(Sibling{of, at, weight, italic});
        clear_shape_cache();
    }

    /// The face (`base` or one of its siblings) to draw `base`'s glyphs at
    /// `weight`: the nearest real weight with the right slant, if it beats the
    /// base face. Variable faces reach any weight themselves, so stay put.
    [[nodiscard]] std::size_t face_for_weight(std::size_t base, float weight, bool italic) const noexcept {
        if (siblings_.empty() || base >= faces_.size() || faces_[base]->is_variable()) return base;
        std::size_t best = base;
        float best_d = num::abs(static_cast<float>(faces_[base]->weight()) - weight) +
                       (faces_[base]->is_italic() != italic ? 1000.0f : 0.0f);
        for (const auto& s : siblings_) {
            if (s.of != base) continue;
            const float d = num::abs(static_cast<float>(s.weight) - weight) + (s.italic != italic ? 1000.0f : 0.0f);
            if (d < best_d) { best_d = d; best = s.at; }
        }
        return best;
    }

    [[nodiscard]] bool empty() const noexcept { return faces_.empty(); }
    [[nodiscard]] std::size_t face_count() const noexcept { return faces_.size(); }
    [[nodiscard]] const Face* primary() const noexcept {
        return faces_.empty() ? nullptr : faces_.front().get();
    }
    [[nodiscard]] const Face* face_at(std::size_t i) const noexcept {
        return i < faces_.size() ? faces_[i].get() : nullptr;
    }

    [[nodiscard]] Atlas& atlas() noexcept { return atlas_; }
    [[nodiscard]] const Atlas& atlas() const noexcept { return atlas_; }
    [[nodiscard]] const FontConfig& config() const noexcept { return config_; }

    void begin_frame(std::uint64_t frame) {
        atlas_.set_frame(frame);
        if (frame % 120 == 0) atlas_.collect(config_.glyph_ttl_frames);
        std::lock_guard lock{shape_mutex_};
        shape_frame_ = frame;
        if (frame % 120 == 0) evict_shapes(frame);
    }

    /// Which face can render `codepoint`, and its glyph id.
    [[nodiscard]] std::pair<std::size_t, std::uint16_t>
    resolve(std::uint32_t codepoint) const noexcept {
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            if (!siblings_.empty() && is_sibling(i)) continue;   // siblings stand in for a face, never fall back
            const std::uint16_t g = faces_[i]->glyph_for(codepoint);
            if (g != 0) return {i, g};
        }
        return {0, 0};
    }

    /// Glyph id for the ellipsis (…) in the primary face, or '.' as a fallback
    /// when the font has no U+2026. Used by ellipsis text truncation.
    [[nodiscard]] std::uint16_t ellipsis_glyph() const noexcept {
        if (faces_.empty()) return 0;
        const Face& f = *faces_.front();
        if (const std::uint16_t g = f.glyph_for(0x2026); g != 0) return g;
        return f.glyph_for('.');
    }

    [[nodiscard]] FaceMetrics metrics(float size_px) const {
        return faces_.empty() ? FaceMetrics{} : faces_.front()->metrics(size_px);
    }

    // ── shaping ─────────────────────────────────────────────────────────────────

    using ShapedRun = std::shared_ptr<const ShapeResult>;

    /// Shape through the cache. Layout and paint shape the same strings every
    /// frame, so a hit skips cmap, advance, kerning and ligature work. The
    /// result is immutable and shared, so a hit doesn't copy glyphs.
    [[nodiscard]] ShapedRun shaped(std::string_view text, const ShapeParams& params) const {
        const std::uint64_t h = shape_hash(text, params);
        {
            std::lock_guard lock{shape_mutex_};
            if (auto it = shapes_.find(ShapeProbe{h, text, &params}); it != shapes_.end()) {
                it->second.last_used = shape_frame_;
                return it->second.run;
            }
        }
        auto fresh = shape_uncached(text, params);
        // The unwrapped extent, computed once here instead of per measure.
        {
            float widest = 0.0f, cur = 0.0f;
            int lines = 1;
            for (const auto& g : fresh.glyphs) {
                if (g.cluster < text.size() && (text[g.cluster] == '\n' || text[g.cluster] == '\r')) { widest = num::max(widest, cur); cur = 0.0f; ++lines; }
                else cur += g.advance;
            }
            fresh.widest_line = num::max(widest, cur);
            fresh.line_count = lines;
        }
        auto run = std::make_shared<const ShapeResult>(std::move(fresh));
        std::lock_guard lock{shape_mutex_};
        if (shapes_.size() >= kMaxShapes) evict_shapes(shape_frame_ + 1);
        shapes_.try_emplace(ShapeKey{h, std::string{text}, params}, ShapeEntry{run, shape_frame_});
        return run;
    }

    [[nodiscard]] ShapeResult shape(std::string_view text,
                                    const ShapeParams& params) const override {
        if (faces_.empty() || text.empty()) return {};
        return *shaped(text, params);
    }

    void clear_shape_cache() {
        std::lock_guard lock{shape_mutex_};
        shapes_.clear();
    }
    [[nodiscard]] std::size_t shape_cache_size() const {
        std::lock_guard lock{shape_mutex_};
        return shapes_.size();
    }

    [[nodiscard]] ShapeResult shape_uncached(std::string_view text,
                                             const ShapeParams& params) const {
        ShapeResult out;
        if (faces_.empty() || text.empty()) return out;

        out.glyphs.reserve(text.size());

        std::size_t   i = 0;
        std::size_t   prev_face = 0;
        std::uint16_t prev_gid  = 0;
        bool          have_prev = false;

        while (i < text.size()) {
            const auto cluster = static_cast<std::uint32_t>(i);
            const std::uint32_t cp = utf8::decode(text, i);

            if (uni::is_rtl(cp))                out.has_rtl = true;
            if (uni::needs_complex_shaping(cp)) out.needs_complex = true;

            // Newlines are layout's business, not the shaper's; emit a
            // zero-width glyph so cluster indices stay aligned with the
            // source string.
            if (cp == '\n' || cp == '\r') {
                out.glyphs.push_back(ShapedGlyph{0, cluster, 0.0f, {}, 0, false});
                have_prev = false;
                continue;
            }

            auto [face_idx, gid] = resolve(cp);
            if (gid != 0) {
                // A real Bold beats an emboldened Regular. Same family, so the
                // codepoint maps through the sibling's own cmap.
                if (const std::size_t w = face_for_weight(face_idx, params.weight, params.italic); w != face_idx) {
                    if (const std::uint16_t g2 = faces_[w]->glyph_for(cp); g2 != 0) { face_idx = w; gid = g2; }
                }
            }
            const bool missing = (gid == 0);
            const Face& face = *faces_[face_idx];
            pin_variations(faces_[face_idx].get(), params.weight,
                           {params.axes.data(), params.axis_count}, params.italic);

            ShapedGlyph g;
            g.glyph_id   = gid;
            g.cluster    = cluster;
            g.face_index = static_cast<std::uint8_t>(num::min<std::size_t>(face_idx, 255));
            g.missing    = missing;
            g.advance    = face.advance(gid, params.size_px);

            // Combining marks have zero advance and stack on the base glyph.
            if (uni::is_combining_mark(cp) || uni::is_variation_selector(cp) ||
                uni::is_zwj(cp)) {
                g.advance = 0.0f;
                // Inherit the previous cluster so the pair is one cursor stop.
                if (!out.glyphs.empty()) g.cluster = out.glyphs.back().cluster;
            }

            // Kerning applies only WITHIN a face — kern pairs are glyph ids,
            // and a glyph id from one font means nothing in another.
            if (params.kerning && have_prev && face_idx == prev_face && !missing) {
                const float k = face.kern(prev_gid, gid, params.size_px);
                if (k != 0.0f && !out.glyphs.empty()) {
                    out.glyphs.back().advance += k;
                    out.width += k;
                }
            }

            g.advance += params.letter_spacing;
            out.width += g.advance;
            out.glyphs.push_back(g);

            prev_face = face_idx;
            prev_gid  = gid;
            have_prev = true;
        }

        if (params.ligatures) apply_ligatures(out, params);

        return out;
    }

    // ── glyph rasterisation and caching ─────────────────────────────────

    /// Fetch (rasterising on demand) the atlas entry for a glyph. `weight`
    /// drives the wght axis; `axes` carries any further variable-font axis
    /// overrides (opsz, wdth, ...) so the raster matches the shaped run.
    [[nodiscard]] const CachedGlyph* glyph(std::size_t face_index, std::uint16_t gid,
                                           float size_px, float weight = 400.0f,
                                           std::span<const ShapeParams::Axis> axes = {},
                                           bool italic = false) {
        if (face_index >= faces_.size()) return nullptr;
        const Face& face = *faces_[face_index];
        pin_variations(faces_[face_index].get(), weight, axes, italic);

        const bool use_sdf = should_use_sdf(size_px);

        GlyphKey key;
        key.face_id  = face.id();
        key.glyph_id = gid;
        // The payoff of SDF: every size collapses to one bucket, so a page
        // with ten type sizes rasterises each glyph once instead of ten times.
        // SDF entries are scale-free, so every size collapses to one bucket.
        // Bitmap entries are rasterised for the size they are drawn at, and
        // quantising to QUARTER pixels keeps the cache from exploding while
        // staying far finer than the eye can resolve.
        key.size_bucket = use_sdf
            ? 0
            : static_cast<std::uint16_t>(num::clamp(num::round(size_px * 4.0f),
                                                    1.0f, 65000.0f));
        // Synthesise italic by shearing when the font can't slant itself
        // (static faces, or variable faces with no slnt/ital axis). Variable
        // faces WITH a slant axis already did it for real in pin_variations,
        // so we must not shear those on top.
        const bool synth_italic = italic && !face.has_slant_axis();

        // Synthesise bold by emboldening the coverage when the font can't reach
        // the requested weight itself — a static face (no wght axis) asked for
        // >= semibold, or a variable face past its wght-axis max. Variable
        // faces that CAN reach the weight already did it for real via the axis.
        const bool synth_bold = weight >= 600.0f && !face.can_reach_weight(weight);

        key.flags = use_sdf ? glyph_flags::sdf : glyph_flags::none;
        if (synth_italic) key.flags |= glyph_flags::synthetic_italic;
        if (synth_bold)   key.flags |= glyph_flags::synthetic_bold;

        if (const CachedGlyph* hit = atlas_.find(key)) return hit;

        Outline o = face.outline(gid);
        if (synth_italic) {
            // Shear ~12deg: x' = x + shear * y (font units, y-up), pivoting on
            // the baseline so ascenders lean right and descenders left — the
            // classic faux-oblique every browser falls back to.
            constexpr float shear = 0.2125f;   // tan(12deg)
            o.transform(Affine{1.0f, 0.0f, shear, 1.0f, 0.0f, 0.0f});
        }
        const float advance = face.advance(gid, size_px);

        // Colour glyph (emoji): the face has no outline, but a CBDT/sbix
        // strike. Decode it, scale the strike to the requested size, and pack
        // it into the atlas's RGBA plane. Marked is_color so the renderer
        // emits an untinted colour-glyph quad.
        if (face.has_colr(gid)) {
            // Vector colour glyph: render at native size (crisp at any scale),
            // no strike rescale. Palette index 0xFFFF resolves to black here;
            // the emit path tints colour glyphs by alpha only, so real palette
            // colours pass through untouched.
            const ColorBitmap cb = face.render_colr(gid, size_px, colr::Rgba{0, 0, 0, 255});
            if (cb.valid()) {
                // bearing_y is px ABOVE the baseline (positive up); the emit
                // path expects y-down, so negate it like the CBDT path.
                const Vec2 bearing{cb.bearing_x, -cb.bearing_y};
                return atlas_.insert_color(key, cb.rgba.data(), cb.width, cb.height,
                                           bearing, advance);
            }
            // Empty COLR render (e.g. all-empty layers) — fall through.
        }

        if (face.is_color()) {
            ColorBitmap cb = color_bitmap_for(face.file(), gid);
            if (cb.valid()) {
                // The strike is authored at cb.ppem px/em; scale it so the em
                // matches the requested size_px.
                const float scale = cb.ppem > 0 ? size_px / static_cast<float>(cb.ppem) : 1.0f;
                const int dw = std::max(1, static_cast<int>(static_cast<float>(cb.width) * scale + 0.5f));
                const int dh = std::max(1, static_cast<int>(static_cast<float>(cb.height) * scale + 0.5f));
                std::vector<std::uint8_t> scaled;
                const std::uint8_t* pixels = cb.rgba.data();
                if (dw != cb.width || dh != cb.height) {
                    scaled = scale_rgba(cb.rgba, cb.width, cb.height, dw, dh);
                    pixels = scaled.data();
                }
                // Emoji sit on the baseline. CBDT bearingY is the height of
                // the bitmap ABOVE the baseline (positive up); mayag's dst is
                // pen.y + bearing.y with y growing DOWN, so the top of the
                // bitmap is at -bearingY from the pen. bearingX is a normal
                // left offset.
                const Vec2 bearing{cb.bearing_x * scale, -cb.bearing_y * scale};
                return atlas_.insert_color(key, pixels, dw, dh, bearing, advance);
            }
            // No usable strike — cache a blank so we stop retrying.
            return atlas_.insert(key, Bitmap{}, Vec2{}, advance, 1.0f, 0.0f, false);
        }

        if (o.empty()) {
            // Blank (space) — cache the metrics so we stop re-checking.
            return atlas_.insert(key, Bitmap{}, Vec2{}, advance, 1.0f, 0.0f, use_sdf);
        }

        if (use_sdf) {
            const float raster_scale = face.scale_for(config_.sdf_raster_size);
            // Faux-bold in the SDF path: thicken the field's source coverage by
            // ~3% of the SDF raster size, matching the coverage path so bold
            // looks the same on both sides of the SDF threshold.
            const float embolden_px = synth_bold
                ? num::max(config_.sdf_raster_size * 0.03f, 1.0f) : 0.0f;
            auto r = rasterize_sdf(o, raster_scale, config_.sdf_spread, 2, embolden_px);
            return atlas_.insert(key, r.bitmap, r.offset, advance,
                                 raster_scale, config_.sdf_spread, true);
        }

        // Rasterise at the size the KEY represents, not the raw request, so
        // two calls that share a cache slot also share identical pixels.
        const float quantised = static_cast<float>(key.size_bucket) * 0.25f;
        const float scale = face.scale_for(quantised);
        auto r = rasterize(o, scale, 1);
        if (synth_bold) {
            // Emboldening grows the bitmap by `pad` on every side; keep the
            // glyph visually centred by shifting the offset back by the same.
            const float strength = num::max(quantised * 0.03f, 0.6f);   // ~stem +6%
            const int pad = static_cast<int>(num::ceil(strength));
            r.bitmap = embolden(r.bitmap, strength);
            r.offset -= Vec2{static_cast<float>(pad), static_cast<float>(pad)};
        }
        return atlas_.insert(key, r.bitmap, r.offset, advance, scale, 0.0f, false);
    }

    // Greedy longest-match ligature substitution over the shaped run. Merges
    // a component sequence (same face, contiguous) into one ligature glyph,
    // keeping the first component's cluster so cursor hit-testing still maps
    // the ligature back to its start. Advance becomes the ligature glyph's own
    // advance at the run's variation. Runs per face so mixed-font text is safe.
    void apply_ligatures(ShapeResult& out, const ShapeParams& params) const {
        bool any = false;
        for (const auto& g : out.glyphs)
            if (g.face_index < faces_.size() && faces_[g.face_index]->has_ligatures()) {
                any = true; break;
            }
        if (!any) return;

        std::vector<ShapedGlyph> merged;
        merged.reserve(out.glyphs.size());
        std::vector<std::uint16_t> gids;
        gids.reserve(out.glyphs.size());
        for (const auto& g : out.glyphs) gids.push_back(g.glyph_id);

        for (std::size_t i = 0; i < out.glyphs.size();) {
            const ShapedGlyph& g = out.glyphs[i];
            const std::size_t fi = g.face_index;
            std::size_t consumed = 0;
            std::uint16_t lig = 0;
            if (fi < faces_.size() && faces_[fi]->has_ligatures()) {
                // Only fold a run whose every component shares this face.
                lig = faces_[fi]->ligature_at(gids, i, consumed);
                if (lig != 0) {
                    for (std::size_t k = 1; k < consumed; ++k)
                        if (i + k >= out.glyphs.size() || out.glyphs[i + k].face_index != fi) {
                            lig = 0; break;
                        }
                }
            }
            if (lig != 0 && consumed >= 2) {
                ShapedGlyph m = g;                 // keep first component's cluster/offset
                m.glyph_id = lig;
                m.advance  = faces_[fi]->advance(lig, params.size_px) + params.letter_spacing;
                merged.push_back(m);
                i += consumed;
            } else {
                merged.push_back(g);
                ++i;
            }
        }

        out.glyphs = std::move(merged);
        out.width = 0.0f;
        for (const auto& g : out.glyphs) out.width += g.advance;
    }

    // Pin a variable face to a run's axis position before we shape or
    // rasterise with it: the wght axis plus any extra overrides (opsz, wdth,
    // custom) and, when `italic` is requested, the font's own slant — slnt
    // (degrees, use its min = most slanted) or ital (0..1, use 1). A no-op for
    // static faces. Face guards its own variation state and set_variations
    // early-outs when nothing moves, so this is cheap per run and safe.
    static void pin_variations(Face* face, float weight,
                               std::span<const ShapeParams::Axis> axes,
                               bool italic = false) {
        if (face == nullptr || !face->is_variable()) return;
        std::array<std::pair<std::uint32_t, float>, ShapeParams::max_axes + 2> vals;
        std::size_t n = 0;
        vals[n++] = {var::tag_wght, weight};

        // Real oblique from the font when italic is asked for and the face has
        // a slant/italic axis (true masters beat any synthetic shear).
        if (italic) {
            for (const auto& ax : face->axes()) {
                if (ax.tag == var::tag_slnt && n < vals.size()) {
                    vals[n++] = {var::tag_slnt, ax.min};   // most-slanted extreme
                    break;
                }
                if (ax.tag == var::tag_ital && n < vals.size()) {
                    vals[n++] = {var::tag_ital, ax.max};   // fully italic
                    break;
                }
            }
        }

        for (const auto& a : axes) {
            if (n >= vals.size()) break;
            bool merged = false;
            for (std::size_t j = 0; j < n; ++j)
                if (vals[j].first == a.tag) { vals[j].second = a.value; merged = true; break; }
            if (!merged) vals[n++] = {a.tag, a.value};
        }
        face->set_variations({vals.data(), n});
    }

    [[nodiscard]] bool should_use_sdf(float size_px) const noexcept {
        switch (config_.mode) {
            case RenderMode::bitmap: return false;
            case RenderMode::sdf:    return true;
            case RenderMode::hybrid: return size_px >= config_.sdf_threshold;
        }
        return false;
    }

  private:
    FontConfig                          config_;
    Atlas                               atlas_;
    std::vector<std::shared_ptr<Face>>  faces_;

    // Shape cache: keyed by text + every ShapeParams field, aged by frame.
    struct ShapeKey {
        std::uint64_t hash;
        std::string   text;
        ShapeParams   params;
    };
    struct ShapeProbe {
        std::uint64_t      hash;
        std::string_view   text;
        const ShapeParams* params;
    };
    struct ShapeEntry {
        ShapedRun     run;
        std::uint64_t last_used = 0;
    };
    static bool same_params(const ShapeParams& a, const ShapeParams& b) noexcept {
        if (a.size_px != b.size_px || a.letter_spacing != b.letter_spacing || a.kerning != b.kerning ||
            a.ligatures != b.ligatures || a.weight != b.weight || a.italic != b.italic ||
            a.axis_count != b.axis_count)
            return false;
        for (std::uint8_t i = 0; i < a.axis_count; ++i)
            if (a.axes[i].tag != b.axes[i].tag || a.axes[i].value != b.axes[i].value) return false;
        return true;
    }
    struct ShapeHash {
        using is_transparent = void;
        std::size_t operator()(const ShapeKey& k) const noexcept { return k.hash; }
        std::size_t operator()(const ShapeProbe& k) const noexcept { return k.hash; }
    };
    struct ShapeEq {
        using is_transparent = void;
        bool operator()(const ShapeKey& a, const ShapeKey& b) const noexcept {
            return a.hash == b.hash && a.text == b.text && same_params(a.params, b.params);
        }
        bool operator()(const ShapeProbe& a, const ShapeKey& b) const noexcept {
            return a.hash == b.hash && a.text == b.text && same_params(*a.params, b.params);
        }
        bool operator()(const ShapeKey& a, const ShapeProbe& b) const noexcept { return (*this)(b, a); }
    };
    static std::uint64_t shape_hash(std::string_view s, const ShapeParams& p) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
        std::size_t i = 0;
        for (; i + 8 <= s.size(); i += 8) { std::uint64_t w; std::memcpy(&w, s.data() + i, 8); mix(w); }
        std::uint64_t tail = 0;
        std::memcpy(&tail, s.data() + i, s.size() - i);
        mix(tail ^ (static_cast<std::uint64_t>(s.size()) << 56));
        const auto bits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return std::uint64_t{u}; };
        mix(bits(p.size_px) | bits(p.weight) << 32);
        mix(bits(p.letter_spacing) | std::uint64_t{p.kerning} << 32 | std::uint64_t{p.ligatures} << 33 |
            std::uint64_t{p.italic} << 34 | std::uint64_t{p.axis_count} << 40);
        for (std::uint8_t a = 0; a < p.axis_count; ++a) mix(p.axes[a].tag | bits(p.axes[a].value) << 32);
        return h;
    }
    // Drop runs not used since `before - ttl` (the whole cache when full).
    void evict_shapes(std::uint64_t now) const {
        const std::uint64_t ttl = shapes_.size() >= kMaxShapes ? 0 : config_.glyph_ttl_frames;
        std::erase_if(shapes_, [&](const auto& kv) { return kv.second.last_used + ttl < now; });
    }
    static constexpr std::size_t kMaxShapes = 1u << 15;
    mutable std::mutex shape_mutex_;
    mutable std::unordered_map<ShapeKey, ShapeEntry, ShapeHash, ShapeEq> shapes_;
    std::uint64_t shape_frame_ = 0;

    struct Sibling { std::size_t of, at; std::uint16_t weight; bool italic; };
    std::vector<Sibling> siblings_;
    [[nodiscard]] bool is_sibling(std::size_t i) const noexcept {
        for (const auto& s : siblings_) if (s.at == i) return true;
        return false;
    }
};

// ════════════════════════════════════════════════════════════════════════
// mayag integration
// ════════════════════════════════════════════════════════════════════════

/// The `layout::TextMeasurer` implementation. Layout asks this how big a
/// string is before there is a GPU or a window.
namespace detail {

/// Build the per-run shaping inputs from a text style: size, spacing, the
/// effective wght (numeric override or the FontWeight enum), and any extra
/// variable-font axis overrides. One place, so measure/draw stay in sync.
[[nodiscard]] inline ShapeParams shape_params_of(const TextStyle& st) {
    ShapeParams p;
    p.size_px        = st.size;
    p.letter_spacing = st.letter_spacing;
    p.kerning        = true;
    p.ligatures      = st.ligatures;
    p.weight         = st.effective_weight();
    p.italic         = st.italic;
    // Layout measures in logical units, so st.size IS the logical size here;
    // the painter pinned opsz to the same value before scaling to device px,
    // so measure and draw select the same design.
    FontVariations vars = st.variations;
    vars.auto_optical_size(st.size);
    for (std::uint8_t i = 0; i < vars.count && p.axis_count < ShapeParams::max_axes; ++i) {
        const auto& a = vars.axes[i];
        p.axes[p.axis_count++] = ShapeParams::Axis{a.tag, a.value};
    }
    return p;
}

}  // namespace detail

class StackMeasurer final : public layout::TextMeasurer {
  public:
    explicit StackMeasurer(const FontStack& stack) : stack_{&stack} {}

    /// Measured sizes of recent (text, style, width) triples. Views rebuild
    /// the same labels every frame; a hit here skips the shape cache's hash
    /// map, lock and refcounts. Direct-mapped, so a miss just overwrites.
    struct SizeMemo {
        std::uint64_t key = 0;
        float         max_width = 0.0f;
        Vec2          size{};
        std::string   text;
        TextStyle     style{};
    };

    [[nodiscard]] float advance(std::string_view s, const TextStyle& st) const override {
        return s.empty() ? 0.0f : stack_->shaped(s, detail::shape_params_of(st))->width;
    }

    [[nodiscard]] float ascent(const TextStyle& st) const override {
        return stack_->metrics(st.size).ascent;
    }

    [[nodiscard]] Vec2 measure(std::string_view s, const TextStyle& st,
                               float max_width) const override {
        if (s.empty()) {
            const FaceMetrics fm = stack_->metrics(st.size);
            return {0.0f, st.line_height > 0.0f ? st.size * st.line_height : fm.line_height};
        }
        // Hash the text and width; the style is compared in full on a hit.
        std::uint64_t h = 0xcbf29ce484222325ull ^ s.size();
        for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
        std::uint32_t wbits; std::memcpy(&wbits, &max_width, 4);
        h ^= wbits * 0x9E3779B97F4A7C15ull;
        h ^= h >> 31;
        SizeMemo& slot = memo_[h & (kMemo - 1)];
        if (slot.key == h && slot.max_width == max_width && slot.text == s && slot.style == st) return slot.size;
        const Vec2 r = measure_uncached(s, st, max_width);
        slot.key = h; slot.max_width = max_width; slot.text.assign(s); slot.style = st; slot.size = r;
        return r;
    }

  private:
    static constexpr std::size_t kMemo = 1024;
    mutable std::vector<SizeMemo> memo_ = std::vector<SizeMemo>(kMemo);

    [[nodiscard]] Vec2 measure_uncached(std::string_view s, const TextStyle& st, float max_width) const {
        const FaceMetrics fm = stack_->metrics(st.size);
        const float line_h = st.line_height > 0.0f ? st.size * st.line_height : fm.line_height;

        const auto run = stack_->shaped(s, detail::shape_params_of(st));
        const ShapeResult& shaped = *run;

        const bool wrapping = st.overflow == TextOverflow::wrap &&
                              max_width > 0.0f && !num::is_inf(max_width);

        // The cache filled in the unwrapped extent. A wrapping line that
        // already fits (the usual case) measures the same as a non-wrapping one.
        if (shaped.line_count > 0 &&
            (!wrapping || (shaped.widest_line <= max_width && st.max_lines == 0))) {
            return {shaped.widest_line, static_cast<float>(shaped.line_count) * line_h};
        }

        if (!wrapping) {
            // Still honour explicit newlines.
            float widest = 0.0f, cur = 0.0f;
            int lines = 1;
            for (const auto& g : shaped.glyphs) {
                if (is_newline(s, g.cluster)) {
                    widest = num::max(widest, cur);
                    cur = 0.0f;
                    ++lines;
                } else {
                    cur += g.advance;
                }
            }
            return {num::max(widest, cur), static_cast<float>(lines) * line_h};
        }

        // Greedy wrap at break opportunities.
        const auto lines = wrap_lines(s, shaped, max_width);
        std::size_t n = num::max<std::size_t>(lines.size(), 1);
        if (st.max_lines > 0) n = num::min<std::size_t>(n, st.max_lines);
        float widest = 0.0f;
        for (std::size_t i = 0; i < lines.size() && i < n; ++i)
            widest = num::max(widest, lines[i].width);
        return {widest, static_cast<float>(n) * line_h};
    }

  public:
    /// One wrapped line: a glyph range and its measured width.
    struct Line {
        std::size_t begin = 0, end = 0;   ///< indices into ShapeResult::glyphs
        float       width = 0.0f;
    };

    /// Break `shaped` into lines fitting `max_width`.
    ///
    /// Break opportunities follow the parts of UAX #14 a UI actually needs:
    /// after spaces, between ideographs, and never before closing punctuation
    /// or after opening brackets. A word longer than the line breaks
    /// mid-word rather than overflowing.
    [[nodiscard]] static std::vector<Line> wrap_lines(std::string_view text,
                                                      const ShapeResult& shaped,
                                                      float max_width) {
        std::vector<Line> lines;
        if (shaped.glyphs.empty()) return lines;

        // A box too narrow for even one glyph must NOT wrap to one character
        // per line. That produces a vertical ribbon of letters that is
        // unreadable, arbitrarily tall, and — worst — makes the layout look
        // like the text engine failed when the real fault is upstream sizing.
        //
        // Overflowing horizontally is the lesser evil: it is obviously wrong,
        // it points at the actual problem, and `clip` still contains it. So
        // below the width of the widest single glyph we stop wrapping.
        float widest_glyph = 0.0f;
        for (const auto& g : shaped.glyphs) widest_glyph = num::max(widest_glyph, g.advance);
        if (max_width < widest_glyph * 2.5f) {
            lines.push_back(Line{0, shaped.glyphs.size(), shaped.width});
            return lines;
        }

        std::size_t line_start = 0;
        std::size_t last_break = std::string_view::npos;
        float       width = 0.0f;
        float       width_at_break = 0.0f;

        for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
            const auto& g = shaped.glyphs[i];

            if (is_newline(text, g.cluster)) {
                lines.push_back(Line{line_start, i, width});
                line_start = i + 1;
                last_break = std::string_view::npos;
                width = 0.0f;
                continue;
            }

            const std::uint32_t cp = codepoint_at(text, g.cluster);

            // Record a break opportunity BEFORE adding this glyph.
            if (i > line_start && can_break_before(text, shaped, i, cp)) {
                last_break = i;
                width_at_break = width;
            }

            width += g.advance;

            if (width > max_width && i > line_start) {
                if (last_break != std::string_view::npos && last_break > line_start) {
                    lines.push_back(Line{line_start, last_break, width_at_break});
                    line_start = last_break;
                    // Leading spaces on the new line are consumed.
                    while (line_start < shaped.glyphs.size() &&
                           uni::is_space(codepoint_at(text, shaped.glyphs[line_start].cluster))) {
                        ++line_start;
                    }
                    width = 0.0f;
                    for (std::size_t k = line_start; k <= i && k < shaped.glyphs.size(); ++k) {
                        width += shaped.glyphs[k].advance;
                    }
                } else {
                    // No break opportunity: break mid-word.
                    lines.push_back(Line{line_start, i, width - g.advance});
                    line_start = i;
                    width = g.advance;
                }
                last_break = std::string_view::npos;
            }
        }

        lines.push_back(Line{line_start, shaped.glyphs.size(), width});
        return lines;
    }

    [[nodiscard]] static bool is_newline(std::string_view s, std::uint32_t cluster) noexcept {
        return cluster < s.size() && (s[cluster] == '\n' || s[cluster] == '\r');
    }

    [[nodiscard]] static std::uint32_t codepoint_at(std::string_view s,
                                                    std::uint32_t cluster) noexcept {
        if (cluster >= s.size()) return 0;
        std::size_t i = cluster;
        return utf8::decode(s, i);
    }

  private:
    [[nodiscard]] static bool can_break_before(std::string_view text, const ShapeResult& shaped,
                                               std::size_t i, std::uint32_t cp) noexcept {
        if (uni::is_no_break_before(cp)) return false;

        const std::uint32_t prev = codepoint_at(text, shaped.glyphs[i - 1].cluster);
        if (uni::is_no_break_after(prev)) return false;

        // After any space.
        if (uni::is_space(prev)) return true;
        // Between ideographs — CJK wraps without spaces.
        if (uni::is_ideographic(cp) && uni::is_ideographic(prev)) return true;
        if (uni::is_ideographic(prev) && !uni::is_no_break_before(cp)) return true;
        // After a hyphen.
        if (prev == '-' || prev == 0x2010 || prev == 0x2013) return true;

        return false;
    }

    const FontStack* stack_;
};

/// The `render::GlyphRenderer` implementation: emits one atlas-sampled quad
/// per glyph into the draw list.
class StackGlyphRenderer final : public render::GlyphRenderer {
  public:
    explicit StackGlyphRenderer(FontStack& stack, std::uint32_t atlas_slot = 1)
        : stack_{&stack}, slot_{atlas_slot} {}

    void draw_text(DrawList& dl, std::string_view s, const Rect& box,
                   const TextStyle& st) const override {
        if (s.empty()) return;

        const FaceMetrics fm = stack_->metrics(st.size);
        const float line_h = st.line_height > 0.0f ? st.size * st.line_height : fm.line_height;

        const ShapeParams sp = detail::shape_params_of(st);
        const auto run = stack_->shaped(s, sp);
        const ShapeResult& shaped = *run;

        const bool wrapping = st.overflow == TextOverflow::wrap && box.width() > 0.0f;
        const auto lines = wrapping
            ? StackMeasurer::wrap_lines(s, shaped, box.width())
            : std::vector<StackMeasurer::Line>{{0, shaped.glyphs.size(), shaped.width}};

        // Line clamp: draw at most max_lines, and if that hides later lines the
        // last drawn line gets an ellipsis (CSS line-clamp).
        const std::size_t line_count =
            (st.max_lines > 0 && lines.size() > st.max_lines)
                ? st.max_lines : lines.size();
        const bool clamped = line_count < lines.size();

        float baseline_y = box.top() + fm.ascent;

        for (std::size_t li = 0; li < line_count; ++li) {
            const auto& line = lines[li];
            // Horizontal alignment happens per LINE, which is the only way
            // centred multi-line text looks right.
            float pen_x = box.left();
            if (st.align == TextAlign::center)     pen_x += (box.width() - line.width) * 0.5f;
            else if (st.align == TextAlign::right) pen_x += box.width() - line.width;

            // Justify: spread the slack across inter-word gaps so the line
            // fills the box — every line but the LAST (a justified last line
            // looks broken), and only when there is a real target width.
            float word_gap = 0.0f;
            if (st.align == TextAlign::justify && wrapping && li + 1 < lines.size()) {
                std::size_t spaces = 0;
                for (std::size_t i = line.begin; i < line.end && i < shaped.glyphs.size(); ++i) {
                    const std::uint32_t c = shaped.glyphs[i].cluster;
                    if (c < s.size() && (s[c] == ' ' || s[c] == '\t')) ++spaces;
                }
                const float slack = box.width() - line.width;
                if (spaces > 0 && slack > 0.0f) word_gap = slack / static_cast<float>(spaces);
            }

            const float line_start_x = pen_x;

            // Ellipsis truncation: for a non-wrapping line that overflows the
            // box, drop the trailing glyphs that don't fit and append “…”. This
            // is the DEFAULT overflow, so a too-long label reads "Some long na…"
            // instead of clipping mid-glyph. Only the last visual line ellipses.
            std::size_t draw_end = line.end;
            bool show_ellipsis = false;
            float ellipsis_x = 0.0f;
            const CachedGlyph* ell_glyph = nullptr;
            // Ellipsis when: a non-wrapping line overflows, OR this is the last
            // line of a clamped block that hides further lines.
            const bool last_clamped_line = clamped && (li + 1 == line_count);
            const bool overflow_line = st.overflow == TextOverflow::ellipsis &&
                                       !wrapping && line.width > box.width() + 0.5f;
            if ((overflow_line || last_clamped_line) && box.width() > 0.0f) {
                const std::uint16_t ell_gid = stack_->ellipsis_glyph();
                ell_glyph = stack_->glyph(0, ell_gid, st.size, st.effective_weight(),
                                          {sp.axes.data(), sp.axis_count}, sp.italic);
                const float ell_w = ell_glyph ? ell_glyph->advance : st.size * 0.5f;
                const float budget = box.width() - ell_w;
                float run = 0.0f;
                draw_end = line.begin;
                for (std::size_t i = line.begin; i < line.end && i < shaped.glyphs.size(); ++i) {
                    const float nx = run + shaped.glyphs[i].advance;
                    if (nx > budget) break;
                    run = nx;
                    draw_end = i + 1;
                }
                ellipsis_x = pen_x + run;
                show_ellipsis = true;
            }

            for (std::size_t i = line.begin; i < draw_end && i < shaped.glyphs.size(); ++i) {
                const auto& g = shaped.glyphs[i];
                if (g.glyph_id == 0 && g.advance == 0.0f) continue;   // newline marker

                const CachedGlyph* cached = stack_->glyph(g.face_index, g.glyph_id, st.size,
                                                           st.effective_weight(),
                                                           {sp.axes.data(), sp.axis_count},
                                                           sp.italic);
                if (cached != nullptr && cached->size.x > 0.0f) {
                    emit(dl, *cached, Vec2{pen_x, baseline_y} + g.offset, st);
                }
                pen_x += g.advance;
                if (word_gap > 0.0f && g.cluster < s.size() &&
                    (s[g.cluster] == ' ' || s[g.cluster] == '\t')) {
                    pen_x += word_gap;
                }
            }

            // Draw the ellipsis glyph after the truncated run.
            if (show_ellipsis && ell_glyph != nullptr && ell_glyph->size.x > 0.0f) {
                emit(dl, *ell_glyph, Vec2{ellipsis_x, baseline_y}, st);
            }

            const float line_end_x = (st.align == TextAlign::justify && word_gap > 0.0f)
                                     ? box.left() + box.width() : line_start_x + line.width;

            if (st.underline) {
                const float y = baseline_y - fm.underline_position;
                dl.line({line_start_x, y}, {line_end_x, y},
                        fm.underline_thickness, st.color);
            }
            if (st.strikethrough) {
                const float y = baseline_y - fm.x_height * 0.5f;
                dl.line({line_start_x, y}, {line_end_x, y},
                        fm.underline_thickness, st.color);
            }

            baseline_y += line_h;
        }
    }

  private:
    void emit(DrawList& dl, const CachedGlyph& g, Vec2 pen, const TextStyle& st) const {
        // An SDF entry was rasterised at one size and is reused at every
        // other, so its bearing and extent scale by the ratio between them.
        // A bitmap entry was made for this exact size, so the ratio is 1.
        float k = 1.0f;
        if (g.is_sdf && g.raster_scale > 0.0f) {
            if (const Face* face = stack_->face_at(0); face != nullptr) {
                k = face->scale_for(st.size) / g.raster_scale;
            }
        }

        Rect dst{pen.x + g.bearing.x * k,
                 pen.y + g.bearing.y * k,
                 g.size.x * k,
                 g.size.y * k};

        // ── snap bitmap glyphs to the pixel grid ────────────────────────
        //
        // A bitmap entry was rasterised at EXACTLY this pixel size, so its
        // texels map 1:1 onto the framebuffer — but only if the destination
        // starts on an integer pixel. Landing at x=44.484 makes every texel
        // straddle two pixels, and the bilinear fetch averages them: the
        // glyph comes out blurred, and no amount of rasteriser quality can
        // recover it. Measured on the real UI, EVERY glyph was fractional.
        //
        // So round the origin to the pixel grid and keep the integer size.
        // Horizontal position quantises to whole pixels (which is what makes
        // stems crisp); the accumulated sub-pixel error stays in the pen, not
        // in the glyph, so advances and kerning are unaffected.
        //
        // SDF entries are resampled by design and must NOT be snapped —
        // that is the whole point of a scale-free field.
        if (!g.is_sdf) {
            dst = Rect{num::round(dst.origin.x), num::round(dst.origin.y),
                       g.size.x, g.size.y};
        }

        if (g.is_color) {
            // Colour glyph: sample RGBA verbatim, tinted only by the run's
            // opacity (from the text colour's alpha), never by its hue.
            dl.color_glyph(dst, g.uv, st.color.a);
        } else if (g.is_sdf) {
            // The SDF field changes by ~1/(2*spread) per RASTER texel; one
            // DISPLAY pixel spans 1/k raster texels, so the field moves
            // 1/(2*spread*k) per display pixel. Size the smoothstep half-band
            // to about one display pixel so a magnified glyph (k>1) gets a
            // thin crisp edge instead of a fat blur, and a shrunk one still
            // antialiases. Clamp so a texel-scale ramp never vanishes.
            const float spread = g.spread > 0.0f ? g.spread : 4.0f;
            const float per_px = 1.0f / (2.0f * spread * num::max(k, 0.01f));
            const float edge = num::clamp(per_px * 0.75f, 0.01f, 0.25f);
            dl.glyph(dst, g.uv, st.color, true, edge);
        } else {
            dl.glyph(dst, g.uv, st.color, false);
        }
    }

  public:
    /// The texture slot this renderer's glyph quads sample (backends bind
    /// the atlas there).
    [[nodiscard]] std::uint32_t atlas_slot() const noexcept { return slot_; }

  private:
    FontStack*    stack_;
    std::uint32_t slot_;
};

/// The `backend::CoverageSampler` implementation, so the software rasteriser
/// can read the atlas.
class StackSampler final : public backend::CoverageSampler {
  public:
    explicit StackSampler(const FontStack& stack) : stack_{&stack} {}

    /// Returns the RAW atlas texel. Interpreting it — coverage as-is, or a
    /// distance field that needs thresholding — is the renderer's job,
    /// because only the renderer knows which kind of entry each instance
    /// points at.
    ///
    /// Deciding here from `config().mode` was a bug: in hybrid mode a frame
    /// contains BOTH kinds, so one rule applied to all of them ran small
    /// bitmap text through an SDF threshold and snapped every antialiased
    /// pixel to 0 or 1.
    [[nodiscard]] float sample(std::uint32_t, float u, float v) const override {
        return stack_->atlas().sample(u, v);
    }

    /// Colour-glyph atlas texel, for `color_glyph` instances (emoji). Straight
    /// RGBA; the renderer premultiplies and blends it verbatim.
    [[nodiscard]] Vec4 sample_rgba(std::uint32_t, float u, float v) const override {
        return stack_->atlas().sample_color(u, v);
    }

  private:
    const FontStack* stack_;
};

}  // namespace mayag::typo
