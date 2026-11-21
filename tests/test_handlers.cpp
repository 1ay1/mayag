// tests/test_handlers.cpp — inline event handlers, end to end
//
// on_click / on_slide / on_enter with no id<> and no on(Clicked): the host
// finds the handler on the node the gesture landed on, turns its result
// into the program's Msg, and folds it on the same turn.

#include <mayag/mayag.hpp>
#include <mayag/app/handlers.hpp>
#include <mayag/widget/slider.hpp>
#include <cstdio>
#include <array>
using namespace mayag; using namespace mayag::dsl;
struct App {
    struct Model { int saves = 0; float vol = 0.5f; int opened = -1; int hovered = -1; bool wifi = false; float bright = 0.0f; };
    struct Save {}; struct SetVol { float v; }; struct Open { int i; }; struct Hover { int i; };
    using Msg = std::variant<Save, SetVol, Open, Hover, Edit<Model>>;
    using Cmd = mayag::Cmd<Msg>;
    static Cmd init(Model&) { return {}; }
    static Cmd update(Model& m, Save) { ++m.saves; return {}; }
    static Cmd update(Model& m, SetVol s) { m.vol = s.v; return {}; }
    static Cmd update(Model& m, Open o) { m.opened = o.i; return {}; }
    static Cmd update(Model& m, Hover h) { m.hovered = h.i; return {}; }
    static Cmd update(Model& m, Edit<Model> e) { e(m); return {}; }
    static Node view(const Model& m) {
        return v(h(text<"Save">) | pad(10) | bg(var::accent) | on_click(Save{}),
                 slider(ctx(), "vol", m.vol, 100) | on_slide([](Vec2 p) { return SetVol{p.x / 100}; }),
                 v(each(std::array{0, 1, 2}, [](int i) {
                     return h(textf("row {}", i)) | pad(6)
                          | on_click([i](const Clicked& c) -> std::optional<Msg> {
                                return c.count == 2 ? std::optional<Msg>{Open{i}} : std::nullopt; })
                          | on_enter(Hover{i});
                 })),
                 h(text<"wifi">) | pad(6) | on_click(toggle(&Model::wifi)),
                 box() | size(100, 20) | bg(var::surface) | on_slide(set_from_x(&Model::bright, 100, 0, 10)))
             | gap(10) | pad(10);
    }
};
int main() {
    AppConfig cfg; cfg.size = {300, 300};
    return run_headless<App>(cfg, [](auto& rt) {
        int fails = 0;
        auto ok = [&](bool c, const char* w) { std::printf("  %s %s\n", c ? "ok  " : "FAIL", w); fails += !c; };
        rt.tick();
        const Node& root = rt.host().tree();
        const Vec2 save = root.children()[0].frame().center();
        rt.window().click(save); rt.tick();
        ok(rt.model().saves == 1, "on_click(Save{}) with no id, no on()");
        const Rect sl = root.children()[1].frame();
        rt.window().push(MouseDown{sl.origin + Vec2{75, 12}, MouseButton::left, {}, 1});
        rt.window().push(MouseUp{sl.origin + Vec2{75, 12}, MouseButton::left, {}});
        rt.tick();
        ok(std::abs(rt.model().vol - 0.75f) < 0.02f, "on_slide sets the value from the press point");
        const Vec2 row1 = rt.host().tree().children()[2].children()[1].frame().center();
        rt.window().move_to(row1); rt.tick();
        ok(rt.model().hovered == 1, "on_enter");
        rt.window().click(row1); rt.tick();
        ok(rt.model().opened == -1, "single click: optional empty, nothing sent");
        rt.window().double_click(row1); rt.tick();
        ok(rt.model().opened == 1, "double click opens (handler reads the gesture)");
        // Tab cycles through every element with a handler (they are controls).
        int saw_save = 0;
        for (int i = 0; i < 6 && !saw_save; ++i) {
            rt.window().press_key(Key::tab); rt.tick();
            saw_save = rt.input().focused() == rt.host().tree().children()[0].style().id;
        }
        ok(saw_save, "elements with handlers are Tab stops");
        rt.window().press_key(Key::enter); rt.tick();
        ok(rt.model().saves == 2, "Enter on the focused handler element clicks it");
        const Vec2 wifi = rt.host().tree().children()[3].frame().center();
        rt.window().click(wifi); rt.tick();
        ok(rt.model().wifi, "on_click(toggle(&Model::wifi)) flips the field via one Edit message");
        rt.window().click(wifi); rt.tick();
        ok(!rt.model().wifi, "and back");
        const Rect br = rt.host().tree().children()[4].frame();
        rt.window().push(MouseDown{br.origin + Vec2{20, 10}, MouseButton::left, {}, 1});
        rt.window().push(MouseMove{br.origin + Vec2{60, 10}, {}, {}});
        rt.window().push(MouseUp{br.origin + Vec2{60, 10}, MouseButton::left, {}});
        rt.tick();
        ok(std::abs(rt.model().bright - 6.0f) < 0.2f, "on_slide(set_from_x(...)) maps press+drag onto the field's range");
        {
            // a program whose Msg is a tree (agentty / hand): `struct Msg : variant<Group, ...>`,
            // `Group = variant<Leaf, ...>` -- a handler's leaf lands in its group
            struct A {}; struct B { int n; }; struct C {};
            using Group = std::variant<A, B>;
            struct Tree : std::variant<Group, C> { using std::variant<Group, C>::variant; };
            const auto lifted = to_program_msg<Tree>(mayag::detail::handlers::wrap(B{7}));
            ok(lifted && std::holds_alternative<Group>(*lifted) && std::get<B>(std::get<Group>(*lifted)).n == 7,
               "a handler's leaf message lifts into a nested, derived Msg");
            ok(to_program_msg<Tree>(mayag::detail::handlers::wrap(C{})).has_value(), "a top-level leaf too");
            ok(!to_program_msg<Tree>(mayag::detail::handlers::wrap(1.5)).has_value(), "a type outside the tree is refused");
        }
        return fails;
    });
}
