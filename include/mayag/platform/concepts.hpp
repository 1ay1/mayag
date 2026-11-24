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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

// ══ capabilities as types ═══════════════════════════════════════════════════════════════
//
// Each capability is a TYPE (a tag in `cap::`) whose truth for a window W is
// the concept above that defines it. So:
//
//   has<W, cap::uploads_images>      the one way to ask (the host asks only so)
//   capabilities_of<W>               W's set, COMPUTED from W -- never listed
//   caps<cap::a, cap::b, ...>        a set, written as a type
//   offers_exactly<W, caps<...>>     a backend's statement of its set, checked
//                                    BOTH ways against the computed one
//
// A backend states its set in one line after its class:
//
//   static_assert(check_capabilities<MacWindow, caps<cap::uploads_images, ...>>());
//
// and the build fails, NAMING the capability, if the class loses one it
// states ("missing: moves_image_rows") or gains one it does not
// ("unstated: repeats_keys"). Nothing is assumed about a window the host
// was not built against: a test's fake window simply has the set its
// methods give it.

/// A set of capabilities, as a type.
template <class... Cs> struct caps {
    static constexpr std::size_t size = sizeof...(Cs);
    /// The names, in order (for diagnostics, logs and tests).
    static constexpr std::array<std::string_view, sizeof...(Cs)> names{Cs::name...};
};

namespace cap {
#define MAYAG_CAPABILITY(tag, Concept)                                    \
    struct tag {                                                          \
        static constexpr std::string_view name = #tag;                    \
        template <class W> static constexpr bool holds = Concept<W>;      \
    }
MAYAG_CAPABILITY(event_fd, HasEventFd);
MAYAG_CAPABILITY(draws_live, DrawsLive);
MAYAG_CAPABILITY(replaces_queued_frames, ReplacesQueuedFrames);
MAYAG_CAPABILITY(counts_skipped_frames, CountsSkippedFrames);
MAYAG_CAPABILITY(latches_late, LatchesLate);
MAYAG_CAPABILITY(counts_latch_misses, CountsLatchMisses);
MAYAG_CAPABILITY(reports_display_timing, ReportsDisplayTiming);
MAYAG_CAPABILITY(repeats_keys, RepeatsKeys);
MAYAG_CAPABILITY(asks_for_redraw, AsksForRedraw);
MAYAG_CAPABILITY(owes_redraw, OwesRedraw);
MAYAG_CAPABILITY(fullscreens, Fullscreens);
MAYAG_CAPABILITY(manages_itself, ManagesItself);
MAYAG_CAPABILITY(uploads_images, UploadsImages);
MAYAG_CAPABILITY(patches_images, PatchesImages);
MAYAG_CAPABILITY(moves_image_rows, MovesImageRows);
MAYAG_CAPABILITY(shares_event_queue, SharesEventQueue);
MAYAG_CAPABILITY(takes_text_input, TakesTextInput);
MAYAG_CAPABILITY(measures_photons, MeasuresPhotons);
MAYAG_CAPABILITY(prestarts_gpu, PrestartsGpu);
#undef MAYAG_CAPABILITY

/// Every capability there is: the universe capabilities_of filters.
using all = caps<event_fd, draws_live, replaces_queued_frames, counts_skipped_frames, latches_late,
                 counts_latch_misses, reports_display_timing, repeats_keys, asks_for_redraw, owes_redraw,
                 fullscreens, manages_itself, uploads_images, patches_images, moves_image_rows,
                 shares_event_queue, takes_text_input, measures_photons, prestarts_gpu>;
}  // namespace cap

/// Does window W have capability C? The one question the host asks.
template <class W, class C> inline constexpr bool has = C::template holds<W>;

namespace detail_caps {
template <class W, class Acc, class Rest> struct filter;
template <class W, class... A> struct filter<W, caps<A...>, caps<>> { using type = caps<A...>; };
template <class W, class... A, class C, class... R>
struct filter<W, caps<A...>, caps<C, R...>>
    : filter<W, std::conditional_t<has<W, C>, caps<A..., C>, caps<A...>>, caps<R...>> {};

template <class C, class Set> inline constexpr bool in = false;
template <class C, class... Cs> inline constexpr bool in<C, caps<Cs...>> = (std::is_same_v<C, Cs> || ...);

template <class Set> inline constexpr bool known = false;   // every member is in cap::all
template <class... Cs> inline constexpr bool known<caps<Cs...>> = (in<Cs, cap::all> && ...);

/// The first difference between W's computed set and a stated one, as text.
struct diff {
    char text[96]{};
    std::size_t n = 0;
    constexpr void put(std::string_view s) {
        for (char c : s) if (n + 1 < sizeof text) text[n++] = c;
    }
    [[nodiscard]] constexpr const char* data() const { return text; }
    [[nodiscard]] constexpr std::size_t size() const { return n; }
    [[nodiscard]] constexpr bool empty() const { return n == 0; }
};
template <class W, class Stated, class... All>
consteval diff first_difference(caps<All...>) {
    diff d;
    const auto one = [&]<class C>() {
        if (!d.empty()) return;
        if (in<C, Stated> && !has<W, C>) { d.put("mayag: a window lost a capability it states -- missing: "); d.put(C::name); }
        else if (!in<C, Stated> && has<W, C>) { d.put("mayag: a window has a capability it does not state -- unstated: "); d.put(C::name); }
    };
    (one.template operator()<All>(), ...);
    return d;
}
}  // namespace detail_caps

/// W's capabilities, computed from W itself (a `caps<...>`, in cap::all's order).
template <class W> using capabilities_of = typename detail_caps::filter<W, caps<>, cap::all>::type;

/// W offers exactly the stated set: nothing missing, nothing unstated.
template <class W, class Stated>
inline constexpr bool offers_exactly = detail_caps::known<Stated> &&
                                       detail_caps::first_difference<W, Stated>(cap::all{}).empty();

/// For a backend's one-line statement:
///   static_assert(check_capabilities<MyWindow, caps<...>>());
/// true when the statement holds; when it does not, the static_assert's own
/// message (C++26 user-generated messages) or the error below names the
/// first capability that differs.
template <class W, class Stated>
consteval bool check_capabilities() {
    static_assert(detail_caps::known<Stated>, "mayag: a stated capability is not in cap::all");
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
    static_assert(offers_exactly<W, Stated>, detail_caps::first_difference<W, Stated>(cap::all{}));
#else
    static_assert(offers_exactly<W, Stated>, "mayag: a window's capabilities differ from what it states");
#endif
    return true;
}

}  // namespace mayag::platform
