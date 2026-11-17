// examples/babel.cpp — every script on Earth, orbiting, shaped live.
//
// Twenty-six greetings circle a hero line on a tilted ellipse, in depth:
// the ones in front are larger, brighter and glow; the ones behind fade into
// a starfield. The hero types itself out one character at a time, so you can
// WATCH the shaper work: Arabic letters change form as their neighbours
// arrive, a Devanagari virama shows until the next consonant fuses it into a
// conjunct, a pre-base matra jumps in front of its consonant, Thai sara am
// splits, Hangul jamo compose. Its weight breathes through the variable
// font's wght axis, and the palette is derived from ONE hue per language.
//
// None of this uses HarfBuzz, ICU or CoreText: bidi (UAX #9), OpenType GSUB/
// GPOS and every script shaper are mayag's own, matching HarfBuzz exactly.
//
// Interactions
//   click a greeting   make it the hero
//   type               write your own hero line, in any script (try mixing
//                      Arabic and English: the bidi algorithm orders it)
//   backspace          delete;  enter   back to the orbit
//   ← →                previous / next language;  tab   pause the orbit
//   escape             quit
//
// Build:  cmake --build build && ./build/examples/mayag_babel
// (--headless self-checks without a window; --png/--gpu write a screenshot)

#include <mayag/mayag.hpp>

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {

struct Lang { const char* hello; const char* name; const char* script; };

// One greeting per script family the engine shapes. Order is the orbit's.
constexpr std::array<Lang, 26> langs{{
    {"Hello, world",          "English",    "Latin"},
    {"مرحبا بالعالم",          "العربية",    "Arabic · joining"},
    {"नमस्ते दुनिया",            "हिन्दी",      "Devanagari · conjuncts"},
    {"שָׁלוֹם עוֹלָם",           "עברית",      "Hebrew · points"},
    {"สวัสดีชาวโลก",          "ไทย",        "Thai · sara am"},
    {"안녕하세요 세계",         "한국어",      "Hangul"},
    {"ওহে বিশ্ব",               "বাংলা",       "Bengali · split matras"},
    {"こんにちは世界",          "日本語",      "Kana + Han"},
    {"Γειά σου Κόσμε",        "Ελληνικά",   "Greek"},
    {"வணக்கம் உலகம்",         "தமிழ்",       "Tamil"},
    {"سلام دنیا",             "فارسی",      "Arabic · Persian"},
    {"你好，世界",             "中文",        "Han"},
    {"Привет, мир",           "Русский",    "Cyrillic"},
    {"ਸਤਿ ਸ੍ਰੀ ਅਕਾਲ",           "ਪੰਜਾਬੀ",      "Gurmukhi"},
    {"გამარჯობა მსოფლიო",     "ქართული",    "Georgian"},
    {"မင်္ဂလာပါ ကမ္ဘာ",          "မြန်မာ",       "Myanmar · kinzi"},
    {"ಹಲೋ ಪ್ರಪಂಚ",             "ಕನ್ನಡ",       "Kannada"},
    {"Բարեւ աշխարհ",          "Հայերեն",    "Armenian"},
    {"ܫܠܡܐ ܥܠܡܐ",             "ܣܘܪܝܝܐ",     "Syriac · alaph forms"},
    {"హలో ప్రపంచం",            "తెలుగు",      "Telugu"},
    {"ሰላም ዓለም",              "አማርኛ",       "Ethiopic"},
    {"សួស្តីពិភពលោក",          "ខ្មែរ",        "Khmer · coeng"},
    {"ഹലോ ലോകം",              "മലയാളം",     "Malayalam"},
    {"བཀྲ་ཤིས་བདེ་ལེགས།",       "བོད་ཡིག",      "Tibetan · stacks"},
    {"ᠰᠠᠢᠨ ᠪᠠᠢᠨ᠎ᠠ ᠤᠤ",          "ᠮᠣᠩᠭᠣᠯ",      "Mongolian · USE joining"},
    {"👋🏽 🌍 👨‍👩‍👧‍👦",           "emoji",      "ZWJ sequences"},
}};
constexpr int n_langs = static_cast<int>(langs.size());
constexpr float tau = 6.28318531f;

// ── a little UTF-8 (the example works in code points, not bytes) ──────────
std::size_t cp_count(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0u) != 0x80u;
    return n;
}
std::string cp_prefix(std::string_view s, std::size_t n) {
    std::size_t i = 0;
    for (; i < s.size() && n > 0; --n) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) ++i;
    }
    return std::string{s.substr(0, i)};
}
void pop_cp(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u) s.pop_back();
    if (!s.empty()) s.pop_back();
}

float hue_of(int i) { return std::fmod(18.0f + static_cast<float>(i) * (360.0f / static_cast<float>(n_langs)), 360.0f); }

// Cheap deterministic noise for the starfield.
float hash01(int i, int salt) {
    std::uint32_t x = static_cast<std::uint32_t>(i) * 2654435761u ^ static_cast<std::uint32_t>(salt) * 40503u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return static_cast<float>(x & 0xFFFFFF) / 16777215.0f;
}

}  // namespace

struct Babel {
    // ── state ───────────────────────────────────────────────────────────
    struct Model {
        float time = 0.0f;        // drives the orbit and the breathing
        float orbit = 0.0f;       // orbit angle; stops when paused
        bool  running = true;
        int   hero = 0;           // which language is the hero
        float hero_since = 0.0f;  // when it became the hero (typewriter clock)
        std::string typed;        // the user's own hero text, if any
        Animated<float> hue{hue_of(0)};
        Animated<float> flash{0.0f};
    };

    // ── messages ────────────────────────────────────────────────────────
    struct Tick { float dt; };
    struct Pick { int index; };
    struct Step { int by; };
    struct Typed { std::string text; };
    struct Back {};
    struct Clear {};
    struct Toggle {};
    struct Quit {};
    using Msg = std::variant<Tick, Pick, Step, Typed, Back, Clear, Toggle, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static std::uint64_t lang_id(int i) { return node_id("lang", static_cast<std::size_t>(i)); }

    static constexpr float cycle_seconds = 5.5f;   // auto-advance the hero

    // ── init / update ───────────────────────────────────────────────────
    static Cmd init(Model&) { return SetTitle{"mayag — babel"}; }

    static void make_hero(Model& m, int i) {
        m.hero = ((i % n_langs) + n_langs) % n_langs;
        m.hero_since = m.time;
        m.hue.to(hue_of(m.hero));
        m.flash.snap(1.0f);
        m.flash.to(0.0f);
    }

    static Cmd update(Model& m, Tick t) {
        m.time += t.dt;
        if (m.running) m.orbit += t.dt * 0.16f;
        m.hue.step(static_cast<double>(t.dt), Spring::gentle());
        m.flash.step(static_cast<double>(t.dt));
        // The orbit hands the hero on by itself while nobody is typing.
        if (m.running && m.typed.empty() && m.time - m.hero_since > cycle_seconds) make_hero(m, m.hero + 1);
        return {};
    }
    static Cmd update(Model& m, Pick p) { m.typed.clear(); make_hero(m, p.index); return {}; }
    static Cmd update(Model& m, Step s) { m.typed.clear(); make_hero(m, m.hero + s.by); return {}; }
    static Cmd update(Model& m, Typed t) {
        if (m.typed.size() < 160) m.typed += t.text;
        m.hero_since = m.time;
        return {};
    }
    static Cmd update(Model& m, Back) { pop_cp(m.typed); m.hero_since = m.time; return {}; }
    static Cmd update(Model& m, Clear) { m.typed.clear(); make_hero(m, m.hero); return {}; }
    static Cmd update(Model& m, Toggle) { m.running = !m.running; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── view ────────────────────────────────────────────────────────────
    static Node view(const Model& m) {
        const Ctx& c = ctx();
        const Vec2 vp = c.size;
        const float h = static_cast<float>(m.hue);
        const auto base = Color<Oklch>{0.66f, 0.16f, h}.to<Srgb>();
        const auto deep = Color<Oklch>{0.16f, 0.05f, h + 40.0f}.to<Srgb>();
        const auto night = Color<Oklch>{0.10f, 0.03f, h - 30.0f}.to<Srgb>();

        std::vector<Node> layers;
        layers.push_back(backdrop(m, base, deep, night, vp));
        layers.push_back(stars(m, vp));
        add_orbit(layers, m, vp, /*front=*/false);
        layers.push_back(hero(m, base, vp));
        add_orbit(layers, m, vp, /*front=*/true);
        layers.push_back(ribbon(base, vp));
        return v_list(std::move(layers)) | width(pct(100)) | height(pct(100)) | bg(night);
    }

    // A deep radial wash plus a slow conic sweep, both from the hero's hue.
    static Node backdrop(const Model& m, Color<Srgb> base, Color<Srgb> deep, Color<Srgb> night, Vec2 vp) {
        const float flash = static_cast<float>(m.flash);
        return (z(box() | width(pct(100)) | height(pct(100))
                        | radial_gradient(deep, night, {0.5f, 0.46f}, 0.95f),
                  box() | width(pct(100)) | height(pct(100))
                        | conic_gradient(base.fade(0.10f + flash * 0.22f), night.fade(0.0f), {0.5f, 0.46f}),
                  box() | width(pct(100)) | height(pct(100))
                        | radial_gradient(base.fade(0.16f + flash * 0.30f), base.fade(0.0f), {0.5f, 0.46f}, 0.42f))
                | width(px(vp.x)) | height(px(vp.y)) | absolute(0.0f, 0.0f)).build();
    }

    // Twinkling points, fixed in place: a pure function of index and time.
    static Node stars(const Model& m, Vec2 vp) {
        std::vector<Node> dots;
        dots.reserve(90);
        for (int i = 0; i < 90; ++i) {
            const float x = hash01(i, 1) * vp.x, y = hash01(i, 2) * vp.y;
            const float d = 1.0f + hash01(i, 3) * 2.2f;
            const float tw = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(m.time * (0.6f + hash01(i, 4) * 2.0f) + static_cast<float>(i)));
            dots.push_back((box() | width(px(d)) | height(px(d)) | radius(d * 0.5f)
                                  | bg(colors::white.fade(0.15f + 0.55f * tw * hash01(i, 5)))
                                  | absolute(x, y)).build());
        }
        return v_list(std::move(dots)) | width(px(vp.x)) | height(px(vp.y)) | absolute(0.0f, 0.0f);
    }

    // Greetings on a tilted ellipse. Depth = sin(angle): -1 is the far side.
    // Far greetings paint before the hero and near ones after it, so the ring
    // passes BEHIND and IN FRONT of the hero line like a planet's ring.
    struct Orb { int i; float x, y, depth; };
    static std::vector<Orb> orbit_positions(const Model& m, Vec2 vp) {
        std::vector<Orb> out;
        const float cx = vp.x * 0.5f, cy = vp.y * 0.47f;
        const float rx = vp.x * 0.42f, ry = vp.y * 0.30f;
        for (int i = 0; i < n_langs; ++i) {
            if (i == m.hero && m.typed.empty()) continue;   // it is on stage
            const float a = m.orbit + static_cast<float>(i) * tau / static_cast<float>(n_langs);
            const float depth = std::sin(a);
            out.push_back({i, cx + std::cos(a) * rx, cy + depth * ry - std::cos(a) * vp.y * 0.05f, depth});
        }
        std::sort(out.begin(), out.end(), [](const Orb& a, const Orb& b) { return a.depth < b.depth; });
        return out;
    }

    static void add_orbit(std::vector<Node>& layers, const Model& m, Vec2 vp, bool front) {
        for (const Orb& o : orbit_positions(m, vp)) {
            if ((o.depth > 0.0f) != front) continue;
            const float near = 0.5f + 0.5f * o.depth;          // 0 far .. 1 near
            // Whole-pixel sizes and wght in steps of 50: a greeting sweeping
            // through depth then reuses ~13 x 6 cached shapes and rasters
            // instead of shaping and rasterising a new instance every frame.
            const float size = std::round(12.0f + near * 13.0f);
            const float wght = 400.0f + std::round(near * 5.0f) * 50.0f;
            const auto tone = Color<Oklch>{0.78f, 0.12f, hue_of(o.i)}.to<Srgb>();
            const float a = 0.22f + near * 0.78f;
            const float w = 360.0f;
            layers.push_back(
                (v(h(text_owned(langs[static_cast<std::size_t>(o.i)].hello) | font(size)
                         | weight(wght) | fg(colors::white.fade(a)))
                       | pad(size * 0.35f, size * 0.8f)
                       | radius(size * 1.2f)
                       | bg(tone.fade(0.06f + near * 0.16f))
                       | border(1.0f, tone.fade(0.10f + near * 0.45f))
                       | shadow(near * 26.0f, tone.fade(near * near * 0.55f), {0.0f, 0.0f})
                       | dsl::Id{lang_id(o.i)})
                   | center | width(px(w))
                   | absolute(o.x - w * 0.5f, o.y - size)).build());
        }
    }

    // The stage: the hero greeting typing itself out, breathing in weight.
    static Node hero(const Model& m, Color<Srgb> base, Vec2 vp) {
        const bool own = !m.typed.empty();
        const Lang& l = langs[static_cast<std::size_t>(m.hero)];
        std::string shown;
        if (own) {
            shown = m.typed;
        } else {
            const std::size_t n = cp_count(l.hello);
            const auto k = static_cast<std::size_t>(std::max(0.0f, (m.time - m.hero_since) * 11.0f));
            shown = cp_prefix(l.hello, std::min(n, k));
        }
        const bool caret_on = std::fmod(m.time, 1.0f) < 0.55f;
        // wght breathes 250..850. Quantised to steps of 25 so the glyph cache
        // keeps ~24 instances instead of re-rasterising every frame.
        const float breathe = 550.0f + 300.0f * std::sin(m.time * 1.1f);
        const float wght = std::round(breathe / 25.0f) * 25.0f;
        const float big = std::clamp(vp.x / 13.0f, 40.0f, 76.0f);
        const auto glow = lighten(base, 0.22f);

        return (v(text_owned(own ? std::string{"your words"} : std::string{l.name}) | font(15) | semibold
                      | tracking(3.0f) | fg(glow.fade(0.9f)),
                  h(text_owned(shown.empty() ? std::string{" "} : shown) | font(big) | weight(wght)
                        | fg(colors::white),
                    box() | width(px(3)) | height(px(big * 1.05f)) | radius(1.5f)
                        | bg(glow.fade(caret_on ? 0.95f : 0.0f)))
                      | gap(6) | center,
                  text_owned(own ? std::string{"any script · bidi ordered · shaped as you type"}
                                 : std::string{l.script})
                      | font(13) | fg(colors::white.fade(0.55f)))
                | gap(10) | center
                | width(px(vp.x))
                | absolute(0.0f, vp.y * 0.47f - big * 0.95f)).build();
    }

    // One mixed-direction sentence along the bottom: the bidi algorithm puts
    // the Arabic run (and its own digits) right-to-left inside the English.
    static Node ribbon(Color<Srgb> base, Vec2 vp) {
        return (v(text<"one engine · English, العربية ١٢٣, עברית (שָׁלוֹם), हिन्दी, ไทย, 한국어, ខ្មែរ — no HarfBuzz">
                      | font(14) | fg(colors::white.fade(0.78f)),
                  text<"click a greeting · type anything · ← → step · tab pauses · enter returns">
                      | font(11) | fg(lighten(base, 0.25f).fade(0.7f)))
                | gap(8) | center | width(px(vp.x))
                | absolute(0.0f, vp.y - 74.0f)).build();
    }

    // ── input ───────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model&, const Clicked& c) {
        for (int i = 0; i < n_langs; ++i)
            if (c.node == lang_id(i)) return Pick{i};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const TextEvent& e) {
        if (e.text.empty() || e.text[0] == '\t' || e.text[0] == '\r' || e.text[0] == '\n' ||
            static_cast<unsigned char>(e.text[0]) == 0x7F || static_cast<unsigned char>(e.text[0]) < 0x20)
            return std::nullopt;
        return Typed{e.text};
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{Key::backspace, Back{}}, {Key::enter, Clear{}},
                                      {Key::left, Step{-1}},    {Key::right, Step{1}},
                                      {Key::tab, Toggle{}},     {Key::escape, Quit{}}};
        return keys(k);
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // ── subscribe ───────────────────────────────────────────────────────
    // Animating while it runs (the hero breathes). Paused, frames stop as
    // soon as the springs settle and the typewriter finishes: 0% CPU.
    static Sub subscribe(const Model& m) {
        const bool typing = m.typed.empty() &&
            (m.time - m.hero_since) * 11.0f < static_cast<float>(cp_count(langs[static_cast<std::size_t>(m.hero)].hello)) + 1.0f;
        if (!m.running && !typing && !m.hue.animating() && !m.flash.animating()) return {};
        return Sub::on_frame([](float dt) -> Msg { return Tick{dt}; });
    }
};

static_assert(mayag::Program<Babel>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, {1100, 720});

    AppConfig cfg;
    cfg.title = "mayag — babel";
    cfg.size = opts.size;
    cfg.continuous_animation = true;   // the hero breathes forever; tab pauses to 0% CPU

    return demo::run<Babel>(opts, std::move(cfg), [](auto& rt) {
        int fails = 0;
        const auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            if (!c) ++fails;
        };
        std::printf("babel headless\n");

        for (int i = 0; i < 30; ++i) rt.tick();
        ok(rt.model().hero == 0, "starts on English");

        rt.click(Babel::lang_id(2));
        rt.tick();
        ok(rt.model().hero == 2, "clicking a greeting makes it the hero");
        ok(rt.model().hue.animating(), "the palette springs to its hue");

        rt.window().press_key(Key::right);
        rt.tick();
        ok(rt.model().hero == 3, "-> steps to the next language");

        rt.window().type("abc ");
        rt.window().type("مرحبا");
        rt.tick();
        ok(rt.model().typed == "abc مرحبا", "typing writes the hero line, any script");
        rt.window().press_key(Key::backspace);
        rt.tick();
        ok(rt.model().typed == "abc مرحب", "backspace deletes one code point");
        rt.window().press_key(Key::enter);
        rt.tick();
        ok(rt.model().typed.empty(), "enter returns to the orbit");

        rt.window().press_key(Key::tab);
        rt.tick();
        ok(!rt.model().running, "tab pauses the orbit");
        rt.window().drive_frames(true);
        for (int i = 0; i < 900 && (rt.model().hue.animating() || rt.model().flash.animating()); ++i) rt.tick();
        rt.window().drive_frames(false);
        for (int i = 0; i < 400; ++i) rt.tick();
        const auto t0 = rt.model().time;
        for (int i = 0; i < 30; ++i) rt.tick();
        ok(rt.model().time == t0, "paused and settled, it stops asking for frames");

        for (int i = 0; i < 20; ++i) rt.tick();
        ok(rt.window().frames_presented() > 1, "the scene rendered");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}
