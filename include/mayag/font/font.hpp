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
#include "bidi.hpp"
#include "color_bitmap.hpp"
#include "colr.hpp"
#include "mapped_file.hpp"
#include "opentype.hpp"
#include "ot_shapers.hpp"
#include "outline.hpp"
#include "raster.hpp"
#include "segment.hpp"
#include "shape.hpp"

#include "../layout/text_metrics.hpp"
#include "../render/painter.hpp"
#include "../backend/software.hpp"

#include <atomic>
#include <algorithm>
#include <array>
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
        face->legacy_kern_ = KernTable::legacy(face->file_);
        face->tracking_ = TrackTable{face->file_};
        face->coretext_emoji_ = face->file_.family().find("Apple Color Emoji") != std::string::npos && face->file_.has_color();
        face->ligatures_ = LigatureTable{face->file_};
        face->layout_ = std::make_unique<ot::Layout>(face->file_);
        face->id_      = next_id();
        face->base_id_ = face->id_;

        // ── variable-font setup ──────────────────────────────────────────
        // Parse the fvar axis table and start the face at every axis's
        // default, so an untouched variable font renders exactly like its
        // default instance. set_variation() later moves along the axes.
        face->axes_ = var::parse_fvar(face->file_.table(ot::tags::fvar));
        face->build_weight_map();
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

    /// How CoreText sets Apple Color Emoji's sbix images, measured against
    /// CTFontGetBoundingRectsForGlyphs at every size (nothing in the font says
    /// this; HarfBuzz/FreeType would put a 1 em image on the baseline, which
    /// draws emoji high and small). Fractions of the em at LOGICAL `point_size`:
    ///   <= 16 pt  the image is 1.25 em, bottom at -0.25 em (top at 1 em)
    ///   16..24    the image shrinks linearly from 20 to 24 units at 16/24 pt
    ///   >= 24 pt  1 em, bottom at -0.125 em
    struct BitmapBox { float scale = 1.0f, bottom = 0.0f; };
    [[nodiscard]] BitmapBox bitmap_box(float point_size) const noexcept {
        if (!coretext_emoji_) return {};
        const float s = std::max(point_size, 1.0f);
        if (s <= 16.0f) return {1.25f, -0.25f};
        if (s >= 24.0f) return {1.0f, -0.125f};
        const float d = s - 16.0f;
        return {(20.0f + d * 0.5f) / s, (-4.0f + d * 0.125f) / s};
    }
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
        return advance_units(gid) * scale_for(size_px);
    }

    /// Advance in font units at the current instance (hmtx + HVAR). The HVAR
    /// delta walks the whole item variation store (~300 ns), and shaping asks
    /// for every glyph of every run, so variable faces cache per glyph; the
    /// cache is dropped whenever the instance moves.
    [[nodiscard]] float advance_units(std::uint16_t gid) const noexcept {
        const float base = static_cast<float>(file_.advance(gid));
        if (!hvar_.ok) return base;
        std::lock_guard lock{mutex_};
        if (adv_cache_.empty()) adv_cache_.assign(file_.num_glyphs(), std::numeric_limits<float>::quiet_NaN());
        if (gid >= adv_cache_.size()) return base;
        float& slot = adv_cache_[gid];
        if (slot != slot) slot = base + var::hvar_advance_delta(hvar_, gid, coords_);   // NaN = not yet
        return slot;
    }

    /// Kerning between two glyphs in pixels, at the face's current
    /// variation instance (weight, optical size, ...).
    [[nodiscard]] float kern(std::uint16_t left, std::uint16_t right, float size_px) const noexcept {
        std::lock_guard lock{mutex_};
        return kerning_.lookup(left, right, coords_) * scale_for(size_px);
    }

    [[nodiscard]] bool has_kerning() const noexcept { return !kerning_.empty(); }

    /// The OpenType layout tables (GSUB/GPOS/GDEF) of this face.
    [[nodiscard]] const ot::Layout& layout() const noexcept { return *layout_; }
    [[nodiscard]] const KernTable& legacy_kern() const noexcept { return legacy_kern_; }
    [[nodiscard]] const TrackTable& track_table() const noexcept { return tracking_; }

    /// Extra advance per glyph, in pixels at `size_px`, that the font's
    /// `trak` table asks for at `point_size` (the LOGICAL size: 13 pt type on
    /// a 2x display is spaced as 13 pt, not 26 pt). 0 for fonts without one.
    [[nodiscard]] float tracking(float point_size, float size_px) const noexcept {
        return tracking_.at(point_size) * scale_for(size_px);
    }

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

    /// The wght axis value CoreText draws `css_weight` at. CoreText resolves a
    /// weight to the face's NAMED instance of that weight class, so a
    /// fallback face whose "Regular" sits at wght 542 (CJK Symbols Fallback)
    /// or 400 (PingFang UI) is drawn there, not at axis value 400. Between
    /// named weights the axis value is interpolated; faces without named
    /// weights use the CSS value directly.
    [[nodiscard]] float wght_for(float css_weight) const {
        if (weight_map_.empty()) return css_weight;
        const auto& m = weight_map_;
        if (css_weight <= m.front().first) return m.front().second;
        if (css_weight >= m.back().first) return m.back().second;
        for (std::size_t i = 1; i < m.size(); ++i)
            if (css_weight <= m[i].first) {
                const float t = (css_weight - m[i - 1].first) / (m[i].first - m[i - 1].first);
                return m[i - 1].second + t * (m[i].second - m[i - 1].second);
            }
        return css_weight;
    }

    /// Ink extents in font units, y-up: {x_bearing, y_bearing (top), width,
    /// height (negative)}, exactly as hb_font_get_glyph_extents reports them
    /// for hb-ot-font: a static TrueType glyph's STORED glyf bounding box
    /// (x from hmtx's lsb), otherwise the outline's bounds. False if blank.
    [[nodiscard]] bool extents(std::uint16_t gid, int& x_bearing, int& y_bearing, int& width, int& height) const {
        std::int16_t box[4];
        const bool default_instance = std::all_of(coords_.begin(), coords_.end(), [](float c) { return c == 0.0f; });
        if (default_instance && glyphs_.header_box(gid, box)) {
            x_bearing = file_.lsb(gid);
            y_bearing = std::max(box[1], box[3]);
            width = std::max(box[0], box[2]) - std::min(box[0], box[2]);
            height = std::min(box[1], box[3]) - std::max(box[1], box[3]);
            return true;
        }
        const Outline o = outline(gid);
        if (o.empty()) { x_bearing = y_bearing = width = height = 0; return false; }
        const Rect r = o.control_bounds();
        x_bearing = static_cast<int>(std::lround(r.origin.x));
        width = static_cast<int>(std::lround(r.origin.x + r.size.x)) - x_bearing;
        y_bearing = static_cast<int>(std::lround(r.origin.y + r.size.y));
        height = static_cast<int>(std::lround(r.origin.y)) - y_bearing;
        return true;
    }

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
    /// The current normalised variation coordinates (empty for static faces).
    [[nodiscard]] const var::Coords& coords() const noexcept { return coords_; }

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

    // CSS weight of a named instance's subfamily ("Regular", "Bold", ...);
    // 0 for anything else ("Condensed Bold", "Regular G1").
    static float css_weight_of(std::string_view s) noexcept {
        static constexpr std::pair<std::string_view, float> names[] = {
            {"Thin", 100}, {"Ultralight", 200}, {"ExtraLight", 200}, {"Extra Light", 200},
            {"Light", 300}, {"Regular", 400}, {"Medium", 500}, {"Semibold", 600},
            {"SemiBold", 600}, {"Semi Bold", 600}, {"Bold", 700}, {"ExtraBold", 800},
            {"Extra Bold", 800}, {"Heavy", 800}, {"Black", 900}};
        for (const auto& [n, w] : names) if (s == n) return w;
        return 0.0f;
    }

    // Map CSS weight -> wght from the face's named weight instances (other
    // axes at their defaults). Kept empty when the named Regular already sits
    // at 400, so faces whose axis is in CSS units (SF, most web fonts) keep
    // using the requested value verbatim.
    void build_weight_map() {
        weight_map_.clear();
        std::size_t wi = axes_.size();
        for (std::size_t a = 0; a < axes_.size(); ++a) if (axes_[a].tag == var::tag_wght) wi = a;
        if (wi == axes_.size()) return;
        std::vector<std::pair<float, float>> m;
        for (const auto& in : var::parse_fvar_instances(file_.table(ot::tags::fvar))) {
            if (in.coords.size() != axes_.size()) continue;
            bool others_default = true;
            for (std::size_t a = 0; a < axes_.size(); ++a)
                if (a != wi && std::abs(in.coords[a] - axes_[a].def) > 1e-3f) others_default = false;
            if (!others_default) continue;
            const float css = css_weight_of(file_.name(in.name_id));
            if (css <= 0.0f) continue;
            if (std::none_of(m.begin(), m.end(), [&](const auto& p) { return p.first == css; }))
                m.emplace_back(css, in.coords[wi]);
        }
        for (std::size_t i = 1; i < m.size(); ++i)   // insertion sort: <= ~10 entries
            for (std::size_t j = i; j > 0 && m[j].first < m[j - 1].first; --j) std::swap(m[j], m[j - 1]);
        const auto reg = std::find_if(m.begin(), m.end(), [](const auto& p) { return p.first == 400.0f; });
        if (reg == m.end() || std::abs(reg->second - 400.0f) < 0.5f) return;
        for (std::size_t i = 1; i < m.size(); ++i) if (m[i].second <= m[i - 1].second) return;  // not monotone
        weight_map_ = std::move(m);
    }

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
        adv_cache_.clear();
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
    KernTable                 legacy_kern_;   // `kern` only, for the OpenType pipeline
    TrackTable                tracking_;
    bool                      coretext_emoji_ = false;   ///< Apple Color Emoji: bitmap_box applies
    LigatureTable             ligatures_;
    std::unique_ptr<ot::Layout> layout_;      // GSUB/GPOS/GDEF, read in place
    std::uint32_t             id_ = 0;
    std::uint32_t             base_id_ = 0;   // stable id ignoring variation

    // ── variable-font state ─────────────────────────────────────────────
    std::vector<var::Axis>        axes_;      // fvar axes (empty ⇒ static font)
    std::vector<std::pair<float, float>> weight_map_;   ///< css weight -> wght, from named instances
    std::span<const std::uint8_t> avar_{};    // optional axis remap table
    var::Coords                   coords_;    // current normalised position
    std::vector<float>            user_;      // current value per axis, USER units
    var::HvarTable                hvar_;      // advance-width variations (optional)

    colr::ColrTable               colr_;      // vector colour glyphs (optional)
    colr::Palette                 cpal_;      // colour palette for COLR

    mutable std::mutex                                 mutex_;
    mutable std::unordered_map<std::uint16_t, Outline> outline_cache_;
    mutable std::vector<float>                         adv_cache_;   // per gid, NaN = unknown
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
    /// Base SDF raster size in pixels. Text up to this size shares one field;
    /// larger text uses sqrt(2)-spaced tiers up to 4x this (48..192 px), so
    /// fields are minified rather than magnified (FontStack::sdf_raster_px).
    float      sdf_raster_size = 48.0f;
    /// SDF spread at the base tier, in pixels; scaled with each tier.
    float      sdf_spread      = 6.0f;
    int        atlas_size      = 1024;
    /// Font smoothing: grow every glyph outline by this many DEVICE pixels
    /// on each side, ramping up from 0 linearly until `smoothing_full_px`
    /// (light text) or `smoothing_full_px_dark` (dark text).
    ///
    /// This is the "weight" macOS text has and FreeType-style text lacks.
    /// CoreGraphics font smoothing (on by default, Retina included) offsets
    /// glyph edges outward; measured gamma-independently on SF with straight
    /// stems at 64 subpixel offsets each, the growth is 0.30 px per side,
    /// reached at ~20 device px for white-on-black and ~51 px for
    /// black-on-white, proportionally less below. Without it, mayag's stems
    /// came out 0.6 px thinner than every native Mac app's and body text read
    /// thin and grey beside them. 0 turns it off.
    float      smoothing              = 0.30f;
    float      smoothing_full_px      = 20.0f;
    float      smoothing_full_px_dark = 51.0f;
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

    /// Append a fallback face that may serve ONLY codepoints in `only`
    /// (sorted, inclusive [lo, hi] ranges) -- a stand-in for a face mayag
    /// cannot read, limited to what that face covered.
    void add_limited(std::shared_ptr<Face> face, std::span<const std::array<char32_t, 2>> only) {
        if (face == nullptr) return;
        if (limits_.size() < faces_.size()) limits_.resize(faces_.size());
        faces_.push_back(std::move(face));
        limits_.emplace_back(only.begin(), only.end());
        clear_shape_cache();
    }

    /// Route codepoints [lo, hi] to face `face` first (when it serves them),
    /// ahead of the normal first-covering order: the per-range exceptions of
    /// a measured platform cascade. Ranges must not overlap.
    void prefer_range(char32_t lo, char32_t hi, std::size_t face) {
        auto it = std::lower_bound(preferred_.begin(), preferred_.end(), lo,
                                   [](const Preferred& p, char32_t c) { return p.lo < c; });
        preferred_.insert(it, Preferred{lo, hi, face});
        clear_shape_cache();
    }

    /// True when face `i` maps `cp` and is allowed to serve it.
    [[nodiscard]] bool serves(std::size_t i, char32_t cp) const noexcept {
        if (i < limits_.size() && !limits_[i].empty()) {
            const auto& r = limits_[i];
            auto it = std::upper_bound(r.begin(), r.end(), cp,
                                       [](char32_t c, const std::array<char32_t, 2>& x) { return c < x[0]; });
            if (it == r.begin() || cp > (it - 1)->at(1)) return false;
        }
        return faces_[i]->glyph_for(cp) != 0;
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

    /// Register `face` as the design `base` switches to from `min_pt` (logical
    /// points) up -- CoreText's PingFang UI Text/Display split at 20 pt. It
    /// inherits the base's coverage limits and never takes part in fallback.
    void add_size_variant(std::size_t base, std::shared_ptr<Face> face, float min_pt) {
        if (face == nullptr || base >= faces_.size()) return;
        const std::size_t at = faces_.size();
        faces_.push_back(std::move(face));
        if (base < limits_.size() && !limits_[base].empty()) {
            limits_.resize(faces_.size());
            limits_[at] = limits_[base];
        }
        siblings_.push_back(Sibling{base, at, 0, false, min_pt});
        clear_shape_cache();
    }

    /// The face (`base` or one of its siblings) to draw `base`'s glyphs at
    /// `weight`: the nearest real weight with the right slant, if it beats the
    /// base face. Variable faces reach any weight themselves, so stay put.
    [[nodiscard]] std::size_t face_for_size(std::size_t base, float pt) const noexcept {
        std::size_t best = base;
        float best_min = 0.0f;
        for (const auto& s : siblings_)
            if (s.of == base && s.min_pt > 0.0f && pt >= s.min_pt && s.min_pt > best_min) { best = s.at; best_min = s.min_pt; }
        return best;
    }

    [[nodiscard]] std::size_t face_for_weight(std::size_t base, float weight, bool italic) const noexcept {
        if (siblings_.empty() || base >= faces_.size() || faces_[base]->is_variable()) return base;
        std::size_t best = base;
        float best_d = num::abs(static_cast<float>(faces_[base]->weight()) - weight) +
                       (faces_[base]->is_italic() != italic ? 1000.0f : 0.0f);
        for (const auto& s : siblings_) {
            if (s.of != base || s.min_pt > 0.0f) continue;
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
    /// True once if the atlas moved glyphs since the last call (see
    /// Atlas::take_moved): a frame painted across the move must be redone.
    [[nodiscard]] bool atlas_moved() noexcept { return atlas_.take_moved(); }
    [[nodiscard]] const FontConfig& config() const noexcept { return config_; }

    void begin_frame(std::uint64_t frame) {
        atlas_.set_frame(frame);
        if (frame % 120 == 0) atlas_.collect(config_.glyph_ttl_frames);
        atlas_.maintain();   // move glyphs only here, before anything paints
        std::lock_guard lock{shape_mutex_};
        shape_frame_ = frame;
        if (frame % 120 == 0) evict_shapes(frame);
    }

    /// Which face can render `codepoint`, and its glyph id.
    [[nodiscard]] std::pair<std::size_t, std::uint16_t>
    resolve(std::uint32_t codepoint) const noexcept {
        if (const std::size_t p = preferred_face(codepoint); p < faces_.size() && serves(p, codepoint))
            return {p, faces_[p]->glyph_for(codepoint)};
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            if (!siblings_.empty() && is_sibling(i)) continue;   // siblings stand in for a face, never fall back
            if (!serves(i, codepoint)) continue;
            return {i, faces_[i]->glyph_for(codepoint)};
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

        // ── decode ──
        std::vector<char32_t> cps;
        std::vector<std::uint32_t> at;   // byte offset of each codepoint
        cps.reserve(text.size());
        at.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            at.push_back(static_cast<std::uint32_t>(i));
            cps.push_back(utf8::decode(text, i));
        }
        const std::size_t n = cps.size();
        for (char32_t c : cps) {
            if (uni::is_rtl(c)) out.has_rtl = true;
            if (uni::needs_complex_shaping(c)) out.needs_complex = true;
        }

        // ── bidi levels, per paragraph (UAX #9 up to I2) ──
        // Shaping needs the resolved embedding levels; rule L1 (trailing
        // whitespace) depends on where LINES end, which is not known yet, so
        // it runs per line when the line is painted (visual_glyph_order).
        std::vector<std::uint8_t> levels(n, 0), paras(n, 0);
        if (!bidi::is_trivially_ltr(cps)) {
            std::size_t p0 = 0;
            for (std::size_t i = 0; i <= n; ++i) {
                const bool end = i == n || ucd::bidi_class(cps[i]) == ucd::BidiClass::B;
                if (!end) continue;
                const std::size_t p1 = std::min(n, i + 1);
                const std::span<const char32_t> para{cps.data() + p0, p1 - p0};
                const bidi::Paragraph p = bidi::resolve(para, bidi::Direction::auto_detect);
                for (std::size_t k = 0; k < p.levels.size() && p0 + k < n; ++k) {
                    levels[p0 + k] = p.levels[k];
                    paras[p0 + k] = p.base_level;
                }
                p0 = p1;
            }
        }

        // ── scripts: Common/Inherited take their neighbour's script ──
        std::vector<ucd::Script> scripts(n, ucd::Script::Common);
        {
            ucd::Script last = ucd::Script::Common;
            // Paired brackets take the script of their opening partner.
            std::vector<std::pair<char32_t, ucd::Script>> open;
            for (std::size_t i = 0; i < n; ++i) {
                ucd::Script s = ucd::script(cps[i]);
                const bool weak = s == ucd::Script::Common || s == ucd::Script::Inherited || s == ucd::Script::Unknown;
                if (weak) {
                    const ucd::Bracket br = ucd::bidi_bracket(cps[i]);
                    if (br.type == ucd::BracketType::open) {
                        open.push_back({br.pair, last});
                        if (open.size() > 64) open.erase(open.begin());
                    } else if (br.type == ucd::BracketType::close) {
                        for (std::size_t k = open.size(); k > 0; --k)
                            if (open[k - 1].first == cps[i]) { last = open[k - 1].second; open.resize(k - 1); break; }
                    }
                    s = last;
                } else {
                    last = s;
                }
                scripts[i] = s;
            }
            // Leading weak characters take the first strong script.
            std::size_t first = 0;
            while (first < n && (ucd::script(cps[first]) == ucd::Script::Common ||
                                 ucd::script(cps[first]) == ucd::Script::Inherited ||
                                 ucd::script(cps[first]) == ucd::Script::Unknown)) ++first;
            if (first < n) for (std::size_t i = 0; i < first; ++i) scripts[i] = scripts[first];
        }

        // ── faces: per grapheme, so a base and its marks stay in one font ──
        std::vector<std::uint16_t> face_of(n, 0);
        {
            std::size_t i = 0;
            while (i < n) {
                std::size_t j = i + 1;
                while (j < n && (ucd::is_mark(cps[j]) || cps[j] == 0x200D || ucd::is_default_ignorable(cps[j]) ||
                                 (cps[j - 1] == 0x200D && ucd::is_extended_pictographic(cps[j])) ||
                                 (cps[j] >= 0x1F3FB && cps[j] <= 0x1F3FF)))
                    ++j;
                // The face that covers the whole grapheme, else the base's face.
                std::size_t fi = resolve(cps[i]).first;
                if (j > i + 1) {
                    const auto covers_all = [&](std::size_t f) {
                        for (std::size_t k = i; k < j; ++k)
                            if (!ucd::is_default_ignorable(cps[k]) && !serves(f, cps[k])) return false;
                        return true;
                    };
                    if (!covers_all(fi))
                        for (std::size_t f = 0; f < faces_.size(); ++f) {
                            if (!siblings_.empty() && is_sibling(f)) continue;
                            if (covers_all(f)) { fi = f; break; }
                        }
                }
                if (cps[i] == '\n' || cps[i] == '\r') fi = 0;
                for (std::size_t k = i; k < j; ++k) face_of[k] = static_cast<std::uint16_t>(fi);
                i = j;
            }
        }

        // ── runs: maximal (face, script, level) spans, newlines alone ──
        std::uint16_t run_id = 0;
        for (std::size_t i = 0; i < n;) {
            if (cps[i] == '\n' || cps[i] == '\r') {
                // Layout's business, not the shaper's: a zero-width marker so
                // cluster indices stay aligned with the source string.
                ShapedGlyph g{0, at[i], 0.0f, {}, 0, false};
                g.level = levels[i];
                g.para_level = paras[i];
                g.run = run_id++;
                out.glyphs.push_back(g);
                ++i;
                continue;
            }
            std::size_t j = i + 1;
            while (j < n && cps[j] != '\n' && cps[j] != '\r' && face_of[j] == face_of[i] &&
                   scripts[j] == scripts[i] && levels[j] == levels[i])
                ++j;
            shape_run(text, cps, at, i, j, face_of[i], scripts[i], levels[i], params, run_id++, out);
            for (std::size_t k = out.glyphs.size(); k-- > 0 && out.glyphs[k].run == run_id - 1;)
                out.glyphs[k].para_level = paras[i];
            i = j;
        }
        return out;
    }

  private:
    /// Shape cps[begin, end) with one face through the OpenType pipeline and
    /// append the glyphs in LOGICAL order.
    void shape_run(std::string_view text, const std::vector<char32_t>& cps, const std::vector<std::uint32_t>& at,
                   std::size_t begin, std::size_t end, std::size_t face_idx0, ucd::Script script,
                   std::uint8_t level, const ShapeParams& params, std::uint16_t run_id, ShapeResult& out) const {
        (void)text;
        // A real Bold beats an emboldened Regular, if it covers the run.
        std::size_t face_idx = face_idx0;
        if (const std::size_t w = face_for_weight(face_idx, params.weight, params.italic); w != face_idx) {
            bool covers = true;
            for (std::size_t k = begin; k < end && covers; ++k)
                covers = ucd::is_default_ignorable(cps[k]) || serves(w, cps[k]);
            if (covers) face_idx = w;
        }
        {   // optical-size design switch (logical points: the opsz pin, else size)
            float pt = params.size_px;
            for (std::uint8_t a = 0; a < params.axis_count; ++a)
                if (params.axes[a].tag == ot::tag("opsz")) pt = params.axes[a].value;
            face_idx = face_for_size(face_idx, pt);
        }
        Face& face = *faces_[face_idx];
        pin_variations(&face, params.weight, {params.axes.data(), params.axis_count}, params.italic);

        const ot::Layout& layout = face.layout();
        const ot::FontFile& file = face.file();
        ot::FontFuncs ff;
        ff.nominal = [&](char32_t c, std::uint16_t& g) { g = file.glyph_for(c); return g != 0; };
    ff.num_glyphs = file.num_glyphs();
        ff.advance = [&](std::uint16_t g) { return face.advance_units(g); };
        ff.upem = file.units_per_em();
        ff.coords = face.coords();
        const KernTable& legacy = face.legacy_kern();
        if (params.kerning && legacy.has_legacy())
            ff.legacy_kern = [&](std::uint16_t a, std::uint16_t b) { return int(legacy.legacy_lookup(a, b)); };
        if (params.kerning)
            if (const auto kt = file.table(ot::tag("kern")); aat::has_kern(kt)) { ff.kern_table = kt; ff.num_glyphs = file.num_glyphs(); }
        ff.kerx_table = file.table(ot::tag("kerx"));   // kern=0 masks its pair kerning, as in HarfBuzz
        ff.ankr_table = file.table(ot::tag("ankr"));
        ff.num_glyphs = file.num_glyphs();
        ff.x_height = file.os2().x_height;
        ff.cap_height = file.os2().cap_height;
        ff.extents = [&](std::uint16_t g, ot::FontFuncs::Extents& e) {
            if (!face.extents(g, e.x_bearing, e.y_bearing, e.width, e.height)) e = {};
            return true;
        };

        const ot::Direction dir = (level & 1) ? ot::Direction::rtl : ot::Direction::ltr;
        std::vector<ot::Feature> feats;
        if (!params.kerning) feats.push_back({ot::make_tag('k', 'e', 'r', 'n'), 0});
        if (!params.ligatures) {
            feats.push_back({ot::make_tag('l', 'i', 'g', 'a'), 0});
            feats.push_back({ot::make_tag('c', 'l', 'i', 'g'), 0});
        }
        const auto plan_lang = std::span<const std::uint32_t>{params.languages.data(),
            static_cast<std::size_t>(std::count_if(params.languages.begin(), params.languages.end(),
                                                   [](std::uint32_t t) { return t != 0; }))};
        const auto& plan = plan_for(face, layout, script, dir, feats, ff, legacy.has_legacy() || !ff.kern_table.empty(), plan_lang);

        ot::Buffer b;
        b.info.reserve((end - begin) + 8);
        b.direction = dir;
        for (std::size_t k = begin; k < end; ++k) {
            ot::GlyphInfo gi;
            gi.codepoint = cps[k];
            gi.cluster = at[k];
            b.info.push_back(gi);
        }
        ot::shape(*plan, layout, ff, b, feats, begin == 0);

        // Shaped output is visual; store logically (reverse RTL runs back).
        if (dir == ot::Direction::rtl) b.reverse();
        const float scale = face.scale_for(params.size_px);
        const float track = tracking_px(face, params);
        // CoreText's emoji box widens the advance with the image: the pen
        // moves ceil(box * em + tracking) (measured, every size).
        float emoji_pt = params.size_px;
        for (std::uint8_t a = 0; a < params.axis_count; ++a)
            if (params.axes[a].tag == ot::tag("opsz")) emoji_pt = params.axes[a].value;
        const Face::BitmapBox ebox = face.bitmap_box(emoji_pt);
        const bool emoji_box = ebox.scale != 1.0f || ebox.bottom != 0.0f;
        const auto fi8 = static_cast<std::uint8_t>(num::min<std::size_t>(face_idx, 255));
        for (std::size_t k = 0; k < b.len(); ++k) {
            const auto& gi = b.info[k];
            const auto& gp = b.pos[k];
            ShapedGlyph g;
            g.glyph_id = gi.gid;
            g.cluster = gi.cluster;
            g.face_index = fi8;
            g.missing = gi.gid == 0 && !ot::is_default_ignorable(gi);
            g.advance = gp.x_advance * scale;
            g.offset = Vec2{gp.x_offset * scale, -gp.y_offset * scale};   // y-up font units -> y-down px
            if (g.advance != 0.0f) g.advance += params.letter_spacing + track;
            if (emoji_box && gp.x_advance != 0.0f) {
                // CoreText's emoji advance: the image width plus tracking,
                // rounded to whole pixels with a bias that fades out between
                // 21 and 25 pt (fitted to CTRunGetAdvances, 8..160 pt).
                const float bias = 0.37f * std::clamp((25.0f - emoji_pt) / 4.0f, 0.0f, 1.0f);
                g.advance = std::round(gp.x_advance * scale * ebox.scale + track + bias) + params.letter_spacing;
            }
            g.level = level;
            g.run = run_id;
            out.width += g.advance;
            out.glyphs.push_back(g);
        }
    }

    /// Plans are cached per (face instance, script, direction, features).
    std::shared_ptr<const ot::ShapePlan> plan_for(const Face& face, const ot::Layout& layout, ucd::Script script,
                                                  ot::Direction dir, const std::vector<ot::Feature>& feats,
                                                  const ot::FontFuncs& ff, bool has_kern,
                                                  std::span<const std::uint32_t> languages = {}) const {
        std::uint64_t key = face.id();
        key = key * 1000003u + static_cast<std::uint64_t>(script);
        key = key * 31u + static_cast<std::uint64_t>(dir);
        for (std::uint32_t l : languages) key = key * 1000003u + l;
        for (const auto& f : feats) key = key * 1000003u + (std::uint64_t{f.tag} << 8 | f.value);
        {
            std::lock_guard lock{plan_mutex_};
            if (auto it = plans_.find(key); it != plans_.end()) return it->second;
        }
        auto plan = std::make_shared<const ot::ShapePlan>(
            ot::compile_plan(layout, script, dir, languages, feats, ff, has_kern, face.file().has_table(ot::tag("kerx")),
                             face.file().table(ot::tag("morx"))));
        std::lock_guard lock{plan_mutex_};
        if (plans_.size() > 512) plans_.clear();
        return plans_.emplace(key, std::move(plan)).first->second;
    }
    mutable std::mutex plan_mutex_;
    mutable std::unordered_map<std::uint64_t, std::shared_ptr<const ot::ShapePlan>> plans_;

  public:

    // ── glyph rasterisation and caching ─────────────────────────────────

    /// Fetch (rasterising on demand) the atlas entry for a glyph. `weight`
    /// drives the wght axis; `axes` carries any further variable-font axis
    /// overrides (opsz, wdth, ...) so the raster matches the shaped run.
    /// Horizontal positions a coverage glyph is rasterised at, per pixel.
    /// A bitmap glyph must land on the integer grid to stay sharp (a
    /// fractional destination makes every texel straddle two pixels and the
    /// bilinear fetch blurs it), but snapping the PEN to whole pixels moves
    /// each glyph up to half a pixel from where the shaper put it, and the
    /// eye reads that as uneven spacing ("rn" crowding, "il" gaps). So the
    /// glyph is rasterised with the pen's fraction baked into its outline and
    /// drawn at the integer part: sharp texels AND placement on the grid the
    /// shaper asked for.
    ///
    /// How finely: exactly as CoreGraphics quantises glyph positions
    /// (CGContextSetShouldSubpixelQuantizeFonts, the macOS default), measured
    /// by drawing a glyph at every 1/96 px offset and finding where its ink
    /// jumps: the fraction is FLOORED to 1/N px with N = 1 + floor(100 / 3s)
    /// for an s px font, at most 5 -- quarters below 11.1 px, thirds to
    /// 16.7 px, halves to 33.3 px, whole pixels above. Rounding to quarters
    /// at every size (Skia's choice) put every SF glyph ~0.15 px right of
    /// where macOS draws it.
    static constexpr int max_subpixel_phases = 5;
    [[nodiscard]] static int subpixel_phases_for(float size_px) noexcept {
        if (!(size_px > 0.0f)) return 1;
        const int n = 1 + static_cast<int>(std::floor(100.0f / (3.0f * size_px) + 1e-4f));
        return std::clamp(n, 1, max_subpixel_phases);
    }

    /// Split a pen x into the integer pixel a coverage glyph is drawn at and
    /// the phase (0..phases-1, in 1/phases px) it must be rasterised with.
    struct PenSplit { float pixel; int phase; };
    [[nodiscard]] static PenSplit split_pen(float x, int phases) noexcept {
        phases = std::clamp(phases, 1, max_subpixel_phases);
        const float pixel = std::floor(x + 1e-4f);
        const int phase = static_cast<int>(std::floor((x - pixel) * static_cast<float>(phases) + 1e-4f));
        return {pixel, std::clamp(phase, 0, phases - 1)};
    }

    [[nodiscard]] const CachedGlyph* glyph(std::size_t face_index, std::uint16_t gid,
                                           float size_px, float weight = 400.0f,
                                           std::span<const ShapeParams::Axis> axes = {},
                                           bool italic = false, int x_phase = 0,
                                           bool dark_text = false) {
        if (face_index >= faces_.size()) return nullptr;
        const Face& face = *faces_[face_index];
        pin_variations(faces_[face_index].get(), weight, axes, italic);

        const bool use_sdf = should_use_sdf(size_px);
        // Phases only mean something for outline coverage: an SDF is resampled
        // anyway, and colour bitmaps are scaled images.
        if (use_sdf || face.is_color() || face.has_colr(gid)) x_phase = 0;
        x_phase = std::clamp(x_phase, 0, subpixel_phases_for(size_px) - 1);
        // Polarity only changes coverage rasters (SDF growth is per draw).
        if (use_sdf || face.is_color() || face.has_colr(gid) || config_.smoothing <= 0.0f)
            dark_text = false;

        GlyphKey key;
        key.face_id  = face.id();
        key.glyph_id = gid;
        // The payoff of SDF: sizes share a raster. Entries are made in TIERS
        // (sdf_raster_px, sqrt(2) apart), so a field is minified rather than
        // magnified up to 4x the base raster, while a page with ten type
        // sizes still rasterises each glyph only a few times.
        // Bitmap entries are rasterised for the size they are drawn at, and
        // quantising to QUARTER pixels keeps the cache from exploding while
        // staying far finer than the eye can resolve.
        const float sdf_px = use_sdf ? sdf_raster_px(size_px) : 0.0f;
        key.size_bucket = use_sdf
            ? static_cast<std::uint16_t>(sdf_px)
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
        key.flags |= glyph_flags::encode_phase(x_phase);
        if (dark_text) key.flags |= glyph_flags::dark_text;

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
            // Strike for the size it will be DRAWN at (device px, emoji box
            // included): HarfBuzz's smallest-strike-at-least rule.
            float want_pt = size_px;
            for (const auto& ax : axes) if (ax.tag == ot::tag("opsz")) want_pt = ax.value;
            const int want_ppem = static_cast<int>(std::ceil(size_px * face.bitmap_box(want_pt).scale));
            ColorBitmap cb = color_bitmap_for(face.file(), gid, want_ppem);
            if (cb.valid()) {
                // The strike is authored at cb.ppem px/em; scale it so the em
                // matches the requested size_px -- times CoreText's emoji box
                // (Face::bitmap_box) at the logical point size.
                float pt = size_px;
                for (const auto& ax : axes) if (ax.tag == ot::tag("opsz")) pt = ax.value;
                const Face::BitmapBox box = face.bitmap_box(pt);
                const float scale = (cb.ppem > 0 ? size_px / static_cast<float>(cb.ppem) : 1.0f) * box.scale;
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
                // left offset. CoreText's emoji box moves the bottom to
                // box.bottom em and centres the image in its tracked advance.
                Vec2 bearing{cb.bearing_x * scale, -cb.bearing_y * scale};
                if (box.scale != 1.0f || box.bottom != 0.0f) {
                    bearing.y = -(static_cast<float>(dh) + box.bottom * size_px);
                    bearing.x += face.tracking(pt, size_px) * 0.4f * box.scale;   // CoreText, measured
                }
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
            const float raster_scale = face.scale_for(sdf_px);
            // Faux-bold in the SDF path: thicken the outline by ~3% of the
            // raster size, matching the coverage path so bold looks the same
            // on both sides of the SDF threshold.
            const float embolden_px = synth_bold
                ? num::max(sdf_px * 0.03f, 1.0f) : 0.0f;
            // Spread scales with the tier so the field covers the same
            // DISPLAY distance (smoothing growth, the edge band) at every tier.
            const float spread = config_.sdf_spread * sdf_px / num::max(config_.sdf_raster_size, 1.0f);
            auto r = rasterize_sdf(o, raster_scale, spread, 2, embolden_px);
            return atlas_.insert(key, r.bitmap, r.offset, advance,
                                 raster_scale, spread, true);
        }

        // Rasterise at the size the KEY represents, not the raw request, so
        // two calls that share a cache slot also share identical pixels.
        const float quantised = static_cast<float>(key.size_bucket) * 0.25f;
        const float scale = face.scale_for(quantised);
        if (const float grow = smoothing_px(quantised, dark_text); grow > 0.0f) {
            // CoreGraphics' smoothing grows each side horizontally by `grow`
            // but vertically grows the TOP by 2*grow and the bottom not at
            // all (measured on straight SF stems and bars, 32-160 px): a
            // symmetric dilation plus an upward shift of `grow`.
            o.embolden(grow / scale);
            o.transform(Affine{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, grow / scale});
        }
        if (x_phase != 0) {
            // Bake the pen's fraction into the outline (font units, so divide
            // the pixel shift by the scale). rasterize() floors the bounds, so
            // the bitmap's offset stays integral and the shift lands in the
            // coverage, where it belongs.
            const float dx = static_cast<float>(x_phase) / static_cast<float>(subpixel_phases_for(size_px)) / scale;
            o.transform(Affine{1.0f, 0.0f, 0.0f, 1.0f, dx, 0.0f});
        }
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
        vals[n++] = {var::tag_wght, face->wght_for(weight)};

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
            // CoreText's automatic optical sizing skips variable faces that
            // live in a collection: SF India and CJK Symbols Fallback draw at
            // their default opsz at every size (CTFontCopyVariation reports no
            // opsz; SF Devanagari's advances match opsz 28 at 12 pt), while
            // standalone SF Arabic/Hebrew/Armenian follow the size.
            if (a.tag == var::tag_opsz && face->file().in_collection()) continue;
            bool merged = false;
            for (std::size_t j = 0; j < n; ++j)
                if (vals[j].first == a.tag) { vals[j].second = a.value; merged = true; break; }
            if (!merged) vals[n++] = {a.tag, a.value};
        }
        face->set_variations({vals.data(), n});
    }

    /// The `trak` tracking for one glyph of `face` at these params. Tracking
    /// is chosen by POINT size, i.e. the logical size, which the shaper only
    /// sees as the optical-size axis: shape_params_of and the painter both pin
    /// opsz to the logical size before scaling to device pixels. Without an
    /// opsz pin, size_px is the best guess (the 1x case).
    [[nodiscard]] static float tracking_px(const Face& face, const ShapeParams& params) noexcept {
        float pt = params.size_px;
        for (std::uint8_t a = 0; a < params.axis_count; ++a)
            if (params.axes[a].tag == ot::tag("opsz")) pt = params.axes[a].value;
        return face.tracking(pt, params.size_px);
    }

    /// Font-smoothing growth per side, in device pixels, for a glyph drawn
    /// at `size_px` in light or dark text. See FontConfig::smoothing.
    [[nodiscard]] float smoothing_px(float size_px, bool dark_text = false) const noexcept {
        const float full = dark_text ? config_.smoothing_full_px_dark : config_.smoothing_full_px;
        if (config_.smoothing <= 0.0f || full <= 0.0f) return 0.0f;
        return config_.smoothing * num::clamp(size_px / full, 0.0f, 1.0f);
    }

    /// Whether text in `c` counts as dark for font smoothing: encoded
    /// luminance below the midpoint, the same split apply_text_gamma uses.
    [[nodiscard]] static bool is_dark_text(Color<Srgb> c) noexcept {
        return 0.2126f * c.c0 + 0.7152f * c.c1 + 0.0722f * c.c2 < 0.5f;
    }

    [[nodiscard]] bool should_use_sdf(float size_px) const noexcept {
        switch (config_.mode) {
            case RenderMode::bitmap: return false;
            case RenderMode::sdf:    return true;
            case RenderMode::hybrid: return size_px >= config_.sdf_threshold;
        }
        return false;
    }

  public:
    /// The pixel size an SDF entry for text drawn at `size_px` is rasterised
    /// at: sdf_raster_size stepped up by sqrt(2) (48, 68, 96, 136, 192 by
    /// default) to the first tier at least as large as the drawn size, so
    /// fields are only ever minified, up to 4x the base; past that the top
    /// tier is magnified. A field magnified 4x blurred the ~1 texel strokes
    /// of dense CJK (龘) into grey bars and shifted thin features.
    [[nodiscard]] float sdf_raster_px(float size_px) const noexcept {
        const float base = num::max(config_.sdf_raster_size, 1.0f);
        float px = base;
        for (int i = 0; i < 4 && size_px > px + 0.5f; ++i)
            px = num::round(base * std::exp2(static_cast<float>(i + 1) * 0.5f));
        return px;
    }

  private:
    FontConfig                          config_;
    Atlas                               atlas_;
    std::vector<std::shared_ptr<Face>>  faces_;
    std::vector<std::vector<std::array<char32_t, 2>>> limits_;   ///< per face; empty = unlimited
    struct Preferred { char32_t lo, hi; std::size_t face; };
    std::vector<Preferred> preferred_;   ///< sorted by lo
    [[nodiscard]] std::size_t preferred_face(char32_t cp) const noexcept {
        if (preferred_.empty()) return static_cast<std::size_t>(-1);
        auto it = std::upper_bound(preferred_.begin(), preferred_.end(), cp,
                                   [](char32_t c, const Preferred& p) { return c < p.lo; });
        if (it == preferred_.begin()) return static_cast<std::size_t>(-1);
        --it;
        return cp <= it->hi ? it->face : static_cast<std::size_t>(-1);
    }

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
            a.axis_count != b.axis_count || a.languages != b.languages)
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
        for (const std::uint32_t l : p.languages) if (l) mix(l);
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

    struct Sibling { std::size_t of, at; std::uint16_t weight; bool italic; float min_pt = 0.0f; };
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
    if (st.language[0] != '\0') {
        const std::string_view lang{st.language.data(), std::char_traits<char>::length(st.language.data())};
        const auto tags = ot::ot_language_tags(lang);
        for (std::size_t k = 0; k < tags.size() && k < p.languages.size(); ++k) p.languages[k] = tags[k];
    }
    return p;
}

}  // namespace detail

namespace detail {
/// The glyphs [begin, end) of one line in VISUAL order (UAX #9 L1 + L2,
/// per line). Glyphs are stored logically. L1: whitespace and isolate
/// controls at the END of the line, and whitespace before a tab or
/// segment separator, take the paragraph level, so a trailing space of
/// an RTL run in an LTR paragraph sits at the line's end, not mid-line.
/// L2: reverse every maximal sequence at or above each odd level, from
/// the highest down. A right-to-left run's glyphs are reversed as a unit
/// (the shaper's own visual order within it is preserved).
inline void visual_glyph_order(std::string_view text, const ShapeResult& shaped, std::size_t begin, std::size_t end,
                         std::vector<std::size_t>& out) {
    out.clear();
    if (begin >= end) return;
    bool any_rtl = false;
    for (std::size_t i = begin; i < end; ++i)
        any_rtl |= ((shaped.glyphs[i].level | shaped.glyphs[i].para_level) & 1) != 0;
    if (!any_rtl) {
        for (std::size_t i = begin; i < end; ++i) out.push_back(i);
        return;
    }
    // L1 levels for this line.
    const auto ws_or_iso = [&](std::size_t i) {
        const std::uint32_t c = shaped.glyphs[i].cluster;
        if (c >= text.size()) return false;
        std::size_t at = c;
        const std::uint32_t cp = utf8::decode(text, at);
        const auto bc = ucd::bidi_class(cp);
        using B = ucd::BidiClass;
        return bc == B::WS || bc == B::FSI || bc == B::LRI || bc == B::RLI || bc == B::PDI ||
               bc == B::BN || bc == B::LRE || bc == B::RLE || bc == B::LRO || bc == B::RLO || bc == B::PDF;
    };
    const auto seg_sep = [&](std::size_t i) {
        const std::uint32_t c = shaped.glyphs[i].cluster;
        if (c >= text.size()) return false;
        std::size_t at = c;
        const auto bc = ucd::bidi_class(utf8::decode(text, at));
        return bc == ucd::BidiClass::S || bc == ucd::BidiClass::B;
    };
    std::vector<std::uint8_t> lv(end - begin);
    for (std::size_t i = begin; i < end; ++i) lv[i - begin] = shaped.glyphs[i].level;
    bool trailing = true;   // walking backwards: still in the line's trailing whitespace
    for (std::size_t i = end; i-- > begin;) {
        const std::uint8_t para = shaped.glyphs[i].para_level;
        if (seg_sep(i)) { lv[i - begin] = para; trailing = true; continue; }
        if (trailing && ws_or_iso(i)) { lv[i - begin] = para; continue; }
        trailing = false;
    }
    // Segments: same run AND same (line) level.
    struct Seg { std::size_t first, last; std::uint8_t level; };
    std::vector<Seg> segs;
    for (std::size_t i = begin; i < end;) {
        std::size_t j = i + 1;
        while (j < end && shaped.glyphs[j].run == shaped.glyphs[i].run && lv[j - begin] == lv[i - begin]) ++j;
        segs.push_back({i, j, lv[i - begin]});
        i = j;
    }
    std::vector<std::uint8_t> levels(segs.size());
    for (std::size_t k = 0; k < segs.size(); ++k) levels[k] = segs[k].level;
    for (const std::uint32_t k : bidi::visual_order(levels)) {
        const Seg& sg = segs[k];
        if (sg.level & 1)
            for (std::size_t i = sg.last; i > sg.first; --i) out.push_back(i - 1);
        else
            for (std::size_t i = sg.first; i < sg.last; ++i) out.push_back(i);
    }
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

    // ── bidi-exact editing geometry ──
    //
    // The line is shaped ONCE (contextual forms and kerning included, unlike
    // measuring a prefix, which reshapes "مرح" as if it ended the word) and
    // laid out in visual order (L1 + L2, as it is painted). Each grapheme
    // gets a visual span [x0, x1) and a direction; a glyph that covers
    // several graphemes (a ligature) is split evenly between them, the
    // fallback CoreText and HarfBuzz-based editors use without GDEF carets.
    struct GraphemeBox { std::size_t begin = 0, end = 0; float x0 = 0, x1 = 0; bool rtl = false; };

    [[nodiscard]] std::vector<GraphemeBox> grapheme_boxes(std::string_view s, const TextStyle& st) const {
        std::vector<GraphemeBox> out;
        if (s.empty()) return out;
        const auto shaped = stack_->shaped(s, detail::shape_params_of(st));
        std::vector<std::size_t> order;
        detail::visual_glyph_order(s, *shaped, 0, shaped->glyphs.size(), order);
        // Grapheme starts, to split clusters.
        std::vector<std::size_t> gstarts;
        for (std::size_t b = 0; b < s.size(); b = seg::next_grapheme(s, b)) gstarts.push_back(b);
        gstarts.push_back(s.size());
        const auto graphemes_in = [&](std::size_t lo, std::size_t hi) {
            std::vector<std::size_t> v;
            for (std::size_t k = 0; k + 1 < gstarts.size(); ++k)
                if (gstarts[k] >= lo && gstarts[k] < hi) v.push_back(k);
            return v;
        };
        // Cluster -> byte end: the next larger cluster value in the text.
        std::vector<std::uint32_t> clusters;
        for (const auto& g : shaped->glyphs) clusters.push_back(g.cluster);
        std::sort(clusters.begin(), clusters.end());
        clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
        const auto cluster_end = [&](std::uint32_t c) -> std::size_t {
            const auto it = std::upper_bound(clusters.begin(), clusters.end(), c);
            return it == clusters.end() ? s.size() : *it;
        };
        // Walk glyphs visually, merging consecutive glyphs of one cluster.
        float x = 0.0f;
        for (std::size_t k = 0; k < order.size();) {
            const auto& g0 = shaped->glyphs[order[k]];
            const std::uint32_t c = g0.cluster;
            float w = 0.0f;
            std::size_t j = k;
            while (j < order.size() && shaped->glyphs[order[j]].cluster == c) { w += shaped->glyphs[order[j]].advance; ++j; }
            const bool rtl = (g0.level & 1) != 0;
            const auto gs = graphemes_in(c, cluster_end(c));
            const std::size_t m = gs.empty() ? 1 : gs.size();
            for (std::size_t q = 0; q < m; ++q) {
                // In an RTL cluster the first grapheme is on the RIGHT.
                const std::size_t slot = rtl ? m - 1 - q : q;
                GraphemeBox gb;
                gb.begin = gs.empty() ? c : gstarts[gs[q]];
                gb.end = gs.empty() ? cluster_end(c) : gstarts[gs[q] + 1];
                gb.x0 = x + w * static_cast<float>(slot) / static_cast<float>(m);
                gb.x1 = x + w * static_cast<float>(slot + 1) / static_cast<float>(m);
                gb.rtl = rtl;
                out.push_back(gb);
            }
            x += w;
            k = j;
        }
        return out;
    }

    [[nodiscard]] float caret_x(std::string_view s, const TextStyle& st, std::size_t offset) const override {
        const auto boxes = grapheme_boxes(s, st);
        if (boxes.empty()) return 0.0f;
        // The caret before `offset` hugs the grapheme that STARTS there, on
        // its leading edge (left for LTR, right for RTL). At the end of the
        // text, the trailing edge of the last logical grapheme.
        for (const auto& b : boxes)
            if (b.begin == offset) return b.rtl ? b.x1 : b.x0;
        for (const auto& b : boxes)
            if (b.end == offset) return b.rtl ? b.x0 : b.x1;
        return 0.0f;
    }

    [[nodiscard]] std::size_t hit(std::string_view s, const TextStyle& st, float x) const override {
        const auto boxes = grapheme_boxes(s, st);
        if (boxes.empty()) return 0;
        // Left of everything / right of everything: the visual extremes.
        if (x <= boxes.front().x0) return boxes.front().rtl ? boxes.front().end : boxes.front().begin;
        for (const auto& b : boxes) {
            if (x >= b.x1) continue;
            const bool left_half = x < (b.x0 + b.x1) * 0.5f;
            // Left half of an LTR grapheme = before it; of an RTL one = after.
            return (left_half != b.rtl) ? b.begin : b.end;
        }
        return boxes.back().rtl ? boxes.back().begin : boxes.back().end;
    }

    [[nodiscard]] std::vector<std::pair<float, float>> selection_spans(std::string_view s, const TextStyle& st,
                                                                       std::size_t a, std::size_t b) const override {
        if (b < a) std::swap(a, b);
        std::vector<std::pair<float, float>> spans;
        if (a == b) return spans;
        // Every grapheme inside [a, b), merged where visually adjacent: a
        // logical selection across a direction change is 2+ rectangles.
        for (const auto& g : grapheme_boxes(s, st)) {
            if (g.begin < a || g.end > b) continue;
            if (!spans.empty() && std::abs(spans.back().second - g.x0) < 0.01f) spans.back().second = g.x1;
            else spans.push_back({g.x0, g.x1});
        }
        return spans;
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

        // UAX #14 opportunities, by byte offset. A glyph can start a line
        // when it is the first glyph of its cluster and the cluster's
        // offset is an opportunity (mandatory ones are newlines, handled
        // below). Thai/Lao/Khmer/Myanmar break between syllables, so long
        // lines in them wrap at all.
        std::vector<std::uint8_t> opportunity(text.size() + 1, 0);
        for (std::size_t o : seg::line_break_offsets(text, seg::Tailoring::sea_syllables))
            if (o < opportunity.size()) opportunity[o] = 1;
        const auto can_break_before = [&](std::size_t i) {
            const std::uint32_t c = shaped.glyphs[i].cluster;
            return c < opportunity.size() && opportunity[c] && shaped.glyphs[i - 1].cluster != c;
        };

        for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
            const auto& g = shaped.glyphs[i];

            if (is_newline(text, g.cluster)) {
                lines.push_back(Line{line_start, i, width});
                line_start = i + 1;
                last_break = std::string_view::npos;
                width = 0.0f;
                continue;
            }

            // Record a break opportunity BEFORE adding this glyph.
            if (i > line_start && can_break_before(i)) {
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
            std::uint16_t ell_gid = 0;
            // Ellipsis when: a non-wrapping line overflows, OR this is the last
            // line of a clamped block that hides further lines.
            const bool last_clamped_line = clamped && (li + 1 == line_count);
            const bool overflow_line = st.overflow == TextOverflow::ellipsis &&
                                       !wrapping && line.width > box.width() + 0.5f;
            if ((overflow_line || last_clamped_line) && box.width() > 0.0f) {
                ell_gid = stack_->ellipsis_glyph();
                ell_glyph = stack_->glyph(0, ell_gid, st.size, st.effective_weight(),
                                          {sp.axes.data(), sp.axis_count}, sp.italic);
                const float ell_w = ell_glyph ? ell_glyph->advance : st.size * 0.5f;
                const float budget = box.width() - ell_w;
                float kept = 0.0f;
                draw_end = line.begin;
                for (std::size_t i = line.begin; i < line.end && i < shaped.glyphs.size(); ++i) {
                    const float nx = kept + shaped.glyphs[i].advance;
                    if (nx > budget) break;
                    kept = nx;
                    draw_end = i + 1;
                }
                ellipsis_x = pen_x + kept;
                show_ellipsis = true;
            }

            const std::size_t line_end = std::min(draw_end, shaped.glyphs.size());
            std::vector<std::size_t> order;
            detail::visual_glyph_order(s, shaped, line.begin, line_end, order);
            for (const std::size_t i : order) {
                const auto& g = shaped.glyphs[i];
                if (g.glyph_id == 0 && g.advance == 0.0f) continue;   // newline marker

                place(dl, g.face_index, g.glyph_id, Vec2{pen_x, baseline_y} + g.offset, st, sp);
                pen_x += g.advance;
                if (word_gap > 0.0f && g.cluster < s.size() &&
                    (s[g.cluster] == ' ' || s[g.cluster] == '\t')) {
                    pen_x += word_gap;
                }
            }

            // Draw the ellipsis glyph after the truncated run.
            if (show_ellipsis && ell_glyph != nullptr && ell_glyph->size.x > 0.0f) {
                place(dl, 0, ell_gid, Vec2{ellipsis_x, baseline_y}, st, sp);
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
    /// Fetch and emit one glyph whose origin the shaper put at `pen`.
    ///
    /// A coverage glyph is drawn at the pen's whole pixel with the fraction
    /// rasterised into its bitmap (FontStack::subpixel_phases_for), so it is
    /// sharp AND where the shaper put it. An SDF glyph is resampled, but is
    /// placed on the SAME grid: x floored to 1/N px and, for both, the
    /// baseline on a whole device pixel, as CoreGraphics positions glyphs (x
    /// subpixel, y never), so coverage bitmaps are never resampled vertically
    /// and text sits on the rows macOS puts it on.
    /// Colour glyphs take the pen as is.
    void place(DrawList& dl, std::size_t face_index, std::uint16_t gid, Vec2 pen,
               const TextStyle& st, const ShapeParams& sp) const {
        const int phases = FontStack::subpixel_phases_for(st.size);
        const auto split = FontStack::split_pen(pen.x, phases);
        const CachedGlyph* cached = stack_->glyph(face_index, gid, st.size, st.effective_weight(),
                                                   {sp.axes.data(), sp.axis_count}, sp.italic,
                                                   split.phase, FontStack::is_dark_text(st.color));
        if (cached == nullptr || cached->size.x <= 0.0f) return;
        Vec2 at = pen;
        if (cached->is_sdf && !cached->is_color)
            at = Vec2{split.pixel + static_cast<float>(split.phase) / static_cast<float>(phases), num::round(pen.y)};
        else if (!cached->is_color)
            at = Vec2{split.pixel, num::round(pen.y)};
        emit(dl, *cached, at, st);
    }

    void emit(DrawList& dl, const CachedGlyph& g, Vec2 pen, const TextStyle& st) const {
        // An SDF entry was rasterised at one size and is reused at every
        // other, so its bearing and extent scale by the ratio between them.
        // A bitmap entry was made for this exact size, so the ratio is 1.
        // Every SDF entry is rasterised at its tier, sdf_raster_px(size), so
        // the ratio is size / tier whatever the face: deriving it from face
        // 0's units-per-em scaled glyphs of faces with another upem wrongly
        // (PingFang UI Display, 1064 upem behind SF's 2048, came out half
        // size from the 56 px SDF threshold up).
        float k = 1.0f;
        if (g.is_sdf && g.raster_scale > 0.0f) k = st.size / stack_->sdf_raster_px(st.size);

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
        // So the origin lands on the pixel grid and the size stays integral.
        // Horizontally that is exact: place() passes the pen's whole pixel
        // and the glyph's bitmap carries the fraction (a subpixel phase), and
        // the bearing of a coverage bitmap is integral, so the round() below
        // changes nothing in x. Vertically the baseline rounds to the grid.
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
            // Font smoothing: the same per-side growth the bitmap path bakes
            // into its outlines, as a threshold shift (per_px field units per
            // display pixel), so text keeps its weight across the threshold.
            const float grow_px = stack_->smoothing_px(st.size, FontStack::is_dark_text(st.color));
            const float grow = grow_px * per_px;
            // ...and CoreGraphics' vertical asymmetry: the top grows 2x, the
            // bottom not at all, i.e. the dilated glyph moves up by grow_px.
            dst.origin.y -= grow_px;
            dl.glyph(dst, g.uv, st.color, true, edge, grow);
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
