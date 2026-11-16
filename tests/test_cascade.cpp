// tests/test_cascade.cpp — the cascade: inheritance, state variants, tokens,
// groups, responsive rules, entry states and spring transitions.
//
// Every assertion here is about RESOLVED style, the thing the painter sees,
// so the suite needs no fonts, no window and no pixels. The last section
// drives a real headless app to prove the host wires it all up.

#include <mayag/mayag.hpp>

#include <cstdio>
#include <string>

using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0;
int checks = 0;

void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const char* name) { std::printf("-- %s\n", name); }

bool same(Color<Srgb> a, Color<Srgb> b, float eps = 1e-4f) {
    return num::abs(a.c0 - b.c0) <= eps && num::abs(a.c1 - b.c1) <= eps &&
           num::abs(a.c2 - b.c2) <= eps && num::abs(a.a - b.a) <= eps;
}

bool near(float a, float b, float eps) { return num::abs(a - b) <= eps; }

const Theme th = themes::midnight;

/// Resolve a fresh copy of `n` in the given interaction state.
Node resolve(const Node& n, CascadeInput in = {}) {
    Node out = n;
    if (in.theme == nullptr) in.theme = &th;
    if (in.viewport.x == 0.0f) in.viewport = {800, 600};
    (void)cascade(out, in);
    return out;
}

const Node& find(const Node& root, std::string_view id) {
    const Node* n = root.find(node_id(id));
    if (n == nullptr) {
        std::printf("  missing node %.*s\n", static_cast<int>(id.size()), id.data());
        std::abort();
    }
    return *n;
}

// ── the reusable vocabulary this suite exercises ────────────────────────

constexpr auto btn = styles(
    pad(8, 14), radius(8), bg(var::surface_raised), fg(var::text_primary),
    hovered(bg(var::tint(var::accent, 0.25f))),
    pressed(bg(var::accent), fg(var::on_accent), scale(0.97f)),
    focus_visible(border(2, var::focus_ring)),
    when(state::disabled, opacity(0.45f)));

}  // namespace

// ════════════════════════════════════════════════════════════════════════

static void test_inheritance() {
    section("inheritance");
    const Node root = v(text<"a">,
                        text<"b"> | fg(rgb<0x112233>) | font(20),
                        v(text<"c">) | italic)
                    | fg(rgb<0xAABBCC>) | font(13) | semibold;
    const Node r = resolve(root);
    const auto& a = r.children()[0].style().text;
    const auto& b = r.children()[1].style().text;
    const auto& c = r.children()[2].children()[0].style().text;
    check(same(a.color, rgb<0xAABBCC>) && a.size == 13.0f, "text inherits colour and size");
    check(a.weight == FontWeight::semi_bold, "and weight");
    check(same(b.color, rgb<0x112233>) && b.size == 20.0f, "explicit beats inherited");
    check(b.weight == FontWeight::semi_bold, "unset fields still inherit around explicit ones");
    check(c.italic && same(c.color, rgb<0xAABBCC>), "inheritance crosses nested containers");

    // Overflow does NOT inherit: a wrapping label is a vertical ribbon.
    const Node w = resolve(v(text<"x">) | fg(colors::white));
    check(w.children()[0].style().text.overflow == TextOverflow::ellipsis,
          "overflow is not inherited");

    // A tree with no text properties anywhere is untouched.
    const Node plain = resolve(v(text<"x">));
    check(plain.children()[0].style().text == TextStyle{}, "no-op without explicit text props");
}

static void test_tokens() {
    section("tokens and vars");
    const Node r = resolve(v(box() | size(10, 10) | bg(var::accent) | id<"a">,
                             box() | size(10, 10) | bg(var::accent.fade(0.5f)) | id<"b">,
                             box() | size(10, 10) | border(1, var::border) | id<"c">,
                             text<"t"> | fg(var::text_secondary) | id<"t">));
    check(same(find(r, "a").style().fill.color, th.accent), "bg(var::accent) resolves to the theme");
    check(same(find(r, "b").style().fill.color, th.accent.fade(0.5f)), "fade() multiplies alpha");
    check(same(find(r, "c").style().stroke.color, th.border), "tokens work in border colour");
    check(same(find(r, "t").style().text.color, th.text_secondary), "and in text colour");

    // A theme switch restyles the same tree with no view change.
    CascadeInput day{};
    day.theme = &themes::daylight;
    const Node d = resolve(v(box() | bg(var::surface) | size(4, 4) | id<"s">), day);
    check(same(find(d, "s").style().fill.color, themes::daylight.surface), "tokens follow the theme");

    // Scoped override, alias, named tokens, and the loud unbound default.
    constexpr auto brand = var::named("brand");
    const Node o = resolve(v(v(box() | size(4, 4) | bg(var::accent) | id<"in">,
                               box() | size(4, 4) | bg(brand) | id<"named">)
                             | vars(var::accent = var::danger, brand = rgb<0x00D1B2>),
                             box() | size(4, 4) | bg(var::accent) | id<"out">,
                             box() | size(4, 4) | bg(brand) | id<"unbound">));
    check(same(find(o, "in").style().fill.color, th.danger), "vars() rebinds a role for the subtree");
    check(same(find(o, "out").style().fill.color, th.accent), "and only for the subtree");
    check(same(find(o, "named").style().fill.color, rgb<0x00D1B2>), "named tokens bind");
    check(same(find(o, "unbound").style().fill.color, rgb<0xFF00FF>), "an unbound named token is loud magenta");

    // A token inherited through text resolves in the CHILD's scope.
    const Node s = resolve(v(v(text<"x"> | id<"x">) | vars(var::text_primary = rgb<0x123456>))
                           | fg(var::text_primary));
    check(same(find(s, "x").style().text.color, rgb<0x123456>),
          "an inherited token resolves where it lands, like a CSS custom property");
}

static void test_variants() {
    section("state variants");
    const Node tree = v(h(text<"Save">) | btn | id<"save">);
    CascadeInput in{};

    Node r = resolve(tree);
    const auto& rest = find(r, "save").style();
    check(same(rest.fill.color, th.surface_raised), "rest");
    check(same(find(r, "save").children()[0].style().text.color, th.text_primary), "rest fg reaches text");

    in.hovered = node_id("save");
    r = resolve(tree, in);
    check(same(find(r, "save").style().fill.color, mix(th.background, th.accent, 0.25f)), "hovered");

    in.pressed = node_id("save");
    r = resolve(tree, in);
    check(same(find(r, "save").style().fill.color, th.accent), "pressed beats hovered");
    check(same(find(r, "save").children()[0].style().text.color, th.on_accent),
          "a variant's text colour reaches the label by inheritance");
    check(find(r, "save").style().transform.a < 1.0f, "pressed scales");

    in.press_inside = false;   // dragged off: the press look releases
    r = resolve(tree, in);
    check(same(find(r, "save").style().fill.color, mix(th.background, th.accent, 0.25f)),
          "dragging off a pressed control releases the pressed look");

    // Focus visibility: ring after Tab, none after a click.
    CascadeInput f{};
    f.focused = node_id("save");
    r = resolve(tree, f);
    check(find(r, "save").style().stroke.width == 0.0f, "no ring for pointer focus");
    f.focus_visible = true;
    r = resolve(tree, f);
    check(find(r, "save").style().stroke.width == 2.0f &&
          same(find(r, "save").style().stroke.color, th.focus_ring), "ring for keyboard focus");

    // Disabled beats pressed regardless of declaration order, and
    // propagates to descendants.
    const Node dis = v(h(text<"x">) | btn | disabled() | id<"d">,
                       v(box() | size(4, 4) | when(state::disabled, opacity(0.2f)) | id<"kid">) | disabled());
    CascadeInput p{};
    p.hovered = p.pressed = node_id("d");
    r = resolve(dis, p);
    check(find(r, "d").style().opacity == 0.45f, "disabled applies");
    check(find(r, "kid").style().opacity == 0.2f, "disabled propagates to descendants");

    // Order independence: declare pressed BEFORE hovered.
    const Node rev = v(box() | size(4, 4) | pressed(bg(colors::red)) | hovered(bg(colors::blue)) | id<"r">);
    CascadeInput hp{};
    hp.hovered = hp.pressed = node_id("r");
    r = resolve(rev, hp);
    check(same(find(r, "r").style().fill.color, colors::red),
          "pressed wins even when declared before hovered");

    // Compound selectors outrank their parts; negation.
    const Node sel = v(box() | size(4, 4) | selected()
                       | when(state::selected, bg(colors::green))
                       | when(state::selected && state::hover, bg(colors::blue))
                       | when(!state::selected, bg(colors::red)) | id<"s">);
    r = resolve(sel);
    check(same(find(r, "s").style().fill.color, colors::green), "selected");
    CascadeInput hs{};
    hs.hovered = node_id("s");
    r = resolve(sel, hs);
    check(same(find(r, "s").style().fill.color, colors::blue), "selected && hover beats selected");

    // App flags.
    const Node fl = v(box() | size(4, 4) | flag<2>() | when(state::flag<2>, bg(colors::red)) | id<"f">,
                      box() | size(4, 4) | flag<2>(false) | when(state::flag<2>, bg(colors::red)) | id<"g">);
    r = resolve(fl);
    check(same(find(r, "f").style().fill.color, colors::red), "flag<N> on");
    check(!find(r, "g").style().fill.visible(), "flag<N> off");
}

static void test_pointer_propagation() {
    section("hover propagates to ancestors; hover-only nodes");
    // A card with a hover look and a button inside it, neither id'd except
    // the button: hovering the button hovers the card too.
    const Node tree = v(v(h(text<"go">) | id<"go">)
                        | pad(8) | hovered(bg(colors::red)));
    CascadeInput in{};
    in.hovered = node_id("go");
    const Node r = resolve(tree, in);
    check(same(r.children()[0].style().fill.color, colors::red),
          "an ancestor of the hovered node is hovered (CSS :hover)");

    // The card got a synthetic id so it can be hovered DIRECTLY, and it is
    // hover-only: not a Tab stop, not a click target.
    Node prepared = tree;
    prepare_for_input(prepared);
    const Node& card = prepared.children()[0];
    check(card.style().id != 0 && card.style().hit_only, "pointer rules get a hover-only id");
    Node again = tree;
    prepare_for_input(again);
    check(again.children()[0].style().id == card.style().id, "the synthetic id is stable across frames");

    layout::layout_tree(prepared, {200, 100}, layout::default_measurer());
    const Vec2 in_card = prepared.children()[0].frame().origin + Vec2{2, 2};
    check(Interaction::identify(prepared, in_card) == card.style().id, "hover hit-tests the card");
    check(Interaction::identify_control(prepared, in_card) == 0, "but a click there targets no control");

    Interaction ix;
    ix.focus_next(prepared);
    check(ix.focused() == node_id("go"), "Tab skips hover-only nodes");
}

static void test_groups() {
    section("groups");
    const Node tree = v(h(text<"Row">,
                          text<">"> | fg(var::text_disabled) | group_hovered(fg(var::text_primary)) | id<"chev">)
                        | group() | id<"row">,
                        text<"far"> | group_hovered(fg(colors::red)) | id<"orphan">);
    Node r = resolve(tree);
    check(same(find(r, "chev").style().text.color, th.text_disabled), "group rest");
    CascadeInput in{};
    in.hovered = node_id("row");
    r = resolve(tree, in);
    check(same(find(r, "chev").style().text.color, th.text_primary), "group hover restyles a descendant");
    check(!same(find(r, "orphan").style().text.color, colors::red), "no group ancestor: never matches");
}

static void test_responsive_and_theme() {
    section("responsive and theme-mode rules");
    const Node tree = h(box() | size(4, 4), box() | size(4, 4))
                    | when(state::narrower(600), direction(Axis::vertical))
                    | when(state::dark, bg(colors::black)) | when(state::light, bg(colors::white));
    CascadeInput wide{};
    wide.viewport = {900, 600};
    CascadeInput narrow{};
    narrow.viewport = {400, 600};
    check(resolve(tree, wide).style().layout.axis == Axis::horizontal, "wide keeps the row");
    check(resolve(tree, narrow).style().layout.axis == Axis::vertical, "narrow stacks: layout reflows");
    check(same(resolve(tree).style().fill.color, colors::black), "dark theme rule");
    CascadeInput day{};
    day.theme = &themes::daylight;
    check(same(resolve(tree, day).style().fill.color, colors::white), "light theme rule");
}

static void test_runtime_elements() {
    section("runtime elements: text_owned and node()");
    const Node t = v(text_owned("dyn") | hovered(fg(colors::red)) | id<"t">) | fg(colors::blue);
    Node r = resolve(t);
    check(same(find(r, "t").style().text.color, colors::blue), "owned text inherits");
    CascadeInput in{};
    in.hovered = node_id("t");
    r = resolve(t, in);
    check(same(find(r, "t").style().text.color, colors::red), "owned text takes variants");

    const Node built = (v(text<"inner">) | hovered(bg(colors::red))).build();
    const Node wrapped = v(node(built) | id<"w"> | pressed(bg(colors::blue)));
    CascadeInput hp{};
    hp.hovered = hp.pressed = node_id("w");
    r = resolve(wrapped, hp);
    check(same(find(r, "w").style().fill.color, colors::blue),
          "node() merges its own rules with the payload's (pressed > hovered)");
}

static void test_classes() {
    section("classes compose");
    constexpr auto base = styles(pad(4), radius(4), hovered(bg(colors::red)));
    constexpr auto loud = styles(base, bg(colors::blue), transition());
    const Node r = resolve(v(box() | size(4, 4) | loud | id<"l">));
    check(same(find(r, "l").style().fill.color, colors::blue), "nested class applies its base");
    check(find(r, "l").style().transition.on(), "class carries transition()");
    check(find(r, "l").style().layout.padding.top == 4.0f, "and the inner class's layout");
    static_assert(std::is_same_v<decltype(styles(pad(1), radius(2))),
                                 Bundle<Pad, Radius>>, "pure-modifier styles() stays a Bundle");
}

static void test_idempotent_and_overlay_sized() {
    section("idempotence");
    Node r = v(text<"x">) | fg(colors::red) | hovered(bg(colors::blue));
    CascadeInput in{};
    in.theme = &th;
    (void)cascade(r, in);
    const Node once = r;
    (void)cascade(r, in);
    check(r == once, "cascading twice is a no-op (no double-applied variants)");
}

static void test_transitions() {
    section("transitions");
    const Node tree = v(box() | size(20, 20) | bg(colors::black) | hovered(bg(colors::white))
                        | transition() | id<"t">);
    StyleMotion m;
    CascadeInput rest{};
    CascadeInput hover{};
    hover.hovered = node_id("t");

    Node f0 = resolve(tree, rest);
    m.apply(f0, 0.0);
    check(same(find(f0, "t").style().fill.color, colors::black), "first frame snaps (nothing to animate from)");
    check(!m.animating(), "and is settled");

    Node f1 = resolve(tree, hover);
    m.apply(f1, 1.0 / 60.0);
    const auto mid = find(f1, "t").style().fill.color;
    check(mid.c0 > 0.0f && mid.c0 < 1.0f, "hover starts a transition rather than jumping");
    check(m.animating(), "and asks for frames");

    float last = mid.c0;
    bool monotone = true;
    for (int i = 0; i < 120 && m.animating(); ++i) {
        Node f = resolve(tree, hover);
        m.apply(f, 1.0 / 60.0);
        const float now = find(f, "t").style().fill.color.c0;
        if (now + 1e-4f < last) monotone = false;
        last = now;
    }
    check(!m.animating(), "settles");
    check(monotone, "critically damped by default: no overshoot");
    Node end = resolve(tree, hover);
    m.apply(end, 1.0 / 60.0);
    check(same(find(end, "t").style().fill.color, colors::white, 1e-3f), "lands exactly on target");

    // Reversal mid-flight continues from where it is.
    Node a = resolve(tree, rest);
    m.apply(a, 1.0 / 60.0);
    Node b = resolve(tree, rest);
    m.apply(b, 1.0 / 60.0);
    const float going_down = find(b, "t").style().fill.color.c0;
    check(going_down < 1.0f && going_down > 0.5f, "unhover reverses smoothly from white");

    // A node that leaves the tree forgets its spring.
    Node gone = v(box());
    m.apply(gone, 1.0 / 60.0);
    check(m.size() == 0, "springs of removed nodes are dropped");

    // The common hover looks ADD something. Each must animate in, not cut.
    const auto appears = [&](const char* what, auto&& tree_of, auto&& measure) {
        StyleMotion mm;
        CascadeInput h{};
        h.hovered = node_id("a");
        Node r0 = resolve(tree_of(), rest);
        mm.apply(r0, 0.0);
        Node r1 = resolve(tree_of(), h);
        mm.apply(r1, 1.0 / 60.0);
        const float mid_v = measure(find(r1, "a").style());
        for (int i = 0; i < 200 && mm.animating(); ++i) {
            Node r = resolve(tree_of(), h);
            mm.apply(r, 1.0 / 60.0);
        }
        Node r2 = resolve(tree_of(), h);
        mm.apply(r2, 1.0 / 60.0);
        const float end_v = measure(find(r2, "a").style());
        check(mid_v > 0.0f && mid_v < end_v * 0.99f, std::string(what) + " animates in");
        check(near(end_v, measure(find(resolve(tree_of(), h), "a").style()), 1e-3f),
              std::string(what) + " lands on target");
        // ...and back out, still drawn while it fades.
        Node o1 = resolve(tree_of(), rest);
        mm.apply(o1, 1.0 / 60.0);
        const float out_v = measure(find(o1, "a").style());
        check(out_v > 0.0f && out_v < end_v, std::string(what) + " fades out instead of vanishing");
    };
    appears("a background on a bare row",
            [] { return v(box() | size(20, 20) | hovered(bg(colors::red)) | transition() | id<"a">); },
            [](const Style& s) { return s.fill.kind == FillKind::solid ? s.fill.color.a : 0.0f; });
    appears("a ring on a borderless control",
            [] { return v(box() | size(20, 20) | hovered(border(2, colors::blue)) | transition() | id<"a">); },
            [](const Style& s) { return s.stroke.width; });
    appears("an elevation shadow",
            [] { return v(box() | size(20, 20) | bg(colors::white) | hovered(elevation(8)) | transition() | id<"a">); },
            [](const Style& s) { return s.shadow_count > 0 ? s.shadows[0].blur : 0.0f; });
}

static void test_painter_transform() {
    section("painter honours transform");
    Node n = v(box() | size(100, 100) | bg(colors::red) | scale(0.5f)) | size(200, 200);
    layout::layout_tree(n, {200, 200}, layout::default_measurer());
    DrawList dl;
    render::paint(n, dl);
    check(dl.size() == 1, "one instance");
    if (dl.size() == 1) {
        const auto& r = dl.instances()[0].rect;
        check(near(r.z, 50.0f, 0.01f) && near(r.w, 50.0f, 0.01f), "scaled to half size");
        check(near(r.x, 25.0f, 0.01f) && near(r.y, 25.0f, 0.01f), "about its centre");
    }
    Node t = v(box() | size(10, 10) | bg(colors::red) | translate(5, 7));
    layout::layout_tree(t, {100, 100}, layout::default_measurer());
    DrawList dl2;
    render::paint(t, dl2);
    if (dl2.size() == 1) {
        const auto& r = dl2.instances()[0].rect;
        check(near(r.x, 5.0f, 0.01f) && near(r.y, 7.0f, 0.01f), "translate moves paint, not layout");
    }
    check(t.children()[0].frame().origin.x == 0.0f, "layout slot unchanged");
}

// ── end to end: a live headless app ──────────────────────────────────────

struct Hover {
    struct Model { int clicks = 0; };
    struct Clicked_ {};
    using Msg = std::variant<Clicked_>;
    using Cmd = mayag::Cmd<Msg>;
    using Sub = mayag::Sub<Msg>;
    static Cmd init(Model&) { return {}; }
    static Cmd update(Model& m, Clicked_) { ++m.clicks; return {}; }
    static Node view(const Model&) {
        return v(v(h(text<"Go">) | pad(10) | bg(var::surface) | hovered(bg(var::accent))
                   | transition() | id<"go">)
                 | pad(20) | hovered(bg(colors::red)) | grow())
             | size(300, 200);
    }
    static std::optional<Msg> on(const Model&, const mayag::Clicked& c) {
        if (is(c, "go")) return Clicked_{};
        return std::nullopt;
    }
};

static void test_live_app() {
    section("live headless app");
    AppConfig cfg;
    cfg.size = {300, 200};
    const int rc = run_headless<Hover>(cfg, [](auto& rt) {
        const auto fill_of = [&](std::string_view id) {
            const Node* n = rt.host().tree().find(node_id(id));
            return n ? n->style().fill.color : Color<Srgb>{};
        };
        rt.tick();
        check(same(fill_of("go"), th.surface), "the host resolves tokens before painting");
        rt.window().move_to({35, 35});
        rt.tick();
        check(rt.input().hovered() == node_id("go"), "hit test lands on the button");
        const auto first = fill_of("go");
        check(!same(first, th.surface) && !same(first, th.accent),
              "hover starts a transition in the live host");
        rt.window().drive_frames(true);
        for (int i = 0; i < 240 && rt.host().transitioning(); ++i) rt.tick();
        rt.window().drive_frames(false);
        check(same(fill_of("go"), th.accent, 2e-3f), "and settles on the hover colour");
        check(!rt.host().transitioning(), "then stops asking for frames (0% CPU)");
        rt.window().click({35, 35});
        rt.tick();
        check(rt.model().clicks == 1, "the hover-styled card does not steal the click");
        // Hover over the card margin: the card is hover-only, not a control.
        rt.window().move_to({5, 5});
        rt.tick();
        check(rt.input().hovered() != 0 && rt.input().hovered() != node_id("go"),
              "the card itself is hoverable");
        rt.window().click({5, 5});
        rt.tick();
        check(rt.model().clicks == 1, "clicking the card margin clicks nothing");
        check(rt.input().focused() == 0, "and focuses nothing");
    });
    check(rc == 0, "headless run");
}

int main() {
    test_inheritance();
    test_tokens();
    test_variants();
    test_pointer_propagation();
    test_groups();
    test_responsive_and_theme();
    test_runtime_elements();
    test_classes();
    test_idempotent_and_overlay_sized();
    test_transitions();
    test_painter_transform();
    test_live_app();
    std::printf("%d checks, %d failures\n%s\n", checks, failures, failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
