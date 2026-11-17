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
- **A real font engine, from scratch.** TrueType, CFF and CFF2 parsing, colour/bitmap faces (CBDT/sbix/COLR), composite glyphs, a full OpenType shaping stack (GSUB/GPOS, every complex script, UAX #9 bidi) that matches HarfBuzz output exactly, per-codepoint script fallback across 30+ scripts, analytic-coverage rasterisation, and a Euclidean-SDF atlas. No FreeType, no HarfBuzz, no ICU, no stb, no fontconfig.
- **One shape kernel.** Buttons, cards, dividers, circles, rings, arcs, capsules, shadows, glyphs — every one is a signed distance field on the same instanced quad. A whole app frame is **1–3 draw calls**, text included.
- **Analytic antialiasing.** No MSAA, no supersampling. The SDF has unit gradient, so coverage is exact at any zoom on any backend.
- **Runs everywhere, immediately.** A pure-CPU reference rasteriser ships in the box, font discovery finds the platform's typefaces with no configuration, and a synthesized last-resort face keeps text legible even on a machine with no fonts at all. `git clone && cmake --build` renders a PNG.
- **Header-only.** `#include <mayag/mayag.hpp>`. The only compiled piece is the [jaal](https://github.com/1ay1/jaal) runtime, a git submodule built with mayag.

## Quickstart

```cpp
#include <mayag/mayag.hpp>            // the framework: DSL, styling, layout, rendering, runtime
#include <mayag/widget/button.hpp>    // widgets are opt-in, one header each
#include <mayag/widget/badge.hpp>

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

### Use it in your project

mayag is header-only on top of jaal, its runtime (`third_party/jaal`, a git
submodule; clone with `--recursive` or run `git submodule update --init`).
Point CMake at an installed copy and link the interface target, which brings
jaal with it:

```cmake
find_package(mayag REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE mayag::mayag)
```

or vendor it directly with no install step:

```cmake
add_subdirectory(third_party/mayag)   # or FetchContent_Declare(mayag ...)
target_link_libraries(myapp PRIVATE mayag::mayag)
```

The smallest complete live app is [`examples/hello.cpp`](examples/hello.cpp) (~110 lines): copy it, change the `Model` and the `update` cases, and you have a working animated app. `./myapp` opens a window; `./myapp --headless` runs its self-test with no display.

## The type-state DSL

Every element carries a **capability set in its type**. Every modifier declares what it *needs*, what it *grants*, and what it *forbids*. The pipe operator is the proof checker, and C++26 user-generated `static_assert` messages (P2741) turn a failed proof into a sentence.

```cpp
box() | border_color(colors::red)
// error: mayag: `| border_color` is not valid here — it requires a border
//        (add `| border(width, color)` first).

box() | bold
// error: mayag: `| weight` is not valid here — it requires a text element,
//        or a container with children (text properties set on a container
//        are inherited by the text inside it).

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

They live in `include/mayag/widget/`, one per header, and **`<mayag/mayag.hpp>` includes none of them** (as in maya). Include the ones you use, or `<mayag/widget/all.hpp>` while sketching. Nothing outside that folder is a widget or depends on one; a test enforces both.

### Events where they happen

An element says what it does next to what it is. You don't need an `id<>`, an `on(Clicked)` router or string matching:

```cpp
h(text<"Save">) | button_look | on_click(Save{})
slider(c, "vol", m.vol, 120)  | on_slide([](Vec2 p) { return SetVol{p.x / 120}; })
row | on_click([i](const Clicked& c) { return c.count == 2 ? Msg{Open{i}} : Msg{Select{i}}; })
card | on_enter(Peek{i}) | on_leave(Unpeek{})
```

- **What a handler can be:** `on_click`, `on_press`, `on_release`, `on_drag`, `on_scroll`, `on_enter`, `on_leave` and `on_slide` take either a message or a function of the gesture. The function can return a message, a `std::optional` of one (empty means nothing is sent), or `Msg` itself. The host matches the result to your `Msg` by type and folds it on the same turn as the gesture.
- **Handlers make controls:** an element with a handler gets a stable id, so it hovers and takes Tab focus, and Enter/Space trigger its `on_click`. Click handlers also show the pointer cursor.
- **Two-way binding for forms:** add `Edit<Model>` to `Msg` once, with `update(m, Edit<Model> e) { e(m); }`, then:

  ```cpp
  switch_control(c, "wifi", m.wifi) | on_click(toggle(&Model::wifi))
  slider(c, "vol", m.vol, 120)      | on_slide(set_from_x(&Model::vol, 120))
  h(text<"Reset">)                 | on_click(set(&Model::vol, 0.5f))
  ```

  An edit is still a message, so replay, undo and vetoes keep working.
- **Tests read like the UI:** `rt.click_text("Save")`.
- **Cost:** a handler adds about 95 ns per element per frame.

### Data in the tree

Any child of `v`, `h` or `z` can be an element, a runtime `Node`, a `std::vector` of either, or one of two fragments, mixed freely:

```cpp
v(text<"Inbox"> | bold,
  each(m.mails, [](const Mail& x, std::size_t i) {
      return h(text_of(x.from), spacer(), textf("{} min", x.age)) | id_of("mail", i);
  }),
  when(m.mails.empty(), text<"Nothing here"> | fg(var::text_disabled)),
  sidebar(m) | width(220))      // a helper returning Node pipes directly
```

`each` and `when` are *spliced*: the rows are the stack's own children, so its `gap`, `wrap` and `justify` apply across them. `when(c, a, b)` picks one; `when(c, a)` leaves nothing at all when false, so no empty box and no stray gap. `textf` is `std::format` that owns its bytes. Inside `z(...)`, every node after the first floats, including each node of a fragment.

## Styling: CSS that type-checks

A view describes how an element looks in **every** state. The cascade
(`style/cascade.hpp`) works out which state it is in every frame, between
view and layout, so `view()` never asks "am I hovered?":

```cpp
constexpr auto button = styles(
    pad(9, 16), radius(10), bg(var::surface_raised), fg(var::text_primary),
    hovered(bg(var::tint(var::accent, 0.22f))),
    pressed(bg(var::accent), fg(var::on_accent), scale(0.96f)),
    focus_visible(border(2, var::focus_ring)),     // rings for Tab, not for clicks
    when(state::disabled, opacity(0.4f)),
    transition());                                  // spring between them

h(text<"Save">) | button | id<"save">
```

| CSS | mayag |
|---|---|
| `.btn { ... }` | `constexpr auto btn = styles(...)`, which compose: `styles(btn, bg(...))` |
| `:hover` `:active` `:focus-visible` `:focus-within` | `hovered(...)` `pressed(...)` `focus_visible(...)` `focus_within(...)` |
| `[aria-selected]:hover`, `:not(...)` | `when(state::selected && state::hover, ...)`, `when(!state::selected, ...)` |
| `.card:hover .chevron` | `group()` on the card, `group_hovered(...)` on the chevron |
| `@media (max-width: 720px)` | `when(state::narrower(720), direction(Axis::vertical))` |
| `prefers-color-scheme` | `when(state::dark, ...)` / `when(state::light, ...)` |
| `color` / `font-*` inheritance | `v(...) \| fg(c) \| font(13)` styles all text inside |
| `var(--accent)`, `--accent: red` on a subtree | `bg(var::accent)`, `vars(var::accent = rgb<0xF7768E>)` |
| `transition`, `@starting-style` | `transition()` (springs), `starting(opacity(0), translate(0, 12))` |
| `transform: scale() / translate()` | `scale(0.96f)`, `translate(0, 12)`: moves paint, not layout |

**What it keeps from CSS** is the vocabulary. **What it drops** is the part that hurts:

- **No global selectors.** A style applies to the element it is piped onto. Nothing at a distance restyles your node.
- **No specificity arithmetic.** Rules resolve by a *fixed* ranking: environment < group < app states (selected/checked/expanded/flags) < hover < focus-within < focus < focus-visible < pressed < disabled < starting. Within a rank, the selector with more conditions wins, then the later declaration. Write `pressed(...)` before `hovered(...)` and pressed still wins.
- **No OR.** `!(a && b)` is a compile error; two rules read better than one clever one.
- **Everything is type-checked.** A variant is checked against its element like any modifier, so `box() | hovered(border_color(red))` fails to compile because there is no border to recolour. A class is checked member by member, in order.

**Tokens bind late.** `var::accent` travels through the tree as a token and resolves *where it lands*. A theme switch restyles every token without rebuilding the view, and a `vars(...)` island (an inverted card, a danger zone) recolours even the text it *inherits*. CSS fixes `var()` where it is declared, so an island there has to restate its text colour. An unbound `var::named("brand")` renders magenta on purpose: a missing definition should be impossible to miss.

**The theme can be application state.** If the program declares `static Theme theme(const Model&)`, a theme switch is an ordinary `update()`. Tokens, `ctx().theme` and the window's clear colour all follow it on the same frame.

**Hover works without ids.** A node with pointer variants but no `id<>` gets a stable, structural, *hover-only* id. It can light up, but it is never a Tab stop or a click target, so a hover-styled card never swallows the click meant for the button inside it. Hover propagates to ancestors the way `:hover` does.

**Motion is springs.** `transition()` interpolates colours in Oklab, plus geometry, opacity and transform. It is critically damped by default; pass `Spring::bouncy()` for overshoot. A hover that ends half-way reverses from where it is, with its velocity. Feedback starts on the frame the input lands, and when everything settles the app goes back to 0% CPU.

**See all of it in one place:** [`examples/stylebook.cpp`](examples/stylebook.cpp) is nine cards, one feature each: inheritance, states, token islands, groups, switches, disclosure, fields, entry motion and breakpoints. Every look is a constexpr class, and its `--headless` run checks every feature end to end through the real host. [`examples/styled.cpp`](examples/styled.cpp) is the same system applied to an ordinary app.

## Themes are the whole look

A `Theme` is one plain value that owns **everything** about how an app looks
and feels. Each part is a token, resolved at render time like `var::accent`:

| Part | Theme fields | Use in a style |
|---|---|---|
| colour | 18 roles + `on_*` labels | `bg(var::surface)`, `fg(var::on_accent)` |
| shape | `radius_small/medium/large`, `line_*` | `radius(var::radius_md)`, `border(var::line, var::border)` |
| space | `space_unit` | `pad(var::space(1.5f), var::space(2))`, `gap(var::space(1))` |
| type | `font_size` | `font(var::font_sm)` … `font(var::font_2xl)` |
| depth | `depth_scale` (0 = flat) | `elevation(var::depth(6))` |
| motion | `motion_ui / emphasis / gentle`, `motion_scale` (0 = reduced motion) | `transition()`, `transition(var::motion_emphasis)` |
| widgets | `sheet`: every widget's look | `Sheet::button`, `::card`, `::track`, `::slider_knob`, … |

Change it at whatever level you need:

```cpp
// a whole look from a few knobs (the palette is still solved for contrast)
Theme t = derive({.accent = rgb<0xFF6B35>, .mode = Mode::light,
                  .roundness = 0, .density = 0.8f, .depth = 0, .motion = 1.4f});

t.radius_medium = 3;                       // any single token
t.motion_ui     = Spring::bouncy();

static const Sheet mine = [] {             // any widget's whole look
    Sheet s = Sheet::standard();
    s.primary = look(pad(10, 18), radius(0), bg(var::warning), border(2, var::text_primary),
                     shadow(0, var::text_primary, {4, 4}),
                     hovered(translate(-2, -2)), pressed(translate(4, 4)),
                     transition(var::motion_emphasis));
    return s;
}();
t.sheet = &mine;

v(settings(m)) | theme(themes::daylight)   // one subtree, everything
button<"Delete">(t) | bg(var::danger)      // one instance: pipes beat the look
```

- **Theme changes morph.** When `static Theme theme(const Model&)` returns something new, the host blends the old theme into the new one on a spring. That covers colours (in Oklab), radii, spacing, type size, depth and springs, and every widget, including those that read the theme directly. It's a pure function of the model, so there is no animation code in the app, and it is pixel-exact at rest. It is instant when the new theme's `motion_scale` is 0. Turn it off with `AppConfig::theme_morph = false`.
- **`themed(t, [&] { ... })` builds any subtree in another theme.** Every widget inside draws in `t`, and every token, sheet and spring below resolves against it. It's the one call that makes "this part looks different" true for everything.
- **Stock looks, not just palettes.** `looks::brutal()`, `looks::glass()` and `looks::terminal()` swap every widget's *shape* (`sheets::brutal / glass / terminal`). Each takes a `Seed`, so `looks::glass({.accent = brand})` keeps the shape and changes the colour.
- **Where the theme comes from.** Per app: `AppConfig::theme`. As state: `static Theme theme(const Model&)`, which makes switching an ordinary `update()`; transitioned elements crossfade and morph shape. Per subtree: `| theme(t)`.
- **Widgets are drawn in the theme you hand them.** If that isn't the frame's theme, the widget scopes its subtree to it.
- **Stock looks.** `themes::square`, `soft` and `compact` are complete looks, not just palettes; `midnight`, `daylight` and the rest are still there. `lerp(a, b, t)` blends two themes, shape and motion included. `validate(t)` / `legible(t)` check text contrast, and every stock theme is `static_assert`ed legible.

**Every widget follows the theme, not just the restyled ones.** Widgets with a sheet look (button, card, panel, glass, switch, slider) take their whole shape from it. Every other widget writes its geometry through the theme's knobs: `t.rounded(px)` for corners, `t.spaced(px)` for padding and gaps, `t.stroke(px)` for borders, `t.lift(dp)` for elevation and `t.capsule(h)` for pill ends. Its colours are `var::` roles. In the standard theme those are exactly the old pixels; in a square, dense, flat or soft theme they reshape. `tests/test_theme.cpp` renders every widget in all eight stock looks and checks two things: every label stays legible on the fill behind it, and roundness 0 makes every widget square.

**Fast.** Tokens are float/colour sentinels, so elements stay `constexpr` and each modifier takes them with no new overloads. The cascade finds tokens with one branch-free `min()` per node and skips nodes without them, at about 50 ns/node all-in. A widget's look is compiled once per sheet: attaching it copies the fields it set and shares its rule set by refcount, so a themed widget allocates no rules of its own. 2,000 themed buttons build in about 2.8 ms and resolve in 0.4 ms; `tests/test_theme.cpp` keeps both within a budget. Stylebook card 10 cycles all five looks live and shows `| theme(...)` islands side by side.

**See it: [`examples/studio.cpp`](examples/studio.cpp)**, *mayag studio*. A live theme designer sits beside a real app: six looks, eight knobs, Remix, and every drag morphs the whole interface, including the designer's own controls. The bottom strip renders the same app state in three worlds at once (brutal, glass, terminal), and Play in any of them drives the one model. The app code never mentions a colour or a size.

### Correctness fixes that came with it

Making every widget follow every theme turned up bugs in older code. All are fixed and covered by tests:

- **Inner shadows** painted a dark slab *around* their box. They now stay clipped inside the shape, with real offset and spread (kernel, MSL and regenerated GLSL/SPIR-V).
- **`node(x) | modifier`** replaced `x`'s whole style, so a helper-returned card lost its fill, padding and radius the moment a caller sized it. It now modifies `x`.
- **Example self-checks that print `FAIL`** used to exit 0. `run_headless` now returns the driver's verdict.
- **Variable fonts** misread `gvar` phantom points, corrupting glyphs such as `N` at heavy weights. **Font discovery** treated CFF2-only faces (macOS's SFIndia) as drawable, which left five Indic scripts with no face. The test suite now selects fonts by behaviour, and all 49 test binaries pass on macOS.
- **The tiled renderer** blurred frosted glass per 64 px tile, which looked like a mosaic.
- **Text with no colour** reads in the theme's `text_primary` inside themed subtrees and light themes, not literal white.
- **The whole tree builds warning-free** with GCC `-Werror` and clang `-Wall -Wextra -Wshadow -Wconversion`.

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

No path rasteriser. No tessellator. No stencil passes. No blur passes — a shadow is the same distance field, softened by an analytic Gaussian approximation. The one primitive that *does* read pixels is **backdrop blur** (`backdrop_blur(radius)` — frosted glass): it blurs whatever the framebuffer already holds behind a panel, with optional saturation and brightness, clipped to the panel's rounded rect. The software rasteriser does it inline (instances draw in order, so the background is already composited when the backdrop is reached); the GPU splits the frame at the backdrop — draw the background, snapshot it to a texture, then draw the panel with a shader that blurs the snapshot — and the two agree pixel-for-pixel on the equivalence test. Consecutive instances sharing clip rect and texture merge automatically:

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
| `hello` | ~110 lines: the smallest live animated app — update overloads, on(Clicked), on_frame(), a spring |
| `counter` | the smallest complete Program — Model/Msg/init/update/view/subscribe |
| `gallery` | every visual feature, 6 live themes, drag + hover + animation |
| `dashboard` | a realistic app layout with a clickable sidebar |
| `typography` | the font engine: type scale, kerning, script fallback |
| `todo` | a real app: text input, virtualised list, overlays, springs |
| `reactive` | 60 Hz continuous input with its own latency budget on screen |

An idle mayag app **blocks** and measures **~0% CPU** — "am I animating" is
derived from the subscriptions, not from a flag someone forgot to clear.

## Typography

mayag parses fonts itself. Not a wrapper — the OpenType tables, the glyph outlines, the kerning, the discovery, and the rasteriser are all in `include/mayag/font/`.

```cpp
// Discovers the platform UI font and chains a covering face for EVERY script
// the machine has a font for — plus colour emoji — so nothing renders as tofu.
auto fonts = typo::system::default_stack();

RenderOptions opts{.fonts = fonts};
render_to_png(page, {900, 880}, "specimen.png", opts);
```

| Layer | What it does |
|-------|--------------|
| `opentype.hpp` | sfnt + TTC, `cmap` 0/4/6/12, `head`/`hhea`/`maxp`/`hmtx`/`name`/`OS/2`, colour/bitmap tables |
| `outline.hpp` | `glyf` incl. composites, CFF/Type-2 charstrings, CID-keyed fonts |
| `raster.hpp` | analytic-coverage scanline fill, 8SSEDT Euclidean SDF |
| `atlas.hpp` | skyline packing, LRU eviction, dirty-rect upload |
| `shape.hpp` | UTF-8, clusters, `kern` + variable `GPOS` pair kerning, `trak` tracking, UAX-14 break rules |
| `system.hpp` | font discovery + a complete per-script fallback chain |
| `last_resort.hpp` | a TrueType face synthesized in memory — the always-available floor |

### Discovery is state of the art, and needs no built-in font

mayag used to ship a second, hardcoded stroke font as the "always works"
fallback. It has been **deleted**. Discovery is now good enough that it is never
needed — and on the pathological font-less machine, a *real* TrueType face is
synthesized in memory and fed through the same engine, so there is exactly one
text path instead of two.

Three things make discovery never fall to tofu:

- **Colour and bitmap faces load.** An emoji font (`NotoColorEmoji`, Apple
  Color Emoji) has no `glyf`/`CFF ` table at all — only `CBDT`/`sbix`/`COLR`
  strikes — and the parser used to reject it outright, so emoji was tofu even
  though the font was installed. It now parses, and its cmap defines real
  coverage.
- **Coverage is a full script bitset, not five booleans.** Every face is
  probed against a representative codepoint for **30+ scripts** — Devanagari,
  Hebrew, Thai, Hangul, Greek, Armenian, Ethiopic and the rest — at scan time.
  The old model knew only Latin/Cyrillic/Arabic/CJK/emoji, so everything else
  fell straight through.
- **The default stack covers everything installed.** `default_stack()` walks
  every script and adds the best covering face — the most *specialised* one at
  a regular weight, so a dedicated Devanagari face beats a 44k-glyph
  pan-Unicode fallback that merely includes it. If a codepoint has any font on
  the machine, it is in the chain.

Measured on a desktop with 816 faces: `default_stack()` assembles a **32-face
chain** and resolves Latin, CJK, Cyrillic, Arabic, Devanagari, Hebrew, Thai,
Korean and emoji with **zero tofu**.

### Colour emoji, decoded from scratch

Loading a colour font is only half of it — the glyphs still have to *render in
colour*. mayag does, with no FreeType and no libpng. An emoji font stores each
glyph as a PNG (Google's `CBDT`/`CBLC`, Apple's `sbix`), so the engine:

1. **inflates the PNG itself** — `image/png_decode.hpp` is a from-scratch
   DEFLATE decoder (stored, fixed- and dynamic-Huffman blocks) plus the five
   PNG scanline filters, verified byte-exact against a reference decoder;
2. **parses `CBLC`/`CBDT` and `sbix`** to locate a glyph's strike and read its
   PNG bytes and placement metrics;
3. **packs the RGBA into a colour plane** of the same atlas as coverage glyphs,
   so a 😀 is cached, packed and evicted through the one glyph path; and
4. **blends it verbatim** — a colour glyph is a distinct draw kind that samples
   the RGBA atlas and takes its pixels as-is, tinted only by the run's opacity,
   never by the text colour. `CPU 温度 🔥` renders every character, and the fire
   is orange.

Both renderers draw colour emoji: the software rasteriser blends the RGBA plane
per pixel, and the Vulkan backend uploads it as a second atlas texture the
shader samples — verified against the software output on the GPU
equivalence test.

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

### Every script, shaped like HarfBuzz shapes it

mayag carries its own OpenType shaping stack, a rule-for-rule port of
HarfBuzz 11.2.1, and it is held to HarfBuzz's output exactly:

- **Bidi:** UAX #9 in full (explicit embeddings, isolates, bracket pairs, L1/L2).
  It passes **770,241 / 770,241** of `BidiTest.txt` and **91,707 / 91,707** of
  `BidiCharacterTest.txt` (Unicode 18).
- **OpenType layout:** GSUB lookup types 1–8 and GPOS types 1–9, including
  context and chaining, mark-to-base/ligature/mark, cursive attachment,
  FeatureVariations, and GDEF variation deltas at the face's instance.
- **Script shapers:** Arabic and Syriac joining (with stretching), Hebrew
  presentation forms, Thai and Lao (Sara Am), Hangul jamo composition, all
  nine Indic scripts with v2 and old-spec tags, Khmer, Myanmar, and the
  Universal Shaping Engine for about 60 more scripts (Balinese, Javanese,
  Tibetan, Mongolian, Sinhala...).
- **The pipeline around them:** normalisation that decomposes only as far as
  the font needs, mark reordering, right-to-left mirroring, fractions, dotted
  circles for broken clusters, fallback mark positioning for fonts without
  GPOS, and HarfBuzz's GDEF blocklist.
- **CFF2 outlines** with `blend`/`vsindex`: SFIndia's 8,883 glyphs match
  HarfBuzz's outlines exactly.
- **Itemisation:** each string splits into (bidi level, script, face) runs.
  For complex scripts the fallback chain prefers faces that carry OpenType
  layout for that script. Glyphs are stored in logical order, and each line
  is painted in visual order.

`tests/test_shaping.cpp` replays **500 hb-shape outputs** over about 200 system
fonts and every script family. All 500 match, glyph for glyph and font unit
for font unit. A fuzzer has shaped 100,000 strings on corrupted GSUB/GPOS/GDEF
tables under ASan and UBSan with no failures. Shaping runs at about 290
ns/char for Latin and 470 ns/char for Devanagari, and results are cached per
string.

The one thing left to HarfBuzz-on-macOS is Apple's `morx` (AAT) tables. Fonts
that have only `morx` are passed over for complex scripts in favour of an
OpenType face for the same script.

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
| Labels degrade, never ribbon | text defaults to **ellipsis**; paragraphs opt in with `wrap_text` |
| Stacks size to their content | `z()` keeps its FIRST child in flow, so it cannot collapse |
| Widgets measure, never guess | `Ctx::measurer()` is available in `view()`; no hardcoded `font_size * 1.4` |
| A size is specified once | a second `width()`/`height()` on one element is a compile error, not last-one-wins |
| Text cannot dangle | `text_of()` on a temporary `std::string` is a compile error; use `text_owned()` |

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
degenerate wrapping, dead `grow()`, impossible constraints (`min > max`), and
non-finite geometry — each located. Every shipped example must audit clean.

The defaults are chosen so the *dangerous* option requires opting in. Four
bugs shipped in mayag's own examples before this rule was applied, all the
same shape — an API whose default was the risky choice, so being correct meant
remembering to opt out. Each fix flipped a default rather than patching a call
site:

| Was | Now |
|-----|-----|
| text defaulted to `wrap` | defaults to `ellipsis`; a squeezed label truncates instead of becoming a vertical ribbon |
| `z()` made every child absolute | first child stays in flow and sizes the stack |
| widgets hardcoded `font_size * 1.4` | `Ctx::measurer()` gives views the real metrics |
| absolute children escaped the overflow check | a *clipping* parent crops them, so they are checked |

Between them, the compile-time bans and the auditor found **four real bugs in
mayag's own examples**: a root element sized twice, swatches whose declared
`size(92, 60)` was silently overridden, cell content overflowing its column,
and a `text_of(std::to_string(x) + "%")` holding three NUL bytes.

## State that belongs to you

Scroll offsets and text-field contents are **application state**, not hidden
properties of a widget. They live in your `Model`, they serialise, they can be
restored, and only `update()` changes them.

```cpp
struct Model {
    TextEditState input;   // text, caret, selection, undo-able by you
    ScrollState   list;    // offset; limits filled in by layout
};

// view — pure, takes the model by const reference
text_field(t, m.input, c.focused(node_id("input")), node_id("input"))
list(Axis::vertical, std::move(rows)) | scroll(m.list) | id<"list">

// update
m.input.handle_key(k.key, k.mods);   // the whole standard keymap, once
m.list.scroll_by(delta);
```

`TextEditState` moves in **graphemes** while storing **byte** offsets, so one
press of Left never splits a multi-byte character and backspace never corrupts
an emoji. `handle_key` implements word motion, shift-selection, Home/End,
select-all and the platform's primary modifier in one place — so every field
in every mayag app behaves the same instead of each author re-deriving it.

`ScrollState` records its own limits during layout, clamps on content change
(so deleting rows cannot leave you scrolled into blank space), and gives you
thumb geometry for free. Wheel events bubble to the nearest scrollable
ancestor, because the cursor is nearly always over a leaf.

**Kinetic scrolling** is built in and, like animation, it is pure state. A
flick calls `m.list.fling(release_velocity)`; the runtime then calls
`m.list.step(dt)` each frame while `m.list.coasting()`, and the offset
decelerates under exponential friction — the long, smooth tail-off iOS uses —
settling on its own so the frame subscription ends and the app falls back to
0% CPU. Hitting an edge spends the momentum instead of fighting the clamp, a
fresh grab `halt()`s a coasting list, and a tap-sized flick is ignored rather
than turned into drift. Because the velocity lives in the model, a scroll in
flight is saved, restored, and replayed like any other state.

Turn on `overscroll` and the edges **rubber-band**: dragging past an edge pulls
the content with diminishing resistance (each pixel of gesture moves it less,
asymptoting at a soft ceiling), and it springs back on release under a
critically-damped spring — a hard fling into the edge overshoots and bounces
exactly back, the iOS/macOS feel. It is off by default, because a desktop list
usually wants the hard stop; it is one flag for a touch surface.

## Writing a live app

mayag runs on [jaal](https://github.com/1ay1/jaal) (`third_party/jaal`, a git
submodule). jaal is the runtime: the loop, `update`, `Cmd`, `Sub`, timers,
background tasks, streams, faults and the OS waiting. mayag is a jaal host
that owns the window and draws. Apps only ever see mayag. An app is a struct with a
`view()`:

```cpp
struct Counter {
    struct Model { int count = 0; Animated<float> pulse{0.0f}; };
    struct Inc {}; struct Tick { float dt; }; struct Quit {};
    using Msg = std::variant<Inc, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;    // the window effects it uses
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"counter"}; }
    static Cmd update(Model& m, Inc)    { ++m.count; m.pulse.snap(1); m.pulse.to(0); return {}; }
    static Cmd update(Model& m, Tick t) { m.pulse.step(t.dt); return {}; }
    static Cmd update(Model&,   Quit)   { return Cmd::quit(); }

    static Node view(const Model& m) { const Theme& t = ctx().theme; /* ... */ }

    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (is(c, "inc")) return Inc{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{Key::escape, Quit{}}};
        return keys(k);
    }
    static Sub subscribe(const Model& m) {
        return when(m.pulse.animating(), Sub::on_frame([](float dt) { return Tick{dt}; }));
    }
};
int main() { return mayag::run<Counter>(); }
```

One `update` overload per message; the compiler names the case you forgot to handle.
Input is one `on()` per event kind: raw events (`KeyEvent`, `TextEvent`,
`MouseMove`, `ResizeEvent`, `CloseRequest`, ...) and gestures the host has
already hit-tested against the laid-out tree (`Clicked`, `Activated`,
`Pressed`, `Released`, `Entered`, `Left`, `Dragged`, `Scrolled`), each naming
its node. `view()` reads the window (size, theme, hover, focus) through
`ctx()`.

For a dynamic list, `id_of(base, index)` folds the row index into the id with
no string building, and `node_id(base, index)` gives the same id in `on()`:

```cpp
std::vector<Node> rows;
for (int i = 0; i < (int)m.items.size(); ++i)
    rows.push_back((row(m.items[i]) | id_of("row", i)).build());
return v_list(std::move(rows), 8.0f);
```

`Sub::on_frame` is the frame clock: one tick per refresh while `subscribe()`
returns it, none otherwise, so a settled app sleeps in the OS at 0% CPU.
Background work is `Cmd::task`, `Cmd::perform` and `Sub::stream`. The
whole thing is testable without a window: `run_headless` drives clicks and
asserts on the model. See [`examples/hello.cpp`](examples/hello.cpp) for the
smallest complete app.

## Overlays, virtualisation, animation

Three more things every real app needs, each solved once rather than by every
author:

**Overlays** are a separate layer, not a flag on a node. A menu, modal or
tooltip must escape its parent's clip, paint above every sibling, and capture
input — none of which flow layout can provide. They are posted from `view()`
and laid out *after* the main tree, so an overlay can position against an
anchor whose rect is not known until layout finishes:

```cpp
c.overlay(Overlay{.content = menu, .anchor_id = row_id,
                  .placement = Placement::below_start});
```

Placement flips when there is no room below and slides back on screen at the
edges. Clicking outside dismisses *and consumes* the click, so a menu never
both closes and activates what was behind it.

**Virtualised lists** build only what is visible:

```
      100 rows -> 26 nodes  layout 0.007 ms
    10000 rows -> 26 nodes  layout 0.003 ms
  1000000 rows -> 26 nodes  layout 0.003 ms
```

Constant cost, because the node count depends on the viewport rather than the
data. The todo example holds 412 items in 74 nodes.

**Animation follows real geometry.** The failure this prevents: an app
animating a magic number that approximates a layout it cannot see. mayag's own
todo example slid a filter underline with `indicator * 52.0f`, guessing the
chips were 52px apart and 46px wide — they were at x=293/333/385 with widths
34/51/47, so it drifted further wrong with every tab.

```cpp
struct Model { Tracked underline; };

// view — name the node to follow; the runtime observes its REAL rect
c.track(m.underline, node_id("filter-active"));
box() | size(m.underline.rect().width(), 2)
      | absolute(m.underline.rect().left(), m.underline.rect().bottom() + 2)
```

The spring chases a measured rect, so it cannot drift: change the font, the
label, or the window size and the animation follows. It also animates the
WIDTH, which a stride constant fundamentally cannot.

The runtime steps tracked motion and requests frames itself — `update()` has
no animation arm at all — and folds the request into the same "when do I next
wake" decision it makes for timers, so an app can neither forget to start nor
forget to stop.

**Springs, not curves.** `Animated<T>` is a value that knows
where it is going; `update()` advances it, `view()` reads it:

```cpp
m.indicator.to(target);        // in update()
m.indicator.step(dt);
box() | offset(m.indicator.value(), 0)   // in view()
```

Springs carry velocity across retargets, so a user toggling a panel mid-flight
gets continuous motion rather than a restart — the reason iOS and Android both
moved away from duration tweens. Integration is sub-stepped at a fixed 1/240 s,
which makes the motion identical at 30, 120 or any other frame rate (measured:
`delta=0.0000`) and stable across a 250 ms hitch that would otherwise make a
stiff spring explode.

An animation that has settled stops requesting frames, so the app returns to
0% CPU with no explicit stop call.

## Latency

"Fast" is not a claim, it is a number. mayag measures its own frame budget and
exposes it, so an app can draw it and CI can assert on it.

Measured on an M1, a continuously animating 60 Hz scene:

```
budget over 202 frames: mean 0.568 ms, p99 0.748 ms, worst 0.800 ms, missed 0
```

That is **3% of a 60 Hz frame**, which is the point: CPU work is not what
costs a user latency. The budget breaks down as

| Stage | Time |
|-------|------|
| input | 0.002 ms |
| view | 0.089 ms |
| layout | 0.027 ms |
| paint | 0.068 ms |
| raster + encode | 0.83 ms |
| **CPU total** | **~1 ms** |
| compositor | **up to 16.7 ms** |

The dominant term is the compositor, and no amount of CPU optimisation touches
it. So the things that actually reduce latency are about scheduling:

- **Coalescing.** Every pending event is processed before ONE render, and
  superseded pointer moves are dropped. A trackpad delivers ~120 moves/second;
  rendering each is double the work for the same visible result, *and* it feels
  slower because the frame you see is several events stale. Measured: 60 moves
  → 1 frame, 59 dropped.
- **Never blocking with a repaint owed**, so an event reaches the screen on the
  very next tick.
- **Requesting frames only while something moves**, so an idle app is at 0%.

```cpp
c.latency->mean_cpu_ms();          // draw your own budget
c.latency->percentile_cpu_ms(0.99);
c.latency->missed_frames();        // the number that actually matters
```

The 99th percentile and worst frame are reported alongside the mean, because a
UI that renders in 2 ms with an occasional 30 ms hitch feels *worse* than one
steadily taking 8 ms — the hitch is what a user notices.

`examples/reactive.cpp` draws all of this while you interact with it.

### Apps must idle

The commonest way a UI framework produces something users call **"hung"** is
not a deadlock — it is an app that renders continuously and pins a core. The
window responds and events flow, so it is much harder to notice than a
deadlock, and mayag's own reactive demo shipped exactly that bug.

So the runtime detects it and says something actionable:

```
mayag: rendering continuously at 60 fps for several seconds.
       Something keeps asking for frames: an on_frame subscription
       that is always on, or a spring that never settles.
       An idle mayag app should sit at 0% CPU.
```

Measured on every shipped example, after startup settles:

| | idle CPU |
|--|--|
| todo, gallery, dashboard, typography, counter, **reactive** | **0.0%** |

An animation earns a *budget* of frames rather than an open-ended
subscription — the reactive demo's cursor trail wakes for 45 frames and then
lets the app fall asleep again on its own.

### Live, and still idle

An idle app blocking at 0% CPU is only half the story. A *live* app —
a dashboard, a monitor, a chat, a log tail — receives data from somewhere
that is **not** the window: a socket, a subprocess, a metrics poller. The
producer runs on its own thread, and the classic bug is that it posts an
update while the UI thread is asleep in `poll()` watching only the display
fd — so the frame sits unrendered until the user happens to move the mouse,
and the app looks frozen. The usual escape is a polling timer, which trades
the freeze for a permanently-awake core.

mayag does neither. A producer is a `Sub::stream`, returned from
`subscribe()`, so it runs exactly as long as the model says it should.
"Connect the socket while logged in, close it on logout" is one line, with no
`connect()`/`disconnect()` in `update` and no thread leaked when the flag
flips:

```cpp
static Sub subscribe(const Model& m) {
    return when(m.streaming, Sub::stream("cpu", [](mayag::Sink<Msg> out, std::stop_token st) {
        CpuSampler s;
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(100ms);
            out.send(Sample{s.read()});
        }
    }));
}
```

The body captures nothing; anything it needs is passed as an argument after
it, so it can't hold a reference into the model. Each send wakes the
reactor, which is the same epoll/kqueue wait that watches the window, so the
message is folded and drawn at once. When the stream is stopped, anything it
had already queued is dropped before it reaches `update()`.

`mayag_live` is a real-time CPU monitor built this way; between samples it
sits at 0% CPU, asleep in `epoll_wait`. `tests/test_live.cpp` runs a real
window whose only wake source is a background thread and checks the message
is folded within a few ms and that the loop used almost no CPU while it
waited. `tests/test_concurrency.cpp` runs under ThreadSanitizer.

## Performance

The same small app (a title, an animated progress bar, a scrolling list of
200 two-line rows) in mayag, GTK4 and Qt6 Widgets, on Linux/Wayland with an
NVIDIA GPU, median of 9 runs (`bench/compare.py`):

| | first frame on screen | CPU while animating | RSS |
|---|---|---|---|
| mayag | **29 ms** | **1.2–1.8%** | 151 MB |
| GTK4 | 184 ms | 2.2–2.8% | 171 MB |
| Qt6 Widgets | 45 ms | 5.0% | 84 MB |

First frame is spawn to the first `wl_surface.commit` with a buffer, read from
`WAYLAND_DEBUG` in every toolkit, so all three are cut at the same point. Qt's
memory is lowest because Qt Widgets draws on the CPU and never loads the GPU
driver; mayag and GTK4 both carry it.

### Startup

- System font discovery is cached in `~/.cache/mayag/fonts.cache`, keyed on
  every font directory's mtime, so adding or removing a font rescans.
- Kerning is read from the font's own tables; opening a face builds nothing.
- The Vulkan instance and device are made on a worker while the window talks
  to the compositor, and fonts load on another. If the GPU isn't ready when
  the window is, the first frames go out through the CPU renderer and the
  window switches to Vulkan as soon as it is.

`MAYAG_STARTUP=1` prints each phase in ms since exec.

### Reusing what didn't change

A frame rebuilds the view, styles it, lays it out and paints it. `memo()`
skips all of that for a subtree whose inputs didn't change:

```cpp
Node list = memo(MemoKey{}.add(m.rows_version).add(m.selected),
                 [&] { return rows(m); });
```

On a hit the subtree comes back from last frame already styled and laid out.
It is still restyled when something it resolves against changes (theme, var
bindings, inherited text, hover/press/focus inside it), relaid out if the
space it gets changes, and kept while a transition inside it is moving. Hover
that `build` read through `ctx()` is tracked, so a subtree styled that way is
rebuilt when the pointer moves. The key has to cover what `build` reads from
the model; size, DPI and theme are folded in for you.

Widgets do the same through their context: `memo_widget(c, key, build)` in
`widget/control.hpp`. `file_tree` memoises itself this way.

`tests/test_memo.cpp` runs an app with and without memo through hover, press,
scroll, transitions, theme, resize and key changes and requires identical
frames. `tools/memo-check.sh EXAMPLE` does the same for any example's
scripted run. A reused subtree that only moved is shifted rather than laid
out again, which can round 1 ulp away from a fresh layout: at most 1/255 on
an antialiased edge.

### Seeing where the time goes

- `MAYAG_FRAMESTATS=1` prints per-stage frame times once a second, and how
  many identical frames were not resubmitted to the GPU.
- Every example takes `--bench N` (headless, per-stage means and peak RSS)
  and `--headless --record DIR` (every frame as a PNG).
- `MAYAG_MEMO=0` turns memo off, `MAYAG_GPU_ASYNC=0` waits for the GPU before
  the first frame, `MAYAG_FONT_CACHE=0` skips the font cache.
- `tools/photon-bench-linux.sh` drives a real uinput mouse and prints the
  window's measured input-to-photon latency.

## Accessibility

A UI that exists only as pixels is unusable with a screen reader and
untestable without comparing images. mayag exposes what each node **means**
alongside how it looks:

```cpp
box() | role(a11y::Role::checkbox) | checked(done) | label(item.text)
```

Most nodes need nothing — a text node announces its own text, a named leaf is
a button — so only ambiguous cases pay. The todo app's tree:

```
group
  heading "Todo"
  tab "all" [selected]
  tab "active"
  textfield "New todo"
  group
    checkbox "Ship the tiled rasteriser" [checked]
    checkbox "Fix Retina contentsScale" [checked]
```

It is a plain queryable tree, not a platform bridge — so it works identically
on every OS and in CI with no accessibility stack installed:

```cpp
auto tree = a11y::snapshot(root);
tree.find_by_label("Save document");   // what a test should assert on
tree.interactive();                    // everything reachable by keyboard
```

Anonymous layout wrappers are collapsed, because the visual tree's shape is an
implementation detail and a screen-reader user should not walk six nested
boxes to reach a label.

## Input methods

Typing Japanese, Chinese or Korean goes through an input method: the user
types `konnichiwa`, an IME shows a growing preedit (k → ko → こん → こんにちは)
and only on confirmation does committed text appear. Reading keystrokes
directly gives ten Latin letters and the conversion never happens — the
framework is simply unusable in those languages.

mayag models composition as state separate from the document:

```cpp
static std::optional<Msg> on(const Model&, const ComposeEvent& e) { return Composing{e}; }
static std::optional<Msg> on(const Model&, const TextEvent& t)    { return Typed{t.text}; }

m.field.set_preedit(e.text);   // shown, not committed
m.field.commit_preedit();      // now it is in the document
m.field.cancel_preedit();      // document untouched
```

`display_text()` splices the preedit in at the caret, so a view renders what
the user is composing without it ever entering the model. While composing,
keys belong to the input method — arrows pick candidates, they do not move the
caret.

The headless backend can script a full composition, so **CJK input is testable
in CI** rather than only by someone with a Japanese keyboard. The macOS
`NSTextInputClient` bridge is the remaining piece; the event model it plugs
into is complete.

## Undo/redo

The Model is a value, so history is nearly free — but two things are easy to
get wrong, and `History<T>` gets them right once:

```cpp
m.doc.edit([](Doc& d) { d.text += c; }, "typing");   // coalesces
m.doc.undo();
```

- **Coalescing.** Typing ten characters is *one* undo, not ten. Without it a
  user presses Cmd-Z and loses a letter.
- **A bound.** Unbounded history of a large model is a leak that only bites in
  long sessions — exactly when losing work hurts most.

Editing after an undo discards the redo branch, because redoing into a future
that no longer follows from the present is incoherent.

## Error boundary

One bad frame should not end a session. With `AppConfig::catch_exceptions`
(the default) a fault is skipped, not fatal:

- a throwing `update` is rolled back to the model before the message, and the
  app loses one message rather than all its state.
- a throwing `view` keeps the **last good tree** on screen. A blank window
  tells the user nothing; a stale frame at least stays usable enough to save.

Faults go to `AppConfig::on_error`. Turn `catch_exceptions` off and a throw
stops the program with exit code 70.

## Clicks

A click carries a **count**; there is no separate `double_click` gesture.

```cpp
static std::optional<Msg> on(const Model&, const Clicked& c) {
    if (!is(c, "word")) return std::nullopt;
    if (c.count == 1) return SelectChar{};
    if (c.count == 2) return SelectWord{};
    return SelectLine{};          // 3 and beyond
}
```

Enter or Space on a focused control is `Activated`, not a click; handle it to
make a control keyboard-activatable, or leave it and the key reaches
`on(KeyEvent)`.

Three things this gets right that the obvious design does not:

- **One gesture per click.** Emitting `click` *and* `double_click` means an
  app handling both fires its single-click action on every double. The count
  is data on the gesture, so `on_click` matches singles only and a button
  that should fire regardless says so with `on_any_click`.
- **The platform's count is authoritative.** macOS, Windows and X11 all track
  click sequences using the user's configured interval — which is an
  accessibility setting, adjustable while the app runs. mayag decodes
  `clickCount` and *uses* it; the timestamp-based synthesis is a fallback for
  backends that report nothing, and it counts to arbitrary depth rather than
  stopping at two.
- **Distance matters as much as time.** Two rapid clicks in different places
  are two clicks. Otherwise a fast user clicking down a list triggers
  double-click actions.

## Where the software rasteriser stops

Being honest about the ceiling. 200 instances — a modest UI — on an M1:

| surface | raster | encode | total | fps |
|---------|--------|--------|-------|-----|
| 800x600 | 0.79 ms | 0.41 ms | 1.20 ms | 832 |
| 1440x900 | 1.90 ms | 1.02 ms | 2.93 ms | 342 |
| 2880x1800 | 4.96 ms | 4.26 ms | 9.22 ms | 108 |
| 3840x2160 | 6.81 ms | 6.80 ms | **13.61 ms** | 73 |

At 4K that is **84% of a 60 Hz budget** for a UI doing almost nothing — and
before the CGImage blit, which is another ~45% of present because it copies
the whole surface on the CPU. The cost is per-PIXEL, so no amount of CPU
tuning fixes it: a GPU does the same work in microseconds because it has
thousands of lanes instead of eight.

That is what `backend/metal.hpp` is for. The whole framework was designed
around this moment — every primitive is a rounded-box SDF on one instanced
quad, so a batch is **one draw call of N instances** and the fragment shader
is the same kernel `render/sdf.hpp` implements in C++. No tessellation, no
per-shape pipelines, no state changes.

### What the GPU path actually costs

Measured by `examples/present_bench.cpp`, which opens a real window, builds
one draw list, and submits it through **both** backends in the same process —
same scene, same machine, no cross-run drift. 303 instances in 3 batches,
400 frames after 60 discarded as warmup, on an M1:

| surface | software CPU | Metal CPU | ratio |
|---------|-------------|-----------|-------|
| 2000x1440 | 4.96 ms | **0.04 ms** | 124x |
| 3840x2160 | 14.07 ms | **0.04 ms** | 352x |

The software column grows with pixel count; the Metal column does not move,
because the CPU's job there is `memcpy` the instance buffer and encode three
draw calls. Scene complexity barely registers either — 2000 instances instead
of 303 costs 0.05 ms.

**The wall-clock number is different and it matters.** Metal's *wall* time per
present is 9.96 ms, because `nextDrawable` blocks until the display is ready
and this monitor runs at 100 Hz — a 10.00 ms frame. That is correct pacing,
not slowness, and it is why the benchmark reports thread CPU time next to
wall time: measuring only wall time would make working vsync look like a
regression, and measuring only CPU time would hide the compositor entirely.

**The claim is falsifiable, which is the point.** "0.04 ms of CPU" is also
exactly what a backend that draws *nothing* costs, so `tests/test_metal.cpp`
renders the same draw list offscreen through Metal and through the software
rasteriser and diffs the pixels. It covers the whole primitive set, because a
backend can be perfect on rectangles and still be broken everywhere else:

| checked on GPU vs CPU | why it could diverge on its own |
|---|---|
| rounded boxes, circles | the shared SDF kernel |
| **text** | needs the glyph atlas uploaded as a texture; the CPU path just calls back into the rasteriser |
| linear / radial gradients | Oklch interpolation is transcribed separately into C++ and MSL |
| shadows | the quad is grown in the vertex stage |
| rings, strokes | different SDF branches |
| clipping | per-batch scissor rects |

Shape interiors must match exactly, clipped content must not escape its
scissor, and whole-frame mean error must stay under 3/255. It currently
measures **0.26/255**, with 0.59% of pixels differing by more than 8 — the
antialiased edges, where hardware derivatives and the analytic footprint
legitimately disagree by a step.

That test earned its place twice over. It caught
`MTLPrimitiveTypeTriangleStrip` being defined as 3 instead of 4 — 3 is
`MTLPrimitiveTypeTriangle` — so every quad was rendering as the first half of
itself. No Metal error, no validation warning, and the benchmark was perfectly
happy: drawing half a quad is *faster*. Only a pixel diff against a known-good
rasteriser could see it, and what gave it away was the shape being in the
right place with the right colour and 7739 of its 16000 pixels.

Then, when the text case was added, it caught the atlas upload being gated on
`valid_` — which means "attached to a window", not "has a device". Uploading a
texture needs no window, so every offscreen render silently skipped it and
text came out **completely blank** while every shape was pixel-perfect. Had
GPU-by-default shipped before that check existed, every example would have
rendered with invisible text.

### GPU by default, on every platform

The GPU backend is **ON everywhere the platform has one** — Metal on Apple,
Vulkan on Linux, BSD and Windows — and it is what apps get with no
configuration. Running any example prints which path it took:

```
$ ./build/examples/mayag_todo
mayag: renderer vulkan (wayland)
```

The software rasteriser has not gone anywhere — it is the reference every GPU
result is diffed against, and the fallback when a machine has no device — but
it is no longer what apps get by default. Shipping a 350x-slower default
because it sounds safer would be the wrong trade for a framework whose entire
design (one SDF kernel, one instance struct, one draw call per batch) exists
to feed a GPU.

The Vulkan backend is built the same way as the Wayland one, for the same
reason: **it links nothing.** `libvulkan.so.1` / `vulkan-1.dll` is `dlopen`'d
at startup and the slice of the Vulkan ABI mayag uses is transcribed by hand,
so there is no Vulkan SDK build dependency and no header/loader version skew.
The one shader is compiled to SPIR-V at configure time when `glslc` is present
and the result is checked in, so a build with no shader compiler still has a
working GPU path — there is no runtime GLSL and no `shaderc` dependency. A
Vulkan-enabled binary still runs on a box with no Vulkan; the loader fails and
it falls back to software.

Selection is a runtime fallthrough, and every step of it degrades rather than
fails: no backend in the build, no device in the machine, or a shader that
will not compile all end up on software. `MAYAG_BACKEND=vulkan|metal|software`
forces a path, which is also what makes the A/B benchmark possible. On Linux a
failed GPU init leaves the shm surfaces already prepared, so the window simply
presents on the CPU instead of showing a black rectangle that looks like a
hang — a worse outcome than being slow.

The GPU output is not taken on faith. `mayag_vulkan_tests` renders the same
scenes through Vulkan and through the software rasteriser and asserts they
agree: a solid fill differs by a mean of **0.08/255**, gradients by 0.12, and
full shape scenes by 0.44 with disagreement confined to the ~1% of pixels on
antialiased edges. Two GPU renders of one scene are bit-identical. The test
skips itself where no device exists, so it is meaningful on a GPU box and
silent on a headless runner.

That fallthrough is exactly why the renderer is announced at boot: the app
still works when it silently drops to software, just two orders of magnitude
slower, and that is precisely the failure worth noticing.

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

The yardstick is CoreText: the same string, size and colours rendered by both,
compared glyph by glyph and pixel by pixel (SF, the macOS system font, the
hardest Latin font a UI meets).

- **Positions are CoreText's, to 1/100 px.** SF keeps its kerning in GPOS
  extension lookups whose values carry per-instance variation deltas (so a pair
  kerns differently at 13 pt and 28 pt, Regular and Bold), and gets its
  spacing from Apple's `trak` table. mayag applies all of it: the `kern`
  feature's lookups only, any value-record format, GDEF variation-store deltas
  at the face's instance, and per-size tracking, floored to whole units as
  CoreText does. Across 81 string × size cases from 6 to 120 px, Regular and
  Bold, every glyph lands within 0.01 px of `CTRunGetPositions`. Before, a
  23-glyph line drifted up to 7.5 px.
- **Subpixel positioning.** Bitmap glyphs must sit on the pixel grid to stay
  sharp, but snapping the pen moves each glyph up to half a pixel and the eye
  reads that as uneven spacing. mayag rasterises each glyph at four horizontal
  phases (as Skia does) and draws it at the pen's whole pixel: sharp texels
  *and* placement within 1/8 px. Measured column-profile drift against
  CoreText went from 0.9–2.6 px RMS to 0.00.
- **Font smoothing.** macOS grows glyph outlines; that is why native Mac text
  has weight. Measured gamma-independently (straight stems at 64 subpixel
  offsets, smoothing on vs off) it is 0.30 px per side, reached at 20 px for
  light text and 51 px for dark. mayag offsets the outline itself (every edge
  along its normal, FreeType's embolden scheme) for bitmap glyphs, and shifts
  the threshold for distance-field glyphs, so weight is continuous across the
  bitmap/SDF switch.
- **Gamma-space text compositing.** Shapes blend in linear light; text is
  blended as if in gamma space, the way CoreText, DirectWrite and Skia blend
  it, because fonts are designed for that weight. In linear light the same
  font reads bold on dark themes and thin on light ones. The kernel reshapes
  coverage so the hardware's linear blend reproduces the gamma-space result
  exactly against the design backdrop; one definition serves CPU, Metal and
  Vulkan.
- **The GPU draws the CPU's pixels.** Metal (the default on macOS) used to
  interpolate glyph uvs across the antialiasing-padded quad instead of the
  glyph's own rect, stretching every glyph by two pixels: all on-screen text
  was blurred and drifted ~0.4 px, while an ink-count test passed. A per-pixel
  GPU/CPU text test now pins mean edge error at 0.6/255 (it was 30).
- **Analytic coverage.** Every pixel gets the true area of the glyph inside
  it, not N supersamples. A coverage-conservation test asserts total area is
  within 0.2% for axis-aligned, offset, fractional, and sub-pixel-thin shapes.
- **Correct decode per glyph.** Bitmap entries are coverage; SDF entries are a
  distance field. Which one travels per *instance*, because hybrid mode mixes
  both in one frame.

The result, as mean coverage error against CoreText over every inked pixel
(SF, 11–60 px, 0 = identical weight): light-on-dark went from +0.17–0.19 to
+0.00–0.05, and dark-on-light is within ±0.01. Ink mass is 0.94–1.09×
CoreText's from 11 to 120 px, with no jump at the bitmap/SDF threshold (it
used to drop 15% there).

## Cross platform

| Platform | Renderer | Windowing |
|----------|----------|-----------|
| macOS, iOS | **Metal** | Cocoa |
| **Linux, BSD** | **Vulkan** (→ software) | Wayland |
| Windows | **Vulkan** / D3D12 | Win32 |
| Web | WebGPU | canvas |
| anything with a compiler | **software** (always) | headless |

Selection is `MAYAG_BACKEND` env var → platform native GPU → software. A backend that fails to initialise falls through to the next, so a missing driver costs you frames, not a crash. The backend interface is four things — bring up a device, own a swapchain, upload the instance buffer, issue N instanced draws — and never sees a widget, a style, or a colour space.

### Linux, in detail

The Linux backend is Wayland, and it **links nothing**. `libwayland-client.so.0`
is resolved with `dlopen` at startup, and the `xdg-shell` interface tables —
which libwayland does not export, and which every other client generates with
`wayland-scanner` — are transcribed by hand in `platform/wayland_protocol.hpp`.
So there is no `find_package`, no `pkg-config`, no code generator, no generated
sources in your build tree, and no version skew between the `.xml` on the build
machine and the `.so` on the user's. One binary spans libwayland 1.19 → 1.26,
and a binary built *with* Wayland still runs on a machine that has none — the
loader fails, `open()` returns `nullopt`, and the runtime falls back to headless
instead of dying at process start with a missing shared object.

What makes it fast is that a CPU renderer and Wayland's buffer model fit
together exactly: the client allocates shared memory and the compositor maps
**the same pages**, so presenting is a pointer handoff with no upload and no
server-side copy. mayag renders into that memory directly.

- **Zero-copy present.** The obvious implementation renders to a `Vec4`
  framebuffer, encodes to an RGBA `vector`, `memcpy`s into the shm buffer, then
  swizzles to BGRA — three full-frame passes and ~14 MB of allocation at 1440p.
  mayag does none of it: the sRGB encode and the BGRA swizzle happen in **one
  parallel pass writing final bytes to their final address**. Per-frame
  allocation is zero.
- **Damage-bounded encode.** The draw list already knows every rect it touched,
  so only changed rows are encoded and only changed pixels are sent as
  `damage_buffer`. Buffer-age tracking makes this safe with buffers in flight.
  Measured: **2.06 ms → 1.35 ms** per present when a label changes rather than
  the whole window.
- **Triple buffering** with release tracking, so `present` never waits on the
  compositor; a frame is dropped rather than torn, and drops are counted.
- **Frame callbacks as the vsync source.** The compositor says when it wants the
  next frame, so no frame is rendered that will be discarded and an occluded
  window costs nothing.
- **Idle means idle.** `Wait::block` is a real `poll()` on the Wayland fd, using
  libwayland's `prepare_read`/`read_events` handshake to close the classic
  check-then-block race. Measured **0.0% CPU** while idle.

It also uses the modern protocols when the compositor offers them, and works
without them when it doesn't: `xdg-decoration` for real server-side title bars,
`fractional-scale` + `viewporter` to rasterise at exactly 1.25× or 1.5× instead
of rounding and resampling, and `cursor-shape-v1` for themed cursors — which
replaces the several hundred lines of `XCURSOR_THEME` parsing and cursor-surface
management that clients usually carry.

Keyboard input maps **evdev scancodes**, which are positional, so shortcuts land
on the same physical keys on QWERTY, AZERTY, Dvorak and Colemak. Text is
resolved through `libxkbcommon` when present (dead keys, AltGr, IME-adjacent
layouts) and degrades to "keys work, exotic text doesn't" when it is absent.

Built by default on Linux and BSD; `-DMAYAG_WITH_WAYLAND=OFF` opts out.

## The runtime is jaal: one strict boundary

```
your app  ──►  mayag  ──►  jaal
```

**Everything about running a program is jaal's:**

- the event loop, `update()` folding and the frame clock
- timers, tasks, streams, keyed tasks and teardown
- OS waiting (epoll, kqueue, Windows) and signals
- the worker pool, and the render fork-join (`jaal::parallel_for` on a warm `jaal::kernel::pool`)
- the clock every frame is paced by
- fault handling

mayag is a jaal *host*: it draws, routes input and runs window effects. It runs no loop, thread or timer of its own.

**Your app talks to mayag only.** Every runtime concept has a mayag name, so an app never includes or names jaal:

| You need | Spell it |
|---|---|
| effects / subscriptions | `mayag::Cmd<Msg, ...>`, `mayag::Sub<Msg, ...>` |
| a custom effect | `mayag::Effect<Payload, "name">` |
| a task talking back | `mayag::Sink<Msg>` |
| a big immutable value across threads | `mayag::Shared<T>` |
| state tasks share | `mayag::CoOwned<T>`, `mayag::Guarded<T[, Rank<N, "name">]>` |
| proof of holding no lock (task bodies) | `const mayag::Unlocked&` |
| OS signals | `on(const Model&, mayag::Signal)` |
| your type in a Msg | `MAYAG_SENDABLE(T)` / `MAYAG_FROZEN(T)` |

**It is enforced by ctest, not by convention:**

- **`mayag_examples_use_only_mayag` / `mayag_tests_use_only_mayag`:** no app or test code names jaal.
- **`mayag_examples_banlist`:** jaal's own concurrency banlist runs over `examples/` with an *empty* allowlist. No raw threads, mutexes, atomics, `thread_local`, futures or `const_cast`.
- **`mayag_runtime_banlist` + `mayag_runtime_allowlist_tight`:** the same banlist over `include/mayag`. The few primitives at boundaries jaal cannot own (a thread Metal calls back on, a C API signature, a cache behind a capability-less interface) are each listed with a reason in `tests/lint/allow_include.txt`, and a grant that is no longer used fails the build.
- **`mayag_uses_jaal_public_api`:** mayag itself uses only jaal's public API. That means no `detail::`, no kernel keys (`pool_access`, `cap_access`) and no minted capabilities. When mayag needed more (a warm-pool fork-join, hosting the kernel from a foreign loop), the fix went into jaal as public API (`jaal::parallel_for(pool&, ...)`, `jaal::forward_host`, `jaal::to_kernel_event`, `jaal::all_signals`), tested by jaal's own suite under TSan.

## Deterministic simulation: async UI bugs become seeds

`<mayag/app/simulate.hpp>` runs a program through the **real** host (cascade, layout, hit testing, inline handlers, theme morph, transitions) and the **real** kernel, on jaal's seeded simulation world:

- **Simulated time:** timers, springs, morphs and frame pacing all see it, so 30 simulated seconds take milliseconds.
- **Seeded task timing:** task bodies run on the loop thread at seeded latencies, so results race clicks, timers and each other in an order that is random but exactly repeatable.
- **Fault injection:** tasks can be made to crash or vanish at a set rate.

```cpp
auto r = mayag::explore<Search>(cfg, {.runs = 1000}, [](auto& app) {
    app.type("m"); app.wait(3ms); app.type("ay");
    app.settle();
    return app.model().shown_for == app.model().query ? 0 : 1;   // the invariant
});
if (!r.ok()) mayag::run_simulated<Search>(cfg, r.seed, scenario);   // exact replay
```

`tests/test_simulate.cpp` does exactly this for the classic out-of-order-results hazard (docs/async-hazards.md #2):

1. explore finds a seed where a late reply overwrites a newer one.
2. Replaying that seed fails identically and draws the stale result on screen.
3. Two runs of a seed draw the identical screen at the identical simulated instant.
4. The generation-stamp fix holds across 1,000 seeds, in about a second of wall time.

The threaded paths that simulation replaces (the worker pool, the render fork-join) are checked the other way, under ThreadSanitizer.

## Building

Header-only, plus the jaal runtime (a submodule, built automatically). With CMake:

```bash
git submodule update --init
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

21 test binaries, plus 8 example apps that double as end-to-end tests and 8
must-not-compile cases. All of it runs with **no display** — which is what
makes the suite work in CI and over SSH alike:

```
ctest                        # 20/21 green on a fresh box
```

The layers match the three ways rendering can be wrong: **numeric** (colour
round-trips, SDF gradient magnitude, transfer curves), **layout** (flex
arithmetic against hand-computed rects), and **pixel** (render a scene, sample
actual pixels). The pixel layer is what catches "compiles, lays out, draws
nothing" — and it caught three real bugs while this was being written:

- cross-axis default was `start` instead of CSS's `stretch`, collapsing grown children to zero width
- `srgb_interpolation` was silently ignored — gradients never used the flag
- the first fix used *Cartesian* Oklab, which desaturates worse than linear RGB; the answer is *polar* Oklch

All three are now regression tests. The GPU (`mayag_vulkan_tests`), Wayland
(`mayag_wayland_tests`) and live-streaming (`mayag_live_tests`) suites carry a
pure-logic half that runs everywhere and a live half that **skips itself** when
no compositor is present, so a headless runner stays green while a machine with
a GPU verifies the real thing.

### Testing over SSH

mayag is a windowing framework, but almost none of it needs a window to test.
Three display-free modes cover the whole surface:

```bash
ctest                                    # the suite (window tests self-skip)
./build/examples/mayag_live --headless   # drive the REAL app, assert, exit
./build/examples/mayag_live --png f.png  # render a real frame to an image
```

`--headless` runs the actual `init`/`update`/`view` through the same runtime
the window uses — including `Cmd::stream` spawning its background thread — so
your live-app logic is exercised for real, only the pixels go nowhere. `--png`
renders frame zero through the full font/SDF pipeline; view it inline if your
terminal is kitty (`kitten icat f.png`, works through mosh + tmux) or `scp` it
back. `tools/test-remote.sh` runs the entire flow — build, ctest, every
example headless and to PNG — in one command; `--show` previews the frames
inline, `--window` spins up a headless `weston` so the window backends run too.

To see a *live* window remotely, either point it at a headless compositor
(`weston --backend=headless`, what CI does) or forward the real window with
`waypipe ssh host ./mayag_live`.

## Layout

Real flexbox, ~400 lines, no Yoga dependency: `grow`/`shrink` with spec-correct weighting, six `justify` modes, five `align` modes, gap, padding, margin, min/max clamping applied where the spec says, absolute/fixed children resolved out of flow, and percentage lengths.

Layout depends on a narrow `TextMeasurer` interface, never a font backend, so it can be tested with no fonts and gives the same result on every machine.

## Project layout

- `core/` — constexpr math, geometry, phantom-typed colour
- `style/` — Style aggregate, generated themes
- `dsl/` — the type-state DSL and the widget library
- `layout/` — flexbox and the text-measurement seam
- `render/` — SDF kernel, draw list, painter, shader source
- `backend/` — backend registry and the software rasteriser
- `font/` — the OpenType engine, discovery, and the synthesized last-resort face
- `app/` — Program/Cmd/Sub, the runtime, interaction, the platform seam
- `platform/` — the concrete windows: Wayland (Linux/BSD), Cocoa (macOS), headless
- `image/` — dependency-free PNG encoder

## License

MIT
