#pragma once
// mayag::image — a PNG decoder in ~300 lines, no dependencies
//
// The sibling of png.hpp's encoder. mayag needs to READ PNGs for two reasons:
// colour-bitmap emoji fonts (CBDT / sbix) store their glyphs as PNG, and an
// `image()` node should be able to load a real file. libpng+zlib would be the
// obvious dependency; mayag does not take dependencies, so this inflates the
// DEFLATE stream itself (RFC 1951: stored, fixed-Huffman, and dynamic-Huffman
// blocks) and reconstructs the scanlines through PNG's five filters.
//
// It handles the colour types real assets use — greyscale, RGB, RGBA, and
// palette, 8-bit — and always returns RGBA8 top-down, so every caller sees one
// format. Interlaced PNGs and 16-bit depth are rejected rather than
// mis-decoded; emoji strikes and UI assets are neither.

#include <cstdint>
#include <array>
#include <span>
#include <vector>

namespace mayag::image {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;   ///< width*height*4, top-down, 8-bit
    [[nodiscard]] bool ok() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

namespace detail {

// ── DEFLATE (RFC 1951) inflate ───────────────────────────────────────────
//
// A compact, allocation-light inflater. Canonical Huffman codes are decoded
// with a small per-table lookup built from the code lengths, which is all a
// PNG's two alphabets (literal/length and distance) need.

class BitReader {
  public:
    explicit BitReader(std::span<const std::uint8_t> d) : data_{d} {}

    /// Read `n` bits LSB-first (the DEFLATE bit order). Returns 0 past the end;
    /// callers check `overrun()` after a decode.
    [[nodiscard]] std::uint32_t bits(int n) noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (bit_pos_ == 0 && byte_pos_ >= data_.size()) { overrun_ = true; return v; }
            const std::uint32_t b = (data_[byte_pos_] >> bit_pos_) & 1u;
            v |= b << i;
            if (++bit_pos_ == 8) { bit_pos_ = 0; ++byte_pos_; }
        }
        return v;
    }

    void align_to_byte() noexcept { if (bit_pos_ != 0) { bit_pos_ = 0; ++byte_pos_; } }

    [[nodiscard]] std::uint8_t next_byte() noexcept {
        if (byte_pos_ >= data_.size()) { overrun_ = true; return 0; }
        return data_[byte_pos_++];
    }

    [[nodiscard]] bool overrun() const noexcept { return overrun_; }
    [[nodiscard]] std::size_t byte_pos() const noexcept { return byte_pos_; }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t byte_pos_ = 0;
    int         bit_pos_ = 0;
    bool        overrun_ = false;
};

/// A canonical Huffman decode table built from per-symbol code lengths.
struct Huffman {
    // Sorted-symbol decode via the classic "count / firstcode / offset" method.
    std::array<int, 16>          count{};   // codes of each length (1..15)
    std::vector<int>             symbol;     // symbols in canonical order
    int                          max_len = 0;

    void build(const std::vector<int>& lengths) {
        count.fill(0);
        for (int l : lengths) if (l > 0) { ++count[l]; max_len = std::max(max_len, l); }
        std::array<int, 16> offsets{};
        for (int l = 1; l < 16; ++l) offsets[l] = offsets[l - 1] + count[l - 1];
        symbol.assign(lengths.size(), 0);
        for (std::size_t s = 0; s < lengths.size(); ++s) {
            if (lengths[s] > 0) symbol[static_cast<std::size_t>(offsets[lengths[s]]++)] = static_cast<int>(s);
        }
    }

    /// Decode one symbol. Reads bits MSB-first WITHIN a code (DEFLATE spec):
    /// bits arrive LSB-first from the stream but a Huffman code is built up
    /// most-significant-bit first.
    [[nodiscard]] int decode(BitReader& br) const noexcept {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(br.bits(1));
            const int c = count[len];
            if (code - first < c) return symbol[static_cast<std::size_t>(index + (code - first))];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// Length/distance base tables (RFC 1951 §3.2.5).
inline constexpr int len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
inline constexpr int len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
inline constexpr int dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
inline constexpr int dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

[[nodiscard]] inline bool inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
    BitReader br{in};
    bool final_block = false;
    while (!final_block) {
        final_block = br.bits(1) != 0;
        const std::uint32_t type = br.bits(2);
        if (br.overrun()) return false;

        if (type == 0) {                       // stored
            br.align_to_byte();
            const std::uint32_t len = static_cast<std::uint32_t>(br.next_byte()) |
                                      (static_cast<std::uint32_t>(br.next_byte()) << 8);
            (void)br.next_byte(); (void)br.next_byte();   // ~len, ignored
            for (std::uint32_t i = 0; i < len; ++i) out.push_back(br.next_byte());
            if (br.overrun()) return false;
            continue;
        }
        if (type == 3) return false;           // reserved

        Huffman lit, dist;
        if (type == 1) {                       // fixed Huffman
            std::vector<int> ll(288);
            for (int i = 0;   i < 144; ++i) ll[i] = 8;
            for (int i = 144; i < 256; ++i) ll[i] = 9;
            for (int i = 256; i < 280; ++i) ll[i] = 7;
            for (int i = 280; i < 288; ++i) ll[i] = 8;
            lit.build(ll);
            dist.build(std::vector<int>(30, 5));
        } else {                               // dynamic Huffman
            const int hlit  = static_cast<int>(br.bits(5)) + 257;
            const int hdist = static_cast<int>(br.bits(5)) + 1;
            const int hclen = static_cast<int>(br.bits(4)) + 4;
            static constexpr int order[19] =
                {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<int> cl(19, 0);
            for (int i = 0; i < hclen; ++i) cl[static_cast<std::size_t>(order[i])] = static_cast<int>(br.bits(3));
            Huffman clh; clh.build(cl);

            std::vector<int> lengths;
            lengths.reserve(static_cast<std::size_t>(hlit + hdist));
            while (static_cast<int>(lengths.size()) < hlit + hdist) {
                const int sym = clh.decode(br);
                if (sym < 0 || br.overrun()) return false;
                if (sym < 16) { lengths.push_back(sym); }
                else if (sym == 16) {
                    if (lengths.empty()) return false;
                    const int rep = 3 + static_cast<int>(br.bits(2));
                    for (int i = 0; i < rep; ++i) lengths.push_back(lengths.back());
                } else if (sym == 17) {
                    const int rep = 3 + static_cast<int>(br.bits(3));
                    for (int i = 0; i < rep; ++i) lengths.push_back(0);
                } else {   // 18
                    const int rep = 11 + static_cast<int>(br.bits(7));
                    for (int i = 0; i < rep; ++i) lengths.push_back(0);
                }
            }
            if (static_cast<int>(lengths.size()) != hlit + hdist) return false;
            lit.build({lengths.begin(), lengths.begin() + hlit});
            dist.build({lengths.begin() + hlit, lengths.end()});
        }

        // Decode symbols into the output, resolving back-references.
        for (;;) {
            const int sym = lit.decode(br);
            if (sym < 0 || br.overrun()) return false;
            if (sym == 256) break;             // end of block
            if (sym < 256) { out.push_back(static_cast<std::uint8_t>(sym)); continue; }
            const int li = sym - 257;
            if (li >= 29) return false;
            const int length = len_base[li] + static_cast<int>(br.bits(len_extra[li]));
            const int ds = dist.decode(br);
            if (ds < 0 || ds >= 30 || br.overrun()) return false;
            const std::size_t distance =
                static_cast<std::size_t>(dist_base[ds]) + br.bits(dist_extra[ds]);
            if (distance == 0 || distance > out.size()) return false;
            const std::size_t start = out.size() - distance;
            for (int i = 0; i < length; ++i) out.push_back(out[start + static_cast<std::size_t>(i)]);
        }
    }
    return true;
}

// ── PNG scanline filters (RFC 2083 §6) ─────────────────────────────────────

[[nodiscard]] inline std::uint8_t paeth(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    return pb <= pc ? static_cast<std::uint8_t>(b) : static_cast<std::uint8_t>(c);
}

inline void unfilter(std::vector<std::uint8_t>& raw, int width, int height, int channels) {
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    std::size_t pos = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t filter = raw[pos++];
        std::uint8_t* row = raw.data() + pos;
        const std::uint8_t* prev = (y > 0) ? raw.data() + pos - stride - 1 : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= static_cast<std::size_t>(channels)
                              ? row[x - channels] : 0;
            const int b = prev ? prev[x] : 0;
            const int c = (prev && x >= static_cast<std::size_t>(channels))
                              ? prev[x - channels] : 0;
            std::uint8_t v = row[x];
            switch (filter) {
                case 0: break;                                    // None
                case 1: v = static_cast<std::uint8_t>(v + a); break;             // Sub
                case 2: v = static_cast<std::uint8_t>(v + b); break;             // Up
                case 3: v = static_cast<std::uint8_t>(v + (a + b) / 2); break;   // Average
                case 4: v = static_cast<std::uint8_t>(v + paeth(a, b, c)); break;// Paeth
                default: break;
            }
            row[x] = v;
        }
        pos += stride;
    }
}

inline std::uint32_t be32(std::span<const std::uint8_t> d, std::size_t o) noexcept {
    return (static_cast<std::uint32_t>(d[o]) << 24) | (static_cast<std::uint32_t>(d[o + 1]) << 16) |
           (static_cast<std::uint32_t>(d[o + 2]) << 8) | static_cast<std::uint32_t>(d[o + 3]);
}

}  // namespace detail

/// Decode PNG bytes to RGBA8 (top-down). Returns an image with ok()==false on
/// any malformed or unsupported input (interlaced, 16-bit) rather than
/// throwing — a bad emoji strike must degrade, not crash the text engine.
[[nodiscard]] inline DecodedImage decode_png(std::span<const std::uint8_t> png) {
    using namespace detail;
    DecodedImage img;
    if (png.size() < 8 || png[0] != 0x89 || png[1] != 'P' || png[2] != 'N' || png[3] != 'G') {
        return img;
    }

    int width = 0, height = 0, bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<std::uint8_t> idat;
    std::vector<std::uint8_t> palette;   // RGB triples
    std::vector<std::uint8_t> trns;      // palette alpha

    std::size_t pos = 8;
    while (pos + 8 <= png.size()) {
        const std::uint32_t len = be32(png, pos);
        const std::size_t type = pos + 4;
        const std::size_t body = pos + 8;
        if (body + len + 4 > png.size()) break;
        auto tag = [&](const char* s) {
            return png[type] == static_cast<std::uint8_t>(s[0]) &&
                   png[type + 1] == static_cast<std::uint8_t>(s[1]) &&
                   png[type + 2] == static_cast<std::uint8_t>(s[2]) &&
                   png[type + 3] == static_cast<std::uint8_t>(s[3]);
        };
        if (tag("IHDR") && len >= 13) {
            width      = static_cast<int>(be32(png, body));
            height     = static_cast<int>(be32(png, body + 4));
            bit_depth  = png[body + 8];
            color_type = png[body + 9];
            interlace  = png[body + 12];
        } else if (tag("PLTE")) {
            palette.assign(png.begin() + static_cast<std::ptrdiff_t>(body),
                           png.begin() + static_cast<std::ptrdiff_t>(body + len));
        } else if (tag("tRNS")) {
            trns.assign(png.begin() + static_cast<std::ptrdiff_t>(body),
                        png.begin() + static_cast<std::ptrdiff_t>(body + len));
        } else if (tag("IDAT")) {
            idat.insert(idat.end(), png.begin() + static_cast<std::ptrdiff_t>(body),
                        png.begin() + static_cast<std::ptrdiff_t>(body + len));
        } else if (tag("IEND")) {
            break;
        }
        pos = body + len + 4;   // skip CRC
    }

    // Support the 8-bit, non-interlaced colour types real assets use.
    if (width <= 0 || height <= 0 || bit_depth != 8 || interlace != 0) return img;
    int channels = 0;
    switch (color_type) {
        case 0: channels = 1; break;   // greyscale
        case 2: channels = 3; break;   // RGB
        case 3: channels = 1; break;   // palette (index)
        case 4: channels = 2; break;   // greyscale + alpha
        case 6: channels = 4; break;   // RGBA
        default: return img;
    }

    // zlib stream: 2-byte header, then DEFLATE, then a 4-byte Adler-32.
    if (idat.size() < 6) return img;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * channels + 1));
    if (!inflate({idat.data() + 2, idat.size() - 2}, raw)) return img;

    const std::size_t expected = static_cast<std::size_t>(height) * (width * channels + 1);
    if (raw.size() < expected) return img;

    unfilter(raw, width, height, channels);

    // Expand to RGBA8.
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t src_stride = static_cast<std::size_t>(width) * channels;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = raw.data() + 1 + static_cast<std::size_t>(y) * (src_stride + 1);
        std::uint8_t* dst = img.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            std::uint8_t r = 0, g = 0, b = 0, a = 255;
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * channels;
            switch (color_type) {
                case 0: r = g = b = px[0]; break;
                case 2: r = px[0]; g = px[1]; b = px[2]; break;
                case 3: {
                    const std::size_t idx = px[0];
                    if (idx * 3 + 2 < palette.size()) {
                        r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2];
                    }
                    a = idx < trns.size() ? trns[idx] : 255;
                    break;
                }
                case 4: r = g = b = px[0]; a = px[1]; break;
                case 6: r = px[0]; g = px[1]; b = px[2]; a = px[3]; break;
                default: break;
            }
            dst[x * 4 + 0] = r; dst[x * 4 + 1] = g; dst[x * 4 + 2] = b; dst[x * 4 + 3] = a;
        }
    }
    return img;
}

}  // namespace mayag::image
