// tests/test_widget_headers.cpp — the widget boundary, enforced
//
// Three rules, checked against the source tree (MAYAG_SOURCE_DIR):
//
//   1. <mayag/mayag.hpp> includes no widget, directly or through anything it
//      includes. Widgets are opt-in, one header each, as in maya.
//   2. Nothing outside include/mayag/widget/ includes a widget header: the
//      framework never depends on its own vocabulary.
//   3. <mayag/widget/all.hpp> lists every header in the folder, so the
//      prototyping include never silently misses one.
//
// Plus the DSL that replaced several widget-ish idioms: each / when / textf
// and piping runtime Nodes, checked on built trees.

#include <mayag/mayag.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef MAYAG_SOURCE_DIR
#  error "MAYAG_SOURCE_DIR must point at the source tree"
#endif

namespace fs = std::filesystem;
using namespace mayag;
using namespace mayag::dsl;

namespace {

int failures = 0;
void check(bool c, const std::string& what) {
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
    if (!c) ++failures;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const fs::path kRoot = fs::path(MAYAG_SOURCE_DIR) / "include" / "mayag";

/// The mayag headers `p` includes, as paths relative to include/mayag.
std::vector<std::string> includes_of(const fs::path& p) {
    std::vector<std::string> out;
    static const std::regex re(R"((?:^|\n)[ \t]*#\s*include\s*[<"]([^>"]+)[>"])");
    // Only real directives: a `#include` inside a comment (mayag.hpp shows
    // how to include widgets) is documentation, not a dependency.
    const std::string src = std::regex_replace(slurp(p), std::regex(R"(//[^\n]*)"), "");
    for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it) {
        const std::string inc = (*it)[1];
        fs::path target;
        if (inc.rfind("mayag/", 0) == 0) target = kRoot.parent_path() / inc;
        else target = p.parent_path() / inc;
        target = target.lexically_normal();
        if (fs::exists(target)) out.push_back(fs::relative(target, kRoot).generic_string());
    }
    return out;
}

bool is_widget(const std::string& rel) { return rel.rfind("widget/", 0) == 0; }

}  // namespace

static void test_boundary() {
    std::printf("-- the widget boundary\n");

    // 1. the umbrella's transitive closure has no widget in it
    std::set<std::string> seen;
    std::vector<std::string> todo{"mayag.hpp"};
    std::string first_widget;
    while (!todo.empty()) {
        const std::string cur = todo.back();
        todo.pop_back();
        if (!seen.insert(cur).second) continue;
        if (is_widget(cur) && first_widget.empty()) first_widget = cur;
        for (const auto& inc : includes_of(kRoot / cur)) todo.push_back(inc);
    }
    check(seen.size() > 20, "walked mayag.hpp's include graph (" + std::to_string(seen.size()) + " headers)");
    check(first_widget.empty(), "mayag.hpp pulls in no widget" +
                                (first_widget.empty() ? std::string{} : " (found " + first_widget + ")"));

    // 2. no header outside widget/ includes one inside it
    std::string offender;
    for (const auto& e : fs::recursive_directory_iterator(kRoot)) {
        if (!e.is_regular_file() || e.path().extension() != ".hpp") continue;
        const std::string rel = fs::relative(e.path(), kRoot).generic_string();
        if (is_widget(rel)) continue;
        for (const auto& inc : includes_of(e.path()))
            if (is_widget(inc) && offender.empty()) offender = rel + " -> " + inc;
    }
    check(offender.empty(), "nothing outside widget/ includes a widget" +
                            (offender.empty() ? std::string{} : " (" + offender + ")"));

    // 3. all.hpp is complete
    std::set<std::string> listed;
    for (const auto& inc : includes_of(kRoot / "widget" / "all.hpp")) listed.insert(inc);
    std::string missing;
    for (const auto& e : fs::directory_iterator(kRoot / "widget")) {
        const std::string rel = "widget/" + e.path().filename().string();
        if (rel == "widget/all.hpp" || e.path().extension() != ".hpp") continue;
        if (!listed.contains(rel) && missing.empty()) missing = rel;
    }
    check(missing.empty(), "widget/all.hpp lists every widget" +
                           (missing.empty() ? std::string{} : " (missing " + missing + ")"));
}

struct Item { std::string name; int n; };

static Node helper(int k) { return v(textf("helper {}", k)); }

static void test_dynamic_children() {
    std::printf("-- each / when / textf / Node pipes\n");
    const std::vector<Item> items{{"a", 1}, {"b", 2}, {"c", 3}};
    const std::vector<Node> extra{text<"x">, text<"y">};

    const Node n = v(text<"head">,
                     each(items, [](const Item& it, std::size_t i) {
                         return h(text_of(it.name), textf("#{}", it.n)) | id_of("row", i);
                     }),
                     when(items.empty(), text<"empty">),
                     when(items.size() == 3, text<"three">, text<"other">),
                     extra,
                     helper(7) | pad(4) | hovered(bg(colors::red)))
                 | gap(6);

    check(n.children().size() == 1 + 3 + 0 + 1 + 2 + 1, "fragments and vectors splice into the parent");
    check(n.find(node_id("row", 2)) != nullptr, "each passes the index");
    check(n.children()[1].children()[1].text() == "#1", "textf formats and owns its bytes");
    check(n.children()[4].text() == "three", "when(c, a, b) picks a branch");
    const Node& last = n.children().back();
    check(last.style().layout.padding.top == 4.0f && last.cascade() != nullptr,
          "a runtime Node pipes modifiers and variants directly");
    check(last.children()[0].text() == "helper 7", "and keeps its children");

    // Nested fragments, and each() returning a fragment.
    const Node g = h(each(items, [](const Item& it) {
        return when(it.n % 2 == 1, text_of(it.name));
    }));
    check(g.children().size() == 2, "each may return when(...): odd items only");

    // z(): every node after the first floats, fragment nodes included.
    const Node zz = z(box() | size(10, 10), each(items, [](const Item&) { return box() | size(2, 2); }));
    bool all_abs = zz.children().size() == 4;
    for (std::size_t i = 1; i < zz.children().size(); ++i)
        all_abs = all_abs && zz.children()[i].style().layout.position == Positioning::absolute;
    check(all_abs && zz.children()[0].style().layout.position == Positioning::flow,
          "z() floats every fragment node after the first");
}

int main() {
    test_boundary();
    test_dynamic_children();
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
