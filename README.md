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
- **A real font engine, from scratch.** TrueType and CFF parsing, composite glyphs, kerning from `kern` and `GPOS`, per-codepoint script fallback, analytic-coverage rasterisation, and a Euclidean-SDF atlas. No FreeType, no HarfBuzz, no stb.
- **One shape kernel.** Buttons, cards, dividers, circles, rings, arcs, capsules, shadows, glyphs — every one is a signed distance field on the same instanced quad. A whole app frame is **1–3 draw calls**, text included.
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

## Running the examples

Every example is three things at once, from one `main()`:

```bash
./build/examples/mayag_gallery                  # a live window
./build/examples/mayag_gallery --png out.png    # one frame to a PNG
./build/examples/mayag_gallery --headless       # scripted + asserted (CI)
```

They run the *same* `Program` in all three modes, so a screenshot in this
README cannot drift from what the app actually does. All four are
interactive: click widgets, drag the slider, press <kbd>space</kbd> to
animate, <kbd>tab</kbd> to move focus, <kbd>esc</kbd> to quit.

| Example | What it shows |
|---------|---------------|
| `counter` | the smallest complete Program — Model/Msg/init/update/view/subscribe |
| `gallery` | every visual feature, 6 live themes, drag + hover + animation |
| `dashboard` | a realistic app layout with a clickable sidebar |
| `typography` | the font engine: type scale, kerning, script fallback |

An idle mayag app **blocks** and measures **~0% CPU** — "am I animating" is
derived from the subscriptions, not from a flag someone forgot to clear.

## Typography

mayag parses fonts itself. Not a wrapper — the OpenType tables, the glyph outlines, the kerning, and the rasteriser are all in `include/mayag/font/`.

```cpp
// Finds the platform UI font, a CJK face, and an emoji face, and chains them.
auto fonts = typo::system::default_stack();

RenderOptions opts{.fonts = fonts.get()};
render_to_png(page, {900, 880}, "specimen.png", opts);
```

| Layer | What it does |
|-------|--------------|
| `opentype.hpp` | sfnt + TTC, `cmap` 0/4/6/12, `head`/`hhea`/`maxp`/`hmtx`/`name`/`OS/2` |
| `outline.hpp` | `glyf` incl. composites, CFF/Type-2 charstrings, CID-keyed fonts |
| `raster.hpp` | analytic-coverage scanline fill, 8SSEDT Euclidean SDF |
| `atlas.hpp` | skyline packing, LRU eviction, dirty-rect upload |
| `shape.hpp` | UTF-8, clusters, `kern` + `GPOS` pair kerning, UAX-14 break rules |
| `system.hpp` | font discovery + per-codepoint fallback chains |

**Kerning is real.** Measured on Arial at 32 px:

| Pair | Kerned | Unkerned | Delta |
|------|--------|----------|-------|
| `To` | 33.80 | 37.34 | **−3.55** |
| `AV` | 40.31 | 42.69 | **−2.38** |
| `Wa` | 46.81 | 48.00 | **−1.19** |
| `ll` | 14.22 | 14.22 | 0.00 |

**Fallback is per codepoint, not per string.** `"CPU 温度 🔥"` draws from three files — and still lands in one atlas and one draw call.

**SDF mode shares one atlas entry across every size.** A page with six type sizes rasterises each glyph once, not six times. That is why the typography specimen renders in **1 draw call**:

```
mayag_typography.png  1800x1760
  nodes 61 · instances 650 · draw calls 1
  atlas 125 glyphs, 18.3% of 1024x1024
```

### What it does not do

No Arabic joining, no Indic reordering, no BiDi. Those are not "more of the same" — each is a distinct algorithm with a decade of conformance work behind it, and a half-implementation fails silently on text the developer cannot read. So mayag *detects* them (`ShapeResult::has_rtl`, `needs_complex`) and defines a `Shaper` interface for HarfBuzz to slot into. The atlas, SDF pipeline, and draw path are unchanged either way.

### Untrusted input

A font file arrives from a download or a user's disk. Every accessor is bounds-checked and returns a defined value when the file lies about its own structure. The test suite sweeps **all ~1300 faces installed on the machine** and feeds the parser **400 randomly corrupted fonts**; both must parse-or-reject without crashing, and both run clean under ASan and UBSan.

## Layout you cannot get wrong

CSS defaults `flex-shrink` to 1, which makes an explicit `width(196)` a
*suggestion*: put it beside a sibling with wide content and the sidebar
silently becomes 147px, its labels wrap one character per line, and the UI
looks broken in a way that points at the wrong file. mayag shipped exactly
that bug in its own dashboard. CSS authors have learned to type
`flex-shrink: 0` reflexively — a DSL that claims layout correctness should
not require a ritual to get the obvious behaviour.

So mayag changes the defaults and adds a safety net:

| Guarantee | Mechanism |
|-----------|-----------|
| A size you ask for is a size you get | `shrink` defaults to **0**, not 1 |
| Filling space implies giving way | `grow()` sets shrink too — `grow()` alone pushed a header to x=2976 in a 980px window |
| Text never becomes a vertical ribbon | below ~2.5 glyph widths, boxes overflow visibly instead of wrapping to 1 char/line |
| Opting out is explicit | `rigid`, `shrink(0)`, `grow(n, 0)` |

Some faults depend on runtime sizes and cannot be decided at compile time. For
those there is an auditor:

```cpp
auto issues = layout::audit(tree, &measurer);
std::printf("%s", layout::format_issues(issues).c_str());
// layout: 2 issue(s)
//   [870,40 6x16] text box too narrow to wrap sanely — width 6 cannot fit a glyph (9)
//   [236,560 704x8] children overflow their parent — by 15x0px
```

It reports collapsed nodes, overflow, unhonoured sizes, clipped text,
degenerate wrapping, and non-finite geometry — each located. Every shipped
example must audit clean, which is the test that would have caught the broken
dashboard before it reached a screenshot.

## The rasteriser

The software backend is the reference every GPU backend is measured against,
so it has to be both **correct** and **fast**. It is tiled, parallel, and
bit-identical to the scalar reference — that equivalence is asserted by tests
across 5 scenes x 2 scales, because a divergence that only shows up on some
core counts is the worst class of bug to chase.

| Stage | Technique |
|-------|-----------|
| Binning | instances bucketed into 64x64 tiles; a tile shades only what can touch it |
| Occlusion | an opaque tile-covering fill drops everything queued beneath it |
| Parallelism | dynamic work-stealing over tiles — UI tiles vary hugely in cost |
| Encode | linear->sRGB via a 4096-entry LUT, split across the same threads |

Measured on an M1 (8 cores), a realistic dashboard frame with shadows,
gradients and text:

| | serial | tiled | |
|---|---|---|---|
| 1x (0.9 MP) | 7.09 ms | **1.96 ms** | 3.6x — 511 fps |
| 2x Retina (3.6 MP) | 27.07 ms | **9.73 ms** | 2.8x — 103 fps |

That is why mayag renders at native Retina scale rather than upscaling a 1x
buffer. An idle window still blocks and measures **0% CPU**.

## Text quality

Three things beyond "rasterise the outline", each of which is visible:

- **Analytic coverage.** Every pixel gets the true area of the glyph inside
  it — not N supersamples. A coverage-conservation test asserts total area is
  within 0.2% for axis-aligned, offset, fractional, and sub-pixel-thin shapes.
- **Correct decode per glyph.** Bitmap entries are coverage; SDF entries are a
  distance field. Which one travels per *instance*, because hybrid mode mixes
  both in one frame. Getting this wrong snapped every antialiased pixel to
  0 or 1 and made text look eroded.
- **Stem darkening.** Linear-light compositing is physically right but
  perceptually wrong for text: a 50%-covered white-on-dark pixel lands ~0.19
  L* lighter than a human reads as "half", so light text looks thin. mayag
  applies a contrast-dependent gamma to *coverage* (never colour, so hue
  cannot shift) — the same correction FreeType and Skia make.

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
PASS  116 checks   # rendering: numeric kernels, layout, real pixels,
                   #            tiled == scalar reference, bit for bit
PASS   49 checks   # app runtime: interaction, effects, subscriptions
PASS 4940 checks   # fonts: 1300 system faces + 400 fuzzed files
100% tests passed, 12 of 12      # + 4 example apps, + 5 must-not-compile
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
- `font/` — the OpenType engine: parsing, outlines, raster, atlas, shaping
- `app/` — Program/Cmd/Sub, the runtime, interaction, the platform seam
- `text/`, `image/` — built-in stroke font, dependency-free PNG encoder

## License

MIT
