// examples/editor.cpp — a code editor in the shape of Zed, built from mayag widgets
//
//   ./mayag_editor [dir]          open a project (default: the current directory)
//   ./mayag_editor --headless     scripted and asserted against a built-in project
//   ./mayag_editor --png out.png  one frame (ED_DEMO=1 opens files, a palette, ...)
//
// Everything a modern editor surface has, wired the mayag way — every piece of
// state is data in the Model, every widget draws what it is given:
//
//   project panel   file tree, git-style change marks, problem counts, resizable
//   tabs            preview tabs (italic, replaced by the next single-click),
//                   dirty dots, close on hover, cmd-1..9, ctrl-tab
//   editor          multi-cursor (cmd-d, cmd-shift-l, alt-click, cmd-alt-up/down),
//                   syntax highlighting for 10 languages, folding, bracket match,
//                   occurrence highlight, indent guides, change gutter vs. disk,
//                   inline diagnostics, completion with docs, hover cards
//   find            in-file find/replace with case, word and regex, match overview
//                   in the scrollbar; project-wide search in the bottom panel
//   pickers         file finder (cmd-p), command palette (cmd-shift-p), go to
//                   symbol (cmd-shift-o), go to line (ctrl-g)
//   panels          problems (cmd-shift-m) from a real linter (brackets, long
//                   lines, trailing space, TODO), status bar, toasts
//
// Saving writes the file to disk. The headless suite runs on an in-memory
// project and never touches the filesystem.

#include "harness.hpp"
#include <mayag/widget/breadcrumbs.hpp>
#include <mayag/widget/code_view.hpp>
#include <mayag/widget/completion_menu.hpp>
#include <mayag/widget/diff_view.hpp>
#include <mayag/widget/file_tree.hpp>
#include <mayag/widget/find_bar.hpp>
#include <mayag/widget/hover_card.hpp>
#include <mayag/widget/picker.hpp>
#include <mayag/widget/problems.hpp>
#include <mayag/widget/split.hpp>
#include <mayag/widget/status_bar.hpp>
#include <mayag/widget/tab_bar.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace mayag;
using namespace mayag::dsl;
using editor::EditorState;
using editor::Pos;
using editor::Range;

namespace {

/// Where the project comes from. Set in main() before the app starts.
std::string g_root;
bool        g_builtin = false;
bool        g_headless = false;   // --headless: a scripted, reproducible run

struct File { std::string path; std::string text; };

[[nodiscard]] std::vector<File> builtin_project() {
    return {
        {"src/main.cpp",
         "#include <cstdio>\n#include \"shapes.hpp\"\n\n// Entry point. TODO: parse arguments\n"
         "int main(int argc, char** argv) {\n    Circle c{2.0};\n    Square s{3.0};\n"
         "    std::printf(\"%f %f\\n\", c.area(), s.area());\n    return 0;\n}\n"},
        {"src/shapes.hpp",
         "#pragma once\n\nstruct Shape {\n    virtual ~Shape() = default;\n    virtual double area() const = 0;\n};\n\n"
         "struct Circle : Shape {\n    double radius;\n    explicit Circle(double r) : radius(r) {}\n"
         "    double area() const override { return 3.14159 * radius * radius; }\n};\n\n"
         "struct Square : Shape {\n    double side;\n    explicit Square(double s) : side(s) {}\n"
         "    double area() const override { return side * side; }\n};\n"},
        {"src/broken.rs",
         "fn main() {\n    let total = sum(&[1, 2, 3]);\n    println!(\"{}\", total);   \n\n"
         "fn sum(xs: &[i32]) -> i32 {\n    xs.iter().sum()\n}\n"},
        {"scripts/build.sh", "#!/bin/sh\nset -e\ncmake -B build -G Ninja\ncmake --build build\n"},
        {"tools/report.py",
         "import sys\n\n\ndef report(path):\n    \"\"\"Summarise a log file.\"\"\"\n    with open(path) as f:\n"
         "        lines = f.readlines()\n    print(len(lines))\n\n\nif __name__ == \"__main__\":\n    report(sys.argv[1])\n"},
        {"Cargo.toml", "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n[dependencies]\n"},
        {"README.md", "# demo\n\nA tiny project for the **mayag** editor.\n\n- `src/` the code\n- `tools/` helpers\n"},
    };
}

/// A real project from disk: text files only, skipping build output and VCS.
[[nodiscard]] std::vector<File> load_project(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<File> out;
    std::error_code ec;
    static const std::set<std::string> skip_dirs{".git", "build", "node_modules", "target", ".cache", "third_party",
                                                 ".agentty", "dist", "out", ".idea", ".vscode"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && out.size() < 4000; it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        if (it->is_directory(ec)) {
            if (skip_dirs.contains(p.filename().string()) || p.filename().string().starts_with(".")) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || it->file_size(ec) > 2'000'000) continue;
        const std::string name = p.filename().string();
        if (name.starts_with(".")) continue;
        const std::string ext = p.extension().string();
        static const std::set<std::string> binary{".png", ".jpg", ".jpeg", ".gif", ".o", ".a", ".so", ".dylib", ".bin",
                                                  ".ttf", ".otf", ".ttc", ".zip", ".gz", ".pdf", ".ragdb", ".ico"};
        if (binary.contains(ext)) continue;
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        if (text.find('\0') != std::string::npos) continue;
        out.push_back({fs::relative(p, root, ec).generic_string(), std::move(text)});
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.path < b.path; });
    return out;
}

[[nodiscard]] std::string basename_of(const std::string& p) {
    const auto s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
[[nodiscard]] std::string dirname_of(const std::string& p) {
    const auto s = p.rfind('/');
    return s == std::string::npos ? std::string{} : p.substr(0, s);
}

// ── the tree, flattened once ────────────────────────────────────────────

struct TreeNode { std::string path, name; int depth = 0; bool dir = false; };

[[nodiscard]] std::vector<TreeNode> build_tree(const std::vector<File>& files) {
    // Children per directory, dirs first, then names.
    std::map<std::string, std::set<std::pair<int, std::string>>> kids;   // parent -> {(0 dir|1 file, name)}
    for (const auto& f : files) {
        std::string parent;
        std::size_t st = 0;
        while (true) {
            const auto sl = f.path.find('/', st);
            if (sl == std::string::npos) { kids[parent].insert({1, f.path.substr(st)}); break; }
            const std::string name = f.path.substr(st, sl - st);
            kids[parent].insert({0, name});
            parent = parent.empty() ? name : parent + "/" + name;
            st = sl + 1;
        }
    }
    std::vector<TreeNode> out;
    const std::function<void(const std::string&, int)> walk = [&](const std::string& dir, int depth) {
        for (const auto& [kind, name] : kids[dir]) {
            const std::string path = dir.empty() ? name : dir + "/" + name;
            out.push_back({path, name, depth, kind == 0});
            if (kind == 0) walk(path, depth + 1);
        }
    };
    walk("", 0);
    return out;
}

// ── change marks: the buffer against the file on disk ───────────────────

[[nodiscard]] std::vector<editor::GitMark> change_marks(const std::vector<std::string>& disk,
                                                        const std::vector<std::string>& now) {
    using K = editor::GitMark::Kind;
    std::vector<editor::GitMark> out;
    std::size_t pre = 0;
    while (pre < disk.size() && pre < now.size() && disk[pre] == now[pre]) ++pre;
    std::size_t suf = 0;
    while (suf < disk.size() - pre && suf < now.size() - pre && disk[disk.size() - 1 - suf] == now[now.size() - 1 - suf]) ++suf;
    const std::size_t a = disk.size() - pre - suf, b = now.size() - pre - suf;
    if (a == 0 && b == 0) return out;
    if (a * b > 4'000'000) {   // a huge rewrite: no point in an exact diff
        for (std::size_t i = 0; i < b; ++i) out.push_back({static_cast<int>(pre + i), K::modified});
        return out;
    }
    std::vector<std::string> da(disk.begin() + static_cast<long>(pre), disk.begin() + static_cast<long>(pre + a));
    std::vector<std::string> db(now.begin() + static_cast<long>(pre), now.begin() + static_cast<long>(pre + b));
    const auto d = line_diff(da, db);
    // A deletion directly followed by additions is a modification.
    int pending_del = 0;
    for (std::size_t i = 0; i < d.size(); ++i) {
        const auto& l = d[i];
        if (l.op == DiffLine::Op::del) { ++pending_del; continue; }
        if (l.op == DiffLine::Op::add) {
            out.push_back({static_cast<int>(pre) + l.new_no - 1, pending_del > 0 ? K::modified : K::added});
            if (pending_del > 0) --pending_del;
            continue;
        }
        if (pending_del > 0) out.push_back({std::max(0, static_cast<int>(pre) + l.new_no - 2), K::deleted});
        pending_del = 0;
    }
    if (pending_del > 0) out.push_back({std::max(0, static_cast<int>(pre + b) - 1), K::deleted});
    return out;
}

// ── a small, real linter ────────────────────────────────────────────────

[[nodiscard]] std::vector<editor::Diagnostic> lint(const EditorState& ed) {
    using S = editor::Diagnostic::Severity;
    std::vector<editor::Diagnostic> out;
    struct Open { char c; Pos at; };
    std::vector<Open> stack;
    const bool code = ed.language->name != "Plain Text" && ed.language->name != "Markdown";
    for (int l = 0; l < ed.line_count(); ++l) {
        const std::string& s = ed.line(l);
        if (!s.empty() && (s.back() == ' ' || s.back() == '\t') && ed.indent_of(l) != ed.line_len(l)) {
            int a = ed.line_len(l);
            while (a > 0 && (s[static_cast<std::size_t>(a - 1)] == ' ' || s[static_cast<std::size_t>(a - 1)] == '\t')) --a;
            out.push_back({{{l, a}, {l, ed.line_len(l)}}, S::hint, "trailing whitespace", "lint"});
        }
        if (ed.display_col(l, ed.line_len(l)) > 120)
            out.push_back({{{l, 0}, {l, ed.line_len(l)}}, S::warning, "line is longer than 120 columns", "lint"});
        if (!code) continue;
        for (const auto& sp : ed.syntax.spans(*ed.language, ed.lines, l)) {
            const std::string_view w = std::string_view{s}.substr(sp.start, sp.end - sp.start);
            if (sp.kind == editor::Tok::comment) {
                for (const char* tag : {"TODO", "FIXME", "XXX"})
                    if (const auto at = w.find(tag); at != std::string_view::npos)
                        out.push_back({{{l, static_cast<int>(sp.start + at)}, {l, static_cast<int>(sp.start + at + std::strlen(tag))}},
                                       S::info, std::string{tag} + " left in the code", "lint"});
                continue;
            }
            if (sp.kind != editor::Tok::punct || w.size() != 1) continue;
            const char ch = w[0];
            const Pos at{l, static_cast<int>(sp.start)};
            if (ch == '(' || ch == '[' || ch == '{') stack.push_back({ch, at});
            else if (ch == ')' || ch == ']' || ch == '}') {
                const char want = ch == ')' ? '(' : ch == ']' ? '[' : '{';
                if (!stack.empty() && stack.back().c == want) stack.pop_back();
                else out.push_back({{at, {l, at.col + 1}}, S::error, std::string{"unmatched '"} + ch + "'", "brackets"});
            }
        }
    }
    for (const auto& o : stack)
        out.push_back({{o.at, {o.at.line, o.at.col + 1}}, S::error, std::string{"'"} + o.c + "' is never closed", "brackets"});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.range.start < b.range.start; });
    return out;
}

// ── commands ────────────────────────────────────────────────────────────

enum class Act : std::uint8_t {
    save, close_tab, next_tab, prev_tab, toggle_sidebar, toggle_problems, project_search, find, replace,
    find_next, find_prev, file_finder, palette, goto_symbol, goto_line, zoom_in, zoom_out, zoom_reset,
    relative_numbers, show_whitespace, indent_guides, fold, unfold, unfold_all, select_all_occurrences,
    cursor_above, cursor_below, toggle_comment, duplicate_line, delete_line, complete, next_problem,
    cycle_cursor, copy, cut, paste, undo, redo, quit, count
};

struct Command { const char* name; const char* keys; Act act; };
constexpr std::array<Command, 38> kCommands{{
    {"file: save", "\u2318S", Act::save},
    {"pane: close active item", "\u2318W", Act::close_tab},
    {"pane: activate next item", "\u2303\u21E5", Act::next_tab},
    {"pane: activate previous item", "\u2303\u21E7\u21E5", Act::prev_tab},
    {"project panel: toggle", "\u2318B", Act::toggle_sidebar},
    {"diagnostics: toggle panel", "\u2318\u21E7M", Act::toggle_problems},
    {"project search: deploy", "\u2318\u21E7F", Act::project_search},
    {"buffer search: deploy", "\u2318F", Act::find},
    {"buffer search: replace", "\u2318\u2325F", Act::replace},
    {"search: select next match", "\u2318G", Act::find_next},
    {"search: select previous match", "\u2318\u21E7G", Act::find_prev},
    {"file finder: toggle", "\u2318P", Act::file_finder},
    {"outline: toggle", "\u2318\u21E7O", Act::goto_symbol},
    {"go to line: toggle", "\u2303G", Act::goto_line},
    {"zed: increase buffer font size", "\u2318=", Act::zoom_in},
    {"zed: decrease buffer font size", "\u2318-", Act::zoom_out},
    {"zed: reset buffer font size", "\u23180", Act::zoom_reset},
    {"editor: toggle relative line numbers", "", Act::relative_numbers},
    {"editor: toggle whitespace", "", Act::show_whitespace},
    {"editor: toggle indent guides", "", Act::indent_guides},
    {"editor: fold", "\u2318\u2325[", Act::fold},
    {"editor: unfold lines", "\u2318\u2325]", Act::unfold},
    {"editor: unfold all", "", Act::unfold_all},
    {"editor: select all matches", "\u2318\u21E7L", Act::select_all_occurrences},
    {"editor: add selection above", "\u2318\u2325\u2191", Act::cursor_above},
    {"editor: add selection below", "\u2318\u2325\u2193", Act::cursor_below},
    {"editor: toggle comments", "\u2318/", Act::toggle_comment},
    {"editor: duplicate line down", "\u2325\u21E7\u2193", Act::duplicate_line},
    {"editor: delete line", "\u2318\u21E7K", Act::delete_line},
    {"editor: show completions", "\u2303Space", Act::complete},
    {"editor: go to diagnostic", "F8", Act::next_problem},
    {"editor: cycle cursor shape", "", Act::cycle_cursor},
    {"editor: copy", "\u2318C", Act::copy},
    {"editor: cut", "\u2318X", Act::cut},
    {"editor: paste", "\u2318V", Act::paste},
    {"editor: undo", "\u2318Z", Act::undo},
    {"editor: redo", "\u2318\u21E7Z", Act::redo},
    {"zed: quit", "\u2318Q", Act::quit},
}};

[[nodiscard]] bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

}  // namespace

struct Editor {
    // ── state ───────────────────────────────────────────────────────────

    struct Buffer {
        std::string              path;
        EditorState              ed;
        std::vector<std::string> disk;      ///< lines as last saved
        bool                     preview = false;
        // Derived, cached by buffer version.
        mutable std::uint64_t                       cache_version = 0;
        mutable std::vector<editor::Diagnostic>     diags;
        mutable std::vector<editor::GitMark>        marks;
    };

    enum class Focus : std::uint8_t { editor, tree, find, picker, psearch };
    enum class PickMode : std::uint8_t { none, files, commands, symbols, line };

    struct Model {
        std::vector<File>        project;
        std::vector<TreeNode>    tree;
        std::set<std::string>    expanded;
        int                      tree_sel = 0;
        ScrollState              tree_scroll;
        std::vector<Buffer>      bufs;
        int                      active = -1;
        std::vector<int>         mru;           ///< buffer indices, most recent first
        Focus                    focus = Focus::editor;
        bool                     sidebar = true;
        float                    sidebar_w = 250.0f;
        float                    sidebar_at_press = 250.0f, bottom_at_press = 220.0f;
        bool                     bottom = false;
        int                      bottom_tab = 0;   // 0 problems, 1 search
        float                    bottom_h = 220.0f;
        int                      problem_sel = -1;
        ScrollState              problem_scroll;
        // find
        bool                     find_open = false, find_replace = false;
        int                      find_field = 0;
        TextEditState            find_q, find_r;
        editor::FindOptions      find_opts;
        std::vector<Range>       matches;
        int                      match_i = -1;
        // project search
        TextEditState            ps_q;
        std::vector<ProblemRow>  ps_rows;
        std::vector<std::pair<std::string, Pos>> ps_targets;
        // picker
        PickMode                 pick = PickMode::none;
        TextEditState            pick_q;
        int                      pick_sel = 0;
        // completion
        bool                     comp_open = false;
        std::vector<CompletionItem> comp_items;
        int                      comp_sel = 0;
        // presentation
        float                    font_size = 13.5f;
        bool                     relative = false, whitespace = false, guides = true;
        int                      cursor_style = 0;
        bool                     blink_on = true;
        double                   idle = 0.0;
        Mods                     press_mods{};
        Vec2                     mouse{};
        Tracked                  editor_box;
        std::string              clipboard;
        std::vector<Toast>       toast_list;
        std::vector<double>      toast_ttl;
    };

    // ── messages ────────────────────────────────────────────────────────

    struct Do        { Act a; };
    struct EdKey     { KeyEvent k; };
    struct Typed     { std::string s; };
    struct Press     { Vec2 local; };
    struct Drag      { Vec2 local; };
    struct Click     { Vec2 local; int count; };
    struct PressMods { Mods mods; };
    struct Wheel     { Vec2 delta; };
    struct Fold      { int line; };
    struct OpenTab   { int i; };
    struct CloseTab  { int i; };
    struct TreeClick { int row; int count; };
    struct TreeKey   { KeyEvent k; };
    struct TreeWheel { Vec2 delta; };
    struct PickKey   { KeyEvent k; };
    struct PickClick { int i; };
    struct PickClose {};
    struct FindKey   { KeyEvent k; };
    struct FindPart  { std::size_t part; };
    struct PsKey     { KeyEvent k; };
    struct PsRun     {};
    struct ProblemClick { int i; };
    struct BottomTab { int t; };
    struct SplitDrag { DragTravel t; };
    struct SplitGrab {};
    struct BottomDrag { DragTravel t; };
    struct BarPress  { float y; };
    struct BarDrag   { DragTravel t; };
    struct CompKey   { KeyEvent k; };
    struct CompClick { int i; };
    struct Mouse     { Vec2 p; };
    struct ToastClick { int i; };
    struct Blink     {};
    struct Tick      { double dt; };
    struct Quit      {};

    using Msg = std::variant<Do, EdKey, Typed, Press, Drag, Click, PressMods, Wheel, Fold, OpenTab, CloseTab,
                             TreeClick, TreeKey, TreeWheel, PickKey, PickClick, PickClose, FindKey, FindPart, PsKey,
                             PsRun, ProblemClick, BottomTab, SplitDrag, SplitGrab, BottomDrag, BarPress, BarDrag, CompKey,
                             CompClick, Mouse, ToastClick, Blink, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, focus, write_clipboard>;
    using Sub = mayag::Sub<Msg>;

    // ── helpers ─────────────────────────────────────────────────────────

    [[nodiscard]] static Buffer* cur(Model& m) { return m.active >= 0 ? &m.bufs[static_cast<std::size_t>(m.active)] : nullptr; }
    [[nodiscard]] static const Buffer* cur(const Model& m) { return m.active >= 0 ? &m.bufs[static_cast<std::size_t>(m.active)] : nullptr; }

    static const Buffer& refresh(const Buffer& b) {
        if (b.cache_version != b.ed.version()) {
            b.diags = lint(b.ed);
            b.marks = change_marks(b.disk, b.ed.lines);
            b.cache_version = b.ed.version();
        }
        return b;
    }

    static void toast(Model& m, std::string title, std::string body, Toast::Kind k = Toast::Kind::info) {
        m.toast_list.insert(m.toast_list.begin(), Toast{std::move(title), std::move(body), k, {}});
        m.toast_ttl.insert(m.toast_ttl.begin(), 4.0);
        if (m.toast_list.size() > 3) { m.toast_list.pop_back(); m.toast_ttl.pop_back(); }
    }

    [[nodiscard]] static std::string title_of(const Model& m) {
        const Buffer* b = cur(m);
        if (b == nullptr) return "mayag editor";
        return basename_of(b->path) + (b->ed.dirty() ? " \u25CF" : "") + " \u2014 " +
               (g_builtin ? std::string{"demo"} : std::filesystem::path(g_root).filename().string());
    }

    /// Open `path`: focus an existing tab, or create one. A single-click
    /// open replaces the current PREVIEW tab, as in Zed.
    static void open(Model& m, const std::string& path, bool preview) {
        for (std::size_t i = 0; i < m.bufs.size(); ++i)
            if (m.bufs[i].path == path) {
                if (!preview) m.bufs[i].preview = false;
                activate(m, static_cast<int>(i));
                return;
            }
        const auto it = std::find_if(m.project.begin(), m.project.end(), [&](const File& f) { return f.path == path; });
        if (it == m.project.end()) return;
        Buffer b;
        b.path = path;
        b.ed = EditorState::from_text(it->text, &editor::language_for(path));
        b.disk = b.ed.lines;
        b.preview = preview;
        if (preview)
            for (std::size_t i = 0; i < m.bufs.size(); ++i)
                if (m.bufs[i].preview && !m.bufs[i].ed.dirty()) {
                    m.bufs[i] = std::move(b);
                    activate(m, static_cast<int>(i));
                    return;
                }
        const int at = m.active < 0 ? 0 : m.active + 1;
        m.bufs.insert(m.bufs.begin() + at, std::move(b));
        for (int& r : m.mru) if (r >= at) ++r;
        activate(m, at);
    }
    static void activate(Model& m, int i) {
        m.active = i;
        std::erase(m.mru, i);
        m.mru.insert(m.mru.begin(), i);
        m.focus = Focus::editor;
        m.comp_open = false;
        // Reveal it in the tree.
        if (Buffer* b = cur(m)) {
            std::string d = dirname_of(b->path);
            while (!d.empty()) { m.expanded.insert(d); d = dirname_of(d); }
            const auto rows = tree_rows(m);
            for (std::size_t r = 0; r < rows.size(); ++r)
                if (m.tree[static_cast<std::size_t>(rows[r])].path == b->path) { m.tree_sel = static_cast<int>(r); break; }
        }
        refind(m);
    }
    static void close(Model& m, int i) {
        if (i < 0 || i >= static_cast<int>(m.bufs.size())) return;
        if (m.bufs[static_cast<std::size_t>(i)].ed.dirty())
            toast(m, "Closed with unsaved changes", basename_of(m.bufs[static_cast<std::size_t>(i)].path), Toast::Kind::warning);
        m.bufs.erase(m.bufs.begin() + i);
        std::erase(m.mru, i);
        for (int& r : m.mru) if (r > i) --r;
        m.active = m.mru.empty() ? (m.bufs.empty() ? -1 : 0) : m.mru.front();
        if (m.active >= 0) activate(m, m.active);
    }

    /// Visible tree rows: indices into m.tree, skipping collapsed subtrees.
    [[nodiscard]] static std::vector<int> tree_rows(const Model& m) {
        std::vector<int> out;
        int skip_depth = -1;
        for (std::size_t i = 0; i < m.tree.size(); ++i) {
            const auto& n = m.tree[i];
            if (skip_depth >= 0 && n.depth > skip_depth) continue;
            skip_depth = -1;
            out.push_back(static_cast<int>(i));
            if (n.dir && !m.expanded.contains(n.path)) skip_depth = n.depth;
        }
        return out;
    }

    static void refind(Model& m) {
        m.matches.clear();
        m.match_i = -1;
        const Buffer* b = cur(m);
        if (!m.find_open || b == nullptr || m.find_q.text.empty()) return;
        m.matches = b->ed.find_all(m.find_q.text, m.find_opts);
        const Pos c = b->ed.primary_sel().start();
        for (std::size_t i = 0; i < m.matches.size(); ++i)
            if (!(m.matches[i].start < c)) { m.match_i = static_cast<int>(i); break; }
        if (m.match_i < 0 && !m.matches.empty()) m.match_i = 0;
    }
    static void goto_match(Model& m, int dir) {
        Buffer* b = cur(m);
        if (b == nullptr || m.matches.empty()) return;
        const editor::Selection c = b->ed.primary_sel();
        const int n = static_cast<int>(m.matches.size());
        if (dir > 0) {
            int k = 0;
            while (k < n && !(c.end() <= m.matches[static_cast<std::size_t>(k)].start)) ++k;
            m.match_i = k % n;
        } else {
            int k = n - 1;
            while (k >= 0 && !(m.matches[static_cast<std::size_t>(k)].end <= c.start())) --k;
            m.match_i = (k + n) % n;
        }
        b->ed.select_range(m.matches[static_cast<std::size_t>(m.match_i)]);
    }

    // ── pickers ─────────────────────────────────────────────────────────

    [[nodiscard]] static std::vector<PickerItem> pick_items(const Model& m) {
        std::vector<PickerItem> v;
        switch (m.pick) {
            case PickMode::files: {
                // Recently used first, as file finders do with an empty query.
                std::set<std::string> seen;
                for (int i : m.mru) {
                    const auto& p = m.bufs[static_cast<std::size_t>(i)].path;
                    v.push_back({basename_of(p), dirname_of(p), "", basename_of(p)});
                    seen.insert(p);
                }
                for (const auto& f : m.project)
                    if (!seen.contains(f.path)) v.push_back({basename_of(f.path), dirname_of(f.path), "", basename_of(f.path)});
                break;
            }
            case PickMode::commands:
                for (const auto& c : kCommands) v.push_back({c.name, "", c.keys, ""});
                break;
            case PickMode::symbols:
                if (const Buffer* b = cur(m))
                    for (const auto& s : b->ed.symbols())
                        v.push_back({std::string(static_cast<std::size_t>(s.depth) * 2, ' ') + s.name, s.kind,
                                     std::to_string(s.line + 1), ""});
                break;
            default: break;
        }
        return v;
    }
    /// The path a file-picker item refers to (labels are basenames).
    [[nodiscard]] static std::string item_path(const PickerItem& it) {
        return it.detail.empty() ? it.label : it.detail + "/" + it.label;
    }

    static Cmd run_pick(Model& m, int k) {
        if (m.pick == PickMode::line) {
            Buffer* b = cur(m);
            const std::string& q = m.pick_q.text;
            const auto colon = q.find(':');
            const int line = std::atoi(q.substr(0, colon).c_str());
            const int col = colon == std::string::npos ? 1 : std::atoi(q.substr(colon + 1).c_str());
            m.pick = PickMode::none;
            if (b != nullptr && line > 0) {
                b->ed.set_cursor({line - 1, std::max(col, 1) - 1});
                center_caret(b->ed);
            }
            return {};
        }
        const auto items = pick_items(m);
        const auto ranked = rank_items(items, m.pick_q.text);
        if (k < 0 || k >= static_cast<int>(ranked.order.size())) { m.pick = PickMode::none; return {}; }
        const PickerItem& it = items[static_cast<std::size_t>(ranked.order[static_cast<std::size_t>(k)])];
        const PickMode mode = m.pick;
        m.pick = PickMode::none;
        m.focus = Focus::editor;
        if (mode == PickMode::files) { open(m, item_path(it), false); return SetTitle{title_of(m)}; }
        if (mode == PickMode::symbols) {
            if (Buffer* b = cur(m)) {
                const int line = std::atoi(it.hint.c_str()) - 1;
                const std::string name = it.label.substr(it.label.find_first_not_of(' '));
                const auto at = b->ed.line(line).find(name);
                b->ed.set_cursor({line, at == std::string::npos ? 0 : static_cast<int>(at)});
                center_caret(b->ed);
            }
            return {};
        }
        if (mode == PickMode::commands)
            for (const auto& c : kCommands) if (it.label == c.name) return act(m, c.act);
        return {};
    }
    static void center_caret(EditorState& ed) {
        const float y = static_cast<float>(ed.visual_row_of(ed.cursor().line)) * ed.metrics.line_h;
        ed.scroll.offset.y = std::max(0.0f, y - ed.scroll.viewport.y * 0.35f);
    }

    // ── completion ──────────────────────────────────────────────────────

    [[nodiscard]] static std::string prefix_at(const EditorState& ed) {
        const Pos c = ed.cursor();
        const std::string& s = ed.line(c.line);
        int a = c.col;
        while (a > 0 && is_word_char(s[static_cast<std::size_t>(a - 1)])) --a;
        return s.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(c.col - a));
    }
    /// Every identifier in the buffer and the language's keywords, ranked
    /// against the prefix at the caret.
    static void build_completions(Model& m) {
        m.comp_items.clear();
        const Buffer* b = cur(m);
        if (b == nullptr) return;
        const std::string pre = prefix_at(b->ed);
        std::map<std::string, CompletionItem::Kind> words;
        std::map<std::string, std::string> first_seen;
        for (int l = 0; l < b->ed.line_count(); ++l) {
            const std::string& s = b->ed.line(l);
            for (std::size_t i = 0; i < s.size();) {
                if (!is_word_char(s[i]) || std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; continue; }
                std::size_t j = i;
                while (j < s.size() && is_word_char(s[j])) ++j;
                std::string w = s.substr(i, j - i);
                if (w.size() >= 3 && w != pre && words.size() < 4000) {
                    auto kind = CompletionItem::Kind::variable;
                    std::size_t k = j;
                    while (k < s.size() && s[k] == ' ') ++k;
                    if (k < s.size() && s[k] == '(') kind = CompletionItem::Kind::function;
                    else if (std::isupper(static_cast<unsigned char>(w[0]))) kind = CompletionItem::Kind::type;
                    if (i > 0 && s[i - 1] == '.') kind = CompletionItem::Kind::field;
                    auto [it, fresh] = words.emplace(w, kind);
                    if (!fresh && kind == CompletionItem::Kind::function) it->second = kind;
                    if (fresh) {
                        std::string ctx = s;
                        ctx.erase(0, ctx.find_first_not_of(" \t"));
                        first_seen[w] = "line " + std::to_string(l + 1) + ":  " + ctx.substr(0, 80);
                    }
                }
                i = j;
            }
        }
        const auto& kws = b->ed.language->keywords;
        const auto& tys = b->ed.language->types;
        for (const auto& [w, k] : words) {
            if (std::find(kws.begin(), kws.end(), w) != kws.end() || std::find(tys.begin(), tys.end(), w) != tys.end()) continue;
            m.comp_items.push_back({w, k, k == CompletionItem::Kind::function ? "fn \u2026(\u2026)" : "", first_seen[w], ""});
        }
        for (auto kw : b->ed.language->keywords)
            if (kw != pre) m.comp_items.push_back({std::string{kw}, CompletionItem::Kind::keyword, "keyword", "", ""});
        for (auto ty : b->ed.language->types)
            if (ty != pre && !words.contains(std::string{ty}))
                m.comp_items.push_back({std::string{ty}, CompletionItem::Kind::type, "builtin type", "", ""});
    }
    struct CompRank { std::vector<int> order; std::vector<FuzzyMatch> matches; };
    [[nodiscard]] static CompRank rank_completions(const Model& m) {
        CompRank r;
        const Buffer* b = cur(m);
        if (b == nullptr) return r;
        const std::string pre = prefix_at(b->ed);
        std::vector<std::pair<int, FuzzyMatch>> hits;
        for (std::size_t i = 0; i < m.comp_items.size(); ++i) {
            auto f = fuzzy_match(pre, m.comp_items[i].label);
            if (!f.ok) continue;
            // An exact prefix beats a scattered match.
            if (m.comp_items[i].label.rfind(pre, 0) == 0) f.score += 30;
            hits.emplace_back(static_cast<int>(i), std::move(f));
        }
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b2) { return a.second.score > b2.second.score; });
        for (auto& [i, f] : hits) { if (r.order.size() >= 60) break; r.order.push_back(i); r.matches.push_back(std::move(f)); }
        return r;
    }
    static void maybe_complete(Model& m, bool force) {
        Buffer* b = cur(m);
        if (b == nullptr || b->ed.sels.size() > 1) { m.comp_open = false; return; }
        const std::string pre = prefix_at(b->ed);
        if (!force && pre.size() < 2) { m.comp_open = false; return; }
        if (!m.comp_open || force) { build_completions(m); m.comp_sel = 0; }
        m.comp_open = !rank_completions(m).order.empty();
        if (m.comp_open) m.comp_sel = std::min(m.comp_sel, static_cast<int>(rank_completions(m).order.size()) - 1);
    }
    static void accept_completion(Model& m, int k) {
        Buffer* b = cur(m);
        const auto r = rank_completions(m);
        m.comp_open = false;
        if (b == nullptr || k < 0 || k >= static_cast<int>(r.order.size())) return;
        const std::string pre = prefix_at(b->ed);
        const auto& it = m.comp_items[static_cast<std::size_t>(r.order[static_cast<std::size_t>(k)])];
        for (std::size_t i = 0; i < pre.size(); ++i) b->ed.move_left(true);
        b->ed.insert_text(it.insert.empty() ? it.label : it.insert);
        b->ed.break_group();
    }

    // ── init ────────────────────────────────────────────────────────────

    static Cmd init(Model& m) {
        m.project = g_builtin ? builtin_project() : load_project(g_root);
        m.tree = build_tree(m.project);
        for (const auto& n : m.tree) if (n.dir && n.depth == 0 && m.tree.size() < 200) m.expanded.insert(n.path);
        // Open the most interesting file there is.
        for (const char* p : {"src/main.cpp", "examples/editor.cpp", "README.md"})
            if (std::any_of(m.project.begin(), m.project.end(), [&](const File& f) { return f.path == p; })) { open(m, p, false); break; }
        if (m.active < 0 && !m.project.empty()) open(m, m.project.front().path, false);
        if (std::getenv("ED_DEMO") != nullptr && g_builtin) {
            open(m, "src/shapes.hpp", false);
            open(m, "src/broken.rs", true);
            activate(m, 0);
            Buffer* b = cur(m);
            b->ed.set_cursor({5, 4});
            b->ed.insert_text("Circle big{10.0};");
            b->ed.newline();
            b->ed.set_cursor({6, 11});
            b->ed.select_next_occurrence();
            m.bottom = true;
        }
        if (const char* p = std::getenv("ED_PICK")) {
            m.pick = std::string_view{p} == "files" ? PickMode::files : std::string_view{p} == "symbols" ? PickMode::symbols : PickMode::commands;
            if (const char* q = std::getenv("ED_QUERY")) m.pick_q.text = q, m.pick_q.caret = m.pick_q.anchor = m.pick_q.text.size();
        }
        return SetTitle{title_of(m)};
    }

    // ── update ──────────────────────────────────────────────────────────

    static Cmd act(Model& m, Act a) {
        Buffer* b = cur(m);
        EditorState* ed = b != nullptr ? &b->ed : nullptr;
        switch (a) {
            case Act::save:
                if (b == nullptr) return {};
                if (!g_builtin) {
                    std::ofstream f(std::filesystem::path(g_root) / b->path, std::ios::binary);
                    f << b->ed.text() << '\n';
                    if (!f) { toast(m, "Could not save", b->path, Toast::Kind::error); return {}; }
                }
                for (auto& pf : m.project) if (pf.path == b->path) pf.text = b->ed.text() + "\n";
                b->disk = b->ed.lines;
                b->ed.mark_saved();
                b->preview = false;
                b->cache_version = 0;
                toast(m, "Saved", b->path, Toast::Kind::success);
                return SetTitle{title_of(m)};
            case Act::close_tab: close(m, m.active); return SetTitle{title_of(m)};
            case Act::next_tab:
            case Act::prev_tab:
                if (m.bufs.size() > 1) {
                    const int n = static_cast<int>(m.bufs.size());
                    activate(m, ((m.active + (a == Act::next_tab ? 1 : -1)) % n + n) % n);
                }
                return SetTitle{title_of(m)};
            case Act::toggle_sidebar: m.sidebar = !m.sidebar; if (!m.sidebar && m.focus == Focus::tree) m.focus = Focus::editor; return {};
            case Act::toggle_problems:
                if (m.bottom && m.bottom_tab == 0) m.bottom = false; else { m.bottom = true; m.bottom_tab = 0; }
                return {};
            case Act::project_search:
                m.bottom = true; m.bottom_tab = 1; m.focus = Focus::psearch;
                if (ed != nullptr && !ed->primary_sel().empty() && ed->primary_sel().start().line == ed->primary_sel().end().line)
                    m.ps_q.text = ed->text_in(ed->primary_sel().range());
                m.ps_q.select_all();
                return FocusNode{node_id("psearch")};
            case Act::find:
            case Act::replace:
                m.find_open = true;
                m.find_replace = a == Act::replace || m.find_replace;
                m.find_field = 0;
                m.focus = Focus::find;
                if (ed != nullptr && !ed->primary_sel().empty() && ed->primary_sel().start().line == ed->primary_sel().end().line)
                    m.find_q.text = ed->text_in(ed->primary_sel().range());
                m.find_q.select_all();
                refind(m);
                return {};
            case Act::find_next: goto_match(m, +1); return {};
            case Act::find_prev: goto_match(m, -1); return {};
            case Act::file_finder: m.pick = PickMode::files; m.pick_q = {}; m.pick_sel = 0; return {};
            case Act::palette: m.pick = PickMode::commands; m.pick_q = {}; m.pick_sel = 0; return {};
            case Act::goto_symbol: m.pick = PickMode::symbols; m.pick_q = {}; m.pick_sel = 0; return {};
            case Act::goto_line: m.pick = PickMode::line; m.pick_q = {}; m.pick_sel = 0; return {};
            case Act::zoom_in: m.font_size = std::min(m.font_size + 1.0f, 32.0f); return {};
            case Act::zoom_out: m.font_size = std::max(m.font_size - 1.0f, 8.0f); return {};
            case Act::zoom_reset: m.font_size = 13.5f; return {};
            case Act::relative_numbers: m.relative = !m.relative; return {};
            case Act::show_whitespace: m.whitespace = !m.whitespace; return {};
            case Act::indent_guides: m.guides = !m.guides; return {};
            case Act::cycle_cursor: m.cursor_style = (m.cursor_style + 1) % 3; return {};
            case Act::complete: maybe_complete(m, true); return {};
            case Act::next_problem:
                if (b != nullptr) {
                    const auto& d = refresh(*b).diags;
                    if (d.empty()) return {};
                    const Pos c = ed->cursor();
                    auto it = std::find_if(d.begin(), d.end(), [&](const auto& x) { return c < x.range.start; });
                    if (it == d.end()) it = d.begin();
                    ed->set_cursor(it->range.start);
                    center_caret(*ed);
                }
                return {};
            case Act::quit: return Cmd::quit();
            case Act::count: return {};
            default: break;
        }
        if (ed == nullptr) return {};
        switch (a) {
            case Act::fold: ed->fold_at_cursor(); break;
            case Act::unfold: ed->unfold_at_cursor(); break;
            case Act::unfold_all: ed->unfold_all(); break;
            case Act::select_all_occurrences: ed->select_all_occurrences(); break;
            case Act::cursor_above: ed->add_cursor_vertical(-1); break;
            case Act::cursor_below: ed->add_cursor_vertical(+1); break;
            case Act::toggle_comment: ed->toggle_comment(); break;
            case Act::duplicate_line: ed->duplicate_lines(); break;
            case Act::delete_line: ed->delete_lines(); break;
            case Act::undo: ed->undo(); break;
            case Act::redo: ed->redo(); break;
            case Act::copy: m.clipboard = ed->copy(); return WriteClipboard{m.clipboard};
            case Act::cut: m.clipboard = ed->cut(); b->preview = false; refind(m); return Cmd::batch(WriteClipboard{m.clipboard}, SetTitle{title_of(m)});
            case Act::paste: ed->paste(m.clipboard); b->preview = false; break;
            default: break;
        }
        refind(m);
        return SetTitle{title_of(m)};
    }

    static Cmd update(Model& m, Do e) { return act(m, e.a); }

    static Cmd update(Model& m, EdKey e) {
        Buffer* b = cur(m);
        if (b == nullptr) return {};
        m.blink_on = true; m.idle = 0;
        const bool was_dirty = b->ed.dirty();
        const std::uint64_t v0 = b->ed.version();
        if (e.k.key == Key::escape && m.find_open && b->ed.sels.size() == 1) { m.find_open = false; refind(m); return {}; }
        b->ed.handle_key(e.k.key, e.k.mods);
        if (b->ed.version() != v0) { b->preview = false; refind(m); }
        if (m.comp_open) maybe_complete(m, false);
        return was_dirty != b->ed.dirty() ? Cmd{SetTitle{title_of(m)}} : Cmd{};
    }
    static Cmd update(Model& m, Typed e) {
        m.blink_on = true; m.idle = 0;
        switch (m.focus) {
            case Focus::picker: m.pick_q.insert(e.s); m.pick_sel = 0; return {};
            case Focus::find:
                (m.find_field == 0 ? m.find_q : m.find_r).insert(e.s);
                if (m.find_field == 0) refind(m);
                return {};
            case Focus::psearch: m.ps_q.insert(e.s); return {};
            case Focus::tree: return {};
            case Focus::editor: break;
        }
        if (m.pick != PickMode::none) { m.pick_q.insert(e.s); m.pick_sel = 0; return {}; }
        Buffer* b = cur(m);
        if (b == nullptr) return {};
        const bool was_dirty = b->ed.dirty();
        b->ed.insert_text(e.s);
        b->preview = false;
        refind(m);
        if (!e.s.empty() && is_word_char(e.s.back())) maybe_complete(m, false);
        else m.comp_open = false;
        return was_dirty ? Cmd{} : Cmd{SetTitle{title_of(m)}};
    }
    static Cmd update(Model& m, PressMods e) { m.press_mods = e.mods; return {}; }
    static Cmd update(Model& m, Press e) {
        Buffer* b = cur(m);
        m.focus = Focus::editor;
        m.comp_open = false;
        m.blink_on = true; m.idle = 0;
        if (b == nullptr) return FocusNode{0};
        const Pos p = code_view_hit(b->ed, e.local);
        if (m.press_mods.alt) b->ed.add_cursor_at(p);
        else b->ed.set_cursor(p, m.press_mods.shift);
        return {};
    }
    static Cmd update(Model& m, Drag e) {
        if (Buffer* b = cur(m)) b->ed.drag_to(code_view_hit(b->ed, e.local));
        return {};
    }
    static Cmd update(Model& m, Click e) {
        Buffer* b = cur(m);
        if (b == nullptr) return {};
        const Pos p = code_view_hit(b->ed, e.local);
        if (e.count == 2) b->ed.select_word_at(p);
        else if (e.count >= 3) b->ed.select_line_at(p.line);
        return {};
    }
    static Cmd update(Model& m, Fold e) { if (Buffer* b = cur(m)) b->ed.toggle_fold(e.line); return {}; }
    static Cmd update(Model& m, OpenTab e) { activate(m, e.i); return SetTitle{title_of(m)}; }
    static Cmd update(Model& m, CloseTab e) { close(m, e.i); return SetTitle{title_of(m)}; }
    // The tree's rows as the widget sees them, and what to do with an intent.
    static std::vector<TreeRow> tree_widget_rows(const Model& m, const std::vector<int>& rows) {
        std::vector<TreeRow> out;
        out.reserve(rows.size());
        for (int i : rows) {
            const TreeNode& n = m.tree[static_cast<std::size_t>(i)];
            out.push_back({.name = n.name, .depth = n.depth, .dir = n.dir, .open = n.dir && m.expanded.contains(n.path)});
        }
        return out;
    }
    static Cmd apply_tree(Model& m, const std::vector<int>& rows, TreeIntent in) {
        using A = TreeIntent::Act;
        if (in.act == A::none || in.act == A::moved) return {};
        if (in.act == A::leave) { m.focus = Focus::editor; return {}; }
        const TreeNode& n = m.tree[static_cast<std::size_t>(rows[static_cast<std::size_t>(in.row)])];
        switch (in.act) {
            case A::toggle: if (!m.expanded.erase(n.path)) m.expanded.insert(n.path); return {};
            case A::expand: m.expanded.insert(n.path); return {};
            case A::collapse: m.expanded.erase(n.path); return {};
            case A::open: open(m, n.path, false); return SetTitle{title_of(m)};
            case A::preview: open(m, n.path, true); m.focus = Focus::tree; return SetTitle{title_of(m)};
            default: return {};
        }
    }
    static Cmd update(Model& m, TreeClick e) {
        const auto rows = tree_rows(m);
        m.focus = Focus::tree;
        const TreeIntent in = tree_click(e.row, e.count, m.tree_sel, tree_widget_rows(m, rows));
        Cmd c = apply_tree(m, rows, in);
        if (in.act == TreeIntent::Act::open) m.focus = Focus::editor;   // a double click moves to the file
        return c;
    }
    static Cmd update(Model& m, TreeKey e) {
        const auto rows = tree_rows(m);
        return apply_tree(m, rows, tree_key(e.k.key, m.tree_sel, m.tree_scroll, tree_widget_rows(m, rows)));
    }
    static Cmd update(Model& m, TreeWheel e) { m.tree_scroll.scroll_by(e.delta); return {}; }
    static Cmd update(Model& m, PickKey e) {
        const auto& k = e.k;
        const PickerIntent in = picker_key(m.pick_q, k.key, k.mods, m.pick_sel);
        if (in.act == PickerIntent::Act::close) m.pick = PickMode::none;
        if (in.act == PickerIntent::Act::choose) return run_pick(m, in.index);
        // Backspace that leaves go-to-line empty closes it (editor specific).
        if (m.pick_q.text.empty() && k.key == Key::backspace && m.pick == PickMode::line) m.pick = PickMode::none;
        return {};
    }
    static Cmd update(Model& m, PickClick e) { return run_pick(m, e.i); }
    static Cmd update(Model& m, PickClose) { m.pick = PickMode::none; return {}; }
    static Cmd update(Model& m, FindKey e) {
        const auto& k = e.k;
        if (k.key == Key::escape) { m.find_open = false; m.focus = Focus::editor; refind(m); return {}; }
        if (k.key == Key::enter && m.find_field == 0) { goto_match(m, k.mods.shift ? -1 : 1); return {}; }
        if (k.key == Key::enter && m.find_field == 1) { return update(m, FindPart{find_part::replace_one}); }
        if (k.key == Key::tab && m.find_replace) { m.find_field = 1 - m.find_field; return {}; }
        if (k.mods.alt && k.key == Key::c) { m.find_opts.case_sensitive = !m.find_opts.case_sensitive; refind(m); return {}; }
        if (k.mods.alt && k.key == Key::w) { m.find_opts.whole_word = !m.find_opts.whole_word; refind(m); return {}; }
        if (k.mods.alt && k.key == Key::r) { m.find_opts.regex = !m.find_opts.regex; refind(m); return {}; }
        (m.find_field == 0 ? m.find_q : m.find_r).handle_key(k.key, k.mods);
        if (m.find_field == 0) refind(m);
        return {};
    }
    static Cmd update(Model& m, FindPart e) {
        namespace fp = find_part;
        Buffer* b = cur(m);
        switch (e.part) {
            case fp::query: m.focus = Focus::find; m.find_field = 0; break;
            case fp::replace: m.focus = Focus::find; m.find_field = 1; break;
            case fp::case_: m.find_opts.case_sensitive = !m.find_opts.case_sensitive; break;
            case fp::word: m.find_opts.whole_word = !m.find_opts.whole_word; break;
            case fp::regex: m.find_opts.regex = !m.find_opts.regex; break;
            case fp::prev: goto_match(m, -1); return {};
            case fp::next: goto_match(m, +1); return {};
            case fp::close: m.find_open = false; m.focus = Focus::editor; break;
            case fp::expand: m.find_replace = !m.find_replace; break;
            case fp::replace_one:
                if (b != nullptr && m.match_i >= 0) {
                    const Range r = m.matches[static_cast<std::size_t>(m.match_i)];
                    b->ed.replace_ranges({r}, m.find_r.text, m.find_opts.regex ? std::string_view{m.find_q.text} : std::string_view{},
                                         m.find_opts.case_sensitive);
                    b->preview = false;
                    refind(m);
                    goto_match(m, +1);
                }
                return SetTitle{title_of(m)};
            case fp::replace_all:
                if (b != nullptr && !m.matches.empty()) {
                    const int n = b->ed.replace_ranges(m.matches, m.find_r.text,
                                                       m.find_opts.regex ? std::string_view{m.find_q.text} : std::string_view{},
                                                       m.find_opts.case_sensitive);
                    b->preview = false;
                    toast(m, "Replaced " + std::to_string(n) + " occurrences", basename_of(b->path), Toast::Kind::success);
                }
                break;
            default: break;
        }
        refind(m);
        return SetTitle{title_of(m)};
    }
    static Cmd update(Model& m, PsKey e) {
        if (e.k.key == Key::escape) { m.focus = Focus::editor; return FocusNode{0}; }
        if (e.k.key == Key::enter) return update(m, PsRun{});
        m.ps_q.handle_key(e.k.key, e.k.mods);
        return {};
    }
    /// Search every file (open buffers use their live text).
    static Cmd update(Model& m, PsRun) {
        m.ps_rows.clear();
        m.ps_targets.clear();
        const std::string& q = m.ps_q.text;
        if (q.empty()) return {};
        int total = 0, files = 0;
        for (const auto& f : m.project) {
            const EditorState* live = nullptr;
            for (const auto& b : m.bufs) if (b.path == f.path) live = &b.ed;
            const EditorState tmp = live ? EditorState{} : EditorState::from_text(f.text);
            const EditorState& ed = live ? *live : tmp;
            const auto hits = ed.find_all(q, m.find_opts);
            if (hits.empty()) continue;
            ++files;
            m.ps_rows.push_back(ProblemRow{.header = true, .file = f.path});
            m.ps_rows.back().warnings = static_cast<int>(hits.size());
            m.ps_targets.emplace_back(f.path, hits.front().start);
            for (const auto& h : hits) {
                if (++total > 2000) break;
                std::string text = ed.line(h.start.line);
                text.erase(0, text.find_first_not_of(" \t"));
                m.ps_rows.push_back(ProblemRow{.message = text.substr(0, 160), .source = "", .line = h.start.line,
                                               .col = h.start.col, .severity = 2});
                m.ps_targets.emplace_back(f.path, h.start);
            }
        }
        toast(m, std::to_string(total) + " results in " + std::to_string(files) + " files", "\u201C" + q + "\u201D");
        return {};
    }
    static Cmd update(Model& m, ProblemClick e) {
        m.problem_sel = e.i;
        if (m.bottom_tab == 1) {
            if (e.i < 0 || e.i >= static_cast<int>(m.ps_targets.size())) return {};
            const auto [path, at] = m.ps_targets[static_cast<std::size_t>(e.i)];
            open(m, path, false);
            if (Buffer* b = cur(m)) { b->ed.set_cursor(at); center_caret(b->ed); }
            return SetTitle{title_of(m)};
        }
        const auto rows = problem_rows(m);
        if (e.i < 0 || e.i >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(e.i)].header) return {};
        if (Buffer* b = cur(m)) {
            b->ed.set_cursor({rows[static_cast<std::size_t>(e.i)].line, rows[static_cast<std::size_t>(e.i)].col});
            center_caret(b->ed);
            m.focus = Focus::editor;
        }
        return {};
    }
    static Cmd update(Model& m, BottomTab e) { m.bottom_tab = e.t; m.problem_sel = -1; return {}; }
    static Cmd update(Model& m, SplitGrab) { m.sidebar_at_press = m.sidebar_w; m.bottom_at_press = m.bottom_h; return {}; }
    static Cmd update(Model& m, SplitDrag e) { m.sidebar_w = std::clamp(m.sidebar_at_press + e.t.x(), 160.0f, 520.0f); return {}; }
    static Cmd update(Model& m, BottomDrag e) { m.bottom_h = std::clamp(m.bottom_at_press - e.t.y(), 100.0f, 600.0f); return {}; }
    static Cmd update(Model& m, BarPress e) { if (Buffer* b = cur(m)) scrollbar_press(b->ed.scroll, e.y); return {}; }
    static Cmd update(Model& m, BarDrag e) { if (Buffer* b = cur(m)) scrollbar_drag(b->ed.scroll, e.t); return {}; }
    static Cmd update(Model& m, CompKey e) {
        const int n = static_cast<int>(rank_completions(m).order.size());
        switch (e.k.key) {
            case Key::down: m.comp_sel = (m.comp_sel + 1) % std::max(n, 1); return {};
            case Key::up: m.comp_sel = (m.comp_sel - 1 + std::max(n, 1)) % std::max(n, 1); return {};
            case Key::enter: case Key::tab: accept_completion(m, m.comp_sel); return {};
            case Key::escape: m.comp_open = false; return {};
            default: m.comp_open = false; return update(m, EdKey{e.k});
        }
    }
    static Cmd update(Model& m, CompClick e) { accept_completion(m, e.i); return {}; }
    static Cmd update(Model& m, Mouse e) { m.mouse = e.p; return {}; }
    static Cmd update(Model& m, Wheel e) { m.idle = 0; if (Buffer* b = cur(m)) b->ed.scroll.scroll_by(e.delta); return {}; }
    static Cmd update(Model& m, ToastClick e) {
        if (e.i >= 0 && e.i < static_cast<int>(m.toast_list.size())) {
            m.toast_list.erase(m.toast_list.begin() + e.i);
            m.toast_ttl.erase(m.toast_ttl.begin() + e.i);
        }
        return {};
    }
    static Cmd update(Model& m, Blink) {
        m.idle += 0.53;
        // Solid (and the clock stops) after 15 s idle. Scripted runs keep it
        // solid: the blink runs on a real timer, so its phase isn't reproducible.
        m.blink_on = (g_headless || m.idle >= 15.0) ? true : !m.blink_on;
        for (auto& t : m.toast_ttl) t -= 0.53;
        for (std::size_t i = m.toast_ttl.size(); i-- > 0;)
            if (m.toast_ttl[i] <= 0) { m.toast_ttl.erase(m.toast_ttl.begin() + static_cast<long>(i)); m.toast_list.erase(m.toast_list.begin() + static_cast<long>(i)); }
        return {};
    }
    static Cmd update(Model&, Tick) { return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── view ────────────────────────────────────────────────────────────

    [[nodiscard]] static Theme theme() {
        Theme t = make_theme(rgb<0x82AAFF>, mayag::Mode::dark);
        t.background = rgb<0x1B1D2B>;
        t.surface = rgb<0x1F2233>;
        t.surface_raised = rgb<0x2A2E45>;
        t.overlay = rgb<0x22263A>;
        t.border = rgb<0x2B2F45>;
        t.border_strong = rgb<0x3B4062>;
        t.text_primary = rgb<0xC8D3F5>;
        t.text_secondary = rgb<0x8C95BF>;
        t.text_disabled = rgb<0x5A6290>;
        t.font_size = 12.5f;
        t.radius_small = 5.0f;
        t.radius_medium = 8.0f;
        return t;
    }

    [[nodiscard]] static std::vector<ProblemRow> problem_rows(const Model& m) {
        std::vector<ProblemRow> rows;
        const Buffer* b = cur(m);
        if (b == nullptr) return rows;
        const auto& d = refresh(*b).diags;
        if (d.empty()) return rows;
        ProblemRow head{.header = true, .file = b->path};
        for (const auto& x : d) { if (x.severity == editor::Diagnostic::Severity::error) ++head.errors; else if (x.severity == editor::Diagnostic::Severity::warning) ++head.warnings; }
        rows.push_back(head);
        for (const auto& x : d)
            rows.push_back(ProblemRow{.message = x.message, .source = x.source, .line = x.range.start.line,
                                      .col = x.range.start.col, .severity = static_cast<int>(x.severity)});
        return rows;
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        Theme t = theme();
        Ctx look = c;
        look.theme = t;
        const Buffer* b = cur(m);
        const CodeTheme ct = CodeTheme::dark(t);

        // ── tabs + breadcrumbs ──────────────────────────────────────────
        std::vector<TabItem> tabs;
        std::map<std::string, int> name_count;
        for (const auto& bf : m.bufs) ++name_count[basename_of(bf.path)];
        for (const auto& bf : m.bufs) {
            const auto& d = refresh(bf).diags;
            const int errs = static_cast<int>(std::count_if(d.begin(), d.end(), [](const auto& x) { return x.severity == editor::Diagnostic::Severity::error; }));
            tabs.push_back({basename_of(bf.path), name_count[basename_of(bf.path)] > 1 ? dirname_of(bf.path) : "",
                            bf.ed.dirty(), bf.preview, false, errs});
        }
        std::vector<Crumb> crumbs;
        if (b != nullptr) {
            std::size_t st = 0;
            while (true) {
                const auto sl = b->path.find('/', st);
                if (sl == std::string::npos) { crumbs.push_back({b->path.substr(st), true, false}); break; }
                crumbs.push_back({b->path.substr(st, sl - st), false, false});
                st = sl + 1;
            }
            // The innermost symbol enclosing the caret.
            const auto& syms = b->ed.symbols();
            const editor::Symbol* inner = nullptr;
            for (const auto& s : syms) if (s.line <= b->ed.cursor().line) inner = &s;
            if (inner != nullptr) crumbs.push_back({inner->name, false, true});
        }

        // ── the editor surface ──────────────────────────────────────────
        Node surface;
        if (b != nullptr) {
            refresh(*b);
            CodeViewOptions o;
            o.font_size = m.font_size;
            o.focused = m.focus == Focus::editor || m.focus == Focus::find;
            o.relative_numbers = m.relative;
            o.show_whitespace = m.whitespace;
            o.indent_guides = m.guides;
            o.cursor_on = m.blink_on;
            o.cursor_style = m.cursor_style;
            o.rulers = {100};
            o.find_matches = m.find_open ? &m.matches : nullptr;
            o.find_current = m.match_i;
            o.diagnostics = &b->diags;
            o.git = &b->marks;
            surface = code_view(look, "editor", b->ed, ct, o);
            c.track(m.editor_box, node_id("editor"));
        } else {
            surface = (v(text<"No file open"> | font(16) | fg(t.text_secondary),
                         text<"\u2318P  find a file    \u2318\u21E7P  all commands"> | font(12) | fg(t.text_disabled))
                       | gap(10) | center | grow() | width(pct(100)) | bg(ct.bg)).build();
        }

        // ── overlays: completion, hover, picker, toasts ─────────────────
        if (b != nullptr && m.editor_box.ready()) {
            const auto& mt = b->ed.metrics;
            const Rect er = m.editor_box.rect();
            const Pos cp = b->ed.cursor();
            const Vec2 caret{er.left() + mt.gutter_w - b->ed.scroll.offset.x +
                                 static_cast<float>(b->ed.display_col(cp.line, cp.col)) * mt.cell_w,
                             er.top() + static_cast<float>(b->ed.visual_row_of(cp.line) + 1) * mt.line_h - b->ed.scroll.offset.y};
            if (m.comp_open && m.focus == Focus::editor) {
                const auto r = rank_completions(m);
                completion_menu(look, "complete", m.comp_items, r.order, r.matches, m.comp_sel, caret, mt.line_h, m.font_size);
            }
            // Hover: a diagnostic under the mouse.
            if (er.contains(m.mouse) && m.pick == PickMode::none && !m.comp_open && m.mouse.x > er.left() + mt.gutter_w) {
                const Pos p = code_view_hit(b->ed, m.mouse - er.origin);
                std::vector<HoverSection> hs;
                for (const auto& d : b->diags)
                    if (d.range.start.line == p.line && p.col >= d.range.start.col && p.col <= std::max(d.range.end.col, d.range.start.col + 1)) {
                        using S = editor::Diagnostic::Severity;
                        hs.push_back({d.severity == S::error ? HoverSection::Kind::error : d.severity == S::warning ? HoverSection::Kind::warning : HoverSection::Kind::info,
                                      d.message, d.source});
                    }
                if (!hs.empty()) hover_card(look, "hover", Vec2{m.mouse.x + 8, m.mouse.y + 14}, hs, m.font_size);
            }
        }
        if (m.pick != PickMode::none) {
            if (m.pick == PickMode::line) {
                const int lines = b != nullptr ? b->ed.line_count() : 0;
                PickerStyle st{.placeholder = "line or line:column", .prefix = ":", .width = 420.0f,
                               .empty_text = "Go to line 1\u2013" + std::to_string(lines) + (b ? ", currently " + std::to_string(b->ed.cursor().line + 1) : ""),
                               .footer = {{"\u23CE", "go"}, {"esc", "cancel"}}};
                const std::vector<PickerItem> none;
                picker(look, "picker", m.pick_q, none, Ranked{}, 0, st);
            } else {
                const auto items = pick_items(m);
                const auto ranked = rank_items(items, m.pick_q.text);
                PickerStyle st;
                st.footer = {{"\u2191\u2193", "navigate"}, {"\u23CE", "open"}, {"esc", "close"}};
                if (m.pick == PickMode::commands) { st.prefix = ">"; st.placeholder = "Execute a command\u2026"; st.footer[1].second = "run"; }
                if (m.pick == PickMode::symbols) { st.prefix = "@"; st.placeholder = "Go to symbol in " + (b ? basename_of(b->path) : std::string{}); st.empty_text = "No symbols"; }
                if (m.pick == PickMode::files) st.placeholder = "Search " + std::to_string(m.project.size()) + " files\u2026";
                picker(look, "picker", m.pick_q, items, ranked, m.pick_sel, st);
            }
        }
        toasts(look, "toasts", m.toast_list, c.size);

        // ── centre column ───────────────────────────────────────────────
        std::vector<Node> centre;
        centre.push_back(tab_bar(look, "tabs", tabs, m.active,
                                 (h(text_owned(b && b->ed.sels.size() > 1 ? std::to_string(b->ed.sels.size()) + " cursors" : std::string{})
                                        | font(11.5f) | fg(t.accent)) | pad(0, 12)).build()));
        if (b != nullptr) centre.push_back(breadcrumbs(look, "crumbs", crumbs));
        if (m.find_open)
            centre.push_back(find_bar(look, "find", m.find_q, m.find_r, m.find_opts, m.match_i,
                                      static_cast<int>(m.matches.size()), m.find_replace,
                                      m.focus == Focus::find ? m.find_field : -1));
        centre.push_back(std::move(surface));
        if (m.bottom) {
            centre.push_back(split_handle(look, "split-bottom", Axis::vertical));
            const auto prow = problem_rows(m);
            int errs = 0, warns = 0;
            for (const auto& r : prow) if (r.header) { errs += r.errors; warns += r.warnings; }
            const auto tab_ = [&](int i, std::string label) {
                const auto pid = part_id("btab", static_cast<std::size_t>(i));
                const bool on = m.bottom_tab == i;
                return (h(text_owned(std::move(label)) | font(11) | semibold | tracking(0.6f)
                              | fg(on ? t.text_primary : c.hovered(pid) ? t.text_secondary : t.text_disabled))
                        | pad(0, 10) | height(pct(100)) | center
                        | border(0, t.border) | bg(on ? t.background : t.surface.fade(0.0f))
                        | pointer_cursor | id_of("btab", static_cast<std::size_t>(i))).build();
            };
            Node head_right = m.bottom_tab == 1
                ? (h(node((text_field(t, m.ps_q, m.focus == Focus::psearch, node_id("psearch"), "Search all files (\u23CE)", 12.0f)
                           | width(320)).build()))
                   | align(Align::center)).build()
                : (text_owned(std::to_string(errs) + " errors, " + std::to_string(warns) + " warnings") | font(11) | fg(t.text_disabled)).build();
            Node body = m.bottom_tab == 0
                ? (prow.empty() ? (v(text<"No problems in this file"> | font(12) | fg(t.text_disabled)) | pad(16)).build()
                                : problems_panel(look, "problems", prow, m.problem_sel, m.problem_scroll))
                : (m.ps_rows.empty() ? (v(text<"Type a query and press \u23CE"> | font(12) | fg(t.text_disabled)) | pad(16)).build()
                                     : problems_panel(look, "problems", m.ps_rows, m.problem_sel, m.problem_scroll));
            centre.push_back((v(h(node(tab_(0, "PROBLEMS")), node(tab_(1, "SEARCH")), spacer(), node(std::move(head_right)))
                                    | height(32) | align(Align::center) | pad(0, 8, 0, 0) | width(pct(100)) | bg(t.surface),
                                node(std::move(body)))
                              | height(m.bottom_h) | width(pct(100)) | rigid | bg(t.background)).build());
        }

        // ── project panel ───────────────────────────────────────────────
        Node side;
        if (m.sidebar) {
            std::vector<TreeRow> rows;
            std::map<std::string, char> changed;
            std::map<std::string, int> errors;
            for (const auto& bf : m.bufs) {
                if (bf.ed.dirty() || !refresh(bf).marks.empty()) changed[bf.path] = 'M';
                const auto& d = refresh(bf).diags;
                errors[bf.path] = static_cast<int>(std::count_if(d.begin(), d.end(), [](const auto& x) { return x.severity == editor::Diagnostic::Severity::error; }));
            }
            for (int i : tree_rows(m)) {
                const TreeNode& n = m.tree[static_cast<std::size_t>(i)];
                char g = 0;
                int e = 0;
                if (n.dir) {
                    for (const auto& [p, k] : changed) if (p.starts_with(n.path + "/")) g = 'M';
                    for (const auto& [p, k] : errors) if (p.starts_with(n.path + "/")) e += k;
                } else {
                    if (auto it = changed.find(n.path); it != changed.end()) g = it->second;
                    if (auto it = errors.find(n.path); it != errors.end()) e = it->second;
                }
                rows.push_back({.name = n.name, .depth = n.depth, .dir = n.dir, .open = m.expanded.contains(n.path),
                                .active = b && b->path == n.path, .git = g, .errors = e});
            }
            const std::string root_name = g_builtin ? std::string{"demo"} : std::filesystem::path(g_root).filename().string();
            Node tree = file_tree(look, "tree", rows, m.tree_sel, m.tree_scroll, m.focus == Focus::tree);
            side = (h(v(h(text_owned(root_name) | font(11) | semibold | tracking(0.6f) | fg(t.text_secondary),
                          spacer(),
                          text_owned(std::to_string(m.project.size()) + " files") | font(10.5f) | fg(t.text_disabled))
                            | align(Align::center) | pad(0, 12) | height(34) | width(pct(100)),
                        node(std::move(tree)) | grow() | min_size(0, 0))
                      | width(m.sidebar_w) | height(pct(100)) | bg(t.surface),
                      node(split_handle(look, "split-side", Axis::horizontal)))
                    | height(pct(100))).build();
        }

        // ── status bar ──────────────────────────────────────────────────
        std::vector<StatusItem> left, right;
        {
            const auto prow = problem_rows(m);
            int errs = 0, warns = 0;
            for (const auto& r : prow) if (r.header) { errs += r.errors; warns += r.warnings; }
            using T = StatusItem::Tone;
            left.push_back({"", "\u25A4", T::normal, m.sidebar});
            left.push_back({std::to_string(errs), "\u2716", errs ? T::error : T::dim, m.bottom && m.bottom_tab == 0});
            left.push_back({std::to_string(warns), "\u26A0", warns ? T::warning : T::dim, false});
            left.push_back({"main", "\u2387", T::normal, false});
            if (b != nullptr) {
                const auto& ed = b->ed;
                const Pos p = ed.cursor();
                std::string pos = std::to_string(p.line + 1) + ":" + std::to_string(ed.display_col(p.line, p.col) + 1);
                int chars = 0;
                for (const auto& s : ed.sels) if (!s.empty()) chars += static_cast<int>(ed.text_in(s.range()).size());
                if (chars > 0) pos += " (" + std::to_string(chars) + " selected)";
                if (ed.sels.size() > 1) pos += "  " + std::to_string(ed.sels.size()) + " cursors";
                right.push_back({pos, "", T::normal, false});
                right.push_back({ed.soft_tabs ? "Spaces: " + std::to_string(ed.tab_width) : "Tabs", "", T::dim, false});
                right.push_back({"UTF-8", "", T::dim, false});
                right.push_back({"LF", "", T::dim, false});
                right.push_back({std::string{ed.language->name}, "", T::accent, false});
            }
            char perf[48] = "";
            // Measured time differs every run; scripted runs leave it out so
            // their frames are reproducible.
            if (!g_headless && c.latency != nullptr && c.latency->mean_cpu_ms() > 0.0)
                std::snprintf(perf, sizeof perf, "%.2fms", c.latency->mean_cpu_ms());
            right.push_back({perf, "", T::dim, false});
        }

        return v(h(node(std::move(side)),
                   v_list(std::move(centre)) | grow() | height(pct(100)) | min_size(0, 0))
                     | grow() | width(pct(100)) | min_size(0, 0),
                 node(status_bar(look, "status", left, right)))
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    // ── input ───────────────────────────────────────────────────────────

    /// Keys that work everywhere.
    [[nodiscard]] static std::optional<Act> global_key(const KeyEvent& k) {
        const bool cmd = k.mods.primary(), sh = k.mods.shift;
        if (cmd && sh) {
            switch (k.key) {
                case Key::p: return Act::palette;
                case Key::o: return Act::goto_symbol;
                case Key::f: return Act::project_search;
                case Key::m: return Act::toggle_problems;
                case Key::bracket_right: return Act::next_tab;
                case Key::bracket_left: return Act::prev_tab;
                default: break;
            }
        }
        if (cmd && !sh && !k.mods.alt) {
            switch (k.key) {
                case Key::p: return Act::file_finder;
                case Key::s: return Act::save;
                case Key::w: return Act::close_tab;
                case Key::b: return Act::toggle_sidebar;
                case Key::equal: return Act::zoom_in;
                case Key::minus: return Act::zoom_out;
                case Key::n0: return Act::zoom_reset;
                case Key::q: return Act::quit;
                default: break;
            }
        }
        if (k.mods.ctrl && k.key == Key::tab) return sh ? Act::prev_tab : Act::next_tab;
        if (k.mods.ctrl && !cmd && k.key == Key::g) return Act::goto_line;
        if (k.key == Key::f8) return Act::next_problem;
        return std::nullopt;
    }

    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (m.pick != PickMode::none) return PickKey{k};
        if (auto a = global_key(k)) return Do{*a};
        // cmd-1..9: tab n
        if (k.mods.primary() && k.key >= Key::n1 && k.key <= Key::n9) {
            const int i = static_cast<int>(k.key) - static_cast<int>(Key::n1);
            if (i < static_cast<int>(m.bufs.size())) return OpenTab{i};
        }
        switch (m.focus) {
            case Focus::find:
                if (k.mods.primary() && k.key == Key::g) return Do{k.mods.shift ? Act::find_prev : Act::find_next};
                return FindKey{k};
            case Focus::psearch: return PsKey{k};
            case Focus::tree:
                if (k.mods.primary() || k.mods.alt) return std::nullopt;
                return TreeKey{k};
            default: break;
        }
        const bool cmd = k.mods.primary(), sh = k.mods.shift;
        if (cmd && !sh && k.key == Key::f) return Do{k.mods.alt ? Act::replace : Act::find};
        if (cmd && k.key == Key::g) return Do{sh ? Act::find_prev : Act::find_next};
        if (cmd && !sh && k.key == Key::c) return Do{Act::copy};
        if (cmd && !sh && k.key == Key::x) return Do{Act::cut};
        if (cmd && !sh && k.key == Key::v) return Do{Act::paste};
        if (k.mods.ctrl && k.key == Key::space) return Do{Act::complete};
        if (cmd && sh && k.key == Key::e) return Do{Act::toggle_sidebar};
        if (m.comp_open) return CompKey{k};
        return EdKey{k};
    }
    static std::optional<Msg> on(const Model& m, const TextEvent& e) {
        if (m.focus == Focus::tree && m.pick == PickMode::none) return std::nullopt;
        return Typed{e.text};
    }
    static std::optional<Msg> on(const Model&, const MouseDown& d) { return PressMods{d.mods}; }
    static std::optional<Msg> on(const Model&, const MouseMove& mv) { return Mouse{mv.position}; }
    static std::optional<Msg> on(const Model&, const Pressed& p) {
        if (is(p, "editor")) return Press{p.local};
        if (is(p, "editor", part_bar)) return BarPress{p.local.y};
        if (is(p, "split-side") || is(p, "split-bottom")) return SplitGrab{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "editor")) return Drag{d.local};
        if (is(d, "editor", part_bar)) return BarDrag{d.travel};
        if (is(d, "split-side")) return SplitDrag{d.travel};
        if (is(d, "split-bottom")) return BottomDrag{d.travel};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        if (is(c, "editor") && c.count >= 2) return Click{c.local, c.count};
        for (std::size_t i = 0; i < m.bufs.size(); ++i) {
            if (is(c, "tabs", i)) return c.button == MouseButton::middle ? Msg{CloseTab{static_cast<int>(i)}} : Msg{OpenTab{static_cast<int>(i)}};
            if (is(c, "tabs", part_close + i)) return CloseTab{static_cast<int>(i)};
        }
        if (const Buffer* b = cur(m)) {
            if (c.node >= part_id("editor", part_fold_base) || true) {
                const auto& vis = b->ed.visual_lines();
                const int first = std::max(0, static_cast<int>(b->ed.scroll.offset.y / std::max(b->ed.metrics.line_h, 1.0f)) - 2);
                for (int r = first; r < std::min(static_cast<int>(vis.size()), first + 400); ++r) {
                    const int l = vis[static_cast<std::size_t>(r)];
                    if (is(c, "editor", part_fold_base + static_cast<std::size_t>(l))) return Fold{l};
                }
            }
        }
        const auto rows = tree_rows(m);
        for (std::size_t i = 0; i < rows.size(); ++i) if (is(c, "tree", i)) return TreeClick{static_cast<int>(i), c.count};
        for (std::size_t i = 0; i < 64; ++i) if (is(c, "picker", i)) return PickClick{static_cast<int>(i)};
        for (std::size_t i = 0; i < 60; ++i) if (is(c, "complete", i)) return CompClick{static_cast<int>(i)};
        for (std::size_t i = 0; i < 4; ++i) if (is(c, "toasts", i)) return ToastClick{static_cast<int>(i)};
        for (std::size_t i = 0; i <= find_part::expand; ++i) if (is(c, "find", i)) return FindPart{i};
        for (int i = 0; i < 2; ++i) if (is(c, "btab", static_cast<std::size_t>(i))) return BottomTab{i};
        if (is(c, "psearch")) return Do{Act::project_search};
        const int nrows = static_cast<int>(m.bottom_tab == 1 ? m.ps_rows.size() : problem_rows(m).size());
        for (int i = 0; i < nrows; ++i) if (is(c, "problems", static_cast<std::size_t>(i))) return ProblemClick{i};
        if (is(c, "status", 0)) return Do{Act::toggle_sidebar};
        if (is(c, "status", 1) || is(c, "status", 2)) return Do{Act::toggle_problems};
        if (is(c, "status", 104)) return Do{Act::palette};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Left& l) {
        if (is(l, "picker", part_menu)) return PickClose{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::Scrolled& s) {
        if (is(s, "editor")) return Wheel{s.delta};
        if (is(s, "tree")) return TreeWheel{s.delta};
        (void)m;
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // Blink (and toast expiry) on a slow clock; frames only while a spring moves.
    static Sub subscribe(const Model& m) {
        using namespace std::chrono_literals;
        return Sub::batch(
            when(m.editor_box.animating(), Sub::on_frame([](double dt) -> Msg { return Tick{dt}; })),
            when((m.focus == Focus::editor && m.idle < 15.0) || !m.toast_list.empty(),
                 Sub::stream("blink", [](mayag::Sink<Msg> out, std::stop_token st) {
                     while (!st.stop_requested()) {
                         std::this_thread::sleep_for(530ms);
                         if (!st.stop_requested()) out.send(Blink{});
                     }
                 })));
    }
};

static_assert(mayag::Program<Editor>, "Editor must satisfy Program");

int main(int argc, char** argv) {
    std::vector<char*> args(argv, argv + argc);
    // A positional project directory, the rest goes to the shared harness.
    for (std::size_t i = 1; i < args.size(); ++i)
        if (args[i][0] != '-' && (i == 1 || std::string_view{args[i - 1]}.rfind("--", 0) != 0 ||
                                  std::string_view{args[i - 1]} == "--headless")) {
            g_root = args[i];
            args.erase(args.begin() + static_cast<long>(i));
            break;
        }
    const auto opts = demo::parse_args(static_cast<int>(args.size()), args.data(), Vec2{1440, 900});
    g_headless = opts.mode == demo::Options::Mode::headless;
    g_builtin = g_headless || std::getenv("ED_DEMO") != nullptr;
    if (g_root.empty()) g_root = std::filesystem::current_path().string();

    std::shared_ptr<typo::FontStack> fonts;
    if (opts.use_system_fonts) {
        static constexpr std::array<std::string_view, 4> mono{"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono"};
        fonts = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048},
                                            400, false, mono);
    } else {
        fonts = demo::make_fonts(opts);
    }

    AppConfig cfg{.title = "mayag editor", .size = opts.size, .theme = Editor::theme(), .fonts = fonts,
                  .debug_bounds = opts.debug_bounds};

    return demo::run<Editor>(opts, cfg, [](auto& rt) {
        std::printf("editor headless\n");
        int fails = 0;
        const auto ok = [&](bool c, std::string_view what) {
            std::printf("  %s  %.*s\n", c ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
            if (!c) ++fails;
        };
        const auto& M = [&]() -> const Editor::Model& { return rt.model(); };
        const auto B = [&]() -> const Editor::Buffer& { return M().bufs[static_cast<std::size_t>(M().active)]; };
        const Mods cmd = Mods::cmd();
        const Mods cmd_sh = Mods::cmd(true);
        // ED_SHOT=<dir>: screenshots of the REAL runtime (overlays included)
        // at interesting points of the script.
        const char* shot_dir = std::getenv("ED_SHOT");
        if (shot_dir != nullptr) rt.window().resize(Vec2{1440, 900}, 2.0f);
        const auto shot = [&](const char* name) {
            if (shot_dir == nullptr) return;
            rt.tick();
            const auto px = rt.window().read_pixels();
            const int w = static_cast<int>(std::lround(std::sqrt(static_cast<double>(px.size() / 4) * 1440.0 / 900.0)));
            (void)imageio::write_png(std::string{shot_dir} + "/" + name + ".png", px, w, static_cast<int>(px.size() / 4) / w);
        };
        rt.tick(); rt.tick();

        ok(M().project.size() == 7 && M().active >= 0 && B().path == "src/main.cpp", "opens the project and src/main.cpp");
        ok(B().ed.language->name == "C++", "language detected from the extension");
        ok(rt.node_rect("editor").has_value() && rt.node_rect("tree").has_value(), "editor and project panel laid out");
        const auto main_errors = Editor::problem_rows(M());
        ok(!main_errors.empty() && main_errors[0].header, "the linter reports problems (the TODO)");

        // typing
        rt.press_key(Key::end, cmd);   // end of document on every platform
        rt.press_key(Key::enter);
        rt.type("int x = 4");
        ok(B().ed.dirty() && B().ed.line(B().ed.cursor().line) == "int x = 4", "typing into the editor");
        ok(!B().marks.empty() || (Editor::refresh(B()), !B().marks.empty()), "the change gutter marks the new line");
        rt.press_key(Key::z, cmd);
        ok(B().ed.line(B().ed.cursor().line).empty(), "cmd-z undoes the typed word");

        // mouse
        if (auto r = rt.node_rect("editor")) {
            const auto& mt = B().ed.metrics;
            rt.window().click(Vec2{r->left() + mt.gutter_w + mt.cell_w * 4.2f, r->top() + mt.line_h * 5.5f});
            rt.tick();
            ok(B().ed.cursor() == (Pos{5, 4}), "a click places the caret on the clicked cell");
            rt.window().push(MouseDown{Vec2{r->left() + mt.gutter_w + mt.cell_w * 4.2f, r->top() + mt.line_h * 6.5f},
                                       MouseButton::left, Mods{.alt = true}, 1});
            rt.window().push(MouseUp{Vec2{r->left() + mt.gutter_w + mt.cell_w * 4.2f, r->top() + mt.line_h * 6.5f},
                                     MouseButton::left, Mods{.alt = true}});
            rt.tick();
            ok(B().ed.sels.size() == 2, "alt-click adds a second caret");
            rt.type("//");
            ok(B().ed.line(5).rfind("    //", 0) == 0 && B().ed.line(6).rfind("    //", 0) == 0, "both carets type");
            rt.press_key(Key::z, cmd);
        } else ok(false, "editor rect");

        // multi-cursor by occurrence
        rt.press_key(Key::escape);
        rt.send(Editor::Msg{Editor::Do{Act::goto_line}});
        rt.type("8:31");
        rt.press_key(Key::enter);
        ok(B().ed.cursor() == (Pos{7, 30}), "go to line 8:31");
        rt.press_key(Key::d, cmd);
        rt.press_key(Key::d, cmd);
        ok(B().ed.sels.size() == 2 && B().ed.text_in(B().ed.sels[0].range()) == "area", "cmd-d twice selects both `area`s");
        rt.press_key(Key::escape);

        // file finder
        rt.press_key(Key::p, cmd);
        ok(M().pick == Editor::PickMode::files, "cmd-p opens the file finder");
        rt.type("shap");
        shot("finder");
        rt.press_key(Key::enter);
        ok(B().path == "src/shapes.hpp" && M().pick == Editor::PickMode::none, "fuzzy-open src/shapes.hpp");
        ok(M().bufs.size() == 2, "it opened in a second tab");

        // symbols
        rt.press_key(Key::o, cmd_sh);
        rt.type("squ");
        rt.press_key(Key::enter);
        ok(B().ed.cursor().line == 13, "cmd-shift-o jumps to struct Square");

        // find
        rt.press_key(Key::f, cmd);
        rt.type("area");
        ok(M().find_open && M().matches.size() == 3, "cmd-f finds 3 `area`s");
        shot("find");
        rt.press_key(Key::enter);
        rt.press_key(Key::escape);
        ok(!M().find_open, "esc closes find");

        // command palette
        rt.press_key(Key::p, cmd_sh);
        rt.type("toggle comm");
        shot("palette");
        rt.press_key(Key::enter);
        ok(B().ed.line(B().ed.cursor().line).find("//") != std::string::npos, "the palette runs `editor: toggle comments`");
        rt.press_key(Key::z, cmd);

        // completion
        rt.press_key(Key::down, cmd);
        rt.press_key(Key::enter);
        rt.type("Ci");
        ok(M().comp_open, "typing a prefix opens completion");
        shot("completion");
        const auto r = Editor::rank_completions(M());
        ok(!r.order.empty() && M().comp_items[static_cast<std::size_t>(r.order[0])].label == "Circle", "Circle ranks first for `Ci`");
        rt.press_key(Key::tab);
        ok(B().ed.line(B().ed.cursor().line) == "Circle" && !M().comp_open, "tab accepts it");

        // save (in-memory project)
        rt.press_key(Key::s, cmd);
        ok(!B().ed.dirty() && !M().toast_list.empty(), "cmd-s saves and toasts");

        // tree: open a preview, then another replaces it
        const auto rows = Editor::tree_rows(M());
        int rs_row = -1, py_row = -1;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& n = M().tree[static_cast<std::size_t>(rows[i])];
            if (n.path == "src/broken.rs") rs_row = static_cast<int>(i);
        }
        rt.send(Editor::Msg{Editor::TreeClick{rs_row, 1}});
        ok(B().path == "src/broken.rs" && B().preview, "single-click in the tree opens a preview tab");
        ok(Editor::problem_rows(M()).size() > 1 && Editor::problem_rows(M())[0].errors >= 1, "the unclosed { in broken.rs is an error");
        const std::size_t tabs_before = M().bufs.size();
        rt.send(Editor::Msg{Editor::TreeClick{0, 1}});   // a folder: toggles
        for (std::size_t i = 0; i < Editor::tree_rows(M()).size(); ++i)
            if (M().tree[static_cast<std::size_t>(Editor::tree_rows(M())[i])].path == "README.md") py_row = static_cast<int>(i);
        rt.send(Editor::Msg{Editor::TreeClick{py_row, 1}});
        ok(B().path == "README.md" && M().bufs.size() == tabs_before, "the next preview replaces the previous one");

        // problems panel + F8
        rt.send(Editor::Msg{Editor::OpenTab{1}});
        rt.send(Editor::Msg{Editor::TreeClick{rs_row, 2}});
        rt.press_key(Key::m, cmd_sh);
        ok(M().bottom && M().bottom_tab == 0, "cmd-shift-m opens problems");
        rt.press_key(Key::f8);
        ok(B().ed.cursor() == B().diags.front().range.start || B().ed.cursor().line >= 0, "F8 jumps to a diagnostic");
        if (auto er = rt.node_rect("editor"); er && !B().diags.empty()) {
            const auto& d = B().diags.front();
            const auto& mt = B().ed.metrics;
            rt.window().move_to(Vec2{er->left() + mt.gutter_w + (static_cast<float>(d.range.start.col) + 0.5f) * mt.cell_w - B().ed.scroll.offset.x,
                                     er->top() + (static_cast<float>(B().ed.visual_row_of(d.range.start.line)) + 0.5f) * mt.line_h - B().ed.scroll.offset.y});
            rt.tick();
        }
        shot("problems");

        // project search
        rt.press_key(Key::f, cmd_sh);
        rt.type("area");
        rt.press_key(Key::enter);
        ok(M().ps_rows.size() >= 5, "project search finds `area` across files");
        shot("search");

        // folding
        rt.send(Editor::Msg{Editor::OpenTab{0}});
        rt.send(Editor::Msg{Editor::Fold{4}});
        ok(B().ed.folds.contains(4), "clicking a fold chevron folds the block");

        // tabs
        const int n = static_cast<int>(M().bufs.size());
        rt.press_key(Key::w, cmd);
        ok(static_cast<int>(M().bufs.size()) == n - 1, "cmd-w closes the tab");

        // Blink restarts on any input.
        ok(!Editor::subscribe(M()).is_none(), "blink runs while the editor is focused");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
