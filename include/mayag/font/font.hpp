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
#include "opentype.hpp"
#include "outline.hpp"
#include "raster.hpp"
#include "shape.hpp"

#include "../layout/text_metrics.hpp"
#include "../render/painter.hpp"
#include "../backend/software.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

        auto parsed = ot::FontFile::parse(face->data_, face_index, err);
        if (!parsed) return nullptr;

        face->file_    = std::move(*parsed);
        face->glyphs_  = GlyphSource{face->file_};
        face->kerning_ = KernTable{face->file_};
        face->id_      = next_id();

        if (!face->glyphs_.valid()) {
            if (err != nullptr) *err = ot::Error::unsupported_format;
            return nullptr;
        }
        return face;
    }

    [[nodiscard]] static std::shared_ptr<Face> from_file(const std::string& path,
                                                         std::uint32_t face_index = 0,
                                                         ot::Error* err = nullptr) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            if (err != nullptr) *err = ot::Error::too_small;
            return nullptr;
        }
        const auto size = static_cast<std::size_t>(f.tellg());
        f.seekg(0);
        std::vector<std::uint8_t> data(size);
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        return from_memory(std::move(data), face_index, err);
    }

    // ── identity ────────────────────────────────────────────────────────

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& family() const noexcept { return file_.family(); }
    [[nodiscard]] const std::string& subfamily() const noexcept { return file_.subfamily(); }
    [[nodiscard]] std::uint16_t weight() const noexcept { return file_.weight(); }
    [[nodiscard]] bool is_italic() const noexcept { return file_.is_italic(); }
    [[nodiscard]] bool is_bold() const noexcept { return file_.is_bold(); }
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

    /// Advance width of a glyph in pixels.
    [[nodiscard]] float advance(std::uint16_t gid, float size_px) const noexcept {
        return static_cast<float>(file_.advance(gid)) * scale_for(size_px);
    }

    /// Kerning between two glyphs in pixels.
    [[nodiscard]] float kern(std::uint16_t left, std::uint16_t right, float size_px) const noexcept {
        return kerning_.lookup(left, right) * scale_for(size_px);
    }

    [[nodiscard]] bool has_kerning() const noexcept { return !kerning_.empty(); }

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

  private:
    Face() = default;

    [[nodiscard]] static std::uint32_t next_id() {
        static std::atomic<std::uint32_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::uint8_t> data_;
    ot::FontFile              file_;
    GlyphSource               glyphs_;
    KernTable                 kerning_;
    std::uint32_t             id_ = 0;

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
    /// Sizes at or above this use SDF in hybrid mode.
    float      sdf_threshold = 20.0f;
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
    }

    /// Which face can render `codepoint`, and its glyph id.
    [[nodiscard]] std::pair<std::size_t, std::uint16_t>
    resolve(std::uint32_t codepoint) const noexcept {
        for (std::size_t i = 0; i < faces_.size(); ++i) {
            const std::uint16_t g = faces_[i]->glyph_for(codepoint);
            if (g != 0) return {i, g};
        }
        return {0, 0};
    }

    [[nodiscard]] FaceMetrics metrics(float size_px) const {
        return faces_.empty() ? FaceMetrics{} : faces_.front()->metrics(size_px);
    }

    // ── shaping ─────────────────────────────────────────────────────────

    [[nodiscard]] ShapeResult shape(std::string_view text,
                                    const ShapeParams& params) const override {
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
            const bool missing = (gid == 0);
            const Face& face = *faces_[face_idx];

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

        return out;
    }

    // ── glyph rasterisation and caching ─────────────────────────────────

    /// Fetch (rasterising on demand) the atlas entry for a glyph.
    [[nodiscard]] const CachedGlyph* glyph(std::size_t face_index, std::uint16_t gid,
                                           float size_px) {
        if (face_index >= faces_.size()) return nullptr;
        const Face& face = *faces_[face_index];

        const bool use_sdf = should_use_sdf(size_px);

        GlyphKey key;
        key.face_id  = face.id();
        key.glyph_id = gid;
        // The payoff of SDF: every size collapses to one bucket, so a page
        // with ten type sizes rasterises each glyph once instead of ten times.
        key.size_bucket = use_sdf
            ? 0
            : static_cast<std::uint16_t>(num::clamp(size_px * 4.0f, 1.0f, 65000.0f));
        key.flags = use_sdf ? glyph_flags::sdf : glyph_flags::none;

        if (const CachedGlyph* hit = atlas_.find(key)) return hit;

        const Outline o = face.outline(gid);
        const float advance = face.advance(gid, size_px);

        if (o.empty()) {
            // Blank (space) — cache the metrics so we stop re-checking.
            return atlas_.insert(key, Bitmap{}, Vec2{}, advance, 1.0f, 0.0f, use_sdf);
        }

        if (use_sdf) {
            const float raster_scale = face.scale_for(config_.sdf_raster_size);
            auto r = rasterize_sdf(o, raster_scale, config_.sdf_spread, 2);
            return atlas_.insert(key, r.bitmap, r.offset, advance,
                                 raster_scale, config_.sdf_spread, true);
        }

        const float scale = face.scale_for(size_px);
        auto r = rasterize(o, scale, 1);
        return atlas_.insert(key, r.bitmap, r.offset, advance, scale, 0.0f, false);
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
};

// ════════════════════════════════════════════════════════════════════════
// mayag integration
// ════════════════════════════════════════════════════════════════════════

/// The `layout::TextMeasurer` implementation. Layout asks this how big a
/// string is before there is a GPU or a window.
class StackMeasurer final : public layout::TextMeasurer {
  public:
    explicit StackMeasurer(const FontStack& stack) : stack_{&stack} {}

    [[nodiscard]] float advance(std::string_view s, const TextStyle& st) const override {
        ShapeParams p{st.size, st.letter_spacing, true, true};
        return stack_->shape(s, p).width;
    }

    [[nodiscard]] float ascent(const TextStyle& st) const override {
        return stack_->metrics(st.size).ascent;
    }

    [[nodiscard]] Vec2 measure(std::string_view s, const TextStyle& st,
                               float max_width) const override {
        const FaceMetrics fm = stack_->metrics(st.size);
        const float line_h = st.line_height > 0.0f ? st.size * st.line_height : fm.line_height;

        if (s.empty()) return {0.0f, line_h};

        ShapeParams p{st.size, st.letter_spacing, true, true};
        const ShapeResult shaped = stack_->shape(s, p);

        const bool wrapping = st.overflow == TextOverflow::wrap &&
                              max_width > 0.0f && !num::is_inf(max_width);

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
        float widest = 0.0f;
        for (const auto& l : lines) widest = num::max(widest, l.width);
        return {widest, static_cast<float>(num::max<std::size_t>(lines.size(), 1)) * line_h};
    }

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

        ShapeParams p{st.size, st.letter_spacing, true, true};
        const ShapeResult shaped = stack_->shape(s, p);

        const bool wrapping = st.overflow == TextOverflow::wrap && box.width() > 0.0f;
        const auto lines = wrapping
            ? StackMeasurer::wrap_lines(s, shaped, box.width())
            : std::vector<StackMeasurer::Line>{{0, shaped.glyphs.size(), shaped.width}};

        float baseline_y = box.top() + fm.ascent;

        for (const auto& line : lines) {
            // Horizontal alignment happens per LINE, which is the only way
            // centred multi-line text looks right.
            float pen_x = box.left();
            if (st.align == TextAlign::center)     pen_x += (box.width() - line.width) * 0.5f;
            else if (st.align == TextAlign::right) pen_x += box.width() - line.width;

            const float line_start_x = pen_x;

            for (std::size_t i = line.begin; i < line.end && i < shaped.glyphs.size(); ++i) {
                const auto& g = shaped.glyphs[i];
                if (g.glyph_id == 0 && g.advance == 0.0f) continue;   // newline marker

                const CachedGlyph* cached = stack_->glyph(g.face_index, g.glyph_id, st.size);
                if (cached != nullptr && cached->size.x > 0.0f) {
                    emit(dl, *cached, Vec2{pen_x, baseline_y} + g.offset, st);
                }
                pen_x += g.advance;
            }

            if (st.underline) {
                const float y = baseline_y - fm.underline_position;
                dl.line({line_start_x, y}, {line_start_x + line.width, y},
                        fm.underline_thickness, st.color);
            }
            if (st.strikethrough) {
                const float y = baseline_y - fm.x_height * 0.5f;
                dl.line({line_start_x, y}, {line_start_x + line.width, y},
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
            const Face* face = nullptr;
            for (std::size_t i = 0; i < 8; ++i) {
                face = stack_->face_at(i);
                if (face != nullptr) break;
            }
            if (face != nullptr) k = face->scale_for(st.size) / g.raster_scale;
        }

        const Rect dst{pen.x + g.bearing.x * k,
                       pen.y + g.bearing.y * k,
                       g.size.x * k,
                       g.size.y * k};

        dl.glyph(dst, g.uv, st.color, g.is_sdf);
    }

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

  private:
    const FontStack* stack_;
};

}  // namespace mayag::typo
