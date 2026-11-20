// The live GPU path vs Direct2D, in a real window.
//
// Opens a mayag Win32Window (Vulkan when available), draws three lines of
// text, reads the PRESENTED pixels back from the screen, and compares them
// with Direct2D drawing the same glyphs at the same positions. This is the
// end-to-end check for the GPU text pass (UNORM view + encoded blend), which
// the software-path test cannot see.
#include <mayag/mayag.hpp>
#include <mayag/app/platform.hpp>
#include <mayag/image/png.hpp>
#include <d2d1.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace mayag;

int main() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto stack = typo::system::default_stack();
    const int W = 560, LH = 28;
    struct Case { const char* s; float px; Color<Srgb> fg, bg; };
    const Case cases[] = {
        {"The quick brown fox jumps over the lazy dog 0123456789", 13, rgb<0xD8DEE9>, rgb<0x1E2128>},
        {"Hamburgefonstiv {}[]()<>=>!= ilI1|0O -> ::", 15, rgb<0xA3BE8C>, rgb<0x1E2128>},
        {"Dark text on a light panel at 12 px, as in a dialog", 12, rgb<0x24292F>, rgb<0xF6F8FA>},
    };
    const int H = LH * static_cast<int>(std::size(cases));

    platform::WindowConfig cfg{};
    cfg.size = Vec2{static_cast<float>(W), static_cast<float>(H)};
    cfg.title = "mayag text check";
    auto win = platform::Native::open(cfg);
    if (!win) return 1;
    const auto t0 = std::chrono::steady_clock::now();
    while (!win->gpu_active() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
        (void)win->dispatch();
        ::Sleep(5);
    }
    std::printf("renderer: %s\n", std::string(win->renderer_name()).c_str());

    mayag::detail::StackBindings bound{*stack};
    win->set_atlas_source(&stack->atlas());
    win->set_coverage_sampler(&bound.sampler);
    const auto& renderer = static_cast<const render::GlyphRenderer&>(bound.glyphs);
    DrawList dl;
    for (std::size_t li = 0; li < std::size(cases); ++li) {
        const auto& c = cases[li];
        const float top = static_cast<float>(li * LH);
        dl.fill_rect(Rect{{0, top}, {static_cast<float>(W), static_cast<float>(LH)}}, c.bg);
        TextStyle st; st.size = c.px; st.color = c.fg;
        renderer.draw_text(dl, c.s, Rect{{8, top}, {static_cast<float>(W - 16), static_cast<float>(LH)}}, st);
    }
    for (int i = 0; i < 30; ++i) { (void)win->dispatch(); win->present(dl, colors::black); ::Sleep(16); }

    // Grab the presented client area from the screen.
    HWND hwnd = ::FindWindowW(L"mayag.window", L"mayag text check");
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    for (int i = 0; i < 10; ++i) { (void)win->dispatch(); win->present(dl, colors::black); ::Sleep(16); }
    POINT o{0, 0}; ::ClientToScreen(hwnd, &o);
    HDC sdc = ::GetDC(nullptr), mdc = ::CreateCompatibleDC(sdc);
    HBITMAP hb = ::CreateCompatibleBitmap(sdc, W, H);
    ::SelectObject(mdc, hb);
    ::BitBlt(mdc, 0, 0, W, H, sdc, o.x, o.y, SRCCOPY);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> shot(static_cast<std::size_t>(W) * H * 4);
    ::GetDIBits(mdc, hb, 0, H, shot.data(), &bi, DIB_RGB_COLORS);   // BGRA

    // Direct2D reference, same glyphs at the same pens.
    IWICImagingFactory* wic = nullptr; IWICBitmap* bmp = nullptr; ID2D1Factory* d2d = nullptr; ID2D1RenderTarget* rt = nullptr;
    ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &rt);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    for (std::size_t li = 0; li < std::size(cases); ++li) {
        const auto& c = cases[li];
        const float top = static_cast<float>(li * LH);
        ID2D1SolidColorBrush* bb = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(c.bg.c0, c.bg.c1, c.bg.c2), &bb);
        rt->FillRectangle(D2D1::RectF(0, top, static_cast<float>(W), top + LH), bb);
        bb->Release();
        typo::ShapeParams sp; sp.size_px = c.px;
        const auto run = stack->shape(c.s, sp);
        const auto fm = stack->metrics(c.px, TextStyle{}.family);
        const float baseline = std::round(top + fm.ascent);
        const int phases = typo::FontStack::subpixel_phases_for(c.px);
        std::vector<UINT16> ids; std::vector<FLOAT> adv; std::vector<DWRITE_GLYPH_OFFSET> off;
        float pen = 8.0f, start = -1.0f;
        for (const auto& g : run.glyphs) {
            if (g.face_index != 0) { pen += g.advance; continue; }
            const auto split = typo::FontStack::split_pen(pen + g.offset.x, phases);
            const float q = split.pixel + static_cast<float>(split.phase) / static_cast<float>(phases);
            if (start < 0) start = q;
            ids.push_back(g.glyph_id); off.push_back({q - start, -g.offset.y}); adv.push_back(0.0f);
            pen += g.advance;
        }
        DWRITE_GLYPH_RUN gr{};
        gr.fontFace = stack->face_at(0)->dwrite_face(); gr.fontEmSize = c.px;
        gr.glyphCount = static_cast<UINT32>(ids.size()); gr.glyphIndices = ids.data();
        gr.glyphAdvances = adv.data(); gr.glyphOffsets = off.data();
        ID2D1SolidColorBrush* fb = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(c.fg.c0, c.fg.c1, c.fg.c2), &fb);
        rt->DrawGlyphRun(D2D1::Point2F(start, baseline), &gr, fb, DWRITE_MEASURING_MODE_NATURAL);
        fb->Release();
    }
    rt->EndDraw();
    std::vector<unsigned char> ref(shot.size());
    bmp->CopyPixels(nullptr, W * 4, static_cast<UINT>(ref.size()), ref.data());

    double sum = 0, worst = 0; long n = 0, o8 = 0;
    // The screen grab includes the window's 1 px border and the bottom row
    // the DWM frame overlaps; text never reaches them, so measure inside.
    for (int y = 2; y < H - 8; ++y)
        for (int x = 2; x < W - 2; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4 + ch;
                const double d = std::abs(int(shot[i]) - int(ref[i]));
                sum += d; worst = std::max(worst, d); ++n; if (d > 8) ++o8;
            }
    std::printf("live window (%s) vs Direct2D: mean %.3f/255, worst %.0f/255, %.3f%% channels off by >8\n",
                std::string(win->renderer_name()).c_str(), sum / n, worst, 100.0 * o8 / n);
    for (int y = 2; y < H - 8; ++y) {
        double rs = 0, rw = 0; int wx = -1;
        for (int x = 2; x < W - 2; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4 + ch;
                const double d = std::abs(int(shot[i]) - int(ref[i]));
                rs += d; if (d > rw) { rw = d; wx = x; }
            }
        if (rw > 8) std::printf("  row %2d: mean %.2f worst %.0f at x=%d (shot %u,%u,%u ref %u,%u,%u)\n", y, rs / (W * 3), rw, wx,
                                shot[(y * W + wx) * 4 + 2], shot[(y * W + wx) * 4 + 1], shot[(y * W + wx) * 4],
                                ref[(y * W + wx) * 4 + 2], ref[(y * W + wx) * 4 + 1], ref[(y * W + wx) * 4]);
    }
    for (std::size_t i = 0; i < shot.size(); i += 4) std::swap(shot[i], shot[i + 2]);
    (void)imageio::write_png("C:\\Develop\\Temp\\live_shot.png", shot, W, H);
}
