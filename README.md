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
- **A real font engine, from scratch.** TrueType and CFF parsing, colour/bitmap faces (CBDT/sbix/COLR), composite glyphs, kerning from `kern` and `GPOS`, per-codepoint script fallback across 30+ scripts, analytic-coverage rasterisation, and a Euclidean-SDF atlas. No FreeType, no HarfBuzz, no stb, no fontconfig.
- **One shape kernel.** Buttons, cards, dividers, circles, rings, arcs, capsules, shadows, glyphs — every one is a signed distance field on the same instanced quad. A whole app frame is **1–3 draw calls**, text included.
- **Analytic antialiasing.** No MSAA, no supersampling. The SDF has unit gradient, so coverage is exact at any zoom on any backend.
- **Runs everywhere, immediately.** A pure-CPU reference rasteriser ships in the box, font discovery finds the platform's typefaces with no configuration, and a synthesized last-resort face keeps text legible even on a machine with no fonts at all. `git clone && cmake --build` renders a PNG.
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
| `shape.hpp` | UTF-8, clusters, `kern` + `GPOS` pair kerning, UAX-14 break rules |
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
       If this is not a game or a visualiser, something is requesting
       frames that should not be:
         * a Sub::every_frame whose condition is always true
         * an Animated<T> that never settles
         * a view that marks itself dirty every frame
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

mayag does neither. Three pieces:

- **`Cmd::stream`** is a long-lived producer, fired imperatively from `update`.
  Where `Cmd::task` runs once and returns a single message, a stream gets a
  thread-safe `sink` and emits an open-ended sequence over its lifetime. The
  runtime owns the thread and joins it on shutdown; a well-behaved stream
  returns when `keep_running()` goes false.

  ```cpp
  // A metrics poller. Each sample flows into update() as a message.
  Cmd<Msg>::stream([](auto sink, auto alive) {
      CpuSampler s;
      while (alive()) {
          std::this_thread::sleep_for(100ms);
          if (alive()) sink(Sample{s.read()});
      }
  });
  ```

- **`Sub::source`** is the *declarative* form — the one you usually want. It is
  a stream keyed by an id and returned from `subscribe()`, so it runs exactly
  as long as the model says it should. "Connect the socket while logged in,
  close it on logout" is one line, with no `connect()`/`disconnect()`
  bookkeeping in `update` and no thread leaked when the flag flips — the
  runtime diffs the ids each frame, starts a producer that appeared, and stops
  and joins one that vanished.

  ```cpp
  Sub<Msg> subscribe(const Model& m) {
      return Sub<Msg>::batch(
          m.streaming ? Sub<Msg>::source("cpu", cpu_feed) : Sub<Msg>::none(),
          Sub<Msg>::on_close(Quit{}));
  }
  ```

  A firehose feed — a socket or sensor faster than the UI renders — would grow
  the inbox without bound if every message were queued. `Sub::source_latest`
  is the backpressure answer: it keeps only the newest message between frames,
  so the app sees one value per frame regardless of rate. In a test, a
  producer emitting 20,000 values delivers **1** — the most recent — rather
  than a 20,000-deep backlog. Use it for a state feed (a price, a metric, a
  position); use plain `source` when every message matters (a chat, a log).

- **A cross-thread waker** (a self-pipe the window's `poll()` also watches) is
  what makes that message *render now*. Posting to the runtime's inbox writes
  one byte to the pipe, which interrupts the blocked UI thread the instant the
  producer fires — no polling timer, no missed update. Wakes coalesce, so a
  burst of a thousand streamed messages between two frames costs one syscall.

The result is an app that is genuinely live and still cheap. `mayag_live` is a
real-time CPU monitor whose sampler is a `Sub::source` emitting ten readings a
second; measured while running, it sits at **0.0% CPU** between samples,
because the UI thread is asleep in the kernel until the next reading arrives.
The whole path — waker, inbox, `Cmd::stream` and `Sub::source` lifecycle — is
verified race-free under ThreadSanitizer, and a test asserts a blocked window
is woken by a background thread in ~80 ms rather than sleeping through it, and
that a source stops and joins its thread the frame the model drops it.

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
Sub<Msg>::on_compose([](const ComposeEvent& e) { return Composing{e}; }),
Sub<Msg>::on_text([](std::string_view s)       { return Typed{s}; }),

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

One bad frame should not end a session. When `update()` or `view()` throws,
the runtime reports it and carries on:

- `update` takes the model **by value**, so a throw leaves it moved-from — the
  runtime restores a snapshot, and the app loses one message rather than all
  its state.
- a throwing `view` keeps the **last good tree** on screen. A blank window
  tells the user nothing; a stale frame at least stays usable enough to save.

Off by default in tests, where a crash at the point of failure is more useful
than a swallowed one.

## Clicks

A click carries a **count**; there is no separate `double_click` gesture.

```cpp
Sub<Msg>::on_click<"word">(SelectChar{}),        // single ONLY
Sub<Msg>::on_double_click<"word">(SelectWord{}),
Sub<Msg>::on_triple_click<"word">(SelectLine{}),
Sub<Msg>::on_any_click<"btn">(Activate{}),       // fires regardless of count
```

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

Layout depends on a narrow `TextMeasurer` interface, never a font backend — so it is testable with no fonts, deterministic across machines, and swapping in HarfBuzz later touches exactly one class.

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
