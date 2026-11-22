// tests/test_memo.cpp — memo(): reusing a subtree must never change a frame.
//
// One app, run twice through the same script: once with its panels wrapped
// in memo(), once without. Every frame's pixels must match. The script hits
// each way a reused subtree can go stale: hover moving into, inside and out
// of it, a theme change, a resize, a key change, and the subtree moving.

#include <mayag/mayag.hpp>
#include <mayag/widget/scroll_view.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0, checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

bool g_memo = true;

struct App {
    struct Model {
        int  version = 0;     // what the list depends on
        int  counter = 0;     // what it doesn't
        bool dark = true;
        bool shift = false;   // moves the list down
        ScrollState scroll{};
    };
    struct Bump {}; struct Count {}; struct Flip {}; struct Shift {}; struct Wheel { float dy; };
    using Msg = std::variant<Bump, Count, Flip, Shift, Wheel>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return {}; }
    static Cmd update(Model& m, Bump)  { ++m.version; return {}; }
    static Cmd update(Model& m, Count) { ++m.counter; return {}; }
    static Cmd update(Model& m, Flip)  { m.dark = !m.dark; return {}; }
    static Cmd update(Model& m, Shift) { m.shift = !m.shift; return {}; }
    static Cmd update(Model& m, Wheel w) { m.scroll.scroll_by({0.0f, w.dy}); return {}; }
    static std::optional<Msg> on(const Model&, const Scrolled& s) { return Wheel{s.delta.y}; }
    static Theme theme(const Model& m) { return m.dark ? themes::midnight : themes::daylight; }

    static Node rows(const Model& m) {
        std::vector<Node> rs;
        for (int i = 0; i < 12; ++i)
            rs.push_back((h(text_owned("row " + std::to_string(i) + " v" + std::to_string(m.version)) | fg(var::text_primary))
                         | pad(6) | bg(var::surface) | id_of("row", static_cast<std::size_t>(i))
                         | when(state::hover, bg(var::accent)) | when(state::pressed, bg(var::danger))
                         | transition())
                            .build());
        return (v(std::move(rs)) | gap(2) | radius(6) | bg(var::surface_raised)).build();
    }
    // Styles itself by asking ctx() about hover, not through a cascade
    // variant: memo() has to notice that it depends on hover.
    static Node badge() {
        const bool hot = ctx().hovered(node_id("badge"));
        return (box() | size(60, 24) | bg(hot ? colors::red : colors::blue) | id_of("badge")).build();
    }
    static Node view(const Model& m) {
        // Piping onto a memo result wraps it in a plain box; the plain path
        // does the same so the two trees match.
        Node list = g_memo ? memo(static_cast<std::uint64_t>(m.version), [&] { return rows(m); })
                           : [&] { Node w{NodeKind::box}; w.add_child(rows(m)); return w; }();
        Node b = g_memo ? memo(7, [] { return badge(); }) : badge();
        // The same key twice in one frame: the second is simply built.
        const auto tag = [] { return (text<"tag"> | fg(var::text_secondary)).build(); };
        Node t1 = g_memo ? memo(11, tag) : tag();
        Node t2 = g_memo ? memo(11, tag) : tag();
        return (v(box() | height(m.shift ? 60.0f : 10.0f), node(std::move(b)), h(node(std::move(t1)), node(std::move(t2))),
                  text_owned("count " + std::to_string(m.counter)) | fg(var::text_secondary),
                  scroll_view(ctx(), "sv", m.scroll, node(std::move(list)) | width(300)) | height(300))
                | pad(10) | bg(var::background))
            .build();
    }
};

std::vector<std::vector<std::uint8_t>> run_script() {
    std::vector<std::vector<std::uint8_t>> frames;
    AppConfig cfg;
    cfg.size = {400, 520};
    (void)run_headless<App>(cfg, [&](auto& rt) {
        const auto shot = [&] { rt.tick(); frames.push_back(rt.window().read_pixels()); };
        shot();
        shot();                                         // a steady frame: reused
        rt.window().move_to(rt.center_of(node_id("badge"))); shot();    // a ctx().hovered() reader
        rt.window().move_to(rt.center_of(node_id("row", 3))); shot();   // hover into
        rt.window().drive_frames(true);
        for (int i = 0; i < 6; ++i) shot();             // the hover transition plays
        rt.window().drive_frames(false);
        rt.window().move_to(rt.center_of(node_id("row", 5))); shot();   // hover within
        shot();                                         // still hovered: reused hovered
        rt.send(App::Count{}); shot();                  // hovered, outside changed
        rt.window().move_to({390, 510}); shot();                        // hover out
        rt.send(App::Count{}); shot();                  // outside changed, list didn't
        rt.send(App::Bump{}); shot();                   // the key changed
        rt.send(App::Flip{}); shot();                   // theme
        rt.send(App::Shift{}); shot();                  // the list moved
        rt.window().resize({460, 520}); shot();         // the window resized
        rt.window().move_to(rt.center_of(node_id("row", 1)));
        const Vec2 p = rt.center_of(node_id("row", 1));
        rt.window().push(MouseDown{p, MouseButton::left, {}, 1}); shot();   // pressed inside
        rt.window().push(MouseUp{p, MouseButton::left, {}}); shot();
        rt.send(App::Flip{}); shot();
        shot();
        rt.window().scroll(rt.center_of(node_id("row", 4)), {0, -40}); shot();   // scrolled: it moves
        rt.window().scroll(rt.center_of(node_id("row", 4)), {0, -40}); shot();
        shot();
        return 0;
    });
    return frames;
}

}  // namespace

int main() {
    {   // a theme is keyed by VALUE: padding bytes (whatever the stack held) never change its id
        alignas(Theme) unsigned char a[sizeof(Theme)], b[sizeof(Theme)];
        std::memset(a, 0xAB, sizeof a);
        std::memset(b, 0xCD, sizeof b);
        const Theme* ta = ::new (a) Theme;   // default-init: members set, padding left as it was
        const Theme* tb = ::new (b) Theme;
        MemoCache cache;
        check(std::memcmp(a, b, sizeof(Theme)) != 0, "(the two themes' bytes do differ: the test exercises padding)");
        check(*ta == *tb && cache.theme_id(*ta) == cache.theme_id(*tb), "equal themes, different padding: one memo key");
        Theme other = *ta;
        other.tab_numbers = !other.tab_numbers;
        check(cache.theme_id(other) != cache.theme_id(*ta), "a changed theme field: a different memo key");
    }
    std::printf("mayag memo()\n============\n");
    g_memo = false;
    const auto plain = run_script();
    g_memo = true;
    const auto memo = run_script();
    check(plain.size() == memo.size(), "both runs drew the same number of frames");
    // The hover transition really animated (frames 4..9 are its steps).
    if (memo.size() > 9) check(memo[4] != memo[9] && memo[5] != memo[6], "the hover transition animated");
    // The script must actually exercise something: scrolling moved the list.
    if (memo.size() >= 3) check(memo[memo.size() - 3] != memo[memo.size() - 4], "the scroll moved the memoised list");
    for (std::size_t i = 0; i < plain.size() && i < memo.size(); ++i) {
        check(plain[i] == memo[i], "frame " + std::to_string(i) + " is identical with memo()");
        if (plain[i] != memo[i] && plain[i].size() == memo[i].size()) {   // say where, to debug a failure
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
            const int w = static_cast<int>(std::sqrt(static_cast<double>(plain[i].size() / 4) * 400.0 / 520.0) + 0.5);
            for (std::size_t p = 0; p < plain[i].size(); p += 4)
                if (std::memcmp(&plain[i][p], &memo[i][p], 4) != 0) {
                    const int px = static_cast<int>(p / 4) % w, py = static_cast<int>(p / 4) / w;
                    x0 = std::min(x0, px); y0 = std::min(y0, py); x1 = std::max(x1, px); y1 = std::max(y1, py);
                }
            std::printf("        differs in x %d..%d y %d..%d (w %d)\n", x0, x1, y0, y1, w);
        }
    }
    std::printf("\n%s  %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
