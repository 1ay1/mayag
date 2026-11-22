# Type in mayag

Every text size in mayag is a **role**, never a number. This page is the why
and the how. The mechanism lives in `include/mayag/style/type.hpp`.

## Why

mayag's widgets were written one at a time, and each picked its text sizes
alone: `font_size * 0.72`, `* 0.74`, `* 0.76`, `* 0.78`, `* 0.8`... By the
time this was fixed there were **25 distinct multipliers over 404 call
sites**. Each one was defensible on its own. Together they put four sizes
within a pixel of each other in one panel (the file pane), and the eye read
that as noise ("why are there so many font sizes?"), not as hierarchy.

Some text should be smaller than other text: a key hint, an age beside a
file name. What goes wrong is text that is smaller **by an amount nobody
chose**.

## The model

Text has a **scale** and a **tier**.

| Scale | Body size | Used for |
|---|---|---|
| `Scale::ui` | `Theme::font_size`, the app's interface size | labels, titles, toolbars, hints |
| `Scale::content` | `Theme::content_size` (hand: the terminal size), or a body the widget is handed | what the widget is *for*: a file list, a terminal's overlays |

| Tier | Step | For |
|---|---|---|
| `display` | +3 | a hero number, a splash |
| `title` | +2 | a panel or page title |
| `large` | +1 | a section heading, a dialog's question |
| `body` | 0 | the text the thing is about: names, labels, entries |
| `small` | −1 | meta beside the body: ages, sizes, crumbs, captions |
| `fine` | −2 | key hints, counts, footnotes: read when looked for |

A step is the theme's `TypeRules::ratio`, by default 1.2 (a minor third).

So one scale has at most six sizes and an app has at most twelve, however
many widgets it has. The user sets **two numbers**: the interface size and
the terminal size. Nothing else about type is a setting.

The scale says how big text is, not which font draws it. Fonts are separate:
`family()`, `monospace`, `Theme::font_family`.

## The rules

`TypeRules::size(body, tier)` computes `body * ratio^tier` and then applies
three rules in order:

1. **Grid.** Sizes snap to half points (`grid`). No size is ever 0.3 px away
   from another.
2. **Floor.** Nothing goes below `floor` (10 px). Stepping down twice from a
   small interface size can't reach illegible.
3. **Distinct.** A tier within `min_step` (1 px) of its neighbour toward
   `body` takes that neighbour's size. Two sizes are either the same or
   visibly different, never "almost". At a small interface size, `fine` and
   `small` merge, and the difference falls to colour and weight. That is
   what a designer would do anyway.

These hold by construction: `static_assert`s in `type.hpp`, and
`tests/test_type.cpp` checks every body from 8 to 32 px against four ratios.

Some ramps, at the default rules:

| body | fine | small | body | large | title | display |
|---|---|---|---|---|---|---|
| 12.5 | 10.5 | 10.5 | 12.5 | 15.0 | 18.0 | 21.5 |
| 13 | 10.0 | 11.0 | 13.0 | 15.5 | 18.5 | 22.5 |
| 14 | 10.0 | 11.5 | 14.0 | 17.0 | 20.0 | 24.0 |
| 16 | 11.0 | 13.5 | 16.0 | 19.0 | 23.0 | 27.5 |

## Spelling it

Late-bound (resolved by the cascade, so it follows a subtree theme and a
theme morph):

```cpp
text<"3d"> | type(Tier::small)
text<"name"> | type(Scale::content, Tier::body)
text<"Hint"> | font(var::fine)
```

Resolved now, when the widget needs the number, for example to measure
with `text_advance`:

```cpp
const float small = t.type(Tier::small);
const float meta  = t.type(Scale::content, Tier::small);
const float row   = t.type_at(p.text_size, Tier::small);   // a body the widget was handed
```

`type(...)` also sets the line height that suits the tier: small text gets
more air, display text less.

### Not text: glyphs and cells

- **A glyph fitted to geometry** says so: `glyph_size(px)`. Examples are a
  check mark sized to its 16 px box, a tab's monogram sized to its tile, or
  a powerline wedge as tall as the bar. It is the one sizing modifier allowed
  to compute.
- **A grid of cells** (`terminal_view`, `code_view`) uses its font size
  exactly. That size *is* the cell metric, so it is neither a tier nor
  snapped. Overlays drawn over a grid (hint labels, search results, history)
  are text, and they use tiers of the grid's size: `t.type_at(o.font_size,
  Tier::small)`.

## Enforcement

`tests/lint/type_scale.cmake` (in mayag's ctest, and in hand's `static`
label) rejects these in `widget/`:

- `font(x * k)`, `font(x / k)`, `font(x + k)`: arithmetic makes a new size;
- `font(11)`: a bare literal;
- `font(t.type(...) * k)`: arithmetic on a tier;
- `font_size * 0.x` anywhere.

## Migration

`tools/migrate_type.py` moved the 404 call sites by effective multiplier:

| Multiplier | Became |
|---|---|
| < 0.77 | fine |
| < 0.90 | small |
| < 1.10 | body |
| < 1.32 | large |
| < 1.60 | title |
| otherwise | display |

Glyphs and grid cells were handled by hand. The old `var::font_xs` through
`font_2xl` tokens are now aliases for the six tiers.
