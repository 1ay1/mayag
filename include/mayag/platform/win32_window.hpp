#pragma once
// mayag::platform::Win32Window — a real window on Windows
//
// Satisfies the same `Window` concept as Cocoa, Wayland and Headless. The
// six things mayag asks of an OS, in Win32 terms:
//
//   1. a drawable surface  → a CreateWindowExW HWND
//   2. events              → the thread message queue, translated to mayag::Event
//   3. size and DPI        → WM_SIZE + per-monitor-v2 WM_DPICHANGED
//   4. present             → a Vulkan swapchain on the HWND, or GDI if no GPU
//   5. a clock             → QueryPerformanceCounter
//   6. clipboard + cursor  → the Win32 clipboard and LoadCursorW
//
// Design notes worth knowing before changing anything here:
//
// * Vulkan is the default renderer, via VK_KHR_win32_surface rendering
//   straight into the HWND. The CPU path is the fallback, not the plan: it
//   keeps the window working on a machine with no usable device, and it is
//   what --png and the headless tests already use. Which one won is logged
//   once by the runtime; gpu_active() reports it.
//
// * The software path blits with SetDIBitsToDevice from a buffer kept across
//   frames, encoding and blitting only the tiles the rasteriser changed. A
//   full-frame encode would discard the tiled renderer's damage tracking
//   (93% of tiles are untouched while typing) and allocate 9 MB a frame.
//
// * Win32 delivers input through a callback, not a queue you can drain. So the
//   WndProc pushes mayag::Events onto a per-window deque that dispatch()
//   drains -- the "events out of it, without blocking" the concept asks for.
//
// * Keyboard is split exactly as mayag wants it: WM_KEYDOWN produces a
//   KeyEvent for named keys, and WM_CHAR produces a TextEvent for text, so
//   layouts and dead keys stay the OS's problem. Surrogate pairs from WM_CHAR
//   are reassembled before being converted to UTF-8.
//
// * DPI: the process is made per-monitor-v2 aware, so `size()` is logical and
//   dpi_scale() is real. Without that Windows silently bitmap-stretches the
//   whole window on a HiDPI display and the terminal looks blurry.

#include "../backend/software.hpp"
#include "../backend/tiled.hpp"
#include "../core/event.hpp"
#include "../render/draw_list.hpp"
#include "types.hpp"

// Vulkan is the default renderer on Windows, exactly as it is on Linux: it
// self-loads at runtime (vulkan-1.dll ships with every modern driver) and
// falls back to the GDI/software path when there is no device -- a headless
// VM, an RDP session, MAYAG_BACKEND=software. Compiled in when
// MAYAG_WITH_VULKAN is defined (the default on Windows).
#ifdef MAYAG_WITH_VULKAN
// vulkan.hpp is the hand-transcribed ABI (namespace vk); vulkan_device.hpp is
// the device logic built on it and does NOT include it itself, so the order
// here matters.
#  include "../backend/vulkan.hpp"
#  include "../backend/vulkan_device.hpp"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#  define NOMINMAX 1
#endif
// NOGDI must NOT be set here: this backend blits through GDI
// (SetDIBitsToDevice/BITMAPINFO). hand defines it for its own translation
// units, so undo it for the duration of this header's Win32 includes.
#ifdef NOGDI
#  undef NOGDI
#endif
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::platform {

namespace win32_detail {

inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/// Ask for per-monitor-v2 DPI awareness once per process. Done in code rather
/// than a manifest so a plain `cmake --build` produces a crisp binary with no
/// extra link steps, and loaded dynamically so the binary still starts on
/// builds of Windows that predate the API.
inline void enable_dpi_awareness() {
    static const bool once = [] {
        using Ctx = HANDLE;
        using Fn = BOOL(WINAPI*)(Ctx);
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            const auto set_ctx = reinterpret_cast<Fn>(reinterpret_cast<void*>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (set_ctx != nullptr && set_ctx(reinterpret_cast<Ctx>(static_cast<INT_PTR>(-4)))) return true;
        }
        return ::SetProcessDPIAware() != FALSE;
    }();
    (void)once;
}

/// Translate a virtual-key code to mayag's named-key vocabulary. Printable
/// keys deliberately return `unknown`: they arrive as WM_CHAR text instead.
inline Key key_of(WPARAM vk) {
    switch (vk) {
        case VK_RETURN: return Key::enter;
        case VK_TAB: return Key::tab;
        case VK_BACK: return Key::backspace;
        case VK_DELETE: return Key::del;
        case VK_INSERT: return Key::insert;
        case VK_ESCAPE: return Key::escape;
        case VK_SPACE: return Key::space;
        case VK_LEFT: return Key::left;
        case VK_RIGHT: return Key::right;
        case VK_UP: return Key::up;
        case VK_DOWN: return Key::down;
        case VK_HOME: return Key::home;
        case VK_END: return Key::end;
        case VK_PRIOR: return Key::page_up;
        case VK_NEXT: return Key::page_down;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return Key::shift;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return Key::ctrl;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return Key::alt;
        case VK_LWIN: case VK_RWIN: return Key::super;
        case VK_CAPITAL: return Key::caps_lock;
        case VK_F1: return Key::f1;   case VK_F2: return Key::f2;
        case VK_F3: return Key::f3;   case VK_F4: return Key::f4;
        case VK_F5: return Key::f5;   case VK_F6: return Key::f6;
        case VK_F7: return Key::f7;   case VK_F8: return Key::f8;
        case VK_F9: return Key::f9;   case VK_F10: return Key::f10;
        case VK_F11: return Key::f11; case VK_F12: return Key::f12;
        case VK_OEM_MINUS: return Key::minus;
        case VK_OEM_PLUS: return Key::equal;
        case VK_OEM_4: return Key::bracket_left;
        case VK_OEM_6: return Key::bracket_right;
        case VK_OEM_5: return Key::backslash;
        case VK_OEM_1: return Key::semicolon;
        case VK_OEM_7: return Key::quote;
        case VK_OEM_COMMA: return Key::comma;
        case VK_OEM_PERIOD: return Key::period;
        case VK_OEM_2: return Key::slash;
        case VK_OEM_3: return Key::grave;
        default: break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<Key>(static_cast<int>(Key::a) + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<Key>(static_cast<int>(Key::n0) + static_cast<int>(vk - '0'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<Key>(static_cast<int>(Key::n0) + static_cast<int>(vk - VK_NUMPAD0));
    return Key::unknown;
}

inline Mods mods_now() {
    const auto down = [](int vk) { return (::GetKeyState(vk) & 0x8000) != 0; };
    return Mods{.shift = down(VK_SHIFT), .ctrl = down(VK_CONTROL),
                .alt = down(VK_MENU), .super = down(VK_LWIN) || down(VK_RWIN)};
}

}  // namespace win32_detail

class Win32Window {
  public:
    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;
    Win32Window(Win32Window&& o) noexcept { move_from(std::move(o)); }
    Win32Window& operator=(Win32Window&& o) noexcept {
        if (this != &o) { destroy(); move_from(std::move(o)); }
        return *this;
    }
    ~Win32Window() { destroy(); }

    [[nodiscard]] static std::optional<Win32Window> open(const WindowConfig& cfg) {
        win32_detail::enable_dpi_awareness();

        Win32Window w;
        w.state_ = std::make_unique<State>();
        w.state_->title = cfg.title;
        w.state_->clear = cfg.background;
        w.state_->decorated = cfg.decorated;

        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        static const ATOM cls = [inst] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            // OWNDC: we blit from one thread and never share the DC.
            // No background brush: painting it would flash before our blit.
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
            wc.lpfnWndProc = &Win32Window::proc;
            wc.hInstance = inst;
            wc.hCursor = nullptr;   // set_cursor owns the cursor
            wc.hbrBackground = nullptr;
            wc.lpszClassName = L"mayag.window";
            return ::RegisterClassExW(&wc);
        }();
        if (cls == 0) return std::nullopt;

        // Convert the requested LOGICAL size to physical pixels for the
        // monitor we are about to appear on, then ask for a client area of
        // exactly that size.
        const UINT dpi = dpi_for_new_window();
        w.state_->dpi = static_cast<float>(dpi) / 96.0f;
        const DWORD style = cfg.decorated ? (cfg.resizable ? WS_OVERLAPPEDWINDOW
                                                           : (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX))
                                          : WS_POPUP;
        RECT r{0, 0, static_cast<LONG>(cfg.size.x * w.state_->dpi),
               static_cast<LONG>(cfg.size.y * w.state_->dpi)};
        adjust_rect(&r, style, dpi);

        const HWND hwnd = ::CreateWindowExW(0, L"mayag.window", win32_detail::widen(cfg.title).c_str(), style,
                                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                            nullptr, nullptr, inst, w.state_.get());
        if (hwnd == nullptr) return std::nullopt;
        w.state_->hwnd = hwnd;
        w.sync_size();
        w.start_watcher();
        w.boot_gpu(cfg);
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);
        return w;
    }

    void close() {
        if (state_ && state_->hwnd != nullptr) ::DestroyWindow(state_->hwnd);
        if (state_) { state_->hwnd = nullptr; state_->open = false; }
    }
    [[nodiscard]] bool is_open() const noexcept { return state_ && state_->open; }

    [[nodiscard]] Vec2 size() const noexcept { return state_ ? state_->size : Vec2{}; }
    [[nodiscard]] float dpi_scale() const noexcept { return state_ ? state_->dpi : 1.0f; }

    /// A waitable HANDLE that jaal's reactor sleeps on, so an idle terminal
    /// costs no CPU instead of being woken on a fixed poll period.
    ///
    /// Win32 exposes no file descriptor for "the message queue", so one is
    /// manufactured: a watcher thread blocks in MsgWaitForMultipleObjectsEx
    /// (the documented "wait until this thread has input" primitive) and
    /// signals `input_event` when the queue goes non-empty. jaal waits on
    /// that event exactly as it waits on Wayland's display fd, and dispatch()
    /// resets and re-arms it. The thread costs ~8 KB of stack and is asleep
    /// in the kernel whenever nothing is happening.
    [[nodiscard]] void* fd() const noexcept { return state_ ? state_->input_event : nullptr; }

    /// Drain the thread's message queue into mayag events. Never blocks: jaal
    /// owns the waiting, so this is PeekMessage, not GetMessage.
    [[nodiscard]] std::vector<Event> dispatch() {
        std::vector<Event> out;
        if (!state_) return out;
        // The reactor woke us because the queue's event fired; clear it before
        // draining so a message that arrives DURING the drain re-signals and
        // we are called again, rather than sleeping on a non-empty queue.
        if (state_->input_event != nullptr) ::ResetEvent(state_->input_event);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (msg.message == WM_QUIT) { state_->open = false; break; }
            ::TranslateMessage(&msg);   // WM_KEYDOWN -> WM_CHAR for text
            ::DispatchMessageW(&msg);
        }
        // Re-arm the watcher now that the queue is drained.
        if (state_->rearm != nullptr) ::SetEvent(state_->rearm);
        // A size or DPI change invalidates the swapchain. Done here, on the
        // UI thread and outside the WndProc, so the rebuild never happens
        // re-entrantly inside a Windows modal resize loop.
        if (state_->resized) {
            state_->resized = false;
            renderer_resized();
        }
        out.reserve(state_->queue.size());
        while (!state_->queue.empty()) {
            out.push_back(std::move(state_->queue.front()));
            state_->queue.pop_front();
        }
        return out;
    }

    void present(const DrawList& dl, Color<Srgb> clear) {
        if (!state_ || state_->hwnd == nullptr) return;
        const int w = static_cast<int>(state_->size.x * state_->dpi);
        const int h = static_cast<int>(state_->size.y * state_->dpi);
        if (w <= 0 || h <= 0) return;

#ifdef MAYAG_WITH_VULKAN
        // GPU path: the Vulkan swapchain owns the HWND's buffers, so there is
        // no DIB, no CPU encode and no damage bookkeeping here -- the driver
        // presents directly. This is the default; it is only inactive when the
        // GPU could not be brought up, in which case control falls through.
        if (state_->gpu_active && state_->gpu) {
            if (state_->atlas_sync) state_->atlas_sync(*state_->gpu);
            // Same pixels as what's on screen: don't wake the GPU for a hover
            // or a model change that didn't change the picture. Texture and
            // atlas uploads count as changes.
            if (state_->had_frame && clear == state_->last_clear &&
                dl.same_as(state_->last_list) && !state_->gpu->needs_present()) {
                ++state_->frames_skipped;
                return;
            }
            state_->gpu->submit(dl, clear);
            state_->last_list.assign_from(dl);
            state_->last_clear = clear;
            state_->had_frame = true;
            ++state_->frames;
            return;
        }
#endif
        present_software(dl, clear, w, h);
    }

  private:
    /// The CPU fallback: rasterise, then blit the damaged band through GDI.
    void present_software(const DrawList& dl, Color<Srgb> clear, int w, int h) {
        if (state_->fb.width() != w || state_->fb.height() != h) state_->fb = backend::Framebuffer{w, h};

        backend::Tiled::render(dl, state_->fb, state_->sampler, &backend::shared_pool(), clear);

        // Encode and blit only what CHANGED. The rasteriser already proves
        // which 64px tiles are untouched (93% of them while typing), so a
        // full-frame encode + full-window blit would throw that away and
        // make an idle terminal cost a megabyte of memcpy per frame.
        const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        const bool resized = state_->blit.size() != bytes;
        if (resized) state_->blit.assign(bytes, 0);
        backend::Tiled::encode_damaged(state_->fb, state_->blit, &backend::shared_pool());

        // The damaged rows, as a single band. GDI's per-call overhead means
        // one blit of the bounding band beats one blit per tile.
        int y0 = 0, y1 = h;
        if (!resized) {
            const auto& mem = state_->fb.tile_memory();
            if (mem.valid && !mem.dirty.empty()) {
                int top = mem.tiles_y, bot = -1;
                for (int ty = 0; ty < mem.tiles_y; ++ty)
                    for (int tx = 0; tx < mem.tiles_x; ++tx)
                        if (mem.dirty[static_cast<std::size_t>(ty) * mem.tiles_x + tx]) {
                            if (ty < top) top = ty;
                            if (ty > bot) bot = ty;
                            break;
                        }
                if (bot < 0) {   // nothing changed: the window already shows it
                    ++state_->frames;
                    return;
                }
                y0 = top * backend::tile_size;
                y1 = std::min(h, (bot + 1) * backend::tile_size);
            }
        }

        // Describe the buffer as it ALREADY is, instead of shuffling bytes to
        // suit GDI. A V4 header carries explicit channel masks, so RGBA8 is
        // consumed directly -- no swap pass, and the scratch buffer stays in
        // the exact layout encode_damaged() expects to patch next frame.
        BITMAPV4HEADER bi{};
        bi.bV4Size = sizeof bi;
        bi.bV4Width = w;
        bi.bV4Height = -h;   // negative: top-down, like our buffer
        bi.bV4Planes = 1;
        bi.bV4BitCount = 32;
        bi.bV4V4Compression = BI_BITFIELDS;
        bi.bV4RedMask = 0x000000FFu;
        bi.bV4GreenMask = 0x0000FF00u;
        bi.bV4BlueMask = 0x00FF0000u;
        bi.bV4AlphaMask = 0xFF000000u;

        const HDC dc = ::GetDC(state_->hwnd);
        if (dc != nullptr) {
            // Source and destination rows both start at y0; the DIB is
            // top-down so the source origin is counted from its first row.
            ::SetDIBitsToDevice(dc, 0, y0, static_cast<DWORD>(w), static_cast<DWORD>(y1 - y0), 0, 0,
                                static_cast<UINT>(y0), static_cast<UINT>(h), state_->blit.data(),
                                reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS);
            ::ReleaseDC(state_->hwnd, dc);
        }
        ++state_->frames;
    }

  public:

    [[nodiscard]] double now() const noexcept {
        LARGE_INTEGER f, c;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&c);
        return f.QuadPart != 0 ? static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
    }

    [[nodiscard]] double double_click_interval() const noexcept {
        const UINT ms = ::GetDoubleClickTime();
        return ms > 0 ? static_cast<double>(ms) / 1000.0 : 0.5;
    }

    void set_title(std::string_view t) {
        if (!state_) return;
        state_->title = std::string{t};
        if (state_->hwnd != nullptr) ::SetWindowTextW(state_->hwnd, win32_detail::widen(t).c_str());
    }

    void set_cursor(CursorShape c) {
        if (!state_) return;
        state_->cursor = c;
        ::SetCursor(cursor_handle(c));
    }

    void set_clipboard(std::string_view s) {
        if (!state_ || ::OpenClipboard(state_->hwnd) == FALSE) return;
        ::EmptyClipboard();
        const std::wstring w = win32_detail::widen(s);
        // CF_UNICODETEXT so other apps get the text losslessly; the buffer
        // becomes the clipboard's property, so it must not be freed here.
        if (const HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)); mem != nullptr) {
            if (void* dst = ::GlobalLock(mem); dst != nullptr) {
                std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                ::GlobalUnlock(mem);
                ::SetClipboardData(CF_UNICODETEXT, mem);
            } else {
                ::GlobalFree(mem);
            }
        }
        ::CloseClipboard();
    }

    [[nodiscard]] std::string get_clipboard() {
        if (!state_ || ::IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) return {};
        if (::OpenClipboard(state_->hwnd) == FALSE) return {};
        std::string out;
        if (const HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h != nullptr)
            if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h)); p != nullptr) {
                out = win32_detail::narrow(p);
                ::GlobalUnlock(h);
            }
        ::CloseClipboard();
        return out;
    }

    [[nodiscard]] std::vector<std::uint8_t> read_pixels() {
        if (!state_) return {};
#ifdef MAYAG_WITH_VULKAN
        // A present-mode Vulkan device has no readback path -- the swapchain
        // images belong to the compositor, and render_offscreen() needs a
        // device brought up with init_offscreen(). Re-rasterising the last
        // list on the CPU gives the same picture (the GPU/software
        // equivalence test is what guarantees that), which is all
        // read_pixels' callers -- screenshots, tests -- actually want.
        if (state_->gpu_active) {
            const int w = static_cast<int>(state_->size.x * state_->dpi);
            const int h = static_cast<int>(state_->size.y * state_->dpi);
            if (w > 0 && h > 0) {
                if (state_->fb.width() != w || state_->fb.height() != h)
                    state_->fb = backend::Framebuffer{w, h};
                backend::Tiled::render(state_->last_list, state_->fb, state_->sampler,
                                       &backend::shared_pool(), state_->last_clear);
            }
        }
#endif
        return state_->fb.to_rgba8();
    }

    // ── window management (optional capabilities the host probes for) ────

    /// Hand the drag back to the window manager, so a title-bar press moves
    /// the window with full snap/aero behaviour instead of us chasing the
    /// mouse. WM_SYSCOMMAND + SC_MOVE enters the OS's own modal move loop.
    void begin_move() {
        if (state_ && state_->hwnd != nullptr) {
            ::ReleaseCapture();
            ::SendMessageW(state_->hwnd, WM_SYSCOMMAND, SC_MOVE | 0x0002, 0);
        }
    }
    void minimize() {
        if (state_ && state_->hwnd != nullptr) ::ShowWindow(state_->hwnd, SW_MINIMIZE);
    }
    void toggle_maximize() {
        if (!state_ || state_->hwnd == nullptr) return;
        ::ShowWindow(state_->hwnd, ::IsZoomed(state_->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }

    /// Borderless fullscreen: remember the framed placement, then stretch to
    /// the monitor the window is on. Preferred over a mode change -- it is
    /// instant, keeps other windows intact, and is what terminals want.
    void set_fullscreen(bool on) {
        if (!state_ || state_->hwnd == nullptr || on == state_->fullscreen) return;
        const HWND hwnd = state_->hwnd;
        if (on) {
            state_->saved_placement.length = sizeof(WINDOWPLACEMENT);
            ::GetWindowPlacement(hwnd, &state_->saved_placement);
            state_->saved_style = ::GetWindowLongW(hwnd, GWL_STYLE);
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) != FALSE) {
                ::SetWindowLongW(hwnd, GWL_STYLE, (state_->saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
                ::SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                               mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                               SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            }
        } else {
            ::SetWindowLongW(hwnd, GWL_STYLE, state_->saved_style);
            ::SetWindowPlacement(hwnd, &state_->saved_placement);
            ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        state_->fullscreen = on;
        sync_size();
    }
    [[nodiscard]] bool fullscreen() const noexcept { return state_ && state_->fullscreen; }

    /// Tell Windows whether an input method should be active, and where the
    /// caret is, so the IME candidate window appears in the right place.
    void set_text_input(bool on, Rect caret) {
        if (!state_ || state_->hwnd == nullptr) return;
        state_->text_input = on;
        if (const HIMC imc = ::ImmGetContext(state_->hwnd); imc != nullptr) {
            if (on) {
                COMPOSITIONFORM cf{};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = static_cast<LONG>(caret.origin.x * state_->dpi);
                cf.ptCurrentPos.y = static_cast<LONG>((caret.origin.y + caret.size.y) * state_->dpi);
                ::ImmSetCompositionWindow(imc, &cf);
            }
            ::ImmReleaseContext(state_->hwnd, imc);
        }
        ::ImmAssociateContextEx(state_->hwnd, nullptr, on ? IACE_DEFAULT : 0);
    }

    void set_coverage_sampler(const backend::CoverageSampler* s) {
        if (!state_) return;
        state_->images.set_glyphs(s);
        state_->sampler = &state_->images;
    }
    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        if (!state_) return;
#ifdef MAYAG_WITH_VULKAN
        if (state_->gpu_active && state_->gpu && img)
            (void)state_->gpu->upload_texture(id, img->rgba.data(), img->width, img->height);
#endif
        state_->images.upload(id, std::move(img));
    }
    void release_image(std::uint32_t id) {
        if (!state_) return;
#ifdef MAYAG_WITH_VULKAN
        if (state_->gpu_active && state_->gpu) state_->gpu->release_texture(id);
#endif
        state_->images.release(id);
    }

    /// Remember how to push dirty glyph rects to the GPU. Type-erased because
    /// the Atlas type is a template parameter of the FontStack; the CPU path
    /// reads the atlas through the coverage sampler and needs no upload.
    template <typename AtlasT>
    void set_atlas_source([[maybe_unused]] AtlasT* atlas) noexcept {
#ifdef MAYAG_WITH_VULKAN
        if (!state_) return;
        if (atlas == nullptr) { state_->atlas_sync = nullptr; return; }
        state_->atlas_sync = [atlas](backend::VulkanDevice& d) { d.sync_atlas(*atlas); };
#endif
    }

    [[nodiscard]] Vec2 pixel_size() const noexcept {
        if (!state_) return {};
        return Vec2{state_->size.x * state_->dpi, state_->size.y * state_->dpi};
    }
    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return state_ ? state_->frames : 0; }

    [[nodiscard]] std::string_view renderer_name() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        if (state_ && state_->gpu_active) return "vulkan";
#endif
        return "software";
    }
    [[nodiscard]] bool gpu_active() const noexcept {
#ifdef MAYAG_WITH_VULKAN
        return state_ != nullptr && state_->gpu_active;
#else
        return false;
#endif
    }

  private:
    Win32Window() = default;

    /// Everything the WndProc touches, at a stable address: the HWND's
    /// userdata points here, so the window may be moved while open.
    struct State {
        HWND hwnd = nullptr;
        bool open = true;
        bool decorated = true;
        Vec2 size{1024, 640};
        float dpi = 1.0f;
        std::deque<Event> queue;
        backend::Framebuffer fb{1024, 640};
        const backend::CoverageSampler* sampler = nullptr;
        backend::ImageStore images;
        std::string title;
        Color<Srgb> clear{};
        CursorShape cursor = CursorShape::arrow;
        bool tracking_mouse = false;
        bool text_input = false;
        bool resized = false;   ///< a WM_SIZE/DPI change needs a swapchain rebuild
        bool fullscreen = false;
        LONG saved_style = 0;
        WINDOWPLACEMENT saved_placement{};
        std::uint32_t high_surrogate = 0;
        std::uint64_t frames = 0;
        Vec2 last_pos{};
        std::vector<std::uint8_t> blit;   ///< BGRA scratch, kept across frames

#ifdef MAYAG_WITH_VULKAN
        // ── the GPU path ────────────────────────────────────────────
        std::unique_ptr<backend::VulkanDevice> gpu;
        bool gpu_active = false;
        /// Type-erased because the Atlas type is a template parameter of the
        /// FontStack, which this header must not depend on.
        std::function<void(backend::VulkanDevice&)> atlas_sync;
        // The last list presented, so an identical frame can be skipped
        // instead of waking the GPU for pixels that already match.
        DrawList last_list;
        Color<Srgb> last_clear{};
        bool had_frame = false;
        std::uint64_t frames_skipped = 0;
#endif

        // ── the "queue is non-empty" event, and the thread that sets it ──
        HANDLE input_event = nullptr;   ///< manual reset: jaal waits on this
        HANDLE rearm = nullptr;         ///< auto reset: dispatch() drained it
        HANDLE quit = nullptr;          ///< manual reset: shut the watcher down
        HANDLE watcher = nullptr;
        DWORD ui_thread = 0;
    };
    std::unique_ptr<State> state_;

    void move_from(Win32Window&& o) noexcept {
        state_ = std::move(o.state_);
        if (state_ && state_->hwnd != nullptr)
            ::SetWindowLongPtrW(state_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state_.get()));
    }
    void destroy() {
        stop_watcher();
#ifdef MAYAG_WITH_VULKAN
        // Before the HWND goes away: the swapchain holds a VkSurfaceKHR that
        // references it, and destroying the window first is undefined.
        if (state_ && state_->gpu) {
            state_->gpu->destroy();
            state_->gpu_active = false;
        }
#endif
        if (state_ && state_->hwnd != nullptr) {
            ::SetWindowLongPtrW(state_->hwnd, GWLP_USERDATA, 0);
            ::DestroyWindow(state_->hwnd);
            state_->hwnd = nullptr;
        }
    }

    /// Tell the renderer the drawable changed size. On the GPU path this
    /// rebuilds the swapchain; the CPU path reallocates lazily in present().
    void renderer_resized() {
#ifdef MAYAG_WITH_VULKAN
        if (state_ && state_->gpu_active && state_->gpu) {
            state_->gpu->resize(state_->size, state_->dpi);
            state_->had_frame = false;   // the old contents are gone
        }
#endif
    }

    /// Manufacture the "this thread has input" readiness object jaal needs.
    ///
    /// The watcher never touches the queue -- only the UI thread may call
    /// PeekMessage -- it just reports that something is THERE. The handshake
    /// is deliberately strict to avoid both a spin and a lost wakeup:
    ///
    ///   watcher: wait for input  ->  SetEvent(input_event)  ->  wait(rearm)
    ///   jaal:    wait(input_event) -> dispatch() -> ResetEvent, SetEvent(rearm)
    ///
    /// Between signalling and being re-armed the watcher is blocked, so it
    /// cannot spin on a queue that the UI thread has not drained yet.
    void start_watcher() {
        if (!state_) return;
        State* st = state_.get();
        st->ui_thread = ::GetCurrentThreadId();
        st->input_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual
        st->rearm = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);        // auto
        st->quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);          // manual
        if (st->input_event == nullptr || st->rearm == nullptr || st->quit == nullptr) return;

        st->watcher = ::CreateThread(
            nullptr, 16 * 1024,
            [](LPVOID p) -> DWORD {
                auto* s = static_cast<State*>(p);
                for (;;) {
                    if (::WaitForSingleObject(s->quit, 0) == WAIT_OBJECT_0) return 0;
                    // QS_ALLINPUT | QS_ALLPOSTMESSAGE: anything that would make
                    // PeekMessage return. MWMO_INPUTAVAILABLE reports a message
                    // that is ALREADY queued, which closes the race where one
                    // arrives between our drain and this wait.
                    const DWORD r = ::MsgWaitForMultipleObjectsEx(
                        1, &s->quit, INFINITE, QS_ALLINPUT | QS_ALLPOSTMESSAGE, MWMO_INPUTAVAILABLE);
                    if (r == WAIT_OBJECT_0) return 0;              // quit
                    if (r == WAIT_FAILED) return 1;
                    ::SetEvent(s->input_event);                    // tell jaal
                    // Block until the UI thread has drained, so this loop
                    // cannot spin while the queue stays non-empty.
                    const HANDLE both[2] = {s->quit, s->rearm};
                    if (::WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0) return 0;
                }
            },
            st, 0, nullptr);
    }

    void stop_watcher() {
        if (!state_) return;
        State* st = state_.get();
        if (st->quit != nullptr) ::SetEvent(st->quit);
        if (st->rearm != nullptr) ::SetEvent(st->rearm);   // unblock a waiting watcher
        if (st->watcher != nullptr) {
            // The watcher only ever waits on our own events, so it exits
            // promptly; the bound wait is belt and braces against a hang.
            if (::WaitForSingleObject(st->watcher, 1000) == WAIT_TIMEOUT)
                ::TerminateThread(st->watcher, 0);
            ::CloseHandle(st->watcher);
            st->watcher = nullptr;
        }
        for (HANDLE* h : {&st->input_event, &st->rearm, &st->quit})
            if (*h != nullptr) { ::CloseHandle(*h); *h = nullptr; }
    }

    /// Bring up Vulkan on the HWND, or leave the CPU path in charge.
    ///
    /// Failure is not an error: no vulkan-1.dll, no device, a software
    /// adapter under RDP, or MAYAG_BACKEND=software all land on the GDI
    /// path, which renders the same pixels more slowly. The choice is silent
    /// by design; the runtime logs the winner once.
    void boot_gpu([[maybe_unused]] const WindowConfig& cfg) {
#ifdef MAYAG_WITH_VULKAN
        State* st = state_.get();
        const char* forced = ::getenv("MAYAG_BACKEND");
        const bool allow = forced == nullptr || std::string_view{forced} != "software";
        if (allow) {
            auto d = std::make_unique<backend::VulkanDevice>();
            d->set_low_latency(!cfg.vsync);
            d->set_transparent(cfg.transparent);
            // The HINSTANCE is accepted but not required by the WSI call; the
            // HWND is what identifies the surface.
            if (d->attach_win32(::GetModuleHandleW(nullptr), st->hwnd, st->size, st->dpi)) {
                st->gpu = std::move(d);
                st->gpu_active = true;
                // Anything uploaded before the GPU existed has to be replayed.
                for (const auto& [id, img] : st->images.all())
                    (void)st->gpu->upload_texture(id, img->rgba.data(), img->width, img->height);
                // The DIB scratch is dead weight now; give it back.
                st->blit.clear();
                st->blit.shrink_to_fit();
                st->fb = backend::Framebuffer{1, 1};
                startup::mark("vulkan ready");
                return;
            }
            d->destroy();
        }
        if (!st->gpu) st->gpu = std::make_unique<backend::VulkanDevice>();
        startup::mark("vulkan unavailable (cpu)");
#endif
    }

    void sync_size() {
        if (!state_ || state_->hwnd == nullptr) return;
        RECT rc{};
        ::GetClientRect(state_->hwnd, &rc);
        const float inv = state_->dpi > 0.0f ? 1.0f / state_->dpi : 1.0f;
        state_->size = Vec2{static_cast<float>(rc.right - rc.left) * inv,
                            static_cast<float>(rc.bottom - rc.top) * inv};
    }

    [[nodiscard]] static UINT dpi_for_new_window() {
        using Fn = UINT(WINAPI*)();
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            const auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForSystem")));
            if (f != nullptr) return f();
        }
        return 96;
    }

    /// AdjustWindowRectExForDpi when available, so the client area is exactly
    /// the size asked for on a HiDPI monitor.
    static void adjust_rect(RECT* r, DWORD style, UINT dpi) {
        using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            const auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(
                ::GetProcAddress(user32, "AdjustWindowRectExForDpi")));
            if (f != nullptr && f(r, style, FALSE, 0, dpi)) return;
        }
        ::AdjustWindowRectEx(r, style, FALSE, 0);
    }

    [[nodiscard]] static HCURSOR cursor_handle(CursorShape c) {
        // IDC_* are macros that expand to a char* or wchar_t* depending on
        // UNICODE; LoadCursorW needs the wide form, so spell them with the
        // MAKEINTRESOURCEW the macros wrap.
        const wchar_t* id = reinterpret_cast<const wchar_t*>(32512);   // IDC_ARROW
        switch (c) {
            case CursorShape::text:        id = reinterpret_cast<const wchar_t*>(32513); break;  // IDC_IBEAM
            case CursorShape::pointer:
            case CursorShape::grab:        id = reinterpret_cast<const wchar_t*>(32649); break;  // IDC_HAND
            case CursorShape::grabbing:    id = reinterpret_cast<const wchar_t*>(32649); break;
            case CursorShape::crosshair:   id = reinterpret_cast<const wchar_t*>(32515); break;  // IDC_CROSS
            case CursorShape::resize_h:    id = reinterpret_cast<const wchar_t*>(32644); break;  // IDC_SIZEWE
            case CursorShape::resize_v:    id = reinterpret_cast<const wchar_t*>(32645); break;  // IDC_SIZENS
            case CursorShape::resize_nwse: id = reinterpret_cast<const wchar_t*>(32642); break;  // IDC_SIZENWSE
            case CursorShape::resize_nesw: id = reinterpret_cast<const wchar_t*>(32643); break;  // IDC_SIZENESW
            case CursorShape::not_allowed: id = reinterpret_cast<const wchar_t*>(32648); break;  // IDC_NO
            case CursorShape::wait:        id = reinterpret_cast<const wchar_t*>(32514); break;  // IDC_WAIT
            case CursorShape::arrow:       break;
        }
        return ::LoadCursorW(nullptr, id);
    }

    [[nodiscard]] Vec2 logical(LPARAM lp) const {
        const float inv = state_->dpi > 0.0f ? 1.0f / state_->dpi : 1.0f;
        return Vec2{static_cast<float>(GET_X_LPARAM(lp)) * inv, static_cast<float>(GET_Y_LPARAM(lp)) * inv};
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return ::DefWindowProcW(hwnd, msg, wp, lp);
        }
        auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st == nullptr) return ::DefWindowProcW(hwnd, msg, wp, lp);

        const auto push = [st](Event e) { st->queue.push_back(std::move(e)); };
        const float inv = st->dpi > 0.0f ? 1.0f / st->dpi : 1.0f;
        const auto pos = [&](LPARAM l) {
            return Vec2{static_cast<float>(GET_X_LPARAM(l)) * inv, static_cast<float>(GET_Y_LPARAM(l)) * inv};
        };

        switch (msg) {
            case WM_CLOSE:
                // The app decides; never destroy the window behind its back.
                push(CloseRequest{});
                return 0;

            case WM_DESTROY:
                st->open = false;
                st->hwnd = nullptr;
                return 0;

            case WM_SIZE: {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                st->size = Vec2{static_cast<float>(rc.right - rc.left) * inv,
                                static_cast<float>(rc.bottom - rc.top) * inv};
                st->resized = true;   // dispatch() rebuilds the swapchain
                push(ResizeEvent{st->size, st->dpi});
                return 0;
            }

            case WM_DPICHANGED: {
                // The suggested rect keeps the window the same PHYSICAL size
                // across a monitor change; honouring it is what stops the
                // window jumping when it crosses a DPI boundary.
                st->dpi = static_cast<float>(LOWORD(wp)) / 96.0f;
                const auto* sug = reinterpret_cast<const RECT*>(lp);
                ::SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                               sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const float i2 = st->dpi > 0.0f ? 1.0f / st->dpi : 1.0f;
                st->size = Vec2{static_cast<float>(rc.right - rc.left) * i2,
                                static_cast<float>(rc.bottom - rc.top) * i2};
                st->resized = true;
                push(ResizeEvent{st->size, st->dpi});
                return 0;
            }

            case WM_PAINT: {
                // Satisfy Windows so the region is not reported dirty forever,
                // but do NOT synthesise an event here. mayag redraws from its
                // own loop, and a ResizeEvent per paint would be a feedback
                // loop: paint -> event -> model touched -> present -> paint,
                // which pins a core at ~30% on a completely idle terminal.
                // Our present() blits the whole client area every frame, so
                // there is never a partially-stale region to repair.
                PAINTSTRUCT ps;
                ::BeginPaint(hwnd, &ps);
                ::EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_SETFOCUS: push(FocusEvent{true}); return 0;
            case WM_KILLFOCUS: push(FocusEvent{false}); return 0;

            case WM_SETCURSOR:
                // Inside the client area the app owns the cursor; on the frame
                // let Windows draw the resize arrows.
                if (LOWORD(lp) == HTCLIENT) {
                    ::SetCursor(cursor_handle(st->cursor));
                    return TRUE;
                }
                break;

            case WM_MOUSEMOVE: {
                if (!st->tracking_mouse) {
                    // Ask for WM_MOUSELEAVE so a cursor that leaves the window
                    // does not leave a stale hover highlight behind.
                    TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                    ::TrackMouseEvent(&tme);
                    st->tracking_mouse = true;
                }
                const Vec2 p = pos(lp);
                push(MouseMove{p, p - st->last_pos, win32_detail::mods_now()});
                st->last_pos = p;
                return 0;
            }

            case WM_MOUSELEAVE:
                st->tracking_mouse = false;
                return 0;

            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
                const bool dbl = msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK;
                const MouseButton b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? MouseButton::left
                                    : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? MouseButton::right
                                                                                         : MouseButton::middle;
                // Capture so a drag that leaves the window still reports.
                ::SetCapture(hwnd);
                push(MouseDown{pos(lp), b, win32_detail::mods_now(), dbl ? 2 : 1,
                               static_cast<double>(::GetMessageTime()) / 1000.0});
                return 0;
            }

            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
                const MouseButton b = msg == WM_LBUTTONUP ? MouseButton::left
                                    : msg == WM_RBUTTONUP ? MouseButton::right
                                                          : MouseButton::middle;
                ::ReleaseCapture();
                push(MouseUp{pos(lp), b, win32_detail::mods_now()});
                return 0;
            }

            case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
                const MouseButton b = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::back : MouseButton::forward;
                if (msg == WM_XBUTTONDOWN)
                    push(MouseDown{pos(lp), b, win32_detail::mods_now(), 1,
                                   static_cast<double>(::GetMessageTime()) / 1000.0});
                else
                    push(MouseUp{pos(lp), b, win32_detail::mods_now()});
                return TRUE;
            }

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                // Wheel coordinates are SCREEN, unlike every other mouse
                // message, so they must be mapped back into the client area.
                POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ::ScreenToClient(hwnd, &sp);
                const float ticks = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                // Three lines per notch, in logical pixels, matching what the
                // user configured for the system.
                UINT lines = 3;
                ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                const float px = ticks * static_cast<float>(lines) * 16.0f;
                const Vec2 d = msg == WM_MOUSEWHEEL ? Vec2{0.0f, px} : Vec2{-px, 0.0f};
                push(ScrollEvent{d,
                                 Vec2{static_cast<float>(sp.x) * inv, static_cast<float>(sp.y) * inv},
                                 win32_detail::mods_now(), false});
                return 0;
            }

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                const Key k = win32_detail::key_of(wp);
                if (k != Key::unknown)
                    push(KeyEvent{k, win32_detail::mods_now(), (lp & (1 << 30)) != 0});
                // F10/Alt chords must not open the system menu in a terminal.
                if (msg == WM_SYSKEYDOWN && (wp == VK_F10 || wp == VK_MENU)) return 0;
                break;   // let TranslateMessage produce WM_CHAR for text
            }

            case WM_CHAR:
            case WM_SYSCHAR: {
                const auto c = static_cast<std::uint32_t>(wp);
                // Control characters are the KeyEvent's job, not text.
                if (c < 0x20 || c == 0x7f) return 0;
                // Reassemble surrogate pairs: Windows delivers astral
                // characters (emoji) as two WM_CHARs.
                if (c >= 0xD800 && c <= 0xDBFF) { st->high_surrogate = c; return 0; }
                std::uint32_t cp = c;
                if (c >= 0xDC00 && c <= 0xDFFF) {
                    if (st->high_surrogate == 0) return 0;
                    cp = 0x10000 + ((st->high_surrogate - 0xD800) << 10) + (c - 0xDC00);
                    st->high_surrogate = 0;
                }
                std::string utf8;
                if (cp < 0x80) {
                    utf8 += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    utf8 += static_cast<char>(0xC0 | (cp >> 6));
                    utf8 += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    utf8 += static_cast<char>(0xE0 | (cp >> 12));
                    utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8 += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    utf8 += static_cast<char>(0xF0 | (cp >> 18));
                    utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8 += static_cast<char>(0x80 | (cp & 0x3F));
                }
                push(TextEvent{std::move(utf8)});
                return 0;
            }

            case WM_IME_COMPOSITION: {
                if ((lp & GCS_COMPSTR) != 0) {
                    if (const HIMC imc = ::ImmGetContext(hwnd); imc != nullptr) {
                        const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                        std::wstring w(static_cast<std::size_t>(bytes > 0 ? bytes : 0) / sizeof(wchar_t), L'\0');
                        if (bytes > 0) ::ImmGetCompositionStringW(imc, GCS_COMPSTR, w.data(), static_cast<DWORD>(bytes));
                        const auto caret = static_cast<std::uint32_t>(::ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0));
                        ::ImmReleaseContext(hwnd, imc);
                        const std::string s = win32_detail::narrow(w);
                        push(ComposeEvent{s, 0, 0, caret <= s.size() ? caret : static_cast<std::uint32_t>(s.size())});
                    }
                }
                if ((lp & GCS_RESULTSTR) != 0) {
                    if (const HIMC imc = ::ImmGetContext(hwnd); imc != nullptr) {
                        const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
                        std::wstring w(static_cast<std::size_t>(bytes > 0 ? bytes : 0) / sizeof(wchar_t), L'\0');
                        if (bytes > 0) ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, w.data(), static_cast<DWORD>(bytes));
                        ::ImmReleaseContext(hwnd, imc);
                        push(ComposeEndEvent{});
                        push(TextEvent{win32_detail::narrow(w)});
                    }
                    return 0;
                }
                break;
            }

            case WM_IME_ENDCOMPOSITION:
                push(ComposeEndEvent{});
                return 0;

            case WM_ERASEBKGND:
                return 1;   // we paint every pixel; erasing would only flash

            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
};

}  // namespace mayag::platform
