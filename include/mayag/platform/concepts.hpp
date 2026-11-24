#pragma once
// mayag::platform concepts -- what a window IS (Window) and what one MAY do
// (the capabilities), with no backend included: the host (app/host.hpp) is
// a template over any window and asks by these names; app/platform.hpp picks
// the native one and states what it offers.

#include "../backend/software.hpp"
#include "../core/event.hpp"
#include "../core/geometry.hpp"
#include "types.hpp"

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::platform {

// ── the concept ─────────────────────────────────────────────────────────

template <typename W>
concept Window = requires(W w, const W cw, const DrawList& dl, std::string_view text) {
    // Lifecycle
    { W::open(WindowConfig{}) } -> std::same_as<std::optional<W>>;
    { w.close() }               -> std::same_as<void>;
    { cw.is_open() }            -> std::same_as<bool>;

    // Geometry
    { cw.size() }      -> std::same_as<Vec2>;    ///< logical
    { cw.dpi_scale() } -> std::same_as<float>;

    // Events: everything queued, without blocking. jaal does the waiting;
    // a window with an fd (Wayland) also exposes `int fd() const`.
    { w.dispatch() } -> std::same_as<std::vector<Event>>;

    // Presentation
    { w.present(dl, Color<Srgb>{}) } -> std::same_as<void>;

    // Services
    { cw.now() }               -> std::same_as<double>;   ///< monotonic seconds
    /// The user's configured multi-click interval, in seconds.
    { cw.double_click_interval() } -> std::same_as<double>;
    { w.set_title(text) }      -> std::same_as<void>;
    { w.set_cursor(CursorShape::arrow) } -> std::same_as<void>;
    { w.set_clipboard(text) }  -> std::same_as<void>;
    { w.get_clipboard() }      -> std::same_as<std::string>;

    // Screenshots / golden tests
    { w.read_pixels() } -> std::same_as<std::vector<std::uint8_t>>;
};

// ── what a window MAY do ────────────────────────────────────────────────────────
//
// Everything a window backend can offer beyond `Window`, each named ONCE,
// with its exact signature. The host asks by name (`if constexpr
// (UploadsImages<W>)`), never by guessing a call shape at the call site,
// and each native backend asserts what it offers next to its definition
// (`static_assert(UploadsImages<MacWindow>)`). Probing by a made-up call
// was the second copy of this contract: a backend whose signature drifted
// lost the ability SILENTLY -- `if constexpr` just went false.

/// A pollable event handle jaal waits on with everything else: an fd on
/// Wayland, a HANDLE on Windows. Its TYPE is the reactor's (the host
/// converts and checks it), so this asks only that there is one -- not
/// `convertible_to<int>`, which a HANDLE is not.
template <class W> concept HasEventFd = requires(const W& w) { w.fd(); };
/// Draws from inside the OS's modal resize / move loop (Win32).
template <class W> concept DrawsLive = requires(W& w, std::function<void()> f) { w.set_live_draw(std::move(f)); };
/// A new frame replaces one still queued, instead of waiting behind it.
template <class W> concept ReplacesQueuedFrames =
    requires(const W& w) { { w.replaces_queued_frames() } -> std::convertible_to<bool>; };
/// Counts frames it did not resubmit because they were identical.
template <class W> concept CountsSkippedFrames =
    requires(const W& w) { { w.frames_skipped() } -> std::convertible_to<std::uint64_t>; };
/// Learns how late before vblank a frame can still make it.
template <class W> concept LatchesLate =
    requires(const W& w) { { w.latch_margin() } -> std::convertible_to<std::chrono::nanoseconds>; };
/// ...and counts the frames that missed it (hits, misses).
template <class W> concept CountsLatchMisses =
    LatchesLate<W> && requires(const W& w) { { w.latch_misses().second } -> std::convertible_to<std::uint64_t>; };
/// Reports the display's refresh: when, and how often.
template <class W> concept ReportsDisplayTiming = requires(W& w) {
    { w.display_timing()->vblank } -> std::convertible_to<std::chrono::steady_clock::time_point>;
};
/// Generates key repeat itself (Wayland: the client does).
template <class W> concept RepeatsKeys =
    requires(const W& w) { { w.seconds_until_repeat() } -> std::convertible_to<double>; };
/// The compositor asked for a frame (a configure, a damage request) ...
template <class W> concept AsksForRedraw = requires(const W& w) { { w.wants_redraw() } -> std::convertible_to<bool>; };
/// ... or one is owed but not yet drawable (a frame callback outstanding).
template <class W> concept OwesRedraw = requires(const W& w) { { w.redraw_pending() } -> std::convertible_to<bool>; };
/// Window management from the app's own chrome (a frameless window).
template <class W> concept Fullscreens = requires(W& w, bool on) { w.set_fullscreen(on); };
template <class W> concept ManagesItself = requires(W& w) { w.begin_move(); w.minimize(); w.toggle_maximize(); };
/// Holds images on the GPU (the DrawList names them by id).
template <class W> concept UploadsImages = requires(W& w, std::uint32_t id, std::shared_ptr<const ImageData> img) {
    w.upload_image(id, img);
    w.release_image(id);
};
/// Rewrites rectangles of an uploaded image in place (false: upload whole).
template <class W> concept PatchesImages = requires(W& w, std::uint32_t id, int n,
                                                    const std::vector<std::array<int, 4>>& rects,
                                                    const std::vector<std::uint8_t>& px) {
    { w.update_image_regions(id, n, n, rects, px) } -> std::convertible_to<bool>;
};
/// Scrolls rows of an uploaded image on the GPU (x, y, w, h, dy).
template <class W> concept MovesImageRows = requires(W& w, std::uint32_t id, int n) {
    { w.move_image_rows(id, n, n, n, n, n) } -> std::convertible_to<bool>;
};
/// Shares the app's event queue with child windows (Cocoa's one run loop).
template <class W> concept SharesEventQueue = requires(W& w) { w.enroll(); w.set_pull(false); };
/// Text input (IME) with the caret's rectangle, for the candidate window.
template <class W> concept TakesTextInput = requires(W& w, bool on, Rect caret) { w.set_text_input(on, caret); };
/// Measures input-to-photon latency.
template <class W> concept MeasuresPhotons = requires(W& w) { { w.photon_stats() } -> std::convertible_to<PhotonStats>; };
/// Starts the GPU device before the window exists (Win32: overlaps startup).
template <class W> concept PrestartsGpu = requires { W::gpu_prestart(); };

}  // namespace mayag::platform
