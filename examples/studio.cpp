// examples/studio.cpp — mayag studio: a live UI you can re-skin while you use it
//
// On the left, a theme designer. On the right, a real app (a music
// dashboard). Every control on the left re-derives the theme, and the HOST
// MORPHS the whole interface to it: colours in Oklab, corner radii, spacing,
// type size, depth, springs -- and the designer's own sliders and switches,
// which are made of the same widgets, reshape under your cursor while you
// drag them.
//
//   presets   Midnight, Daylight, Brutal, Glass, Terminal, Soft: not palettes,
//             complete looks. Brutal / Glass / Terminal swap every widget's
//             SHAPE (sheets::brutal / glass / terminal).
//   knobs     hue, chroma, roundness, density, type, depth, motion, lines;
//             motion 0 is reduced motion: themes then switch instantly
//   remix     a random look, morphed to
//   worlds    the bottom strip renders the same app state in three themes
//             at once with themed(...): press Play in any world, every world
//             (and the main player) answers -- one model, many skins.
//
// What makes it work is that a theme is DATA. view() never mentions a
// colour or a size: it names roles (var::accent, var::space(2),
// var::radius_lg, var::depth(8)) and widgets (button, card, slider), and the
// theme decides the rest. The app code below has no idea which look is on.
//
// Keys: 1-6 presets, R remix, Space play/pause, Esc quit.
// Build:  cmake --build build && ./build/examples/mayag_studio
// (--headless runs the self-checks; --png writes a screenshot)

#include <mayag/mayag.hpp>
#include <mayag/widget/button.hpp>
#include <mayag/widget/card.hpp>
#include <mayag/widget/scroll_view.hpp>
#include <mayag/widget/sheet.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/toggle.hpp>

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <variant>

using namespace mayag;
using namespace mayag::dsl;

// ════════════════════════════════════════════════════════════════════════
// Looks: the designer's model is a handful of knobs
// ════════════════════════════════════════════════════════════════════════

enum class Skin : std::uint8_t { standard, brutal, glass, terminal };

struct Knobs {
    Skin  skin = Skin::standard;
    float hue = 262.0f, chroma = 0.16f, lum = 0.70f;
    float roundness = 1.0f, density = 1.0f, type = 1.0f, depth = 1.0f, motion = 1.0f, lines = 1.0f;
    bool  dark = true;
    friend constexpr bool operator==(const Knobs&, const Knobs&) = default;
};

[[nodiscard]] inline Theme theme_for(const Knobs& k) {
    Seed s;
    s.accent    = Color<Oklch>{k.lum, k.chroma, k.hue}.to<Srgb>();
    s.mode      = k.dark ? Mode::dark : Mode::light;
    s.roundness = k.roundness;
    s.density   = k.density;
    s.type      = k.type;
    s.depth     = k.depth;
    s.motion    = k.motion;
    s.lines     = k.lines;
    switch (k.skin) {
        case Skin::brutal:   return looks::brutal(s);
        case Skin::glass:    return looks::glass(s);
        case Skin::terminal: return looks::terminal(s);
        case Skin::standard: break;
    }
    return looks::standard(s);
}

struct PresetDef { const char* name; const char* tag; Knobs k; };

inline const std::array<PresetDef, 6> kPresets{{
    {"Midnight", "the default", {Skin::standard, 262, 0.16f, 0.70f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, true}},
    {"Daylight", "light, crisp", {Skin::standard, 245, 0.17f, 0.60f, 1.0f, 1.0f, 1.0f, 0.8f, 1.0f, 1.0f, false}},
    {"Brutal", "ink and paper", {Skin::brutal, 95, 0.17f, 0.88f, 0.0f, 1.0f, 1.05f, 0.0f, 1.0f, 1.4f, false}},
    {"Glass", "frosted, glowing", {Skin::glass, 295, 0.19f, 0.68f, 1.6f, 1.05f, 1.0f, 1.4f, 0.9f, 1.0f, true}},
    {"Terminal", "phosphor green", {Skin::terminal, 150, 0.22f, 0.86f, 0.0f, 0.9f, 0.95f, 0.0f, 1.3f, 1.0f, true}},
    {"Soft", "round and airy", {Skin::standard, 330, 0.12f, 0.64f, 1.9f, 1.25f, 1.05f, 1.6f, 0.8f, 1.0f, false}},
}};

struct KnobDef { const char* label; float lo, hi; float Knobs::*field; };
inline const std::array<KnobDef, 8> kKnobs{{
    {"Hue", 0.0f, 360.0f, &Knobs::hue},
    {"Chroma", 0.03f, 0.26f, &Knobs::chroma},
    {"Roundness", 0.0f, 2.0f, &Knobs::roundness},
    {"Density", 0.7f, 1.4f, &Knobs::density},
    {"Type", 0.85f, 1.25f, &Knobs::type},
    {"Depth", 0.0f, 2.0f, &Knobs::depth},
    {"Motion", 0.0f, 2.0f, &Knobs::motion},
    {"Lines", 0.5f, 2.5f, &Knobs::lines},
}};
inline constexpr float kKnobW = 150.0f;   // knob slider track width
inline constexpr float kSeekW = 280.0f;   // main player's seek bar
inline constexpr float kMiniW = 200.0f;   // a world's seek bar

// ════════════════════════════════════════════════════════════════════════
// The stylesheet: roles and widgets only, never a colour or a size
// ════════════════════════════════════════════════════════════════════════

namespace ui {
constexpr auto motion  = transition();
constexpr auto caps    = styles(font(var::font_xs), bold, tracking(1.4f), fg(var::text_disabled));
constexpr auto muted   = styles(font(var::font_sm), fg(var::text_secondary));
constexpr auto h1      = styles(font(var::font_xl), bold, fg(var::text_primary));
constexpr auto h2      = styles(font(var::font_lg), semibold, fg(var::text_primary));

constexpr auto nav_item = styles(
    pad(var::space(0.75f), var::space(1.5f)), radius(var::radius_full), fg(var::text_secondary),
    font(var::font_sm), semibold,
    hovered(fg(var::text_primary), bg(var::tint(var::accent, 0.10f))),
    when(state::selected, bg(var::accent), fg(var::on_accent)),
    motion);

constexpr auto track_row = styles(
    pad(var::space(0.75f), var::space(1)), gap(var::space(1.5f)), radius(var::radius_md),
    align(Align::center),
    hovered(bg(var::tint(var::accent, 0.12f))),
    when(state::selected, bg(var::tint(var::accent, 0.22f))),
    group(), motion);
constexpr auto row_hint = styles(fg(var::accent), opacity(0.0f),
                                 group_hovered(opacity(1.0f), translate(-2, 0)),
                                 when(state::group(state::selected), opacity(1.0f)), motion);

constexpr auto preset_chip = styles(
    width(132), pad(var::space(1), var::space(1.25f)), gap(var::space(0.5f)), radius(var::radius_md),
    bg(var::surface_raised), border(var::line, var::border),
    hovered(border_color(var::border_strong), translate(0, -2)),
    pressed(scale(0.97f)),
    when(state::selected, border(var::line_focus, var::accent)),
    focus_visible(border(var::line_focus, var::focus_ring)),
    motion);

constexpr auto delta_up   = styles(pad(var::space(0.25f), var::space(0.75f)), radius(var::radius_full),
                                   bg(var::success.fade(0.18f)), fg(var::success), font(var::font_xs), bold);
constexpr auto delta_down = styles(pad(var::space(0.25f), var::space(0.75f)), radius(var::radius_full),
                                   bg(var::danger.fade(0.18f)), fg(var::danger), font(var::font_xs), bold);

constexpr auto search = styles(
    width(260), pad(var::space(1), var::space(1.5f)), radius(var::radius_md),
    bg(var::surface), border(var::line, var::border), fg(var::text_disabled), font(var::font_sm),
    hovered(border_color(var::border_strong)), motion);
}  // namespace ui

// ════════════════════════════════════════════════════════════════════════
// The app
// ════════════════════════════════════════════════════════════════════════

struct Studio {
    struct TrackDef { const char* title; const char* artist; const char* len; };
    static constexpr std::array<TrackDef, 6> kTracks{{
        {"Neon Cathedral", "Halcyon Drift", "3:42"},
        {"Glass Animals Dream", "Low Orbit", "4:05"},
        {"Paper Satellites", "Mira Vale", "2:58"},
        {"Phosphor", "The Terminal Kids", "3:21"},
        {"Soft Machinery", "Cloudline", "5:12"},
        {"Brutal Sunrise", "Ink & Iron", "3:07"},
    }};
    struct StatDef { const char* label; const char* value; const char* delta; bool up; };
    static constexpr std::array<StatDef, 3> kStats{{
        {"LISTENERS", "48.2k", "+12.4%", true},
        {"STREAMS TODAY", "1.36M", "+3.1%", true},
        {"SKIP RATE", "8.7%", "-0.9%", false},
    }};
    static constexpr std::array<const char*, 3> kNav{"Overview", "Library", "Radio"};

    struct Model {
        Knobs knobs = kPresets[0].k;
        int   preset = 0;            ///< -1 once knobs were turned by hand
        int   remixes = 0;
        int   nav = 0;
        int   track = 0;
        bool  playing = false;
        bool  shuffle = true;
        float progress = 0.32f;
        double clock = 0.0;          ///< drives the visualiser while playing
        ScrollState page{};
    };

    struct Preset { int i; }; struct Remix {}; struct Knob { int k; float f; }; struct Dark {};
    struct Nav { int i; }; struct Track { int i; }; struct Play {}; struct Prev {}; struct Next {};
    struct Shuffle {}; struct Seek { float f; }; struct Tick { double dt; };
    struct Scroll { float dy; }; struct BarPress { float y; }; struct BarDrag { float dy; }; struct Quit {};
    using Msg = std::variant<Preset, Remix, Knob, Dark, Nav, Track, Play, Prev, Next, Shuffle, Seek, Tick,
                             Scroll, BarPress, BarDrag, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
        // STUDIO_LOOK=0..5 opens on a preset (handy for screenshots).
        if (const char* e = std::getenv("STUDIO_LOOK")) (void)update(m, Preset{std::clamp(std::atoi(e), 0, 5)});
        if (std::getenv("STUDIO_PLAYING") != nullptr) m.playing = true;
        return SetTitle{"mayag studio"};
    }

    // ── the theme is a pure function of the model ───────────────────────
    static Theme theme(const Model& m) { return theme_for(m.knobs); }

    static Cmd update(Model& m, Preset p) {
        // A preset is a look; motion is the USER's preference (reduced
        // motion stays reduced), so it carries over unless still default.
        const float motion = m.knobs.motion;
        m.preset = p.i;
        m.knobs = kPresets[static_cast<std::size_t>(p.i)].k;
        if (motion != 1.0f) m.knobs.motion = motion;
        return {};
    }
    static Cmd update(Model& m, Remix) {
        // Deterministic "random": a hash of how many remixes came before.
        std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(++m.remixes) * 2654435761u;
        const auto next = [&](float lo, float hi) {
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            return lo + (hi - lo) * static_cast<float>(h % 10000u) / 9999.0f;
        };
        const float motion_keep = m.knobs.motion;
        Knobs k;
        k.skin = static_cast<Skin>(static_cast<int>(next(0.0f, 3.99f)));
        k.hue = next(0.0f, 360.0f);
        k.chroma = next(0.10f, 0.24f);
        k.dark = next(0.0f, 1.0f) > 0.45f;
        k.lum = k.skin == Skin::brutal || k.skin == Skin::terminal ? 0.86f : (k.dark ? 0.70f : 0.60f);
        k.roundness = k.skin == Skin::brutal || k.skin == Skin::terminal ? 0.0f : next(0.2f, 2.0f);
        k.density = next(0.8f, 1.3f);
        k.type = next(0.92f, 1.12f);
        k.depth = k.skin == Skin::brutal ? 0.0f : next(0.0f, 1.8f);
        k.motion = motion_keep;      // remix the look, never the user's motion preference
        k.lines = k.skin == Skin::brutal ? 1.4f : next(0.8f, 1.6f);
        m.knobs = k;
        m.preset = -1;
        return {};
    }
    static Cmd update(Model& m, Knob k) {
        const KnobDef& d = kKnobs[static_cast<std::size_t>(k.k)];
        m.knobs.*d.field = d.lo + std::clamp(k.f, 0.0f, 1.0f) * (d.hi - d.lo);
        m.preset = -1;
        return {};
    }
    static Cmd update(Model& m, Dark) {
        m.knobs.dark = !m.knobs.dark;
        if (m.knobs.skin == Skin::standard || m.knobs.skin == Skin::glass)
            m.knobs.lum = m.knobs.dark ? 0.70f : 0.60f;
        m.preset = -1;
        return {};
    }
    static Cmd update(Model& m, Nav n)    { m.nav = n.i; return {}; }
    static Cmd update(Model& m, Track t)  { m.track = t.i; m.progress = 0.0f; m.playing = true; return {}; }
    static Cmd update(Model& m, Play)     { m.playing = !m.playing; return {}; }
    static Cmd update(Model& m, Prev)     { m.track = (m.track + 5) % 6; m.progress = 0.0f; return {}; }
    static Cmd update(Model& m, Next)     { m.track = (m.track + 1) % 6; m.progress = 0.0f; return {}; }
    static Cmd update(Model& m, Shuffle)  { m.shuffle = !m.shuffle; return {}; }
    static Cmd update(Model& m, Seek s)   { m.progress = std::clamp(s.f, 0.0f, 1.0f); return {}; }
    static Cmd update(Model& m, Tick t) {
        m.clock += t.dt;
        m.progress += static_cast<float>(t.dt / 222.0);
        if (m.progress >= 1.0f) { m.progress = 0.0f; m.track = (m.track + 1) % 6; }
        return {};
    }
    static Cmd update(Model& m, Scroll e)   { m.page.scroll_by({0.0f, e.dy}); return {}; }
    static Cmd update(Model& m, BarPress e) { scrollbar_press(m.page, e.y); return {}; }
    static Cmd update(Model& m, BarDrag e)  { scrollbar_drag(m.page, e.dy); return {}; }
    static Cmd update(Model&, Quit)         { return Cmd::quit(); }

    /// Frames only while music plays: a paused studio idles at 0% CPU, and a
    /// theme morph asks for its own frames.
    static Sub subscribe(const Model& m) {
        return mayag::when(m.playing, Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }

    // ── designer panel ──────────────────────────────────────────────────

    static Node preset_chips(const Model& m) {
        return h(each(kPresets, [&](const PresetDef& p, std::size_t i) {
                   // The chip previews what it will BECOME, in its own colours.
                   const Theme pt = theme_for(p.k);
                   const auto dot = [](Color<Srgb> c) {
                       return box() | size(12, 12) | radius(var::radius_full) | bg(c)
                            | border(var::line, var::border_strong);
                   };
                   return v(h(dot(pt.background), dot(pt.surface), dot(pt.accent), dot(pt.text_primary))
                                | gap(3),
                            text_of(p.name) | semibold,
                            text_of(p.tag) | font(var::font_xs) | fg(var::text_secondary))
                        | ui::preset_chip | selected(m.preset == static_cast<int>(i)) | id_of("preset", i);
               }))
             | wrap | gap(var::space(1));
    }

    static Node knob_rows(const Model& m) {
        const Ctx& c = ctx();
        return v(each(kKnobs, [&](const KnobDef& d, std::size_t i) {
                   const float v_ = m.knobs.*d.field;
                   const float f = (v_ - d.lo) / (d.hi - d.lo);
                   return h(text_of(d.label) | font(var::font_sm) | width(78),
                            slider(c, d.label, f, kKnobW)
                                | id_of("knob", i),
                            (i == 0 ? textf("{:.0f}\u00B0", v_) : textf("{:.2f}", v_))
                                | font(var::font_xs) | fg(var::text_secondary) | width(40)
                                | text_align(TextAlign::right))
                        | gap(var::space(1)) | align(Align::center);
               }))
             | gap(var::space(0.75f));
    }

    static Node swatches() {
        constexpr std::array roles{var::background, var::surface, var::surface_raised, var::border_strong,
                                   var::text_primary, var::accent, var::success, var::warning, var::danger,
                                   var::info};
        return h(each(roles, [](var::Token r) {
                   return box() | size(20, 20) | radius(var::radius_full) | bg(r)
                        | border(var::line, var::border_strong) | transition();
               }))
             | gap(var::space(0.5f)) | wrap;
    }

    static Node designer(const Model& m) {
        const Theme& t = ctx().theme;
        return dsl::card(t,
                    text<"mayag studio"> | ui::h1,
                    text<"Drag anything. The interface is the theme."> | ui::muted,
                    text<"LOOK"> | ui::caps,
                    preset_chips(m),
                    h(button<"Remix">(t) | dsl::id<"remix">,
                      text<"R \u00B7 1-6 \u00B7 Space"> | font(var::font_xs) | fg(var::text_disabled))
                        | gap(var::space(1.5f)) | align(Align::center),
                    text<"KNOBS"> | ui::caps,
                    knob_rows(m),
                    h(text<"Dark"> | font(var::font_sm) | grow(),
                      switch_control(ctx(), "dark", m.knobs.dark))
                        | align(Align::center),
                    text<"TOKENS"> | ui::caps,
                    swatches())
             | width(316) | rigid | gap(var::space(1.5f)) | dsl::id<"designer">;
    }

    // ── the app being designed ──────────────────────────────────────────

    static Node header(const Model& m) {
        return h(box() | size(30, 30) | radius(var::radius_sm)
                     | linear_gradient(var::accent, var::info, {0, 0}, {1, 1}),
                 text<"Pulse"> | ui::h1,
                 h(each(kNav, [&](const char* n, std::size_t i) {
                       return h(text_of(n)) | ui::nav_item | selected(m.nav == static_cast<int>(i))
                            | id_of("nav", i);
                   })) | gap(var::space(0.5f)),
                 spacer(),
                 h(text<"Search artists, tracks\u2026">) | ui::search,
                 h(text<"AY"> | font(var::font_sm) | bold | fg(var::on_accent)) | center
                     | size(38, 38) | radius(var::radius_full)
                     | linear_gradient(var::accent, var::danger, {0, 0}, {1, 1}))
             | gap(var::space(2)) | align(Align::center);
    }

    static Node stats(const Model& m) {
        const Theme& t = ctx().theme;
        return h(each(kStats, [&](const StatDef& s, std::size_t si) {
                   std::array<float, 14> bars{};
                   for (std::size_t i = 0; i < bars.size(); ++i) {
                       const float base = 0.35f + 0.3f * std::sin(static_cast<float>(i * 7 + si * 3) * 0.9f);
                       const float live = m.playing
                           ? 0.25f * std::sin(static_cast<float>(m.clock) * (3.0f + static_cast<float>(i % 5))
                                              + static_cast<float>(i))
                           : 0.0f;
                       bars[i] = std::clamp(base + live + 0.2f, 0.08f, 1.0f);
                   }
                   return dsl::card(t,
                              text_of(s.label) | ui::caps,
                              h(text_of(s.value) | font(var::font_2xl) | bold, spacer(),
                                h(text_of(s.delta)) | (s.up ? ui::delta_up : ui::delta_down))
                                  | align(Align::center),
                              h(each(bars, [](float f) {
                                    return box() | width(8) | height(5.0f + f * 34.0f)
                                         | radius(var::radius_sm) | bg(var::accent.fade(0.30f + 0.70f * f));
                                })) | gap(4) | align(Align::end) | height(40))
                        | grow();
               }))
             | gap(var::space(2));
    }

    static Node tracks(const Model& m) {
        const Theme& t = ctx().theme;
        return dsl::card(t,
                    h(text<"Top tracks"> | ui::h2, spacer(),
                      button<"See all">(t, ButtonVariant::ghost) | dsl::id<"see_all">)
                        | align(Align::center),
                    v(each(kTracks, [&](const TrackDef& tr, std::size_t i) {
                          const bool cur = m.track == static_cast<int>(i);
                          return h(textf("{:02}", i + 1) | font(var::font_xs) | fg(var::text_disabled) | width(20),
                                   box() | size(40, 40) | radius(var::radius_sm)
                                       | (i % 3 == 0 ? linear_gradient(var::accent, var::info, {0, 0}, {1, 1})
                                          : i % 3 == 1 ? linear_gradient(var::danger, var::warning, {0, 0}, {1, 1})
                                                       : linear_gradient(var::success, var::accent, {0, 0}, {1, 1})),
                                   v(text_of(tr.title) | semibold, text_of(tr.artist) | ui::muted)
                                       | gap(2) | grow(),
                                   when(cur && m.playing, text<"\u25B6 playing"> | font(var::font_xs) | bold
                                                              | fg(var::accent)),
                                   text_of(tr.len) | ui::muted,
                                   text<"\u203A"> | font(var::font_lg) | ui::row_hint)
                               | ui::track_row | selected(cur) | id_of("track", i);
                      })) | gap(2))
             | grow() | min_size(0, 0);
    }

    static Node now_playing(const Model& m) {
        const Theme& t = ctx().theme;
        const auto& tr = kTracks[static_cast<std::size_t>(m.track)];
        const int secs = static_cast<int>(m.progress * 222.0f);
        return dsl::card(t,
                    z(box() | width(pct(100)) | height(118) | radius(var::radius_lg)
                          | linear_gradient(var::accent, var::info, {0, 0}, {1, 1}),
                      h(text<"NOW PLAYING"> | font(var::font_xs) | bold | tracking(1.4f) | fg(var::on_accent))
                          | pad(var::space(0.5f), var::space(1)) | radius(var::radius_full)
                          | bg(colors::black.fade(0.25f)) | absolute(12, 12)),
                    v(text_of(tr.title) | ui::h2, text_of(tr.artist) | ui::muted) | gap(2),
                    slider(ctx(), "seek", m.progress, kSeekW),
                    h(textf("{}:{:02}", secs / 60, secs % 60) | font(var::font_xs) | fg(var::text_secondary),
                      spacer(), text_of(tr.len) | font(var::font_xs) | fg(var::text_secondary)),
                    h(button<"Prev">(t, ButtonVariant::ghost) | dsl::id<"prev">,
                      when(m.playing, button<"Pause">(t) | dsl::id<"play">, button<"Play">(t) | dsl::id<"play">),
                      button<"Next">(t, ButtonVariant::ghost) | dsl::id<"next">)
                        | gap(var::space(1)) | justify(Justify::center),
                    h(text<"Shuffle"> | font(var::font_sm) | grow(), switch_control(ctx(), "shuffle", m.shuffle))
                        | align(Align::center))
             | width(kSeekW + 2.0f * 40.0f) | rigid | dsl::id<"player">;
    }

    /// The same player, in `t`. Built inside themed(...), so every widget --
    /// card, buttons, switch, slider -- is drawn in that world's theme, and
    /// all of them read and write the ONE model.
    static Node mini_player(const Ctx& c, const Model& m, std::size_t world, const char* name) {
        const Theme& t = c.theme;
        const auto& tr = kTracks[static_cast<std::size_t>(m.track)];
        return dsl::card(t,
                    h(text_of(name) | ui::caps, spacer(), switch_control(c, "", m.shuffle) | id_of("w_shuffle", world))
                        | align(Align::center),
                    h(box() | size(44, 44) | radius(var::radius_md)
                          | linear_gradient(var::accent, var::info, {0, 0}, {1, 1}),
                      v(text_of(tr.title) | semibold, text_of(tr.artist) | ui::muted) | gap(2) | grow())
                        | gap(var::space(1.25f)) | align(Align::center),
                    slider(c, "", m.progress, kMiniW) | id_of("w_seek", world),
                    h(button<"Prev">(t, ButtonVariant::ghost) | id_of("w_prev", world),
                      when(m.playing, button<"Pause">(t) | id_of("w_play", world),
                                      button<"Play">(t) | id_of("w_play", world)))
                        | gap(var::space(1)))
             | grow() | id_of("world", world);
    }

    static Node worlds(const Model& m) {
        return v(h(text<"Same app. Same state. Three worlds."> | ui::h2, spacer(),
                   text<"themed(look, ...) \u2014 press Play in any of them"> | ui::muted)
                     | align(Align::center),
                 h(themed(looks::brutal(), [&](const Ctx& c) { return mini_player(c, m, 0, "BRUTAL"); }),
                   themed(looks::glass(), [&](const Ctx& c) { return mini_player(c, m, 1, "GLASS"); }),
                   themed(looks::terminal(), [&](const Ctx& c) { return mini_player(c, m, 2, "TERMINAL"); }))
                     | gap(var::space(2)))
             | gap(var::space(1.5f));
    }

    /// Light behind the glass: two soft glows in the theme's accent and info
    /// colours. Their strength is the theme's DEPTH, so flat looks (brutal,
    /// terminal) get none, and glass gets plenty -- the view never asks.
    static Node glows() {
        const Theme& t = ctx().theme;
        const Vec2 sz = ctx().size;
        const float k = std::clamp(t.depth_scale, 0.0f, 2.0f);
        const auto glow = [&](var::Token c, float d, float x, float y) {
            return box() | size(d, d) | radius(var::radius_full) | bg(c.fade(0.10f * k))
                 | shadow(d * 0.45f, c.fade(0.22f * k)) | absolute(x, y);
        };
        return z(box() | width(pct(100)) | height(pct(100)),
                 glow(var::accent, 560, sz.x * 0.38f, -220),
                 glow(var::info, 460, sz.x * 0.78f, sz.y * 0.55f),
                 glow(var::danger, 320, sz.x * 0.20f, sz.y * 0.70f))
             | width(pct(100)) | height(pct(100)) | absolute(0, 0);
    }

    // What a panel reads from the model, for memo(). The clock and progress
    // aren't in these, so playback only rebuilds the panels that show them.
    static std::uint64_t knobs_key(const Knobs& k) {
        return MemoKey{}.add(static_cast<int>(k.skin)).add(k.hue).add(k.chroma).add(k.lum).add(k.roundness)
            .add(k.density).add(k.type).add(k.depth).add(k.motion).add(k.lines).add(k.dark);
    }

    static Node view(const Model& m) {
        Node app = v(memo(MemoKey{}.add(m.nav), [&] { return header(m); }),
                     stats(m),
                     h(memo(MemoKey{}.add(m.track).add(m.playing), [&] { return tracks(m); }), now_playing(m))
                         | gap(var::space(2)),
                     worlds(m))
                 | gap(var::space(2)) | pad(var::space(0.5f), var::space(1), var::space(3), var::space(0.5f));
        return h(glows(),
                 memo(MemoKey{}.add(m.preset).add(knobs_key(m.knobs)), [&] { return designer(m); }),
                 v(scroll_view(ctx(), "main", m.page, node(std::move(app))) | grow() | min_size(0, 0))
                     | grow() | min_size(0, 0))
             | gap(var::space(2.5f)) | pad(var::space(2.5f))
             | width(pct(100)) | height(pct(100)) | clip
             | bg(var::background) | fg(var::text_primary) | font(var::font_md);
    }

    // ── input ───────────────────────────────────────────────────────────

    static std::optional<Msg> on(const Model&, const Clicked& c) {
        for (std::size_t i = 0; i < kPresets.size(); ++i) if (is(c, "preset", i)) return Preset{static_cast<int>(i)};
        for (std::size_t i = 0; i < kNav.size(); ++i) if (is(c, "nav", i)) return Nav{static_cast<int>(i)};
        for (std::size_t i = 0; i < kTracks.size(); ++i) if (is(c, "track", i)) return Track{static_cast<int>(i)};
        if (is(c, "remix")) return Remix{};
        if (is(c, "dark")) return Dark{};
        if (is(c, "play")) return Play{};
        if (is(c, "prev")) return Prev{};
        if (is(c, "next")) return Next{};
        if (is(c, "shuffle")) return Shuffle{};
        for (std::size_t w = 0; w < 3; ++w) {
            if (is(c, "w_play", w)) return Play{};
            if (is(c, "w_prev", w)) return Prev{};
            if (is(c, "w_shuffle", w)) return Shuffle{};
        }
        return std::nullopt;
    }
    static std::optional<Msg> slide(std::uint64_t node, float x) {
        for (std::size_t i = 0; i < kKnobs.size(); ++i)
            if (node == node_id("knob", i)) return Knob{static_cast<int>(i), x / kKnobW};
        if (node == node_id("seek")) return Seek{x / kSeekW};
        for (std::size_t w = 0; w < 3; ++w)
            if (node == node_id("w_seek", w)) return Seek{x / kMiniW};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Pressed& p) {
        if (is(p, "main", part_bar)) return BarPress{p.local.y};
        return slide(p.node, p.local.x);
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "main", part_bar)) return BarDrag{d.delta.y};
        return slide(d.node, d.local.x);
    }
    static std::optional<Msg> on(const Model&, const Scrolled& s) {
        if (is(s, "main")) return Scroll{s.delta.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.key == Key::escape) return Quit{};
        if (k.key == Key::space) return Play{};
        if (k.key == Key::r) return Remix{};
        if (k.key >= Key::n1 && k.key <= Key::n6)
            return Preset{static_cast<int>(k.key) - static_cast<int>(Key::n1)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
};

// ════════════════════════════════════════════════════════════════════════
// Headless self-check
// ════════════════════════════════════════════════════════════════════════

namespace {
bool near(Color<Srgb> a, Color<Srgb> b, float eps = 2e-3f) {
    return num::abs(a.c0 - b.c0) <= eps && num::abs(a.c1 - b.c1) <= eps &&
           num::abs(a.c2 - b.c2) <= eps && num::abs(a.a - b.a) <= eps;
}
}  // namespace

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {1440, 900});
    AppConfig cfg;
    cfg.title = "mayag studio";
    cfg.size  = opts.size;
    cfg.theme = theme_for(kPresets[0].k);

    return demo::run<Studio>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const std::string& what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
            if (!c) ++fails;
        };
        const auto by_id = [&](std::uint64_t nid) { return rt.host().tree().find(nid); };
        const auto centre = [&](std::uint64_t nid) {
            const Node* n = by_id(nid);
            return n ? n->frame().center() : Vec2{};
        };
        const auto settle = [&] {
            rt.window().drive_frames(true);
            for (int i = 0; i < 600 && (i < 3 || rt.host().transitioning()); ++i) rt.tick();
            rt.window().drive_frames(false);
        };
        rt.tick();
        settle();

        std::printf("the theme is the model\n");
        const Theme mid = theme_for(kPresets[0].k);
        ok(rt.host().theme() == mid, "the app starts in Midnight");
        const Node* play = by_id(node_id("play"));
        ok(play && near(play->style().fill.color, mid.accent), "the player's Play button wears the accent");

        std::printf("presets morph the WHOLE interface\n");
        rt.window().click(centre(node_id("preset", 2)));     // Brutal
        rt.tick();
        const Theme brutal = theme_for(kPresets[2].k);
        ok(rt.model().preset == 2, "clicking a chip selects the look");
        ok(rt.host().transitioning(), "and the host starts a morph, with no code in the app");
        const float r_mid = rt.host().theme().radius_medium;
        ok(r_mid < mid.radius_medium && r_mid > brutal.radius_medium,
           "mid-morph the radius is between the two looks (" + std::to_string(r_mid) + ")");
        settle();
        ok(rt.host().theme() == brutal, "the morph lands exactly on the new theme");
        play = by_id(node_id("play"));
        ok(play && play->style().corners.tl == 0.0f && play->style().shadow_count >= 1 &&
           play->style().shadows[0].offset.x == 4.0f,
           "Play is now square with a hard ink shadow: the sheet changed the widget's SHAPE");
        ok(!rt.host().transitioning(), "then it rests: 0% CPU");

        std::printf("three worlds, one state\n");
        const Node* w0 = by_id(node_id("w_play", 0));
        const Node* w1 = by_id(node_id("world", 1));
        const Node* w2 = by_id(node_id("w_play", 2));
        ok(w0 && w0->style().corners.tl == 0.0f, "the brutal world's button is square");
        ok(w1 && w1->style().backdrop.blur > 0.0f, "the glass world's card blurs what is behind it");
        ok(w2 && w2->style().stroke.width > 0.0f && near(w2->style().stroke.color, looks::terminal().accent),
           "the terminal world outlines in phosphor");
        rt.send(Studio::Play{});
        settle();
        ok(rt.model().playing, "Play from any world drives the one model");
        ok(by_id(node_id("w_play", 1)) && by_id(node_id("play")), "and every player re-renders as Pause");
        rt.send(Studio::Play{});
        settle();

        std::printf("knobs\n");
        const Color<Srgb> before = rt.host().theme().accent;
        rt.window().push(MouseDown{centre(node_id("knob", 0)) - Vec2{40, 0}, MouseButton::left, {}, 1});
        rt.window().push(MouseUp{centre(node_id("knob", 0)) - Vec2{40, 0}, MouseButton::left, {}});
        rt.tick();
        settle();
        ok(rt.model().preset == -1 && !near(rt.host().theme().accent, before),
           "dragging the Hue knob re-derives the accent");
        rt.send(Studio::Knob{2, 1.0f});   // roundness to max
        settle();
        ok(num::abs(rt.host().theme().radius_medium - theme_for(rt.model().knobs).radius_medium) < 1e-4f,
           "Roundness reshapes every corner");

        std::printf("reduced motion\n");
        rt.send(Studio::Knob{6, 0.0f});   // motion 0
        settle();
        rt.send(Studio::Preset{3});
        ok(!rt.host().transitioning(), "Motion 0: the next look arrives instantly, no morph");
        ok(rt.host().theme().radius_medium != brutal.radius_medium, "and it did arrive");

        std::printf("remix\n");
        rt.send(Studio::Remix{});
        settle();
        ok(!rt.host().transitioning(), "everything settles");
        ok(!rt.host().busy_loop_detected(), "and no spring or morph ever ran away (no busy loop)");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}
