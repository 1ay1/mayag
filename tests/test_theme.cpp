// tests/test_theme.cpp — a theme is the WHOLE look: colour, shape, space,
// type, depth, motion and every widget's style; and it is fast.
//
// Correctness on resolved styles (no fonts, no window), then budgets on the
// hot paths, measured with a generous margin so a slow CI machine still
// passes while a real regression (an extra allocation per node, a full
// Style copy per look) does not.

#include <mayag/mayag.hpp>
#include <mayag/widget/button.hpp>
#include <mayag/widget/card.hpp>
#include <mayag/widget/toggle.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/all.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
using namespace mayag;
using namespace mayag::dsl;

int fails = 0;
void ok(bool c, const char* w) { std::printf("  %s %s\n", c ? "ok  " : "FAIL", w); fails += !c; }
const Node* fid(const Node& n, std::string_view s) { return n.find(node_id(s)); }


// Every widget, every stock look: (1) the theme's shape knobs reach older
// widgets too (square looks draw square), and (2) no text ends up illegible
// on the fill directly behind it.
static void older_widgets_follow_and_stay_legible() {
    std::printf("-- every widget follows every look\n");
    const std::pair<const char*, Theme> looks_[] = {
        {"midnight", themes::midnight}, {"daylight", themes::daylight}, {"square", themes::square},
        {"soft", themes::soft}, {"compact", themes::compact},
        {"brutal", looks::brutal()}, {"glass", looks::glass()}, {"terminal", looks::terminal()}};
    for (const auto& [name, t] : looks_) {
        Node root = themed(t, [&](const Ctx& c) {
            return v(h(text<"Title"> | bold, spacer(), kbd<"K">(t)),
                     h(key_hint(t, "R", "remix", 11), badge<"beta">(t, t.warning), badge<"new">(t, t.accent)),
                     segmented(c, "seg", {"One", "Two"}, 1), stepper(c, "st", "12"),
                     progress(t, 0.5f), button<"Go">(t), button<"Cancel">(t, ButtonVariant::secondary),
                     dsl::card(t, text<"in a card">))
                 | gap(8) | pad(12) | bg(t.surface);
        });
        cascade_static(root, t, {500, 600});
        layout::layout_tree(root, {500, 600}, layout::default_measurer());

        // Legibility: each text against the nearest opaque-ish fill above it.
        int bad = 0;
        std::string first;
        std::function<void(const Node&, Color<Srgb>)> walk = [&](const Node& n, Color<Srgb> under) {
            const Style& st = n.style();
            if (st.fill.kind == FillKind::solid && st.fill.color.a > 0.0f)
                under = mix(under, Color<Srgb>{st.fill.color.c0, st.fill.color.c1, st.fill.color.c2, 1.0f}, st.fill.color.a);
            if (n.kind() == NodeKind::text && !n.text().empty() && st.opacity > 0.5f) {
                const auto& c = st.text.color;
                const Color<Srgb> ink = mix(under, Color<Srgb>{c.c0, c.c1, c.c2, 1.0f}, c.a);
                if (contrast_ratio(ink, under) < 2.5f) { ++bad; if (first.empty()) first = std::string(n.text()); }
            }
            for (const auto& k : n.children()) walk(k, under);
        };
        walk(root, t.background);
        ok(bad == 0, (std::string(name) + ": every label is legible on its own fill" +
                      (bad ? " (first: '" + first + "')" : "")).c_str());

        // Shape: a square look's widgets have square corners.
        if (t.radius_small == 0.0f) {
            float worst = 0.0f;
            std::function<void(const Node&)> corners = [&](const Node& n) {
                worst = std::max(worst, std::min(n.style().corners.tl, 50.0f));
                for (const auto& k : n.children()) corners(k);
            };
            corners(root);
            ok(worst == 0.0f, (std::string(name) + ": roundness 0 squares EVERY widget").c_str());
        }
    }
}

int main() {
    std::printf("-- tokens, themes, sheets, motion\n");
    older_widgets_follow_and_stay_legible();
    // numeric tokens
    const Theme t = themes::midnight;
    Node a = v(box() | pad(var::space(2)) | radius(var::radius_md) | border(var::line, var::border)
                 | elevation(var::depth(6)) | dsl::id<"a">,
               text<"x"> | font(var::font_xl) | dsl::id<"x">);
    cascade_static(a, t, {400, 300});
    const Style& s = fid(a, "a")->style();
    ok(s.layout.padding.top == t.space_unit * 2, "space(2)");
    ok(s.corners.tl == t.radius_medium, "radius_md");
    ok(s.stroke.width == t.line_hairline, "line");
    Style lit; elevation(6.0f).apply(lit);
    ok(s.shadow_count == 2 && s.shadows[0] == lit.shadows[0] && s.shadows[1] == lit.shadows[1],
       "depth(6) == elevation(6) exactly");
    ok(fid(a, "x")->style().text.size == t.font_size * 1.6f, "font_xl");

    Theme flat = derive({.roundness = 0, .density = 0.5f, .depth = 0});
    Node b = v(box() | pad(var::space(2)) | radius(var::radius_md) | elevation(var::depth(6)) | dsl::id<"a">);
    cascade_static(b, flat, {400, 300});
    ok(fid(b, "a")->style().corners.tl == 0 && fid(b, "a")->style().layout.padding.top == flat.space_unit * 2,
       "derive: square, dense");
    ok(fid(b, "a")->style().shadows[0].color.a == 0, "derive: depth 0 is flat");

    // widgets follow the theme
    Node w1 = v(button<"Go">(t) | dsl::id<"b">);
    cascade_static(w1, t, {400, 300});
    ok(fid(w1, "b")->style().fill.color == t.accent, "button primary = accent");
    ok(fid(w1, "b")->style().corners.tl == t.radius_small, "button radius from theme");
    Node w2 = v(button<"Go">(t) | dsl::id<"b">);
    cascade_static(w2, flat, {400, 300});
    ok(fid(w2, "b")->style().corners.tl == 0, "SAME widget, flat theme: square (no rebuild)");
    Node w3 = v(button<"Go">(t) | bg(colors::red) | dsl::id<"b">);
    cascade_static(w3, t, {400, 300});
    ok(fid(w3, "b")->style().fill.color == colors::red, "caller pipe beats the look");
    CascadeInput hv{}; hv.theme = &t; hv.viewport = {400, 300}; hv.hovered = node_id("b");
    Node w4 = v(button<"Go">(t) | dsl::id<"b">); (void)cascade(w4, hv);
    ok(fid(w4, "b")->style().fill.color == t.accent_hover, "button hover from the sheet");

    // a custom sheet
    static const Sheet brutal = [] {
        Sheet x = Sheet::standard();
        x.primary = look(pad(10, 18), radius(0), bg(var::warning), border(2, var::text_primary),
                         shadow(0, var::text_primary, {4, 4}));
        return x;
    }();
    Theme bt = t; bt.sheet = &brutal;
    Node w5 = v(button<"Go">(bt) | dsl::id<"b">);
    cascade_static(w5, bt, {400, 300});
    ok(fid(w5, "b")->style().fill.color == t.warning && fid(w5, "b")->style().stroke.width == 2,
       "custom sheet restyles the widget");

    // subtree theme
    Node w6 = v(v(box() | bg(var::surface) | size(4, 4) | dsl::id<"in">) | theme(themes::daylight),
                box() | bg(var::surface) | size(4, 4) | dsl::id<"out">);
    cascade_static(w6, t, {400, 300});
    ok(fid(w6, "in")->style().fill.color == themes::daylight.surface &&
       fid(w6, "out")->style().fill.color == t.surface, "| theme(t) subtree");

    // motion scale
    Theme still = t; still.motion_scale = 0;
    Node m1 = v(box() | size(4, 4) | transition() | dsl::id<"m">);
    cascade_static(m1, still, {400, 300});
    ok(!fid(m1, "m")->style().transition.on(), "motion_scale 0 disables transitions");
    Node m2 = v(box() | size(4, 4) | transition() | dsl::id<"m">);
    cascade_static(m2, t, {400, 300});
    ok(fid(m2, "m")->style().transition.stiffness == t.motion_ui.stiffness, "transition() = theme motion_ui");

    // speed
    std::vector<Node> rows;
    for (std::size_t i = 0; i < 2000; ++i)
        rows.push_back(h(text_owned("row " + std::to_string(i)) | grow(), button<"Open">(t))
                       | pad(var::space(1)) | radius(var::radius_sm) | id_of("r", i));
    const Node tree = v(std::move(rows));
    double best_build = 1e9, best_cas = 1e9;
    for (int it = 0; it < 20; ++it) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Node> rr;
        for (std::size_t i = 0; i < 2000; ++i)
            rr.push_back(h(text_owned("row"), button<"Open">(t)) | pad(var::space(1)) | id_of("r", i));
        auto t1 = std::chrono::steady_clock::now();
        Node n = tree;
        auto t2 = std::chrono::steady_clock::now();
        cascade_static(n, t, {1000, 800});
        auto t3 = std::chrono::steady_clock::now();
        best_build = std::min(best_build, std::chrono::duration<double, std::milli>(t1 - t0).count());
        best_cas = std::min(best_cas, std::chrono::duration<double, std::milli>(t3 - t2).count());
    }
    const double ns_per_node = best_cas * 1e6 / static_cast<double>(tree.count());
    std::printf("  nodes %zu  build 2000 themed buttons %.3f ms  cascade %.3f ms (%.1f ns/node)\n",
                tree.count(), best_build, best_cas, ns_per_node);
#ifdef NDEBUG
    ok(ns_per_node < 400.0, "cascade stays O(nodes) and cheap (< 400 ns/node, typically ~50)");
    ok(best_build < 25.0, "2000 themed buttons build in < 25 ms (typically ~3)");
#endif
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails;
}
static_assert(legible(themes::midnight) && legible(themes::daylight) && legible(themes::paper));
static_assert(legible(themes::square) && legible(themes::soft) && legible(themes::compact));
