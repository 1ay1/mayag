#pragma once
// mayag::image — a PNG encoder in ~150 lines, no dependencies
//
// mayag can render to a file on any machine with a C++ compiler and nothing
// else. That is what makes golden-image testing, CI screenshots, and headless
// thumbnail generation work without dragging in libpng/zlib.
//
// It emits a valid deflate stream using STORED (uncompressed) blocks. Files
// are larger than a real encoder's, but they are correct PNGs readable by
// every viewer, and the code has no compression state machine to get wrong.

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace mayag::image {

namespace detail {

/// CRC-32 (PNG chunk checksum), table built once on first use.
inline std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    for (std::uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc;
}

/// Adler-32 (zlib stream checksum).
inline std::uint32_t adler32(std::span<const std::uint8_t> data) {
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
                      std::span<const std::uint8_t> payload) {
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_begin = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
    out.insert(out.end(), payload.begin(), payload.end());
    const std::uint32_t crc = crc32({out.data() + crc_begin, out.size() - crc_begin});
    put_u32(out, crc ^ 0xFFFFFFFFu);
}

/// Wrap raw bytes in a zlib stream of stored deflate blocks.
inline std::vector<std::uint8_t> zlib_store(std::span<const std::uint8_t> raw) {
    std::vector<std::uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);   // CMF: deflate, 32K window
    z.push_back(0x01);   // FLG: no dict, fastest — makes (CMF<<8|FLG) % 31 == 0

    std::size_t offset = 0;
    while (offset < raw.size() || raw.empty()) {
        const std::size_t block = std::min<std::size_t>(raw.size() - offset, 65535);
        const bool final_block = (offset + block >= raw.size());
        z.push_back(final_block ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(block & 0xFF));
        z.push_back(static_cast<std::uint8_t>(block >> 8));
        z.push_back(static_cast<std::uint8_t>(~block & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~block >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + block);
        offset += block;
        if (final_block) break;
    }
    put_u32(z, adler32(raw));
    return z;
}

}  // namespace detail

/// Encode an RGBA8 buffer (row-major, top-down, 4 bytes/pixel) as PNG bytes.
[[nodiscard]] inline std::vector<std::uint8_t> encode_png(std::span<const std::uint8_t> rgba,
                                                          int width, int height) {
    // PNG requires a filter byte per scanline; 0 = None.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 4 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
    }

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<std::uint8_t> ihdr;
    detail::put_u32(ihdr, static_cast<std::uint32_t>(width));
    detail::put_u32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type: RGBA
    ihdr.push_back(0);   // deflate
    ihdr.push_back(0);   // adaptive filtering
    ihdr.push_back(0);   // no interlace
    detail::put_chunk(png, "IHDR", ihdr);

    const auto idat = detail::zlib_store(raw);
    detail::put_chunk(png, "IDAT", idat);
    detail::put_chunk(png, "IEND", {});

    return png;
}

/// Write an RGBA8 buffer to a .png file. Returns false on any I/O failure.
[[nodiscard]] inline bool write_png(const std::string& path, std::span<const std::uint8_t> rgba,
                                    int width, int height) {
    const auto bytes = encode_png(rgba, width, height);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const std::size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return wrote == bytes.size();
}

/// Write a PPM — trivially parseable, useful when debugging the PNG path itself.
[[nodiscard]] inline bool write_ppm(const std::string& path, std::span<const std::uint8_t> rgba,
                                    int width, int height) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        std::fputc(rgba[i * 4 + 0], f);
        std::fputc(rgba[i * 4 + 1], f);
        std::fputc(rgba[i * 4 + 2], f);
    }
    std::fclose(f);
    return true;
}

}  // namespace mayag::image
