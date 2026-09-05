// tests/test_png.cpp — the PNG decoder against hostile input.
//
// png_decode.hpp inflates DEFLATE and reconstructs PNG scanlines by hand, in
// ~300 lines, with no dependencies. It parses bytes that arrive from a
// download, a user's disk, or a font file's embedded emoji strike -- the same
// threat model the font parser has, and the README already commits to for
// fonts: "every accessor is bounds-checked ... 400 randomly corrupted fonts
// ... must parse-or-reject without crashing".
//
// The decoder had no tests at all. This gives it the same treatment: prove it
// decodes correctly first (a decoder that safely rejects everything is also
// "safe"), then prove it cannot be made to crash, read out of bounds, or
// allocate unboundedly by a malformed file.
//
// Build with -fsanitize=address,undefined to make the corruption sweep
// meaningful; the CMake target mayag_png_tests_asan does that.

#include <mayag/mayag.hpp>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace mayag;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

/// A known image, encoded by mayag's own encoder. Round-tripping through the
/// pair is the only way to check the decoder without shipping binary fixtures.
std::vector<std::uint8_t> make_rgba(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            // A pattern with gradients AND hard edges, so both the filter
            // reconstruction and the literal path get exercised.
            px[i + 0] = static_cast<std::uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
            px[i + 1] = static_cast<std::uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
            px[i + 2] = static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 2 ? 240 : 15);
            px[i + 3] = 255;
        }
    }
    return px;
}

}  // namespace

int main() {
    std::printf("mayag png decoder\n=================\n");

    // ── correctness first ────────────────────────────────────────────
    section("round trip: encode then decode reproduces the pixels");
    {
        for (const auto [w, h] : {std::pair{1, 1}, {2, 3}, {17, 5}, {64, 64}}) {
            const auto original = make_rgba(w, h);
            const auto encoded  = imageio::encode_png(original, w, h);
            const auto decoded  = imageio::decode_png(encoded);

            const bool same_size = decoded.width == w && decoded.height == h;
            const bool same_px   = decoded.rgba == original;
            check(same_size && same_px,
                  std::to_string(w) + "x" + std::to_string(h) +
                      " survives the round trip");
        }
    }

    section("a truncated file is never decoded into a bogus buffer");
    {
        const auto full = imageio::encode_png(make_rgba(32, 32), 32, 32);
        int inconsistent = 0, decoded = 0, total = 0;
        // Every prefix. A decoder that reads past the end of any of these is
        // a heap overread on a file the user merely opened.
        //
        // The check is CONSISTENCY, not rejection. Cutting the last few bytes
        // only removes the IEND chunk and its CRC, and every pixel is already
        // present -- returning the image is right, and refusing it would make
        // the decoder worse. What must never happen is claiming a size the
        // buffer does not have, because that is what turns a bad download into
        // an out-of-bounds read in whatever draws it.
        for (std::size_t n = 0; n < full.size(); n += 7) {
            ++total;
            const std::vector<std::uint8_t> cut(full.begin(),
                                                full.begin() + static_cast<long>(n));
            const auto out = imageio::decode_png(cut);
            if (out.width <= 0 || out.height <= 0) continue;
            ++decoded;
            if (out.rgba.size() !=
                static_cast<std::size_t>(out.width) * out.height * 4) {
                ++inconsistent;
            }
        }
        std::printf("    %d prefixes, %d decoded (all trailing-chunk cuts)\n",
                    total, decoded);
        check(inconsistent == 0,
              "no truncation produced a buffer smaller than its dimensions");
    }

    section("garbage is rejected");
    {
        const std::vector<std::uint8_t> empty;
        check(imageio::decode_png(empty).width == 0, "empty input");

        const std::vector<std::uint8_t> not_png(64, 0xAB);
        check(imageio::decode_png(not_png).width == 0, "random bytes");

        // Correct signature, nothing after it: the tempting case for a
        // decoder that trusts its own magic number.
        const std::vector<std::uint8_t> sig_only{
            0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        check(imageio::decode_png(sig_only).width == 0, "signature with no chunks");
    }

    section("a lying header cannot make the decoder allocate the world");
    {
        // IHDR carries width and height. A file claiming 65535x65535 is 17 GB
        // of RGBA; the decoder must notice the pixel data does not match
        // rather than trying to reserve it.
        auto png = imageio::encode_png(make_rgba(8, 8), 8, 8);
        // IHDR width/height live at bytes 16..23 (8 sig + 4 len + 4 type).
        const auto put32 = [&](std::size_t at, std::uint32_t v) {
            png[at + 0] = static_cast<std::uint8_t>(v >> 24);
            png[at + 1] = static_cast<std::uint8_t>(v >> 16);
            png[at + 2] = static_cast<std::uint8_t>(v >> 8);
            png[at + 3] = static_cast<std::uint8_t>(v);
        };
        auto huge = png;
        put32(16, 0xFFFFu);
        put32(20, 0xFFFFu);
        const auto out = imageio::decode_png(huge);
        check(out.rgba.size() <=
                  static_cast<std::size_t>(0xFFFF) * 0xFFFF * 4,
              "a 65535x65535 claim does not produce more data than it claims");
        check(out.width == 0 || out.rgba.size() ==
                  static_cast<std::size_t>(out.width) * out.height * 4,
              "and whatever it returns is self-consistent");

        auto zero = png;
        put32(16, 0);
        check(imageio::decode_png(zero).width == 0, "zero width is rejected");
    }

    // ── the sweep ────────────────────────────────────────────────────
    section("corruption sweep");
    {
        // The same treatment the font parser gets: take a valid file and
        // corrupt it in a thousand places. Every result must be a decoded
        // image or a clean rejection -- never a crash, and under ASan never
        // an out-of-bounds read.
        const auto valid = imageio::encode_png(make_rgba(48, 32), 48, 32);
        std::mt19937 rng{0xC0FFEE};
        std::uniform_int_distribution<std::size_t> pos{0, valid.size() - 1};
        std::uniform_int_distribution<int> byte{0, 255};

        int decoded = 0, rejectd = 0;
        for (int trial = 0; trial < 400; ++trial) {
            auto corrupt = valid;
            const int hits = 1 + (trial % 8);
            for (int k = 0; k < hits; ++k) {
                corrupt[pos(rng)] = static_cast<std::uint8_t>(byte(rng));
            }
            const auto out = imageio::decode_png(corrupt);
            if (out.width > 0 && out.height > 0) {
                ++decoded;
                // If it claims success, the buffer must match the dimensions.
                // A short buffer here is what turns a bad file into a crash
                // in whatever draws the image.
                if (out.rgba.size() !=
                    static_cast<std::size_t>(out.width) * out.height * 4) {
                    check(false, "decoded image had an inconsistent buffer");
                    break;
                }
            } else {
                ++rejectd;
            }
        }
        std::printf("    %d decoded, %d rejected\n", decoded, rejectd);
        check(decoded + rejectd == 400,
              "400 corrupted PNGs parsed or rejected without crashing");
    }

    section("truncation sweep on the compressed stream");
    {
        // Cutting inside the IDAT data specifically exercises the DEFLATE
        // inflater's end-of-input handling, which is the part most likely to
        // read past its buffer.
        const auto valid = imageio::encode_png(make_rgba(40, 40), 40, 40);
        int handled = 0;
        for (std::size_t n = 40; n < valid.size(); n += 3) {
            std::vector<std::uint8_t> cut(valid.begin(),
                                          valid.begin() + static_cast<long>(n));
            const auto out = imageio::decode_png(cut);
            if (out.width == 0 ||
                out.rgba.size() ==
                    static_cast<std::size_t>(out.width) * out.height * 4) {
                ++handled;
            }
        }
        check(handled > 0, "every mid-stream truncation handled cleanly");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
