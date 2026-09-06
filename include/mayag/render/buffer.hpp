#pragma once

/// Buffers: where a picture's pixels actually live.
///
/// mayag's original pixel type is `ImageData` — an owned, CPU-side, tightly
/// packed RGBA8 vector. That is exactly right for an app drawing its own UI:
/// simple, backend-agnostic, and impossible to misuse.
///
/// It is fatal for a COMPOSITOR, which never owns its pixels. A Wayland client
/// hands the compositor a `dmabuf`: a GPU buffer, in a tiled vendor-specific
/// format, described by DRM format modifiers, which the client still owns and
/// will reuse. The entire value of a modern compositor is that those bytes are
/// never copied and never touched by the CPU. Measured on one 1080p frame:
/// ~8 MB copied, 3.27 ms to convert, 2.8 ms to move. Zero-copy is 0 of each.
///
/// So a buffer is a HANDLE to memory that may be foreign, and `ImageData` is
/// one case of it. Nothing else in mayag changes: a `texture` instance still
/// names a handle, and only the origin of the pixels behind it becomes plural.
///
/// Three cases, because there are exactly three places pixels come from:
///
///   Cpu     bytes mayag owns          — an app's own image, a decoded frame
///   Dmabuf  a GPU buffer, borrowed    — a client's window contents
///   Shm     mapped memory, borrowed   — wl_shm, software clients
///
/// The borrowed cases carry no ownership: they describe memory belonging to
/// someone else for the duration of a call. See `Dmabuf` for the fd contract,
/// which is the only subtle part.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "mayag/core/math.hpp"
#include "mayag/render/draw_list.hpp"   // ImageData

namespace mayag {

/// A DRM FourCC pixel format (`DRM_FORMAT_*` from drm_fourcc.h).
///
/// Spelled as a value rather than an enum because the set is open: drivers
/// gain formats, and a compositor must be able to pass through one mayag has
/// never heard of. The few mayag itself cares about are named below.
enum class Fourcc : std::uint32_t {};

/// Build a FourCC the way drm_fourcc.h does, so these agree by construction.
[[nodiscard]] constexpr Fourcc fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<Fourcc>(
        static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24));
}

namespace formats {
/// The formats mayag can name. A device may import many more.
inline constexpr Fourcc argb8888 = fourcc('A', 'R', '2', '4');
inline constexpr Fourcc xrgb8888 = fourcc('X', 'R', '2', '4');
inline constexpr Fourcc abgr8888 = fourcc('A', 'B', '2', '4');
inline constexpr Fourcc xbgr8888 = fourcc('X', 'B', '2', '4');
/// What ImageData is: 8 bits per channel, R first in memory.
inline constexpr Fourcc rgba8888 = abgr8888;

// Planar YUV, which is what every video source produces. A media player or a
// screen-capture client hands the compositor one of these, not RGB, so a
// framework that cannot take them makes every video path pay a conversion it
// should not.
//
// Named here rather than left to each caller because the PLANE LAYOUT is part
// of the format and easy to get wrong: NV12 interleaves U and V in one plane,
// YUV420 keeps them separate, and both subsample chroma by two in each
// direction.

/// Y plane + interleaved UV plane, chroma at half resolution. The most common
/// hardware decoder output there is.
inline constexpr Fourcc nv12 = fourcc('N', 'V', '1', '2');
/// Y, U and V in three separate planes, chroma at half resolution.
inline constexpr Fourcc yuv420 = fourcc('Y', 'U', '1', '2');
}  // namespace formats

/// A DRM format modifier: how the pixels are arranged in memory.
///
/// This is the part people skip and then wonder why their compositor is slow.
/// A modifier says whether a buffer is linear, or tiled in one of a dozen
/// vendor-specific ways, or compressed. Importing with the WRONG modifier does
/// not fail loudly — it samples garbage. So a modifier travels with every
/// dmabuf and is matched exactly.
enum class Modifier : std::uint64_t {
    /// Row-major, no tiling. Always importable, never the fastest.
    linear = 0,
    /// "The allocator picked; ask the driver." Legacy, and ambiguous: two
    /// buffers with this modifier may have different layouts. Present because
    /// old clients still send it.
    invalid = 0x00ffffffffffffffULL,
};

/// One format + modifier pair a device can import.
///
/// A compositor advertises the whole set to clients (via zwp_linux_dmabuf_v1)
/// so they allocate buffers the GPU can actually sample or scan out. Getting
/// this list wrong is how clients end up allocating buffers that force a copy.
struct FormatModifier {
    Fourcc   format{};
    Modifier modifier = Modifier::linear;
    /// True when a buffer in this layout can be handed straight to a display
    /// plane with no compositing at all. That is the whole prize: a fullscreen
    /// video that costs zero GPU work.
    bool     scanout = false;

    [[nodiscard]] friend constexpr bool operator==(FormatModifier,
                                                   FormatModifier) = default;
};

/// A single plane of a dmabuf. Multi-planar formats (NV12, YUV420) split
/// luma and chroma across planes, sometimes into different fds.
struct DmabufPlane {
    /// BORROWED. See the ownership note on `Dmabuf`.
    int           fd     = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

/// A GPU buffer owned by someone else — in a compositor, by a client.
///
/// OWNERSHIP, which is the only subtle part of this whole header:
///
/// The fds are BORROWED for the duration of the `import()` call and no
/// longer. A device that needs to keep the memory must `dup()` — Vulkan's
/// `VkImportMemoryFdInfoKHR` takes ownership of the fd it is given, so the
/// backend dups and hands over the dup. The caller closes its own fds
/// whenever it likes.
///
/// This is deliberately the strictest of the possible contracts. The
/// alternatives — "import takes ownership" or "the Buffer owns the fds" —
/// both make the common compositor path (a client re-submits the same buffer
/// every frame) either leak or double-close, and neither is checkable. A
/// borrow that lasts exactly one call is the one rule that cannot be got
/// wrong by accident.
struct Dmabuf {
    std::array<DmabufPlane, 4> planes{};
    int           n_planes = 0;
    Fourcc        format{};
    Modifier      modifier = Modifier::linear;
};

/// Mapped memory belonging to someone else: `wl_shm`, or any pool a client
/// wrote into. Borrowed for the duration of the call, like Dmabuf.
struct Shm {
    std::span<const std::uint8_t> pixels;
    std::uint32_t                 stride = 0;
    Fourcc                        format{};
};

/// Where a picture's pixels live, plus its dimensions.
///
/// The variant is the point: `Cpu` is what mayag has always done, and the
/// other two are memory mayag does not own. Code that only ever handles Cpu
/// keeps working untouched — this type is additive.
struct Buffer {
    /// Bytes mayag owns. The original path.
    struct Cpu {
        std::shared_ptr<const ImageData> image;
    };

    std::variant<Cpu, Dmabuf, Shm> storage;
    int width  = 0;
    int height = 0;

    [[nodiscard]] bool is_foreign() const noexcept {
        return !std::holds_alternative<Cpu>(storage);
    }
    [[nodiscard]] bool valid() const noexcept {
        if (width <= 0 || height <= 0) return false;
        if (const auto* c = std::get_if<Cpu>(&storage))
            return c->image != nullptr && c->image->valid();
        if (const auto* d = std::get_if<Dmabuf>(&storage))
            return d->n_planes > 0 && d->planes[0].fd >= 0;
        if (const auto* s = std::get_if<Shm>(&storage))
            return !s->pixels.empty() && s->stride > 0;
        return false;
    }

    /// Wrap an owned CPU image. The bridge from every existing call site.
    [[nodiscard]] static Buffer from(std::shared_ptr<const ImageData> img) {
        if (!img) return Buffer{};
        const int w = img->width, h = img->height;
        return Buffer{Cpu{std::move(img)}, w, h};
    }
};

/// Copy `src` into tightly-packed RGBA8, converting format and dropping any
/// stride padding.
///
/// This exists so a protocol implementation never writes graphics code. A
/// `wl_shm` client -- the most common client there is -- sends a pool fd, an
/// offset, a stride and a DRM format, and none of those match what a renderer
/// wants: the format is usually ARGB8888 where mayag needs RGBA8, and the
/// stride is padded to whatever the toolkit's allocator liked. Without this,
/// every compositor written on mayag reimplements channel swizzling and
/// stride removal, gets the byte order subtly wrong, and ships a UI where red
/// and blue are swapped.
///
/// Returns nullptr for a format mayag cannot convert, so the caller reports
/// an unsupported format to the client rather than showing it garbage.
///
/// The byte-order trap, stated once so nobody has to rediscover it: DRM names
/// channels in LITTLE-ENDIAN WORD order, so DRM_FORMAT_ARGB8888 is B,G,R,A in
/// MEMORY. mayag's ImageData is R,G,B,A in memory. They are reverses of each
/// other, not the same thing spelled differently.
[[nodiscard]] inline std::shared_ptr<const ImageData>
to_rgba8(const Shm& src, int width, int height) {
    if (width <= 0 || height <= 0 || src.stride == 0) return nullptr;

    // Which byte of the source maps to R, G, B, A.
    int r = 0, g = 0, b = 0, a = 0;
    bool opaque = false;
    if (src.format == formats::argb8888)      { b = 0; g = 1; r = 2; a = 3; }
    else if (src.format == formats::xrgb8888) { b = 0; g = 1; r = 2; opaque = true; }
    else if (src.format == formats::abgr8888) { r = 0; g = 1; b = 2; a = 3; }
    else if (src.format == formats::xbgr8888) { r = 0; g = 1; b = 2; opaque = true; }
    else return nullptr;

    const std::size_t need =
        static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(height);
    if (src.pixels.size() < need) return nullptr;   // client lied about size

    auto out = std::make_shared<ImageData>();
    out->width = width;
    out->height = height;
    out->rgba.resize(static_cast<std::size_t>(width) * height * 4);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* in = src.pixels.data() +
                                 static_cast<std::size_t>(y) * src.stride;
        std::uint8_t* dst = out->rgba.data() +
                            static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = in + static_cast<std::size_t>(x) * 4;
            std::uint8_t* q = dst + static_cast<std::size_t>(x) * 4;
            q[0] = p[r];
            q[1] = p[g];
            q[2] = p[b];
            // An X format has no alpha channel: the fourth byte is padding
            // and may hold anything. Reading it as alpha is why some clients
            // render invisible.
            q[3] = opaque ? 0xff : p[a];
        }
    }
    return out;
}

/// Convert planar YUV to tightly-packed RGBA8.
///
/// Every video source produces YUV, so a framework that cannot take it makes
/// each video path do this itself -- and the colour maths is exactly the kind
/// of thing that is wrong in a way nobody notices until they compare two
/// players side by side.
///
/// `planes` are the per-plane bases and strides, matching a Dmabuf's layout.
/// NV12 wants two, YUV420 three.
///
/// The conversion is BT.601 limited-range, which is what almost every buffer
/// arriving over Wayland actually is: Y spans 16..235 and chroma 16..240,
/// with the remaining values reserved. Treating limited range as full washes
/// blacks grey and clips highlights -- the single most common colour bug in
/// video pipelines, and invisible unless you know to look for it.
///
/// Returns nullptr for anything unsupported.
struct YuvPlane {
    const std::uint8_t* data = nullptr;
    std::uint32_t       stride = 0;
};

[[nodiscard]] inline std::shared_ptr<const ImageData>
yuv_to_rgba8(Fourcc format, std::span<const YuvPlane> planes,
             int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;

    const bool is_nv12 = format == formats::nv12;
    const bool is_i420 = format == formats::yuv420;
    if (!is_nv12 && !is_i420) return nullptr;
    if (planes.size() < (is_nv12 ? 2u : 3u)) return nullptr;
    for (std::size_t i = 0; i < (is_nv12 ? 2u : 3u); ++i) {
        if (planes[i].data == nullptr || planes[i].stride == 0) return nullptr;
    }

    auto out = std::make_shared<ImageData>();
    out->width = width;
    out->height = height;
    out->rgba.resize(static_cast<std::size_t>(width) * height * 4);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* yr =
            planes[0].data + static_cast<std::size_t>(y) * planes[0].stride;
        // Chroma is subsampled by two vertically as well as horizontally, so
        // two output rows read the same chroma row.
        const int cy = y / 2;
        std::uint8_t* dst =
            out->rgba.data() + static_cast<std::size_t>(y) * width * 4;

        for (int x = 0; x < width; ++x) {
            const int cx = x / 2;
            int u = 0, v = 0;
            if (is_nv12) {
                // U and V interleaved in one plane, two bytes per chroma
                // sample.
                const std::uint8_t* uv = planes[1].data +
                    static_cast<std::size_t>(cy) * planes[1].stride +
                    static_cast<std::size_t>(cx) * 2;
                u = uv[0];
                v = uv[1];
            } else {
                u = planes[1].data[static_cast<std::size_t>(cy) * planes[1].stride + cx];
                v = planes[2].data[static_cast<std::size_t>(cy) * planes[2].stride + cx];
            }

            // BT.601 limited range. The 16 and 128 offsets are the whole
            // point: Y starts at 16, not 0, and chroma is centred on 128.
            const int c = static_cast<int>(yr[x]) - 16;
            const int d = u - 128;
            const int e = v - 128;

            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;

            std::uint8_t* q = dst + static_cast<std::size_t>(x) * 4;
            q[0] = static_cast<std::uint8_t>(num::clamp(r, 0, 255));
            q[1] = static_cast<std::uint8_t>(num::clamp(g, 0, 255));
            q[2] = static_cast<std::uint8_t>(num::clamp(b, 0, 255));
            q[3] = 0xff;    // video is opaque
        }
    }
    return out;
}

/// Why an import failed. A compositor needs to tell "this device will never
/// take that format" (renegotiate with the client) from "out of memory right
/// now" (retry), because the responses are completely different.
enum class ImportError {
    unsupported_format,    ///< format/modifier pair not in importable_formats()
    invalid_buffer,        ///< malformed descriptor: bad fd, zero stride
    out_of_memory,
    not_supported,         ///< this backend cannot import at all
};

/// The result of an import: a handle, or why not.
using ImportResult = std::expected<TextureHandle, ImportError>;

}  // namespace mayag
