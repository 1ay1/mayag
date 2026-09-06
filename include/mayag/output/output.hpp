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

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mayag::output {

/// How a display's subpixels are arranged.
///
/// A client uses this to decide whether subpixel text antialiasing will help
/// or hurt: rendering RGB-ordered subpixels on a BGR panel produces coloured
/// fringes that look worse than no antialiasing at all. So it must be
/// reported, and reported honestly -- `unknown` is the right answer when the
/// display does not say, and guessing `horizontal_rgb` because it is the
/// common case is how a minority of users get fringed text forever.
///
/// Values match wl_output.subpixel so a protocol layer forwards rather than
/// translates.
enum class Subpixel : std::uint8_t {
    unknown = 0,
    none,                ///< no subpixel structure: a projector, an OLED
    horizontal_rgb,
    horizontal_bgr,
    vertical_rgb,
    vertical_bgr,
};

/// Who made the display, as it identifies itself.
///
/// Carried because wl_output.geometry sends it, and because a compositor uses
/// it to remember per-display settings across unplugs -- a connector name
/// like "DP-1" is reused by whatever is plugged in next, while make/model and
/// serial identify the actual panel.
struct DisplayIdentity {
    std::string make;      ///< manufacturer, from EDID
    std::string model;
    std::string serial;    ///< empty when the display does not report one
};

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

/// How a buffer is oriented relative to the output.
///
/// `wl_output.transform`'s eight values, and the compositor's job is to make
/// the picture come out upright whichever one a client used. Kept here rather
/// than in protocol code because applying a rotation is a rendering question:
/// the display controller can often do it for free on a plane, and only the
/// graphics layer knows that.
///
/// The naming is the one thing everybody gets wrong, so it is stated here:
/// `rotate_90` means the BUFFER is rotated 90 degrees clockwise relative to
/// the output, so the compositor rotates it 90 degrees COUNTER-clockwise to
/// display it. It describes the buffer, not the correction.
enum class Transform : std::uint8_t {
    normal = 0,
    rotate_90,
    rotate_180,
    rotate_270,
    flipped,
    flipped_90,
    flipped_180,
    flipped_270,
};

/// Does this transform swap width and height?
[[nodiscard]] constexpr bool swaps_axes(Transform t) noexcept {
    return t == Transform::rotate_90  || t == Transform::rotate_270 ||
           t == Transform::flipped_90 || t == Transform::flipped_270;
}

/// A set of rectangles: what changed, or what is opaque.
///
/// Both questions are regions rather than single rects, and treating them as
/// rects is a real loss. A client that is opaque in the middle and
/// transparent at its rounded corners has to declare itself entirely
/// transparent if opacity is a bool, which throws away every occlusion
/// optimisation for the whole window.
///
/// Deliberately a plain vector, not an interval tree. A surface has a handful
/// of damage rects, not thousands, and the cost of getting this wrong is
/// paid every frame in cache misses rather than in asymptotics.
struct Region {
    std::vector<Rect> rects;

    [[nodiscard]] bool empty() const noexcept { return rects.empty(); }
    void add(const Rect& r) { if (r.size.x > 0 && r.size.y > 0) rects.push_back(r); }
    void clear() noexcept { rects.clear(); }

    /// Does any rect cover `p`?
    [[nodiscard]] bool contains(Vec2 p) const noexcept {
        return std::any_of(rects.begin(), rects.end(),
                           [&](const Rect& r) { return r.contains(p); });
    }

    /// The smallest rect containing everything. Empty when the region is.
    [[nodiscard]] Rect bounds() const noexcept {
        if (rects.empty()) return Rect{};
        float l = rects[0].left(), t = rects[0].top();
        float r = rects[0].right(), b = rects[0].bottom();
        for (const Rect& x : rects) {
            l = num::min(l, x.left());   t = num::min(t, x.top());
            r = num::max(r, x.right());  b = num::max(b, x.bottom());
        }
        return Rect{{l, t}, {r - l, b - t}};
    }
};

/// What to do to a layer's pixels beyond placing them.
///
/// Effects belong in mayag by the same rule everything else does: they are
/// pixel decisions, getting them wrong is a rendering bug, and none of them
/// needs a device. mayag could already DRAW all of this -- rounded boxes,
/// analytic shadows, backdrop blur are ShapeKinds the renderer has had all
/// along -- but a Layer could not ASK for any of it. The renderer and the
/// output layer disagreed about what a picture is.
///
/// That gap is not cosmetic. Every desktop rounds window corners, and a
/// compositor that cannot say "round this one" must composite a mask itself,
/// which costs a pass AND the hardware plane. The effect is cheaper as a
/// property of the layer than as an extra layer.
///
/// A default Effects does nothing, so this is additive: a compositor that
/// ignores it renders exactly as before.
struct Effects {
    /// Corner radius in physical pixels. 0 is square.
    ///
    /// Worth stating because it is the one that interacts with planes: a
    /// rounded layer is no longer rectangular, so it cannot be scanned out
    /// directly and cannot be treated as fully opaque. The planner enforces
    /// both rather than trusting the caller to remember.
    float corner_radius = 0.0f;

    /// Multiplied into the layer's alpha. 1 is unchanged.
    ///
    /// Distinct from `opaque`, which is an occlusion HINT about the buffer's
    /// contents. This is a compositor-applied fade -- a window animating in,
    /// or an inactive one dimmed -- and it makes the layer non-opaque
    /// whatever the buffer contains.
    float opacity = 1.0f;

    /// Blur applied to what is BEHIND the layer, in pixels. 0 is off.
    ///
    /// The frosted-glass look. Expensive and unmistakable, which is why it
    /// is opt-in per layer rather than a global mode.
    float backdrop_blur = 0.0f;
    /// Saturation and brightness applied with the backdrop blur. 1 is
    /// unchanged. These travel together because a blur alone reads as murky;
    /// every real implementation lifts saturation slightly to compensate.
    float backdrop_saturation = 1.0f;
    float backdrop_brightness = 1.0f;

    /// Drop shadow radius in pixels. 0 is off.
    ///
    /// mayag's shadow is ANALYTIC -- a closed-form falloff, not a blur pass
    /// over a rendered copy -- so it costs one instance rather than a
    /// render-to-texture and a two-pass gaussian. That is why a compositor
    /// can afford one per window.
    float shadow_radius = 0.0f;
    Vec2  shadow_offset{};
    Color<Srgb> shadow_color = colors::black.fade(0.35f);

    /// Does this layer need anything beyond a plain blit?
    [[nodiscard]] bool is_identity() const noexcept {
        return corner_radius == 0.0f && opacity == 1.0f &&
               backdrop_blur == 0.0f && shadow_radius == 0.0f &&
               backdrop_saturation == 1.0f && backdrop_brightness == 1.0f;
    }

    /// Can a layer with these effects still be scanned out by the hardware?
    ///
    /// Only when nothing is asked for. A display plane blits a rectangle: it
    /// cannot round a corner, blur a backdrop or draw a shadow. Answering
    /// this here rather than in the planner keeps the rule in one place --
    /// adding an effect later cannot forget to disqualify itself.
    ///
    /// Opacity is the one exception in principle, since some planes have a
    /// constant-alpha register, but it is not assumed: a wrong yes shows the
    /// wrong thing on screen, and a wrong no costs a little power.
    [[nodiscard]] bool scanout_capable() const noexcept { return is_identity(); }
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
    /// A fast path for the common case. When only PART of the layer is
    /// opaque, leave this false and fill `opaque_region` instead.
    bool       opaque = false;
    /// Which parts of the layer are fully opaque, in layer-local coordinates.
    ///
    /// wl_surface.set_opaque_region is a region, not a flag, and the
    /// difference matters: a window with rounded corners is opaque
    /// everywhere except four small arcs. Collapsing that to "transparent"
    /// means the compositor draws everything behind it, every frame, for the
    /// sake of a few dozen pixels.
    Region     opaque_region;
    /// How the buffer is oriented relative to the output.
    Transform  transform = Transform::normal;
    /// What changed since the last frame, in layer-local coordinates.
    ///
    /// Empty means "everything", which is the safe default: a compositor that
    /// wrongly believes nothing changed shows a stale frame, while one that
    /// redraws too much only wastes power. Damage arrives from clients in
    /// BUFFER coordinates and has to cross scale, transform and position to
    /// become screen coordinates -- that arithmetic is a graphics question,
    /// so it belongs on this side of the seam rather than in protocol code.
    Region     damage;
    /// When these pixels are ready, and how to say we are done. Default means
    /// implicit sync: ready now, nobody to tell.
    BufferSync sync;
    /// What to do to the pixels beyond placing them. Default does nothing.
    Effects    effects;
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
    /// When the NEXT frame is expected to reach the screen.
    ///
    /// A compositor hands this to clients as a deadline: draw by then and
    /// your frame is shown, miss it and it waits a whole refresh. Without a
    /// predicted time a client can only pace to when its LAST frame landed,
    /// which is the wrong clock -- it drifts, and under load it collapses
    /// into rendering frames nobody ever sees.
    ///
    /// Predicted, not promised: a mode change or a missed vblank moves it.
    /// Zero means the display cannot say, which a client must treat as "draw
    /// when you like" rather than "draw immediately".
    double        next_vblank = 0.0;
};

using CommitResult = std::expected<Presented, CommitError>;

/// Format+modifier pairs usable on BOTH the renderer and the display.
///
/// A compositor advertises this to clients, and neither side can answer it
/// alone: the GPU decides what it can sample, the display engine decides what
/// it can scan out, and they are different hardware with different rules. A
/// client wants a buffer that satisfies both, because one only the GPU
/// accepts can never reach a plane, and one only the display accepts cannot
/// be composited when it loses a plane.
///
/// Provided here rather than left to each compositor because getting it wrong
/// is SILENT: clients allocate buffers that work, just slowly, forever.
///
/// `scanout` is set on every result, so a caller can tell which pairs are on
/// the fast path without intersecting the lists again.
[[nodiscard]] inline std::vector<FormatModifier>
common_formats(std::span<const FormatModifier> renderer,
               std::span<const FormatModifier> display) {
    std::vector<FormatModifier> out;
    for (const FormatModifier& r : renderer) {
        const auto it = std::find_if(
            display.begin(), display.end(), [&](const FormatModifier& d) {
                return d.format == r.format && d.modifier == r.modifier;
            });
        if (it == display.end()) continue;
        out.push_back(FormatModifier{r.format, r.modifier, /*scanout=*/true});
    }
    return out;
}

/// A hardware cursor: a small buffer the display moves without a full commit.
///
/// Almost every display controller has a dedicated cursor plane, and using it
/// is the difference between a pointer that tracks the hand and one that lags
/// a frame behind everything. Moving it costs one register write; compositing
/// it means redrawing the screen for every mouse motion, which is the most
/// visible latency mistake a compositor can make and the easiest to avoid.
///
/// Separate from Layer because the hardware treats it separately: it updates
/// on its own schedule, it does not participate in plane assignment, and it
/// has size limits (often 64x64 or 256x256) that ordinary planes do not.
struct Cursor {
    /// The image. An empty buffer hides the cursor.
    Buffer buffer;
    /// Where the pointer tip is inside the image, so the compositor can place
    /// the hotspot on the actual pointer position rather than the corner.
    Vec2   hotspot{};
    /// Top-left position on screen, in physical pixels.
    Vec2   position{};
};

/// A physical display mayag drives.
///
/// A CONCEPT rather than a base class, matching platform::Window: the
/// concrete type is chosen at compile time, so there is no vtable and no
/// indirect call on the frame path. A DRM/KMS output, a nested Wayland
/// surface and a headless test double all satisfy it.
template <typename O>
concept Output = requires(O o, const O co, std::span<const Layer> layers,
                          const DisplayMode& mode, const Cursor& cursor) {
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

    /// Move or replace the hardware cursor.
    ///
    /// Separate from commit() because that is the entire point: a pointer
    /// motion must not cost a frame. A display that has no cursor plane
    /// reports so via has_cursor_plane(), and the compositor composites one
    /// as an ordinary layer instead.
    { o.set_cursor(cursor) } -> std::same_as<CommitResult>;
    { co.has_cursor_plane() } -> std::same_as<bool>;
    /// Largest cursor the hardware plane accepts, {0,0} when there is none.
    { co.max_cursor_size() } -> std::same_as<Vec2>;

    /// A stable identity, so a compositor can remember per-display settings
    /// across unplugs. Typically connector name plus EDID serial.
    { co.name() } -> std::convertible_to<std::string_view>;
    /// Make, model and serial, as wl_output.geometry reports them. A
    /// protocol layer forwards these rather than inventing them.
    { co.identity() } -> std::same_as<DisplayIdentity>;
    /// Subpixel layout, for clients deciding how to antialias text.
    { co.subpixel() } -> std::same_as<Subpixel>;
    /// Physical size in millimetres, for computing real DPI. {0,0} when the
    /// display does not say.
    { co.physical_size_mm() } -> std::same_as<Vec2>;
};

}  // namespace mayag::output
