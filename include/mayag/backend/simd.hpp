#pragma once
// mayag::backend::simd — portable vector primitives
//
// One 4-wide float type, mapped onto NEON on ARM and SSE2 on x86, with a
// scalar fallback that compiles everywhere. The rasteriser's inner loops are
// written once against this and get vectorised on every platform mayag
// targets.
//
// Deliberately minimal: only the operations the pixel loops actually use.
// A general-purpose SIMD wrapper would be larger, slower to compile, and no
// faster here.

#include "../core/math.hpp"

#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
    #define MAYAG_SIMD_NEON 1
    #include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define MAYAG_SIMD_SSE2 1
    #include <emmintrin.h>
    #if defined(__SSE4_1__)
        #include <smmintrin.h>
        #define MAYAG_SIMD_SSE41 1
    #endif
#else
    #define MAYAG_SIMD_SCALAR 1
#endif

namespace mayag::backend::simd {

[[nodiscard]] constexpr const char* backend_name() noexcept {
#if defined(MAYAG_SIMD_NEON)
    return "neon";
#elif defined(MAYAG_SIMD_SSE41)
    return "sse4.1";
#elif defined(MAYAG_SIMD_SSE2)
    return "sse2";
#else
    return "scalar";
#endif
}

/// Four packed floats.
struct f32x4 {
#if defined(MAYAG_SIMD_NEON)
    float32x4_t v;
    f32x4() = default;
    explicit f32x4(float32x4_t x) noexcept : v{x} {}
    explicit f32x4(float s) noexcept : v{vdupq_n_f32(s)} {}
    [[nodiscard]] static f32x4 load(const float* p) noexcept { return f32x4{vld1q_f32(p)}; }
    void store(float* p) const noexcept { vst1q_f32(p, v); }
    [[nodiscard]] float lane(int i) const noexcept {
        alignas(16) float tmp[4];
        vst1q_f32(tmp, v);
        return tmp[i];
    }
#elif defined(MAYAG_SIMD_SSE2)
    __m128 v;
    f32x4() = default;
    explicit f32x4(__m128 x) noexcept : v{x} {}
    explicit f32x4(float s) noexcept : v{_mm_set1_ps(s)} {}
    [[nodiscard]] static f32x4 load(const float* p) noexcept { return f32x4{_mm_loadu_ps(p)}; }
    void store(float* p) const noexcept { _mm_storeu_ps(p, v); }
    [[nodiscard]] float lane(int i) const noexcept {
        alignas(16) float tmp[4];
        _mm_store_ps(tmp, v);
        return tmp[i];
    }
#else
    float v[4];
    f32x4() = default;
    explicit f32x4(float s) noexcept : v{s, s, s, s} {}
    [[nodiscard]] static f32x4 load(const float* p) noexcept {
        f32x4 r;
        std::memcpy(r.v, p, sizeof(r.v));
        return r;
    }
    void store(float* p) const noexcept { std::memcpy(p, v, sizeof(v)); }
    [[nodiscard]] float lane(int i) const noexcept { return v[i]; }
#endif
};

// ── arithmetic ──────────────────────────────────────────────────────────

[[nodiscard]] inline f32x4 add(f32x4 a, f32x4 b) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vaddq_f32(a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_add_ps(a.v, b.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i];
    return r;
#endif
}

[[nodiscard]] inline f32x4 sub(f32x4 a, f32x4 b) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vsubq_f32(a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_sub_ps(a.v, b.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i];
    return r;
#endif
}

[[nodiscard]] inline f32x4 mul(f32x4 a, f32x4 b) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vmulq_f32(a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_mul_ps(a.v, b.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i];
    return r;
#endif
}

/// Fused multiply-add: a * b + c. Uses a real FMA where the ISA has one,
/// which is both faster and more accurate than the two-instruction form.
[[nodiscard]] inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vfmaq_f32(c.v, a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i];
    return r;
#endif
}

[[nodiscard]] inline f32x4 min(f32x4 a, f32x4 b) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vminq_f32(a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_min_ps(a.v, b.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
    return r;
#endif
}

[[nodiscard]] inline f32x4 max(f32x4 a, f32x4 b) noexcept {
#if defined(MAYAG_SIMD_NEON)
    return f32x4{vmaxq_f32(a.v, b.v)};
#elif defined(MAYAG_SIMD_SSE2)
    return f32x4{_mm_max_ps(a.v, b.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
    return r;
#endif
}

[[nodiscard]] inline f32x4 clamp01(f32x4 a) noexcept {
    return min(max(a, f32x4{0.0f}), f32x4{1.0f});
}

// ── conversion ──────────────────────────────────────────────────────────

/// Convert 4 floats in [0,1] to 4 bytes, with round-to-nearest.
inline void store_u8x4(f32x4 a, std::uint8_t* out) noexcept {
#if defined(MAYAG_SIMD_NEON)
    const float32x4_t scaled = vmlaq_f32(vdupq_n_f32(0.5f), a.v, vdupq_n_f32(255.0f));
    const uint32x4_t  u32    = vcvtq_u32_f32(scaled);
    const uint16x4_t  u16    = vmovn_u32(u32);
    const uint8x8_t   u8     = vmovn_u16(vcombine_u16(u16, u16));
    // Only the low 4 bytes are meaningful.
    std::uint32_t packed = vget_lane_u32(vreinterpret_u32_u8(u8), 0);
    std::memcpy(out, &packed, 4);
#elif defined(MAYAG_SIMD_SSE2)
    const __m128 scaled = _mm_add_ps(_mm_mul_ps(a.v, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
    const __m128i i32   = _mm_cvttps_epi32(scaled);
    const __m128i i16   = _mm_packs_epi32(i32, i32);
    const __m128i i8    = _mm_packus_epi16(i16, i16);
    std::uint32_t packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(i8));
    std::memcpy(out, &packed, 4);
#else
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>(num::saturate(a.v[i]) * 255.0f + 0.5f);
    }
#endif
}

// ── reductions ──────────────────────────────────────────────────────────

/// True when every lane is >= `t`. Used to detect fully-opaque spans.
[[nodiscard]] inline bool all_ge(f32x4 a, float t) noexcept {
#if defined(MAYAG_SIMD_NEON)
    const uint32x4_t m = vcgeq_f32(a.v, vdupq_n_f32(t));
    return vminvq_u32(m) != 0;
#elif defined(MAYAG_SIMD_SSE2)
    return _mm_movemask_ps(_mm_cmpge_ps(a.v, _mm_set1_ps(t))) == 0xF;
#else
    for (int i = 0; i < 4; ++i) if (a.v[i] < t) return false;
    return true;
#endif
}

/// True when every lane is <= `t`. Used to skip fully-transparent spans.
[[nodiscard]] inline bool all_le(f32x4 a, float t) noexcept {
#if defined(MAYAG_SIMD_NEON)
    const uint32x4_t m = vcleq_f32(a.v, vdupq_n_f32(t));
    return vminvq_u32(m) != 0;
#elif defined(MAYAG_SIMD_SSE2)
    return _mm_movemask_ps(_mm_cmple_ps(a.v, _mm_set1_ps(t))) == 0xF;
#else
    for (int i = 0; i < 4; ++i) if (a.v[i] > t) return false;
    return true;
#endif
}

}  // namespace mayag::backend::simd
