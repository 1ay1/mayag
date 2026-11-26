#pragma once
// mayag::platform::MacWindow — a real Cocoa window, header-only
//
// Calls AppKit through the Objective-C runtime C API (`objc_msgSend`) rather
// than through Objective-C++ source. That is a deliberate trade:
//
//   + mayag stays HEADER-ONLY — no .mm file, no separate build rule, no
//     ARC/ObjC++ dialect flags leaking into consuming projects
//   + it links against libobjc and AppKit, both present on every Mac
//   - the call sites are uglier than `[window makeKeyAndOrderFront:nil]`
//
// The ugliness is contained in this one file, behind `msg<>()`.
//
// The window presents by handing the software rasteriser's framebuffer to a
// CGImage and drawing it into the layer. That is the ALWAYS-WORKS path, and
// it makes "does mayag actually open a window" answerable with zero GPU
// setup. When the Metal backend is compiled in and a device is available, the
// window instead hands the draw list straight to the GPU and the whole
// rasterise + encode + CGImage + blit chain disappears.

#include "../core/target.hpp"
#include "../core/diag.hpp"
#include "../core/event.hpp"
#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../render/draw_list.hpp"
#include "raw_input.hpp"
#include "types.hpp"
#include "concepts.hpp"   // check_capabilities
#include "keys.hpp"
#include "photon.hpp"
#include "images.hpp"
#include "../backend/gpu_policy.hpp"

#if MAYAG_GPU_METAL
#include "../backend/metal.hpp"
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <time.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ── CoreGraphics, declared by hand ──────────────────────────────────────
//
// We deliberately do NOT `#include <ApplicationServices/...>`. Apple's
// headers use the blocks extension (`int (^cb)(void)`), which Clang supports
// and GCC does not — and mayag needs GCC on macOS because Apple Clang has no
// C++26. Declaring the six symbols we actually call keeps this header
// compiler-agnostic, and the ABI of these C functions is stable and public.

extern "C" {

struct CGPoint { double x, y; };
struct CGSize  { double width, height; };
struct CGRect  { CGPoint origin; CGSize size; };

typedef struct CGColorSpace* CGColorSpaceRef;
typedef struct CGContext*    CGContextRef;
typedef struct CGImage*      CGImageRef;

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
void            CGColorSpaceRelease(CGColorSpaceRef);

CGContextRef CGBitmapContextCreate(void* data, std::size_t width, std::size_t height,
                                   std::size_t bitsPerComponent, std::size_t bytesPerRow,
                                   CGColorSpaceRef space, std::uint32_t bitmapInfo);
CGImageRef   CGBitmapContextCreateImage(CGContextRef);
void         CGContextRelease(CGContextRef);
void         CGImageRelease(CGImageRef);

}  // extern "C"

/// CGImageAlphaInfo::kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big.
/// Matches the byte layout `Framebuffer::to_rgba8()` produces.
inline constexpr std::uint32_t mayag_cg_rgba8 = 1u;

inline CGRect mayag_cgrect(double x, double y, double w, double h) {
    return CGRect{CGPoint{x, y}, CGSize{w, h}};
}

namespace mayag::platform {

namespace objc {

/// Typed `objc_msgSend`. The cast is required: `objc_msgSend` is declared
/// variadic, but calling it that way has the wrong ABI for anything but
/// integer args on arm64. Casting to the exact signature is the documented,
/// correct way to invoke it.
template <typename Ret, typename... Args>
inline Ret msg(id self, SEL op, Args... args) {
    using Fn = Ret (*)(id, SEL, Args...);
    return reinterpret_cast<Fn>(&objc_msgSend)(self, op, args...);
}

template <typename Ret, typename... Args>
inline Ret msg_cls(Class cls, SEL op, Args... args) {
    using Fn = Ret (*)(Class, SEL, Args...);
    return reinterpret_cast<Fn>(&objc_msgSend)(cls, op, args...);
}

inline id cls(const char* name) { return reinterpret_cast<id>(objc_getClass(name)); }
inline SEL sel(const char* name) { return sel_registerName(name); }

/// NSString from a UTF-8 C string.
inline id nsstring(std::string_view s) {
    const std::string owned{s};
    return msg_cls<id>(objc_getClass("NSString"), sel("stringWithUTF8String:"), owned.c_str());
}

inline std::string from_nsstring(id str) {
    if (str == nullptr) return {};
    const char* c = msg<const char*>(str, sel("UTF8String"));
    return c ? std::string{c} : std::string{};
}

}  // namespace objc

// Cocoa constants we need, spelled out so we do not depend on the headers.
inline constexpr unsigned long style_titled          = 1u << 0;
inline constexpr unsigned long style_closable        = 1u << 1;
inline constexpr unsigned long style_miniaturizable  = 1u << 2;
inline constexpr unsigned long style_resizable       = 1u << 3;

inline constexpr unsigned long long any_event_mask = ~0ull;

/// NSEventType values.
enum : unsigned long {
    ev_left_down = 1, ev_left_up = 2, ev_right_down = 3, ev_right_up = 4,
    ev_mouse_moved = 5, ev_left_dragged = 6, ev_right_dragged = 7,
    ev_mouse_entered = 8, ev_mouse_exited = 9,
    ev_key_down = 10, ev_key_up = 11, ev_flags_changed = 12,
    ev_scroll_wheel = 22, ev_other_down = 25, ev_other_up = 26, ev_other_dragged = 27,
};

/// NSEventModifierFlags.
enum : unsigned long long {
    mod_shift = 1ull << 17, mod_control = 1ull << 18,
    mod_option = 1ull << 19, mod_command = 1ull << 20,
};

class MacWindow {
  public:
    [[nodiscard]] static std::optional<MacWindow> open(const WindowConfig& cfg) {
        using namespace objc;

        // NSApplication must exist and be told it is a real, focusable app;
        // without setActivationPolicy the window opens behind everything and
        // never takes keyboard focus, which looks exactly like a hang.
        id app = msg_cls<id>(objc_getClass("NSApplication"), sel("sharedApplication"));
        startup::mark("  mac: NSApplication");
        if (app == nullptr) return std::nullopt;
        if (!cfg.child) {
        msg<BOOL>(app, sel("setActivationPolicy:"), static_cast<long>(0));  // Regular

        // `finishLaunching` is NOT optional when you drive the run loop
        // yourself instead of calling `[NSApp run]`. Without it AppKit never
        // completes its startup handshake, and the window is created but
        // never mapped by the window server — the process sits there alive,
        // consuming events, showing nothing. Costly to diagnose, one line to fix.
        msg<void>(app, sel("finishLaunching"));
        startup::mark("  mac: app launched");
        }

        // Undecorated: no title bar, no buttons, square corners -- the app draws
        // its own (a tab strip with window controls). Borderless + resizable
        // still resizes from its edges; miniaturizable lets it minimise.
        unsigned long style = cfg.decorated ? (style_titled | style_closable | style_miniaturizable) : style_miniaturizable;
        if (cfg.resizable) style |= style_resizable;

        const CGRect frame = mayag_cgrect(0, 0, static_cast<double>(cfg.size.x), static_cast<double>(cfg.size.y));

        id win = msg_cls<id>(window_class(), sel("alloc"));
        win = msg<id>(win, sel("initWithContentRect:styleMask:backing:defer:"),
                      frame, style, static_cast<unsigned long>(2) /* Buffered */,
                      static_cast<BOOL>(NO));
        if (win == nullptr) return std::nullopt;
        startup::mark("  mac: window created");

        msg<void>(win, sel("setTitle:"), nsstring(cfg.title));
        if (!cfg.decorated) msg<void>(win, sel("setHasShadow:"), static_cast<BOOL>(YES));
        if (cfg.child) {
            // beside the window that asked for it, the way a preferences
            // window opens over its app, not in a fixed corner of the screen
            id key = msg<id>(app, sel("keyWindow"));
            if (key != nullptr) {
                const CGRect kf = msg<CGRect>(key, sel("frame"));
                const CGRect wf = msg<CGRect>(win, sel("frame"));
                const CGPoint at{kf.origin.x + (kf.size.width - wf.size.width) * 0.5,
                                 kf.origin.y + (kf.size.height - wf.size.height) * 0.5 - 24.0};
                msg<void>(win, sel("setFrameOrigin:"), at);
            } else {
                msg<void>(win, sel("center"));
            }
            // hidden with the app, kept above nothing: a real window, not a panel
            msg<void>(win, sel("setTabbingMode:"), static_cast<long>(2));   // NSWindowTabbingModeDisallowed
        } else {
            msg<void>(win, sel("center"));
        }
        msg<void>(win, sel("setReleasedWhenClosed:"), static_cast<BOOL>(NO));
        msg<void>(win, sel("setAcceptsMouseMovedEvents:"), static_cast<BOOL>(YES));
        msg<void>(win, sel("makeKeyAndOrderFront:"), nullptr);
        if (!cfg.child) msg<void>(app, sel("activateIgnoringOtherApps:"), static_cast<BOOL>(YES));
        startup::mark("  mac: ordered front");

        // A layer-backed content view is what we blit the framebuffer into.
        // It is mayag's own NSView: the window's first responder and an
        // NSTextInputClient, so keys go through the system input method
        // (Japanese, Chinese, Korean, emoji, dead keys) and nothing beeps.
        {
            id own = msg<id>(msg_cls<id>(view_class(), sel("alloc")), sel("initWithFrame:"), frame);
            msg<void>(win, sel("setContentView:"), own);
            msg<void>(own, sel("release"));
            msg<void>(win, sel("makeFirstResponder:"), own);
            // Pointer motion and exit, ALWAYS: a tracking area that follows
            // the view's bounds. Without it a window that isn't key gets no
            // mouseMoved (hover dies the moment another app is focused) and
            // nothing ever says the pointer left -- the last hover sticks.
            constexpr unsigned long entered_exited = 0x01, moved = 0x02, active_always = 0x80,
                                    in_visible_rect = 0x200, cursor_update = 0x04;
            id area = msg<id>(msg_cls<id>(objc_getClass("NSTrackingArea"), sel("alloc")),
                              sel("initWithRect:options:owner:userInfo:"), mayag_cgrect(0, 0, 0, 0),
                              entered_exited | moved | active_always | in_visible_rect | cursor_update, own,
                              static_cast<id>(nullptr));
            if (area != nullptr) { msg<void>(own, sel("addTrackingArea:"), area); msg<void>(area, sel("release")); }
        }
        if (!cfg.child) install_app_menu(cfg.title);
        id view = msg<id>(win, sel("contentView"));
        msg<void>(view, sel("setWantsLayer:"), static_cast<BOOL>(YES));

        id layer = msg<id>(view, sel("layer"));
        if (layer != nullptr) {
            // Configure the layer ONCE, not per frame.
            //
            // Two settings that together cost ~70 ms/frame if left at their
            // defaults on a wide-gamut display:
            //
            //  * `contentsGravity = resize` stops Core Animation from
            //    re-deriving layout on every `setContents:`.
            //  * opaque + no colour matching stops it from running a
            //    synchronous sRGB -> Display P3 conversion over the whole
            //    surface each time we hand it a new image.
            //
            // We already produce correctly-encoded sRGB, so that conversion
            // is both expensive and unwanted.
            msg<void>(layer, sel("setMagnificationFilter:"), nsstring("nearest"));
            msg<void>(layer, sel("setMinificationFilter:"), nsstring("nearest"));
            msg<void>(layer, sel("setContentsGravity:"), nsstring("resize"));
            msg<void>(layer, sel("setOpaque:"), static_cast<BOOL>(YES));
            msg<void>(layer, sel("setNeedsDisplayOnBoundsChange:"), static_cast<BOOL>(NO));
            msg<void>(layer, sel("setDrawsAsynchronously:"), static_cast<BOOL>(NO));

            // Present WITHOUT waiting for the next compositor pass where the
            // platform allows it.
            //
            // This is the dominant latency term: CPU work for a frame is
            // ~1 ms, but handing the result to Core Animation adds up to a
            // full refresh (16.7 ms at 60 Hz) before photons. Nothing we do
            // on the CPU touches that, which is why "make rendering faster"
            // stops mattering long before the user stops noticing lag.
            //
            // Marking the layer opaque with no implicit animations is what we
            // can do from a CALayer; the real fix is a CAMetalLayer with
            // `presentsWithTransaction = NO` and a drawable presented on the
            // display link, which is the GPU backend's job.
            msg<void>(layer, sel("setAllowsGroupOpacity:"), static_cast<BOOL>(NO));

            // THE line that decides whether Retina rendering survives.
            //
            // A CALayer's contentsScale defaults to 1.0. Hand it a 2x image
            // without setting this and Core Animation treats the buffer as
            // low-resolution content for a 1x layer: it DOWNSCALES to the
            // layer's point size, then the compositor scales back up for the
            // display. Every ounce of the extra resolution is destroyed in a
            // round trip, and the result looks exactly like soft 1x output —
            // which is what made the window look blurry while the very same
            // draw list produced pixel-perfect PNGs.
            const auto scale = msg<double>(win, sel("backingScaleFactor"));
            msg<void>(layer, sel("setContentsScale:"), scale);
        }

        MacWindow w;
        w.app_   = app;
        w.window_ = win;
        w.view_  = view;
        w.size_  = cfg.size;
        w.ime_->view = view;
        w.ime_->option_as_alt = cfg.option_as_alt;
        ti_slot(view) = w.ime_.get();
        if (!cfg.child) primary_window() = win;

        // Render at the display's NATIVE scale.
        //
        // This used to be pinned to 1x because the serial rasteriser needed
        // ~26 ms for a 3.6 MP Retina frame. The tiled parallel renderer does
        // the same frame in ~7.8 ms (128 fps) with bit-identical output, so
        // there is no longer a reason to hand the compositor a soft upscale.
        //
        // `MAYAG_DPI` still overrides, for screenshots and pixel comparisons.
        const auto native = static_cast<float>(msg<double>(win, sel("backingScaleFactor")));
        w.native_dpi_ = native > 0.0f ? native : 1.0f;
        w.dpi_ = w.native_dpi_;
        if (const char* forced = std::getenv("MAYAG_DPI")) {
            const float v = std::strtof(forced, nullptr);
            if (v > 0.0f) { w.dpi_ = v; w.dpi_forced_ = true; }
        }

        w.start_ = std::chrono::steady_clock::now();

        // Animation frame rate. 60 is the default; `MAYAG_FPS=30` halves the
        // software rasteriser's load, which matters on battery until a GPU
        // backend lands.
        if (const char* fps = std::getenv("MAYAG_FPS")) {
            const double v = std::strtod(fps, nullptr);
            if (v > 0.0 && v <= 480.0) w.frame_interval_ = 1.0 / v;
        }

        // The software framebuffer is made by the first software present()
        // (it resizes to the surface), never up front: with Metal drawing,
        // a full-size float framebuffer here was ~37 MB at 1920x1200 that
        // nothing ever rendered into.
        w.fb_ = backend::Framebuffer{1, 1};

        // ── GPU, when it is both compiled in and actually available ──────
        //
        // Attaching REPLACES the view's CALayer with a CAMetalLayer, so it
        // has to happen after the software layer above is configured and
        // before the first present. Everything is a fallthrough rather than
        // an error: no Metal in the build, no device in the machine, a
        // shader that fails to compile, a VM with no GPU — each leaves
        // `gpu_` invalid and the window keeps using the path that always
        // works. A missing driver should cost frame time, not the app.
        //
        // MAYAG_BACKEND=software forces the CPU path, which is what makes
        // the two comparable on the same machine: the whole point of the GPU
        // claim is a measured A/B, and that needs a switch.
        w.gpu_enabled_ = w.init_gpu();
        startup::mark("  mac: gpu");

        return w;
    }

    /// Which path this window is actually presenting through.
    [[nodiscard]] std::string_view renderer_name() const noexcept {
#if MAYAG_GPU_METAL
        if (gpu_enabled_) return "metal";
#endif
        return "software";
    }

    [[nodiscard]] bool gpu_active() const noexcept { return gpu_enabled_; }

    void close() {
        unenroll();
        if (view_ != nullptr) ti_slot(view_) = nullptr;   // the view may outlive us in AppKit's pools
        if (window_ != nullptr && primary_window() == window_) primary_window() = nullptr;
        if (window_ != nullptr) { objc::msg<void>(window_, objc::sel("close")); (void)take_close(window_); }
        window_ = nullptr;
        open_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] Vec2  size() const noexcept { return size_; }
    [[nodiscard]] float dpi_scale() const noexcept { return dpi_; }

    /// The user's configured double-click interval.
    ///
    /// A system preference, not a constant: it is adjustable in Accessibility
    /// settings and some users set it several times the default. Hardcoding
    /// 0.4s means those users' double clicks are silently seen as two singles
    /// — the app feels broken and they cannot tell you why.
    [[nodiscard]] double double_click_interval() const {
        return objc::msg_cls<double>(objc_getClass("NSEvent"),
                                     objc::sel("doubleClickInterval"));
    }

    [[nodiscard]] double now() const noexcept {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    // ── events ────────────────────────────────────────────────────────────────────

    /// What arrived since the last call. Under the Cocoa run loop
    /// (macos_loop.hpp) the loop feeds each NSEvent in with feed(); with
    /// pulling on (the default), this drains NSApp itself without waiting.
    [[nodiscard]] std::vector<Event> dispatch() {
        using namespace objc;
        // Presented frames' feedback, collected here so a photon sink hears
        // every sample even when nothing asks for display_timing().
        if (photon_sink_) drain_feedback();
        std::vector<Event> out = std::move(fed_);
        fed_.clear();

        // Track live geometry: the content view's size in points and the
        // window's backing scale. Both are reported as one ResizeEvent, and
        // the FIRST call always reports — the same contract as Wayland's
        // initial configure. Without it a program whose model starts at some
        // other size than the window (the window may also have been clamped
        // to the screen) lays out for a size it never gets told about, and
        // the frame fills only part of the window.
        const CGRect content = msg<CGRect>(view_, sel("frame"));
        const Vec2 now_size{static_cast<float>(content.size.width),
                            static_cast<float>(content.size.height)};
        float now_dpi = dpi_;
        if (!dpi_forced_) {
            // dragged to a display with another scale (Retina <-> 1x), or the
            // display's own scale changed: render at the new native scale
            const auto native = static_cast<float>(msg<double>(window_, sel("backingScaleFactor")));
            if (native > 0.0f) now_dpi = native;
        }
        if (!announced_ || now_size != size_ || now_dpi != dpi_) {
            if (now_dpi != dpi_) {
                native_dpi_ = now_dpi;
                // the software path's CALayer must know the new scale, or Core
                // Animation resamples the image (the GPU path re-sets its
                // CAMetalLayer's scale in present(), keyed on pixel size)
                if (!gpu_enabled_) {
                    if (id layer = msg<id>(view_, sel("layer")))
                        msg<void>(layer, sel("setContentsScale:"), static_cast<double>(now_dpi));
                }
            }
            announced_ = true;
            size_ = now_size;
            dpi_ = now_dpi;
            out.push_back(ResizeEvent{size_, dpi_});
        }

        if (pull_) {
            id until = msg_cls<id>(objc_getClass("NSDate"), sel("distantPast"));
            id mode = nsstring("kCFRunLoopDefaultMode");
            for (;;) {
                id ev = msg<id>(app_, sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                                any_event_mask, until, mode, static_cast<BOOL>(YES));
                if (ev == nullptr) break;
                feed(ev);
                msg<void>(app_, sel("sendEvent:"), ev);
            }
        }
        // a plain loop, not insert(make_move_iterator...): Apple clang 2100 at
        // -O2 through a PCH mangles move_iterator's two constrained
        // operator++(int) overloads alike and refuses the TU
        out.reserve(out.size() + fed_.size());
        for (auto& e : fed_) out.push_back(std::move(e));
        fed_.clear();
        // what the input method said outside a key press (a candidate picked
        // with the mouse, a composition ended because the input source changed)
        if (!ime_->out.empty()) {
            for (auto& e : ime_->out) out.push_back(std::move(e));
            ime_->out.clear();
        }

        // key focus moved between our windows (or away from the app)
        if (window_ != nullptr) {
            const bool key = msg<BOOL>(window_, sel("isKeyWindow")) != 0;
            if (key != was_key_) { was_key_ = key; out.push_back(FocusEvent{key}); }
            // Occlusion: minimised, on another Space, hidden (cmd-H) or fully
            // covered. No frames while so -- CAMetalLayer's nextDrawable would
            // block up to a second per frame on a window nobody can see -- and
            // a fresh one the moment it shows again (a resize event at the
            // same size: the host lays out and paints).
            constexpr unsigned long occlusion_visible = 1ul << 1;   // NSWindowOcclusionStateVisible
            const bool vis = (msg<unsigned long>(window_, sel("occlusionState")) & occlusion_visible) != 0;
            if (vis != visible_) {
                visible_ = vis;
                if (vis) out.push_back(ResizeEvent{size_, dpi_});
            }
        }

        // Closed is what the window was TOLD (its close button, or close),
        // never inferred from visibility: a minimised window, one whose app
        // is hidden (cmd-H), or one mid-way through the minimise animation
        // is not visible either -- and reading that as "closed" quit the app
        // when you minimised it.
        if (open_) {
            const int bits = take_close(window_);
            if (bits & close_done) open_ = false;                     // gone: the host stops
            if (bits) out.push_back(CloseRequest{});                  // a request: the app decides
        }
        return out;
    }

    /// The run loop hands over each event it dequeued, before sendEvent:.
    /// Events of another window (a child window, a panel) are not ours:
    /// they go to that window, and app-wide ones (no window) to the key one.
    void feed(id ev) {
        using namespace objc;
        id w = msg<id>(ev, sel("window"));
        const auto type = msg<unsigned long>(ev, sel("type"));
        const bool keyish = type == ev_key_down || type == ev_key_up || type == ev_flags_changed;
        if (w == nullptr && keyish) w = msg<id>(app_, sel("keyWindow"));
        if (w == nullptr || w == window_) { translate(ev, fed_); return; }
        for (MacWindow* o : registry()) if (o != this && o->window_ == w) { o->translate(ev, o->fed_); return; }
    }
    /// Every open MacWindow, so one run loop can feed them all.
    [[nodiscard]] static std::vector<MacWindow*>& registry() { static std::vector<MacWindow*> r; return r; }
    /// Register this window at its final address (after it has been moved
    /// into place); a host does this once. Idempotent.
    void enroll() {
        auto& r = registry();
        if (std::find(r.begin(), r.end(), this) == r.end()) r.push_back(this);
    }
    void unenroll() { std::erase(registry(), this); }

    /// Off under the Cocoa run loop: events come through feed(), and pulling
    /// from inside a live-resize callback would re-enter AppKit.
    void set_pull(bool on) noexcept { pull_ = on; }

    [[nodiscard]] id ns_app() const noexcept { return app_; }
    [[nodiscard]] id ns_window() const noexcept { return window_; }
    [[nodiscard]] id ns_view() const noexcept { return view_; }
    /// macOS convention: ctrl-click is a right click (off by default).
    void set_ctrl_click_right(bool on) noexcept { ctrl_click_is_right_ = on; }

    /// A text field has keyboard focus (`on`), its caret at `caret` (logical
    /// px): the input method's candidate window opens at the caret. When
    /// focus leaves text entry mid-composition, the composition is dropped.
    void set_text_input(bool on, Rect caret) {
        ime_->want = on;
        if (on) { ime_->caret = caret; ime_->view_h = size_.y; }
        if (!on && ime_->marked && view_ != nullptr) {
            using namespace objc;
            if (id ctx = msg<id>(view_, sel("inputContext"))) msg<void>(ctx, sel("discardMarkedText"));
            ime_->marked = false;
            ime_->marked_text.clear();
            ime_->out.push_back(ComposeEndEvent{});
        }
    }

    // ── presentation ────────────────────────────────────────────────────

    void present(const DrawList& dl, Color<Srgb> clear) {
        const int w = static_cast<int>(size_.x * dpi_);
        const int h = static_cast<int>(size_.y * dpi_);
        if (w <= 0 || h <= 0 || !visible_) return;   // nobody can see it: no work, no blocking

#if MAYAG_GPU_METAL
        if (gpu_enabled_) {
            // The GPU path is four steps shorter than the CPU one: no
            // framebuffer, no per-pixel shading, no sRGB encode pass, no
            // CGImage wrap, no whole-surface blit. The draw list goes to a
            // buffer and the compositor already owns the target texture.
            if (gpu_size_ != Vec2{static_cast<float>(w), static_cast<float>(h)}) {
                gpu_.resize(size_, dpi_);
                gpu_size_ = Vec2{static_cast<float>(w), static_cast<float>(h)};
            }
            if (atlas_sync_) atlas_sync_(gpu_);
            // Same pixels as what's on screen: don't wake the GPU and the
            // compositor for it (a hover or model change that didn't change
            // the picture). Texture and atlas uploads count as changes. The
            // host sees frames_skipped move and keeps the refresh slot free
            // for the change that is coming (the shell's echo).
            if (last_had_frame_ && clear == last_clear_ && dl.same_as(last_list_) && !gpu_.needs_present()) {
                ++frames_skipped_;
                return;
            }
            gpu_.submit(dl, clear, next_tag());
            last_list_.assign_from(dl); last_clear_ = clear; last_had_frame_ = true;
            return;
        }
#endif

        if (fb_.width() != w || fb_.height() != h) {
            fb_ = backend::Framebuffer{w, h};
            pixels_.assign(static_cast<std::size_t>(w) * h * 4, 0);
            release_context();
            blitted_ = false;
        }

        // Tiled + parallel: the clear is folded into each tile (so the pixels
        // are already hot in cache when shading starts), and the sRGB encode
        // runs on the same threads that just produced those rows. Tiles whose
        // inputs did not change keep their pixels -- shaded, encoded and all
        // -- and a frame that changed nothing is not handed to the compositor.
        backend::Tiled::render(dl, fb_, sampler_, &backend::shared_pool(), clear);
        if (!backend::Tiled::any_damage(fb_) && blitted_) { ++frames_skipped_; return; }
        backend::Tiled::encode_damaged(fb_, pixels_, &backend::shared_pool());
        blit(w, h);
        blitted_ = true;
    }

    /// Frames not handed over because they matched what is on screen.
    [[nodiscard]] std::uint64_t frames_skipped() const noexcept { return frames_skipped_; }
    /// See window_host::next_draw_at: with displaySyncEnabled off the layer
    /// shows the newest drawable at the next refresh, so a draw never waits
    /// behind a queued one (MAYAG_PRESENT=fifo restores the queue).
    [[nodiscard]] bool replaces_queued_frames() const noexcept {
#if MAYAG_GPU_METAL
        return gpu_enabled_ && gpu_.replaces_queued();
#else
        return false;
#endif
    }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() { return fb_.to_rgba8(); }

    /// The size, IN PIXELS, of the buffer `read_pixels()` returns. Reported
    /// from the framebuffer itself rather than `size() * dpi_scale()`, so a
    /// caller can never mis-shape the capture (same contract as Wayland).
    [[nodiscard]] Vec2 pixel_size() const noexcept {
        return Vec2{static_cast<float>(fb_.width()), static_cast<float>(fb_.height())};
    }

    // ── presentation timing (GPU path) ───────────────────────────────────────
    //
    // The macOS twin of Wayland's wp_presentation. Every drawable carries a
    // presented handler (MetalDevice), which reports the exact time the
    // frame reached the glass. From that the host gets a real vsync clock,
    // and "input to photon" becomes a measurement from the HARDWARE event
    // timestamp (NSEvent.timestamp, stamped by the HID stack) to scanout --
    // not from when our process happened to read the event.

    /// The display's vsync clock, once a frame has been presented.
    [[nodiscard]] std::optional<DisplayTiming> display_timing() {
        drain_feedback();
        if (vblank_host_s_ <= 0.0 || period_s_ <= 0.0) return std::nullopt;
        using namespace std::chrono;
        // Host time (mach_absolute_time) does not advance in sleep; steady_clock
        // (CLOCK_MONOTONIC on Darwin) does. Map through "now" on both clocks
        // rather than assume they share an epoch.
        const auto steady_now = steady_clock::now();
        const double ago = host_now() - vblank_host_s_;
        const auto vb = steady_now - duration_cast<steady_clock::duration>(duration<double>(ago));
        return DisplayTiming{vb, duration_cast<nanoseconds>(duration<double>(period_s_)), vblank_seq_};
    }

    /// Input to photon, measured by the drawables' presented handlers
    /// (photon.hpp: the same meter as Wayland's).
    [[nodiscard]] PhotonStats photon_stats() {
        drain_feedback();
        return photon_.stats();
    }

    /// How long before a vblank the late-latch draw must be SUBMITTED to make
    /// it. Not a constant: WindowServer composites ahead of scanout, and how
    /// far ahead depends on the machine, the display and the load. Learned
    /// from feedback (AIMD): a late frame that missed its vblank widens it at
    /// once; every one that made it shaves a little off.
    [[nodiscard]] std::chrono::nanoseconds latch_margin() const noexcept {
        return std::chrono::nanoseconds{static_cast<std::int64_t>(latch_margin_s_ * 1e9)};
    }

    /// Frames that missed the vblank they were drawn for, out of those that
    /// were drawn for one. For tests and HUDs.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> latch_misses() const noexcept {
        return {latch_missed_, latch_tried_};
    }

    // ── services ────────────────────────────────────────────────────────

    // ── window actions (an undecorated window's own title area) ─────────────

    /// Move the window with the pointer, from the press being handled now.
    void begin_move() {
        using namespace objc;
        if (window_ == nullptr) return;
        id ev = msg<id>(app_, sel("currentEvent"));
        if (ev != nullptr) msg<void>(window_, sel("performWindowDragWithEvent:"), ev);
    }
    void minimize() {
        if (window_ != nullptr) objc::msg<void>(window_, objc::sel("miniaturize:"), static_cast<id>(nullptr));
    }
    /// Fill the screen's usable area, or go back. A borderless window has no
    /// zoom button for AppKit to drive, so it is done by hand.
    void toggle_maximize() {
        using namespace objc;
        if (window_ == nullptr) return;
        if (maximized_) {
            msg<void>(window_, sel("setFrame:display:animate:"), restore_frame_, static_cast<BOOL>(YES), static_cast<BOOL>(YES));
            maximized_ = false;
            return;
        }
        id screen = msg<id>(window_, sel("screen"));
        if (screen == nullptr) return;
        restore_frame_ = msg<CGRect>(window_, sel("frame"));
        const CGRect vis = msg<CGRect>(screen, sel("visibleFrame"));
        msg<void>(window_, sel("setFrame:display:animate:"), vis, static_cast<BOOL>(YES), static_cast<BOOL>(YES));
        maximized_ = true;
    }

    void set_title(std::string_view t) {
        objc::msg<void>(window_, objc::sel("setTitle:"), objc::nsstring(t));
    }

    void set_cursor(CursorShape shape) {
        using namespace objc;
        const char* name = nullptr;
        switch (shape) {
            case CursorShape::text:        name = "IBeamCursor"; break;
            case CursorShape::pointer:     name = "pointingHandCursor"; break;
            case CursorShape::crosshair:   name = "crosshairCursor"; break;
            case CursorShape::resize_h:    name = "resizeLeftRightCursor"; break;
            case CursorShape::resize_v:    name = "resizeUpDownCursor"; break;
            case CursorShape::grab:        name = "openHandCursor"; break;
            case CursorShape::grabbing:    name = "closedHandCursor"; break;
            case CursorShape::not_allowed: name = "operationNotAllowedCursor"; break;
            default:                       name = "arrowCursor"; break;
        }
        id cursor = msg_cls<id>(objc_getClass("NSCursor"), sel(name));
        cursor_ = cursor;
        if (ime_) ime_->cursor = cursor;
        if (cursor != nullptr) msg<void>(cursor, sel("set"));
    }
    /// AppKit resets the cursor to the arrow on its own (entering the window,
    /// a focus change, a cursor rect update): re-assert the app's.
    void reapply_cursor() const {
        if (cursor_ != nullptr && !cursor_hidden_) objc::msg<void>(cursor_, objc::sel("set"));
    }

    // ── images (image(id) nodes) ────────────────────────────────────────────────

    /// Hand pixels to the renderer for image(id): the CPU store (software
    /// path) and, when Metal is up, a texture (platform/images.hpp: the one
    /// policy every window follows).
    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        images::upload(images_, gpu_images(), id, std::move(img));
    }
    /// Rows of an uploaded image moved in place on the GPU (Metal). false:
    /// not possible here; send the pixels instead.
    bool move_image_rows(std::uint32_t id, int x, int y, int w, int h, int dy) {
#if MAYAG_GPU_METAL
        if (gpu_enabled_) return gpu_.move_image_rows(id, x, y, w, h, dy);
#endif
        (void)id, (void)x, (void)y, (void)w, (void)h, (void)dy;
        return false;
    }
    /// An uploaded image as the GPU holds it (RGBA), for checking. Empty: none.
    [[nodiscard]] std::vector<std::uint8_t> read_image(std::uint32_t id) {
#if MAYAG_GPU_METAL
        if (gpu_enabled_) return gpu_.read_image(id);
#endif
        (void)id;
        return {};
    }
    /// Rewrite rectangles of an uploaded image in place (Metal). false:
    /// upload it whole instead.
    bool update_image_regions(std::uint32_t id, int w, int h, const std::vector<std::array<int, 4>>& rects,
                              const std::vector<std::uint8_t>& packed) {
#if MAYAG_GPU_METAL
        if (gpu_enabled_) return gpu_.update_image_regions(id, w, h, rects, packed);
#endif
        (void)id, (void)w, (void)h, (void)rects, (void)packed;
        return false;
    }
    void release_image(std::uint32_t id) {
        images::release(images_, gpu_images(), id);
    }

  private:
    /// The device presenting images now, or null (no Metal in this build, or
    /// Metal not up): what platform/images.hpp mirrors to.
#if MAYAG_GPU_METAL
    [[nodiscard]] backend::MetalDevice* gpu_images() noexcept { return gpu_enabled_ ? &gpu_ : nullptr; }
#else
    [[nodiscard]] static images::NoGpu* gpu_images() noexcept { return nullptr; }
#endif

  public:

    // ── fullscreen ───────────────────────────────────────────────────────────

    void set_fullscreen(bool on) {
        using namespace objc;
        if (window_ == nullptr) return;
        constexpr unsigned long full = 1ul << 14;  // NSWindowStyleMaskFullScreen
        const bool is = (msg<unsigned long>(window_, sel("styleMask")) & full) != 0;
        if (is != on) msg<void>(window_, sel("toggleFullScreen:"), static_cast<id>(nullptr));
    }

    // ── cursor, as another program asked ──────────────────────────────────────────

    /// A wp_cursor_shape_v1 shape by number, as the nearest NSCursor.
    void set_cursor_shape_raw(std::uint32_t shape) {
        using namespace objc;
        show_cursor();
        const char* name = "arrowCursor";
        switch (shape) {
            case 2: name = "contextualMenuCursor"; break;   // context-menu
            case 4: case 13: name = "pointingHandCursor"; break;  // pointer, alias-ish
            case 7: name = "crosshairCursor"; break;
            case 8: case 9: name = "IBeamCursor"; break;     // text, vertical-text
            case 10: name = "dragLinkCursor"; break;         // alias
            case 11: name = "dragCopyCursor"; break;         // copy
            case 14: case 15: name = "operationNotAllowedCursor"; break;  // no-drop, not-allowed
            case 16: name = "openHandCursor"; break;         // grab
            case 17: name = "closedHandCursor"; break;       // grabbing
            case 18: case 22: case 23: case 26: name = "resizeLeftRightCursor"; break;  // e/w/ew/col
            case 19: case 21: case 24: case 27: name = "resizeUpDownCursor"; break;     // n/s/ns/row
            default: break;
        }
        id cursor = msg_cls<id>(objc_getClass("NSCursor"), sel(name));
        if (cursor != nullptr) msg<void>(cursor, sel("set"));
    }

    void hide_cursor() {
        if (cursor_hidden_) return;
        objc::msg_cls<void>(objc_getClass("NSCursor"), objc::sel("hide"));
        cursor_hidden_ = true;
    }

    /// A cursor drawn by another program: little-endian ARGB words
    /// (premultiplied), hotspot in its pixels.
    void set_cursor_image(const std::uint8_t* argb, int w, int h, int stride, int hot_x, int hot_y) {
        using namespace objc;
        if (argb == nullptr || w <= 0 || h <= 0) return;
        show_cursor();
        id rep = msg<id>(msg_cls<id>(objc_getClass("NSBitmapImageRep"), sel("alloc")),
            sel("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:"
                "colorSpaceName:bytesPerRow:bitsPerPixel:"),
            static_cast<unsigned char**>(nullptr), static_cast<long>(w), static_cast<long>(h), static_cast<long>(8),
            static_cast<long>(4), static_cast<BOOL>(YES), static_cast<BOOL>(NO), nsstring("NSDeviceRGBColorSpace"),
            static_cast<long>(w * 4), static_cast<long>(32));
        if (rep == nullptr) return;
        auto* dst = msg<unsigned char*>(rep, sel("bitmapData"));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto* s = argb + static_cast<std::ptrdiff_t>(y) * stride + x * 4;  // B, G, R, A
                auto* d = dst + (static_cast<std::ptrdiff_t>(y) * w + x) * 4;          // R, G, B, A
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
            }
        id image = msg<id>(msg_cls<id>(objc_getClass("NSImage"), sel("alloc")), sel("initWithSize:"),
                           CGSize{static_cast<double>(w) / dpi_, static_cast<double>(h) / dpi_});
        msg<void>(image, sel("addRepresentation:"), rep);
        id cursor = msg<id>(msg_cls<id>(objc_getClass("NSCursor"), sel("alloc")), sel("initWithImage:hotSpot:"), image,
                            CGPoint{static_cast<double>(hot_x) / dpi_, static_cast<double>(hot_y) / dpi_});
        if (cursor != nullptr) msg<void>(cursor, sel("set"));
        if (custom_cursor_ != nullptr) msg<void>(custom_cursor_, sel("release"));
        custom_cursor_ = cursor;
        msg<void>(image, sel("release"));
        msg<void>(rep, sel("release"));
    }

    /// The raw input tap (see raw_input.hpp): every key press AND release,
    /// as evdev codes. Unset by default.
    void set_raw_input(std::function<void(const RawInput&)> f) { raw_ = std::move(f); }

    // -- input to photon, for a program that forwards input -----------------
    //
    // A remote desktop does not draw the answer to its window's input; a
    // client elsewhere does, later. In manual mode the window's own events
    // no longer start the clock; the program marks the input whose answer
    // the next frame will show (the Wayland window has the same three).

    void set_photon_manual(bool on) noexcept { photon_manual_ = on; }
    /// The next frame drawn shows the answer to input from `at`, in
    /// std::chrono::steady_clock seconds (mapped here onto the presented
    /// handler's clock). The oldest unshown mark is kept.
    void mark_input(double at) noexcept {
        const double steady =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const double age = steady - at;
        if (age < 0.0 || age > 1.0) return;  // not a plausible input time
        photon_.mark(host_now() - age, host_now());
    }
    /// Every measured sample, as it is measured (ms).
    void set_photon_sink(std::function<void(double)> f) { photon_sink_ = static_cast<bool>(f); photon_.set_sink(std::move(f)); }

    void set_clipboard(std::string_view s) {
        using namespace objc;
        id pb = msg_cls<id>(objc_getClass("NSPasteboard"), sel("generalPasteboard"));
        msg<long>(pb, sel("clearContents"));
        msg<BOOL>(pb, sel("setString:forType:"), nsstring(s), nsstring("public.utf8-plain-text"));
    }

    [[nodiscard]] std::string get_clipboard() {
        using namespace objc;
        id pb = msg_cls<id>(objc_getClass("NSPasteboard"), sel("generalPasteboard"));
        id s  = msg<id>(pb, sel("stringForType:"), nsstring("public.utf8-plain-text"));
        return from_nsstring(s);
    }

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        images_.set_glyphs(s);
        sampler_ = &images_;
    }

    /// Give the window the glyph atlas the GPU path must keep in sync.
    ///
    /// The software rasteriser reads the atlas through `CoverageSampler` on
    /// the CPU, so it needs nothing here. The GPU samples a texture, which
    /// has to be uploaded — but the window must not learn what a FontStack
    /// is to do that. Taking the atlas as a template parameter and storing a
    /// closure keeps the dependency pointing the right way: the runtime,
    /// which already knows about fonts, tells the window how to sync; the
    /// window just calls it. On a software build this compiles to nothing.
    template <typename AtlasT>
    void set_atlas_source([[maybe_unused]] AtlasT* atlas) {
#if MAYAG_GPU_METAL
        if (atlas == nullptr) { atlas_sync_ = nullptr; return; }
        atlas_sync_ = [atlas](backend::MetalDevice& d) { d.sync_atlas(*atlas); };
#endif
    }

  private:
    /// Longest the event loop will sleep with nothing to do.
    ///
    /// `distantFuture` is the textbook answer and it is subtly wrong here: it
    /// makes the loop unable to notice anything that did not arrive as a
    /// window event — a worker thread's result, a `send()` from application
    /// code, an atlas that wants collecting. Waking a few times a second
    /// costs nothing measurable (an idle app still reads ~0% CPU) and means
    /// the runtime can never be wedged by a state change it did not see.

    /// Bring up the GPU backend, or report that we are staying on the CPU.
    ///
    /// Every failure is a fallthrough. The one thing this must NOT do is
    /// half-attach: `MetalDevice::attach` swaps the view's layer as one of
    /// its first steps, so if a later step fails (shader compile, sampler)
    /// the view is left with a CAMetalLayer that nothing renders into — a
    /// black window that looks like a hang. Restoring the software layer on
    /// failure is what keeps the fallback honest.
    [[nodiscard]] bool init_gpu() {
#if MAYAG_GPU_METAL
        if (!backend::gpu_policy::allows(backend::gpu_policy::Choice::metal)) return false;

        id saved_layer = objc::msg<id>(view_, objc::sel("layer"));

        if (gpu_.attach(view_, dpi_)) {
            gpu_.resize(size_, dpi_);
            gpu_size_ = Vec2{size_.x * dpi_, size_.y * dpi_};
            return true;
        }

        // Put the CALayer back, so a failed GPU init is invisible rather
        // than fatal.
        if (saved_layer != nullptr) {
            objc::msg<void>(view_, objc::sel("setLayer:"), saved_layer);
            objc::msg<void>(view_, objc::sel("setWantsLayer:"), static_cast<BOOL>(YES));
        }
        return false;
#else
        return false;
#endif
    }

    [[nodiscard]] static id block_deadline() {
        return objc::msg_cls<id>(objc_getClass("NSDate"),
                                 objc::sel("dateWithTimeIntervalSinceNow:"), 0.25);
    }

    [[nodiscard]] static id distant_future() {
        return objc::msg_cls<id>(objc_getClass("NSDate"), objc::sel("distantFuture"));
    }

    /// Blit the RGBA8 framebuffer into the layer via a CGImage.
    ///
    /// The context and colour space are CACHED across frames. Creating a
    /// fresh CGBitmapContext every frame cost ~79 ms at 1020x880 — an order
    /// of magnitude more than rendering the frame — because each one
    /// allocates, zeroes, and tears down a multi-megabyte buffer. Rebuilding
    /// only on resize takes that to near zero.
    void blit(int w, int h) {
        using namespace objc;

        if (ctx_ == nullptr || ctx_w_ != w || ctx_h_ != h) {
            release_context();
            cs_ = CGColorSpaceCreateDeviceRGB();
            ctx_ = CGBitmapContextCreate(
                pixels_.data(), static_cast<std::size_t>(w), static_cast<std::size_t>(h),
                8, static_cast<std::size_t>(w) * 4, cs_, mayag_cg_rgba8);
            ctx_w_ = w;
            ctx_h_ = h;
        }
        if (ctx_ == nullptr) return;

        CGImageRef img = CGBitmapContextCreateImage(ctx_);
        if (img == nullptr) return;

        // The layer was configured at open(); per frame we only swap the
        // image. Wrapping the swap in a transaction with actions disabled
        // prevents Core Animation from starting an implicit animation on
        // every single frame.
        id tx = reinterpret_cast<id>(objc_getClass("CATransaction"));
        msg_cls<void>(objc_getClass("CATransaction"), sel("begin"));
        msg_cls<void>(objc_getClass("CATransaction"), sel("setDisableActions:"),
                      static_cast<BOOL>(YES));
        (void)tx;

        id layer = msg<id>(view_, sel("layer"));
        msg<void>(layer, sel("setContents:"), reinterpret_cast<id>(img));

        msg_cls<void>(objc_getClass("CATransaction"), sel("commit"));
        CGImageRelease(img);
    }

    void release_context() {
        if (ctx_ != nullptr) { CGContextRelease(ctx_); ctx_ = nullptr; }
        if (cs_  != nullptr) { CGColorSpaceRelease(cs_); cs_ = nullptr; }
    }

    void translate(id ev, std::vector<Event>& out) {
        using namespace objc;
        const std::size_t before = out.size();
        translate_one(ev, out);
        // Every event translate_one emits is user input (resize and close are
        // reported by dispatch()). Stamp it with the time the HID stack saw
        // it, which is where input-to-photon honestly starts -- unless it is
        // a bare modifier, whose answer is nothing on screen (photon.hpp).
        if (photon_manual_) return;
        for (std::size_t i = before; i < out.size(); ++i)
            if (starts_photon_clock(out[i])) { note_input(msg<double>(ev, sel("timestamp"))); break; }
    }

    static double host_now() noexcept {
        // mach_absolute_time's clock: NSEvent.timestamp and presentedTime.
        return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9;
    }

    /// An input event arrived; the meter keeps the oldest one not yet on
    /// screen (photon.hpp).
    void note_input(double ts) noexcept {
        const double now = host_now();
        // NSEvent.timestamp is seconds since boot on the mach_absolute_time
        // clock, the same one host_now() reads. Guard anyway: a
        // synthetic event (timestamp 0) must not record a 40-day latency.
        photon_.note((ts > now - 1.0 && ts <= now + 0.001) ? ts : now, now);
    }

#if MAYAG_GPU_METAL
    /// Every frame committed while an input is unshown carries it, so a frame
    /// replaced before scanout never takes the sample with it.
    [[nodiscard]] backend::FrameTag next_tag() noexcept {
        const double now = host_now();
        const PhotonMeter::Frame f = photon_.take(now);
        backend::FrameTag tag;
        tag.input_s  = f.input_at;
        tag.read_s   = f.read_at;
        tag.commit_s = f.commit_at;
        if (vblank_host_s_ > 0.0 && period_s_ > 0.0) {
            const double n   = std::ceil((now - vblank_host_s_) / period_s_);
            tag.target_s     = vblank_host_s_ + std::max(n, 1.0) * period_s_;
        }
        return tag;
    }
#endif

    void drain_feedback() {
#if MAYAG_GPU_METAL
        if (!gpu_enabled_) return;
        auto frames = gpu_.take_presented();
        if (frames.empty()) return;
        refresh_period();
        for (const auto& f : frames) {
            // Replaced by a newer frame before scanout: nothing to measure.
            // The input it carried is still unshown in the meter, and the
            // frames after it carry it too (next_tag), so the sample is not lost.
            if (f.presented_s <= 0.0) continue;
            if (f.presented_s > vblank_host_s_) {
                vblank_host_s_ = f.presented_s;
                ++vblank_seq_;
            }
            photon_.presented(PhotonMeter::Frame{f.tag.input_s, f.tag.read_s, f.tag.commit_s}, f.presented_s,
                              period_s_ * 1e3);
            if (PhotonMeter::trace_level() >= 2 && f.tag.target_s > 0.0) {
                // the late-latch schedule: how early the frame was handed
                // over for the vblank it aimed at, and how far it slipped
                ::mayag::diag::say("latch lead %.3f slip %.3f ms\n",
                             (f.tag.target_s - f.submit_s) * 1e3, (f.presented_s - f.tag.target_s) * 1e3);
            }
            learn_latch(f);
        }
#endif
    }

#if MAYAG_GPU_METAL
    void learn_latch(const backend::Presented& f) noexcept {
        if (f.tag.target_s <= 0.0 || period_s_ <= 0.0) return;
        const double lead = f.tag.target_s - f.submit_s;    // how early it was handed over
        if (lead <= 0.0 || lead > period_s_ * 0.5) return;  // an early-slot draw says nothing
        ++latch_tried_;
        const double cap   = period_s_ * 0.5;
        const bool  missed = f.presented_s > f.tag.target_s + period_s_ * 0.5;
        if (missed) {
            ++latch_missed_;
            latch_margin_s_ = std::min(std::max(latch_margin_s_ + 0.5e-3, lead + 0.5e-3), cap);
        } else {
            latch_margin_s_ = std::max(latch_margin_s_ - 10e-6, kMinLatchMargin);
        }
    }
#endif

    /// The panel's refresh period. maximumFramesPerSecond is 60 on a fixed
    /// panel and 120 on ProMotion, which runs at its maximum while frames are
    /// arriving -- exactly when the latch schedule matters.
    void refresh_period() {
        using namespace objc;
        id screen = window_ != nullptr ? msg<id>(window_, sel("screen")) : nullptr;
        if (screen == nullptr) return;
        if (!msg<BOOL>(screen, sel("respondsToSelector:"), sel("maximumFramesPerSecond"))) return;
        const long fps = msg<long>(screen, sel("maximumFramesPerSecond"));
        if (fps > 0) period_s_ = 1.0 / static_cast<double>(fps);
    }

    void translate_one(id ev, std::vector<Event>& out) {
        using namespace objc;

        const auto type  = msg<unsigned long>(ev, sel("type"));
        const auto flags = msg<unsigned long long>(ev, sel("modifierFlags"));

        Mods mods{};
        mods.shift = (flags & mod_shift)   != 0;
        mods.ctrl  = (flags & mod_control) != 0;
        mods.alt   = (flags & mod_option)  != 0;
        mods.super = (flags & mod_command) != 0;

        // Cocoa's origin is bottom-left; mayag's is top-left.
        const CGPoint loc = msg<CGPoint>(ev, sel("locationInWindow"));
        const Vec2 p{static_cast<float>(loc.x), size_.y - static_cast<float>(loc.y)};

        switch (type) {
            case ev_mouse_exited:
                // The pointer left the window: park it off-surface so hover
                // clears (the same contract as Wayland's pointer leave).
                out.push_back(MouseMove{Vec2{-1.0f, -1.0f}, Vec2{}, mods});
                last_mouse_ = Vec2{-1.0f, -1.0f};
                break;
            case ev_mouse_entered:
                out.push_back(MouseMove{p, Vec2{}, mods});
                last_mouse_ = p;
                reapply_cursor();
                break;
            case ev_mouse_moved:
            case ev_left_dragged:
            case ev_right_dragged:
            case ev_other_dragged:
                if (p == last_mouse_) break;   // AppKit repeats a still pointer (tracking + window): one event
                out.push_back(MouseMove{p, p - last_mouse_, mods});
                last_mouse_ = p;
                break;

            case ev_left_down:
                // ctrl-click as the Mac's right click, when the app opts in
                // (set_ctrl_click_right) -- off by default, because terminal
                // apps and editors bind ctrl+click themselves.
                if (mods.ctrl && !mods.super && !mods.alt && !mods.shift && ctrl_click_is_right_) {
                    right_from_ctrl_ = true;
                    out.push_back(MouseDown{p, MouseButton::right, mods, 1});
                    break;
                }
                out.push_back(MouseDown{p, MouseButton::left, mods,
                                        static_cast<int>(msg<long>(ev, sel("clickCount")))});
                break;
            case ev_left_up:
                if (right_from_ctrl_) { right_from_ctrl_ = false; out.push_back(MouseUp{p, MouseButton::right, mods}); break; }
                out.push_back(MouseUp{p, MouseButton::left, mods});
                break;
            case ev_right_down:
                out.push_back(MouseDown{p, MouseButton::right, mods,
                                        static_cast<int>(msg<long>(ev, sel("clickCount")))});
                break;
            case ev_right_up:
                out.push_back(MouseUp{p, MouseButton::right, mods});
                break;
            case ev_other_down:
            case ev_other_up: {
                // buttonNumber: 2 middle, 3 back, 4 forward (the side buttons)
                const long bn = msg<long>(ev, sel("buttonNumber"));
                const MouseButton b = bn == 3 ? MouseButton::back : bn == 4 ? MouseButton::forward : MouseButton::middle;
                if (type == ev_other_down) out.push_back(MouseDown{p, b, mods, static_cast<int>(msg<long>(ev, sel("clickCount")))});
                else out.push_back(MouseUp{p, b, mods});
                break;
            }

            case ev_scroll_wheel: {
                // Trackpads report precise sub-pixel deltas; wheels report
                // coarse "lines" that need scaling to feel right.
                const bool precise = msg<BOOL>(ev, sel("hasPreciseScrollingDeltas")) != 0;
                const double dx = msg<double>(ev, sel("scrollingDeltaX"));
                const double dy = msg<double>(ev, sel("scrollingDeltaY"));
                const float k = precise ? 1.0f : 16.0f;
                const auto phase = msg<unsigned long>(ev, sel("momentumPhase"));
                out.push_back(ScrollEvent{
                    Vec2{static_cast<float>(dx) * k, static_cast<float>(dy) * k},
                    p, mods, phase != 0});
                break;
            }

            case ev_key_down: {
                const auto code = msg<unsigned short>(ev, sel("keyCode"));
                const bool repeat = msg<BOOL>(ev, sel("isARepeat")) != 0;
                if (!repeat) raw_key(ev, true, mods);
                // While the input method composes, this key is its own (Enter
                // picks the candidate, Escape cancels): not the app's.
                const Key key = key_of(ev, code);
                Mods km = mods;
                // ⌥ types characters (the Mac way): ⌥e is text, not Alt+e --
                // a terminal must not ALSO send ESC e. Keys that type nothing
                // (arrows, Delete, Return) keep their ⌥.
                if (km.alt && !ime_->option_as_alt && !km.ctrl && !km.super && is_char_key(key)) km.alt = false;
                if (!ime_->marked) out.push_back(KeyEvent{key, km, repeat});
                // Text does NOT come from `characters`: AppKit is about to
                // hand this key to the content view (sendEvent:), whose input
                // method commits text with insertText: -- so a dead key (´ e
                // → é) or a CJK composition (konnichiwa → こんにちは) types what
                // the user meant, not its Latin keystrokes. A shortcut and the
                // character it would type still never both fire.
                break;
            }
            case ev_key_up:
                raw_key(ev, false, mods);
                break;
            case ev_flags_changed: {
                // A modifier went down or up: which, from its own flag bit.
                // Caps Lock toggles, so each change is a press and release.
                const auto code = msg<unsigned short>(ev, sel("keyCode"));
                if (code == 0x39) {
                    raw_key(ev, true, mods);
                    raw_key(ev, false, mods);
                } else if (const auto bit = modifier_bit(code); bit != 0) {
                    raw_key(ev, (flags & bit) != 0, mods);
                }
                raw_flags_ = flags;
                break;
            }
            default:
                break;
        }
    }

    /// The Key a key-down/up event is. A character key is the character the
    /// layout puts on it (charactersIgnoringModifiers -- what AppKit's own
    /// menu shortcuts match), so ctrl+z is the key labelled Z on AZERTY and
    /// Dvorak, exactly as on Wayland (platform/keys.hpp holds the one rule).
    /// Named keys and the keypad keep their position; so does a key typing
    /// no Latin character (ж, é, a dead key): ctrl+c still copies there.
    /// Not for flagsChanged events, which have no characters (AppKit throws).
    [[nodiscard]] static Key key_of(id ev, unsigned short code) {
        using namespace objc;
        const Key pos = key_from_code(code);
        constexpr unsigned short kIsoSection = 10;   // § / < : a character key with no US twin
        if (!is_char_key(pos) && code != kIsoSection) return pos;
        const std::string s = from_nsstring(msg<id>(ev, sel("charactersIgnoringModifiers")));
        return layout_key(pos, s.size() == 1 ? static_cast<unsigned char>(s[0]) : 0u);
    }

    /// Virtual key codes are POSITIONAL on macOS (they describe where the key
    /// is, not what it prints), so this table is layout-independent.
    [[nodiscard]] static Key key_from_code(unsigned short c) {
        switch (c) {
            case 0:  return Key::a;  case 11: return Key::b;  case 8:  return Key::c;
            case 2:  return Key::d;  case 14: return Key::e;  case 3:  return Key::f;
            case 5:  return Key::g;  case 4:  return Key::h;  case 34: return Key::i;
            case 38: return Key::j;  case 40: return Key::k;  case 37: return Key::l;
            case 46: return Key::m;  case 45: return Key::n;  case 31: return Key::o;
            case 35: return Key::p;  case 12: return Key::q;  case 15: return Key::r;
            case 1:  return Key::s;  case 17: return Key::t;  case 32: return Key::u;
            case 9:  return Key::v;  case 13: return Key::w;  case 7:  return Key::x;
            case 16: return Key::y;  case 6:  return Key::z;

            case 29: return Key::n0; case 18: return Key::n1; case 19: return Key::n2;
            case 20: return Key::n3; case 21: return Key::n4; case 23: return Key::n5;
            case 22: return Key::n6; case 26: return Key::n7; case 28: return Key::n8;
            case 25: return Key::n9;

            case 36:  return Key::enter;     case 48: return Key::tab;
            case 49:  return Key::space;     case 51: return Key::backspace;
            case 53:  return Key::escape;    case 117: return Key::del;
            case 123: return Key::left;      case 124: return Key::right;
            case 125: return Key::down;      case 126: return Key::up;
            case 115: return Key::home;      case 119: return Key::end;
            case 116: return Key::page_up;   case 121: return Key::page_down;

            case 122: return Key::f1;  case 120: return Key::f2;  case 99:  return Key::f3;
            case 118: return Key::f4;  case 96:  return Key::f5;  case 97:  return Key::f6;
            case 98:  return Key::f7;  case 100: return Key::f8;  case 101: return Key::f9;
            case 109: return Key::f10; case 103: return Key::f11; case 111: return Key::f12;

            case 27: return Key::minus;         case 24: return Key::equal;
            case 33: return Key::bracket_left;  case 30: return Key::bracket_right;
            case 42: return Key::backslash;     case 41: return Key::semicolon;
            case 39: return Key::quote;         case 43: return Key::comma;
            case 47: return Key::period;        case 44: return Key::slash;
            case 50: return Key::grave;
            default: return Key::unknown;
        }
    }

    id    app_    = nullptr;
    id    window_ = nullptr;
    id    view_   = nullptr;
    bool  open_   = true;
    Vec2  size_{1024, 640};
    float dpi_ = 1.0f;
    float native_dpi_ = 1.0f;   ///< what the display actually offers
    bool  dpi_forced_ = false;  ///< MAYAG_DPI pins the scale; display changes are ignored
    bool  announced_ = false;   ///< the first dispatch() reports the real geometry
    bool  was_key_ = true;      ///< last reported keyboard focus
    bool  maximized_ = false;   ///< toggle_maximize's state, and where it came from
    bool  visible_ = true;      ///< on screen (not minimised / hidden / covered)
    bool  right_from_ctrl_ = false;      ///< this left press was reported as right (ctrl-click)
    bool  ctrl_click_is_right_ = false;  ///< off: a terminal's apps see ctrl+click as itself
    CGRect restore_frame_{};

    /// What happened to each window since the last dispatch: its close button
    /// was pressed (a request), or it was really closed (by AppKit, or by
    /// someone calling close). AppKit runs on one thread; so does this.
    enum : int { close_requested = 1, close_done = 2 };
    static std::vector<std::pair<id, int>>& close_notes() { static std::vector<std::pair<id, int>> v; return v; }
    static void note_close(id w, int what) {
        for (auto& [win, bits] : close_notes()) if (win == w) { bits |= what; return; }
        close_notes().emplace_back(w, what);
    }
    static int take_close(id w) {
        auto& v = close_notes();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->first == w) { const int bits = it->second; v.erase(it); return bits; }
        return 0;
    }

    // ── text input: the system input method ──────────────────────────────────
    //
    // Keys reach the content view (keyDown:), which hands them to AppKit's
    // input manager (interpretKeyEvents:). That calls back as an
    // NSTextInputClient: insertText: is committed text, setMarkedText: a
    // composition (こん on the way to こんにちは, ´ waiting for its e),
    // doCommandBySelector: a key that is a command -- swallowed, because mayag
    // reads keys from the event stream and NSView's default for an unknown
    // command is the alert beep. While something is composing the keys are
    // the input method's: no KeyEvent goes out for them.

    struct NSRangeM { unsigned long location, length; };
    static constexpr unsigned long kNotFound = 0x7fffffffffffffffUL;   // NSNotFound

    struct Ime {
        id view = nullptr;
        std::vector<Event> out;   ///< what it said since the last dispatch
        bool marked = false;      ///< composing
        std::string marked_text;
        bool want = false;        ///< a text field has focus
        bool option_as_alt = false;
        id cursor = nullptr;      ///< the app's cursor, for cursorUpdate:
        Rect caret{};             ///< its caret, logical px from the top-left
        float view_h = 0.0f;
    };
    std::unique_ptr<Ime> ime_ = std::make_unique<Ime>();

    static std::unordered_map<id, Ime*>& ti_table() { static std::unordered_map<id, Ime*> t; return t; }
    static Ime*& ti_slot(id view) { return ti_table()[view]; }
    static Ime* ime_of(id view) {
        const auto it = ti_table().find(view);
        return it == ti_table().end() ? nullptr : it->second;
    }

    /// The NSString in an NSString or NSAttributedString.
    static std::string text_of(id s) {
        using namespace objc;
        if (s == nullptr) return {};
        if (msg<BOOL>(s, sel("isKindOfClass:"), objc_getClass("NSAttributedString"))) s = msg<id>(s, sel("string"));
        return from_nsstring(s);
    }
    /// UTF-16 offset (what AppKit counts in) -> byte offset in UTF-8.
    static std::uint32_t byte_at(const std::string& s, unsigned long u16) {
        std::size_t i = 0;
        unsigned long n = 0;
        while (i < s.size() && n < u16) {
            const auto c = static_cast<unsigned char>(s[i]);
            const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n += len == 4 ? 2 : 1;
            i += len;
        }
        return static_cast<std::uint32_t>(std::min(i, s.size()));
    }
    static unsigned long u16_len(const std::string& s) {
        unsigned long n = 0;
        for (std::size_t i = 0; i < s.size();) {
            const auto c = static_cast<unsigned char>(s[i]);
            const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n += len == 4 ? 2 : 1;
            i += len;
        }
        return n;
    }
    static bool is_text(const std::string& s) {
        if (s.empty()) return false;
        const auto u = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
        // Arrows, F-keys, Home/End report characters in Apple's private-use
        // block U+F700..U+F8FF: keys, not text.
        const bool function_key = s.size() >= 3 && u(0) == 0xEF && u(1) >= 0x9C && u(1) <= 0xA3;
        return u(0) >= 0x20 && u(0) != 0x7F && !function_key;
    }
    static void ime_commit(Ime& m, const std::string& s) {
        if (m.marked) { m.marked = false; m.marked_text.clear(); m.out.push_back(ComposeEndEvent{}); }
        if (is_text(s)) m.out.push_back(TextEvent{s});
    }

    static Class view_class() {
        static Class cls = [] {
            using namespace objc;
            if (Class existing = objc_getClass("MayagView")) return existing;
            Class c = objc_allocateClassPair(objc_getClass("NSView"), "MayagView", 0);
            if (Protocol* p = objc_getProtocol("NSTextInputClient")) class_addProtocol(c, p);
            const auto yes = +[](id, SEL) -> BOOL { return YES; };
            const auto key_down = +[](id self, SEL, id ev) {
                Ime* m = ime_of(self);
                if (m == nullptr) return;
                constexpr unsigned long cmd = 1ul << 20, ctrl = 1ul << 18, opt = 1ul << 19;
                const auto flags = msg<unsigned long>(ev, sel("modifierFlags"));
                // ⌘/⌃ chords (and ⌥ ones, when ⌥ is Alt) are shortcuts, not
                // text -- unless composing, when every key is the input method's
                const unsigned long chord = cmd | ctrl | (m->option_as_alt ? opt : 0ul);
                if (!m->marked && (flags & chord) != 0) return;
                id arr = msg_cls<id>(objc_getClass("NSArray"), sel("arrayWithObject:"), ev);
                msg<void>(self, sel("interpretKeyEvents:"), arr);
            };
            const auto swallow = +[](id, SEL, id) {};
            const auto do_command = +[](id, SEL, SEL) {};   // never the beep
            const auto insert2 = +[](id self, SEL, id s, NSRangeM) {
                if (Ime* m = ime_of(self)) ime_commit(*m, text_of(s));
            };
            const auto insert1 = +[](id self, SEL, id s) {
                if (Ime* m = ime_of(self)) ime_commit(*m, text_of(s));
            };
            const auto set_marked = +[](id self, SEL, id s, NSRangeM sel_r, NSRangeM) {
                Ime* m = ime_of(self);
                if (m == nullptr) return;
                std::string t = text_of(s);
                if (t.empty()) {
                    if (m->marked) { m->marked = false; m->marked_text.clear(); m->out.push_back(ComposeEndEvent{}); }
                    return;
                }
                m->marked = true;
                const std::uint32_t a = byte_at(t, sel_r.location), b = byte_at(t, sel_r.location + sel_r.length);
                m->marked_text = t;
                m->out.push_back(ComposeEvent{std::move(t), a, b, sel_r.length > 0 ? b : a});
            };
            const auto unmark = +[](id self, SEL) {
                Ime* m = ime_of(self);
                if (m == nullptr || !m->marked) return;
                const std::string t = m->marked_text;   // accepted as it stands
                ime_commit(*m, t);
            };
            const auto has_marked = +[](id self, SEL) -> BOOL { const Ime* m = ime_of(self); return m && m->marked ? YES : NO; };
            const auto marked_range = +[](id self, SEL) -> NSRangeM {
                const Ime* m = ime_of(self);
                return m && m->marked ? NSRangeM{0, u16_len(m->marked_text)} : NSRangeM{kNotFound, 0};
            };
            const auto selected_range = +[](id, SEL) -> NSRangeM { return NSRangeM{0, 0}; };
            const auto valid_attrs = +[](id, SEL) -> id { return msg_cls<id>(objc_getClass("NSArray"), sel("array")); };
            const auto substring = +[](id, SEL, NSRangeM, NSRangeM*) -> id { return nullptr; };
            const auto char_index = +[](id, SEL, CGPoint) -> unsigned long { return kNotFound; };
            const auto first_rect = +[](id self, SEL, NSRangeM, NSRangeM* actual) -> CGRect {
                // the candidate window opens under the caret
                if (actual != nullptr) *actual = NSRangeM{kNotFound, 0};
                const Ime* m = ime_of(self);
                Rect c = m ? m->caret : Rect{};
                const float vh = m ? m->view_h : 0.0f;
                const CGRect local = mayag_cgrect(c.origin.x, vh - c.origin.y - std::max(c.size.y, 1.0f),
                                                  std::max(c.size.x, 1.0f), std::max(c.size.y, 1.0f));
                const CGRect in_win = msg<CGRect>(self, sel("convertRect:toView:"), local, static_cast<id>(nullptr));
                id win = msg<id>(self, sel("window"));
                return win ? msg<CGRect>(win, sel("convertRectToScreen:"), in_win) : in_win;
            };
            class_addMethod(c, sel("acceptsFirstResponder"), reinterpret_cast<IMP>(+yes), "c@:");
            // A click on a window in the background acts at once (a TUI's
            // button, a tab) instead of only focusing the window.
            const auto yes1 = +[](id, SEL, id) -> BOOL { return YES; };
            class_addMethod(c, sel("acceptsFirstMouse:"), reinterpret_cast<IMP>(+yes1), "c@:@");
            // mayag reads pointer events from the event stream; the tracking
            // area's callbacks to the view need no handling (and must not walk
            // the responder chain).
            for (const char* s : {"mouseMoved:", "mouseEntered:", "mouseExited:"})
                class_addMethod(c, sel(s), reinterpret_cast<IMP>(+swallow), "v@:@");
            // AppKit asks the view which cursor to show (entering, refocus):
            // the app's, not NSView's default arrow.
            const auto cursor_update = +[](id self, SEL, id) {
                if (const Ime* m = ime_of(self); m != nullptr && m->cursor != nullptr) msg<void>(m->cursor, sel("set"));
                else msg<void>(msg_cls<id>(objc_getClass("NSCursor"), sel("arrowCursor")), sel("set"));
            };
            class_addMethod(c, sel("cursorUpdate:"), reinterpret_cast<IMP>(+cursor_update), "v@:@");
            class_addMethod(c, sel("keyDown:"), reinterpret_cast<IMP>(+key_down), "v@:@");
            class_addMethod(c, sel("keyUp:"), reinterpret_cast<IMP>(+swallow), "v@:@");
            class_addMethod(c, sel("doCommandBySelector:"), reinterpret_cast<IMP>(+do_command), "v@::");
            class_addMethod(c, sel("insertText:replacementRange:"), reinterpret_cast<IMP>(+insert2), "v@:@{_NSRange=QQ}");
            class_addMethod(c, sel("insertText:"), reinterpret_cast<IMP>(+insert1), "v@:@");
            class_addMethod(c, sel("setMarkedText:selectedRange:replacementRange:"), reinterpret_cast<IMP>(+set_marked),
                            "v@:@{_NSRange=QQ}{_NSRange=QQ}");
            class_addMethod(c, sel("unmarkText"), reinterpret_cast<IMP>(+unmark), "v@:");
            class_addMethod(c, sel("hasMarkedText"), reinterpret_cast<IMP>(+has_marked), "c@:");
            class_addMethod(c, sel("markedRange"), reinterpret_cast<IMP>(+marked_range), "{_NSRange=QQ}@:");
            class_addMethod(c, sel("selectedRange"), reinterpret_cast<IMP>(+selected_range), "{_NSRange=QQ}@:");
            class_addMethod(c, sel("validAttributesForMarkedText"), reinterpret_cast<IMP>(+valid_attrs), "@@:");
            class_addMethod(c, sel("attributedSubstringForProposedRange:actualRange:"), reinterpret_cast<IMP>(+substring),
                            "@@:{_NSRange=QQ}^{_NSRange=QQ}");
            class_addMethod(c, sel("characterIndexForPoint:"), reinterpret_cast<IMP>(+char_index), "Q@:{CGPoint=dd}");
            class_addMethod(c, sel("firstRectForCharacterRange:actualRange:"), reinterpret_cast<IMP>(+first_rect),
                            "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
            objc_registerClassPair(c);
            return c;
        }();
        return cls;
    }

    // ── the app: its menu, and quitting ─────────────────────────────────────────────

    /// The app's main window: where an app-wide quit (⌘Q, the Dock's Quit,
    /// logging out) is delivered, as a close request the app can answer.
    static id& primary_window() { static id w = nullptr; return w; }

    /// A macOS app without a menu bar is not a macOS app: ⌘Q, ⌘H and ⌘M do
    /// nothing and the menu bar shows a stranger's menus. The standard ones,
    /// built once. Quit goes through the delegate below, so it can be refused.
    static void install_app_menu(std::string_view title) {
        using namespace objc;
        static bool done = false;
        if (done) return;
        done = true;
        id app = msg_cls<id>(objc_getClass("NSApplication"), sel("sharedApplication"));
        if (msg<id>(app, sel("mainMenu")) != nullptr) return;   // the host app has its own
        const auto make = [](const char* cls) { return msg<id>(msg_cls<id>(objc_getClass(cls), sel("alloc")), sel("init")); };
        const auto item = [](id menu, const std::string& t, const char* action, const char* key, unsigned long mods = 1ul << 20) {
            id it = msg<id>(menu, sel("addItemWithTitle:action:keyEquivalent:"), nsstring(t), sel(action), nsstring(key));
            if (*key != '\0') msg<void>(it, sel("setKeyEquivalentModifierMask:"), mods);
            return it;
        };
        const auto separator = [](id menu) { msg<void>(menu, sel("addItem:"), msg_cls<id>(objc_getClass("NSMenuItem"), sel("separatorItem"))); };
        const std::string name{title.empty() ? std::string_view{"mayag"} : title};
        id bar = make("NSMenu");
        id app_item = make("NSMenuItem");
        msg<void>(bar, sel("addItem:"), app_item);
        id app_menu = msg<id>(msg_cls<id>(objc_getClass("NSMenu"), sel("alloc")), sel("initWithTitle:"), nsstring(name));
        item(app_menu, "About " + name, "orderFrontStandardAboutPanel:", "");
        separator(app_menu);
        item(app_menu, "Hide " + name, "hide:", "h");
        item(app_menu, "Hide Others", "hideOtherApplications:", "h", (1ul << 20) | (1ul << 19));
        item(app_menu, "Show All", "unhideAllApplications:", "");
        separator(app_menu);
        item(app_menu, "Quit " + name, "terminate:", "q");
        msg<void>(app_item, sel("setSubmenu:"), app_menu);

        id win_item = make("NSMenuItem");
        msg<void>(bar, sel("addItem:"), win_item);
        id win_menu = msg<id>(msg_cls<id>(objc_getClass("NSMenu"), sel("alloc")), sel("initWithTitle:"), nsstring("Window"));
        item(win_menu, "Minimize", "miniaturize:", "m");
        item(win_menu, "Enter Full Screen", "toggleFullScreen:", "f", (1ul << 20) | (1ul << 18));
        separator(win_menu);
        item(win_menu, "Bring All to Front", "arrangeInFront:", "");
        msg<void>(win_item, sel("setSubmenu:"), win_menu);

        msg<void>(app, sel("setMainMenu:"), bar);
        msg<void>(app, sel("setWindowsMenu:"), win_menu);
        msg<void>(app, sel("setDelegate:"), make(delegate_class_name()));   // kept: the delegate is weak
    }

    static const char* delegate_class_name() {
        static const char* name = [] {
            using namespace objc;
            if (objc_getClass("MayagAppDelegate") != nullptr) return "MayagAppDelegate";
            Class c = objc_allocateClassPair(objc_getClass("NSObject"), "MayagAppDelegate", 0);
            // ⌘Q, the Dock's Quit, logout: ask the main window's app (it may
            // have a running command to confirm), never die on the spot
            const auto should_terminate = +[](id, SEL, id app) -> unsigned long {
                id w = primary_window();
                if (w == nullptr) return 1;   // NSTerminateNow: nothing to ask
                note_close(w, close_requested);
                // wake the loop: a quit from the Dock arrives as an Apple event,
                // not an NSEvent, and would otherwise wait for the next input
                id ev = msg_cls<id>(objc_getClass("NSEvent"),
                    sel("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"),
                    static_cast<unsigned long>(15), CGPoint{0, 0}, 0ul, 0.0, 0l, static_cast<id>(nullptr),
                    static_cast<short>(0), 0l, 0l);
                if (ev != nullptr) msg<void>(app, sel("postEvent:atStart:"), ev, static_cast<BOOL>(NO));
                return 0;   // NSTerminateCancel
            };
            const auto no = +[](id, SEL, id) -> BOOL { return NO; };
            class_addMethod(c, sel("applicationShouldTerminate:"), reinterpret_cast<IMP>(+should_terminate), "Q@:@");
            class_addMethod(c, sel("applicationShouldTerminateAfterLastWindowClosed:"), reinterpret_cast<IMP>(+no), "c@:@");
            objc_registerClassPair(c);
            return "MayagAppDelegate";
        }();
        return name;
    }

    /// mayag's NSWindow: it may become key without a title bar, and it
    /// SWALLOWS key events. mayag reads keys from the event stream itself;
    /// left to NSWindow, every key reaches the end of the responder chain
    /// and AppKit plays the "nobody handled that" alert -- a beep per keypress.
    /// Its close button does not close it: it asks (a CloseRequest), and the
    /// app decides -- it may have a running command to confirm first.
    static Class window_class() {
        static Class cls = [] {
            if (Class existing = objc_getClass("MayagWindow")) return existing;
            Class c = objc_allocateClassPair(objc_getClass("NSWindow"), "MayagWindow", 0);
            const auto yes = +[](id, SEL) -> BOOL { return YES; };
            const auto swallow = +[](id, SEL, id) {};
            const auto ask_close = +[](id self, SEL, id) { note_close(self, close_requested); };
            const auto close = +[](id self, SEL s) {
                note_close(self, close_done);
                objc_super sup{self, objc_getClass("NSWindow")};
                reinterpret_cast<void (*)(objc_super*, SEL)>(objc_msgSendSuper)(&sup, s);
            };
            class_addMethod(c, objc::sel("canBecomeKeyWindow"), reinterpret_cast<IMP>(+yes), "c@:");
            class_addMethod(c, objc::sel("canBecomeMainWindow"), reinterpret_cast<IMP>(+yes), "c@:");
            class_addMethod(c, objc::sel("keyDown:"), reinterpret_cast<IMP>(+swallow), "v@:@");
            class_addMethod(c, objc::sel("keyUp:"), reinterpret_cast<IMP>(+swallow), "v@:@");
            class_addMethod(c, objc::sel("performClose:"), reinterpret_cast<IMP>(+ask_close), "v@:@");
            class_addMethod(c, objc::sel("close"), reinterpret_cast<IMP>(+close), "v@:");
            objc_registerClassPair(c);
            return c;
        }();
        return cls;
    }
    Vec2  last_mouse_{};
    bool  pull_ = true;
    std::vector<Event> fed_;

    /// Frame pacing for the animated path.
    double frame_interval_ = 1.0 / 60.0;
    double last_frame_ = 0.0;
    double last_tick_  = 0.0;

    backend::Framebuffer      fb_{1, 1};
    std::vector<std::uint8_t> pixels_;
    bool                      blitted_ = false;   ///< pixels_ is on screen (a frame with no damage need not be)
    const backend::CoverageSampler* sampler_ = nullptr;
    backend::ImageStore images_;          // image(id) for the software path
    std::function<void(const RawInput&)> raw_;
    unsigned long long raw_flags_ = 0;    // modifier flags last seen, for raw key-up of modifiers
    bool cursor_hidden_ = false;
    id cursor_ = nullptr;          ///< the shape the app asked for (re-set after AppKit resets it)
    id custom_cursor_ = nullptr;

    void show_cursor() {
        if (!cursor_hidden_) return;
        objc::msg_cls<void>(objc_getClass("NSCursor"), objc::sel("unhide"));
        cursor_hidden_ = false;
    }

    /// macOS virtual keycode (kVK_*) -> evdev keycode (linux/input-event-codes.h),
    /// the one keyboard vocabulary i4-style programs forward. 0: no evdev key.
    [[nodiscard]] static std::uint32_t evdev_from_code(unsigned short c) {
        static constexpr std::uint16_t map[128] = {
            30, 31, 32, 33, 35, 34, 44, 45, 46, 47, 86, 48, 16, 17, 18, 19,      // 0x00
            21, 20, 2, 3, 4, 5, 7, 6, 13, 10, 8, 12, 9, 11, 27, 24,               // 0x10
            22, 26, 23, 25, 28, 38, 36, 40, 37, 39, 43, 51, 53, 49, 50, 52,       // 0x20
            15, 57, 41, 14, 0, 1, 126, 125, 42, 58, 56, 29, 54, 100, 97, 0,       // 0x30
            187, 83, 0, 55, 0, 78, 0, 69, 115, 114, 113, 98, 96, 0, 74, 188,      // 0x40
            189, 117, 82, 79, 80, 81, 75, 76, 77, 71, 190, 72, 73, 124, 89, 121,  // 0x50
            63, 64, 65, 61, 66, 67, 0, 87, 0, 183, 186, 184, 0, 68, 0, 88,        // 0x60
            0, 185, 110, 102, 104, 111, 62, 107, 60, 109, 59, 105, 106, 108, 103, 0,  // 0x70
        };
        return c < 128 ? map[c] : 0;
    }

    /// The device-dependent flag bit for a modifier key (left/right apart).
    [[nodiscard]] static unsigned long long modifier_bit(unsigned short code) {
        switch (code) {
            case 0x38: return 0x2;      // left shift
            case 0x3C: return 0x4;      // right shift
            case 0x3B: return 0x1;      // left control
            case 0x3E: return 0x2000;   // right control
            case 0x3A: return 0x20;     // left option
            case 0x3D: return 0x40;     // right option
            case 0x37: return 0x8;      // left command
            case 0x36: return 0x10;     // right command
            default: return 0;
        }
    }

    void raw_key(id ev, bool pressed, const Mods& mods) {
        using namespace objc;
        if (!raw_) return;
        const auto code = msg<unsigned short>(ev, sel("keyCode"));
        const auto e = evdev_from_code(code);
        if (e == 0) return;
        const auto t = static_cast<std::uint32_t>(msg<double>(ev, sel("timestamp")) * 1000.0);
        // the same Key the KeyEvent carries (Wayland's raw keys resolve too)
        const bool keyed = msg<unsigned long>(ev, sel("type")) != ev_flags_changed;
        raw_(raw::Key{t, e, pressed, keyed ? key_of(ev, code) : key_from_code(code), mods});
    }

    bool gpu_enabled_ = false;
    Vec2 gpu_size_{};
#if MAYAG_GPU_METAL
    backend::MetalDevice gpu_{};
    /// Set by the runtime when a font engine is bound. Type-erased so this
    /// header keeps knowing nothing about the font stack — the window moves
    /// pixels, it does not understand typefaces.
    std::function<void(backend::MetalDevice&)> atlas_sync_;
    // The last frame handed to Metal, to skip an identical one (as Wayland).
    DrawList    last_list_{};
    Color<Srgb> last_clear_{};
    bool        last_had_frame_ = false;
#endif
    std::uint64_t frames_skipped_ = 0;   ///< identical frames not resubmitted

    /// Cached across frames; rebuilt only when the surface resizes.
    CGContextRef    ctx_ = nullptr;
    CGColorSpaceRef cs_  = nullptr;
    int             ctx_w_ = 0;
    int             ctx_h_ = 0;

    std::chrono::steady_clock::time_point start_{};

    // Presentation timing (host seconds, CACurrentMediaTime's clock).
    static constexpr double kMinLatchMargin = 0.75e-3;
    double        vblank_host_s_  = 0.0;      // last frame that reached the glass
    double        period_s_       = 1.0 / 60.0;
    std::uint64_t vblank_seq_     = 0;
    double        latch_margin_s_ = 2.0e-3;   // learned; see latch_margin()
    std::uint64_t latch_tried_ = 0, latch_missed_ = 0;
    PhotonMeter   photon_;                    // input to photon (photon.hpp, shared with Wayland)
    bool          photon_manual_ = false;
    bool          photon_sink_ = false;       // someone listens: drain feedback every dispatch
};

// What MacWindow can do, stated: checked both ways against what its methods
// give it (platform/concepts.hpp) -- losing one or gaining one unstated
// fails the build, naming the capability.
static_assert(check_capabilities<MacWindow, caps<cap::replaces_queued_frames, cap::counts_skipped_frames, cap::latches_late,
                                                        cap::counts_latch_misses, cap::reports_display_timing, cap::fullscreens,
                                                        cap::manages_itself, cap::uploads_images, cap::patches_images,
                                                        cap::moves_image_rows, cap::shares_event_queue, cap::takes_text_input,
                                                        cap::measures_photons>>());

}  // namespace mayag::platform
