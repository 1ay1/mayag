#pragma once
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
//   * EVENT-DRIVEN BLOCKING via prepare_read/poll. `Wait::block` is a real
//     poll() on the Wayland fd with no timeout, so an idle app is genuinely
//     asleep in the kernel at 0% CPU — which is the property the runtime's
//     whole subscription model exists to enable.

#include "../app/event.hpp"
#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../core/geometry.hpp"
#include "../render/draw_list.hpp"
#include "types.hpp"
#include "waker.hpp"
#include "wayland_protocol.hpp"

// The Vulkan backend is the default renderer on Linux; it self-loads at
// runtime and falls back to the shm path if the GPU is unavailable. Compiled
// in when MAYAG_WITH_VULKAN is defined (the default on Linux); otherwise the
// shm path is the only renderer.
#ifdef MAYAG_WITH_VULKAN
#include "../backend/vulkan.hpp"
#endif

#if defined(__linux__) || defined(__FreeBSD__)

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

        // Commit with no buffer, then wait: the xdg protocol requires the
        // first configure to arrive before the first buffer is attached.
        wl::request(surface_, wl::surface_commit);
        L.display_roundtrip(display_);

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
        const char* forced = ::getenv("MAYAG_BACKEND");
        const bool allow_gpu = forced == nullptr ||
                               std::string_view{forced} == "vulkan" ||
                               std::string_view{forced} == "gpu";
        if (allow_gpu && gpu_.attach_wayland(display_, surface_, logical_, scale_)) {
            gpu_active_ = true;
        }
#endif

        // The shm path is still initialised even when the GPU is active: it is
        // the read_pixels() fallback and the resize target if the GPU is later
        // lost. It costs one framebuffer allocation, which is cheap insurance.
        resize_surfaces();
        return true;
    }


    void close() {
        open_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // ── geometry ────────────────────────────────────────────────────────

    [[nodiscard]] Vec2  size() const noexcept { return logical_; }
    [[nodiscard]] float dpi_scale() const noexcept { return scale_; }

    // ── events ──────────────────────────────────────────────────────────

    /// Drain the compositor, translating to mayag events.
    ///
    /// The prepare_read/read_events dance is Wayland's answer to the classic
    /// "check then block" race: between deciding the queue is empty and
    /// calling poll(), another thread (or this one, via a flush) could enqueue
    /// an event, and a naive implementation would sleep through it. Announcing
    /// the intent to read FIRST makes the wakeup reliable.
    [[nodiscard]] std::vector<Event> poll_events(Wait wait, double timeout) {
        const wl::Lib& L = wl::lib();
        pending_.clear();
        if (!open_ || display_ == nullptr) return {};

        // Anything already decoded but not yet dispatched.
        L.display_dispatch_pending(display_);

        if (!pending_.empty()) wait = Wait::immediate;

        int timeout_ms = 0;
        switch (wait) {
            case Wait::immediate: timeout_ms = 0; break;
            case Wait::block:     timeout_ms = -1; break;
            case Wait::poll:
                // Pace to the compositor. If a frame callback is outstanding
                // the compositor will wake us when it wants the next frame, so
                // block; otherwise do not sleep at all.
                timeout_ms = frame_pending_ ? 50 : 0;
                break;
            case Wait::timeout:
                timeout_ms = timeout > 0.0 ? static_cast<int>(timeout * 1000.0) : 0;
                break;
        }

        // Announce the read, flush our requests, then sleep.
        while (L.display_prepare_read(display_) != 0) {
            L.display_dispatch_pending(display_);
        }
        L.display_flush(display_);

        // Poll the display fd AND the waker fd together. The waker is how a
        // background thread (a socket, a subprocess, a Cmd::stream) interrupts
        // an idle UI thread: without it, an app blocked in poll() with no
        // window activity would sleep through streamed messages until the user
        // moved the mouse. A byte on the waker pipe wakes us the instant the
        // producer posts.
        pollfd pfds[2];
        pfds[0] = pollfd{L.display_get_fd(display_), POLLIN, 0};
        int nfds = 1;
        const int waker_fd = (waker_ != nullptr) ? waker_->fd() : -1;
        if (waker_fd >= 0) {
            pfds[1] = pollfd{waker_fd, POLLIN, 0};
            nfds = 2;
        }

        const int r = ::poll(pfds, static_cast<nfds_t>(nfds), timeout_ms);
        if (r > 0 && (pfds[0].revents & POLLIN) != 0) {
            L.display_read_events(display_);
        } else {
            // Timed out, or woken by the waker rather than the compositor.
            // Either way there are no display events to read, so cancel the
            // announced read to keep libwayland's state consistent.
            L.display_cancel_read(display_);
        }
        // Clear the waker so the next post() writes a fresh byte. Cheap and
        // harmless when nothing woke us.
        if (nfds == 2 && (pfds[1].revents & POLLIN) != 0 && waker_ != nullptr) {
            waker_->drain();
        }

        L.display_dispatch_pending(display_);

        // A protocol error is fatal and unrecoverable — the compositor has
        // already dropped the connection. Closing cleanly beats spinning on a
        // dead fd forever.
        if (L.display_get_error(display_) != 0) {
            open_ = false;
        }

        if (resize_needed_) {
            resize_surfaces();
#ifdef MAYAG_WITH_VULKAN
            if (gpu_active_) gpu_.resize(logical_, scale_);
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
        // GPU path: the Vulkan swapchain owns the wl_surface's buffers, so
        // there is no shm buffer, no CPU encode, and no damage bookkeeping
        // here — the driver presents directly. This is the default on Linux;
        // it is only inactive when the GPU could not be brought up, in which
        // case gpu_.valid() is false and control falls through to shm.
        if (gpu_active_) {
            if (atlas_ != nullptr) sync_atlas_to_gpu();
            gpu_.submit(dl, clear);
            last_list_ = dl; last_clear_ = clear; last_had_frame_ = true;
            // Still request a frame callback so Wait::poll paces on the
            // compositor exactly as the shm path does.
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
        wl::request(surface_, wl::surface_commit);
        wl::lib().display_flush(display_);
        ++frames_presented_;
    }

    // ── services ────────────────────────────────────────────────────────

    [[nodiscard]] double now() const noexcept {
        return wldetail::monotonic_now() - start_time_;
    }

    /// Wayland has no protocol for this; GTK and Qt both default to 400 ms.
    [[nodiscard]] double double_click_interval() const noexcept { return 0.4; }

    void set_title(std::string_view t) {
        if (toplevel_ == nullptr) return;
        title_.assign(t);
        wl::request(toplevel_, wl::toplevel_set_title, title_.c_str());
    }

    void set_cursor(CursorShape shape) {
        if (shape == cursor_) return;
        cursor_ = shape;
        apply_cursor();
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
        // On the GPU path the swapchain owns the pixels, not `fb_`. Re-render
        // the last presented frame offscreen (into an sRGB target that reads
        // back byte-identical to the software path) so screenshots and golden
        // tests see real GPU output rather than the empty shm buffer.
        if (gpu_active_ && last_had_frame_) {
            std::vector<std::uint8_t> out;
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
        return fb_.to_rgba8();
    }

    // ── runtime hooks ───────────────────────────────────────────────────

    void set_coverage_sampler(const backend::CoverageSampler* s) { sampler_ = s; }

    /// Give the window the runtime's waker so poll() can be interrupted by a
    /// background thread. Set once at boot; a null waker just means the app
    /// has no off-thread producers and poll() watches only the display fd.
    void set_waker(Waker* w) noexcept { waker_ = w; }

#ifdef MAYAG_WITH_VULKAN
    /// Upload dirty glyph rects to the GPU atlas image before a frame that
    /// uses them. Cheap in steady state — the atlas reports an empty dirty
    /// region and the upload is skipped.
    void sync_atlas_to_gpu() {
        if (atlas_sync_) atlas_sync_(gpu_);
    }
#endif

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

    [[nodiscard]] std::uint64_t frames_presented() const noexcept {
        return frames_presented_;
    }

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

        first_frame_ = true;
        damage_supported_ = wl::version_of(surface_) >= 4;
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
        if (cursor_device_ == nullptr || pointer_serial_ == 0) return;
        wl::request(cursor_device_, wl::cursor_dev_set_shape, pointer_serial_,
                    wldetail::cursor_to_shape(cursor_));
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
            w->seat_ = bind(L.i_seat, 5);
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
        } else if (id == "wp_cursor_shape_manager_v1") {
            w->cursor_manager_ = bind(&wl::xdg().cursor_mgr, 1);
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
        // Park the pointer off-surface so hover state clears.
        w->pending_.push_back(MouseMove{Vec2{-1.0f, -1.0f}, Vec2{}, w->mods_});
    }

    static void on_ptr_motion(void* data, wl::proxy*, std::uint32_t,
                              wl::fixed_t sx, wl::fixed_t sy) {
        Impl* w = self(data);
        const Vec2 p{static_cast<float>(wl::fixed_to_double(sx)),
                     static_cast<float>(wl::fixed_to_double(sy))};
        const Vec2 delta{p.x - w->pointer_pos_.x, p.y - w->pointer_pos_.y};
        w->pointer_pos_ = p;
        w->pending_.push_back(MouseMove{p, delta, w->mods_});
    }

    static void on_ptr_button(void* data, wl::proxy*, std::uint32_t serial,
                              std::uint32_t, std::uint32_t button,
                              std::uint32_t state) {
        Impl* w = self(data);
        w->pointer_serial_ = serial;

        // evdev button codes.
        MouseButton b = MouseButton::left;
        switch (button) {
            case 0x110: b = MouseButton::left;   break;   // BTN_LEFT
            case 0x111: b = MouseButton::right;  break;   // BTN_RIGHT
            case 0x112: b = MouseButton::middle; break;   // BTN_MIDDLE
            default: return;
        }

        if (state == wl::pointer_button_pressed) {
            w->pending_.push_back(MouseDown{w->pointer_pos_, b, w->mods_, 1});
        } else {
            w->pending_.push_back(MouseUp{w->pointer_pos_, b, w->mods_});
        }
    }

    static void on_ptr_axis(void* data, wl::proxy*, std::uint32_t,
                            std::uint32_t axis, wl::fixed_t value) {
        Impl* w = self(data);
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
        // Scroll is delivered as one-or-more axis events terminated by a
        // frame. Coalescing here means a high-resolution trackpad produces one
        // mayag event per physical gesture step, not three.
        if (w->scroll_accum_.x != 0.0f || w->scroll_accum_.y != 0.0f) {
            w->pending_.push_back(ScrollEvent{w->scroll_accum_, w->pointer_pos_,
                                              w->mods_, false});
            w->scroll_accum_ = Vec2{};
        }
    }

    static void on_ptr_axis_source(void*, wl::proxy*, std::uint32_t) {}
    static void on_ptr_axis_stop(void*, wl::proxy*, std::uint32_t, std::uint32_t) {}
    static void on_ptr_axis_discrete(void*, wl::proxy*, std::uint32_t, std::int32_t) {}
    static void on_ptr_axis_value120(void* data, wl::proxy*, std::uint32_t axis,
                                     std::int32_t v120) {
        Impl* w = self(data);
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
        w->load_keymap(format, fd, size);
    }

    static void on_kb_enter(void* data, wl::proxy*, std::uint32_t, wl::proxy*,
                            wl::wl_array*) {
        self(data)->pending_.push_back(FocusEvent{true});
    }

    static void on_kb_leave(void* data, wl::proxy*, std::uint32_t, wl::proxy*) {
        Impl* w = self(data);
        w->mods_ = Mods{};
        w->pending_.push_back(FocusEvent{false});
    }

    static void on_kb_key(void* data, wl::proxy*, std::uint32_t serial,
                          std::uint32_t, std::uint32_t key, std::uint32_t state) {
        Impl* w = self(data);
        w->key_serial_ = serial;

        // evdev codes are offset by 8 from the X11 keycodes xkbcommon wants.
        const std::uint32_t xkb_code = key + 8u;

        // mayag's Event variant models key PRESSES only — a release carries no
        // information the app model uses, and inventing a KeyUp alternative
        // here would be a platform backend widening the public event type.
        if (state != wl::keyboard_key_pressed) return;

        w->pending_.push_back(KeyEvent{wldetail::key_from_evdev(key), w->mods_, false});

        // Text. With a keymap this is the layout-correct character (including
        // dead keys and AltGr); without one it is nothing, which is correct —
        // guessing ASCII from a keycode is wrong on every non-US layout.
        if (w->xkb_state_ != nullptr && w->xkb_.key_get_utf8 != nullptr) {
            char buf[32];
            const int n = w->xkb_.key_get_utf8(w->xkb_state_, xkb_code, buf,
                                               sizeof(buf));
            if (n > 0 && !w->mods_.ctrl && !w->mods_.super) {
                const std::string_view s{buf, static_cast<std::size_t>(n)};
                // Filter control characters; Enter/Tab/Backspace are KeyDown.
                if (s.size() > 1 || static_cast<unsigned char>(s[0]) >= 0x20) {
                    w->pending_.push_back(TextEvent{std::string{s}});
                }
            }
        }
    }

    static void on_kb_mods(void* data, wl::proxy*, std::uint32_t,
                           std::uint32_t depressed, std::uint32_t latched,
                           std::uint32_t locked, std::uint32_t group) {
        Impl* w = self(data);
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
        w->repeat_rate_  = rate;
        w->repeat_delay_ = delay;
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

        if (xkb_context_ == nullptr) xkb_context_ = xkb_.context_new(0);
        if (xkb_context_ != nullptr) {
            if (xkb_state_ != nullptr) { xkb_.state_unref(xkb_state_); xkb_state_ = nullptr; }
            if (xkb_keymap_ != nullptr) { xkb_.keymap_unref(xkb_keymap_); xkb_keymap_ = nullptr; }

            // 1 == XKB_KEYMAP_FORMAT_TEXT_V1
            xkb_keymap_ = xkb_.keymap_new_from_string(
                xkb_context_, static_cast<const char*>(map), 1, 0);
            if (xkb_keymap_ != nullptr) xkb_state_ = xkb_.state_new(xkb_keymap_);
        }

        ::munmap(map, size);
        ::close(fd);
    }

    // ── teardown ────────────────────────────────────────────────────────

    ~Impl() { teardown(); }

    void teardown() noexcept {
        if (display_ == nullptr) return;
        const wl::Lib& L = wl::lib();

#ifdef MAYAG_WITH_VULKAN
        // The GPU device wraps the wl_surface (its VkSurfaceKHR) and marshals
        // its own teardown over the wl_display, so it MUST be destroyed while
        // both are still alive — before the surface is destroyed and the
        // display disconnected below. Doing it in the VulkanDevice destructor
        // instead runs it after ~Impl and crashes in the driver.
        if (gpu_active_) { gpu_.destroy(); gpu_active_ = false; }
        readback_gpu_.destroy();
#endif

        for (auto& b : buffers_) b.unmap();
        if (pool_ != nullptr)         wl::request_destroy(pool_, wl::shm_pool_destroy);
        if (pool_base_ != nullptr)    ::munmap(pool_base_, pool_bytes_);
        if (pool_fd_ >= 0)            ::close(pool_fd_);

        if (xkb_state_ != nullptr && xkb_.state_unref != nullptr)
            xkb_.state_unref(xkb_state_);
        if (xkb_keymap_ != nullptr && xkb_.keymap_unref != nullptr)
            xkb_.keymap_unref(xkb_keymap_);
        if (xkb_context_ != nullptr && xkb_.context_unref != nullptr)
            xkb_.context_unref(xkb_context_);
        if (xkb_.handle != nullptr) ::dlclose(xkb_.handle);

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
    wl::proxy* frame_callback_ = nullptr;
    wl::proxy* pool_           = nullptr;

    std::uint8_t* pool_base_  = nullptr;
    std::size_t   pool_bytes_ = 0;
    int           pool_fd_    = -1;

    wldetail::ShmBuffer buffers_[buffer_count]{};
    backend::Framebuffer fb_{1, 1};

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
    std::uint32_t key_serial_     = 0;

    double        start_time_       = 0.0;
    double        refresh_hz_       = 60.0;
    std::uint64_t frames_presented_ = 0;
    std::uint64_t frames_dropped_   = 0;
    std::int32_t  repeat_rate_      = 25;
    std::int32_t  repeat_delay_     = 600;

    const backend::CoverageSampler* sampler_ = nullptr;
    Waker* waker_ = nullptr;

#ifdef MAYAG_WITH_VULKAN
    // GPU renderer. `gpu_active_` gates every GPU code path; when false the
    // window is on the shm path and `gpu_` is an inert, unattached device.
    backend::VulkanDevice gpu_{};
    bool gpu_active_ = false;
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

    [[nodiscard]] std::vector<Event> poll_events(Wait wait, double timeout) {
        return p_->poll_events(wait, timeout);
    }

    void present(const DrawList& dl, Color<Srgb> clear) { p_->present(dl, clear); }

    [[nodiscard]] double now() const noexcept { return p_->now(); }
    [[nodiscard]] double double_click_interval() const noexcept {
        return p_->double_click_interval();
    }

    void set_title(std::string_view t)   { p_->set_title(t); }
    void set_cursor(CursorShape shape)   { p_->set_cursor(shape); }
    void set_clipboard(std::string_view text) { p_->set_clipboard(text); }
    [[nodiscard]] std::string get_clipboard() { return p_->get_clipboard(); }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() { return p_->read_pixels(); }

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        p_->set_coverage_sampler(s);
    }

    void set_waker(Waker* w) noexcept { p_->set_waker(w); }

    template <typename AtlasT>
    void set_atlas_source(AtlasT* atlas) noexcept { p_->set_atlas_source(atlas); }

    [[nodiscard]] std::string_view renderer_name() const noexcept {
        return p_->renderer_name();
    }
    [[nodiscard]] bool gpu_active() const noexcept { return p_->gpu_active(); }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept {
        return p_->frames_presented();
    }

    [[nodiscard]] std::uint64_t frames_dropped() const noexcept {
        return p_->frames_dropped();
    }

  private:
    std::unique_ptr<Impl> p_;
};

}  // namespace mayag::platform

#endif  // __linux__ || __FreeBSD__
