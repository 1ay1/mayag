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
