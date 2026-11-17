// tests/test_widget_behaviour.cpp — the keyboard and click handling that
// widgets ship (tree_key, tree_click, picker_key), checked without a window.

#include <mayag/mayag.hpp>
#include <mayag/widget/file_tree.hpp>
#include <mayag/widget/picker.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {
int failures = 0, checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

// src/          0
//   app.cpp     1
//   util/       2
//     a.hpp     3
// README.md     4
std::vector<TreeRow> rows() {
    return {{.name = "src", .depth = 0, .dir = true, .open = true},
            {.name = "app.cpp", .depth = 1},
            {.name = "util", .depth = 1, .dir = true, .open = true},
            {.name = "a.hpp", .depth = 2},
            {.name = "README.md", .depth = 0}};
}

void test_tree() {
    std::printf("\nfile tree\n");
    using A = TreeIntent::Act;
    const auto r = rows();
    ScrollState sc;
    // What layout would have measured: 5 rows in a 3-row viewport.
    sc.viewport = {200, 3 * kTreeRowH};
    sc.content = {200, 5 * kTreeRowH};
    sc.max_offset = {0, 2 * kTreeRowH};
    int sel = 0;

    auto in = tree_key(Key::down, sel, sc, r);
    check(in.act == A::moved && sel == 1, "down moves the selection");
    (void)tree_key(Key::j, sel, sc, r);
    check(sel == 2, "j does too");
    (void)tree_key(Key::up, sel, sc, r); (void)tree_key(Key::k, sel, sc, r);
    check(sel == 0, "up and k move back");
    (void)tree_key(Key::up, sel, sc, r);
    check(sel == 0, "up stops at the first row");

    sel = 4;
    (void)tree_key(Key::down, sel, sc, r);
    check(sel == 4, "down stops at the last row");
    check(sc.offset.y > 0.0f, "moving below the viewport scrolls it into view");
    (void)tree_key(Key::up, sel, sc, r); (void)tree_key(Key::up, sel, sc, r); (void)tree_key(Key::up, sel, sc, r);
    (void)tree_key(Key::up, sel, sc, r);
    check(sel == 0 && sc.offset.y == 0.0f, "and moving back up scrolls back");

    sel = 2;
    check(tree_key(Key::left, sel, sc, r).act == A::collapse, "left on an open folder collapses it");
    sel = 3;
    in = tree_key(Key::left, sel, sc, r);
    check(in.act == A::moved && sel == 2, "left on a file goes to its folder");
    sel = 0;
    check(tree_key(Key::right, sel, sc, r).act == A::none, "right on an open folder does nothing");
    auto closed = r; closed[0].open = false;
    check(tree_key(Key::right, sel, sc, closed).act == A::expand, "right on a closed folder expands it");

    sel = 1;
    in = tree_key(Key::enter, sel, sc, r);
    check(in.act == A::open && in.row == 1, "enter opens a file");
    check(tree_key(Key::space, sel, sc, r).act == A::preview, "space previews it");
    sel = 2;
    check(tree_key(Key::enter, sel, sc, r).act == A::toggle, "enter on a folder toggles it");
    check(tree_key(Key::escape, sel, sc, r).act == A::leave, "escape hands focus back");

    sel = 0;
    in = tree_click(3, 1, sel, r);
    check(sel == 3 && in.act == A::preview, "a click selects and previews a file");
    check(tree_click(3, 2, sel, r).act == A::open, "a double click opens it");
    check(tree_click(0, 1, sel, r).act == A::toggle, "a click on a folder toggles it");
    check(tree_click(9, 1, sel, r).act == A::none && sel == 0, "a click past the end is ignored");

    sel = 0;
    check(tree_key(Key::down, sel, sc, {}).act == A::none, "an empty tree ignores keys");
}

void test_picker() {
    std::printf("\npicker\n");
    using A = PickerIntent::Act;
    TextEditState q;
    int sel = 0;
    check(picker_key(q, Key::down, {}, sel, 3).act == A::moved && sel == 1, "down moves");
    (void)picker_key(q, Key::n, Mods{.ctrl = true}, sel, 3);
    check(sel == 2, "ctrl-n moves");
    (void)picker_key(q, Key::down, {}, sel, 3);
    check(sel == 2, "down stops at the last item");
    (void)picker_key(q, Key::p, Mods{.ctrl = true}, sel, 3);
    (void)picker_key(q, Key::up, {}, sel, 3);
    (void)picker_key(q, Key::up, {}, sel, 3);
    check(sel == 0, "ctrl-p and up move back, stopping at the first");
    sel = 2;
    const auto in = picker_key(q, Key::enter, {}, sel, 3);
    check(in.act == A::choose && in.index == 2, "enter chooses the selected item");
    check(picker_key(q, Key::escape, {}, sel, 3).act == A::close, "escape closes");
    q.insert("ab");
    sel = 2;
    check(picker_key(q, Key::backspace, {}, sel, 3).act == A::edited && q.text == "a" && sel == 0,
          "other keys edit the query and reset the selection");
    sel = 5;
    check(picker_key(q, Key::enter, {}, sel, 0).index == 0, "enter with no items picks index 0");
}
}  // namespace

int main() {
    std::printf("mayag widget behaviour\n======================\n");
    test_tree();
    test_picker();
    std::printf("\n%s  %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
