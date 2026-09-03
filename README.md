<h1 align="center">mayag</h1>

<p align="center">
  <b>A GPU UI framework for C++26.</b><br>
  Type-state DSL · perceptual colour as a type system · one SDF kernel, one draw call
</p>

<p align="center">
  <a href="#quickstart">Quickstart</a> ·
  <a href="#the-type-state-dsl">DSL</a> ·
  <a href="#colour-spaces-are-types">Colour</a> ·
  <a href="#one-shape-to-rule-them-all">Rendering</a> ·
  <a href="#building">Building</a>
</p>

---

- **Invalid UI does not compile.** `box() | border_color(red)` is a compile error that says *"it requires a border (add `| border(width, color)` first)"* — not a silent no-op you find three days later.
- **Colour spaces are types.** `Color<Srgb>` and `Color<Linear>` are different types. Alpha blending is only *defined* on `Linear`. You cannot gamma-blend by accident because there is no overload for it.
- **One shape kernel.** Buttons, cards, dividers, circles, rings, arcs, capsules, shadows, glyphs — every one is a signed distance field on the same instanced quad. A whole app frame is **1–3 draw calls**.
- **Analytic antialiasing.** No MSAA, no supersampling. The SDF has unit gradient, so coverage is exact at any zoom on any backend.
- **Runs everywhere, immediately.** A pure-CPU reference rasteriser and a built-in vector font ship in the box. No GPU, no font file, no dependencies — `git clone && cmake --build` renders a PNG.
- **Header-only.** `#include <mayag/mayag.hpp>`. No library to link, no ABI to match.

## Quickstart

```cpp
#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

int main() {
    constexpr Theme t = themes::midnight;

    constexpr auto card =
        v(text<"Deploy">     | font(20) | bold     | fg(t.text_primary),
          text<"3 services"> | font(13)            | fg(t.text_secondary),
          h(button<"Ship it">(t), badge<"staging">(t, t.warning)) | gap(8))
        | gap(6) | pad(20)
        | bg(t.surface)
        | border(1, t.border)
        | radius(14)
        | elevation(8);

    RenderOptions opts{.background = t.background,
                       .font = &fonts::Font::builtin_font()};
    return render_to_png(card, {320, 160}, "card.png", opts) ? 0 : 1;
}
```

`card` is a `constexpr` object. The entire style tree is folded at compile time; the only runtime work is layout, painting, and rasterising.

## The type-state DSL

Every element carries a **capability set in its type**. Every modifier declares what it *needs*, what it *grants*, and what it *forbids*. The pipe operator is the proof checker, and C++26 user-generated `static_assert` messages (P2741) turn a failed proof into a sentence.

```cpp
box() | border_color(colors::red)
// error: mayag: `| border_color` is not valid here — it requires a border
//        (add `| border(width, color)` first).

box() | bold
// error: mayag: `| weight` is not valid here — it requires a text element.

text<"hi"> | gap(8)
// error: mayag: `| gap` is not valid here — it requires a container
//        (built with v/h/z).

box() | absolute(10, 10) | grow()
// error: mayag: `| grow` conflicts with absolute or fixed positioning
//        which is already set on this element.
```

That last one is caught in **either order** — `grow` bans `positioned` and `absolute` bans `flexible`. Five of these are enforced as CI tests that *must fail to compile*; a DSL whose invalid states are only supposed to be unrepresentable is worth nothing.

### Writing your own

A modifier is any literal type satisfying the `Modifier` concept. Nothing built-in is privileged:

```cpp
struct Frosted {
    MAYAG_MODIFIER(Frosted, caps::container, caps::none, caps::none, "frosted");
    float amount;
    constexpr void apply(Style& s) const {
        s.backdrop = Backdrop{.blur = amount, .saturation = 1.8f};
        s.fill     = solid_fill(colors::white.fade(0.08f));
    }
};
constexpr auto frosted(float a) { return Frosted{a}; }

auto panel = v(...) | frosted(24);   // participates in every check
```

Widgets are the same story — `button`, `badge`, `toggle`, `card` are ordinary functions composing the public DSL, and they return elements whose capabilities are still live, so callers keep piping:

```cpp
button<"Deploy">(theme) | grow() | id<"deploy-btn">
```

## Colour spaces are types

```cpp
constexpr auto brand = rgb<0x5B8CFF>;      // Color<Srgb>

brand * 2.0f                               // compile error: not linear light
brand.to<Linear>() * 2.0f                  // fine

over(src.to<Linear>(), dst.to<Linear>())   // compositing: Linear only
mix_perceptual(a, b, 0.5f)                 // routes through Oklch
lighten(brand, 0.1f)                       // Oklch: hue & chroma stay put
contrast_ratio(fg, bg)                     // WCAG, from real luminance
```

Four spaces — `Srgb`, `Linear`, `Oklab`, `Oklch` — as distinct phantom-typed instantiations. `Linear` is the hub; conversions are explicit and constexpr, and the sRGB transfer function is the real piecewise curve (linear toe included), not `pow(x, 2.2)`.

### Themes are generated, and provably legible

Give `make_theme()` one accent colour; it derives surfaces, text, borders, and status colours in Oklch — then the palette is checked for accessibility **at compile time**:

```cpp
static_assert(contrast_ratio(themes::midnight.text_primary,
                             themes::midnight.background) >= 4.5f,
              "mayag theme: primary text fails WCAG AA against the background.");
```

This caught a real failure during development: `on_accent` picked black-or-white and landed at 3.16:1. The fix wasn't to loosen the assert — it was to make `on_accent` *solve* for the contrast target. Six stock themes now provably meet WCAG AA at every hue.

### Gradients, done right

Gradients interpolate in Oklch by default. Measured across a blue→orange ramp:

| position | Oklch chroma | sRGB chroma |
|----------|--------------|-------------|
| 10%      | 0.186        | 0.163       |
| 40%      | 0.192        | 0.110       |
| 55%      | 0.191        | 0.108       |
| 85%      | 0.192        | 0.148       |

sRGB sags 44% in the middle — that's the grey dead-zone everyone recognises. Oklch holds chroma flat. Opt out per-gradient with `| srgb_interpolation` when matching a legacy design.

## One shape to rule them all

Every primitive is a rounded-box SDF on the same 128-byte instance:

```cpp
struct Instance {
    Vec4 rect, radii, color, color2, axis, uv, params;
    uint32_t kind, flags, blend, texture_slot;
};
```

No path rasteriser. No tessellator. No stencil passes. No blur passes — a shadow is the same distance field, softened by an analytic Gaussian approximation. Consecutive instances sharing clip rect and texture merge automatically:

```
mayag_gallery.png  1720x1800
  nodes      117
  instances  1450
  draw calls 1        ← the entire gallery
```

The kernel lives in [`render/sdf.hpp`](include/mayag/render/sdf.hpp) as constexpr C++, and in [`render/shader_source.hpp`](include/mayag/render/shader_source.hpp) as GLSL/MSL/WGSL/HLSL — line-for-line translations, side by side, so a divergence is an obvious diff. The CPU rasteriser calls the C++ directly, which makes it the executable specification every GPU backend is tested against.

The vertex stage generates quads from `gl_VertexIndex` alone: no vertex buffer, no index buffer, no VAO churn. Just instances.

## Cross platform

| Platform | Backend |
|----------|---------|
| macOS, iOS | Metal |
| Linux, Android | Vulkan |
| Windows | D3D12 / Vulkan |
| Web | WebGPU |
| anything with a compiler | **software** (always) |

Selection is `MAYAG_BACKEND` env var → platform native → portable → software. A backend that fails to initialise falls through to the next, so a missing driver costs you frames, not a crash. The backend interface is four methods — `resize`, `submit`, `upload_texture`, `read_pixels` — and never sees a widget, a style, or a colour space.

## Building

Header-only, so you can just add `include/` to your path. With CMake:

```bash
cmake -B build -G Ninja
cmake --build build -j
ctest --test-dir build
```

```cmake
add_subdirectory(mayag)
target_link_libraries(my_app PRIVATE mayag::mayag)
```

**C++26** is used for P2741 `static_assert` messages. GCC 15+, Clang 20+. Apple Clang doesn't support it — use Homebrew GCC:

```bash
brew install gcc cmake ninja
cmake -B build -DCMAKE_CXX_COMPILER=g++-16
```

MSVC falls back to C++23; everything works, but DSL errors degrade to a generic message.

## Tests

```
PASS  103 checks, 0 failures     # numeric, layout, and PIXEL assertions
100% tests passed, 6 of 6        # including 5 must-not-compile cases
```

Three layers, matching the three ways this can be wrong: **numeric** (colour round-trips, SDF gradient magnitude, transfer curves), **layout** (flex arithmetic against hand-computed rects), and **pixel** (render a scene, sample actual pixels). The pixel layer is what catches "compiles, lays out, draws nothing" — and it caught three real bugs while this was being written:

- cross-axis default was `start` instead of CSS's `stretch`, collapsing grown children to zero width
- `srgb_interpolation` was silently ignored — gradients never used the flag
- the first fix used *Cartesian* Oklab, which desaturates worse than linear RGB; the answer is *polar* Oklch

All three are now regression tests.

## Layout

Real flexbox, ~400 lines, no Yoga dependency: `grow`/`shrink` with spec-correct weighting, six `justify` modes, five `align` modes, gap, padding, margin, min/max clamping applied where the spec says, absolute/fixed children resolved out of flow, and percentage lengths.

Layout depends on a narrow `TextMeasurer` interface, never a font backend — so it is testable with no fonts, deterministic across machines, and swapping in HarfBuzz later touches exactly one class.

## Project layout

- `core/` — constexpr math, geometry, phantom-typed colour
- `style/` — Style aggregate, generated themes
- `dsl/` — the type-state DSL and the widget library
- `layout/` — flexbox and the text-measurement seam
- `render/` — SDF kernel, draw list, painter, shader source
- `backend/` — backend registry and the software rasteriser
- `text/`, `image/` — built-in stroke font, dependency-free PNG encoder

## License

MIT
