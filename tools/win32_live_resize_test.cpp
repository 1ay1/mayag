// Drives a REAL drag-resize of hand's window with synthesized mouse input and
// checks, while the button is still held (Windows' modal sizing loop running),
// that the newly exposed area was painted by the app rather than left
// black/stale. Prints: the colour of a reference pixel, the colour inside the
// newly exposed region, and the verdict.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static void mouse(DWORD flags, int x, int y) {
    const int sx = ::GetSystemMetrics(SM_CXVIRTUALSCREEN), sy = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int ox = ::GetSystemMetrics(SM_XVIRTUALSCREEN), oy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((x - ox) * 65535.0 / (sx - 1));
    in.mi.dy = static_cast<LONG>((y - oy) * 65535.0 / (sy - 1));
    in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ::SendInput(1, &in, sizeof in);
}

static COLORREF px(int x, int y) {
    const HDC dc = ::GetDC(nullptr);
    const COLORREF c = ::GetPixel(dc, x, y);
    ::ReleaseDC(nullptr, dc);
    return c;
}

int main(int argc, char** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const char* exe = argc > 1 ? argv[1] : "build\\hand.exe";
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string{"\""} + exe + "\"";
    if (!::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 1;
    ::Sleep(4000);

    HWND hwnd = nullptr;
    for (HWND h = ::FindWindowExW(nullptr, nullptr, L"mayag.window", nullptr); h;
         h = ::FindWindowExW(nullptr, h, L"mayag.window", nullptr)) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (pid == pi.dwProcessId) { hwnd = h; break; }
    }
    if (!hwnd) { std::printf("no window\n"); ::TerminateProcess(pi.hProcess, 0); return 1; }

    ::SetWindowPos(hwnd, HWND_TOPMOST, 200, 150, 700, 450, SWP_SHOWWINDOW);
    ::Sleep(800);
    RECT wr{}, cr{};
    ::GetWindowRect(hwnd, &wr);
    ::GetClientRect(hwnd, &cr);
    POINT c0{0, 0};
    ::ClientToScreen(hwnd, &c0);

    // Reference: a pixel well inside the existing client area, away from text.
    const COLORREF ref = px(c0.x + cr.right - 60, c0.y + cr.bottom - 30);

    // Grab the bottom-right resize corner (just inside the window rect).
    POINT p{wr.right - 3, wr.bottom - 3};
    POINT probe_screen = p;
    ::ScreenToClient(hwnd, &probe_screen);
    const LRESULT hit = ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
    mouse(MOUSEEVENTF_MOVE, p.x, p.y);
    ::Sleep(150);
    mouse(MOUSEEVENTF_LEFTDOWN, p.x, p.y);
    ::Sleep(150);
    const int dx = 360, dy = 240;
    for (int i = 1; i <= 24; ++i) {
        mouse(MOUSEEVENTF_MOVE, p.x + dx * i / 24, p.y + dy * i / 24);
        ::Sleep(16);
    }
    // Hold still (still inside the modal loop) and look at the new area.
    ::Sleep(400);
    RECT wr2{};
    ::GetWindowRect(hwnd, &wr2);
    const int nx = wr2.right - 80, ny = wr2.bottom - 60;   // inside the newly exposed region
    const COLORREF fresh = px(nx, ny);
    mouse(MOUSEEVENTF_LEFTUP, p.x + dx, p.y + dy);
    ::Sleep(300);

    const auto rgb = [](COLORREF c) {
        char b[32];
        std::snprintf(b, sizeof b, "#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        return std::string{b};
    };
    const auto close = [](COLORREF a, COLORREF b) {
        return std::abs(GetRValue(a) - GetRValue(b)) < 12 && std::abs(GetGValue(a) - GetGValue(b)) < 12 &&
               std::abs(GetBValue(a) - GetBValue(b)) < 12;
    };
    std::printf("hit-test at corner : %lld (17 = HTBOTTOMRIGHT)\n", static_cast<long long>(hit));
    std::printf("window grew        : %ldx%ld -> %ldx%ld\n", wr.right - wr.left, wr.bottom - wr.top,
                wr2.right - wr2.left, wr2.bottom - wr2.top);
    std::printf("reference pixel    : %s\n", rgb(ref).c_str());
    std::printf("new area, mid-drag : %s\n", rgb(fresh).c_str());
    std::printf("verdict            : %s\n",
                close(ref, fresh) ? "LIVE (painted during the drag)" : "STALE (not painted until release)");
    ::TerminateProcess(pi.hProcess, 0);
    return 0;
}
