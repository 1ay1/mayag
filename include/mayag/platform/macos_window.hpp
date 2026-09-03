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

#include "../app/event.hpp"
#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../render/draw_list.hpp"
#include "types.hpp"

#if defined(MAYAG_WITH_METAL)
#include "../backend/metal.hpp"
#endif

#include <chrono>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
        if (app == nullptr) return std::nullopt;
        msg<BOOL>(app, sel("setActivationPolicy:"), static_cast<long>(0));  // Regular

        // `finishLaunching` is NOT optional when you drive the run loop
        // yourself instead of calling `[NSApp run]`. Without it AppKit never
        // completes its startup handshake, and the window is created but
        // never mapped by the window server — the process sits there alive,
        // consuming events, showing nothing. Costly to diagnose, one line to fix.
        msg<void>(app, sel("finishLaunching"));

        unsigned long style = style_titled | style_closable | style_miniaturizable;
        if (cfg.resizable) style |= style_resizable;

        const CGRect frame = mayag_cgrect(0, 0, cfg.size.x, cfg.size.y);

        id win = msg_cls<id>(objc_getClass("NSWindow"), sel("alloc"));
        win = msg<id>(win, sel("initWithContentRect:styleMask:backing:defer:"),
                      frame, style, static_cast<unsigned long>(2) /* Buffered */,
                      static_cast<BOOL>(NO));
        if (win == nullptr) return std::nullopt;

        msg<void>(win, sel("setTitle:"), nsstring(cfg.title));
        msg<void>(win, sel("center"));
        msg<void>(win, sel("setReleasedWhenClosed:"), static_cast<BOOL>(NO));
        msg<void>(win, sel("setAcceptsMouseMovedEvents:"), static_cast<BOOL>(YES));
        msg<void>(win, sel("makeKeyAndOrderFront:"), nullptr);
        msg<void>(app, sel("activateIgnoringOtherApps:"), static_cast<BOOL>(YES));

        // A layer-backed content view is what we blit the framebuffer into.
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
            if (v > 0.0f) w.dpi_ = v;
        }

        w.start_ = std::chrono::steady_clock::now();

        // Animation frame rate. 60 is the default; `MAYAG_FPS=30` halves the
        // software rasteriser's load, which matters on battery until a GPU
        // backend lands.
        if (const char* fps = std::getenv("MAYAG_FPS")) {
            const double v = std::strtod(fps, nullptr);
            if (v > 0.0 && v <= 480.0) w.frame_interval_ = 1.0 / v;
        }

        w.fb_    = backend::Framebuffer{static_cast<int>(cfg.size.x * w.dpi_),
                                        static_cast<int>(cfg.size.y * w.dpi_)};

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

        return w;
    }

    /// Which path this window is actually presenting through.
    [[nodiscard]] std::string_view renderer_name() const noexcept {
#if defined(MAYAG_WITH_METAL)
        if (gpu_enabled_) return "metal";
#endif
        return "software";
    }

    [[nodiscard]] bool gpu_active() const noexcept { return gpu_enabled_; }

    void close() {
        if (window_ != nullptr) objc::msg<void>(window_, objc::sel("close"));
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

    // ── events ──────────────────────────────────────────────────────────

    [[nodiscard]] std::vector<Event> poll_events(Wait wait, double timeout) {
        using namespace objc;
        std::vector<Event> out;

        // Track live size, so a resize by the user is reported once.
        const CGRect content = msg<CGRect>(view_, sel("frame"));
        const Vec2 now_size{static_cast<float>(content.size.width),
                            static_cast<float>(content.size.height)};
        if (now_size != size_) {
            size_ = now_size;
            out.push_back(ResizeEvent{size_, dpi_});
        }

        // The deadline encodes the runtime's Wait decision.
        //
        // `poll` means "an animation is running", NOT "burn a core". Cocoa
        // has no vsync primitive we can block on here, so we wait until the
        // next frame boundary.
        //
        // CPU use: a busy 2x Retina frame costs ~7.8 ms across 8 cores with
        // the tiled renderer, so 60 Hz animation is comfortable. An idle app
        // blocks here and measures ~1%. `MAYAG_FPS` trades smoothness for
        // battery if an app wants it.
        id until = block_deadline();
        if (wait == Wait::immediate) {
            // No sleeping: the runtime owes the screen a frame.
            until = msg_cls<id>(objc_getClass("NSDate"), sel("distantPast"));
        } else if (wait == Wait::poll) {
            const double now_s = now();
            const double next  = last_frame_ + frame_interval_;
            const double sleep = next - now_s;
            last_frame_ = sleep > 0.0 ? next : now_s;
            until = (sleep > 0.0005)
                ? msg_cls<id>(objc_getClass("NSDate"),
                              sel("dateWithTimeIntervalSinceNow:"), sleep)
                : msg_cls<id>(objc_getClass("NSDate"), sel("distantPast"));
        } else if (wait == Wait::timeout) {
            until = msg_cls<id>(objc_getClass("NSDate"),
                                sel("dateWithTimeIntervalSinceNow:"), timeout);
        }

        id mode = nsstring("kCFRunLoopDefaultMode");

        for (;;) {
            id ev = msg<id>(app_, sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                            any_event_mask, until, mode, static_cast<BOOL>(YES));
            if (ev == nullptr) break;

            translate(ev, out);
            msg<void>(app_, sel("sendEvent:"), ev);

            // After the first (possibly blocking) wait, drain without waiting.
            until = msg_cls<id>(objc_getClass("NSDate"), sel("distantPast"));
        }

        // Emit a frame tick so animations advance. The runtime only asks for
        // `Wait::poll` when something is animating, so this is not generated
        // for an idle app.
        if (wait == Wait::poll) {
            const double t = now();
            out.push_back(FrameEvent{t, t - last_tick_ > 0.0 ? num::min(t - last_tick_, 0.1) : 1.0 / 60.0});
            last_tick_ = t;
        }

        if (!msg<BOOL>(window_, sel("isVisible"))) {
            open_ = false;
            out.push_back(CloseRequest{});
        }
        return out;
    }

    // ── presentation ────────────────────────────────────────────────────

    void present(const DrawList& dl, Color<Srgb> clear) {
        const int w = static_cast<int>(size_.x * dpi_);
        const int h = static_cast<int>(size_.y * dpi_);
        if (w <= 0 || h <= 0) return;

#if defined(MAYAG_WITH_METAL)
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
            gpu_.submit(dl, clear);
            return;
        }
#endif

        if (fb_.width() != w || fb_.height() != h) {
            fb_ = backend::Framebuffer{w, h};
            pixels_.assign(static_cast<std::size_t>(w) * h * 4, 0);
            release_context();
        }

        // Tiled + parallel: the clear is folded into each tile (so the pixels
        // are already hot in cache when shading starts), and the sRGB encode
        // runs on the same threads that just produced those rows.
        backend::Tiled::render(dl, fb_, sampler_, &backend::shared_pool(), clear);
        backend::Tiled::encode_parallel(fb_, pixels_, &backend::shared_pool());
        blit(w, h);
    }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() { return fb_.to_rgba8(); }

    // ── services ────────────────────────────────────────────────────────

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
        if (cursor != nullptr) msg<void>(cursor, sel("set"));
    }

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

    void set_coverage_sampler(const backend::CoverageSampler* s) { sampler_ = s; }

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
#if defined(MAYAG_WITH_METAL)
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
#if defined(MAYAG_WITH_METAL)
        if (const char* forced = std::getenv("MAYAG_BACKEND")) {
            if (std::string_view{forced} != "metal") return false;
        }

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
            case ev_mouse_moved:
            case ev_left_dragged:
            case ev_right_dragged:
            case ev_other_dragged:
                out.push_back(MouseMove{p, p - last_mouse_, mods});
                last_mouse_ = p;
                break;

            case ev_left_down:
                out.push_back(MouseDown{p, MouseButton::left, mods,
                                        static_cast<int>(msg<long>(ev, sel("clickCount")))});
                break;
            case ev_left_up:
                out.push_back(MouseUp{p, MouseButton::left, mods});
                break;
            case ev_right_down:
                out.push_back(MouseDown{p, MouseButton::right, mods, 1});
                break;
            case ev_right_up:
                out.push_back(MouseUp{p, MouseButton::right, mods});
                break;
            case ev_other_down:
                out.push_back(MouseDown{p, MouseButton::middle, mods, 1});
                break;
            case ev_other_up:
                out.push_back(MouseUp{p, MouseButton::middle, mods});
                break;

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
                out.push_back(KeyEvent{key_from_code(code), mods, repeat});

                // Printable input arrives separately, so a shortcut and the
                // character it would type never both fire.
                //
                // NOTE ON INPUT METHODS: reading `characters` directly gives
                // the Latin keystrokes and BYPASSES the input manager, which
                // is why CJK cannot be typed this way — `konnichiwa` arrives
                // as ten ASCII letters and the conversion to こんにちは never
                // happens. A complete implementation routes the event through
                // `interpretKeyEvents:` on an NSTextInputClient view, which
                // calls back with `insertText:` and `setMarkedText:`.
                //
                // That requires registering a custom NSView subclass through
                // the Objective-C runtime, which is a substantial amount of
                // class-pair plumbing. The EVENT MODEL is what matters and it
                // is complete: ComposeEvent / ComposeEndEvent flow through
                // the runtime, TextEditState holds preedit separately from
                // committed text, and the headless backend can script a full
                // composition — so CJK input is testable today and the Cocoa
                // bridge is a contained piece of work rather than a redesign.
                if (!mods.super && !mods.ctrl) {
                    const std::string s = from_nsstring(msg<id>(ev, sel("characters")));
                    if (!s.empty() && static_cast<unsigned char>(s[0]) >= 0x20 &&
                        static_cast<unsigned char>(s[0]) != 0x7F) {
                        out.push_back(TextEvent{s});
                    }
                }
                break;
            }
            default:
                break;
        }
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
    Vec2  last_mouse_{};

    /// Frame pacing for the animated path.
    double frame_interval_ = 1.0 / 60.0;
    double last_frame_ = 0.0;
    double last_tick_  = 0.0;

    backend::Framebuffer      fb_{1, 1};
    std::vector<std::uint8_t> pixels_;
    const backend::CoverageSampler* sampler_ = nullptr;

    bool gpu_enabled_ = false;
    Vec2 gpu_size_{};
#if defined(MAYAG_WITH_METAL)
    backend::MetalDevice gpu_{};
    /// Set by the runtime when a font engine is bound. Type-erased so this
    /// header keeps knowing nothing about the font stack — the window moves
    /// pixels, it does not understand typefaces.
    std::function<void(backend::MetalDevice&)> atlas_sync_;
#endif

    /// Cached across frames; rebuilt only when the surface resizes.
    CGContextRef    ctx_ = nullptr;
    CGColorSpaceRef cs_  = nullptr;
    int             ctx_w_ = 0;
    int             ctx_h_ = 0;

    std::chrono::steady_clock::time_point start_{};
};

}  // namespace mayag::platform
