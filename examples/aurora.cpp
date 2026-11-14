// examples/aurora.cpp — a live, deliberately GPU-heavy visual showcase.
//
// Every other example demonstrates a piece of the architecture. This one
// exists to make the RENDERER work: conic and radial gradients, layered
// shadows, blurred glows, rings, translucency and a couple of dozen springs
// all animating at once, over a backdrop-blurred panel.
//
// It is also the honest stress case for the claims in the README. Everything
// on screen is a signed distance field on the same instanced quad, so this
// whole scene is a handful of draw calls, and the antialiasing is analytic at
// any size — which is exactly the thing that is easy to say and hard to do.
//
// Interactions
//   click a swatch   retune the palette (every colour is derived from one hue)
//   space            start/stop the motion
//   +/-              add or remove orbiting particles
//   escape           quit
//
// Build:  cmake --build build && ./build/examples/mayag_aurora
// (--headless self-checks without a window; --png/--gpu write a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

struct Aurora {
    static constexpr int   max_orbs   = 14;
    static constexpr float tau        = 6.28318531f;

    // ── state ───────────────────────────────────────────────────────────
    struct Model {
        float           time    = 0.0f;   // drives every orbit
        bool            running = true;
        int             orbs    = 8;
        int             palette = 0;

        // One spring per animated quantity. They are independent, so an
        // interaction never fights an in-flight animation: retargeting a
        // spring carries its velocity instead of restarting from zero.
        Animated<float> hue{212.0f};      // the whole palette derives from this
        Animated<float> bloom{0.0f};      // pulses when the palette changes
        Animated<float> density{8.0f};    // orb count, animated so it eases
    };

    // ── messages ────────────────────────────────────────────────────────
    struct Tick { float dt; };
    struct Toggle {};
    struct More {};
    struct Less {};
    struct Pick { int index; };
    struct Quit {};
    using Msg = std::variant<Tick, Toggle, More, Less, Pick, Quit>;
    using Cmd = jaal::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    // The palette swatches are a dynamic list, so their ids are folded from a
    // base id + index. Declaring them here keeps the view, the input
    // and the handler reading from ONE source.
    static constexpr std::uint64_t swatch_base = node_id("swatch");
    static std::uint64_t swatch(int i) { return node_id("swatch", static_cast<std::size_t>(i)); }

    // Base hues the swatches select between.
    static constexpr float hues[5] = {212.0f, 286.0f, 336.0f, 28.0f, 158.0f};

    // ── init ────────────────────────────────────────────────────────────
    static Cmd init(Model&) { return SetTitle{"mayag — aurora"}; }

    // ── update ──────────────────────────────────────────────────────────
    static Cmd update(Model& m, Tick t) {
        if (m.running) m.time += t.dt;
        m.hue.step(t.dt);
        m.bloom.step(t.dt);
        m.density.step(t.dt);
        return {};
    }
    static Cmd update(Model& m, Toggle) { m.running = !m.running; return {}; }
    static Cmd update(Model& m, More) {
        m.orbs = m.orbs < max_orbs ? m.orbs + 1 : m.orbs;
        m.density.to(static_cast<float>(m.orbs));
        return {};
    }
    static Cmd update(Model& m, Less) {
        m.orbs = m.orbs > 1 ? m.orbs - 1 : m.orbs;
        m.density.to(static_cast<float>(m.orbs));
        return {};
    }
    static Cmd update(Model& m, Pick p) {
        m.palette = p.index;
        m.hue.to(hues[p.index % 5]);
        m.bloom.snap(1.0f);          // flash, then settle back
        m.bloom.to(0.0f);
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── view ────────────────────────────────────────────────────────────
    static Node view(const Model& m) {
        const Ctx&   c = ctx();
        const Theme& t = c.theme;
        const float  h = static_cast<float>(m.hue);
        const float  bloom = static_cast<float>(m.bloom);

        // Absolute offsets are in PIXELS, so the scene needs the real
        // viewport to place anything proportionally.
        const Vec2 vp = c.size;

        // The whole scene is derived from ONE hue. That is the perceptual
        // colour argument made visible: rotating a hue in Oklch keeps every
        // derived colour equally light and equally saturated, so the palette
        // stays balanced at every setting instead of going muddy at some.
        const auto base   = Color<Oklch>{0.62f, 0.17f, h}.to<Srgb>();
        const auto warm   = rotate_hue(base,  48.0f);
        const auto cool   = rotate_hue(base, -52.0f);
        const auto accent = lighten(base, 0.16f);

        // z() is variadic over DSL expressions, so the layers are composed
        // here rather than passed around as built Nodes.
        std::vector<Node> layers;
        layers.push_back(backdrop(t, base, cool, bloom, vp));
        layers.push_back(orbit_field(m, base, warm, cool, vp));
        layers.push_back(glass_panel(m, t, base, accent, vp));
        layers.push_back(hud(m, t, vp));
        return v_list(std::move(layers)) | width(pct(100)) | height(pct(100))
             | bg(t.background);
    }

    // A layered background: a broad radial wash plus a conic sweep, both
    // translucent so they mix in linear light rather than banding.
    static Node backdrop(const Theme& t, Color<Srgb> base, Color<Srgb> cool,
                         float bloom, Vec2 vp) {
        // Two full-bleed washes stacked: a radial one for depth and a conic
        // sweep on top, both translucent so they composite in LINEAR light.
        // Banding here would be the tell-tale of gamma-space blending.
        return (z(box() | width(pct(100)) | height(pct(100))
                        | radial_gradient(darken(base, 0.10f), t.background,
                                          {0.5f, 0.42f}, 0.92f),
                  box() | width(pct(100)) | height(pct(100))
                        | conic_gradient(cool.fade(0.42f + bloom * 0.35f),
                                         base.fade(0.10f), {0.5f, 0.5f}))
                | width(px(vp.x)) | height(px(vp.y))
                | absolute(0.0f, 0.0f)).build();
    }

    // The moving part: N orbs on independent elliptical paths, each a soft
    // radial gradient with a glow. Positions are a pure function of time, so
    // there is no per-orb state to keep in sync.
    static Node orbit_field(const Model& m, Color<Srgb> base,
                            Color<Srgb> warm, Color<Srgb> cool, Vec2 vp) {
        const int n = m.orbs;
        std::vector<Node> orbs;
        orbs.reserve(static_cast<std::size_t>(n) + 1);

        for (int i = 0; i < n; ++i) {
            const float f     = static_cast<float>(i) / static_cast<float>(max_orbs);
            const float phase = m.time * (0.22f + f * 0.42f) + f * tau;
            const float rx    = vp.x * (0.30f + f * 0.16f);
            const float ry    = vp.y * (0.20f + f * 0.14f);

            const float size_px = 120.0f + f * 260.0f;
            const float cx = vp.x * 0.5f + std::cos(phase) * rx;
            const float cy = vp.y * 0.46f + std::sin(phase * 1.31f) * ry;

            const auto  tone  = mix_perceptual(warm, cool, f);
            // Bright cores, fading to nothing at the rim. Additive-looking
            // overlap comes free from compositing many of these in linear
            // light -- in gamma space the same stack goes muddy.
            const float alpha = 0.55f + (1.0f - f) * 0.35f;

            orbs.push_back(
                (box() | width(px(size_px)) | height(px(size_px))
                       | radial_gradient(tone.fade(alpha), tone.fade(0.0f),
                                         {0.5f, 0.5f}, 0.5f)
                       | radius(size_px * 0.5f)
                       | absolute(cx - size_px * 0.5f, cy - size_px * 0.5f)).build());
        }

        // A bright core that tracks the slowest orbit, to anchor the eye.
        const float cp = m.time * 0.22f;
        const float core_x = vp.x * 0.5f + std::cos(cp) * vp.x * 0.30f;
        const float core_y = vp.y * 0.46f + std::sin(cp * 1.31f) * vp.y * 0.20f;
        orbs.push_back(
            (box() | width(px(34)) | height(px(34)) | radius(17)
                   | bg(lighten(base, 0.42f))
                   | shadow(70.0f, base.fade(0.95f), {0.0f, 0.0f})
                   | absolute(core_x - 17.0f, core_y - 17.0f)).build());

        // Every orb is absolutely positioned, so the container's axis never
        // applies to them; v_list is just the vector-shaped box to hold them.
        return v_list(std::move(orbs)) | width(px(vp.x)) | height(px(vp.y))
             | absolute(0.0f, 0.0f);
    }

    // Frosted glass over the animation: the panel blurs whatever is behind it,
    // which is the case that forces a mid-frame capture on the GPU path.
    //
    // This is also the one effect where the backends legitimately disagree.
    // The GPU snapshots the colour attachment mid-frame and samples it; the
    // software rasteriser reads its framebuffer in place. Same intent, genuinely
    // different algorithms, so the blurred region differs by more than an
    // antialiased edge would. Measured on this scene: GPU-vs-CPU mean|d| is
    // 0.31/255 with the blur removed and 1.68/255 with it, and ALL of the
    // excess sits under this panel. Worth knowing before treating it as a
    // regression — the parity gate in tests/test_vulkan.cpp deliberately does
    // not include a backdrop for the same reason.
    static Node glass_panel(const Model& m, const Theme& t,
                            Color<Srgb> base, Color<Srgb> accent, Vec2 vp) {
        return (v(
            text<"AURORA"> | font(13) | semibold | tracking(4.0f) | fg(accent),
            text<"every shape here is one distance field"> | font(12)
                | fg(t.text_secondary),
            spacer(10),
            // A conic ring reading as a dial for the orb count: the sweep is
            // one gradient, the hole is one rounded box on top.
            z(box() | width(px(96)) | height(px(96)) | radius(48)
                    | conic_gradient(accent, accent.fade(0.06f), {0.5f, 0.5f}),
              box() | width(px(74)) | height(px(74)) | radius(37)
                    | bg(t.surface.fade(0.92f))
                    | absolute(11.0f, 11.0f),
              v(text_owned(std::to_string(m.orbs)) | font(26) | bold
                  | fg(t.text_primary))
                | center | width(px(96)) | height(px(96))
                | absolute(0.0f, 0.0f)),
            spacer(6),
            text_owned(m.running ? "running" : "paused") | font(11) | semibold
                | fg(m.running ? accent : t.text_secondary)
                | id<"play">)
            | gap(4) | center
            | pad(22, 26)
            | backdrop_blur(18.0f, 1.06f)
            | bg(t.surface.fade(0.34f))
            | border(1, base.fade(0.42f))
            | radius(t.radius_large)
            | elevation(16)
            | absolute(vp.x * 0.5f - 150.0f, vp.y * 0.5f - 150.0f)).build();
    }

    // Palette swatches along the bottom: a dynamic list wired by index.
    static Node hud(const Model& m, const Theme& t, Vec2 vp) {
        std::vector<Node> chips;
        chips.reserve(5);
        for (int i = 0; i < 5; ++i) {
            const auto tone = Color<Oklch>{0.62f, 0.17f, hues[i]}.to<Srgb>();
            const bool on   = m.palette == i;
            const float d   = on ? 34.0f : 26.0f;
            chips.push_back(
                (box() | width(px(d)) | height(px(d))
                       | bg(tone)
                       | radius(d * 0.5f)
                       | border(on ? 2.0f : 0.0f, colors::white.fade(0.85f))
                       | elevation(on ? 10.0f : 4.0f)
                       | dsl::Id{swatch(i)}).build());
        }

        return (v(h_list(std::move(chips), 12.0f) | center,
                  text<"click a colour · space pauses · +/- orbs"> | font(11)
                      | fg(t.text_secondary.fade(0.85f)))
             | gap(10) | center
             | width(px(vp.x))
             | absolute(0.0f, vp.y - 108.0f)).build();
    }

    // ── input ───────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model&, const Clicked& c) {
        if (c.count != 1) return std::nullopt;
        if (is(c, "play")) return Toggle{};
        for (int i = 0; i < 5; ++i)
            if (c.node == swatch(i)) return Pick{i};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{Key::space,  Toggle{}},
                                      {Key::equal,  More{}},
                                      {Key::minus,  Less{}},
                                      {Key::escape, Quit{}}};
        return keys(k);
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // ── subscribe ───────────────────────────────────────────────────────
    static Sub subscribe(const Model& m) {
        // Frames are requested while anything is moving. When the scene is
        // paused AND every spring has settled, this yields nothing and the
        // app idles at 0% CPU.
        const bool moving = m.running || m.hue.animating() ||
                            m.bloom.animating() || m.density.animating();
        return when(moving, Sub::on_frame([](float dt) -> Msg { return Tick{dt}; }));
    }
};

static_assert(mayag::Program<Aurora>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {820, 620});

    AppConfig cfg;
    cfg.title = "mayag — aurora";
    cfg.size  = opts.size;

    return demo::run<Aurora>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        std::printf("aurora headless\n");

        ok(rt.model().running, "starts animating");
        ok(rt.model().orbs == 8, "starts with 8 orbs");

        // Palette selection routes by index through one shared action. The
        // test resolves the id the SAME way the view does, so a change to the
        // id scheme cannot silently desync the two.
        rt.click(node_id("swatch", 2));
        rt.tick();
        ok(rt.model().palette == 2, "clicking a swatch selects that palette");
        ok(rt.model().hue.animating(), "the hue springs toward the new palette");

        rt.window().press_key(Key::space);
        rt.tick();
        ok(!rt.model().running, "space pauses the motion");

        rt.window().press_key(Key::equal);
        rt.tick();
        ok(rt.model().orbs == 9, "+ adds an orb");

        // Let every spring settle, then confirm the app stops asking for
        // frames: paused really means idle, not merely still-looking.
        rt.window().drive_frames(true);
        for (int i = 0; i < 900 && rt.model().hue.animating(); ++i) rt.tick();
        rt.window().drive_frames(false);
        ok(!rt.model().hue.animating(), "springs settle on their own");
        ok(rt.window().frames_presented() > 1, "the scene rendered");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    });
}
