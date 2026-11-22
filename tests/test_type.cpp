// tests/test_type.cpp — the type scale (style/type.hpp) keeps its promises.
//
// For every body size an app could set (8..32 px in quarter steps), both
// faces, and the ratios a theme might choose: every size is on the grid,
// none is below the floor, the tiers never run backwards, and two sizes are
// either equal or at least min_step apart. Then the plumbing: a tier token
// resolves through the cascade to exactly Theme::type, follows a subtree
// theme, and the content face follows content_size.

#include <mayag/mayag.hpp>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int checks = 0, failures = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    if (!c) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

const Node* find(const Node& n, std::uint64_t id) {
    if (n.id() == id) return &n;
    for (const auto& k : n.children())
        if (const Node* f = find(k, id)) return f;
    return nullptr;
}

void rules_hold_everywhere() {
    int bad = 0;
    for (float ratio : {1.125f, 1.2f, 1.25f, 1.333f}) {
        const TypeRules s{.ratio = ratio};
        for (float base = 8.0f; base <= 32.0f; base += 0.25f) {
            const TypeRamp r = TypeRamp::of(base, s);
            for (std::size_t i = 0; i < 6; ++i) {
                const float px = r.px[i];
                bad += px < s.floor;                                          // floor
                bad += std::fabs(px / s.grid - std::round(px / s.grid)) > 1e-4f;   // grid
                if (i > 0) {
                    const float d = px - r.px[i - 1];
                    bad += d < 0.0f;                                          // monotone
                    bad += d > 0.0f && d < s.min_step - 1e-4f;                // distinct
                }
            }
            bad += r[Tier::body] != std::max(s.snap(base), s.floor);          // body is the base
        }
    }
    ok(bad == 0, "floor, grid, order and distinctness hold for every base and ratio");
}

void how_many_sizes() {
    // An app with an interface and a content size can never show more than
    // twelve sizes; at typical settings it shows far fewer distinct ones.
    Theme t = themes::midnight;
    t.font_size = 12.5f;
    t.content_size = 13.0f;
    std::set<float> all;
    for (Scale f : {Scale::ui, Scale::content})
        for (Tier k : all_tiers) all.insert(t.type(f, k));
    ok(all.size() <= 12, "at most twelve sizes in an app");
    ok(t.type(Tier::fine) >= t.type_rules.floor, "fine text is never below the floor");
    ok(t.type(Tier::small) < t.type(Tier::body), "small is smaller than body");
    const auto d = [&](Tier k) { return static_cast<double>(t.type(k)); };
    std::printf("    ui 12.5:  fine %.1f  small %.1f  body %.1f  large %.1f  title %.1f  display %.1f\n",
                d(Tier::fine), d(Tier::small), d(Tier::body), d(Tier::large), d(Tier::title), d(Tier::display));
}

void resolves_through_the_cascade() {
    Theme t = themes::midnight;
    t.font_size = 14.0f;
    t.content_size = 18.0f;
    Node a = v(text<"a"> | type(Tier::small) | dsl::id<"a">,
               text<"b"> | type(Scale::content, Tier::body) | dsl::id<"b">,
               text<"c"> | font(var::fine) | dsl::id<"c">,
               v(text<"d"> | type(Tier::body) | dsl::id<"d">) | theme(themes::daylight));
    cascade_static(a, t, {400, 300});
    const auto sz = [&](const char* n) { return find(a, node_id(n))->style().text.size; };
    ok(sz("a") == t.type(Tier::small), "type(small) is Theme::type(small)");
    ok(sz("b") == 18.0f, "the content face's body is content_size");
    ok(sz("c") == t.type(Tier::fine), "var::fine resolves like type(fine)");
    ok(sz("d") == themes::daylight.type(Tier::body), "a subtree theme re-sizes its tiers");
    ok(find(a, node_id("a"))->style().text.line_height == tier_line_height(Tier::small),
       "a tier brings its line height");
    Theme no_content = t;
    no_content.content_size = 0.0f;
    ok(no_content.type(Scale::content, Tier::body) == no_content.font_size, "no content_size: content follows the interface");
}

void handed_bases_snap() {
    const Theme t = themes::midnight;
    ok(t.type_at(13.3f, Tier::body) == 13.5f, "a handed base lands on the grid");
    ok(t.type_at(6.0f, Tier::body) == t.type_rules.floor, "...and never below the floor");
    ok(t.type_at(13.0f, Tier::small) == TypeRules{}.size(13.0f, Tier::small), "type_at is the scale's size()");
}

}  // namespace

int main() {
    std::printf("-- the type scale\n");
    rules_hold_everywhere();
    how_many_sizes();
    resolves_through_the_cascade();
    handed_bases_snap();
    std::printf("%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
