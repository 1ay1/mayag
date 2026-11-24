// tests/test_widget_scaling.cpp — a widget's frame costs what is on SCREEN, never
// what is in its data.
//
// The bugs this holds shut were all one shape: a frame that grew with the
// data. The file pane built every row of a folder (21 ms a scroll step at
// 3000 files); DrawList rescanned every instance for every glyph, then every
// restore rescanned again (paint 0.07 -> 6 ms). Each was found by a user, by
// feel. Here each widget is built at a small and a 100x larger data size with
// the SAME viewport, and the structure must not grow: node counts and
// instance counts are machine-independent, so this is a hard gate, not a
// timing test. DrawList's linearity is the one time ratio, with a wide margin.

#include <mayag/mayag.hpp>
#include <mayag/widget/code_view.hpp>
#include <mayag/widget/file_pane.hpp>
#include <mayag/widget/file_tree.hpp>
#include <mayag/widget/picker.hpp>
#include <mayag/widget/problems.hpp>
#include <mayag/widget/ssh_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mayag;
using namespace mayag::dsl;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
std::size_t nodes(const Node& n) {
    std::size_t k = 1;
    for (const auto& c : n.children()) k += nodes(c);
    return k;
}

Node pane_at(std::size_t n, Ctx& c) {
    FilePaneState st;
    st.scroll.viewport = Vec2{300.0f, 600.0f};
    st.scroll.offset.y = 26.0f * static_cast<float>(n / 2);   // the middle of the folder
    FilePaneProps p;
    p.path = "/tmp/x";
    for (std::size_t i = 0; i < n; ++i) p.entries.push_back({"file_" + std::to_string(i) + ".cpp", false, false, i, "", "", 0, 0, 0.0f});
    return file_pane(c, "files", st, p, [](FilePaneOp) { return 0; });
}
Node tree_at(std::size_t n, Ctx& c) {
    std::vector<TreeRow> rows;
    for (std::size_t i = 0; i < n; ++i) rows.push_back({"f" + std::to_string(i), 1});
    ScrollState sc;
    sc.viewport = Vec2{240.0f, 600.0f};
    sc.offset.y = 22.0f * static_cast<float>(n / 2);
    return file_tree(c, "tree", rows, 0, sc);
}
Node code_at(std::size_t n, Ctx& c) {
    std::string text;
    for (std::size_t i = 0; i < n; ++i) text += "    int x" + std::to_string(i) + " = f(a, b) + 1;   // line\n";
    auto ed = editor::EditorState::from_text(text, &editor::language_named("C++"));
    ed.scroll.viewport = Vec2{800.0f, 600.0f};
    ed.set_cursor({static_cast<int>(n / 2), 0});
    ed.scroll.offset.y = 20.0f * static_cast<float>(n / 2);
    return code_view(c, "code", ed, CodeTheme{});
}
/// The picker's lazy form: it must ask only for the rows it shows.
std::size_t picker_asked = 0;
Node picker_at(std::size_t n, Ctx& c) {
    OverlayList ol;
    c.overlays = &ol;
    TextEditState q;
    picker_asked = 0;
    picker(c, "pick", q, PickerCount{static_cast<int>(n), n},
           [&](int k) {
               ++picker_asked;
               return PickerRow{{.label = "item " + std::to_string(k), .detail = "src/a/b"}, {0, 1}};
           },
           static_cast<int>(n / 2));
    c.overlays = nullptr;
    return ol.empty() ? Node{} : ol.items().front().content;
}
Node problems_at(std::size_t n, Ctx& c) {
    std::vector<ProblemRow> rows;
    for (std::size_t i = 0; i < n; ++i) rows.push_back({.header = i % 20 == 0, .file = "a.cpp", .message = "unused variable", .line = static_cast<int>(i)});
    ScrollState sc;
    sc.viewport = Vec2{600.0f, 400.0f};
    sc.offset.y = 24.0f * static_cast<float>(n / 2);
    return problems_panel(c, "problems", rows, static_cast<int>(n / 2), sc);
}

template <class Build>
void scales(const char* what, Build build) {
    Ctx c;
    const std::size_t small = nodes(build(100, c)), big = nodes(build(10000, c));
    // same viewport: the same rows on screen, give or take a few at the edges
    check(big <= small + small / 4 + 8,
          std::string{what} + ": " + std::to_string(small) + " nodes at 100 items, " + std::to_string(big) + " at 10000");
}

/// Paint: glyphs under nested backdrops (a list of cards, each with its own
/// text backdrop, inside one window), at N and 8N. Linear means ~8x time;
/// the quadratic bugs were 64x and worse.
double paint_ms(int cards) {
    DrawList dl;
    const auto t0 = std::chrono::steady_clock::now();
    dl.fill_rect(Rect{0, 0, 2000, 20000}, rgb<0x101010>);
    dl.set_text_backdrop(rgb<0x101010>);
    for (int i = 0; i < cards; ++i) {
        const auto outer = dl.text_backdrop();
        const float y = static_cast<float>(i) * 30.0f;
        dl.fill_rect(Rect{0, y, 400, 28}, rgb<0x202020>);
        dl.set_text_backdrop(rgb<0x202020>);
        for (int g = 0; g < 40; ++g)
            dl.glyph(Rect{static_cast<float>(g) * 9.0f, y + 6.0f, 8, 14}, Rect{0, 0, 1, 1}, rgb<0xEEEEEE>);
        dl.restore_text_backdrop(outer);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main() {
    std::printf("mayag scaling: a frame is O(screen), not O(data)\n");
    scales("file_pane", pane_at);
    scales("file_tree", tree_at);
    scales("problems_panel", problems_at);
    scales("ssh_manager", [](std::size_t n, Ctx& c) {
        std::vector<SshHost> hosts;
        for (std::size_t i = 0; i < n; ++i)
            hosts.push_back({.alias = "host-" + std::to_string(i), .user = "deploy", .hostname = "10.0." + std::to_string(i % 250) + ".1",
                             .tags = {i % 3 ? "web" : "db"}, .pinned = i % 17 == 0});
        SshManagerState st;
        st.list_scroll.viewport = Vec2{280.0f, 600.0f};
        st.list_scroll.offset.y = 44.0f * static_cast<float>(n / 2);
        st.sel = static_cast<int>(n / 2);
        return ssh_manager(c, "mgr", hosts, st, SshManagerProps{}, [](SshManagerOp) { return 0; });
    });
    scales("picker", picker_at);
    {
        Ctx c;
        (void)picker_at(10000, c);
        check(picker_asked <= 40, "picker: built " + std::to_string(picker_asked) + " rows of 10000 (only those on screen)");
    }
    scales("code_view", code_at);
    double small = 1e9, big = 1e9;   // best of a few: noise only ever adds
    for (int r = 0; r < 5; ++r) { small = std::min(small, paint_ms(500)); big = std::min(big, paint_ms(4000)); }
    check(big < small * 24.0, "DrawList: 8x the cards costs " + std::to_string(big / std::max(small, 1e-6)).substr(0, 4) +
                                  "x the time (linear ~8x; the quadratic bugs were 64x+)");
    std::printf("%s  %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
