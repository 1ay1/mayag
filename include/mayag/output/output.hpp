#pragma once

/// Outputs: a display mayag drives directly.
///
/// A `Window` is something an OS gives you. It has a size someone else
/// decided, it is presented by someone else's compositor, and "present this
/// frame" cannot fail in a way the app must handle.
///
/// An `Output` is a display mayag OWNS. A compositor sets the mode, chooses
/// the buffer, and asks the kernel to scan it out. Everything the Window
/// concept can assume away becomes explicit:
///
///   * a mode must be CHOSEN, from a list the hardware supplies
///   * a commit can FAIL, and failing is normal
///   * presentation happens at a KNOWN TIME, which clients need to pace to
///
/// The second is the one that decides the shape of this file. An atomic KMS
/// commit is a test-then-apply operation: the kernel takes a whole
/// configuration — which buffer on which plane, at what position — and either
/// accepts all of it or rejects all of it. Rejection is not an error
/// condition, it is the normal way a compositor discovers that this hardware
/// cannot put that buffer on that plane. The response is to retry with a
/// simpler configuration, usually by compositing something that had hoped to
/// be scanned out directly.
///
/// So `commit` returns a RESULT, not void. A void present cannot express
/// "that layout was refused, try another", and retrofitting the return later
/// is a breaking change for everyone who implemented the concept. It belongs
/// in the signature from the first line.

#include "../core/geometry.hpp"
#include "../render/buffer.hpp"
#include "../render/sync.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mayag::output {

/// A display mode: resolution plus refresh rate.
struct DisplayMode {
    int           width  = 0;
    int           height = 0;
    /// Millihertz, as DRM reports it — 59.94 Hz is 59940, and rounding it to
    /// an integer Hz loses the distinction between 59.94 and 60 that video
    /// playback actually cares about.
    std::uint32_t refresh_mhz = 0;
    /// The mode the display prefers, usually its native resolution.
    bool          preferred = false;

    [[nodiscard]] double refresh_hz() const noexcept {
        return static_cast<double>(refresh_mhz) / 1000.0;
    }
    [[nodiscard]] friend constexpr bool operator==(DisplayMode, DisplayMode) = default;
};

/// How a buffer is placed on a display.
///
/// A layer is one composited element: a client window, a cursor, a video.
/// The compositor proposes a set of layers per frame, and the hardware
/// decides how many it can scan out directly rather than requiring the GPU to
/// blend them.
struct Layer {
    /// The pixels. Usually a client's dmabuf, imported not copied.
    Buffer     buffer;
    /// Where it goes on screen, in physical pixels.
    Rect       dst{};
    /// Which part of the buffer to use. Empty means all of it.
    Rect       src{};
    /// Back to front. Layer 0 is the wallpaper, the highest is the cursor.
    int        z = 0;
    /// True when the layer covers `dst` completely with no transparency.
    ///
    /// Not a hint — it is what lets the hardware skip everything underneath,
    /// and getting it wrong shows as content bleeding through. The scene
    /// graph knows this exactly, which is why mayag can answer it where a
    /// compositor bolted onto a renderer has to guess.
    bool       opaque = false;
    /// When these pixels are ready, and how to say we are done. Default means
    /// implicit sync: ready now, nobody to tell.
    BufferSync sync;
};

/// Why an atomic commit was refused.
///
/// Distinguished because the RESPONSES differ, which is the same reason
/// ImportError has cases: a compositor that cannot tell "try a simpler plane
/// layout" from "the display went away" will either give up too early or
/// spin forever.
enum class CommitError {
    /// The hardware cannot do this layer configuration. Retry with fewer
    /// planes — this is the normal path, not a bug.
    unsupported_layout,
    /// A buffer is not scanout-capable: wrong format, wrong modifier, or in
    /// memory the display engine cannot reach.
    buffer_unusable,
    /// The display was unplugged or the session was revoked (VT switch).
    disconnected,
    /// Another commit is already in flight for this output.
    busy,
};

/// What happened to a commit that succeeded.
struct Presented {
    /// When the frame actually reached the screen. Clients pace to this, so
    /// it must be the hardware's answer rather than the time the commit was
    /// issued.
    double        time = 0.0;
    /// Monotonically increasing count of scanned-out frames.
    std::uint64_t sequence = 0;
    /// How many layers the hardware scanned out DIRECTLY, without the GPU
    /// compositing them.
    ///
    /// The number worth optimising. A fullscreen video that reaches
    /// direct_scanout == 1 costs no GPU work and no power at all; the same
    /// video composited costs a full-screen blend every frame. Reported so an
    /// app can see whether it is on the fast path instead of guessing.
    int           direct_scanout = 0;
};

using CommitResult = std::expected<Presented, CommitError>;

/// A physical display mayag drives.
///
/// A CONCEPT rather than a base class, matching platform::Window: the
/// concrete type is chosen at compile time, so there is no vtable and no
/// indirect call on the frame path. A DRM/KMS output, a nested Wayland
/// surface and a headless test double all satisfy it.
template <typename O>
concept Output = requires(O o, const O co, std::span<const Layer> layers,
                          const DisplayMode& mode) {
    /// Modes the hardware supports. Never empty for a connected display.
    { co.modes() } -> std::ranges::range;
    /// The mode currently set, if any. Nullopt means the display is off.
    { co.current_mode() } -> std::same_as<std::optional<DisplayMode>>;
    /// Set a mode. Fails the same way a commit does.
    { o.set_mode(mode) } -> std::same_as<CommitResult>;

    /// Scan out these layers. Atomic: all of it, or none of it.
    { o.commit(layers) } -> std::same_as<CommitResult>;

    /// Formats this display can scan out DIRECTLY.
    ///
    /// Different from a renderer's importable_formats(): the display engine
    /// and the GPU have different capabilities, and the intersection is what
    /// a compositor should advertise to clients that want the fast path.
    { co.scanout_formats() } -> std::ranges::range;

    /// A stable identity, so a compositor can remember per-display settings
    /// across unplugs. Typically connector name plus EDID serial.
    { co.name() } -> std::convertible_to<std::string_view>;
    /// Physical size in millimetres, for computing real DPI. {0,0} when the
    /// display does not say.
    { co.physical_size_mm() } -> std::same_as<Vec2>;
};

}  // namespace mayag::output
