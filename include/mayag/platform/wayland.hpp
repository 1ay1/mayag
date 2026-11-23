#pragma once
#include "../core/target.hpp"
// mayag::platform::WaylandWindow — the native Linux backend
//
// Wayland rather than X11 because it is what every current Linux desktop
// actually runs, and because its buffer model is the one that lets a CPU
// renderer be fast: the client allocates shared memory, the compositor maps
// the SAME pages, and "present" is a pointer handoff. There is no upload, no
// server-side copy, no X protocol round trip per frame.
//
// ── the fast path, end to end ───────────────────────────────────────────
//
// The obvious implementation of a software-rendered Wayland window does this
// per frame:
//
//     render -> Vec4 framebuffer          (linear float, 16 B/px)
//     to_rgba8() -> std::vector<uint8_t>  (allocate + encode, 4 B/px)
//     memcpy into the shm buffer          (another full-frame copy)
//     swizzle RGBA -> BGRA                (a third pass over every pixel)
//
// At 2560x1440 that is ~14 MB of allocation and three full-frame passes over
// 3.7 M pixels, every frame, before the compositor has done anything. mayag
// does none of it. `encode_into()` writes the final BGRA bytes DIRECTLY into
// the mmap'd shm pages the compositor will read, in the same parallel pass
// that already had those tiles hot in L2. The frame is rendered and delivered
// in one traversal, and the only per-frame allocation is zero.
//
// Four more things carry the rest of the win:
//
//   * TRIPLE BUFFERING with release tracking. Two buffers stall whenever the
//     compositor still holds one; three means `present` never waits. Buffers
//     are picked by "not currently held", not round-robin, so a slow frame
//     does not cascade.
//
//   * DAMAGE FROM THE DRAW LIST. mayag already knows every rect it touched.
//     The union of those, snapped out to pixels, is sent as damage_buffer, so
//     a blinking cursor repaints ~200 px rather than 3.7 M. This is the single
//     biggest win for a text app and it costs one Rect union per frame.
//
//   * FRAME CALLBACKS as the vsync source. The compositor tells us when it
//     wants the next frame; we never sleep on a timer and never render a frame
//     that will be discarded. An occluded window gets no callbacks and so
//     burns no CPU at all.
//
//   * NO WAITING OF ITS OWN. fd() hands the display socket to jaal's reactor,
//     which sleeps on it; dispatch() reads what arrived without blocking. An
//     idle app is asleep in epoll at 0% CPU.

#include "../core/event.hpp"
#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../core/geometry.hpp"
#include "../core/locale.hpp"
#include "../core/startup.hpp"
#include "../render/draw_list.hpp"
#include "raw_input.hpp"
#include "types.hpp"
#include "keys.hpp"
#include "photon.hpp"
#include "wayland_protocol.hpp"

#include <jaal/core/capability.hpp>
#include <jaal/kernel/pool.hpp>

// The Vulkan backend is the default renderer on Linux; it self-loads at
// runtime and falls back to the shm path if the GPU is unavailable. Compiled
// in when MAYAG_WITH_VULKAN is defined (the default on Linux); otherwise the
// shm path is the only renderer.
#ifdef MAYAG_WITH_VULKAN
#include "../backend/vulkan.hpp"
#endif

#if MAYAG_OS_LINUX || MAYAG_OS_FREEBSD

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <functional>
#include <array>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mayag::platform {

namespace wldetail {

/// An anonymous shared-memory file. memfd_create is used directly through the
/// syscall so this works on any libc, glibc or musl, old or new, with no
/// feature-test dance.
[[nodiscard]] inline int make_shm_fd(std::size_t size) noexcept {
#if defined(SYS_memfd_create)
    int fd = static_cast<int>(::syscall(SYS_memfd_create, "mayag-shm", 0u));
    if (fd >= 0) {
        if (::ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
        ::close(fd);
    }
#endif
    // Fallback for kernels without memfd: an unlinked file in the runtime dir.
    const char* dir = ::getenv("XDG_RUNTIME_DIR");
    if (dir == nullptr) dir = "/tmp";
    std::string tmpl = std::string{dir} + "/mayag-XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    int shm_fd = ::mkstemp(path.data());
    if (shm_fd < 0) return -1;
    ::unlink(path.data());
    if (::ftruncate(shm_fd, static_cast<off_t>(size)) != 0) {
        ::close(shm_fd);
        return -1;
    }
    return shm_fd;
}

/// One shm buffer: the fd, the mapping, and the wl_buffer the compositor sees.
///
/// `busy` is the whole reason this class exists. A wl_buffer handed to the
/// compositor may still be scanned out after commit; writing to it before the
/// `release` event arrives is a tear, and it is the classic Wayland bug.
struct ShmBuffer {
    wl::proxy*     buffer = nullptr;
    std::uint8_t*  pixels = nullptr;
    std::size_t    bytes  = 0;
    int            width  = 0;
    int            height = 0;
    bool           busy   = false;
    /// Region of THIS buffer that is stale relative to the live framebuffer.
    /// Accumulates while the buffer waits its turn in the rotation; cleared
    /// when the buffer is re-encoded. This is what makes a partial encode
    /// safe with more than one buffer in flight.
    Rect           pending_damage{};

    void unmap() noexcept {
        if (pixels != nullptr) {
            ::munmap(pixels, bytes);
            pixels = nullptr;
        }
        if (buffer != nullptr) {
            wl::request_destroy(buffer, wl::buffer_destroy);
            buffer = nullptr;
        }
        bytes = 0;
        width = height = 0;
        busy = false;
    }
};

/// Smallest rect containing both. An empty operand is identity, so a fresh
/// buffer accumulates damage from the first frame that touches it.
[[nodiscard]] inline Rect union_rect(const Rect& a, const Rect& b) noexcept {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const float x0 = a.left()   < b.left()   ? a.left()   : b.left();
    const float y0 = a.top()    < b.top()    ? a.top()    : b.top();
    const float x1 = a.right()  > b.right()  ? a.right()  : b.right();
    const float y1 = a.bottom() > b.bottom() ? a.bottom() : b.bottom();
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// Linux monotonic clock in seconds.
[[nodiscard]] inline double monotonic_now() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

/// Client-side key repeat.
///
/// Wayland does not repeat keys for a client: the compositor announces a
/// rate and delay (wl_keyboard.repeat_info) and the CLIENT generates the
/// repeats. Without this, holding Backspace deletes one character and
/// holding an arrow moves once — every GTK/Qt app repeats, so a mayag app
/// that does not reads as broken.
///
/// Pure: times are seconds on any monotonic clock, so it is tested headless.
struct KeyRepeat {
    std::int32_t rate  = 25;    ///< repeats per second; 0 disables (per protocol)
    std::int32_t delay = 600;   ///< ms before the first repeat
    std::uint32_t key = 0;      ///< evdev code held, 0 = none
    double next = 0.0;          ///< when the next repeat fires

    /// A key went down. Modifier keys and keys the keymap says do not
    /// repeat (`repeats` false) never start a repeat.
    void press(std::uint32_t code, bool repeats, double now) noexcept {
        if (!repeats || rate <= 0) { key = 0; return; }
        key  = code;
        next = now + static_cast<double>(delay) / 1000.0;
    }
    void release(std::uint32_t code) noexcept { if (code == key) key = 0; }
    void cancel() noexcept { key = 0; }
    [[nodiscard]] bool active() const noexcept { return key != 0; }

    /// How many repeats are due by `now` (advancing the clock). Capped so a
    /// stalled loop does not dump a burst of hundreds of deletions.
    [[nodiscard]] int due(double now) noexcept {
        if (key == 0 || rate <= 0 || now < next) return 0;
        const double period = 1.0 / static_cast<double>(rate);
        int n = 0;
        while (next <= now && n < 4) { next += period; ++n; }
        if (next <= now) next = now + period;
        return n;
    }
};

/// zwp_text_input_v3's double-buffered state, applied at `done`.
///
/// The input method sends preedit_string / commit_string /
/// delete_surrounding_text in any order and nothing takes effect until
/// `done`, which applies them in the protocol's fixed order: delete the
/// surrounding text, insert the commit string, then show the new preedit.
/// This turns one `done` into mayag's events (TextEvent for commits,
/// ComposeEvent / ComposeEndEvent for the preedit), so the IME speaks the
/// same event model the headless backend scripts and macOS will.
struct TextInputState {
    // pending (since the last done)
    std::string preedit, commit;
    std::int32_t cursor_begin = -1, cursor_end = -1;
    bool has_preedit = false, has_commit = false;
    std::uint32_t delete_before = 0, delete_after = 0;
    // applied
    bool composing = false;
    std::uint32_t serial = 0;   ///< number of commit requests we sent

    void on_preedit(const char* text, std::int32_t begin, std::int32_t end) {
        preedit = text != nullptr ? text : "";
        cursor_begin = begin; cursor_end = end;
        has_preedit = true;
    }
    void on_commit(const char* text) {
        commit = text != nullptr ? text : "";
        has_commit = true;
    }
    void on_delete(std::uint32_t before, std::uint32_t after) {
        delete_before = before; delete_after = after;
    }

    /// Apply at `done`. `out` receives the events in protocol order.
    template <class Out>
    void on_done(Out&& out) {
        // Deleting surrounding text needs the app's buffer; mayag does not
        // report surrounding text, so a compliant IME sends none. Ignored.
        delete_before = delete_after = 0;
        // A commit (or an explicit empty preedit) ends the composition.
        const bool new_preedit = has_preedit && !preedit.empty();
        if (composing && (has_commit || !new_preedit)) {
            out(Event{ComposeEndEvent{}});
            composing = false;
        }
        if (has_commit && !commit.empty()) out(Event{TextEvent{commit}});
        if (new_preedit) {
            const auto len = static_cast<std::uint32_t>(preedit.size());
            // -1/-1 means "hide the cursor": put it at the end.
            const std::uint32_t b = cursor_begin < 0 ? len : std::min<std::uint32_t>(static_cast<std::uint32_t>(cursor_begin), len);
            const std::uint32_t e = cursor_end < 0 ? b : std::min<std::uint32_t>(static_cast<std::uint32_t>(cursor_end), len);
            out(Event{ComposeEvent{preedit, std::min(b, e), std::max(b, e), e}});
            composing = true;
        }
        preedit.clear(); commit.clear();
        has_preedit = has_commit = false;
        cursor_begin = cursor_end = -1;
    }

    /// Focus left the text input: drop anything half-composed.
    template <class Out>
    void reset(Out&& out) {
        if (composing) out(Event{ComposeEndEvent{}});
        composing = false;
        preedit.clear(); commit.clear();
        has_preedit = has_commit = false;
    }
};

/// Shift/Ctrl/Alt/Super/Caps/Num/AltGr: keys that never auto-repeat (the
/// fallback when no keymap can say).
[[nodiscard]] constexpr bool is_modifier_evdev(std::uint32_t code) noexcept {
    switch (code) {
        case 29: case 97:            // ctrl
        case 42: case 54:            // shift
        case 56: case 100:           // alt, altgr
        case 125: case 126:          // super
        case 58: case 69: case 70:   // caps, num, scroll lock
            return true;
        default: return false;
    }
}

/// Linux evdev keycode -> mayag Key.
///
/// Wayland sends raw evdev codes, which are POSITIONAL: they name the physical
/// key, not the character on it. That is exactly what mayag's `Key` means
/// (printable characters arrive separately as TextEvent), so this maps
/// straight across with no keymap involvement and works identically on QWERTY,
/// AZERTY, Dvorak, and Colemak.
[[nodiscard]] inline Key key_from_evdev(std::uint32_t code) noexcept {
    switch (code) {
        case 1:   return Key::escape;
        case 2:   return Key::n1;
        case 3:   return Key::n2;
        case 4:   return Key::n3;
        case 5:   return Key::n4;
        case 6:   return Key::n5;
        case 7:   return Key::n6;
        case 8:   return Key::n7;
        case 9:   return Key::n8;
        case 10:  return Key::n9;
        case 11:  return Key::n0;
        case 12:  return Key::minus;
        case 13:  return Key::equal;
        case 14:  return Key::backspace;
        case 15:  return Key::tab;
        case 16:  return Key::q;
        case 17:  return Key::w;
        case 18:  return Key::e;
        case 19:  return Key::r;
        case 20:  return Key::t;
        case 21:  return Key::y;
        case 22:  return Key::u;
        case 23:  return Key::i;
        case 24:  return Key::o;
        case 25:  return Key::p;
        case 26:  return Key::bracket_left;
        case 27:  return Key::bracket_right;
        case 28:  return Key::enter;
        case 29:  return Key::ctrl;         // left ctrl
        case 30:  return Key::a;
        case 31:  return Key::s;
        case 32:  return Key::d;
        case 33:  return Key::f;
        case 34:  return Key::g;
        case 35:  return Key::h;
        case 36:  return Key::j;
        case 37:  return Key::k;
        case 38:  return Key::l;
        case 39:  return Key::semicolon;
        case 40:  return Key::quote;
        case 41:  return Key::grave;
        case 42:  return Key::shift;        // left shift
        case 43:  return Key::backslash;
        case 44:  return Key::z;
        case 45:  return Key::x;
        case 46:  return Key::c;
        case 47:  return Key::v;
        case 48:  return Key::b;
        case 49:  return Key::n;
        case 50:  return Key::m;
        case 51:  return Key::comma;
        case 52:  return Key::period;
        case 53:  return Key::slash;
        case 54:  return Key::shift;        // right shift
        case 56:  return Key::alt;          // left alt
        case 57:  return Key::space;
        case 58:  return Key::caps_lock;
        case 59:  return Key::f1;
        case 60:  return Key::f2;
        case 61:  return Key::f3;
        case 62:  return Key::f4;
        case 63:  return Key::f5;
        case 64:  return Key::f6;
        case 65:  return Key::f7;
        case 66:  return Key::f8;
        case 67:  return Key::f9;
        case 68:  return Key::f10;
        case 87:  return Key::f11;
        case 88:  return Key::f12;
        case 96:  return Key::enter;        // keypad enter
        case 97:  return Key::ctrl;         // right ctrl
        case 100: return Key::alt;          // right alt
        case 102: return Key::home;
        case 103: return Key::up;
        case 104: return Key::page_up;
        case 105: return Key::left;
        case 106: return Key::right;
        case 107: return Key::end;
        case 108: return Key::down;
        case 109: return Key::page_down;
        case 110: return Key::insert;
        case 111: return Key::del;
        case 125: return Key::super;        // left super
        case 126: return Key::super;        // right super
        default:  return Key::unknown;
    }
}

/// The NAMED key a keysym means (Key::unknown for printable keysyms).
///
/// Named keys follow the keymap, not the position: with `caps:escape` the
/// Caps Lock key IS Escape, and a virtual keyboard (on-screen, remote
/// desktop, wtype) sends whatever keycode its own keymap gives BackSpace.
/// GTK and Qt read these from the keysym; so does mayag. Letters and digits
/// stay positional (key_from_evdev) so shortcuts keep their place.
[[nodiscard]] constexpr Key key_from_keysym(std::uint32_t sym) noexcept {
    switch (sym) {
        case 0xff08: return Key::backspace;
        case 0xff09: case 0xfe20: return Key::tab;          // Tab, ISO_Left_Tab
        case 0xff0d: case 0xff8d: return Key::enter;        // Return, KP_Enter
        case 0xff1b: return Key::escape;
        case 0xffff: case 0xff9f: return Key::del;          // Delete, KP_Delete
        case 0xff63: case 0xff9e: return Key::insert;
        case 0xff50: case 0xff95: return Key::home;
        case 0xff57: case 0xff9c: return Key::end;
        case 0xff51: case 0xff96: return Key::left;
        case 0xff52: case 0xff97: return Key::up;
        case 0xff53: case 0xff98: return Key::right;
        case 0xff54: case 0xff99: return Key::down;
        case 0xff55: case 0xff9a: return Key::page_up;
        case 0xff56: case 0xff9b: return Key::page_down;
        case 0xffe1: case 0xffe2: return Key::shift;
        case 0xffe3: case 0xffe4: return Key::ctrl;
        case 0xffe9: case 0xffea: return Key::alt;
        case 0xffeb: case 0xffec: return Key::super;
        case 0xffe5: return Key::caps_lock;
        default: break;
    }
    if (sym >= 0xffbe && sym <= 0xffc9) return static_cast<Key>(static_cast<int>(Key::f1) + static_cast<int>(sym - 0xffbe));
    return Key::unknown;
}

/// The Key for evdev code `code` whose keymap gives keysym `sym` (0: no map).
///
/// The keymap is the truth, as in GTK and Qt: a key is the keysym it
/// produces -- named keys (caps:escape, a remapped Enter) and Latin
/// characters alike, so ctrl+z is the key LABELLED Z on AZERTY and Dvorak,
/// and a virtual keyboard (wtype, an IME, a remote desktop), whose keymap
/// puts characters on arbitrary codes, types what it says. Physical position
/// is the answer only without a keymap, or for a non-Latin layout's letters
/// (Cyrillic, Greek...), so ctrl+c still copies there.
[[nodiscard]] inline Key resolve_key(std::uint32_t code, std::uint32_t sym) noexcept {
    const Key pos = key_from_evdev(code);
    if (sym == 0) return pos;
    if (const Key named = key_from_keysym(sym); named != Key::unknown) return named;
    // Latin keysyms are their ASCII code points: the shared character rule
    return layout_key(pos, sym);
}

/// mayag cursor -> wp_cursor_shape_device_v1 shape.
[[nodiscard]] inline std::uint32_t cursor_to_shape(CursorShape c) noexcept {
    switch (c) {
        case CursorShape::text:        return wl::cursor_text;
        case CursorShape::pointer:     return wl::cursor_pointer;
        case CursorShape::crosshair:   return wl::cursor_crosshair;
        case CursorShape::resize_h:    return wl::cursor_ew_resize;
        case CursorShape::resize_v:    return wl::cursor_ns_resize;
        case CursorShape::resize_nwse: return wl::cursor_nwse_resize;
        case CursorShape::resize_nesw: return wl::cursor_nesw_resize;
        case CursorShape::grab:        return wl::cursor_grab;
        case CursorShape::grabbing:    return wl::cursor_grabbing;
        case CursorShape::not_allowed: return wl::cursor_not_allowed;
        case CursorShape::wait:        return wl::cursor_wait;
        case CursorShape::arrow:
        default:                       return wl::cursor_default;
    }
}

}  // namespace wldetail


// ════════════════════════════════════════════════════════════════════════
// WaylandWindow
// ════════════════════════════════════════════════════════════════════════

class WaylandWindow {
  private:
    /// Everything a listener can touch, pinned to one heap address.
    ///
    /// libwayland stores the `void* data` given to add_listener and hands it
    /// back on every event, forever. If that pointer names a member of a
    /// WaylandWindow that later MOVES — and it does move exactly once, out of
    /// `open()`'s local and into the caller's storage — every subsequent
    /// event writes through a dangling pointer. That is not theoretical: it
    /// segfaults on the first keyboard focus event, inside a vector push_back
    /// whose `this` is garbage.
    ///
    /// Making the state a separate heap allocation fixes it by construction:
    /// moving the window moves a unique_ptr, and the address the compositor
    /// knows about never changes. It also means `open()` can register
    /// listeners BEFORE the window is returned, which is required — the
    /// roundtrips that discover the globals have to run with listeners live.
    struct Impl {
    [[nodiscard]] bool boot(const WindowConfig& cfg) {
        const wl::Lib& L = wl::lib();
        if (!L.ok) return false;                 // no libwayland: fall back

#ifdef MAYAG_WITH_VULKAN
        // Driver init (instance + device) takes tens of ms and needs no
        // window, so it runs on a worker while we talk to the compositor.
        const char* forced_backend = ::getenv("MAYAG_BACKEND");
        const bool allow_gpu = forced_backend == nullptr ||
                               std::string_view{forced_backend} == "vulkan" ||
                               std::string_view{forced_backend} == "gpu";
        std::optional<jaal::kernel::pool>& prep_pool = prep_pool_;
        auto& prep = prep_;
        if (allow_gpu) {
            prep_pool.emplace(1);
            prep = prep_pool->submit([](std::stop_token, std::unique_ptr<backend::VulkanDevice> d) {
                if (!d->prepare()) d.reset();
                return d;
            }, std::move(gpu_));
        }
#endif

        display_ = L.display_connect(nullptr);
        if (display_ == nullptr) return false;            // no compositor

        cfg_    = cfg;
        logical_ = cfg.size;

        // ── registry ────────────────────────────────────────────────────
        registry_ = wl::request_new(display_, wl::display_get_registry,
                                      L.i_registry, 1u);
        if (registry_ == nullptr) return false;
        wl::add_listener(registry_, &registry_listener, this);

        // Two roundtrips: the first delivers the global list, the second the
        // events emitted by the objects bound during the first (seat
        // capabilities, shm formats, output scale).
        L.display_roundtrip(display_);
        L.display_roundtrip(display_);
        startup::mark("wayland connected (registry)");

        if (compositor_ == nullptr || shm_ == nullptr || wm_base_ == nullptr) {
            // A compositor with no xdg-shell is not one we can put a window on.
            return false;
        }

        // ── surface + xdg role ──────────────────────────────────────────
        surface_ = wl::request_new(compositor_, wl::compositor_create_surface,
                                     L.i_surface, wl::version_of(compositor_));
        if (surface_ == nullptr) return false;
        wl::add_listener(surface_, &surface_listener, this);

        xdg_surface_ = wl::request_new(wm_base_, wl::wm_base_get_xdg_surface,
                                         &wl::xdg().surface,
                                         wl::version_of(wm_base_),
                                         surface_);
        if (xdg_surface_ == nullptr) return false;
        wl::add_listener(xdg_surface_, &xdg_surface_listener, this);

        toplevel_ = wl::request_new(xdg_surface_, wl::xdg_surface_get_toplevel,
                                      &wl::xdg().toplevel,
                                      wl::version_of(xdg_surface_));
        if (toplevel_ == nullptr) return false;
        wl::add_listener(toplevel_, &toplevel_listener, this);

        wl::request(toplevel_, wl::toplevel_set_title, cfg.title.c_str());
        wl::request(toplevel_, wl::toplevel_set_app_id, "mayag");
        if (!cfg.resizable) {
            const auto iw = static_cast<std::int32_t>(cfg.size.x);
            const auto ih = static_cast<std::int32_t>(cfg.size.y);
            wl::request(toplevel_, wl::toplevel_set_min_size, iw, ih);
            wl::request(toplevel_, wl::toplevel_set_max_size, iw, ih);
        }

        // Server-side decorations: ask, and take what we are given. A
        // compositor that refuses (or lacks the protocol) leaves the window
        // undecorated, which is correct for a tiling WM and is what the user
        // of such a WM expects anyway.
        if (deco_manager_ != nullptr && cfg.decorated) {
            decoration_ = wl::request_new(deco_manager_,
                                            wl::deco_manager_get_toplevel,
                                            &wl::xdg().deco,
                                            wl::version_of(deco_manager_),
                                            toplevel_);
            if (decoration_ != nullptr) {
                wl::request(decoration_, wl::deco_set_mode,
                            wl::deco_mode_server_side);
            }
        }

        // Fractional scale: the compositor reports the exact scale it wants,
        // in 120ths. Combined with the viewport below this renders at native
        // device resolution on a 1.25x or 1.5x display instead of rounding to
        // an integer and resampling.
        if (frac_manager_ != nullptr) {
            frac_scale_ = wl::request_new(frac_manager_,
                                            wl::frac_manager_get_scale,
                                            &wl::xdg().frac,
                                            wl::version_of(frac_manager_),
                                            surface_);
            if (frac_scale_ != nullptr) {
                wl::add_listener(frac_scale_, &frac_listener, this);
            }
        }
        if (viewporter_ != nullptr) {
            viewport_ = wl::request_new(viewporter_, wl::viewporter_get_viewport,
                                          &wl::xdg().viewport,
                                          wl::version_of(viewporter_),
                                          surface_);
        }
        // vsync off: the lowest input-to-photon the display allows. Ask the
        // compositor to flip our buffers the moment they arrive (tearing,
        // presentation hint 1 = async) instead of at the next vblank, and the
        // swapchain for IMMEDIATE. Compositors honour it per policy
        // (Hyprland: general:allow_tearing + an `immediate` window rule).
        if (!cfg.vsync && tearing_mgr_ != nullptr) {
            tearing_ = wl::request_new(tearing_mgr_, wl::tearing_mgr_get_control, &wl::xdg().tearing,
                                       wl::version_of(tearing_mgr_), surface_);
            if (tearing_ != nullptr) wl::request(tearing_, wl::tearing_control_set_hint, 1u);
        }

        // Commit with no buffer, then wait: the xdg protocol requires the
        // first configure to arrive before the first buffer is attached.
        wl::request(surface_, wl::surface_commit);
        L.display_roundtrip(display_);
        startup::mark("wayland surface configured");

        // The compositor may not have sized us; fall back to the requested size.
        if (logical_.x < 1.0f || logical_.y < 1.0f) logical_ = cfg.size;

        open_ = true;
        start_time_ = wldetail::monotonic_now();

#ifdef MAYAG_WITH_VULKAN
        // Bring up the GPU. This is the default renderer on Linux: a Vulkan
        // swapchain rendering straight into the wl_surface. If it fails — no
        // libvulkan, no device, a headless VM, MAYAG_BACKEND=software — the
        // shm surfaces below are created and the CPU path takes over. The
        // choice is silent by design; the runtime logs the winner once.
        if (allow_gpu) {
            // GPU first, from frame one: the device was prepared on a worker
            // while the compositor handshake ran, and the first frame waits
            // for it (up to MAYAG_GPU_WAIT_MS, default 3000 -- a cold Vulkan
            // start on a hybrid laptop can take seconds, and a frame on the
            // wrong renderer followed by a switch is worse than waiting).
            // Only if it misses the budget do the first frames go out on the
            // CPU and the window switch when it is ready. MAYAG_GPU_WAIT_MS=0
            // restores CPU-first.
            int budget = 3000;
            if (const char* e = ::getenv("MAYAG_GPU_WAIT_MS"); e != nullptr && *e != '\0') budget = std::max(0, std::atoi(e));
            if (const char* async = ::getenv("MAYAG_GPU_ASYNC"); async != nullptr && std::string_view{async} == "0") budget = 1 << 30;
            if (budget > 0 && !prep.ready()) (void)prep.wait_for_at_root(std::chrono::milliseconds(budget));
            if (prep.ready()) adopt_gpu(prep.get_at_root());
            else gpu_pending_ = true;
        }
        if (!gpu_) gpu_ = std::make_unique<backend::VulkanDevice>();
        startup::mark(gpu_pending_ ? "vulkan pending (missed the wait budget: cpu first)" : "vulkan ready (first frame)");
#endif

        // The shm buffers (a float framebuffer plus three encoded copies,
        // ~80 MB at 2x) are only needed when the CPU path presents, so with
        // the GPU up they're made on first use instead.
        if (!gpu_on()) resize_surfaces();
        return true;
    }


    void close() {
        open_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // ── geometry ────────────────────────────────────────────────────────

    [[nodiscard]] Vec2  size() const noexcept { return logical_; }
    [[nodiscard]] float dpi_scale() const noexcept { return scale_; }

    /// The size, IN PIXELS, of the buffer `read_pixels()` returns.
    ///
    /// Not simply `size() * dpi_scale()`: the compositor owns the surface
    /// dimensions and can resize it between frames, so the logical size and
    /// the swapchain extent drift apart. A caller that assumes the product is
    /// correct will reinterpret the rows at the wrong stride and see a
    /// perfectly convincing diagonal-stripe "rendering bug" that does not
    /// exist. Ask the window instead of computing it.
    [[nodiscard]] Vec2 pixel_size() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        if (gpu_active_) {
            const auto e = gpu_->swapchain_extent();
            if (e.x > 0.0f && e.y > 0.0f) return e;
        }
#endif
        return {logical_.x * scale_, logical_.y * scale_};
    }

    // ── events ──────────────────────────────────────────────────────────

    /// The display fd. jaal's reactor waits on it; dispatch() reads it.
    [[nodiscard]] int fd() const noexcept {
        return (open_ && display_ != nullptr) ? wl::lib().display_get_fd(display_) : -1;
    }

    /// Read whatever the compositor sent, without blocking, and return it as
    /// mayag events. jaal calls this when fd() is readable; it also runs once
    /// at attach so events queued while opening aren't stranded.
    ///
    /// prepare_read/read_events is libwayland's guard against the "check
    /// then read" race: announcing the read first means no event slips
    /// between draining the queue and reading the socket.
    [[nodiscard]] std::vector<Event> dispatch() {
        const wl::Lib& L = wl::lib();
        pending_.clear();
        if (!open_ || display_ == nullptr) return {};

        while (L.display_prepare_read(display_) != 0) {
            L.display_dispatch_pending(display_);
        }
        L.display_flush(display_);

        // Zero timeout: only read what is already there. jaal did the waiting.
        pollfd pfd{L.display_get_fd(display_), POLLIN, 0};
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
            L.display_read_events(display_);
        } else {
            L.display_cancel_read(display_);
        }

        L.display_dispatch_pending(display_);
        // Client-side key repeat: the held key's repeats due by now.
        pump_repeat();
        // Requests the listeners made (acks, cursor) go out now, not at the
        // next frame.
        L.display_flush(display_);

        // Start the input-to-photon clock at the first user event not yet on
        // screen (unless the program marks inputs itself).
        for (const auto& e : pending_) {
            if (photon_manual_) break;
            if (starts_photon_clock(e)) { note_input(); break; }
        }

        // A protocol error is fatal and unrecoverable — the compositor has
        // already dropped the connection. Closing cleanly beats spinning on a
        // dead fd forever.
        if (L.display_get_error(display_) != 0) {
            open_ = false;
        }

        if (resize_needed_) {
            if (!gpu_on() || shm_made_) resize_surfaces();   // shm buffers + geometry
#ifdef MAYAG_WITH_VULKAN
            if (gpu_active_) {
                gpu_->resize(logical_, scale_);
                apply_surface_geometry();   // the swapchain's frames map to the NEW size
            }
#endif
            resize_needed_ = false;
            pending_.push_back(ResizeEvent{logical_, scale_});
        }

        return std::move(pending_);
    }

    // ── presentation ────────────────────────────────────────────────────

    /// Render and hand the frame to the compositor.
    void present(const DrawList& dl, Color<Srgb> clear) {
        if (!open_ || surface_ == nullptr) return;

#ifdef MAYAG_WITH_VULKAN
        if (gpu_pending_ && prep_.ready()) {
            adopt_gpu(prep_.get_at_root());
            gpu_pending_ = false;
            prep_pool_.reset();
            startup::mark("vulkan took over");
        }
        // GPU path: the Vulkan swapchain owns the wl_surface's buffers, so
        // there is no shm buffer, no CPU encode, and no damage bookkeeping
        // here — the driver presents directly. This is the default on Linux;
        // it is only inactive when the GPU could not be brought up, in which
        // case gpu_->valid() is false and control falls through to shm.
        if (gpu_active_) {
            if (atlas_ != nullptr) sync_atlas_to_gpu();
            // Same pixels as what's on screen: don't wake the GPU and the
            // compositor for it (a hover or model change that didn't change
            // the picture). Texture and atlas uploads count as changes.
            if (last_had_frame_ && clear == last_clear_ && dl.same_as(last_list_) && !gpu_->needs_present()) {
                ++frames_skipped_;
                return;
            }
            request_feedback();   // before the commit vkQueuePresentKHR makes
            gpu_->submit(dl, clear);
            last_list_.assign_from(dl); last_clear_ = clear; last_had_frame_ = true;
            // Still ask for a frame callback, same as the shm path.
            if (!frame_pending_) {
                frame_callback_ = wl::request_new(surface_, wl::surface_frame,
                                                  wl::lib().i_callback,
                                                  wl::version_of(surface_));
                if (frame_callback_ != nullptr) {
                    wl::add_listener(frame_callback_, &frame_listener, this);
                    frame_pending_ = true;
                }
            }
            wl::lib().display_flush(display_);
            ++frames_presented_;
            return;
        }
#endif

        if (!shm_made_) resize_surfaces();   // first CPU present (GPU lost or off)
        wldetail::ShmBuffer* buf = acquire_buffer();
        if (buf == nullptr) {
            // Every buffer is still held by the compositor. Dropping is the
            // correct response — the alternative is writing into a buffer
            // being scanned out, which tears — but it is also a signal: a
            // caller that hits this constantly is presenting faster than the
            // display can consume, and should be pacing on frame callbacks.
            ++frames_dropped_;
            return;
        }

        // Render with the tiled parallel rasteriser, then encode straight into
        // the compositor's pages. No intermediate vector, no copy.
        backend::Tiled::render(dl, fb_, sampler_, &backend::shared_pool(), clear);

        // ── damage ──────────────────────────────────────────────────────
        //
        // The union of what the draw list touched, in buffer pixels. A caret
        // blink damages a few hundred pixels instead of the whole window,
        // which is the difference between the compositor re-uploading a 14 MB
        // texture and a 1 KB one.
        //
        // The same rect also bounds the ENCODE. Buffers rotate, so a given
        // shm buffer was last written some frames ago and is only valid
        // outside the region that has changed since THEN — the union of this
        // frame's damage and everything damaged while this buffer sat idle.
        // That is buffer-age tracking, and without it a partial encode would
        // leave stale pixels from two frames back.
        const Rect d = damage_of(dl);

        Rect encode_rect = d;
        for (auto& b : buffers_) {
            b.pending_damage = wldetail::union_rect(b.pending_damage, d);
        }
        encode_rect = buf->pending_damage;
        buf->pending_damage = Rect{};

        const float area_full = static_cast<float>(pixel_w_) *
                                static_cast<float>(pixel_h_);
        const bool partial = damage_supported_ && !first_frame_ &&
                             !encode_rect.empty() &&
                             encode_rect.width() * encode_rect.height() <
                                 0.9f * area_full;

        if (partial) encode_into(buf->pixels, encode_rect);
        else         encode_into(buf->pixels, Rect{0.0f, 0.0f,
                                                   static_cast<float>(pixel_w_),
                                                   static_cast<float>(pixel_h_)});

        wl::request(surface_, wl::surface_attach, buf->buffer, 0, 0);

        if (damage_supported_ && !first_frame_ && !d.empty()) {
            wl::request(surface_, wl::surface_damage_buffer,
                        static_cast<std::int32_t>(d.left()),
                        static_cast<std::int32_t>(d.top()),
                        static_cast<std::int32_t>(d.width()),
                        static_cast<std::int32_t>(d.height()));
        } else {
            wl::request(surface_, wl::surface_damage_buffer, 0, 0,
                        0x7FFFFFFF, 0x7FFFFFFF);
        }
        first_frame_ = false;

        // Ask to be told when the compositor wants the next frame. This is the
        // vsync source: no timer, no sleep, no frame rendered that will be
        // thrown away.
        if (!frame_pending_) {
            frame_callback_ = wl::request_new(surface_, wl::surface_frame,
                                              wl::lib().i_callback,
                                              wl::version_of(surface_));
            if (frame_callback_ != nullptr) {
                wl::add_listener(frame_callback_, &frame_listener, this);
                frame_pending_ = true;
            }
        }

        buf->busy = true;
        request_feedback();
        wl::request(surface_, wl::surface_commit);
        wl::lib().display_flush(display_);
        ++frames_presented_;
    }

    // ── services ────────────────────────────────────────────────────────

    [[nodiscard]] double now() const noexcept {
        return wldetail::monotonic_now() - start_time_;
    }

    /// Wayland has no protocol for this; GTK and Qt both default to 400 ms.
    /// Wayland has no protocol for it, so read what the desktop says: the
    /// MAYAG_DOUBLE_CLICK_MS override, else GNOME's / GTK's
    /// org.gnome.desktop.peripherals.mouse double-click (via the
    /// gtk-3.0/4.0 settings.ini gtk-double-click-time), else 500 ms -- the
    /// common desktop default (400 ms reads a relaxed double click as two
    /// singles, which users report as "double click doesn't work").
    [[nodiscard]] double double_click_interval() const noexcept {
        const auto ms_to_s = [](long ms) { return ms >= 150 && ms <= 2000 ? static_cast<double>(ms) / 1000.0 : 0.0; };
        if (const char* v = std::getenv("MAYAG_DOUBLE_CLICK_MS"))
            if (const double s = ms_to_s(std::strtol(v, nullptr, 10)); s > 0.0) return s;
        const char* home = std::getenv("HOME");
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        const std::string base = xdg && *xdg ? std::string{xdg} : (home ? std::string{home} + "/.config" : std::string{});
        for (const char* f : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
            if (base.empty()) break;
            if (std::FILE* fp = std::fopen((base + f).c_str(), "r")) {
                char line[256];
                double found = 0.0;
                while (std::fgets(line, sizeof line, fp))
                    if (std::strncmp(line, "gtk-double-click-time", 21) == 0)
                        if (const char* eq = std::strchr(line, '=')) found = ms_to_s(std::strtol(eq + 1, nullptr, 10));
                std::fclose(fp);
                if (found > 0.0) return found;
            }
        }
        return 0.5;
    }

    void set_title(std::string_view t) {
        if (toplevel_ == nullptr) return;
        title_.assign(t);
        wl::request(toplevel_, wl::toplevel_set_title, title_.c_str());
    }

    void set_fullscreen(bool on) {
        if (toplevel_ == nullptr) return;
        // Let the COMPOSITOR pick the output by passing a null one. Naming an
        // output means guessing which monitor the window is on, and guessing
        // wrong moves it to another screen -- the compositor already knows.
        //
        // No size is set here on purpose: the compositor answers with a
        // configure carrying the fullscreen geometry, which arrives through
        // the normal resize path. Setting a size locally would fight it.
        if (on) wl::request(toplevel_, wl::toplevel_set_fullscreen, nullptr);
        else    wl::request(toplevel_, wl::toplevel_unset_fullscreen);
    }

    void set_cursor(CursorShape shape) {
        if (shape == cursor_ && cursor_mode_ == CursorMode::shape) return;
        cursor_ = shape;
        cursor_mode_ = CursorMode::shape;
        apply_cursor();
    }

    /// Interactive move from the press in progress (xdg_toplevel.move needs
    /// that press's serial; the compositor takes the drag from here).
    void begin_move() {
        if (toplevel_ != nullptr && seat_ != nullptr && pointer_serial_ != 0)
            wl::request(toplevel_, wl::toplevel_move, seat_, pointer_serial_);
    }
    void minimize() { if (toplevel_ != nullptr) wl::request(toplevel_, wl::toplevel_set_minimized); }
    void toggle_maximize() {
        if (toplevel_ == nullptr) return;
        if (maximized_) wl::request(toplevel_, wl::toplevel_unset_maximized);
        else wl::request(toplevel_, wl::toplevel_set_maximized);
        maximized_ = !maximized_;
    }

    /// Clipboard.
    ///
    /// Wayland's data-device model is asynchronous and requires a serial from
    /// a real input event to take the selection, plus a pipe and a source
    /// object that must outlive the request. mayag keeps a local copy so that
    /// within-app copy/paste is always correct and instant; cross-app
    /// selection ownership is offered when a seat serial is available.
    void set_clipboard(std::string_view text) {
        clipboard_.assign(text);
    }

    [[nodiscard]] std::string get_clipboard() { return clipboard_; }

    /// Read the current frame back as RGBA8, for screenshots and golden tests.
    [[nodiscard]] std::vector<std::uint8_t> read_pixels() {
#ifdef MAYAG_WITH_VULKAN
        if (gpu_active_ && last_had_frame_) {
            std::vector<std::uint8_t> out;

            // FIRST CHOICE: copy the image that was actually presented.
            //
            // This used to go straight to the re-render path below, which is a
            // different pipeline -- different image format, different render
            // pass, no swapchain, no compositor. It therefore could not observe
            // any bug living in presentation itself, which is precisely the
            // case where a window shows something other than what the renderer
            // produced. A screenshot facility that cannot see that class of bug
            // is worse than none, because it actively reassures you.
            if (gpu_->read_swapchain(out)) return out;

            // FALLBACK: re-render the last draw list offscreen. Used when
            // nothing has been presented yet, or the driver refused the
            // readback. Still useful, but it shows what SHOULD have been
            // presented rather than what was.
            const int w = static_cast<int>(logical_.x * scale_ + 0.5f);
            const int h = static_cast<int>(logical_.y * scale_ + 0.5f);
            if (readback_gpu_.valid() || readback_gpu_.init_offscreen()) {
                if (atlas_sync_) atlas_sync_(readback_gpu_);
                if (readback_gpu_.render_offscreen(last_list_, w, h, last_clear_, out)) {
                    return out;
                }
            }
        }
#endif
        if (!shm_made_) return {};
        return fb_.to_rgba8();
    }

    // ── runtime hooks ───────────────────────────────────────────────────

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        images_.set_glyphs(s);
        sampler_ = &images_;
    }

    /// Optional window capability: hand pixels to the renderer for image(id).
    ///
    /// Both paths are fed, because which one is live is decided at runtime:
    /// the GPU gets a sampled image, the shm fallback gets the CPU store.
    /// Uploading to both costs one copy of an image the app already built,
    /// and means a GPU that fails to initialise does not silently lose every
    /// picture in the app.
    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        if (!img) return;
#ifdef MAYAG_WITH_VULKAN
        if (gpu_active_) {
            (void)gpu_->upload_texture(id, img->rgba.data(), img->width, img->height);
        }
#endif
        images_.upload(id, std::move(img));
    }

    /// Rewrite rectangles of an uploaded image in place (see
    /// VulkanDevice::update_texture_regions). false: upload it whole.
    bool update_image_regions(std::uint32_t id, int w, int h, const std::vector<std::array<int, 4>>& rects,
                              const std::vector<std::uint8_t>& packed) {
#ifdef MAYAG_WITH_VULKAN
        if (!gpu_active_ || rects.empty()) return false;
        std::vector<backend::VulkanDevice::Region> r;
        r.reserve(rects.size());
        for (const auto& q : rects) r.push_back({q[0], q[1], q[2], q[3]});
        return gpu_->update_texture_regions(id, w, h, r.data(), r.size(), packed.data(), packed.size());
#else
        (void)id, (void)w, (void)h, (void)rects, (void)packed;
        return false;
#endif
    }

    void release_image(std::uint32_t id) {
        images_.release(id);
#ifdef MAYAG_WITH_VULKAN
        if (gpu_active_) gpu_->release_texture(id);
#endif
    }

#ifdef MAYAG_WITH_VULKAN
    /// Optional window capability: show a FOREIGN GPU buffer as image(id),
    /// zero copies. The compositor path: a client's dmabuf, imported by the
    /// device, sampled where image(id) is drawn. `on_free` runs when the GPU
    /// has finished with the buffer this replaces (or this one, on failure).
    [[nodiscard]] ImportResult import_image(std::uint32_t id, const Dmabuf& d, int w, int h,
                                            std::function<void()> on_free) {
        if (!gpu_active_) {
            if (on_free) on_free();
            return std::unexpected(ImportError::not_supported);
        }
        images_.release(id);  // the CPU path cannot show it; don't show a stale one
        return gpu_->import_dmabuf_as(id, d, w, h, std::move(on_free));
    }
    [[nodiscard]] bool can_import_images() const noexcept { return gpu_active_ && gpu_->can_import(); }
    /// Free GPU resources whose last frame has finished, without waiting for
    /// the next frame (see VulkanDevice::poll_retired). True while some are
    /// still in flight, so the caller polls again soon.
    bool poll_gpu_retired() { return gpu_active_ && gpu_->poll_retired(); }
    // Explicit sync (see VulkanDevice): a client timeline, waited on and
    // signalled by the GPU in the frame that shows its buffer.
    [[nodiscard]] std::uint32_t import_timeline(int fd) { return gpu_active_ ? gpu_->import_timeline(fd) : 0; }
    void release_timeline(std::uint32_t t) { if (gpu_active_) gpu_->release_timeline(t); }
    void frame_wait(std::uint32_t t, std::uint64_t v) { if (gpu_active_) gpu_->frame_wait(t, v); }
    void frame_signal(std::uint32_t t, std::uint64_t v) { if (gpu_active_) gpu_->frame_signal(t, v); }
    void signal_after_submitted(std::uint32_t t, std::uint64_t v) { if (gpu_active_) gpu_->signal_after_submitted(t, v); }
    [[nodiscard]] bool explicit_sync() const noexcept { return gpu_active_ && gpu_->sync_caps().import_syncobj; }
    /// `f` once the frames already submitted have finished (see VulkanDevice).
    void after_gpu(std::function<void()> f) {
        if (gpu_active_) gpu_->after_gpu(std::move(f));
        else if (f) f();
    }
    [[nodiscard]] std::span<const FormatModifier> importable_formats() const {
        return gpu_active_ ? gpu_->importable_formats() : std::span<const FormatModifier>{};
    }
#endif

#ifdef MAYAG_WITH_VULKAN
    /// Upload dirty glyph rects to the GPU atlas image before a frame that
    /// uses them. Cheap in steady state — the atlas reports an empty dirty
    /// region and the upload is skipped.
    void sync_atlas_to_gpu() {
        if (atlas_sync_) atlas_sync_(*gpu_);
    }

    /// Attach a prepared device to the surface and make it the renderer. The
    /// atlas uploads in full on its first sync (a fresh device holds a 1x1
    /// placeholder); the bound image is re-uploaded here.
    void adopt_gpu(std::unique_ptr<backend::VulkanDevice> d) {
        if (d) { d->set_low_latency(!cfg_.vsync); d->set_transparent(cfg_.transparent); }
        if (!d || !d->attach_wayland(display_, surface_, logical_, scale_)) {
            if (d) d->destroy();
            if (!gpu_) gpu_ = std::make_unique<backend::VulkanDevice>();
            return;
        }
        gpu_ = std::move(d);
        gpu_active_ = true;
        apply_surface_geometry();   // the size may have changed since the CPU frames
        for (const auto& [id, img] : images_.all())
            (void)gpu_->upload_texture(id, img->rgba.data(), img->width, img->height);
        // The shm buffers are done with; give them back.
        if (shm_made_) {
            for (auto& b : buffers_) b.unmap();
            if (pool_ != nullptr) { wl::request_destroy(pool_, wl::shm_pool_destroy); pool_ = nullptr; }
            if (pool_fd_ >= 0) { ::close(pool_fd_); pool_fd_ = -1; }
            fb_ = backend::Framebuffer{1, 1};
            shm_made_ = false;
        }
    }
#endif

    [[nodiscard]] bool gpu_on() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        return gpu_active_;
#else
        return false;
#endif
    }

    /// The atlas source. On the shm path the atlas is read through the CPU
    /// coverage sampler, so nothing to upload; on the GPU path the atlas is a
    /// sampled image, so we remember how to sync it (type-erased, since the
    /// Atlas type is a template parameter of the FontStack).
    template <typename AtlasT>
    void set_atlas_source(AtlasT* atlas) noexcept {
#ifdef MAYAG_WITH_VULKAN
        if (atlas == nullptr) { atlas_ = nullptr; atlas_sync_ = nullptr; return; }
        atlas_ = atlas;
        atlas_sync_ = [atlas](backend::VulkanDevice& d) { d.sync_atlas(*atlas); };
#else
        (void)atlas;
#endif
    }

    [[nodiscard]] std::string_view renderer_name() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        if (gpu_active_) return "vulkan (wayland)";
        if (gpu_pending_) return "vulkan (wayland; first frames on cpu)";
#endif
        return "software (wayland/shm)";
    }
    [[nodiscard]] bool gpu_active() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        return gpu_active_;
#else
        return false;
#endif
    }
    [[nodiscard]] bool gpu_pending() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        return gpu_pending_;
#else
        return false;
#endif
    }
    [[nodiscard]] bool gpu_ready_to_adopt() const {
#ifdef MAYAG_WITH_VULKAN
        return gpu_pending_ && prep_.ready();
#else
        return false;
#endif
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept {
        return frames_presented_;
    }
    [[nodiscard]] std::uint64_t frames_skipped() const noexcept { return frames_skipped_; }
    [[nodiscard]] bool replaces_queued_frames() const noexcept { return gpu_ != nullptr && gpu_->valid() && gpu_->replaces_queued(); }

    /// Frames skipped because the compositor still held every buffer.
    /// Nonzero in a tight present loop; should be zero in a frame-callback
    /// paced app.
    [[nodiscard]] std::uint64_t frames_dropped() const noexcept {
        return frames_dropped_;
    }

    // ── buffer management ───────────────────────────────────────────────

    /// Pick a buffer the compositor is not currently reading.
    [[nodiscard]] wldetail::ShmBuffer* acquire_buffer() {
        for (auto& b : buffers_) {
            if (!b.busy && b.pixels != nullptr &&
                b.width == pixel_w_ && b.height == pixel_h_) {
                return &b;
            }
        }
        return nullptr;
    }

    /// (Re)allocate the framebuffer and the shm pool for the current size.
    void resize_surfaces() {
        const wl::Lib& L = wl::lib();
        shm_made_ = true;

        pixel_w_ = static_cast<int>(logical_.x * scale_ + 0.5f);
        pixel_h_ = static_cast<int>(logical_.y * scale_ + 0.5f);
        if (pixel_w_ < 1) pixel_w_ = 1;
        if (pixel_h_ < 1) pixel_h_ = 1;

        fb_ = backend::Framebuffer{pixel_w_, pixel_h_};

        for (auto& b : buffers_) b.unmap();
        if (pool_ != nullptr) {
            wl::request_destroy(pool_, wl::shm_pool_destroy);
            pool_ = nullptr;
        }
        if (pool_fd_ >= 0) { ::close(pool_fd_); pool_fd_ = -1; }

        const std::size_t stride = static_cast<std::size_t>(pixel_w_) * 4u;
        const std::size_t one    = stride * static_cast<std::size_t>(pixel_h_);
        const std::size_t total  = one * buffer_count;

        pool_fd_ = wldetail::make_shm_fd(total);
        if (pool_fd_ < 0) return;

        void* map = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED,
                           pool_fd_, 0);
        if (map == MAP_FAILED) {
            ::close(pool_fd_);
            pool_fd_ = -1;
            return;
        }
        pool_base_  = static_cast<std::uint8_t*>(map);
        pool_bytes_ = total;

        pool_ = wl::request_new(shm_, wl::shm_create_pool, L.i_shm_pool,
                                wl::version_of(shm_), pool_fd_,
                                static_cast<std::int32_t>(total));
        if (pool_ == nullptr) return;

        // XRGB when the window is opaque: the compositor can then skip
        // blending the surface entirely, which is a real win on a
        // battery-powered machine.
        const std::uint32_t fmt = cfg_.transparent ? wl::shm_format_argb8888
                                                   : wl::shm_format_xrgb8888;

        for (std::size_t i = 0; i < buffer_count; ++i) {
            auto& b = buffers_[i];
            b.pixels = pool_base_ + one * i;
            b.bytes  = one;
            b.width  = pixel_w_;
            b.height = pixel_h_;
            b.busy   = false;
            // A fresh buffer is stale everywhere until it is first encoded.
            b.pending_damage = Rect{0.0f, 0.0f, static_cast<float>(pixel_w_),
                                    static_cast<float>(pixel_h_)};
            b.buffer = wl::request_new(pool_, wl::shm_pool_create_buffer,
                                       L.i_buffer, wl::version_of(pool_),
                                       static_cast<std::int32_t>(one * i),
                                       static_cast<std::int32_t>(pixel_w_),
                                       static_cast<std::int32_t>(pixel_h_),
                                       static_cast<std::int32_t>(stride), fmt);
            if (b.buffer != nullptr) wl::add_listener(b.buffer, &buffer_listener, &b);
        }
        // The mapping is owned by the pool now; the fd may be closed but is
        // kept for resize().

        // Tell the compositor how the buffer maps to the surface.
        apply_surface_geometry();

        first_frame_ = true;
        damage_supported_ = wl::version_of(surface_) >= 4;
    }

    /// How the buffer maps onto the surface (viewport destination or buffer
    /// scale) and the opaque region: the ONE place either is set, called on
    /// every size change by whichever path draws. It used to live only in the
    /// shm path, so once Vulkan owned the surface a resize kept the old
    /// viewport destination and the compositor scaled every new frame into
    /// the old, smaller box (the app shrunk into a corner, wallpaper around).
    void apply_surface_geometry() {
        const wl::Lib& L = wl::lib();
        if (surface_ == nullptr) return;
        if (viewport_ != nullptr) {
            wl::request(viewport_, wl::viewport_set_destination,
                        static_cast<std::int32_t>(logical_.x),
                        static_cast<std::int32_t>(logical_.y));
        } else if (wl::version_of(surface_) >= 3) {
            wl::request(surface_, wl::surface_set_buffer_scale,
                        static_cast<std::int32_t>(scale_ + 0.5f));
        }

        // An opaque window lets the compositor skip blending it.
        if (!cfg_.transparent && compositor_ != nullptr) {
            wl::proxy* region = wl::request_new(compositor_,
                                                wl::compositor_create_region,
                                                L.i_region,
                                                wl::version_of(compositor_));
            if (region != nullptr) {
                wl::request(region, wl::region_add, 0, 0,
                            static_cast<std::int32_t>(logical_.x),
                            static_cast<std::int32_t>(logical_.y));
                wl::request(surface_, wl::surface_set_opaque_region, region);
                wl::request_destroy(region, wl::region_destroy);
            }
        }
    }

    /// Encode the linear framebuffer into the compositor's pages as BGRA8.
    ///
    /// This is the hot loop of the whole backend, so it does the swizzle and
    /// the sRGB encode in ONE pass, in parallel, writing final bytes to their
    /// final address. `encode_span` produces RGBA; Wayland's XRGB8888 is BGRA
    /// in memory, so the two colour channels are swapped as they are written
    /// rather than in a second pass.
    ///
    /// `region` bounds the work. Only rows the frame actually changed are
    /// touched, so a caret blink costs a few rows of a few hundred pixels
    /// instead of a full-frame transfer.
    void encode_into(std::uint8_t* dst, const Rect& region) {
        const int w = fb_.width();
        const int h = fb_.height();
        if (w <= 0 || h <= 0) return;

        int x0 = static_cast<int>(region.left());
        int y0 = static_cast<int>(region.top());
        int x1 = static_cast<int>(region.right()  + 0.999f);
        int y1 = static_cast<int>(region.bottom() + 0.999f);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        if (x1 <= x0 || y1 <= y0) return;

        const int span = x1 - x0;
        const int rows = y1 - y0;

        auto& pool = backend::shared_pool();
        const int rows_per_job = 32;
        const int jobs = (rows + rows_per_job - 1) / rows_per_job;

        pool.parallel_for(static_cast<std::size_t>(jobs), [&](std::size_t j) {
            const int ry0 = y0 + static_cast<int>(j) * rows_per_job;
            const int ry1 = (ry0 + rows_per_job < y1) ? ry0 + rows_per_job : y1;
            for (int y = ry0; y < ry1; ++y) {
                const Vec4*   src = &fb_.at(x0, y);
                std::uint8_t* out = dst + (static_cast<std::size_t>(y) * w + x0) * 4;
                encode_row_bgra(src, out, static_cast<std::size_t>(span));
            }
        });
    }

    /// One row: un-premultiply, sRGB-encode, store BGRA.
    static void encode_row_bgra(const Vec4* src, std::uint8_t* dst,
                                std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            const Vec4& p = src[i];
            const float a = p.w >= 1.0f ? 1.0f : (p.w <= 0.0f ? 0.0f : p.w);
            if (a >= 1.0f) {
                dst[0] = backend::detail::srgb_encode(p.z);   // B
                dst[1] = backend::detail::srgb_encode(p.y);   // G
                dst[2] = backend::detail::srgb_encode(p.x);   // R
                dst[3] = 255;
            } else if (a <= 0.0f) {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            } else {
                const float inv = 1.0f / a;
                dst[0] = backend::detail::srgb_encode(p.z * inv);
                dst[1] = backend::detail::srgb_encode(p.y * inv);
                dst[2] = backend::detail::srgb_encode(p.x * inv);
                dst[3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
            }
            dst += 4;
        }
    }

    /// Union of every batch's clip rect, in buffer pixels.
    [[nodiscard]] Rect damage_of(const DrawList& dl) const {
        const Rect screen{0.0f, 0.0f, static_cast<float>(pixel_w_),
                          static_cast<float>(pixel_h_)};
        if (dl.batches().empty()) return screen;

        bool  any = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (const auto& b : dl.batches()) {
            const Rect c = b.clip;
            // An unbounded clip means "everything"; no point unioning further.
            if (!(c.size.x < 1e8f && c.size.y < 1e8f)) return screen;
            const float bx0 = c.left(), by0 = c.top();
            const float bx1 = c.right(), by1 = c.bottom();
            if (!any) { x0 = bx0; y0 = by0; x1 = bx1; y1 = by1; any = true; }
            else {
                if (bx0 < x0) x0 = bx0;
                if (by0 < y0) y0 = by0;
                if (bx1 > x1) x1 = bx1;
                if (by1 > y1) y1 = by1;
            }
        }
        if (!any) return screen;

        // Clip rects are in LOGICAL coordinates; damage_buffer wants buffer
        // pixels. Snap outward by a pixel so an antialiased edge is never
        // half-updated.
        const float s = scale_;
        Rect r{x0 * s - 1.0f, y0 * s - 1.0f,
               (x1 - x0) * s + 2.0f, (y1 - y0) * s + 2.0f};
        return r.intersect(screen);
    }

    void apply_cursor() {
        if (pointer_ == nullptr || pointer_serial_ == 0) return;
        switch (cursor_mode_) {
            case CursorMode::image:
                if (cursor_surface_ != nullptr) {
                    wl::request(pointer_, wl::pointer_set_cursor, pointer_serial_, cursor_surface_, cursor_hot_x_,
                                cursor_hot_y_);
                    return;
                }
                break;
            case CursorMode::hidden:
                wl::request(pointer_, wl::pointer_set_cursor, pointer_serial_, static_cast<wl::proxy*>(nullptr), 0, 0);
                return;
            case CursorMode::raw_shape:
                if (cursor_device_ != nullptr) {
                    wl::request(cursor_device_, wl::cursor_dev_set_shape, pointer_serial_, cursor_raw_shape_);
                    return;
                }
                break;
            case CursorMode::shape: break;
        }
        if (cursor_device_ == nullptr) return;
        wl::request(cursor_device_, wl::cursor_dev_set_shape, pointer_serial_,
                    wldetail::cursor_to_shape(cursor_));
    }

    /// A wp_cursor_shape_v1 shape by number: the full set (34 shapes), for a
    /// program passing on what another client asked for.
    void set_cursor_shape_raw(std::uint32_t shape) {
        cursor_mode_ = CursorMode::raw_shape;
        cursor_raw_shape_ = shape;
        apply_cursor();
        if (display_ != nullptr) wl::lib().display_flush(display_);
    }

    void hide_cursor() {
        cursor_mode_ = CursorMode::hidden;
        apply_cursor();
        if (display_ != nullptr) wl::lib().display_flush(display_);
    }

    /// A cursor IMAGE: premultiplied ARGB8888 rows (wl_shm's format), drawn
    /// by the host compositor on its cursor plane, so it moves with zero
    /// added latency. Copied once into a buffer of mayag's own.
    void set_cursor_image(const std::uint8_t* argb, int w, int h, int stride, int hot_x, int hot_y) {
        const wl::Lib& L = wl::lib();
        if (argb == nullptr || w <= 0 || h <= 0 || w > 256 || h > 256 || compositor_ == nullptr || shm_ == nullptr)
            return;
        if (cursor_surface_ == nullptr)
            cursor_surface_ = wl::request_new(compositor_, wl::compositor_create_surface, L.i_surface,
                                              wl::version_of(compositor_));
        if (cursor_surface_ == nullptr) return;
        const std::size_t row = static_cast<std::size_t>(w) * 4, bytes = row * static_cast<std::size_t>(h);
        const int fd = wldetail::make_shm_fd(bytes);
        if (fd < 0) return;
        void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { ::close(fd); return; }
        for (int y = 0; y < h; ++y)
            std::memcpy(static_cast<std::uint8_t*>(map) + row * static_cast<std::size_t>(y),
                        argb + static_cast<std::size_t>(stride) * static_cast<std::size_t>(y), row);
        ::munmap(map, bytes);
        wl::proxy* pool = wl::request_new(shm_, wl::shm_create_pool, L.i_shm_pool, wl::version_of(shm_), fd,
                                          static_cast<std::int32_t>(bytes));
        ::close(fd);
        if (pool == nullptr) return;
        wl::proxy* buf = wl::request_new(pool, wl::shm_pool_create_buffer, L.i_buffer, wl::version_of(pool), 0, w, h,
                                         static_cast<std::int32_t>(row), wl::shm_format_argb8888);
        wl::request_destroy(pool, wl::shm_pool_destroy);
        if (buf == nullptr) return;
        wl::request(cursor_surface_, wl::surface_attach, buf, 0, 0);
        wl::request(cursor_surface_, wl::surface_damage_buffer, 0, 0, w, h);
        wl::request(cursor_surface_, wl::surface_commit);
        // The previous image's buffer: replaced by this commit, so done with.
        if (cursor_buffer_ != nullptr) wl::request_destroy(cursor_buffer_, wl::buffer_destroy);
        cursor_buffer_ = buf;
        cursor_hot_x_ = hot_x;
        cursor_hot_y_ = hot_y;
        cursor_mode_ = CursorMode::image;
        apply_cursor();
        if (display_ != nullptr) L.display_flush(display_);
    }

    /// The raw input tap (see raw::). Unset by default. The keyboard's
    /// keymap and repeat rate usually arrive before anyone installs a tap,
    /// so they are replayed to it at once.
    void set_raw_input(std::function<void(const RawInput&)> f) {
        raw_ = std::move(f);
        if (!raw_) return;
        if (!keymap_text_.empty()) raw_(raw::Keymap{keymap_text_});
        if (keyboard_ != nullptr) raw_(raw::Repeat{repeat_.rate, repeat_.delay});
    }


    // ════════════════════════════════════════════════════════════════════
    // Listeners
    //
    // Every one is a plain C function pointer with the exact arity libwayland
    // will call it with. A mismatch here is a stack corruption, not a
    // compile error, which is why each mirrors its .xml signature literally.
    // ════════════════════════════════════════════════════════════════════

    static Impl* self(void* d) noexcept {
        return static_cast<Impl*>(d);
    }

    // ── registry ────────────────────────────────────────────────────────

    static void on_global(void* data, wl::proxy* reg, std::uint32_t name,
                          const char* iface, std::uint32_t version) {
        Impl* w = self(data);
        const wl::Lib& L = wl::lib();
        const std::string_view id{iface};

        auto bind = [&](const wl::wl_interface* target, std::uint32_t want) {
            return wl::registry_bind_to(reg, name, target,
                                        version < want ? version : want);
        };

        if (id == "wl_compositor") {
            w->compositor_ = bind(L.i_compositor, 4);
        } else if (id == "wl_shm") {
            w->shm_ = bind(L.i_shm, 1);
        } else if (id == "xdg_wm_base") {
            w->wm_base_ = bind(&wl::xdg().wm_base, 3);
            if (w->wm_base_ != nullptr) {
                wl::add_listener(w->wm_base_, &wm_base_listener, w);
            }
        } else if (id == "wl_seat") {
            // v8: high-resolution wheel (axis_value120), never more than the
            // libwayland we loaded knows how to decode.
            const auto seat_max = static_cast<std::uint32_t>(L.i_seat->version);
            w->seat_ = bind(L.i_seat, seat_max < 8 ? seat_max : 8);
            if (w->seat_ != nullptr) wl::add_listener(w->seat_, &seat_listener, w);
        } else if (id == "wl_output") {
            if (w->output_ == nullptr) {
                w->output_ = bind(L.i_output, 2);
                if (w->output_ != nullptr) {
                    wl::add_listener(w->output_, &output_listener, w);
                }
            }
        } else if (id == "zxdg_decoration_manager_v1") {
            w->deco_manager_ = bind(&wl::xdg().deco_manager, 1);
        } else if (id == "wp_fractional_scale_manager_v1") {
            w->frac_manager_ = bind(&wl::xdg().frac_manager, 1);
        } else if (id == "wp_viewporter") {
            w->viewporter_ = bind(&wl::xdg().viewporter, 1);
        } else if (id == "wp_presentation") {
            w->presentation_ = bind(&wl::xdg().presentation, 1);
            if (w->presentation_ != nullptr)
                wl::add_listener(w->presentation_, &presentation_listener, w);
        } else if (id == "wp_cursor_shape_manager_v1") {
            w->cursor_manager_ = bind(&wl::xdg().cursor_mgr, 1);
        } else if (id == "wp_tearing_control_manager_v1") {
            w->tearing_mgr_ = bind(&wl::xdg().tearing_mgr, 1);
        } else if (id == "zwp_text_input_manager_v3") {
            w->text_input_mgr_ = bind(&wl::xdg().text_input_mgr, 1);
            w->make_text_input();
        }
    }

    static void on_global_remove(void*, wl::proxy*, std::uint32_t) {}

    static inline const struct {
        void (*global)(void*, wl::proxy*, std::uint32_t, const char*, std::uint32_t);
        void (*global_remove)(void*, wl::proxy*, std::uint32_t);
    } registry_listener{&on_global, &on_global_remove};

    // ── xdg_wm_base: ping/pong keeps the window from being killed ───────

    static void on_ping(void* data, wl::proxy* base, std::uint32_t serial) {
        (void)data;
        wl::request(base, wl::wm_base_pong, serial);
    }

    static inline const struct {
        void (*ping)(void*, wl::proxy*, std::uint32_t);
    } wm_base_listener{&on_ping};

    // ── xdg_surface ─────────────────────────────────────────────────────

    static void on_xdg_configure(void* data, wl::proxy* xdg_surf,
                                 std::uint32_t serial) {
        Impl* w = self(data);
        wl::request(xdg_surf, wl::xdg_surface_ack_configure, serial);
        w->configured_ = true;
    }

    static inline const struct {
        void (*configure)(void*, wl::proxy*, std::uint32_t);
    } xdg_surface_listener{&on_xdg_configure};

    // ── xdg_toplevel ────────────────────────────────────────────────────

    static void on_toplevel_configure(void* data, wl::proxy*, std::int32_t width,
                                      std::int32_t height, wl::wl_array* states) {
        Impl* w = self(data);
        (void)states;
        // 0x0 means "you choose" — keep the current size.
        if (width > 0 && height > 0) {
            const Vec2 next{static_cast<float>(width), static_cast<float>(height)};
            if (next.x != w->logical_.x || next.y != w->logical_.y) {
                w->logical_ = next;
                w->resize_needed_ = true;
            }
        }
    }

    static void on_toplevel_close(void* data, wl::proxy*) {
        Impl* w = self(data);
        w->pending_.push_back(CloseRequest{});
    }

    static void on_toplevel_bounds(void*, wl::proxy*, std::int32_t, std::int32_t) {}
    static void on_toplevel_caps(void*, wl::proxy*, wl::wl_array*) {}

    static inline const struct {
        void (*configure)(void*, wl::proxy*, std::int32_t, std::int32_t, wl::wl_array*);
        void (*close)(void*, wl::proxy*);
        void (*configure_bounds)(void*, wl::proxy*, std::int32_t, std::int32_t);
        void (*wm_capabilities)(void*, wl::proxy*, wl::wl_array*);
    } toplevel_listener{&on_toplevel_configure, &on_toplevel_close,
                        &on_toplevel_bounds, &on_toplevel_caps};

    // ── wl_surface: which output are we on ──────────────────────────────

    static void on_surface_enter(void*, wl::proxy*, wl::proxy*) {}
    static void on_surface_leave(void*, wl::proxy*, wl::proxy*) {}
    static void on_surface_pref_scale(void* data, wl::proxy*, std::int32_t factor) {
        Impl* w = self(data);
        // Only honour the integer scale when there is no fractional protocol;
        // otherwise the fractional value is strictly better.
        if (w->frac_scale_ != nullptr || factor < 1) return;
        const float s = static_cast<float>(factor);
        if (s != w->scale_) { w->scale_ = s; w->resize_needed_ = true; }
    }
    static void on_surface_pref_transform(void*, wl::proxy*, std::uint32_t) {}

    static inline const struct {
        void (*enter)(void*, wl::proxy*, wl::proxy*);
        void (*leave)(void*, wl::proxy*, wl::proxy*);
        void (*preferred_buffer_scale)(void*, wl::proxy*, std::int32_t);
        void (*preferred_buffer_transform)(void*, wl::proxy*, std::uint32_t);
    } surface_listener{&on_surface_enter, &on_surface_leave,
                       &on_surface_pref_scale, &on_surface_pref_transform};

    // ── fractional scale ────────────────────────────────────────────────

    static void on_preferred_scale(void* data, wl::proxy*, std::uint32_t scale_120) {
        Impl* w = self(data);
        const float s = static_cast<float>(scale_120) / 120.0f;
        if (s > 0.0f && s != w->scale_) {
            w->scale_ = s;
            w->resize_needed_ = true;
        }
    }

    static inline const struct {
        void (*preferred_scale)(void*, wl::proxy*, std::uint32_t);
    } frac_listener{&on_preferred_scale};

    // ── wl_output ───────────────────────────────────────────────────────

    static void on_output_geometry(void*, wl::proxy*, std::int32_t, std::int32_t,
                                   std::int32_t, std::int32_t, std::int32_t,
                                   const char*, const char*, std::int32_t) {}
    static void on_output_mode(void* data, wl::proxy*, std::uint32_t flags,
                               std::int32_t, std::int32_t, std::int32_t refresh) {
        Impl* w = self(data);
        // `current` flag == 1. Refresh is in mHz.
        if ((flags & 1u) != 0 && refresh > 0) {
            w->refresh_hz_ = static_cast<double>(refresh) / 1000.0;
        }
    }
    static void on_output_done(void*, wl::proxy*) {}
    static void on_output_scale(void* data, wl::proxy*, std::int32_t factor) {
        Impl* w = self(data);
        if (w->frac_scale_ != nullptr || factor < 1) return;
        const float s = static_cast<float>(factor);
        if (w->scale_ == 1.0f && s > 1.0f) { w->scale_ = s; w->resize_needed_ = true; }
    }
    static void on_output_name(void*, wl::proxy*, const char*) {}
    static void on_output_description(void*, wl::proxy*, const char*) {}

    static inline const struct {
        void (*geometry)(void*, wl::proxy*, std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::int32_t, const char*, const char*,
                         std::int32_t);
        void (*mode)(void*, wl::proxy*, std::uint32_t, std::int32_t, std::int32_t,
                     std::int32_t);
        void (*done)(void*, wl::proxy*);
        void (*scale)(void*, wl::proxy*, std::int32_t);
        void (*name)(void*, wl::proxy*, const char*);
        void (*description)(void*, wl::proxy*, const char*);
    } output_listener{&on_output_geometry, &on_output_mode, &on_output_done,
                      &on_output_scale, &on_output_name, &on_output_description};

    // ── frame callback ──────────────────────────────────────────────────

    // ── wp_presentation: when frames really reach the display ───────────
    //
    // A feedback object is asked for before each commit. The compositor
    // answers `presented` with the exact scanout time and the refresh
    // period, or `discarded` if the frame never made it. That gives the host
    // a real vsync clock, and turns "input to photon" into a measurement.

    struct Feedback {
        Impl*      w;
        wl::proxy* proxy;
        PhotonMeter::Frame frame;   // the input it carries, when it was committed
    };

    void request_feedback() {
        if (presentation_ == nullptr || surface_ == nullptr) return;
        wl::proxy* p = wl::request_new_after(presentation_, wl::presentation_feedback,
                                             &wl::xdg().pres_feedback, wl::version_of(presentation_),
                                             surface_);
        if (p == nullptr) return;
        // Owned by the window: a feedback still pending at teardown is freed
        // there (the compositor will never answer a destroyed proxy), so no
        // listener can outlive the window it points at.
        // Every frame committed while an input is unshown carries it; the
        // FIRST one presented shows it. (Under MAILBOX a frame is discarded
        // when a newer one replaces it at the same vblank -- the newer one
        // shows the input too, and its feedback is the one that answers.)
        auto fb = std::make_unique<Feedback>(Feedback{this, p, photon_.take(wldetail::monotonic_now())});
        wl::add_listener(p, &feedback_listener, fb.get());
        pending_feedback_.push_back(std::move(fb));
    }

    static void on_fb_sync_output(void*, wl::proxy*, wl::proxy*) {}
    static void on_fb_presented(void* data, wl::proxy*, std::uint32_t tv_sec_hi,
                                std::uint32_t tv_sec_lo, std::uint32_t tv_nsec,
                                std::uint32_t refresh_ns, std::uint32_t, std::uint32_t,
                                std::uint32_t pres_flags) {
        auto* fb = static_cast<Feedback*>(data);
        Impl* w = fb->w;
        const double secs = static_cast<double>((std::uint64_t{tv_sec_hi} << 32) | tv_sec_lo) +
                            static_cast<double>(tv_nsec) * 1e-9;
        // CLOCK_MONOTONIC is the default presentation clock on Linux; a
        // compositor using another one says so in clock_id, which we read.
        if (w->pres_clock_monotonic_) {
            w->vblank_s_ = secs;
            ++w->vblank_seq_;
            if (refresh_ns > 0) w->period_ns_ = refresh_ns;
            const char flags[5] = {(pres_flags & 1u) ? 'v' : '-', (pres_flags & 2u) ? 'c' : '-',
                                   (pres_flags & 4u) ? 'h' : '-', (pres_flags & 8u) ? 'z' : '-', '\0'};
            w->photon_.presented(fb->frame, secs, static_cast<double>(w->period_ns_) / 1e6, flags);
        }
        w->retire_feedback(fb);
    }
    static void on_fb_discarded(void* data, wl::proxy*) {
        auto* fb = static_cast<Feedback*>(data);
        // Replaced before scanout: the input it carried is still unshown, and
        // the frames after it carry it (see request_feedback).
        fb->w->retire_feedback(fb);
    }
    /// A feedback answered: destroy its proxy and free it.
    void retire_feedback(Feedback* fb) {
        wl::destroy(fb->proxy);
        std::erase_if(pending_feedback_, [fb](const std::unique_ptr<Feedback>& p) { return p.get() == fb; });
    }
    static inline const struct {
        void (*sync_output)(void*, wl::proxy*, wl::proxy*);
        void (*presented)(void*, wl::proxy*, std::uint32_t, std::uint32_t, std::uint32_t,
                          std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
        void (*discarded)(void*, wl::proxy*);
    } feedback_listener{&on_fb_sync_output, &on_fb_presented, &on_fb_discarded};

    static void on_pres_clock(void* data, wl::proxy*, std::uint32_t clk) {
        self(data)->pres_clock_monotonic_ = (clk == CLOCK_MONOTONIC);
    }
    static inline const struct {
        void (*clock_id)(void*, wl::proxy*, std::uint32_t);
    } presentation_listener{&on_pres_clock};

    /// An input event arrived. A frame shows the NEWEST input it was drawn
    /// with (pointer moves coalesce), so latency is measured from that: from
    /// the event's own timestamp when the compositor gave one (it's the
    /// kernel's, on CLOCK_MONOTONIC in ms), else from when we read it.
    void note_input() {
        const double now = wldetail::monotonic_now();
        double t = now;
        if (last_event_ms_ != 0) {
            // Both in wrapping 32-bit ms; the unsigned difference is the age.
            const auto now_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now * 1000.0));
            const std::uint32_t age = now_ms - last_event_ms_;
            if (age < 1000) t = now - static_cast<double>(age) / 1000.0;
            last_event_ms_ = 0;
        }
        photon_.note(t, now);
    }
    void note_event_time(std::uint32_t ms) noexcept { if (ms != 0) last_event_ms_ = ms; }

    // ── input to photon, for a program that forwards input ───────────────
    //
    // A nested compositor or remote desktop does not draw the answer to its
    // window's input; a client somewhere else does, later. Its latency runs
    // from the input to the frame that first shows that client's answer.
    // In manual mode the window's own events no longer start the clock;
    // the program marks the input whose answer the next frame will show.

    void set_photon_manual(bool on) noexcept { photon_manual_ = on; }
    /// The next frame drawn shows the answer to input from `at` (monotonic
    /// seconds). The oldest unshown mark is kept: the honest latency.
    void mark_input(double at) noexcept { photon_.mark(at, wldetail::monotonic_now()); }
    /// An input event's wl timestamp (ms, CLOCK_MONOTONIC, wrapping) as
    /// monotonic seconds; `now` if it is implausible (synthetic events).
    [[nodiscard]] double event_seconds(std::uint32_t ms) const noexcept {
        const double now = wldetail::monotonic_now();
        const auto now_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now * 1000.0));
        const std::uint32_t age = now_ms - ms;
        return age < 1000 ? now - static_cast<double>(age) / 1000.0 : now;
    }
    /// Every measured sample, as it is measured (ms).
    void set_photon_sink(std::function<void(double)> f) { photon_.set_sink(std::move(f)); }

    [[nodiscard]] std::optional<DisplayTiming> display_timing() const noexcept {
        if (vblank_s_ <= 0.0 || period_ns_ == 0) return std::nullopt;
        using namespace std::chrono;
        // steady_clock is CLOCK_MONOTONIC on Linux (libstdc++ and libc++),
        // so the compositor's timestamp maps across exactly.
        const auto vb = steady_clock::time_point{
            duration_cast<steady_clock::duration>(duration<double>(vblank_s_))};
        return DisplayTiming{vb, nanoseconds{period_ns_}, vblank_seq_};
    }
    [[nodiscard]] PhotonStats photon_stats() const noexcept { return photon_.stats(); }

    static void on_frame_done(void* data, wl::proxy* cb, std::uint32_t) {
        Impl* w = self(data);
        wl::destroy(cb);
        if (w->frame_callback_ == cb) w->frame_callback_ = nullptr;
        w->frame_pending_ = false;
    }

    static inline const struct {
        void (*done)(void*, wl::proxy*, std::uint32_t);
    } frame_listener{&on_frame_done};

    // ── buffer release ──────────────────────────────────────────────────

    static void on_buffer_release(void* data, wl::proxy*) {
        static_cast<wldetail::ShmBuffer*>(data)->busy = false;
    }

    static inline const struct {
        void (*release)(void*, wl::proxy*);
    } buffer_listener{&on_buffer_release};

    // ── seat ────────────────────────────────────────────────────────────

    static void on_seat_caps(void* data, wl::proxy* seat, std::uint32_t caps) {
        Impl* w = self(data);
        const wl::Lib& L = wl::lib();

        if ((caps & wl::seat_cap_pointer) != 0 && w->pointer_ == nullptr) {
            w->pointer_ = wl::request_new(seat, wl::seat_get_pointer, L.i_pointer,
                                          wl::version_of(seat));
            if (w->pointer_ != nullptr) {
                wl::add_listener(w->pointer_, &pointer_listener, w);
                if (w->cursor_manager_ != nullptr) {
                    w->cursor_device_ = wl::request_new(
                        w->cursor_manager_, wl::cursor_mgr_get_pointer,
                        &wl::xdg().cursor_dev,
                        wl::version_of(w->cursor_manager_), w->pointer_);
                }
            }
        }
        if ((caps & wl::seat_cap_keyboard) != 0 && w->keyboard_ == nullptr) {
            w->keyboard_ = wl::request_new(seat, wl::seat_get_keyboard,
                                           L.i_keyboard, wl::version_of(seat));
            if (w->keyboard_ != nullptr) {
                wl::add_listener(w->keyboard_, &keyboard_listener, w);
            }
            w->make_text_input();
        }
    }

    static void on_seat_name(void*, wl::proxy*, const char*) {}

    static inline const struct {
        void (*capabilities)(void*, wl::proxy*, std::uint32_t);
        void (*name)(void*, wl::proxy*, const char*);
    } seat_listener{&on_seat_caps, &on_seat_name};

    // ── pointer ─────────────────────────────────────────────────────────

    static void on_ptr_enter(void* data, wl::proxy*, std::uint32_t serial,
                             wl::proxy*, wl::fixed_t sx, wl::fixed_t sy) {
        Impl* w = self(data);
        w->pointer_serial_ = serial;
        if (w->raw_) {
            w->raw_(raw::PointerFocus{true});
            w->raw_(raw::Motion{0, sx, sy});
        }
        w->pointer_pos_ = Vec2{static_cast<float>(wl::fixed_to_double(sx)),
                               static_cast<float>(wl::fixed_to_double(sy))};
        // The cursor is undefined on enter and MUST be set, or it vanishes.
        w->apply_cursor();
        w->pending_.push_back(MouseMove{w->pointer_pos_, Vec2{}, w->mods_});
    }

    static void on_ptr_leave(void* data, wl::proxy*, std::uint32_t serial,
                             wl::proxy*) {
        Impl* w = self(data);
        w->pointer_serial_ = serial;
        if (w->raw_) w->raw_(raw::PointerFocus{false});
        // Park the pointer off-surface so hover state clears.
        w->pending_.push_back(MouseMove{Vec2{-1.0f, -1.0f}, Vec2{}, w->mods_});
    }

    static void on_ptr_motion(void* data, wl::proxy*, std::uint32_t time_ms,
                              wl::fixed_t sx, wl::fixed_t sy) {
        Impl* w = self(data);
        w->note_event_time(time_ms);
        if (w->raw_) w->raw_(raw::Motion{time_ms, sx, sy});
        const Vec2 p{static_cast<float>(wl::fixed_to_double(sx)),
                     static_cast<float>(wl::fixed_to_double(sy))};
        const Vec2 delta{p.x - w->pointer_pos_.x, p.y - w->pointer_pos_.y};
        w->pointer_pos_ = p;
        w->pending_.push_back(MouseMove{p, delta, w->mods_});
    }

    static void on_ptr_button(void* data, wl::proxy*, std::uint32_t serial,
                              std::uint32_t time_ms, std::uint32_t button,
                              std::uint32_t state) {
        Impl* w = self(data);
        w->pointer_serial_ = serial;
        w->note_event_time(time_ms);   // the photon clock starts at the press / release
        if (w->raw_) w->raw_(raw::Button{time_ms, button, state == 1});

        // evdev button codes.
        MouseButton b = MouseButton::left;
        switch (button) {
            case 0x110: b = MouseButton::left;   break;   // BTN_LEFT
            case 0x111: b = MouseButton::right;  break;   // BTN_RIGHT
            case 0x112: b = MouseButton::middle; break;   // BTN_MIDDLE
            default: return;
        }

        if (state == wl::pointer_button_pressed) {
            // Click count 0 means UNKNOWN, not one.
            //
            // Wayland has no multi-click protocol -- wl_pointer.button says
            // only "a button went down" -- so the interaction layer has to
            // derive the count from timing and distance. It only does that
            // when the platform reports nothing, and a hardcoded 1 is a
            // platform reporting "definitely a single click": synthesis never
            // ran and a double-click was impossible on this backend. macOS
            // sends a real clickCount from NSEvent, which is why it worked
            // there and only there.
            // The compositor's timestamp: a double click is timed from when
            // the user pressed, not from when a (possibly busy) frame got
            // round to handling the event.
            w->pending_.push_back(MouseDown{w->pointer_pos_, b, w->mods_, 0, static_cast<double>(time_ms) / 1000.0});
        } else {
            w->pending_.push_back(MouseUp{w->pointer_pos_, b, w->mods_});
        }
    }

    static void on_ptr_axis(void* data, wl::proxy*, std::uint32_t time_ms,
                            std::uint32_t axis, wl::fixed_t value) {
        Impl* w = self(data);
        w->note_event_time(time_ms);   // ...and at the wheel notch
        if (w->raw_) w->raw_(raw::Axis{time_ms, axis, value});
        // Wayland's axis is "down/right is positive", and its magnitude is in
        // surface units. mayag scrolls content, so the sign is inverted here
        // once rather than in every scroll view.
        const float v = -static_cast<float>(wl::fixed_to_double(value));
        Vec2 d{};
        if (axis == wl::axis_vertical) d.y = v; else d.x = v;
        w->scroll_accum_.x += d.x;
        w->scroll_accum_.y += d.y;
    }

    static void on_ptr_frame(void* data, wl::proxy*) {
        Impl* w = self(data);
        if (w->raw_) w->raw_(raw::Frame{});
        // Scroll is delivered as one-or-more axis events terminated by a
        // frame. Coalescing here means a high-resolution trackpad produces one
        // mayag event per physical gesture step, not three.
        if (w->scroll_accum_.x != 0.0f || w->scroll_accum_.y != 0.0f) {
            w->pending_.push_back(ScrollEvent{w->scroll_accum_, w->pointer_pos_,
                                              w->mods_, false});
            w->scroll_accum_ = Vec2{};
        }
    }

    static void on_ptr_axis_source(void* data, wl::proxy*, std::uint32_t source) {
        if (self(data)->raw_) self(data)->raw_(raw::AxisSource{source});
    }
    static void on_ptr_axis_stop(void* data, wl::proxy*, std::uint32_t time_ms, std::uint32_t axis) {
        if (self(data)->raw_) self(data)->raw_(raw::AxisStop{time_ms, axis});
    }
    static void on_ptr_axis_discrete(void*, wl::proxy*, std::uint32_t, std::int32_t) {}
    static void on_ptr_axis_value120(void* data, wl::proxy*, std::uint32_t axis,
                                     std::int32_t v120) {
        Impl* w = self(data);
        if (w->raw_) w->raw_(raw::AxisValue120{axis, v120});
        // High-resolution wheel: 120 units per detent. Prefer this over the
        // legacy axis event when both arrive, by replacing rather than adding.
        const float lines = -static_cast<float>(v120) / 120.0f;
        const float px = lines * 53.0f;   // one detent ~= three text lines
        if (axis == wl::axis_vertical) w->scroll_accum_.y = px;
        else                           w->scroll_accum_.x = px;
    }
    static void on_ptr_axis_dir(void*, wl::proxy*, std::uint32_t, std::uint32_t) {}

    static inline const struct {
        void (*enter)(void*, wl::proxy*, std::uint32_t, wl::proxy*, wl::fixed_t,
                      wl::fixed_t);
        void (*leave)(void*, wl::proxy*, std::uint32_t, wl::proxy*);
        void (*motion)(void*, wl::proxy*, std::uint32_t, wl::fixed_t, wl::fixed_t);
        void (*button)(void*, wl::proxy*, std::uint32_t, std::uint32_t,
                       std::uint32_t, std::uint32_t);
        void (*axis)(void*, wl::proxy*, std::uint32_t, std::uint32_t, wl::fixed_t);
        void (*frame)(void*, wl::proxy*);
        void (*axis_source)(void*, wl::proxy*, std::uint32_t);
        void (*axis_stop)(void*, wl::proxy*, std::uint32_t, std::uint32_t);
        void (*axis_discrete)(void*, wl::proxy*, std::uint32_t, std::int32_t);
        void (*axis_value120)(void*, wl::proxy*, std::uint32_t, std::int32_t);
        void (*axis_relative_direction)(void*, wl::proxy*, std::uint32_t,
                                        std::uint32_t);
    } pointer_listener{&on_ptr_enter, &on_ptr_leave, &on_ptr_motion, &on_ptr_button,
                       &on_ptr_axis, &on_ptr_frame, &on_ptr_axis_source,
                       &on_ptr_axis_stop, &on_ptr_axis_discrete,
                       &on_ptr_axis_value120, &on_ptr_axis_dir};

    // ── keyboard ────────────────────────────────────────────────────────

    static void on_kb_keymap(void* data, wl::proxy*, std::uint32_t format,
                             std::int32_t fd, std::uint32_t size) {
        Impl* w = self(data);
        // A new keymap changes what the held key means (layout switch,
        // virtual keyboards): never repeat an old key under a new map.
        w->repeat_.cancel();
        w->load_keymap(format, fd, size);
    }

    static void on_kb_enter(void* data, wl::proxy*, std::uint32_t, wl::proxy*,
                            wl::wl_array*) {
        self(data)->repeat_.cancel();
        if (self(data)->raw_) self(data)->raw_(raw::KeyboardFocus{true});
        self(data)->pending_.push_back(FocusEvent{true});
    }

    static void on_kb_leave(void* data, wl::proxy*, std::uint32_t, wl::proxy*) {
        Impl* w = self(data);
        if (w->raw_) w->raw_(raw::KeyboardFocus{false});
        w->mods_ = Mods{};
        w->repeat_.cancel();
        if (w->compose_state_ != nullptr && w->xkb_.compose_reset != nullptr) w->xkb_.compose_reset(w->compose_state_);
        w->pending_.push_back(FocusEvent{false});
    }

    static void on_kb_key(void* data, wl::proxy*, std::uint32_t serial,
                          std::uint32_t time_ms, std::uint32_t key, std::uint32_t state) {
        Impl* w = self(data);
        w->key_serial_ = serial;
        if (state == wl::keyboard_key_pressed) w->note_event_time(time_ms);   // the photon clock starts at the press
        if (w->raw_) w->raw_(raw::Key{time_ms, key, state == wl::keyboard_key_pressed, w->resolve_key(key), w->mods_});

        // mayag's Event variant models key PRESSES only — a release carries no
        // information the app model uses, and inventing a KeyUp alternative
        // here would be a platform backend widening the public event type.
        // A release does stop that key's repeat.
        if (state != wl::keyboard_key_pressed) { w->repeat_.release(key); return; }

        w->emit_key(key, false);
        const bool repeats = w->xkb_keymap_ != nullptr && w->xkb_.key_repeats != nullptr
            ? w->xkb_.key_repeats(w->xkb_keymap_, key + 8u) != 0
            : !wldetail::is_modifier_evdev(key);
        w->repeat_.press(key, repeats, wldetail::monotonic_now());
    }

    /// One key press (or a client-side repeat of one): the KeyEvent, then
    /// its text through Compose.
    /// The mayag Key for an evdev code: positional, unless the keymap names
    /// a key mayag knows (so a remapped layout's Backspace is Backspace).
    [[nodiscard]] Key resolve_key(std::uint32_t key) const {
        return wldetail::resolve_key(key, xkb_state_ != nullptr && xkb_.key_get_one_sym != nullptr
                                              ? xkb_.key_get_one_sym(xkb_state_, key + 8u) : 0u);
    }

    void emit_key(std::uint32_t key, bool repeat) {
        // evdev codes are offset by 8 from the X11 keycodes xkbcommon wants.
        const std::uint32_t xkb_code = key + 8u;
        const Key k = resolve_key(key);
        pending_.push_back(KeyEvent{k, mods_, repeat});

        // Text. With a keymap this is the layout-correct character (AltGr
        // included); without one it is nothing, which is correct — guessing
        // ASCII from a keycode is wrong on every non-US layout.
        if (xkb_state_ == nullptr || xkb_.key_get_utf8 == nullptr) return;
        if (mods_.ctrl || mods_.super) return;
        char buf[64];
        int n = 0;

        // Compose (dead keys, the Compose key): feed the keysym; while a
        // sequence is in progress nothing is typed, a completed one types
        // its result, a cancelled one types nothing (as GTK/Qt do).
        // XKB_COMPOSE_FEED_ACCEPTED = 1; statuses: NOTHING 0, COMPOSING 1,
        // COMPOSED 2, CANCELLED 3.
        if (compose_state_ != nullptr && xkb_.key_get_one_sym != nullptr && xkb_.compose_feed != nullptr &&
            xkb_.compose_status != nullptr) {
            const std::uint32_t sym = xkb_.key_get_one_sym(xkb_state_, xkb_code);
            if (xkb_.compose_feed(compose_state_, sym) == 1) {
                const int st = xkb_.compose_status(compose_state_);
                if (st == 1) return;
                if (st == 3) { xkb_.compose_reset(compose_state_); return; }
                if (st == 2) {
                    n = xkb_.compose_utf8 != nullptr ? xkb_.compose_utf8(compose_state_, buf, sizeof(buf)) : 0;
                    xkb_.compose_reset(compose_state_);
                    if (n > 0) pending_.push_back(TextEvent{std::string{buf, static_cast<std::size_t>(n)}});
                    return;
                }
            }
        }
        n = xkb_.key_get_utf8(xkb_state_, xkb_code, buf, sizeof(buf));
        if (n > 0) {
            const std::string_view s{buf, static_cast<std::size_t>(n)};
            // Filter control characters; Enter/Tab/Backspace are KeyDown.
            if (s.size() > 1 || static_cast<unsigned char>(s[0]) >= 0x20) {
                pending_.push_back(TextEvent{std::string{s}});
            }
        }
    }

    static void on_kb_mods(void* data, wl::proxy*, std::uint32_t,
                           std::uint32_t depressed, std::uint32_t latched,
                           std::uint32_t locked, std::uint32_t group) {
        Impl* w = self(data);
        if (w->raw_) w->raw_(raw::Modifiers{depressed, latched, locked, group});
        if (w->xkb_state_ != nullptr && w->xkb_.update_mask != nullptr) {
            w->xkb_.update_mask(w->xkb_state_, depressed, latched, locked,
                                0, 0, group);
            if (w->xkb_.mod_active != nullptr) {
                auto on = [&](const char* n) {
                    // 1 == XKB_STATE_MODS_EFFECTIVE
                    return w->xkb_.mod_active(w->xkb_state_, n, 1u) > 0;
                };
                w->mods_.shift = on("Shift");
                w->mods_.ctrl  = on("Control");
                w->mods_.alt   = on("Mod1");
                w->mods_.super = on("Mod4");
                return;
            }
        }
        // No xkbcommon: decode the standard mod indices directly. This keeps
        // shortcuts working on a system without libxkbcommon installed.
        const std::uint32_t eff = depressed | latched | locked;
        w->mods_.shift = (eff & (1u << 0)) != 0;
        w->mods_.ctrl  = (eff & (1u << 2)) != 0;
        w->mods_.alt   = (eff & (1u << 3)) != 0;
        w->mods_.super = (eff & (1u << 6)) != 0;
    }

    static void on_kb_repeat(void* data, wl::proxy*, std::int32_t rate,
                             std::int32_t delay) {
        Impl* w = self(data);
        if (w->raw_) w->raw_(raw::Repeat{rate, delay});
        w->repeat_.rate  = rate;
        w->repeat_.delay = delay;
        if (rate <= 0) w->repeat_.cancel();
    }

    static inline const struct {
        void (*keymap)(void*, wl::proxy*, std::uint32_t, std::int32_t, std::uint32_t);
        void (*enter)(void*, wl::proxy*, std::uint32_t, wl::proxy*, wl::wl_array*);
        void (*leave)(void*, wl::proxy*, std::uint32_t, wl::proxy*);
        void (*key)(void*, wl::proxy*, std::uint32_t, std::uint32_t, std::uint32_t,
                    std::uint32_t);
        void (*modifiers)(void*, wl::proxy*, std::uint32_t, std::uint32_t,
                          std::uint32_t, std::uint32_t, std::uint32_t);
        void (*repeat_info)(void*, wl::proxy*, std::int32_t, std::int32_t);
    } keyboard_listener{&on_kb_keymap, &on_kb_enter, &on_kb_leave, &on_kb_key,
                        &on_kb_mods, &on_kb_repeat};

    // ── xkbcommon, optionally ───────────────────────────────────────────
    //
    // Loaded the same way as libwayland and for the same reason: text input
    // must be layout-correct, but a missing libxkbcommon should degrade to
    // "keys work, exotic text does not" rather than failing to start.

    struct Xkb {
        void* handle = nullptr;
        void* (*context_new)(int)                                        = nullptr;
        void* (*keymap_new_from_string)(void*, const char*, int, int)    = nullptr;
        void* (*state_new)(void*)                                        = nullptr;
        void  (*keymap_unref)(void*)                                     = nullptr;
        void  (*state_unref)(void*)                                      = nullptr;
        void  (*context_unref)(void*)                                    = nullptr;
        int   (*key_get_utf8)(void*, std::uint32_t, char*, std::size_t)  = nullptr;
        void  (*update_mask)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t, std::uint32_t) = nullptr;
        int   (*mod_active)(void*, const char*, std::uint32_t)           = nullptr;
        // Repeat and Compose (dead keys, the Compose key). All optional.
        int   (*key_repeats)(void*, std::uint32_t)                       = nullptr;
        std::uint32_t (*key_get_one_sym)(void*, std::uint32_t)           = nullptr;
        void* (*compose_table_new)(void*, const char*, int)              = nullptr;
        void  (*compose_table_unref)(void*)                              = nullptr;
        void* (*compose_state_new)(void*, int)                           = nullptr;
        void  (*compose_state_unref)(void*)                              = nullptr;
        int   (*compose_feed)(void*, std::uint32_t)                      = nullptr;
        void  (*compose_reset)(void*)                                    = nullptr;
        int   (*compose_status)(void*)                                   = nullptr;
        int   (*compose_utf8)(void*, char*, std::size_t)                 = nullptr;
        int   (*keysym_utf8)(std::uint32_t, char*, std::size_t)          = nullptr;
    };

    void load_keymap(std::uint32_t format, std::int32_t fd, std::uint32_t size) {
        // format 1 == XKB_V1 text keymap.
        if (format != 1u) { ::close(fd); return; }

        if (xkb_.handle == nullptr) {
            xkb_.handle = ::dlopen("libxkbcommon.so.0", RTLD_LAZY | RTLD_LOCAL);
            if (xkb_.handle != nullptr) {
                void* h = xkb_.handle;
                auto sym = [h](const char* n) { return ::dlsym(h, n); };
                xkb_.context_new = reinterpret_cast<decltype(xkb_.context_new)>(
                    sym("xkb_context_new"));
                xkb_.keymap_new_from_string =
                    reinterpret_cast<decltype(xkb_.keymap_new_from_string)>(
                        sym("xkb_keymap_new_from_string"));
                xkb_.state_new = reinterpret_cast<decltype(xkb_.state_new)>(
                    sym("xkb_state_new"));
                xkb_.keymap_unref = reinterpret_cast<decltype(xkb_.keymap_unref)>(
                    sym("xkb_keymap_unref"));
                xkb_.state_unref = reinterpret_cast<decltype(xkb_.state_unref)>(
                    sym("xkb_state_unref"));
                xkb_.context_unref = reinterpret_cast<decltype(xkb_.context_unref)>(
                    sym("xkb_context_unref"));
                xkb_.key_get_utf8 = reinterpret_cast<decltype(xkb_.key_get_utf8)>(
                    sym("xkb_state_key_get_utf8"));
                xkb_.update_mask = reinterpret_cast<decltype(xkb_.update_mask)>(
                    sym("xkb_state_update_mask"));
                xkb_.mod_active = reinterpret_cast<decltype(xkb_.mod_active)>(
                    sym("xkb_state_mod_name_is_active"));
                xkb_.key_repeats = reinterpret_cast<decltype(xkb_.key_repeats)>(sym("xkb_keymap_key_repeats"));
                xkb_.key_get_one_sym = reinterpret_cast<decltype(xkb_.key_get_one_sym)>(sym("xkb_state_key_get_one_sym"));
                xkb_.compose_table_new = reinterpret_cast<decltype(xkb_.compose_table_new)>(sym("xkb_compose_table_new_from_locale"));
                xkb_.compose_table_unref = reinterpret_cast<decltype(xkb_.compose_table_unref)>(sym("xkb_compose_table_unref"));
                xkb_.compose_state_new = reinterpret_cast<decltype(xkb_.compose_state_new)>(sym("xkb_compose_state_new"));
                xkb_.compose_state_unref = reinterpret_cast<decltype(xkb_.compose_state_unref)>(sym("xkb_compose_state_unref"));
                xkb_.compose_feed = reinterpret_cast<decltype(xkb_.compose_feed)>(sym("xkb_compose_state_feed"));
                xkb_.compose_reset = reinterpret_cast<decltype(xkb_.compose_reset)>(sym("xkb_compose_state_reset"));
                xkb_.compose_status = reinterpret_cast<decltype(xkb_.compose_status)>(sym("xkb_compose_state_get_status"));
                xkb_.compose_utf8 = reinterpret_cast<decltype(xkb_.compose_utf8)>(sym("xkb_compose_state_get_utf8"));
                xkb_.keysym_utf8 = reinterpret_cast<decltype(xkb_.keysym_utf8)>(sym("xkb_keysym_to_utf8"));
            }
        }

        if (xkb_.handle == nullptr || xkb_.context_new == nullptr ||
            xkb_.keymap_new_from_string == nullptr || xkb_.state_new == nullptr) {
            ::close(fd);
            return;
        }

        // The compositor sends the keymap as a read-only fd. MAP_PRIVATE
        // because a shared mapping of another client's keymap is not
        // guaranteed writable and we only ever read it.
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { ::close(fd); return; }
        if (size > 0) {
            keymap_text_.assign(static_cast<const char*>(map), ::strnlen(static_cast<const char*>(map), size));
            if (raw_) raw_(raw::Keymap{keymap_text_});
        }

        if (xkb_context_ == nullptr) xkb_context_ = xkb_.context_new(0);
        if (xkb_context_ != nullptr) {
            if (xkb_state_ != nullptr) { xkb_.state_unref(xkb_state_); xkb_state_ = nullptr; }
            if (xkb_keymap_ != nullptr) { xkb_.keymap_unref(xkb_keymap_); xkb_keymap_ = nullptr; }

            // 1 == XKB_KEYMAP_FORMAT_TEXT_V1
            xkb_keymap_ = xkb_.keymap_new_from_string(
                xkb_context_, static_cast<const char*>(map), 1, 0);
            if (xkb_keymap_ != nullptr) xkb_state_ = xkb_.state_new(xkb_keymap_);

            // Compose: dead keys (´ then e -> é) and the Compose key, from
            // the locale's table plus ~/.XCompose, exactly as GTK and Qt read
            // them. Built once; a keymap change keeps it.
            if (compose_state_ == nullptr && xkb_.compose_table_new != nullptr &&
                xkb_.compose_state_new != nullptr) {
                compose_table_ = xkb_.compose_table_new(xkb_context_, mayag::locale::ctype_name().c_str(), 0);
                if (compose_table_ != nullptr) compose_state_ = xkb_.compose_state_new(compose_table_, 0);
            }
        }

        ::munmap(map, size);
        ::close(fd);
    }

    // ── input methods: zwp_text_input_v3 ──────────────────────────────────
    //
    // fcitx5 and ibus reach Wayland clients through this protocol (GTK4, Qt6,
    // Chromium and Firefox all speak it). While enabled, the compositor
    // routes keys through the IME, which answers with preedit and commit
    // strings; mayag turns those into ComposeEvent / TextEvent, the event
    // model the headless backend already scripts. The caret rectangle tells
    // the IME where to put its candidate window.

    void make_text_input() {
        if (text_input_ != nullptr || text_input_mgr_ == nullptr || seat_ == nullptr) return;
        text_input_ = wl::request_new(text_input_mgr_, wl::text_input_mgr_get_text_input,
                                      &wl::xdg().text_input, wl::version_of(text_input_mgr_), seat_);
        if (text_input_ != nullptr) wl::add_listener(text_input_, &text_input_listener, this);
    }

    /// Send enable/disable and the caret rectangle when they changed. The
    /// protocol double-buffers everything until `commit`, and the IME
    /// counts commits to match its `done` serials.
    void sync_text_input() {
        if (text_input_ == nullptr || !ti_entered_) return;
        const bool want = ti_want_;
        bool dirty = false;
        if (want && !ti_enabled_) {
            wl::request(text_input_, wl::text_input_enable);
            // content type: normal hint, normal purpose
            wl::request(text_input_, wl::text_input_set_content_type, 0u, 0u);
            ti_enabled_ = true; dirty = true;
            ti_sent_rect_ = Rect{Vec2{-1, -1}, Vec2{}};
        } else if (!want && ti_enabled_) {
            wl::request(text_input_, wl::text_input_disable);
            ti_enabled_ = false; dirty = true;
            ti_.reset([&](Event e) { pending_.push_back(std::move(e)); });
        }
        if (ti_enabled_ && !(ti_rect_ == ti_sent_rect_)) {
            wl::request(text_input_, wl::text_input_set_cursor_rectangle,
                        static_cast<std::int32_t>(ti_rect_.origin.x), static_cast<std::int32_t>(ti_rect_.origin.y),
                        static_cast<std::int32_t>(std::max(ti_rect_.size.x, 1.0f)),
                        static_cast<std::int32_t>(std::max(ti_rect_.size.y, 1.0f)));
            ti_sent_rect_ = ti_rect_;
            dirty = true;
        }
        if (dirty) {
            wl::request(text_input_, wl::text_input_commit);
            ++ti_.serial;
            wl::lib().display_flush(display_);
        }
    }

    static void on_ti_enter(void* data, wl::proxy*, wl::proxy* surface) {
        Impl* w = self(data);
        if (surface != w->surface_) return;
        w->ti_entered_ = true;
        w->ti_enabled_ = false;
        w->sync_text_input();
    }
    static void on_ti_leave(void* data, wl::proxy*, wl::proxy*) {
        Impl* w = self(data);
        if (w->ti_enabled_ && w->text_input_ != nullptr) {
            wl::request(w->text_input_, wl::text_input_disable);
            wl::request(w->text_input_, wl::text_input_commit);
            ++w->ti_.serial;
        }
        w->ti_entered_ = false;
        w->ti_enabled_ = false;
        w->ti_.reset([&](Event e) { w->pending_.push_back(std::move(e)); });
    }
    static void on_ti_preedit(void* data, wl::proxy*, const char* text, std::int32_t b, std::int32_t e) {
        self(data)->ti_.on_preedit(text, b, e);
    }
    static void on_ti_commit(void* data, wl::proxy*, const char* text) {
        self(data)->ti_.on_commit(text);
    }
    static void on_ti_delete(void* data, wl::proxy*, std::uint32_t before, std::uint32_t after) {
        self(data)->ti_.on_delete(before, after);
    }
    static void on_ti_done(void* data, wl::proxy*, std::uint32_t) {
        Impl* w = self(data);
        // A stale serial still applies text (the spec allows it; dropping a
        // commit would lose what the user typed).
        w->ti_.on_done([&](Event e) { w->pending_.push_back(std::move(e)); });
    }

    static inline const struct {
        void (*enter)(void*, wl::proxy*, wl::proxy*);
        void (*leave)(void*, wl::proxy*, wl::proxy*);
        void (*preedit_string)(void*, wl::proxy*, const char*, std::int32_t, std::int32_t);
        void (*commit_string)(void*, wl::proxy*, const char*);
        void (*delete_surrounding_text)(void*, wl::proxy*, std::uint32_t, std::uint32_t);
        void (*done)(void*, wl::proxy*, std::uint32_t);
    } text_input_listener{&on_ti_enter, &on_ti_leave, &on_ti_preedit, &on_ti_commit,
                          &on_ti_delete, &on_ti_done};

    /// Repeats due now for the held key, as KeyEvent{repeat=true} + text.
    void pump_repeat() {
        if (!repeat_.active()) return;
        // While the IME owns the keyboard, the compositor repeats for it.
        if (ti_enabled_) { repeat_.cancel(); return; }
        const int n = repeat_.due(wldetail::monotonic_now());
        for (int i = 0; i < n; ++i) emit_key(repeat_.key, true);
    }

    // ── teardown ────────────────────────────────────────────────────────

    ~Impl() { teardown(); }

    void teardown() noexcept {
        if (display_ == nullptr) return;
        const wl::Lib& L = wl::lib();
        raw_ = nullptr;
        if (cursor_buffer_ != nullptr) { wl::request_destroy(cursor_buffer_, wl::buffer_destroy); cursor_buffer_ = nullptr; }
        if (cursor_surface_ != nullptr) { wl::request_destroy(cursor_surface_, wl::surface_destroy); cursor_surface_ = nullptr; }

#ifdef MAYAG_WITH_VULKAN
        // The GPU device wraps the wl_surface (its VkSurfaceKHR) and marshals
        // its own teardown over the wl_display, so it MUST be destroyed while
        // both are still alive — before the surface is destroyed and the
        // display disconnected below. Doing it in the VulkanDevice destructor
        // instead runs it after ~Impl and crashes in the driver.
        if (gpu_active_) { gpu_->destroy(); gpu_active_ = false; }
        readback_gpu_.destroy();
#endif

        for (auto& b : buffers_) b.unmap();
        if (pool_ != nullptr)         wl::request_destroy(pool_, wl::shm_pool_destroy);
        if (pool_base_ != nullptr)    ::munmap(pool_base_, pool_bytes_);
        if (pool_fd_ >= 0)            ::close(pool_fd_);

        if (compose_state_ != nullptr && xkb_.compose_state_unref != nullptr)
            xkb_.compose_state_unref(compose_state_);
        if (compose_table_ != nullptr && xkb_.compose_table_unref != nullptr)
            xkb_.compose_table_unref(compose_table_);
        if (xkb_state_ != nullptr && xkb_.state_unref != nullptr)
            xkb_.state_unref(xkb_state_);
        if (xkb_keymap_ != nullptr && xkb_.keymap_unref != nullptr)
            xkb_.keymap_unref(xkb_keymap_);
        if (xkb_context_ != nullptr && xkb_.context_unref != nullptr)
            xkb_.context_unref(xkb_context_);
        if (xkb_.handle != nullptr) ::dlclose(xkb_.handle);

        for (auto& fb : pending_feedback_) wl::destroy(fb->proxy);   // never answered now
        pending_feedback_.clear();
        wl::destroy(frame_callback_);
        wl::destroy(cursor_device_);
        wl::destroy(viewport_);
        wl::destroy(frac_scale_);
        wl::destroy(decoration_);
        wl::destroy(toplevel_);
        wl::destroy(xdg_surface_);
        wl::destroy(surface_);
        wl::destroy(pointer_);
        wl::destroy(keyboard_);

        // The globals bound off the registry are proxies too, and leak if they
        // are not destroyed. wl_display_disconnect frees the connection but
        // not the objects created on it — ASan sees each one as a direct leak.
        wl::destroy(cursor_manager_);
        if (text_input_ != nullptr)     wl::request_destroy(text_input_, wl::text_input_destroy);
        if (text_input_mgr_ != nullptr) wl::request_destroy(text_input_mgr_, wl::text_input_mgr_destroy);
        text_input_ = text_input_mgr_ = nullptr;
        if (tearing_ != nullptr) wl::request_destroy(tearing_, wl::tearing_control_destroy);
        if (tearing_mgr_ != nullptr) wl::request_destroy(tearing_mgr_, wl::tearing_mgr_destroy);
        tearing_ = tearing_mgr_ = nullptr;
        if (presentation_ != nullptr) wl::request_destroy(presentation_, wl::presentation_destroy);
        presentation_ = nullptr;
        wl::destroy(viewporter_);
        wl::destroy(frac_manager_);
        wl::destroy(deco_manager_);
        wl::destroy(seat_);
        wl::destroy(output_);
        wl::destroy(wm_base_);
        wl::destroy(shm_);
        wl::destroy(compositor_);
        wl::destroy(registry_);

        L.display_disconnect(display_);
        display_ = nullptr;
        open_ = false;
    }

    // ── state ───────────────────────────────────────────────────────────

    static constexpr std::size_t buffer_count = 3;

    wl::proxy* display_        = nullptr;
    wl::proxy* registry_       = nullptr;
    wl::proxy* compositor_     = nullptr;
    wl::proxy* shm_            = nullptr;
    wl::proxy* wm_base_        = nullptr;
    wl::proxy* seat_           = nullptr;
    wl::proxy* pointer_        = nullptr;
    wl::proxy* keyboard_       = nullptr;
    wl::proxy* output_         = nullptr;
    wl::proxy* surface_        = nullptr;
    wl::proxy* xdg_surface_    = nullptr;
    wl::proxy* toplevel_       = nullptr;
    wl::proxy* decoration_     = nullptr;
    wl::proxy* deco_manager_   = nullptr;
    wl::proxy* frac_manager_   = nullptr;
    wl::proxy* frac_scale_     = nullptr;
    wl::proxy* viewporter_     = nullptr;
    wl::proxy* viewport_       = nullptr;
    wl::proxy* cursor_manager_ = nullptr;
    wl::proxy* cursor_device_  = nullptr;
    wl::proxy* tearing_mgr_    = nullptr;
    wl::proxy* tearing_        = nullptr;
    wl::proxy* presentation_   = nullptr;
    std::vector<std::unique_ptr<Feedback>> pending_feedback_;   ///< presentation feedbacks not yet answered
    bool       pres_clock_monotonic_ = true;
    double     vblank_s_  = 0.0;          // last presented frame, monotonic s
    std::uint64_t vblank_seq_ = 0;
    std::uint32_t period_ns_ = 0;         // refresh period from the compositor
    std::uint32_t last_event_ms_ = 0;     // newest input's wl timestamp this dispatch
    PhotonMeter photon_;                  // input to photon (photon.hpp, shared with Cocoa)
    bool       photon_manual_ = false;
    wl::proxy* frame_callback_ = nullptr;
    wl::proxy* pool_           = nullptr;

    std::uint8_t* pool_base_  = nullptr;
    std::size_t   pool_bytes_ = 0;
    int           pool_fd_    = -1;

    wldetail::ShmBuffer buffers_[buffer_count]{};
    backend::Framebuffer fb_{1, 1};
    bool shm_made_ = false;   ///< buffers_ and fb_ exist (made on first CPU present)

    WindowConfig       cfg_{};
    std::string        title_{};
    std::string        clipboard_{};
    std::vector<Event> pending_{};

    Vec2  logical_{1024, 640};
    float scale_   = 1.0f;
    int   pixel_w_ = 0;
    int   pixel_h_ = 0;

    bool open_             = false;
    bool configured_       = false;
    bool resize_needed_    = false;
    bool first_frame_      = true;
    bool damage_supported_ = false;
    bool frame_pending_    = false;

    Vec2        pointer_pos_{};
    Vec2        scroll_accum_{};
    Mods        mods_{};
    CursorShape cursor_ = CursorShape::arrow;

    std::uint32_t pointer_serial_ = 0;
    std::function<void(const RawInput&)> raw_;
    std::string keymap_text_;  // the compositor's keymap, for a late raw tap
    enum class CursorMode : std::uint8_t { shape, raw_shape, image, hidden };
    CursorMode cursor_mode_ = CursorMode::shape;
    std::uint32_t cursor_raw_shape_ = 1;
    wl::proxy* cursor_surface_ = nullptr;
    wl::proxy* cursor_buffer_ = nullptr;
    std::int32_t cursor_hot_x_ = 0, cursor_hot_y_ = 0;
    std::uint32_t key_serial_     = 0;
    bool          maximized_      = false;

    double        start_time_       = 0.0;
    double        refresh_hz_       = 60.0;
    std::uint64_t frames_presented_ = 0;
    std::uint64_t frames_skipped_   = 0;   ///< identical frames not resubmitted
    std::uint64_t frames_dropped_   = 0;
    wldetail::KeyRepeat repeat_{};

    // Input methods (zwp_text_input_v3). The manager is global; the text
    // input is per seat. Enabled while the app has a text field focused.
    wl::proxy* text_input_mgr_ = nullptr;
    wl::proxy* text_input_     = nullptr;
    bool       ti_entered_     = false;   ///< compositor sent `enter` for our surface
    bool       ti_enabled_     = false;   ///< we sent enable (+commit)
    bool       ti_want_        = false;   ///< the app has a text field focused
    Rect       ti_rect_{};                ///< caret rect, logical px
    Rect       ti_sent_rect_{Vec2{-1, -1}, Vec2{}};
    wldetail::TextInputState ti_{};

    const backend::CoverageSampler* sampler_ = nullptr;
    backend::ImageStore images_;

#ifdef MAYAG_WITH_VULKAN
    // GPU renderer. `gpu_active_` gates every GPU code path; when false the
    // window is on the shm path and `gpu_` is an inert, unattached device.
    std::unique_ptr<backend::VulkanDevice> gpu_ = std::make_unique<backend::VulkanDevice>();
    bool gpu_active_ = false;
    bool gpu_pending_ = false;   ///< prepared on a worker; adopt on the first frame after
    std::optional<jaal::kernel::pool> prep_pool_;
    jaal::kernel::job_result<std::unique_ptr<backend::VulkanDevice>> prep_;
    const void* atlas_ = nullptr;
    std::function<void(backend::VulkanDevice&)> atlas_sync_;
    // A second, offscreen device just for read_pixels()/screenshots on the GPU
    // path, plus the last frame it should reproduce.
    backend::VulkanDevice readback_gpu_{};
    DrawList last_list_{};
    Color<Srgb> last_clear_{};
    bool last_had_frame_ = false;
#endif

    Xkb   xkb_{};
    void* xkb_context_ = nullptr;
    void* xkb_keymap_  = nullptr;
    void* xkb_state_   = nullptr;
    void* compose_table_ = nullptr;
    void* compose_state_ = nullptr;
    };

  public:
    WaylandWindow() = default;

    WaylandWindow(const WaylandWindow&)            = delete;
    WaylandWindow& operator=(const WaylandWindow&) = delete;

    /// Moving is a pointer swap. Every listener libwayland holds points at the
    /// heap-pinned Impl, never at this handle, so a move is invisible to the
    /// compositor — which is the entire reason Impl exists.
    WaylandWindow(WaylandWindow&&) noexcept            = default;
    WaylandWindow& operator=(WaylandWindow&&) noexcept = default;

    ~WaylandWindow() = default;

    [[nodiscard]] static std::optional<WaylandWindow> open(const WindowConfig& cfg) {
        WaylandWindow w;
        w.p_ = std::make_unique<Impl>();
        if (!w.p_->boot(cfg)) return std::nullopt;
        return w;
    }

    void close() { p_->close(); }
    [[nodiscard]] bool is_open() const noexcept { return p_ && p_->is_open(); }

    [[nodiscard]] Vec2  size()      const noexcept { return p_->size(); }
    [[nodiscard]] float dpi_scale() const noexcept { return p_->dpi_scale(); }
    /// Pixel dimensions of what read_pixels() returns. See Impl::pixel_size.
    [[nodiscard]] Vec2 pixel_size() const noexcept { return p_->pixel_size(); }

    [[nodiscard]] int fd() const noexcept { return p_ ? p_->fd() : -1; }
    [[nodiscard]] std::vector<Event> dispatch() { return p_->dispatch(); }

    void present(const DrawList& dl, Color<Srgb> clear) { p_->present(dl, clear); }

    /// The compositor's vsync clock, once a frame has been presented.
    [[nodiscard]] std::optional<DisplayTiming> display_timing() const noexcept {
        return p_ ? p_->display_timing() : std::nullopt;
    }
    /// Input to photon, measured by wp_presentation.
    [[nodiscard]] PhotonStats photon_stats() const noexcept {
        return p_ ? p_->photon_stats() : PhotonStats{};
    }

    [[nodiscard]] double now() const noexcept { return p_->now(); }
    [[nodiscard]] double double_click_interval() const noexcept {
        return p_->double_click_interval();
    }

    void set_title(std::string_view t)   { p_->set_title(t); }
    void set_fullscreen(bool on)         { p_->set_fullscreen(on); }
    void set_cursor(CursorShape shape)   { p_->set_cursor(shape); }
    void set_cursor_shape_raw(std::uint32_t shape) { p_->set_cursor_shape_raw(shape); }
    void hide_cursor() { p_->hide_cursor(); }
    void set_cursor_image(const std::uint8_t* argb, int w, int h, int stride, int hot_x, int hot_y) {
        p_->set_cursor_image(argb, w, h, stride, hot_x, hot_y);
    }
    void set_raw_input(std::function<void(const RawInput&)> f) { p_->set_raw_input(std::move(f)); }
    void set_photon_manual(bool on) noexcept { p_->set_photon_manual(on); }
    void mark_input(double at) noexcept { p_->mark_input(at); }
    [[nodiscard]] double event_seconds(std::uint32_t ms) const noexcept { return p_->event_seconds(ms); }
    void set_photon_sink(std::function<void(double)> f) { p_->set_photon_sink(std::move(f)); }
    void begin_move()                    { p_->begin_move(); }
    void minimize()                      { p_->minimize(); }
    void toggle_maximize()               { p_->toggle_maximize(); }
    void set_clipboard(std::string_view text) { p_->set_clipboard(text); }
    [[nodiscard]] std::string get_clipboard() { return p_->get_clipboard(); }

    /// A text field has keyboard focus (`on`), its caret at `caret` (logical
    /// px): enables the input method (fcitx5, ibus) and places its
    /// candidate window. Cheap to call every frame; only changes go out.
    void set_text_input(bool on, Rect caret) {
        if (!p_) return;
        p_->ti_want_ = on;
        if (on) p_->ti_rect_ = caret;
        p_->sync_text_input();
    }
    /// Seconds until the held key's next client-side repeat, or a negative
    /// value when no key is held. The host wakes then and pumps dispatch().
    [[nodiscard]] double seconds_until_repeat() const noexcept {
        if (!p_ || !p_->repeat_.active() || p_->ti_enabled_) return -1.0;
        return std::max(0.0, p_->repeat_.next - wldetail::monotonic_now());
    }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() { return p_->read_pixels(); }

    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        p_->upload_image(id, std::move(img));
    }
    bool update_image_regions(std::uint32_t id, int w, int h, const std::vector<std::array<int, 4>>& rects,
                              const std::vector<std::uint8_t>& packed) {
        return p_->update_image_regions(id, w, h, rects, packed);
    }
    void release_image(std::uint32_t id) { p_->release_image(id); }

#ifdef MAYAG_WITH_VULKAN
    /// A foreign GPU buffer (dmabuf) as image(id), zero copies. See Impl.
    [[nodiscard]] ImportResult import_image(std::uint32_t id, const Dmabuf& d, int w, int h,
                                            std::function<void()> on_free) {
        return p_->import_image(id, d, w, h, std::move(on_free));
    }
    [[nodiscard]] bool can_import_images() const noexcept { return p_->can_import_images(); }
    [[nodiscard]] std::span<const FormatModifier> importable_formats() const { return p_->importable_formats(); }
    bool poll_gpu_retired() { return p_->poll_gpu_retired(); }
    void after_gpu(std::function<void()> f) { p_->after_gpu(std::move(f)); }
    [[nodiscard]] std::uint32_t import_timeline(int fd) { return p_->import_timeline(fd); }
    void release_timeline(std::uint32_t t) { p_->release_timeline(t); }
    void frame_wait(std::uint32_t t, std::uint64_t v) { p_->frame_wait(t, v); }
    void frame_signal(std::uint32_t t, std::uint64_t v) { p_->frame_signal(t, v); }
    void signal_after_submitted(std::uint32_t t, std::uint64_t v) { p_->signal_after_submitted(t, v); }
    [[nodiscard]] bool explicit_sync() const noexcept { return p_->explicit_sync(); }
    void release_texture(std::uint32_t id) { p_->release_image(id); }
#endif

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        p_->set_coverage_sampler(s);
    }

    template <typename AtlasT>
    void set_atlas_source(AtlasT* atlas) noexcept { p_->set_atlas_source(atlas); }

    [[nodiscard]] std::string_view renderer_name() const noexcept {
        return p_->renderer_name();
    }
    [[nodiscard]] bool gpu_active() const noexcept { return p_->gpu_active(); }
    /// The GPU finished coming up after the first CPU frames: redraw now so
    /// the switch happens at once, not at the next input.
    [[nodiscard]] bool wants_redraw() const noexcept { return p_ && p_->gpu_ready_to_adopt(); }
    [[nodiscard]] bool redraw_pending() const noexcept { return p_ && p_->gpu_pending(); }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept {
        return p_->frames_presented();
    }
    [[nodiscard]] std::uint64_t frames_skipped() const noexcept { return p_->frames_skipped(); }
    /// See window_host::next_draw_at: a draw never waits behind a queued one.
    [[nodiscard]] bool replaces_queued_frames() const noexcept { return p_->replaces_queued_frames(); }

    [[nodiscard]] std::uint64_t frames_dropped() const noexcept {
        return p_->frames_dropped();
    }

  private:
    std::unique_ptr<Impl> p_;
};

}  // namespace mayag::platform

#endif  // __linux__ || __FreeBSD__
