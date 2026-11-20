// tests/test_win32_text.cpp -- Windows text matches DirectWrite.
//
// On Windows mayag's default text look is the NATIVE one: glyph coverage
// from DirectWrite (font/dwrite_raster.hpp) composited with DirectWrite's
// own grayscale alpha correction (kernel::dwrite_text_alpha). This test is
// the contract for that: one line shaped ONCE by mayag, drawn twice at the
// same glyph positions -- by mayag (StackGlyphRenderer + Tiled, the exact
// path a window takes) and by Direct2D's DrawGlyphRun -- and compared per
// pixel. Layout is identical by construction, so any difference is
// rasterisation or compositing.
//
// Measured when written: mean 0.03/255, worst 2/255. The macOS look and the
// portable look were 2.2-2.3/255 mean with 6.9-7.0 % of channels off by more
// than 8/255, which is what "Windows text looks off" was.
//
// Also covers the bug that made that comparison impossible: the CPU
// rasteriser painted a bitmap glyph's edge columns a second time into its
// 1 px antialiasing skirt, so every glyph was a pixel too wide on each side.

#include <mayag/mayag.hpp>

#if defined(_WIN32)
#include <d2d1.h>
#include <wincodec.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mayag;

namespace {
int failures = 0, checks = 0;
void check(bool c, const std::string& what) {
    ++checks;
    if (!c) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
    else std::printf("  ok    %s\n", what.c_str());
}

#if defined(_WIN32)
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Result { double mean = 0, worst = 0, over8 = 0; bool ran = false; };

Result compare(typo::FontConfig cfg) {
    Result res;
    auto stack = typo::system::default_stack(cfg);
    if (!stack || stack->face_count() == 0 || stack->face_at(0)->dwrite_face() == nullptr) return res;

    struct Case { const char* s; float px; Color<Srgb> fg, bg; };
    const Case cases[] = {
        {"The quick brown fox jumps over the lazy dog 0123456789", 13, rgb<0xD8DEE9>, rgb<0x1E2128>},
        {"Hamburgefonstiv {}[]()<>=>!= ilI1|0O -> ::", 15, rgb<0xA3BE8C>, rgb<0x1E2128>},
        {"Dark text on a light panel at 12 px, as in a dialog", 12, rgb<0x24292F>, rgb<0xF6F8FA>},
    };
    const int W = 560, LH = 28, H = LH * static_cast<int>(std::size(cases));

    IWICImagingFactory* wic = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
        FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &rt))) {
        rel(rt); rel(d2d); rel(bmp); rel(wic);
        return res;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    mayag::detail::StackBindings bound{*stack};
    const auto& renderer = static_cast<const render::GlyphRenderer&>(bound.glyphs);
    DrawList dl;
    rt->BeginDraw();
    for (std::size_t li = 0; li < std::size(cases); ++li) {
        const auto& c = cases[li];
        const float top = static_cast<float>(li * LH);
        dl.fill_rect(Rect{{0, top}, {static_cast<float>(W), static_cast<float>(LH)}}, c.bg);
        ID2D1SolidColorBrush* bb = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(c.bg.c0, c.bg.c1, c.bg.c2), &bb);
        rt->FillRectangle(D2D1::RectF(0, top, static_cast<float>(W), top + LH), bb);
        rel(bb);

        TextStyle st; st.size = c.px; st.color = c.fg;
        renderer.draw_text(dl, c.s, Rect{{8, top}, {static_cast<float>(W - 16), static_cast<float>(LH)}}, st);

        // The same shaping and pens mayag used (draw_text: baseline = top +
        // ascent, pen quantised to 1/N px), handed to Direct2D.
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
            ids.push_back(g.glyph_id);
            off.push_back({q - start, -g.offset.y});
            adv.push_back(0.0f);
            pen += g.advance;
        }
        DWRITE_GLYPH_RUN gr{};
        gr.fontFace = stack->face_at(0)->dwrite_face();
        gr.fontEmSize = c.px;
        gr.glyphCount = static_cast<UINT32>(ids.size());
        gr.glyphIndices = ids.data(); gr.glyphAdvances = adv.data(); gr.glyphOffsets = off.data();
        ID2D1SolidColorBrush* fb = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(c.fg.c0, c.fg.c1, c.fg.c2), &fb);
        rt->DrawGlyphRun(D2D1::Point2F(start, baseline), &gr, fb, DWRITE_MEASURING_MODE_NATURAL);
        rel(fb);
    }
    rt->EndDraw();

    backend::Framebuffer fbuf{W, H};
    backend::Tiled::render(dl, fbuf, static_cast<const backend::CoverageSampler*>(&bound.sampler), nullptr, colors::black);
    const auto mine = fbuf.to_rgba8();
    std::vector<unsigned char> ref(static_cast<std::size_t>(W) * H * 4);
    bmp->CopyPixels(nullptr, W * 4, static_cast<UINT>(ref.size()), ref.data());
    rel(rt); rel(d2d); rel(bmp); rel(wic);

    double sum = 0; long n = 0, o8 = 0;
    for (std::size_t i = 0; i < ref.size(); i += 4)
        for (int ch = 0; ch < 3; ++ch) {
            // D2D's bitmap is BGRA, mayag's RGBA.
            const double d = std::abs(int(mine[i + ch]) - int(ref[i + 2 - ch]));
            sum += d; res.worst = std::max(res.worst, d); ++n;
            if (d > 8) ++o8;
        }
    res.mean = sum / n;
    res.over8 = 100.0 * o8 / n;
    res.ran = true;
    return res;
}
#endif
}  // namespace

int main() {
#if defined(_WIN32)
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("\nnative text vs Direct2D\n");
    const Result r = compare(typo::FontConfig::native());
    if (!r.ran) {
        std::printf("  (DirectWrite unavailable -- skipped)\n");
        return 0;
    }
    std::printf("  mean %.3f/255, worst %.0f/255, %.3f%% of channels off by >8\n", r.mean, r.worst, r.over8);
    check(r.mean < 0.25, "mayag's Windows text matches Direct2D on average (< 0.25/255)");
    check(r.worst <= 6, "and pixel by pixel (worst <= 6/255)");
    check(typo::FontConfig{}.native_raster, "the native look is the default on Windows");

    // Vertical metrics as DirectWrite reports them. The comparison above
    // takes its baseline from mayag's own metrics, so it cannot see this:
    // mayag used hhea where Windows uses usWin*, which put Consolas'
    // baseline 2.3 px high and every terminal line against the top of its row.
    std::printf("\nvertical metrics vs IDWriteFontFace::GetMetrics\n");
    const auto& db = typo::system::Database::instance();
    for (std::string_view fam : {"Consolas", "Cascadia Mono", "Cascadia Code", "Courier New", "Segoe UI",
                                 "Arial", "Lilex", "JetBrains Mono"}) {
        const auto* e = db.first_available(std::vector<std::string_view>{fam});
        if (e == nullptr || e->family != fam) continue;
        auto face = typo::Face::from_file(e->path, e->face_index);
        IDWriteFontFace* dw = face ? face->dwrite_face() : nullptr;
        if (dw == nullptr) continue;
        DWRITE_FONT_METRICS m{};
        dw->GetMetrics(&m);
        const float px = 13.0f, s = px / static_cast<float>(m.designUnitsPerEm);
        const auto mine = face->metrics(px);
        const float da = std::abs(mine.ascent - m.ascent * s), dd = std::abs(mine.descent - m.descent * s),
                    dg = std::abs(mine.line_gap - static_cast<float>(m.lineGap) * s);
        std::printf("  %-14.*s ascent %.2f/%.2f descent %.2f/%.2f gap %.2f/%.2f\n", static_cast<int>(fam.size()),
                    fam.data(), mine.ascent, m.ascent * s, mine.descent, m.descent * s, mine.line_gap,
                    static_cast<float>(m.lineGap) * s);
        check(da < 0.01f && dd < 0.01f && dg < 0.01f, std::string{fam} + ": ascent, descent and gap match DirectWrite");
    }
#else
    std::printf("  (not Windows -- skipped)\n");
#endif
    std::printf("\n%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
