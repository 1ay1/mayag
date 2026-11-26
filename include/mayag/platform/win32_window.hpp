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
#include "concepts.hpp"   // check_capabilities
#include "images.hpp"
#include "gpu_bringup.hpp"
#include "../backend/gpu_policy.hpp"

// The GPU renderer is Direct3D 11 + DXGI flip model by default (the native
// Windows path), Vulkan when MAYAG_BACKEND=vulkan; see backend/win32_gpu.hpp.
// Both load at runtime, and the GDI/software path takes over when neither
// comes up -- a headless VM, an RDP session, MAYAG_BACKEND=software.
#include "../backend/win32_gpu.hpp"

#include "../core/win32.hpp"   // windows.h, with its keyword macros removed
#include <windowsx.h>   // GET_X_LPARAM
// dwmapi.h pulls uxtheme.h, which pulls commctrl.h -- whose DPA_/DSA_
// inline wrappers, emitted by a PCH built with -fpch-codegen, made every
// app import functions comctl32 v5 exports only by ordinal: the exe would
// not start (0xC0000139). mayag uses none of them.
#ifndef NO_COMMCTRL_DA
#  define NO_COMMCTRL_DA
#endif
#include <uxtheme.h>    // MARGINS
#include <dwmapi.h>     // types only: every dwmapi function is loaded at runtime

#include <jaal/platform/windows/wait_reactor.hpp>   // the message-queue sentinel
#include "../core/startup.hpp"

#include <algorithm>
#include <chrono>
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

/// Opt out of Windows 11 power throttling, once per process. On battery (or
/// under a vendor's "optimized" plan) Windows may run a process with
/// EcoQoS -- efficiency cores, lowered clocks -- and ignore its 1 ms timer
/// resolution request, so every frame tick, glide and resize step lands up
/// to ~15 ms late and the whole UI feels like it drags. A UI that animates
/// on the display's clock must not be treated as background work.
inline void opt_out_of_power_throttling() {
    static const bool once = [] {
        struct Throttle { ULONG version, control, state; };   // PROCESS_POWER_THROTTLING_STATE
        constexpr ULONG kExecutionSpeed = 0x1;          // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        constexpr ULONG kIgnoreTimerResolution = 0x4;   // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        constexpr int kProcessPowerThrottling = 4;      // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
        using Fn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        const auto set = k32 != nullptr ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                              ::GetProcAddress(k32, "SetProcessInformation")))
                                        : nullptr;
        if (set == nullptr) return false;
        // control = what we decide, state = 0: neither throttle applies to us
        Throttle t{1, kExecutionSpeed | kIgnoreTimerResolution, 0};
        if (set(::GetCurrentProcess(), kProcessPowerThrottling, &t, sizeof t) != FALSE) return true;
        t.control = kExecutionSpeed;   // pre-22H2 rejects the timer flag
        return set(::GetCurrentProcess(), kProcessPowerThrottling, &t, sizeof t) != FALSE;
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

/// How a Win32 GPU is brought up on the worker (platform/gpu_bringup.hpp).
struct PrepareWin32Gpu {
    static std::unique_ptr<backend::Win32Gpu> run(std::unique_ptr<backend::Win32Gpu> d) {
        if (!d->prepare()) {
            d->destroy();
            d.reset();
        }
        return d;
    }
};
using Win32BringUp = GpuBringUp<backend::Win32Gpu, PrepareWin32Gpu>;

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

    [[nodiscard]] static std::optional<Win32Window> open(const WindowConfig& cfg) { return open_impl(cfg, false); }

  private:
    [[nodiscard]] static std::optional<Win32Window> open_impl(const WindowConfig& cfg, bool force_gdi) {
        win32_detail::enable_dpi_awareness();
        win32_detail::opt_out_of_power_throttling();

        Win32Window w;
        w.state_ = std::make_unique<State>();
        w.state_->title = cfg.title;
        w.state_->clear = cfg.background;
        w.state_->decorated = cfg.decorated;
        w.state_->resizable = cfg.resizable;
        w.state_->vsync = cfg.vsync;
        w.state_->load_settings();

        // GPU first, from frame one. Device + pipeline need no window and are
        // ~all of the bring-up cost (D3D11: ~110 ms device, ~5 ms pipeline
        // with the shader cache warm), so they start NOW on a worker and
        // overlap the class registration, CreateWindowExW and DWM setup
        // below; the first present attaches the swapchain and draws on the
        // GPU. There is no CPU-rendered first frame to hand over from.
        if (!force_gdi) w.start_gpu(cfg);

        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        static const ATOM cls = [inst] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            // No CS_HREDRAW/CS_VREDRAW: they invalidate the whole client on
            // every resize step, which on a GPU swapchain is pure flicker.
            // No CS_OWNDC: the GPU path never touches a DC, and the GDI
            // fallback takes one per present.
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &Win32Window::proc;
            wc.hInstance = inst;
            wc.hCursor = nullptr;        // set_cursor owns the cursor
            wc.hbrBackground = nullptr;  // never erase: we paint every pixel
            wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(1));   // the exe's icon, if any
            wc.lpszClassName = L"mayag.window";
            return ::RegisterClassExW(&wc);
        }();
        if (cls == 0) return std::nullopt;
        startup::mark("win32: class registered");

        // Convert the requested LOGICAL size to physical pixels for the
        // monitor we are about to appear on, then ask for a client area of
        // exactly that size.
        const UINT dpi = dpi_for_new_window();
        w.state_->dpi = static_cast<float>(dpi) / 96.0f;
        // Undecorated is still WS_OVERLAPPEDWINDOW: the caption is removed in
        // WM_NCCALCSIZE instead of by WS_POPUP, which keeps every native
        // behaviour -- Aero Snap and the snap-layout flyout, the resize
        // border, the minimise/maximise animations, and the DWM shadow.
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!cfg.resizable) style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
        RECT r{0, 0, static_cast<LONG>(cfg.size.x * w.state_->dpi),
               static_cast<LONG>(cfg.size.y * w.state_->dpi)};
        if (cfg.decorated) adjust_rect(&r, style, dpi);

        // A flip-model swapchain owns the pixels, so the window needs no GDI
        // redirection surface: without it DWM composes the swapchain
        // directly (no full-window copy, no stale-content smear on resize).
        // The surface can only be chosen at creation, so it is kept only when
        // CPU-first was asked for (MAYAG_GPU_WAIT_MS=0) or no GPU is expected;
        // a GPU that fails or misses its budget gets the window recreated
        // for GDI (recreate_for_gdi), which is rare by construction.
        const bool gpu_expected = w.state_->bringup.pending();
        const bool may_fall_back = !gpu_expected || w.gpu_wait_budget_ms() == 0;
        w.state_->no_redirection = gpu_expected && !may_fall_back;
        const DWORD ex = w.state_->no_redirection ? WS_EX_NOREDIRECTIONBITMAP : 0;
        const HWND hwnd = ::CreateWindowExW(ex, L"mayag.window", win32_detail::widen(cfg.title).c_str(), style,
                                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                            nullptr, nullptr, inst, w.state_.get());
        if (hwnd == nullptr) return std::nullopt;
        startup::mark("win32: CreateWindowExW");
        w.state_->hwnd = hwnd;
        w.state_->monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        // Hidden from DWM until the first frame is presented (reveal()), so
        // the user never sees an empty or half-initialised window.
        const BOOL cloak = TRUE;
        dwm_set(hwnd, 13 /*DWMWA_CLOAK*/, &cloak, sizeof cloak);
        w.state_->cloaked = true;
        w.apply_chrome(cfg);
        if (!cfg.decorated) {
            // Make WM_NCCALCSIZE take effect now that it can see the state.
            ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                           SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        w.sync_size();
        w.refresh_timing();
        startup::mark("win32: dwm chrome + timing");
        w.bind_callbacks();
        startup::mark("win32: callbacks bound");
        ::ShowWindow(hwnd, SW_SHOW);   // still cloaked: revealed by frame one
        startup::mark("win32: ShowWindow (cloaked)");
        return w;
    }

  public:

    void close() {
        if (state_ && state_->hwnd != nullptr) ::DestroyWindow(state_->hwnd);
        if (state_) { state_->hwnd = nullptr; state_->open = false; }
    }
    [[nodiscard]] bool is_open() const noexcept { return state_ && state_->open; }

    [[nodiscard]] Vec2 size() const noexcept { return state_ ? state_->size : Vec2{}; }
    [[nodiscard]] float dpi_scale() const noexcept { return state_ ? state_->dpi : 1.0f; }

    /// What jaal's reactor sleeps on: this thread's window-message queue
    /// itself. The reactor recognises the sentinel and waits with
    /// MsgWaitForMultipleObjectsEx, so a keystroke wakes the loop in one
    /// kernel transition -- no helper thread, no event relay, and the
    /// thread is always in a message-aware wait, so Windows never ghosts
    /// the window as "Not Responding".
    [[nodiscard]] void* fd() const noexcept {
        return state_ ? jaal::platform::wait_reactor::message_queue() : nullptr;
    }

    /// Draw from inside an OS modal loop. Windows traps the thread in
    /// DefWindowProc for the whole of a drag-resize or a window move; the
    /// host installs this so the WndProc can render each step of the drag.
    void set_live_draw(std::function<void()> f) {
        // MAYAG_LIVE_RESIZE=0 restores the stock Windows behaviour (content
        // frozen until the drag ends), for comparison and for debugging.
        if (const char* e = ::getenv("MAYAG_LIVE_RESIZE"); e != nullptr && std::string_view{e} == "0") return;
        if (state_) state_->live_draw = std::move(f);
    }

    /// Drain the thread's message queue into mayag events. Never blocks: jaal
    /// owns the waiting, so this is PeekMessage, not GetMessage.
    [[nodiscard]] std::vector<Event> dispatch() {
        std::vector<Event> out;
        if (!state_) return out;
        if (!state_->in_modal) {
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                if (msg.message == WM_QUIT) { state_->open = false; break; }
                ::TranslateMessage(&msg);   // WM_KEYDOWN -> WM_CHAR for text
                ::DispatchMessageW(&msg);
            }
        }
        // A size or DPI change invalidates the swapchain. Done here, on the
        // UI thread and outside the WndProc, so the rebuild never happens
        // re-entrantly in the middle of a message.
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
        if (!state_ || state_->hwnd == nullptr || state_->minimized) return;
        adopt_gpu();
        if (state_->monitor_changed && !state_->in_modal) {
            state_->monitor_changed = false;
            const HMONITOR m = ::MonitorFromWindow(state_->hwnd, MONITOR_DEFAULTTONEAREST);
            if (m != state_->monitor) {
                state_->monitor = m;
                refresh_timing();
            }
        }
        const int w = static_cast<int>(state_->size.x * state_->dpi);
        const int h = static_cast<int>(state_->size.y * state_->dpi);
        if (w <= 0 || h <= 0) return;

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
            learn_vblank();
            state_->last_list.assign_from(dl);
            state_->last_clear = clear;
            state_->had_frame = true;
            ++state_->frames;
            reveal();
            return;
        }
        if (state_->no_redirection && (state_->gpu_failed || !state_->gpu_active)) recreate_for_gdi();
        present_software(dl, clear, w, h);
        reveal();
    }

  private:
    /// The GPU was expected (so the window was made without a GDI redirection
    /// surface) and failed. GDI cannot present to such a window and the flag
    /// cannot be changed after creation, so a fresh HWND replaces it -- same
    /// State, same placement, same title. Rare by construction: it needs
    /// d3d11.dll present and yet no usable device.
    void recreate_for_gdi() {
        State* st = state_.get();
        st->no_redirection = false;
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        ::GetWindowPlacement(st->hwnd, &wp);
        const DWORD style = static_cast<DWORD>(::GetWindowLongW(st->hwnd, GWL_STYLE));
        const HWND old = st->hwnd;
        ::SetWindowLongPtrW(old, GWLP_USERDATA, 0);
        const HWND hwnd = ::CreateWindowExW(0, L"mayag.window", win32_detail::widen(st->title).c_str(), style & ~WS_VISIBLE,
                                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                            ::GetModuleHandleW(nullptr), st);
        if (hwnd == nullptr) return;
        st->hwnd = hwnd;
        ::SetWindowPlacement(hwnd, &wp);
        const BOOL dark = TRUE;
        dwm_set(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
        ::DestroyWindow(old);
        sync_size();
        st->open = true;
    }

  public:
  private:
    /// The CPU fallback: rasterise, then blit the damaged band through GDI.
    void present_software(const DrawList& dl, Color<Srgb> clear, int w, int h) {
        State* st = state_.get();
        if (st->fb.width() != w || st->fb.height() != h) st->fb = backend::Framebuffer{w, h};

        backend::Tiled::render(dl, st->fb, st->sampler, &backend::shared_pool(), clear);

        // The fastest CPU-to-screen path GDI has: a DIB SECTION in BGRA (its
        // native order), selected into a memory DC once and kept. The tiles
        // the rasteriser changed are encoded straight into the section's
        // bits, and only their band is BitBlt'd -- a plain copy, no format
        // conversion. SetDIBitsToDevice from a client buffer measured
        // 18.5 ms a frame at 1280x800 with channel masks; that path converts
        // every pixel and copies the buffer into GDI first.
        const bool full = ensure_dib(w, h);
        const auto [y0, y1] = backend::Tiled::encode_damaged_bgra(
            st->fb, static_cast<std::uint8_t*>(st->dib_bits), static_cast<std::size_t>(w) * 4u, full,
            &backend::shared_pool());
        if (y1 > y0) {
            if (const HDC dc = ::GetDC(st->hwnd); dc != nullptr) {
                ::BitBlt(dc, 0, y0, w, y1 - y0, st->dib_dc, 0, y0, SRCCOPY);
                ::ReleaseDC(st->hwnd, dc);
            }
        }
        ++st->frames;
    }

    /// (Re)create the BGRA DIB section and its memory DC at w x h. True when
    /// it is new, so the caller encodes every tile, not just the damage.
    bool ensure_dib(int w, int h) {
        State* st = state_.get();
        if (st->dib != nullptr && st->dib_w == w && st->dib_h == h) return false;
        release_dib();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down, like the framebuffer
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        st->dib_dc = ::CreateCompatibleDC(nullptr);
        st->dib = ::CreateDIBSection(st->dib_dc, &bi, DIB_RGB_COLORS, &st->dib_bits, nullptr, 0);
        st->dib_old = ::SelectObject(st->dib_dc, st->dib);
        st->dib_w = w;
        st->dib_h = h;
        return true;
    }

    void release_dib() {
        State* st = state_.get();
        if (st == nullptr || st->dib_dc == nullptr) return;
        if (st->dib_old != nullptr) ::SelectObject(st->dib_dc, st->dib_old);
        if (st->dib != nullptr) ::DeleteObject(st->dib);
        ::DeleteDC(st->dib_dc);
        st->dib = nullptr;
        st->dib_dc = nullptr;
        st->dib_old = nullptr;
        st->dib_bits = nullptr;
        st->dib_w = st->dib_h = 0;
    }

  public:

    [[nodiscard]] double now() const noexcept {
        LARGE_INTEGER f, c;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&c);
        return f.QuadPart != 0 ? static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
    }

    [[nodiscard]] double double_click_interval() const noexcept {
        return state_ ? state_->dbl_click : 0.5;
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
    // image(id): the one policy every window follows (platform/images.hpp)
    void upload_image(std::uint32_t id, std::shared_ptr<const ImageData> img) {
        if (state_) images::upload(state_->images, gpu_images(), id, std::move(img));
    }
    void release_image(std::uint32_t id) {
        if (state_) images::release(state_->images, gpu_images(), id);
    }
    /// The device presenting images now, or null (still on the CPU path).
    [[nodiscard]] backend::Win32Gpu* gpu_images() noexcept {
        return state_ && state_->gpu_active ? state_->gpu.get() : nullptr;
    }

    /// Remember how to push dirty glyph rects to the GPU. Type-erased because
    /// the Atlas type is a template parameter of the FontStack; the CPU path
    /// reads the atlas through the coverage sampler and needs no upload.
    template <typename AtlasT>
    void set_atlas_source([[maybe_unused]] AtlasT* atlas) noexcept {
        if (!state_) return;
        if (atlas == nullptr) { state_->atlas_sync = nullptr; return; }
        state_->atlas_sync = [atlas](backend::Win32Gpu& d) { d.sync_atlas(*atlas); };
    }

    [[nodiscard]] Vec2 pixel_size() const noexcept {
        if (!state_) return {};
        return Vec2{state_->size.x * state_->dpi, state_->size.y * state_->dpi};
    }
    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return state_ ? state_->frames : 0; }

    /// The GPU that won ("Intel(R) Arc(TM) Pro Graphics"), or null on the
    /// CPU path.
    [[nodiscard]] const char* device_name() const noexcept {
        if (state_ && state_->gpu_active && state_->gpu) return state_->gpu->device_name();
        return nullptr;
    }

    [[nodiscard]] std::string_view renderer_name() const noexcept {
        if (state_ && state_->gpu_active && state_->gpu) return state_->gpu->api_name();
        return "software";
    }
    [[nodiscard]] bool gpu_active() const noexcept {
        return state_ != nullptr && state_->gpu_active;
    }

    /// The display's real refresh period and a recent vblank, from DWM and
    /// the monitor's mode. The host lays its frame slots on this grid, so a
    /// 144 Hz panel is driven at 144 Hz and the late-latch draw lands just
    /// before the actual vblank instead of a guessed 60 Hz one.
    [[nodiscard]] std::optional<DisplayTiming> display_timing() const noexcept {
        if (!state_ || state_->timing.seq == 0) return std::nullopt;
        return state_->timing;
    }

    [[nodiscard]] std::uint64_t frames_skipped() const noexcept {
        return state_ ? state_->frames_skipped : 0;
    }

    /// How long before a vblank a late-latch frame must be PRESENTED to make
    /// it. DWM composes well ahead of scan-out: measured with DXGI frame
    /// statistics (MAYAG_PRESENTSTATS=1), a frame handed over 2 ms before the
    /// vblank -- the generic margin -- missed it EVERY time and showed a
    /// whole refresh later (16.5 ms present->glass at 60 Hz). Half a refresh
    /// makes it: ~7.7 ms (3/4 gave ~12; a per-frame AIMD learner, as on
    /// macOS, was tried and oscillated as DWM's deadline moved under it).
    [[nodiscard]] std::chrono::nanoseconds latch_margin() const noexcept {
        const auto p = state_ && state_->timing.period.count() > 0 ? state_->timing.period : std::chrono::nanoseconds{16'666'667};
        return p / 2;
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
        bool no_redirection = false;   ///< created WS_EX_NOREDIRECTIONBITMAP: GDI cannot present
        bool gpu_failed = false;       ///< the GPU was expected and did not come up
        bool fullscreen = false;
        LONG saved_style = 0;
        WINDOWPLACEMENT saved_placement{};
        std::uint32_t high_surrogate = 0;
        std::uint64_t frames = 0;
        Vec2 last_pos{};
        // ── the GDI fallback's present target ────────────────────────
        HDC dib_dc = nullptr;       ///< memory DC the section stays selected into
        HBITMAP dib = nullptr;      ///< top-down BGRA DIB section
        HGDIOBJ dib_old = nullptr;  ///< what dib_dc held before
        void* dib_bits = nullptr;   ///< the section's pixels: encoded into directly
        int dib_w = 0, dib_h = 0;

        // ── the GPU path ────────────────────────────────────────────
        std::unique_ptr<backend::Win32Gpu> gpu;
        bool gpu_active = false;
        /// Type-erased because the Atlas type is a template parameter of the
        /// FontStack, which this header must not depend on.
        std::function<void(backend::Win32Gpu&)> atlas_sync;
        // The last list presented, so an identical frame can be skipped
        // instead of waking the GPU for pixels that already match.
        DrawList last_list;
        Color<Srgb> last_clear{};
        bool had_frame = false;
        std::uint64_t frames_skipped = 0;
        /// Instance, device and swapchain are brought up on a jaal worker
        /// (see start_gpu); adopt_gpu() takes the result between frames.
        Win32BringUp bringup;   ///< platform/gpu_bringup.hpp (the prestart's, when adopted)
        struct Settings { bool low_latency, transparent; };
        std::optional<Settings> gpu_settings;   ///< set when the device was prestarted

        // ── the OS modal loop (drag-resize, window move) ─────────────
        std::function<void()> live_draw;   ///< the host's "draw now"
        std::function<void()> on_resize;   ///< rebuild the swapchain, from the WndProc
        bool in_modal = false;              ///< between WM_ENTER/EXITSIZEMOVE
        UINT_PTR modal_timer = 0;           ///< keeps animation alive while moving
        bool minimized = false;             ///< no surface to draw into
        bool monitor_changed = false;       ///< re-read the refresh rate

        // ── presentation state ───────────────────────────────────────
        bool cloaked = false;               ///< hidden by DWM until frame one
        bool shown = false;                 ///< frame one has been presented
        bool vsync = true;
        bool resizable = true;

        // ── the display clock (DwmGetCompositionTimingInfo) ──────────
        DisplayTiming timing{};
        HMONITOR monitor = nullptr;
        // presentation feedback: the last vblank a shown frame reached the
        // screen on, and the measured refresh period (ns, smoothed)
        LONGLONG fb_qpc = 0;
        UINT     fb_refresh = 0;
        double   fb_period_ns = 0.0;

        // ── input ────────────────────────────────────────────────────
        UINT wheel_lines = 3;   ///< SPI_GETWHEELSCROLLLINES (cached)
        UINT wheel_chars = 3;   ///< SPI_GETWHEELSCROLLCHARS (cached)
        double dbl_click = 0.5; ///< GetDoubleClickTime, seconds (cached)

        void load_settings() {
            UINT v = 3;
            if (::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &v, 0) != FALSE) wheel_lines = v;
            v = 3;
            if (::SystemParametersInfoW(0x006C /*SPI_GETWHEELSCROLLCHARS*/, 0, &v, 0) != FALSE) wheel_chars = v;
            const UINT ms = ::GetDoubleClickTime();
            dbl_click = ms > 0 ? static_cast<double>(ms) / 1000.0 : 0.5;
        }
    };
    std::unique_ptr<State> state_;

    void move_from(Win32Window&& o) noexcept {
        state_ = std::move(o.state_);
        if (state_ && state_->hwnd != nullptr)
            ::SetWindowLongPtrW(state_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state_.get()));
        bind_callbacks();
    }

    /// Callbacks the WndProc invokes capture `this`, which changes when the
    /// window object is moved; re-bind after every move.
    void bind_callbacks() {
        if (!state_) return;
        state_->on_resize = [this] { renderer_resized(); };
    }
    void destroy() {
        release_dib();
        // A prep still running owns a device; let it finish and drop it.
        if (state_)
            if (auto d = state_->bringup.finish()) d->destroy();
        // Before the HWND goes away: the swapchain holds a VkSurfaceKHR that
        // references it, and destroying the window first is undefined.
        if (state_ && state_->gpu) {
            state_->gpu->destroy();
            state_->gpu_active = false;
        }
        if (state_ && state_->hwnd != nullptr) {
            ::SetWindowLongPtrW(state_->hwnd, GWLP_USERDATA, 0);
            ::DestroyWindow(state_->hwnd);
            state_->hwnd = nullptr;
        }
    }

    /// Tell the renderer the drawable changed size. On the GPU path this
    /// rebuilds the swapchain; the CPU path reallocates lazily in present().
    void renderer_resized() {
        if (state_) state_->resized = false;
        if (state_ && state_->gpu_active && state_->gpu) {
            state_->gpu->resize(state_->size, state_->dpi);
            state_->had_frame = false;   // the old contents are gone
        }
    }

    /// Start the GPU's window-independent bring-up (device + pipeline) on a
    /// worker, before the HWND exists. adopt_gpu() attaches the swapchain to
    /// the window when the first frame is presented.
    void start_gpu([[maybe_unused]] const WindowConfig& cfg) {
        if (backend::gpu_policy::software_only()) return;
        State* st = state_.get();
        // Already preparing since the process started (gpu_prestart): adopt
        // that job instead of beginning again.
        if (auto& pre = prestart(); pre.pending()) {
            st->bringup = std::move(pre);
            st->gpu_settings = State::Settings{!cfg.vsync, cfg.transparent};
            return;
        }
        auto d = std::make_unique<backend::Win32Gpu>();
        d->set_low_latency(!cfg.vsync);
        d->set_transparent(cfg.transparent);
        st->bringup.start(std::move(d));
    }

  public:
    /// Start bringing the GPU up NOW, before anything else: device and
    /// pipeline need no window and no app state, and on a warm machine they
    /// are the longest single thing between launch and the first frame
    /// (D3D11 ~100 ms). An app calls this first thing in main(); the window
    /// it opens later adopts the prepared device. Harmless if no window ever
    /// opens (headless runs just never adopt it).
    static void gpu_prestart() {
        if (backend::gpu_policy::software_only()) return;
        auto& pre = prestart();
        if (pre.pending()) return;
        pre.start(std::make_unique<backend::Win32Gpu>());
    }

  private:
    /// The bring-up begun by gpu_prestart(), before any window exists; the
    /// first window to open moves it into its State. Written only from the
    /// main thread (prestart in main(), adoption in open()).
    static Win32BringUp& prestart() {
        static Win32BringUp p;
        return p;
    }

    /// How long the first present may wait for the GPU, in ms: the GPU draws
    /// frame one on every API, and only a GPU that misses this budget lets the
    /// CPU draw first. Default 1000 for D3D11 (ready in ~110 ms warm, ~370 ms
    /// cold) and 3000 for Vulkan (0.3 s warm, ~2.1 s cold on a hybrid laptop,
    /// where its loader starts every driver). MAYAG_GPU_WAIT_MS overrides;
    /// 0 restores CPU-first (draw now, switch later).
    [[nodiscard]] static int gpu_wait_budget_ms() {
        using backend::gpu_policy::Choice;
        const Choice device = backend::gpu_policy::prefers(Choice::vulkan) ? Choice::vulkan : Choice::d3d11;
        return static_cast<int>(std::min<long long>(backend::gpu_policy::first_frame_wait(device).count(), 1 << 30));
    }

    /// Take the prepared GPU and put it on the window. The first present
    /// waits (up to gpu_wait_budget_ms) so frame one is already a GPU frame;
    /// later presents only poll. Failure is not an error: no device, a
    /// software adapter under RDP, or MAYAG_BACKEND=software all draw on the
    /// CPU path.
    void adopt_gpu() {
        State* st = state_.get();
        if (!st->bringup.pending()) return;
        if (!st->shown) startup::mark(st->bringup.ready() ? "first present: gpu already prepared" : "first present: waiting for gpu");
        // The first present waits (woken the instant the device is ready:
        // Sleep(1) polling rounds every sleep up to the 15.6 ms timer tick);
        // later presents only poll.
        auto taken = st->shown ? st->bringup.poll() : st->bringup.first_frame(std::chrono::milliseconds{gpu_wait_budget_ms()});
        if (!taken) return;
        auto d = std::move(*taken);
        if (d && st->gpu_settings) {   // a prestarted device learns the window's wishes now
            d->set_low_latency(st->gpu_settings->low_latency);
            d->set_transparent(st->gpu_settings->transparent);
        }
        if (d && !d->attach_surface(st->hwnd, st->size, st->dpi)) d.reset();
        if (!d) {
            startup::mark("gpu unavailable (cpu)");
            st->gpu_failed = true;
            return;
        }
        st->gpu = std::move(d);
        st->gpu_active = true;
        // Anything uploaded before the GPU existed has to be replayed.
        images::replay(st->images, *st->gpu);
        st->had_frame = false;
        release_dib();
        st->fb = backend::Framebuffer{1, 1};
        startup::mark(st->shown ? "gpu ready (switched from cpu)" : "gpu ready (first frame)");
    }

    // ── DWM: native Windows 11 chrome ────────────────────────────────────
    //
    // dwmapi is loaded at runtime: everything here is cosmetic and must
    // degrade to a no-op on a build of Windows that lacks an attribute (the
    // call simply fails), and linking it would make dwmapi.dll a hard
    // dependency for the headless tests too.
    struct Dwm {
        HRESULT(WINAPI* set_attr)(HWND, DWORD, LPCVOID, DWORD) = nullptr;
        HRESULT(WINAPI* extend)(HWND, const void*) = nullptr;
        HRESULT(WINAPI* timing)(HWND, DWM_TIMING_INFO*) = nullptr;
        HRESULT(WINAPI* flush)() = nullptr;
    };
    [[nodiscard]] static const Dwm& dwm() {
        static const Dwm d = [] {
            Dwm r{};
            if (const HMODULE m = ::LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                r.set_attr = reinterpret_cast<decltype(r.set_attr)>(reinterpret_cast<void*>(::GetProcAddress(m, "DwmSetWindowAttribute")));
                r.extend = reinterpret_cast<decltype(r.extend)>(reinterpret_cast<void*>(::GetProcAddress(m, "DwmExtendFrameIntoClientArea")));
                r.timing = reinterpret_cast<decltype(r.timing)>(reinterpret_cast<void*>(::GetProcAddress(m, "DwmGetCompositionTimingInfo")));
                r.flush = reinterpret_cast<decltype(r.flush)>(reinterpret_cast<void*>(::GetProcAddress(m, "DwmFlush")));
            }
            return r;
        }();
        return d;
    }
    static void dwm_set(HWND h, DWORD attr, const void* v, DWORD n) {
        if (dwm().set_attr != nullptr) (void)dwm().set_attr(h, attr, v, n);
    }

    /// Dark title bar matching the background, Mica on Windows 11, rounded
    /// corners, and a caption coloured to the app so the frame reads as part
    /// of it. Each attribute fails harmlessly where unsupported.
    void apply_chrome(const WindowConfig& cfg) {
        const HWND h = state_->hwnd;
        // Perceived luminance decides light vs dark chrome.
        const auto lin = cfg.background;
        const float lum = 0.2126f * lin.c0 + 0.7152f * lin.c1 + 0.0722f * lin.c2;
        const BOOL dark = lum < 0.5f ? TRUE : FALSE;
        dwm_set(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
        const int round = 2 /*DWMWCP_ROUND*/;
        dwm_set(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof round);
        // Caption and border take the app's own background (COLORREF 0x00BBGGRR).
        const auto to8 = [](float c) { return static_cast<DWORD>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
        const DWORD bg = to8(lin.c0) | (to8(lin.c1) << 8) | (to8(lin.c2) << 16);
        dwm_set(h, 35 /*DWMWA_CAPTION_COLOR*/, &bg, sizeof bg);
        dwm_set(h, 34 /*DWMWA_BORDER_COLOR*/, &bg, sizeof bg);
        if (cfg.transparent) {
            // Mica behind a translucent client: extend the frame over the
            // whole client area, then ask for the system backdrop.
            const MARGINS m{-1, -1, -1, -1};
            if (dwm().extend != nullptr) (void)dwm().extend(h, &m);
            const int mica = 2 /*DWMSBT_MAINWINDOW*/;
            dwm_set(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof mica);
        }
        if (!cfg.decorated) {
            // Frameless but still a real window: keep the DWM shadow by
            // extending a 1px frame, so it looks native and snaps natively.
            const MARGINS m{0, 0, 1, 0};
            if (dwm().extend != nullptr) (void)dwm().extend(h, &m);
        }
    }

    /// The real refresh rate and a recent vblank, from DWM. The host paces
    /// frames against this, so a 144 Hz or 240 Hz monitor gets 144 or 240
    /// frames, not the 60 Hz default grid. Re-read when the window moves to
    /// another monitor.
    void refresh_timing() {
        State* st = state_.get();
        // The RATE comes from the window's own monitor mode: authoritative,
        // per-monitor (DWM's composition rate is the primary's), and cheap.
        std::chrono::nanoseconds period{0};
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (::GetMonitorInfoW(st->monitor, &mi) != FALSE) {
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) != FALSE &&
                dm.dmDisplayFrequency > 1)
                period = std::chrono::nanoseconds(1'000'000'000LL / dm.dmDisplayFrequency);
        }
        if (period.count() == 0) return;

        // The PHASE (a recent vblank) from DWM, when it will say.
        auto vblank = std::chrono::steady_clock::now();
        if (dwm().timing != nullptr) {
            DWM_TIMING_INFO info{};
            info.cbSize = sizeof info;
            if (SUCCEEDED(dwm().timing(nullptr, &info)) && info.qpcVBlank != 0) {
                LARGE_INTEGER f, now_q;
                ::QueryPerformanceFrequency(&f);
                ::QueryPerformanceCounter(&now_q);
                const auto now_tp = std::chrono::steady_clock::now();
                const auto now_u = static_cast<std::uint64_t>(now_q.QuadPart);
                // QPC and steady_clock may not share an epoch: convert by
                // offset from a reading of both. Windows 11's DWM often
                // reports the NEXT vblank (in the future) -- just as good a
                // phase. Treating it as "no report" (as this once did) laid
                // the frame grid at a random phase: draws aimed just before a
                // vblank landed just after it, and motion juddered.
                const auto to_ns = [&](std::uint64_t d) {
                    return std::chrono::nanoseconds(static_cast<std::int64_t>(
                        static_cast<long double>(d) * 1.0e9L / static_cast<long double>(f.QuadPart)));
                };
                // DWM's period is exact (60.006 Hz, not 60): take it when it is
                // this monitor's rate (DWM's is the primary's)
                if (info.qpcRefreshPeriod != 0) {
                    const auto dp = to_ns(info.qpcRefreshPeriod);
                    if (dp * 100 > period * 98 && dp * 100 < period * 102) period = dp;
                }
                if (now_u >= info.qpcVBlank) {
                    if (const auto ago = to_ns(now_u - info.qpcVBlank); ago < std::chrono::seconds(1)) vblank = now_tp - ago;
                } else if (const auto ahead = to_ns(info.qpcVBlank - now_u); ahead < std::chrono::seconds(1)) {
                    vblank = now_tp + ahead;
                }
            }
        }
        st->timing.vblank = vblank;
        st->timing.period = period;
        ++st->timing.seq;
    }

    /// The display clock from presentation feedback. A one-off DWM reading
    /// gives a phase that is stale the moment it is taken and a whole-Hz
    /// rate (60, for a 60.006 Hz panel: the grid slid ~4 ms a minute across
    /// the real vblank, so for stretches every late-latch draw missed its
    /// refresh and motion juddered). Each frame DXGI names the vblank our
    /// last shown present hit and the refresh count there: the true phase on
    /// the monitor the window is on, and -- across refreshes -- the true
    /// period. Chromium's vsync thread keeps its timebase the same way.
    void learn_vblank() {
        State* st = state_.get();
        LONGLONG q = 0;
        UINT n = 0;
        if (!st->gpu->last_shown_vblank(q, n) || (q == st->fb_qpc && n == st->fb_refresh)) return;
        LARGE_INTEGER f, now_q;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&now_q);
        const auto now_tp = std::chrono::steady_clock::now();
        const double per_tick = 1.0e9 / static_cast<double>(f.QuadPart);   // ns per QPC tick
        if (st->fb_refresh != 0 && n > st->fb_refresh && n - st->fb_refresh <= 600 && q > st->fb_qpc) {
            const double p = static_cast<double>(q - st->fb_qpc) * per_tick / static_cast<double>(n - st->fb_refresh);
            const double nominal = static_cast<double>(st->timing.period.count());
            if (nominal <= 0.0 || (p > nominal * 0.5 && p < nominal * 2.0))
                st->fb_period_ns = st->fb_period_ns > 0.0 ? st->fb_period_ns * 0.9 + p * 0.1 : p;
        }
        st->fb_qpc = q;
        st->fb_refresh = n;   // (ResizeBuffers restarts DXGI's counts: the guards above skip that sample)
        const auto d = std::chrono::nanoseconds(static_cast<std::int64_t>(static_cast<double>(now_q.QuadPart - q) * per_tick));
        if (d < std::chrono::seconds(1) && d > -std::chrono::seconds(1)) st->timing.vblank = now_tp - d;
        if (st->fb_period_ns > 0.0) st->timing.period = std::chrono::nanoseconds(static_cast<std::int64_t>(st->fb_period_ns));
        ++st->timing.seq;
    }

    /// Reveal the window the moment its first real frame is on screen: no
    /// white flash, no half-drawn first frame, no flicker of the default
    /// background while the swapchain spins up.
    void reveal() {
        State* st = state_.get();
        if (st->shown) return;
        st->shown = true;
        if (st->cloaked) {
            const BOOL off = FALSE;
            dwm_set(st->hwnd, 13 /*DWMWA_CLOAK*/, &off, sizeof off);
            st->cloaked = false;
        }
    }

    void sync_size() {
        if (!state_ || state_->hwnd == nullptr) return;
        RECT rc{};
        ::GetClientRect(state_->hwnd, &rc);
        const float inv = state_->dpi > 0.0f ? 1.0f / state_->dpi : 1.0f;
        state_->size = Vec2{static_cast<float>(rc.right - rc.left) * inv,
                            static_cast<float>(rc.bottom - rc.top) * inv};
    }

    /// A system metric at the window's own DPI (GetSystemMetricsForDpi),
    /// so frame bands are the right width on a HiDPI monitor.
    [[nodiscard]] static int get_metric(int idx, const State* st) {
        using Fn = int(WINAPI*)(int, UINT);
        static const Fn f = [] {
            const HMODULE u = ::GetModuleHandleW(L"user32.dll");
            return u ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(u, "GetSystemMetricsForDpi"))) : nullptr;
        }();
        const UINT dpi = static_cast<UINT>(st->dpi * 96.0f + 0.5f);
        return f != nullptr ? f(idx, dpi) : ::GetSystemMetrics(idx);
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

            // ── the modal loop ────────────────────────────────────────────────
            // A drag-resize or move runs Windows' own message loop inside
            // DefWindowProc; our loop is frozen until the mouse is released.
            // So during it, every size step renders from right here, and a
            // timer keeps animations (a blinking cursor, a spring) alive
            // while the window is merely being held or moved.
            case WM_ENTERSIZEMOVE:
                st->in_modal = true;
                if (st->live_draw) {
                    const UINT ms = static_cast<UINT>(
                        std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      st->timing.period.count() > 0 ? st->timing.period
                                                                                    : std::chrono::nanoseconds{16'666'667})
                                                      .count()));
                    st->modal_timer = ::SetTimer(hwnd, 0x6D61 /*'ma'*/, ms, nullptr);
                }
                return 0;
            case WM_EXITSIZEMOVE:
                st->in_modal = false;
                if (st->modal_timer != 0) { ::KillTimer(hwnd, st->modal_timer); st->modal_timer = 0; }
                // The window may have been dragged onto another monitor.
                st->monitor_changed = true;
                return 0;
            case WM_TIMER:
                if (wp == 0x6D61 && st->in_modal && st->live_draw) {
                    st->live_draw();
                    return 0;
                }
                break;

            case WM_SIZE: {
                if (wp == SIZE_MINIMIZED) {
                    // A zero-sized client: there is nothing to draw into, and
                    // a swapchain of 0x0 is invalid. Keep the old size, so the
                    // restore lands where it left off, and stay quiet.
                    st->minimized = true;
                    return 0;
                }
                st->minimized = false;
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const Vec2 sz{static_cast<float>(rc.right - rc.left) * inv,
                              static_cast<float>(rc.bottom - rc.top) * inv};
                if (sz == st->size && !st->in_modal) return 0;
                st->size = sz;
                st->resized = true;   // dispatch() rebuilds the swapchain
                push(ResizeEvent{st->size, st->dpi});
                if (st->in_modal && st->live_draw) {
                    // Inside the drag: dispatch() is not running, so rebuild
                    // and draw now. This is what makes resize live instead of
                    // a frozen frame that snaps on release.
                    if (st->on_resize) st->on_resize();
                    st->live_draw();
                }
                return 0;
            }

            case WM_GETMINMAXINFO: {
                // A floor on the size, so a drag can never produce a
                // degenerate (e.g. 1px) client that layouts cannot handle.
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = static_cast<LONG>(200.0f * st->dpi);
                mmi->ptMinTrackSize.y = static_cast<LONG>(120.0f * st->dpi);
                return 0;
            }

            case WM_DISPLAYCHANGE:
            case WM_MOVE:
                // A new mode or a new monitor: the refresh rate may differ.
                st->monitor_changed = true;
                break;

            // ── frameless, but still a native window ───────────────────────────
            case WM_NCCALCSIZE:
                if (!st->decorated && wp == TRUE) {
                    // Claim the whole window as client area, which removes
                    // the caption while keeping WS_THICKFRAME: Aero Snap,
                    // snap layouts, resize borders and the DWM shadow stay.
                    // When maximised, Windows positions the window past the
                    // monitor edge by the frame width; inset by it so no
                    // content is lost off-screen.
                    if (::IsZoomed(hwnd)) {
                        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                        const int fx = get_metric(SM_CXSIZEFRAME, st) + get_metric(SM_CXPADDEDBORDER, st);
                        const int fy = get_metric(SM_CYSIZEFRAME, st) + get_metric(SM_CXPADDEDBORDER, st);
                        p->rgrc[0].left += fx;
                        p->rgrc[0].right -= fx;
                        p->rgrc[0].top += fy;
                        p->rgrc[0].bottom -= fy;
                    }
                    return 0;
                }
                break;

            case WM_NCHITTEST:
                if (!st->decorated && st->resizable && !::IsZoomed(hwnd)) {
                    // Without a frame, resize handles have to be reported by
                    // hand: a band at each edge and corner. Everything else
                    // is client; the app moves the window itself (begin_move).
                    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    ::ScreenToClient(hwnd, &p);
                    RECT rc{};
                    ::GetClientRect(hwnd, &rc);
                    const int b = get_metric(SM_CXSIZEFRAME, st) + get_metric(SM_CXPADDEDBORDER, st);
                    const bool l = p.x < b, r = p.x >= rc.right - b, t = p.y < b, bo = p.y >= rc.bottom - b;
                    if (t && l) return HTTOPLEFT;
                    if (t && r) return HTTOPRIGHT;
                    if (bo && l) return HTBOTTOMLEFT;
                    if (bo && r) return HTBOTTOMRIGHT;
                    if (t) return HTTOP;
                    if (bo) return HTBOTTOM;
                    if (l) return HTLEFT;
                    if (r) return HTRIGHT;
                    return HTCLIENT;
                }
                break;

            case WM_NCACTIVATE:
                // Without a caption DefWindowProc would repaint the (absent)
                // non-client area on activation, flashing a stock frame.
                if (!st->decorated) return TRUE;
                break;

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
                st->monitor_changed = true;
                push(ResizeEvent{st->size, st->dpi});
                return 0;
            }

            case WM_PAINT: {
                // Validate, so the region is not reported dirty forever. No
                // event: a ResizeEvent per paint was a feedback loop (paint,
                // event, present, paint) pinning a core on an idle window.
                // Exception: the modal loop, where nobody else will draw.
                PAINTSTRUCT ps;
                const HDC pdc = ::BeginPaint(hwnd, &ps);
                // CPU path: re-show the last frame for the uncovered region.
                // DWM keeps the content of a composited window, so this runs
                // rarely -- but when it does, it must not show garbage.
                if (st->dib_dc != nullptr && pdc != nullptr)
                    ::BitBlt(pdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                             ps.rcPaint.bottom - ps.rcPaint.top, st->dib_dc, ps.rcPaint.left, ps.rcPaint.top,
                             SRCCOPY);
                ::EndPaint(hwnd, &ps);
                if (st->in_modal && st->live_draw) st->live_draw();
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
                // Fractional on purpose: a precision touchpad reports deltas
                // far smaller than WHEEL_DELTA, and rounding them to whole
                // notches is what makes cheap Windows apps scroll in jerks.
                const float ticks = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                const bool vertical = msg == WM_MOUSEWHEEL;
                // The user's lines-per-notch, read once and refreshed on
                // WM_SETTINGCHANGE rather than asked of the kernel per event.
                const UINT lines = vertical ? st->wheel_lines : st->wheel_chars;
                float px;
                if (lines == WHEEL_PAGESCROLL) {
                    px = ticks * (vertical ? st->size.y : st->size.x) * 0.9f;   // "one screen" per notch
                } else {
                    px = ticks * static_cast<float>(lines) * 16.0f;
                }
                const Vec2 d = vertical ? Vec2{0.0f, px} : Vec2{-px, 0.0f};
                push(ScrollEvent{d,
                                 Vec2{static_cast<float>(sp.x) * inv, static_cast<float>(sp.y) * inv},
                                 win32_detail::mods_now(), false});
                return 0;
            }

            case WM_SETTINGCHANGE:
                st->load_settings();
                break;

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

// What Win32Window can do, stated: checked both ways against what its methods
// give it (platform/concepts.hpp) -- losing one or gaining one unstated
// fails the build, naming the capability.
static_assert(check_capabilities<Win32Window, caps<cap::event_fd, cap::draws_live, cap::counts_skipped_frames,
                                                          cap::latches_late,
                                                          cap::reports_display_timing, cap::fullscreens, cap::manages_itself,
                                                          cap::uploads_images, cap::takes_text_input, cap::prestarts_gpu>>());

}  // namespace mayag::platform
