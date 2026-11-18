#pragma once
// mayag::typo::colorbitmap — colour glyph bitmaps (CBLC/CBDT, sbix)
//
// Emoji fonts do not store outlines. NotoColorEmoji is a stack of PNG images,
// one per emoji, indexed by CBLC (the location table) into CBDT (the data).
// Apple Color Emoji uses sbix, which is the same idea with a different header.
// mayag's outline rasteriser cannot draw these — there is nothing to
// rasterise — so this reads the PNG for a glyph and hands back RGBA pixels the
// atlas can store and the shader can sample directly.
//
// Only the format real emoji fonts actually use is decoded: CBLC index
// subtable formats 1 and 3 (variable/constant offsets into CBDT), CBDT image
// format 17/18/19 (small/big-glyph-metrics-prefixed PNG), and sbix PNG
// strikes. Everything else returns "no bitmap" and the face falls through to
// blank-but-spaced, which is still better than tofu.

#include "opentype.hpp"
#include "../image/png_decode.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace mayag::typo {

/// A decoded colour glyph: RGBA pixels plus the design-space placement the
/// shaper needs to position it against the baseline.
struct ColorBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;   ///< width*height*4, straight (un-premultiplied)
    float bearing_x = 0.0f;           ///< left side bearing, in the bitmap's px
    float bearing_y = 0.0f;           ///< top above baseline, in the bitmap's px
    float advance   = 0.0f;           ///< advance width, in the bitmap's px
    int   ppem      = 0;              ///< pixels-per-em the strike was authored at

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

namespace colordetail {

namespace tags {
inline constexpr std::uint32_t cblc = ot::tag("CBLC");
inline constexpr std::uint32_t cbdt = ot::tag("CBDT");
inline constexpr std::uint32_t sbix = ot::tag("sbix");
}

inline std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return o + 2 <= d.size() ? static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]) : 0;
}
inline std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    if (o + 4 > d.size()) return 0;
    return (static_cast<std::uint32_t>(d[o]) << 24) | (static_cast<std::uint32_t>(d[o + 1]) << 16) |
           (static_cast<std::uint32_t>(d[o + 2]) << 8) | static_cast<std::uint32_t>(d[o + 3]);
}
inline std::int8_t s8(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return o < d.size() ? static_cast<std::int8_t>(d[o]) : 0;
}

/// Find, in the CBDT data, the PNG byte range and metrics for `gid`, using the
/// CBLC index. Returns {offset, length} into CBDT, or {0,0} if absent.
struct BitmapLoc {
    std::size_t cbdt_offset = 0;
    std::size_t length = 0;
    int         ppem = 0;
    float       bearing_x = 0, bearing_y = 0, advance = 0;
    bool        found = false;
};

[[nodiscard]] inline BitmapLoc locate_cblc(std::span<const std::uint8_t> cblc,
                                           std::uint16_t gid, int want_ppem = 1 << 30) {
    BitmapLoc loc;
    if (cblc.size() < 8) return loc;

    // CBLC header: version (u32) + numSizes (u32), then bitmapSizeTable[].
    // HarfBuzz CBLC::choose_strike: the smallest strike whose ppem is at
    // least `want_ppem`, else the largest; at most 256 strikes considered.
    const std::uint32_t num_sizes = std::min<std::uint32_t>(u32(cblc, 4), 256);
    if (num_sizes == 0 || 8 + 48 > cblc.size()) return loc;
    const auto ppem_of = [&](std::uint32_t s) {
        const std::size_t rec = 8 + static_cast<std::size_t>(s) * 48;
        return rec + 48 <= cblc.size() ? std::max<int>(cblc[rec + 44], cblc[rec + 45]) : 0;
    };
    const unsigned want = want_ppem > 0 ? static_cast<unsigned>(want_ppem) : 1u << 30;
    std::uint32_t best = 0;
    unsigned best_ppem = static_cast<unsigned>(ppem_of(0));
    for (std::uint32_t s = 1; s < num_sizes; ++s) {
        const unsigned p = static_cast<unsigned>(ppem_of(s));
        if ((want <= p && p < best_ppem) || (want > best_ppem && p > best_ppem)) { best = s; best_ppem = p; }
    }
    // Each BitmapSize record is 48 bytes.
    for (std::uint32_t s = best; s == best; ++s) {
        const std::size_t rec = 8 + static_cast<std::size_t>(s) * 48;
        if (rec + 48 > cblc.size()) break;
        const std::uint32_t idx_array_off = u32(cblc, rec + 0);   // indexSubTableArrayOffset
        const std::uint32_t num_subtables = u32(cblc, rec + 8);   // numberOfIndexSubTables
        // ppemX at byte 44, ppemY at 45 (uint8 each); bit depth at 46.
        const int ppem = cblc[rec + 44] ? cblc[rec + 44] : 128;

        // IndexSubTableArray: numberOfIndexSubTables entries of
        //   { firstGlyphIndex u16, lastGlyphIndex u16, additionalOffset u32 }.
        for (std::uint32_t t = 0; t < num_subtables; ++t) {
            const std::size_t a = idx_array_off + static_cast<std::size_t>(t) * 8;
            const std::uint16_t first = u16(cblc, a + 0);
            const std::uint16_t last  = u16(cblc, a + 2);
            const std::uint32_t add   = u32(cblc, a + 4);
            if (gid < first || gid > last) continue;

            // The IndexSubTable itself lives at idx_array_off + add.
            const std::size_t h = idx_array_off + add;
            const std::uint16_t index_format = u16(cblc, h + 0);
            const std::uint16_t image_format = u16(cblc, h + 2);
            const std::uint32_t image_data_off = u32(cblc, h + 4);
            (void)image_format;

            const std::uint16_t k = static_cast<std::uint16_t>(gid - first);
            if (index_format == 1) {
                // u32 offsets[last-first+2], into CBDT after imageDataOffset.
                const std::size_t off_pos = h + 8 + static_cast<std::size_t>(k) * 4;
                const std::uint32_t o0 = u32(cblc, off_pos);
                const std::uint32_t o1 = u32(cblc, off_pos + 4);
                if (o1 <= o0) return loc;
                loc.cbdt_offset = image_data_off + o0;
                loc.length = o1 - o0;
                loc.ppem = ppem;
                loc.found = true;
                return loc;
            } else if (index_format == 3) {
                // u16 offsets (short version of format 1).
                const std::size_t off_pos = h + 8 + static_cast<std::size_t>(k) * 2;
                const std::uint16_t o0 = u16(cblc, off_pos);
                const std::uint16_t o1 = u16(cblc, off_pos + 2);
                if (o1 <= o0) return loc;
                loc.cbdt_offset = image_data_off + o0;
                loc.length = o1 - o0;
                loc.ppem = ppem;
                loc.found = true;
                return loc;
            }
            // Formats 2/4/5 (constant-size / sparse) are not used by the emoji
            // fonts mayag targets; fall through to "not found".
            return loc;
        }
    }
    return loc;
}

/// A CBDT glyph record is a small/big glyph-metrics header followed by the PNG.
/// Formats 17 (small metrics) and 18/19 (big metrics) all end in PNG bytes;
/// we skip the metrics and read the PNG length + data.
[[nodiscard]] inline ColorBitmap decode_cbdt_entry(std::span<const std::uint8_t> entry,
                                                   int ppem, float upem) {
    ColorBitmap out;
    if (entry.size() < 5) return out;

    // Format 17: SmallGlyphMetrics (5 bytes) + dataLen(u32) + PNG.
    // Format 18/19: BigGlyphMetrics (8 bytes) + dataLen(u32) + PNG.
    // We detect by trying small first (the common NotoColorEmoji case) and
    // validate the PNG signature; if it fails, try big.
    auto try_at = [&](std::size_t metrics_len, float bx, float by, float adv) -> bool {
        if (entry.size() < metrics_len + 4) return false;
        const std::uint32_t data_len = u32(entry, metrics_len);
        const std::size_t png_off = metrics_len + 4;
        if (png_off + data_len > entry.size() || data_len < 8) return false;
        std::span<const std::uint8_t> png{entry.data() + png_off, data_len};
        if (png[0] != 0x89 || png[1] != 'P') return false;
        auto img = imageio::decode_png(png);
        if (!img.ok()) return false;
        out.width = img.width; out.height = img.height; out.rgba = std::move(img.rgba);
        out.ppem = ppem;
        out.bearing_x = bx; out.bearing_y = by; out.advance = adv;
        return true;
    };

    // SmallGlyphMetrics: height u8, width u8, bearingX i8, bearingY i8, advance u8.
    const float s_bx = s8(entry, 2), s_by = s8(entry, 3), s_adv = entry[4];
    if (try_at(5, s_bx, s_by, s_adv)) { (void)upem; return out; }

    // BigGlyphMetrics: height u8, width u8, horiBearingX i8, horiBearingY i8,
    // horiAdvance u8, then vertical metrics (3 bytes) = 8 bytes total.
    const float b_bx = s8(entry, 2), b_by = s8(entry, 3), b_adv = entry[4];
    try_at(8, b_bx, b_by, b_adv);
    return out;
}

/// sbix: strikes[] each a table of glyph-offset u32s; entry = {originX i16,
/// originY i16, graphicType u32 ('png ' or 'dupe'), then image bytes}.
/// HarfBuzz rules (OT/Color/sbix): the smallest strike whose ppem is at least
/// `want_ppem` (else the largest), `dupe` entries redirect to another glyph
/// up to 8 times, and the image's bottom-left sits at (originX, originY).
[[nodiscard]] inline ColorBitmap decode_sbix(std::span<const std::uint8_t> sbix,
                                             std::uint16_t gid, std::uint16_t num_glyphs,
                                             int want_ppem = 1 << 30) {
    ColorBitmap out;
    if (sbix.size() < 8) return out;
    const std::uint32_t num_strikes = u32(sbix, 4);
    std::size_t best_strike_off = 0; int best_ppem = 0;          // smallest >= want
    std::size_t max_strike_off = 0; int max_ppem = 0;            // fallback: largest
    for (std::uint32_t s = 0; s < num_strikes && 8 + s * 4 + 4 <= sbix.size(); ++s) {
        const std::uint32_t so = u32(sbix, 8 + s * 4);
        if (so + 4 > sbix.size()) continue;
        const int ppem = u16(sbix, so);
        if (ppem == 0) continue;
        if (ppem > max_ppem) { max_ppem = ppem; max_strike_off = so; }
        if (ppem >= want_ppem && (best_ppem == 0 || ppem < best_ppem)) { best_ppem = ppem; best_strike_off = so; }
    }
    if (best_strike_off == 0) { best_strike_off = max_strike_off; best_ppem = max_ppem; }
    if (best_strike_off == 0) return out;

    // Strike: ppem u16, resolution u16, then glyphDataOffset[numGlyphs+1] u32.
    std::size_t entry = 0, png_len = 0;
    for (int retry = 0; retry <= 8; ++retry) {
        if (gid >= num_glyphs) return out;
        const std::size_t goff = best_strike_off + 4 + static_cast<std::size_t>(gid) * 4;
        if (goff + 8 > sbix.size()) return out;
        const std::uint32_t o0 = u32(sbix, goff), o1 = u32(sbix, goff + 4);
        if (o1 <= o0 + 8 || best_strike_off + o1 > sbix.size()) return out;
        entry = best_strike_off + o0;
        const std::uint32_t gtype = u32(sbix, entry + 4);
        if (gtype == ot::tag("dupe")) {
            if (o1 - o0 - 8 < 2) return out;
            gid = u16(sbix, entry + 8);
            continue;
        }
        if (gtype != ot::tag("png ")) return out;
        png_len = o1 - o0 - 8;
        break;
    }
    if (png_len == 0) return out;
    const std::size_t png_off = entry + 8;
    auto img = imageio::decode_png({sbix.data() + png_off, png_len});
    if (!img.ok()) return out;
    out.width = img.width; out.height = img.height; out.rgba = std::move(img.rgba);
    const int img_height = out.height;
    out.ppem = best_ppem;
    out.bearing_x = static_cast<float>(static_cast<std::int16_t>(u16(sbix, entry)));      // originX
    // originOffsetY places the bitmap's BOTTOM-left corner relative to the
    // origin (y up), so its top sits height + originY above the baseline
    // (HarfBuzz sbix::get_png_extents). Using originY as the top dropped
    // every Apple Color Emoji glyph a full em below the baseline.
    out.bearing_y = static_cast<float>(img_height + static_cast<std::int16_t>(u16(sbix, entry + 2)));
    return out;
}

}  // namespace colordetail

/// Resample a straight-RGBA image to a new size exactly as CoreText draws a
/// scaled emoji strike (CGContextDrawImage, kCGInterpolationHigh): shrinking
/// is a separable Catmull-Rom whose support widens with the reduction, so
/// every source pixel contributes (a 160 px flag's 1 px border survives at
/// 13 px); enlarging is an eased bilinear. Within 0.3/255 of CoreGraphics.
/// Done in PREMULTIPLIED space so transparent edges do not bleed their
/// undefined colour into the image; results are clamped (bicubic rings).
[[nodiscard]] inline std::vector<std::uint8_t> scale_rgba(
        const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return out;
    const auto keys = [](float x) {
        constexpr float a = -0.5f;
        x = std::fabs(x);
        if (x < 1.0f) return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
        if (x < 2.0f) return (((x - 5.0f) * x + 8.0f) * x - 4.0f) * a;
        return 0.0f;
    };
    // CoreGraphics' kCGInterpolationHigh, measured texel by texel (a one-hot
    // row through CGContextDrawImage, 8 size pairs from 20->18 to 160->20):
    // Catmull-Rom whose sample CENTRES are corner-aligned, output i sampling
    // source i * (in-1)/(out-1) (texel centres to texel centres, so the
    // first and last output pixels sit exactly on the first and last
    // texels), its support widened by that same step when shrinking, and
    // edge texels clamped. Mean per-row error 0.056 vs 0.26 for PIL's
    // pixel-area-aligned bicubic: the old fit drew every small emoji a
    // quarter pixel too soft at its edges.
    // Per output index: a [first, first+n) window and normalised weights.
    struct Taps { int first = 0; std::vector<float> w; };
    const auto taps = [&](int in, int outn) {
        std::vector<Taps> t(static_cast<std::size_t>(outn));
        if (outn > in) {
            // Enlarging is not cubic at all: CoreGraphics interpolates
            // linearly between the two nearest texels (area-aligned centres,
            // clamped at the edges), with the fraction snapped to eighths and
            // eased through this table -- measured exactly, 6 size pairs.
            static constexpr float ease[9] = {0.0f, 0.0625f, 0.125f, 0.25f, 0.5f, 0.75f, 0.875f, 0.9375f, 1.0f};
            for (int i = 0; i < outn; ++i) {
                const float c = std::clamp((static_cast<float>(i) + 0.5f) * static_cast<float>(in) / static_cast<float>(outn) - 0.5f,
                                           0.0f, static_cast<float>(in - 1));
                const int k = std::min(static_cast<int>(c), in - 1);
                const int q = static_cast<int>(std::ceil((c - static_cast<float>(k)) * 8.0f - 0.5f));
                const float w = ease[std::clamp(q, 0, 8)];
                Taps& tp = t[static_cast<std::size_t>(i)];
                tp.first = k;
                tp.w = k + 1 < in ? std::vector<float>{1.0f - w, w} : std::vector<float>{1.0f};
            }
            return t;
        }
        const float scale = outn > 1 ? static_cast<float>(in - 1) / static_cast<float>(outn - 1)
                                     : static_cast<float>(in);
        const float fscale = std::max(scale, 1.0f);
        const float support = 2.0f * fscale;
        for (int i = 0; i < outn; ++i) {
            const float centre = outn > 1 ? static_cast<float>(i) * scale + 0.5f : 0.5f * static_cast<float>(in);
            const int lo = static_cast<int>(std::floor(centre - support - 0.5f));
            const int hi = static_cast<int>(std::ceil(centre + support + 0.5f));
            Taps& tp = t[static_cast<std::size_t>(i)];
            const int first = std::clamp(lo, 0, in - 1), last = std::clamp(hi, 0, in - 1);
            tp.first = first;
            tp.w.assign(static_cast<std::size_t>(last - first + 1), 0.0f);
            float total = 0;
            for (int x = lo; x <= hi; ++x) {
                const float w = keys((static_cast<float>(x) - centre + 0.5f) / fscale);
                if (w == 0.0f) continue;
                tp.w[static_cast<std::size_t>(std::clamp(x, 0, in - 1) - first)] += w;   // edge clamp
                total += w;
            }
            if (total != 0) for (float& w : tp.w) w /= total;
        }
        return t;
    };
    const auto tx = taps(sw, dw), ty = taps(sh, dh);
    // premultiply into floats
    std::vector<float> pm(static_cast<std::size_t>(sw) * sh * 4);
    for (std::size_t i = 0; i < pm.size(); i += 4) {
        const float a = static_cast<float>(src[i + 3]) / 255.0f;
        pm[i + 0] = static_cast<float>(src[i + 0]) * a;
        pm[i + 1] = static_cast<float>(src[i + 1]) * a;
        pm[i + 2] = static_cast<float>(src[i + 2]) * a;
        pm[i + 3] = static_cast<float>(src[i + 3]);
    }
    // horizontal pass
    std::vector<float> mid(static_cast<std::size_t>(dw) * sh * 4, 0.0f);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < dw; ++x) {
            const Taps& tp = tx[static_cast<std::size_t>(x)];
            float* m = &mid[(static_cast<std::size_t>(y) * dw + x) * 4];
            for (std::size_t k = 0; k < tp.w.size(); ++k) {
                const float* s = &pm[(static_cast<std::size_t>(y) * sw + tp.first + static_cast<int>(k)) * 4];
                for (int c = 0; c < 4; ++c) m[c] += s[c] * tp.w[k];
            }
        }
    // vertical pass, unpremultiply, clamp
    for (int y = 0; y < dh; ++y) {
        const Taps& tp = ty[static_cast<std::size_t>(y)];
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < tp.w.size(); ++k) {
                const float* s = &mid[(static_cast<std::size_t>(tp.first + static_cast<int>(k)) * dw + x) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * tp.w[k];
            }
            std::uint8_t* d = out.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            d[3] = static_cast<std::uint8_t>(a + 0.5f);
            for (int c = 0; c < 3; ++c)
                d[c] = a > 0.5f ? static_cast<std::uint8_t>(std::clamp(acc[c] / (a / 255.0f), 0.0f, 255.0f) + 0.5f) : 0;
        }
    }
    return out;
}

/// Read the colour bitmap for `gid` from a parsed font, if it has one.
/// Returns an invalid ColorBitmap when the font has no colour bitmap tables or
/// the glyph has no strike, so the caller falls back to the outline path.
/// `want_ppem`: the device pixels per em it will be drawn at (strike choice).
[[nodiscard]] inline ColorBitmap color_bitmap_for(const ot::FontFile& file,
                                                  std::uint16_t gid, int want_ppem = 1 << 30) {
    using namespace colordetail;
    const float upem = static_cast<float>(file.units_per_em());

    // CBLC/CBDT (Google — NotoColorEmoji).
    const auto cblc = file.table(tags::cblc);
    const auto cbdt = file.table(tags::cbdt);
    if (!cblc.empty() && !cbdt.empty()) {
        const BitmapLoc loc = locate_cblc(cblc, gid, want_ppem);
        if (loc.found && loc.cbdt_offset + loc.length <= cbdt.size() && loc.length > 0) {
            std::span<const std::uint8_t> entry{cbdt.data() + loc.cbdt_offset, loc.length};
            ColorBitmap bmp = decode_cbdt_entry(entry, loc.ppem, upem);
            if (bmp.valid()) return bmp;
        }
    }

    // sbix (Apple — Apple Color Emoji).
    const auto sbix = file.table(tags::sbix);
    if (!sbix.empty()) {
        ColorBitmap bmp = decode_sbix(sbix, gid, file.num_glyphs(), want_ppem);
        if (bmp.valid()) return bmp;
    }

    return {};
}

}  // namespace mayag::typo
