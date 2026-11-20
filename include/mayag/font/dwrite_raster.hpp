#pragma once
// mayag/font/dwrite_raster.hpp — native Windows glyph rasterisation.
//
// On Windows, "correct" text is DirectWrite's: every native app, the shell,
// the browser and Windows Terminal draw through it, so that is the look a
// Windows user's eye is calibrated to. mayag's own rasteriser reproduces
// CoreGraphics (its default everywhere) or a plain unhinted outline
// (FontConfig::portable()); measured against DirectWrite on Segoe UI 13 px,
// those come out 17-23 % heavier and 11-15 % lighter respectively, and
// neither applies the font's hinting or DirectWrite's vertical
// anti-aliasing and contrast.
//
// So on Windows the coverage for an outline glyph is asked of DirectWrite
// itself: IDWriteGlyphRunAnalysis with the system's rendering parameters
// (natural symmetric rendering -- the same mode Windows' own UI uses at
// these sizes -- grid-fit vertically, and the user's configured contrast,
// read via IDWriteRenderingParams), in grayscale so the result composites
// on any background and any GPU path. The same face file mayag parsed is
// handed to DirectWrite by path and index, so shaping, metrics and layout
// stay mayag's -- only the pixels of each glyph change.
//
// Everything is optional. A face DirectWrite cannot open (a synthesized
// last-resort face, a font loaded from memory), a variable face away from
// its default instance, a synthetic italic or bold, or a machine without
// dwrite.dll returns nullopt, and the caller rasterises the outline itself
// as before.

#if defined(_WIN32)

#include "../core/win32.hpp"
#include "../core/geometry.hpp"
#include "raster.hpp"

#include <dwrite_2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace mayag::typo::dwrite {

/// One rasterised glyph: an 8-bit coverage bitmap and where its top-left
/// sits relative to the pen, in device px, y DOWN (the atlas convention).
struct Glyph {
    Bitmap bitmap;
    Vec2   offset{};
};

namespace detail {

template <class T> void release(T*& p) noexcept { if (p != nullptr) { p->Release(); p = nullptr; } }

/// The process-wide factory and the user's rendering parameters, loaded
/// once. dwrite.dll is loaded at runtime: linking it would make it a hard
/// dependency of every mayag program, headless tools included.
struct Runtime {
    IDWriteFactory2* factory = nullptr;
    IDWriteRenderingParams* params = nullptr;   ///< the user's system params
    float gamma = 1.8f;
    float enhanced_contrast = 0.5f;
    float grayscale_contrast = 1.0f;
    bool  ok = false;

    static Runtime& get() {
        static Runtime r = [] {
            Runtime rt;
            const HMODULE m = ::LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m == nullptr) return rt;
            using Create = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
            const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(::GetProcAddress(m, "DWriteCreateFactory")));
            if (create == nullptr) return rt;
            IUnknown* u = nullptr;
            if (FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), &u)) || u == nullptr) return rt;
            rt.factory = static_cast<IDWriteFactory2*>(u);
            // The user's text tuning (ClearType Text Tuner writes these):
            // gamma, enhanced contrast, grayscale enhanced contrast.
            IDWriteRenderingParams* p = nullptr;
            if (SUCCEEDED(rt.factory->CreateRenderingParams(&p)) && p != nullptr) {
                rt.gamma = p->GetGamma();
                rt.enhanced_contrast = p->GetEnhancedContrast();
                IDWriteRenderingParams1* p1 = nullptr;
                if (SUCCEEDED(p->QueryInterface(__uuidof(IDWriteRenderingParams1), reinterpret_cast<void**>(&p1))) && p1) {
                    rt.grayscale_contrast = p1->GetGrayscaleEnhancedContrast();
                    p1->Release();
                }
                rt.params = p;   // kept for GetRecommendedRenderingMode
            }
            rt.ok = true;
            return rt;
        }();
        return r;
    }
};

[[nodiscard]] inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// DirectWrite's grayscale text correction, as Microsoft ships it for GPU
/// renderers (Windows Terminal, src/renderer/atlas/dwrite_helpers.{cpp,hlsl},
/// MIT): the four "gamma ratio" constants for the user's gamma, from the
/// table DirectWrite uses internally. Verified here against Direct2D's own
/// grayscale text on 6612 edge pixels: 0.9/255 RMS, worst 2/255.
[[nodiscard]] inline std::array<float, 4> gamma_ratios(float gamma) noexcept {
    static constexpr float t[13][4]{
        {0.0000f, 0.0000f, 0.0000f, 0.0000f},   {0.0166f, -0.0807f, 0.2227f, -0.0751f},
        {0.0350f, -0.1760f, 0.4325f, -0.1370f}, {0.0543f, -0.2821f, 0.6302f, -0.1876f},
        {0.0739f, -0.3963f, 0.8167f, -0.2287f}, {0.0933f, -0.5161f, 0.9926f, -0.2616f},
        {0.1121f, -0.6395f, 1.1588f, -0.2877f}, {0.1300f, -0.7649f, 1.3159f, -0.3080f},
        {0.1469f, -0.8911f, 1.4644f, -0.3234f}, {0.1627f, -1.0170f, 1.6051f, -0.3347f},
        {0.1773f, -1.1420f, 1.7385f, -0.3426f}, {0.1908f, -1.2652f, 1.8650f, -0.3476f},
        {0.2031f, -1.3864f, 1.9851f, -0.3501f},
    };
    const float norm13 = static_cast<float>(static_cast<double>(0x10000) / (255 * 255) * 4) / 4.0f;
    const float norm24 = static_cast<float>(static_cast<double>(0x100) / 255 * 4) / 4.0f;
    const int i = std::clamp(static_cast<int>(gamma * 10.0f + 0.5f), 10, 22) - 10;
    return {norm13 * t[i][0], norm24 * t[i][1], norm13 * t[i][2], norm24 * t[i][3]};
}

}  // namespace detail

/// The user's text rendering parameters, for the composite: the gamma
/// ratios and grayscale enhanced contrast DirectWrite would apply. Read
/// once (Windows does not expect apps to refresh them after startup).
struct TextParams {
    std::array<float, 4> gamma_ratios{};
    float grayscale_contrast = 1.0f;
};
[[nodiscard]] inline TextParams text_params() {
    auto& rt = detail::Runtime::get();
    return {detail::gamma_ratios(rt.gamma), rt.grayscale_contrast};
}

/// Open a face file with DirectWrite. The caller owns the returned
/// reference (Release it) and caches it: mayag's Face holds one per face.
[[nodiscard]] inline IDWriteFontFace* open_face(const std::string& path, std::uint32_t index) {
    auto& rt = detail::Runtime::get();
    if (!rt.ok || path.empty()) return nullptr;
    const std::wstring wpath = detail::widen(path);
    IDWriteFontFile* file = nullptr;
    if (FAILED(rt.factory->CreateFontFileReference(wpath.c_str(), nullptr, &file)) || file == nullptr) return nullptr;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE ft{};
    DWRITE_FONT_FACE_TYPE fft{};
    UINT32 nfaces = 0;
    if (FAILED(file->Analyze(&supported, &ft, &fft, &nfaces)) || !supported || index >= nfaces) {
        file->Release();
        return nullptr;
    }
    IDWriteFontFace* f = nullptr;
    const HRESULT hr = rt.factory->CreateFontFace(fft, 1, &file, index, DWRITE_FONT_SIMULATIONS_NONE, &f);
    file->Release();
    return SUCCEEDED(hr) ? f : nullptr;
}

/// Rasterise one glyph through DirectWrite.
///
///   face        DirectWrite face (open_face)
///   gid         glyph id (shared with mayag: same file, same index)
///   size_px     em size in device pixels
///   x_phase     the pen's sub-pixel offset in [0, 1), baked into the raster
///               exactly as mayag's own path does
[[nodiscard]] inline std::optional<Glyph> rasterize(IDWriteFontFace* face, std::uint16_t gid, float size_px,
                                                    float x_phase) {
    auto& rt = detail::Runtime::get();
    if (!rt.ok || face == nullptr || !(size_px > 0.0f)) return std::nullopt;

    const UINT16 g = gid;
    const FLOAT adv = 0.0f;
    const DWRITE_GLYPH_OFFSET off{0.0f, 0.0f};
    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = size_px;
    run.glyphCount = 1;
    run.glyphIndices = &g;
    run.glyphAdvances = &adv;
    run.glyphOffsets = &off;

    // The rendering mode DirectWrite itself picks for this face and size
    // (GetRecommendedRenderingMode with the user's params) -- NATURAL at
    // UI sizes on Segoe UI, i.e. vertical grid-fitting with horizontal
    // anti-aliasing. Measured: with the same mode as D2D, DWrite's coverage
    // lines up with D2D's pixels exactly (232/232 interior, 1739/1739
    // empty); with NATURAL_SYMMETRIC it was off by a column in places.
    // Grayscale, because coverage is composited later onto arbitrary
    // backgrounds by either backend; per-channel ClearType would need
    // dual-source blending end to end and is wrong under transparency.
    DWRITE_RENDERING_MODE mode = DWRITE_RENDERING_MODE_NATURAL;
    if (IDWriteRenderingParams* p = rt.params) {
        DWRITE_RENDERING_MODE rec{};
        if (SUCCEEDED(face->GetRecommendedRenderingMode(size_px, 1.0f, DWRITE_MEASURING_MODE_NATURAL, p, &rec)))
            mode = rec;
    }
    // Outline mode (large sizes) has no alpha texture; render those as
    // natural-symmetric, which is what outline rendering approximates.
    if (mode == DWRITE_RENDERING_MODE_OUTLINE || mode == DWRITE_RENDERING_MODE_DEFAULT)
        mode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    if (mode == DWRITE_RENDERING_MODE_ALIASED) mode = DWRITE_RENDERING_MODE_NATURAL;
    const DWRITE_MATRIX m{1.0f, 0.0f, 0.0f, 1.0f, x_phase, 0.0f};
    IDWriteGlyphRunAnalysis* a = nullptr;
    if (FAILED(rt.factory->CreateGlyphRunAnalysis(&run, &m, mode,
                                                  DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
                                                  DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &a)) ||
        a == nullptr)
        return std::nullopt;

    RECT r{};
    if (FAILED(a->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &r))) { a->Release(); return std::nullopt; }
    Glyph out;
    if (r.right <= r.left || r.bottom <= r.top) {   // blank glyph (space)
        a->Release();
        return out;
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    out.bitmap = Bitmap{w, h};
    if (FAILED(a->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &r, out.bitmap.pixels.data(),
                                     static_cast<UINT32>(out.bitmap.pixels.size())))) {
        a->Release();
        return std::nullopt;
    }
    a->Release();
    // RAW coverage: the gamma/contrast correction is applied at composite
    // time (kernel::dwrite_text_alpha), because it depends on the text
    // colour -- exactly as DirectWrite's own blend does.
    // The pen is at (0, 0), baseline y = 0, y down: the texture rect is
    // already in that frame. Drop the baked sub-pixel phase from x, since
    // the caller positions the bitmap at the integer pixel.
    out.offset = Vec2{static_cast<float>(r.left), static_cast<float>(r.top)};
    return out;
}

}  // namespace mayag::typo::dwrite

#endif  // _WIN32
