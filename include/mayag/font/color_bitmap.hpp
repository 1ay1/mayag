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
                                           std::uint16_t gid) {
    BitmapLoc loc;
    if (cblc.size() < 8) return loc;

    // CBLC header: version (u32) + numSizes (u32), then bitmapSizeTable[].
    const std::uint32_t num_sizes = u32(cblc, 4);
    // Each BitmapSize record is 48 bytes.
    for (std::uint32_t s = 0; s < num_sizes; ++s) {
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
        auto img = image::decode_png(png);
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
/// originY i16, graphicType u32 ('png '), then image bytes}.
[[nodiscard]] inline ColorBitmap decode_sbix(std::span<const std::uint8_t> sbix,
                                             std::uint16_t gid, std::uint16_t num_glyphs) {
    ColorBitmap out;
    if (sbix.size() < 8) return out;
    const std::uint32_t num_strikes = u32(sbix, 4);
    // Pick the largest strike (last offset is usually the biggest ppem).
    std::size_t best_strike_off = 0; int best_ppem = 0;
    for (std::uint32_t s = 0; s < num_strikes; ++s) {
        const std::uint32_t so = u32(sbix, 8 + s * 4);
        if (so + 4 > sbix.size()) continue;
        const int ppem = u16(sbix, so);   // strike ppem
        if (ppem >= best_ppem) { best_ppem = ppem; best_strike_off = so; }
    }
    if (best_strike_off == 0) return out;

    // Strike: ppem u16, resolution u16, then glyphDataOffset[numGlyphs+1] u32.
    const std::size_t goff = best_strike_off + 4 + static_cast<std::size_t>(gid) * 4;
    const std::uint32_t o0 = u32(sbix, goff);
    const std::uint32_t o1 = u32(sbix, goff + 4);
    (void)num_glyphs;
    if (o1 <= o0 + 8) return out;
    const std::size_t entry = best_strike_off + o0;
    const std::uint32_t gtype = u32(sbix, entry + 4);
    if (gtype != ot::tag("png ")) return out;
    const std::size_t png_off = entry + 8;
    const std::size_t png_len = o1 - o0 - 8;
    if (png_off + png_len > sbix.size()) return out;
    auto img = image::decode_png({sbix.data() + png_off, png_len});
    if (!img.ok()) return out;
    out.width = img.width; out.height = img.height; out.rgba = std::move(img.rgba);
    out.ppem = best_ppem;
    out.bearing_x = static_cast<float>(static_cast<std::int16_t>(u16(sbix, entry)));      // originX
    out.bearing_y = static_cast<float>(static_cast<std::int16_t>(u16(sbix, entry + 2)));  // originY
    return out;
}

}  // namespace colordetail

/// Bilinearly resample a straight-RGBA image to a new size. Emoji strikes are
/// authored at ~128 px and usually drawn smaller, so a good downscale matters
/// more than speed; bilinear is the right cost/quality point here.
[[nodiscard]] inline std::vector<std::uint8_t> scale_rgba(
        const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return out;
    const float fx = static_cast<float>(sw) / static_cast<float>(dw);
    const float fy = static_cast<float>(sh) / static_cast<float>(dh);
    for (int y = 0; y < dh; ++y) {
        const float syf = (static_cast<float>(y) + 0.5f) * fy - 0.5f;
        int y0 = static_cast<int>(syf); float ty = syf - static_cast<float>(y0);
        if (y0 < 0) { y0 = 0; ty = 0.0f; }
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        for (int x = 0; x < dw; ++x) {
            const float sxf = (static_cast<float>(x) + 0.5f) * fx - 0.5f;
            int x0 = static_cast<int>(sxf); float tx = sxf - static_cast<float>(x0);
            if (x0 < 0) { x0 = 0; tx = 0.0f; }
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            std::uint8_t* d = out.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int px, int py) {
                    return static_cast<float>(src[(static_cast<std::size_t>(py) * sw + px) * 4 + c]);
                };
                const float top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
                const float bot = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
                d[c] = static_cast<std::uint8_t>(top * (1 - ty) + bot * ty + 0.5f);
            }
        }
    }
    return out;
}

/// Read the colour bitmap for `gid` from a parsed font, if it has one.
/// Returns an invalid ColorBitmap when the font has no colour bitmap tables or
/// the glyph has no strike, so the caller falls back to the outline path.
[[nodiscard]] inline ColorBitmap color_bitmap_for(const ot::FontFile& file,
                                                  std::uint16_t gid) {
    using namespace colordetail;
    const float upem = static_cast<float>(file.units_per_em());

    // CBLC/CBDT (Google — NotoColorEmoji).
    const auto cblc = file.table(tags::cblc);
    const auto cbdt = file.table(tags::cbdt);
    if (!cblc.empty() && !cbdt.empty()) {
        const BitmapLoc loc = locate_cblc(cblc, gid);
        if (loc.found && loc.cbdt_offset + loc.length <= cbdt.size() && loc.length > 0) {
            std::span<const std::uint8_t> entry{cbdt.data() + loc.cbdt_offset, loc.length};
            ColorBitmap bmp = decode_cbdt_entry(entry, loc.ppem, upem);
            if (bmp.valid()) return bmp;
        }
    }

    // sbix (Apple — Apple Color Emoji).
    const auto sbix = file.table(tags::sbix);
    if (!sbix.empty()) {
        ColorBitmap bmp = decode_sbix(sbix, gid, file.num_glyphs());
        if (bmp.valid()) return bmp;
    }

    return {};
}

}  // namespace mayag::typo
