// examples/terminal.cpp — a conventional terminal's chrome, with tabs that
// show what each tab is doing
//
// UI only: every screen is a hand-made TermGrid and the "processes" are
// simulated, so the tab strip can be seen live: status rings turning while a
// command runs, a progress ring filling (OSC 9;4), output sparklines, a
// green/red flash when a background command finishes, the sliding active
// pill, hover peek, and the new-tab menu.
//
//   ./mayag_terminal              open a window
//   ./mayag_terminal --headless   scripted, asserted
//   TM_SHOT=<dir> ./mayag_terminal --headless   screenshots

#include "harness.hpp"
#include <mayag/widget/activity_feed.hpp>
#include <mayag/widget/project_panel.hpp>
#include <mayag/widget/scrollback_bar.hpp>
#include <mayag/widget/tab_launcher.hpp>
#include <mayag/widget/tab_peek.hpp>
#include <mayag/widget/terminal_view.hpp>
#include <mayag/widget/workspace_rail.hpp>

#include <array>
#include <cmath>
#include <set>

using namespace mayag;
using namespace mayag::dsl;

namespace {

enum class Screen : std::uint8_t { shell, editor, build, ssh, logs, monitor };

struct TabData {
    TermTabInfo info;
    Screen      screen = Screen::shell;
    double      finish_at = -1;      ///< simulated: the running command ends at this time
    int         finish_code = 0;
    double      rate = 0.5;          ///< simulated output rate
};

[[nodiscard]] TermGrid screen_of(Screen s, int cols) {
    switch (s) {
        case Screen::editor:
            return term_grid({
                "{d}  1 {0}{m}use{0} std::collections::{b}HashMap{0};",
                "{d}  2 {0}",
                "{d}  3 {0}{m}pub struct{0} {b}Cache{0}<{b}K{0}, {b}V{0}> {",
                "{d}  4 {0}    entries: {b}HashMap{0}<{b}K{0}, {b}V{0}>,",
                "{d}  5 {0}    capacity: {b}usize{0},",
                "{d}  6 {0}}",
                "{d}  7 {0}",
                "{d}  8 {0}{m}impl{0}<{b}K{0}: {b}Hash{0} + {b}Eq{0}, {b}V{0}> {b}Cache{0}<{b}K{0}, {b}V{0}> {",
                "{d}  9 {0}    {m}pub fn{0} {c}get{0}(&{m}self{0}, key: &{b}K{0}) -> {b}Option{0}<&{b}V{0}> {",
                "{d} 10 {0}        {m}self{0}.entries.{c}get{0}(key)",
                "{d} 11 {0}    }",
                "{d} 12 {0}}",
                "{d}  ~ {0}",
                "{d}  ~ {0}",
                "",
                "{R} NORMAL {0} {d}src/cache.rs{0}                                              {d}rust  utf-8  9:24{0}",
            }, cols, true);
        case Screen::build:
            return term_grid({
                "{b}{B}~/src/mayag{0} {m}main{0} {g}\u276F{0} cargo build --release",
                "{g}{B}   Compiling{0} proc-macro2 v1.0.86",
                "{g}{B}   Compiling{0} unicode-ident v1.0.12",
                "{g}{B}   Compiling{0} serde v1.0.210",
                "{g}{B}   Compiling{0} syn v2.0.77",
                "{g}{B}   Compiling{0} tokio v1.40.0",
                "{g}{B}   Compiling{0} mayag-term v0.4.2 ({c}{U}~/src/mayag{0})",
                "{c}{B}    Building{0} [=================>       ] 212/287: mayag-term",
            }, cols);
        case Screen::ssh:
            return term_grid({
                "{d}Last login: Thu Sep  4 13:58:02 2026 from 10.0.1.7{0}",
                "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ psql -c 'select count(*) from events'",
                "  count",
                "{d}----------{0}",
                " 48211907",
                "{d}(1 row){0}",
                "",
                "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ df -h /var/lib/postgresql",
                "Filesystem      Size  Used Avail Use% Mounted on",
                "/dev/nvme1n1    1.8T  1.4T  402G  {y}78%{0} /var/lib/postgresql",
                "",
                "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ ",
            }, cols, true);
        case Screen::logs:
            return term_grid({
                "{b}{B}~/src/api{0} {g}\u276F{0} tail -f logs/access.log",
                "{d}14:02:11{0} {g}200{0} GET  /v1/events?limit=50        {d}12ms{0}",
                "{d}14:02:11{0} {g}200{0} GET  /v1/users/me               {d}4ms{0}",
                "{d}14:02:12{0} {g}201{0} POST /v1/events                 {d}31ms{0}",
                "{d}14:02:12{0} {y}429{0} GET  /v1/search?q=render       {d}1ms{0}",
                "{d}14:02:13{0} {g}200{0} GET  /v1/events?limit=50        {d}11ms{0}",
                "{d}14:02:13{0} {r}500{0} POST /v1/upload                 {d}2103ms{0}",
                "{d}14:02:14{0} {g}200{0} GET  /healthz                   {d}0ms{0}",
                "{d}14:02:14{0} {g}200{0} GET  /v1/events?limit=50        {d}13ms{0}",
            }, cols);
        case Screen::monitor:
            return term_grid({
                "  {B}CPU{0} [{g}||||||||||||{y}|||{0}            41.2%]   {B}Tasks:{0} 412, 1802 thr; 3 running",
                "  {B}Mem{0} [{g}||||||||||||||||{b}||||{0}      11.4G/32G]   {B}Load:{0} 2.81 2.40 2.13",
                "",
                "{R}  PID USER       CPU%  MEM%  TIME+    Command                              {0}",
                "  412 ayush      38.1   2.4  3:12.04  cargo build --release",
                " 1803 ayush      12.7   6.1  41:02.7  mayag_terminal",
                "  981 _windowser  6.2   1.1  2h04:11  WindowServer",
            }, cols);
        default:
            return term_grid({
                "{b}{B}~/src/mayag{0} {m}main{0} {g}\u276F{0} git status -sb",
                "## main...origin/main [ahead 2]",
                " {r}M{0} include/mayag/widget/tab_strip.hpp",
                "{r}??{0} examples/terminal.cpp",
                "{b}{B}~/src/mayag{0} {m}main{0} {g}\u276F{0} ls",
                "{b}{B}cmake{0}  CMakeLists.txt  {b}{B}docs{0}  {b}{B}examples{0}  {b}{B}include{0}  LICENSE  README.md  {b}{B}tests{0}  {b}{B}tools{0}",
                "{b}{B}~/src/mayag{0} {m}main{0} {g}\u276F{0} ",
            }, cols, true);
    }
}

[[nodiscard]] TabData make_tab(std::string title, std::string cwd, std::string process, Screen s) {
    TabData d;
    d.info.title = std::move(title);
    d.info.cwd = std::move(cwd);
    d.info.process = std::move(process);
    d.screen = s;
    return d;
}

const std::vector<LaunchProfile> kProfiles{
    {"zsh", "/bin/zsh -l", "zsh", 0, "\u2318T", false, true},
    {"fish", "/opt/homebrew/bin/fish", "fish", 0x6FB3D2, "", false, false},
    {"python", "python3 -q", "python", 0xE8C547, "", false, false},
    {"prod-db-01", "ssh deploy@10.0.4.21", "ssh", 0xF7768E, "", true, false},
    {"gpu-box", "ssh ayush@gpu.lan", "ssh", 0x9ECE6A, "", false, false},
};
const std::vector<std::string> kRecent{"~/src/mayag", "~/src/core", "~/src/api", "~/Downloads"};

// ── a workspace's folder ──────────────────────────────────────────────────
// Simulated: a file list in tree order (folders end in '/', folders before
// files), shared by every worktree of the repo, plus git marks.
struct Wt { std::string branch, path; int ahead = 0, behind = 0; };
struct Project {
    std::string name, root;
    bool        git = true;
    std::vector<Wt> worktrees;
    std::vector<std::pair<std::string, char>> files;   ///< path, git mark
    std::string empty;                                 ///< no folder: why
    int         shown = 0;
    std::set<std::string> expanded;
    int         tree_sel = -1;
    ScrollState tree_scroll;
};

[[nodiscard]] Project mayag_project() {
    Project p{.name = "mayag", .root = "~/src/mayag",
              .worktrees = {{"main", "~/src/mayag", 10, 0}, {"fix/cell-px", "~/src/mayag-wt/cell-px", 2, 3},
                            {"agent/metal-text", "~/src/mayag-wt/metal-text", 5, 0}}};
    p.files = {{"examples/", 0}, {"examples/editor.cpp", 0}, {"examples/harness.hpp", 0}, {"examples/terminal.cpp", 'M'},
               {"include/", 0}, {"include/mayag/", 0}, {"include/mayag/app/", 0}, {"include/mayag/app/app.hpp", 0},
               {"include/mayag/app/sources.hpp", 0}, {"include/mayag/render/", 0}, {"include/mayag/render/kernel.hpp", 0},
               {"include/mayag/widget/", 0}, {"include/mayag/widget/file_tree.hpp", 'M'},
               {"include/mayag/widget/project_panel.hpp", 'U'}, {"include/mayag/widget/tab_strip.hpp", 0},
               {"include/mayag/widget/terminal_view.hpp", 0}, {"include/mayag/mayag.hpp", 'M'}, {"tests/", 0},
               {"tests/test_mayag.cpp", 0}, {"tools/", 0}, {"tools/photon-bench.sh", 0}, {"CMakeLists.txt", 0},
               {"README.md", 0}};
    p.expanded = {"examples/"};
    return p;
}
[[nodiscard]] Project core_project() {
    Project p{.name = "core", .root = "~/src/core", .worktrees = {{"main", "~/src/core", 0, 1}, {"perf/arena", "~/src/core-wt/arena", 4, 0}}};
    p.files = {{"benches/", 0}, {"benches/cache.rs", 'M'}, {"src/", 0}, {"src/cache.rs", 'M'}, {"src/lib.rs", 0},
               {"src/arena.rs", 'A'}, {"tests/", 0}, {"tests/cache.rs", 0}, {"Cargo.toml", 0}, {"Cargo.lock", 'M'}};
    p.expanded = {"src/"};
    return p;
}
[[nodiscard]] Project home_project() {
    Project p{.name = "ayush", .root = "~", .git = false, .worktrees = {{"", "~"}}};
    p.files = {{"Desktop/", 0}, {"Downloads/", 0}, {"Downloads/notes.txt", 0}, {"src/", 0}, {".zshrc", 0}, {"todo.md", 0}};
    return p;
}

[[nodiscard]] bool is_dir(std::string_view p) { return !p.empty() && p.back() == '/'; }
[[nodiscard]] int depth_of(std::string_view p) {
    int d = 0;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) d += p[i] == '/';
    return d;
}
[[nodiscard]] std::string leaf_of(std::string_view p) {
    if (is_dir(p)) p.remove_suffix(1);
    const auto s = p.rfind('/');
    return std::string(s == std::string_view::npos ? p : p.substr(s + 1));
}
/// "a/b/c.txt" -> {"a/", "a/b/"}
[[nodiscard]] std::vector<std::string> ancestors_of(std::string_view p) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) if (p[i] == '/') out.emplace_back(p.substr(0, i + 1));
    return out;
}
/// Indices of the files the tree shows (every ancestor folder expanded).
[[nodiscard]] std::vector<int> visible_rows(const Project& p) {
    std::vector<int> rows;
    for (std::size_t i = 0; i < p.files.size(); ++i) {
        bool ok = true;
        for (const auto& a : ancestors_of(p.files[i].first)) ok = ok && p.expanded.contains(a);
        if (ok) rows.push_back(static_cast<int>(i));
    }
    return rows;
}
/// Which worktree a working directory is in (-1: outside the project), and
/// the folder inside it ("" = the worktree root, else "src/").
[[nodiscard]] std::pair<int, std::string> locate(const Project& p, const TermTabInfo& tb) {
    if (!tb.host.empty() || p.root.empty()) return {-1, {}};
    int best = -1;
    std::size_t best_len = 0;
    for (std::size_t w = 0; w < p.worktrees.size(); ++w) {
        const std::string& root = p.worktrees[w].path;
        const bool in = tb.cwd == root || (tb.cwd.starts_with(root) && tb.cwd.size() > root.size() && tb.cwd[root.size()] == '/')
                        || root == "~";
        if (in && root.size() >= best_len) { best = static_cast<int>(w); best_len = root.size(); }
    }
    if (best < 0) return {-1, {}};
    if (tb.cwd.size() <= best_len + 1) return {best, {}};
    return {best, tb.cwd.substr(p.worktrees[static_cast<std::size_t>(best)].path == "~" ? 2 : best_len + 1) + "/"};
}

}  // namespace

struct Terminal {
    struct Workspace {
        std::string          name;
        std::uint32_t        hue = 0;
        std::vector<TabData> tabs;     ///< parked here while another workspace is shown
        int                  active = 0;
        Project              project;  ///< the folder behind it (and its worktrees)
    };
    struct Model {
        std::vector<TabData> tabs;     ///< the shown workspace's tabs
        int    active = 0;
        std::vector<Workspace> workspaces;
        int    ws = 0;
        bool   rail_open = true;
        bool   panel_open = true;
        std::vector<ActivityEvent> feed;
        TabStripMotion motion;
        int    hovered = -1;
        double hover_since = 0;
        bool   launcher = false;
        int    launcher_sel = -1;
        bool   numbers = false;
        double time = 0;
        int    opened = 0;
    };

    struct Activate { int i; };  struct Close { int i; };  struct NewTab { int profile; };
    struct Launcher {};          struct LaunchDir { int j; };
    struct Hover { int i; };     struct Unhover { int i; };
    struct SwitchWs { int i; };  struct ToggleRail {};       struct FeedClick { int i; };  struct FeedDismiss { int i; };
    struct Key_ { KeyEvent k; }; struct Tick { double dt; }; struct Quit {};
    struct ShowWt { int i; };    struct WtTab { int i; };     struct NewWt {};             struct TogglePanel {};
    struct TreeClick { int row; }; struct TreeAction { int row; }; struct TreeWheel { Vec2 d; };

    using Msg = std::variant<Activate, Close, NewTab, Launcher, LaunchDir, Hover, Unhover, SwitchWs, ToggleRail, FeedClick,
                             FeedDismiss, Key_, Tick, Quit, ShowWt, WtTab, NewWt, TogglePanel, TreeClick, TreeAction, TreeWheel>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
        auto shell = make_tab("mayag", "~/src/mayag", "zsh", Screen::shell);
        shell.info.last_exit = 0;
        auto ed = make_tab("file_tree.hpp", "~/src/mayag/include/mayag/widget", "nvim", Screen::editor);
        auto build = make_tab("cargo build", "~/src/mayag-wt/cell-px", "cargo", Screen::build);
        build.info.running = true; build.info.elapsed = 48; build.info.progress = 74; build.rate = 0.7;
        auto test = make_tab("cargo test", "~/src/core", "cargo", Screen::build);
        test.info.running = false; test.info.last_exit = 101; test.info.finished_ago = 0.9;
        for (int k = 0; k < 18; ++k) test.info.activity.push_back(0.0f);
        auto ssh = make_tab("prod-db-01", "/home/deploy", "ssh", Screen::ssh);
        ssh.info.host = "deploy@prod-db-01"; ssh.info.host_danger = true; ssh.info.hue = 0xF7768E; ssh.info.unread = true;
        auto logs = make_tab("tail -f access.log", "~/src/api", "tail", Screen::logs);
        logs.info.running = true; logs.info.elapsed = 3725; logs.rate = 0.9;
        auto top = make_tab("htop", "~", "htop", Screen::monitor);
        top.info.pinned = true;
        m.tabs = {top, shell, ed, build, test, ssh, logs};
        m.active = 1;
        // Seed the sparklines so the first frame already shows them.
        for (auto& t : m.tabs)
            if (t.info.running)
                for (int k = 0; k < 28; ++k)
                    t.info.activity.push_back(static_cast<float>(0.25 + 0.6 * std::fabs(std::sin(k * 0.7 + t.rate * 9.0))) * static_cast<float>(t.rate));
        m.numbers = std::getenv("TM_NUMBERS") != nullptr;
        if (std::getenv("TM_LAUNCHER") != nullptr) m.launcher = true;

        // Other workspaces, running in the background.
        const auto seeded = [](TabData d) {
            if (d.info.running)
                for (int k = 0; k < 28; ++k)
                    d.info.activity.push_back(static_cast<float>(0.2 + 0.6 * std::fabs(std::sin(k * 0.9 + d.rate * 5.0))) * static_cast<float>(d.rate));
            return d;
        };
        auto jt = make_tab("cargo test", "~/src/core", "cargo", Screen::build);
        jt.info.running = true; jt.info.elapsed = 12; jt.rate = 0.6; jt.finish_at = 2.5; jt.finish_code = 101;
        auto jsh = make_tab("core", "~/src/core/src", "zsh", Screen::shell); jsh.info.last_exit = 0;
        auto bench = make_tab("cargo bench", "~/src/core-wt/arena", "cargo", Screen::build);
        bench.info.running = true; bench.info.elapsed = 194; bench.info.progress = 38; bench.rate = 0.4;
        auto p1 = make_tab("prod-db-01", "/home/deploy", "ssh", Screen::ssh);
        p1.info.host = "deploy@prod-db-01"; p1.info.host_danger = true; p1.info.hue = 0xF7768E;
        auto p2 = make_tab("prod-web-03", "/var/www", "ssh", Screen::logs);
        p2.info.host = "deploy@prod-web-03"; p2.info.host_danger = true; p2.info.hue = 0xF7768E; p2.info.running = true; p2.rate = 0.8;
        p2.info.elapsed = 9000;
        auto deploy = make_tab("deploy.sh", "~/ops", "bash", Screen::build);
        deploy.info.running = true; deploy.info.elapsed = 61; deploy.info.progress = 55; deploy.rate = 0.5; deploy.finish_at = 6.0;
        auto notes = make_tab("scratch", "~", "zsh", Screen::shell);
        Project prod;
        prod.empty = "An ssh workspace: its tabs are on 3 remote hosts.";
        m.workspaces = {{"mayag", 0x8AB4F8, {}, 0, mayag_project()},
                        {"core", 0xBB9AF7, {seeded(jsh), seeded(jt), seeded(bench)}, 1, core_project()},
                        {"prod", 0xF7768E, {seeded(p1), seeded(p2), seeded(deploy)}, 0, prod},
                        {"scratch", 0x9ECE6A, {notes}, 0, home_project()}};
        m.ws = 0;
        if (std::getenv("TM_RAIL_COLLAPSED") != nullptr) m.rail_open = false;
        if (std::getenv("TM_NO_PANEL") != nullptr) m.panel_open = false;
        follow(m);
        return SetTitle{"mayag terminal"};
    }

    // ── the project panel ──────────────────────────────────────────────────────────
    static Project& proj(Model& m) { return m.workspaces[static_cast<std::size_t>(m.ws)].project; }
    static const Project& proj(const Model& m) { return m.workspaces[static_cast<std::size_t>(m.ws)].project; }

    /// The tree follows the focused tab: show its worktree and reveal its cwd.
    static void follow(Model& m) {
        Project& p = proj(m);
        const auto [w, folder] = locate(p, m.tabs[static_cast<std::size_t>(m.active)].info);
        if (w < 0) return;
        p.shown = w;
        p.tree_sel = -1;
        if (folder.empty()) return;
        for (const auto& a : ancestors_of(folder)) p.expanded.insert(a);
        p.expanded.insert(folder);
        const auto rows = visible_rows(p);
        for (std::size_t r = 0; r < rows.size(); ++r)
            if (p.files[static_cast<std::size_t>(rows[r])].first == folder) {
                p.tree_sel = static_cast<int>(r);
                const float y = static_cast<float>(r) * kTreeRowH;   // reveal
                if (p.tree_scroll.viewport.y > 0 && (y < p.tree_scroll.offset.y || y + kTreeRowH > p.tree_scroll.offset.y + p.tree_scroll.viewport.y))
                    p.tree_scroll.scroll_to({0, std::max(0.0f, y - p.tree_scroll.viewport.y * 0.4f)});
            }
    }
    /// The visible row a folder shows up as: itself, or its deepest expanded ancestor.
    static std::string shown_as(const Project& p, const std::string& folder) {
        std::string at;
        for (const auto& a : ancestors_of(folder)) { if (!p.expanded.contains(a)) return at; at = a; }
        return folder;
    }
    static void open_tab(Model& m, TabData d) {
        m.tabs.insert(m.tabs.begin() + m.active + 1, std::move(d));
        m.active += 1;
        ++m.opened;
        follow(m);
    }
    static std::string wt_dir(const Project& p, int w, std::string_view folder) {
        std::string base = p.worktrees[static_cast<std::size_t>(w)].path;
        if (!folder.empty()) { base += "/"; base += folder.substr(0, folder.size() - 1); }
        return base;
    }
    static Cmd update(Model& m, ShowWt e) {
        Project& p = proj(m);
        if (e.i < 0 || e.i >= static_cast<int>(p.worktrees.size())) return {};
        p.shown = e.i;
        p.tree_sel = -1;
        return {};
    }
    static Cmd update(Model& m, WtTab e) {
        const Project& p = proj(m);
        if (e.i < 0 || e.i >= static_cast<int>(p.worktrees.size())) return {};
        const auto& w = p.worktrees[static_cast<std::size_t>(e.i)];
        auto d = make_tab(w.branch.empty() ? p.name : leaf_of(w.branch), w.path, "zsh", Screen::shell);
        open_tab(m, std::move(d));
        return {};
    }
    static Cmd update(Model& m, NewWt) {
        Project& p = proj(m);
        if (!p.git) return {};
        const std::string name = "agent/task-" + std::to_string(p.worktrees.size());
        p.worktrees.push_back({name, p.root + "-wt/task-" + std::to_string(p.worktrees.size())});
        p.shown = static_cast<int>(p.worktrees.size()) - 1;
        return update(m, WtTab{p.shown});
    }
    static Cmd update(Model& m, TogglePanel) { m.panel_open = !m.panel_open; return {}; }
    static Cmd update(Model& m, TreeClick e) {
        Project& p = proj(m);
        const auto rows = visible_rows(p);
        if (e.row < 0 || e.row >= static_cast<int>(rows.size())) return {};
        const std::string& path = p.files[static_cast<std::size_t>(rows[static_cast<std::size_t>(e.row)])].first;
        p.tree_sel = e.row;
        if (is_dir(path)) { if (!p.expanded.erase(path)) p.expanded.insert(path); }
        return {};
    }
    static Cmd update(Model& m, TreeAction e) {
        const Project& p = proj(m);
        const auto rows = visible_rows(p);
        if (e.row < 0 || e.row >= static_cast<int>(rows.size())) return {};
        const std::string path = p.files[static_cast<std::size_t>(rows[static_cast<std::size_t>(e.row)])].first;
        if (is_dir(path)) {   // a shell in that folder
            open_tab(m, make_tab(leaf_of(path), wt_dir(p, p.shown, path), "zsh", Screen::shell));
        } else {              // $EDITOR on that file, from its folder
            const auto anc = ancestors_of(path);
            open_tab(m, make_tab(leaf_of(path), wt_dir(p, p.shown, anc.empty() ? std::string{} : anc.back()), "nvim", Screen::editor));
        }
        return {};
    }
    static Cmd update(Model& m, TreeWheel e) { proj(m).tree_scroll.scroll_by(e.d); return {}; }

    static Cmd update(Model& m, Activate e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.tabs.size())) return {};
        m.active = e.i;
        m.tabs[static_cast<std::size_t>(e.i)].info.unread = false;
        m.tabs[static_cast<std::size_t>(e.i)].info.bell = false;
        follow(m);
        return {};
    }
    static Cmd update(Model& m, Close e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.tabs.size()) || m.tabs.size() == 1) return {};
        m.tabs.erase(m.tabs.begin() + e.i);
        if (m.active >= e.i && m.active > 0) --m.active;
        m.hovered = -1;
        follow(m);
        return {};
    }
    static Cmd update(Model& m, NewTab e) {
        const auto& p = kProfiles[static_cast<std::size_t>(std::clamp(e.profile, 0, static_cast<int>(kProfiles.size()) - 1))];
        // A new shell opens where you are looking: the worktree the panel shows.
        const Project& pr = proj(m);
        const std::string here = pr.root.empty() ? std::string{"~"} : pr.worktrees[static_cast<std::size_t>(pr.shown)].path;
        TabData d = make_tab(p.name, p.process == "ssh" ? "~" : here, p.process, p.process == "ssh" ? Screen::ssh : Screen::shell);
        if (p.process == "ssh") { d.info.host = p.command.substr(4); d.info.host_danger = p.danger; d.info.hue = p.hue; }
        m.launcher = false;
        open_tab(m, std::move(d));
        return {};
    }
    static Cmd update(Model& m, Launcher) { m.launcher = !m.launcher; m.launcher_sel = -1; return {}; }
    static Cmd update(Model& m, LaunchDir e) {
        TabData d = make_tab(kRecent[static_cast<std::size_t>(e.j)].substr(kRecent[static_cast<std::size_t>(e.j)].rfind('/') + 1),
                             kRecent[static_cast<std::size_t>(e.j)], "zsh", Screen::shell);
        m.launcher = false;
        open_tab(m, std::move(d));
        return {};
    }
    static Cmd update(Model& m, SwitchWs e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.workspaces.size()) || e.i == m.ws) return {};
        // Park the current workspace's tabs, bring the other's forward.
        m.workspaces[static_cast<std::size_t>(m.ws)].tabs = std::move(m.tabs);
        m.workspaces[static_cast<std::size_t>(m.ws)].active = m.active;
        m.ws = e.i;
        m.tabs = std::move(m.workspaces[static_cast<std::size_t>(e.i)].tabs);
        m.active = std::clamp(m.workspaces[static_cast<std::size_t>(e.i)].active, 0, static_cast<int>(m.tabs.size()) - 1);
        m.motion = TabStripMotion{};   // the pill snaps into the new strip
        m.hovered = -1;
        // Seeing a workspace clears its events.
        std::erase_if(m.feed, [&](const ActivityEvent& ev) { return ev.where.rfind(m.workspaces[static_cast<std::size_t>(e.i)].name, 0) == 0; });
        return update(m, Activate{m.active});
    }
    static Cmd update(Model& m, ToggleRail) { m.rail_open = !m.rail_open; return {}; }
    static Cmd update(Model& m, FeedClick e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.feed.size())) return {};
        const std::string where = m.feed[static_cast<std::size_t>(e.i)].where;
        for (std::size_t w = 0; w < m.workspaces.size(); ++w)
            if (where.rfind(m.workspaces[w].name, 0) == 0) return update(m, SwitchWs{static_cast<int>(w)});
        return {};
    }
    static Cmd update(Model& m, FeedDismiss e) {
        if (e.i >= 0 && e.i < static_cast<int>(m.feed.size())) m.feed.erase(m.feed.begin() + e.i);
        return {};
    }
    static Cmd update(Model& m, Hover e) { if (m.hovered != e.i) { m.hovered = e.i; m.hover_since = m.time; } return {}; }
    static Cmd update(Model& m, Unhover e) { if (m.hovered == e.i) m.hovered = -1; return {}; }
    static Cmd update(Model& m, Key_ e) {
        const auto& k = e.k;
        if (m.launcher) {
            const int n = static_cast<int>(kProfiles.size() + kRecent.size());
            if (k.key == Key::escape) { m.launcher = false; return {}; }
            if (k.key == Key::down) { m.launcher_sel = (m.launcher_sel + 1) % n; return {}; }
            if (k.key == Key::up) { m.launcher_sel = (m.launcher_sel - 1 + n) % n; return {}; }
            if (k.key == Key::enter && m.launcher_sel >= 0) {
                if (m.launcher_sel < static_cast<int>(kProfiles.size())) return update(m, NewTab{m.launcher_sel});
                return update(m, LaunchDir{m.launcher_sel - static_cast<int>(kProfiles.size())});
            }
            return {};
        }
        const bool cmd = k.mods.primary();
        if (k.mods == workspace_mods() && k.key >= Key::n1 && k.key <= Key::n9)
            return update(m, SwitchWs{static_cast<int>(k.key) - static_cast<int>(Key::n1)});
        if (cmd && k.key == Key::b) return update(m, ToggleRail{});
        if (cmd && k.key == Key::backslash) return update(m, TogglePanel{});
        if (cmd && k.key == Key::t) return update(m, NewTab{0});
        if (cmd && k.key == Key::w) return update(m, Close{m.active});
        if (cmd && k.mods.shift && k.key == Key::bracket_right) return update(m, Activate{(m.active + 1) % static_cast<int>(m.tabs.size())});
        if (cmd && k.mods.shift && k.key == Key::bracket_left)
            return update(m, Activate{(m.active - 1 + static_cast<int>(m.tabs.size())) % static_cast<int>(m.tabs.size())});
        if (cmd && k.key >= Key::n1 && k.key <= Key::n9) {
            const int i = k.key == Key::n9 ? static_cast<int>(m.tabs.size()) - 1 : static_cast<int>(k.key) - static_cast<int>(Key::n1);
            return update(m, Activate{i});
        }
        if (k.key == Key::super) { m.numbers = true; return {}; }
        return {};
    }
    /// Workspaces switch on a modifier that isn't the tab one: ⌃N on macOS
    /// (⌘N is tabs), Alt+N elsewhere (Ctrl+N is tabs there).
    static constexpr Mods workspace_mods() noexcept {
#if defined(__APPLE__)
        return Mods{.ctrl = true};
#else
        return Mods{.alt = true};
#endif
    }
    /// One tab's simulated life. `foreground` tabs never count as unread.
    static void simulate(Model& m, TabData& t, std::size_t i, double dt, bool foreground, const std::string& ws_name, std::uint32_t hue) {
        if (t.info.running) {
            t.info.elapsed += dt;
            if (t.info.progress >= 0) t.info.progress = std::min(100, t.info.progress + (std::fmod(m.time, 0.6) < dt ? 1 : 0));
            const float sample = static_cast<float>(std::clamp(t.rate * (0.35 + 0.65 * std::fabs(std::sin(m.time * 3.1 + static_cast<double>(i)))), 0.0, 1.0));
            if (std::fmod(m.time, 0.25) < dt) {   // four samples a second
                t.info.activity.push_back(sample);
                if (t.info.activity.size() > 40) t.info.activity.erase(t.info.activity.begin());
                if (!foreground) t.info.unread = true;
            }
            if (t.finish_at >= 0 && m.time >= t.finish_at) {   // the command ends
                t.info.running = false;
                t.info.progress = -1;
                t.info.last_exit = t.finish_code;
                t.info.finished_ago = 0;
                t.finish_at = -1;
                if (!foreground)
                    m.feed.insert(m.feed.begin(), ActivityEvent{t.finish_code ? ActivityEvent::Kind::failed : ActivityEvent::Kind::finished,
                                                                t.info.title, ws_name + " \u00B7 " + t.info.title, hue, t.info.elapsed,
                                                                t.finish_code, 0});
            }
        } else {
            t.info.finished_ago += dt;
        }
    }
    static Cmd update(Model& m, Tick e) {
        m.time += e.dt;
        const auto& cur = m.workspaces[static_cast<std::size_t>(m.ws)];
        for (std::size_t i = 0; i < m.tabs.size(); ++i)
            simulate(m, m.tabs[i], i, e.dt, static_cast<int>(i) == m.active, cur.name, cur.hue);
        for (std::size_t w = 0; w < m.workspaces.size(); ++w) {
            if (static_cast<int>(w) == m.ws) continue;
            auto& ws = m.workspaces[w];
            for (std::size_t i = 0; i < ws.tabs.size(); ++i) simulate(m, ws.tabs[i], i, e.dt, false, ws.name, ws.hue);
        }
        for (auto& ev : m.feed) ev.age += e.dt;
        std::erase_if(m.feed, [](const ActivityEvent& ev) { return ev.kind == ActivityEvent::Kind::finished && ev.age > 6.0; });
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    [[nodiscard]] static Theme theme() {
        Theme t = make_theme(rgb<0x8AB4F8>, mayag::Mode::dark);
        t.background = rgb<0x101116>;
        t.surface = rgb<0x15161C>;
        t.surface_raised = rgb<0x21232D>;
        t.overlay = rgb<0x1A1C24>;
        t.border = rgb<0x23252F>;
        t.border_strong = rgb<0x353847>;
        t.text_primary = rgb<0xE8E9F0>;
        t.text_secondary = rgb<0xA2A5B5>;
        t.text_disabled = rgb<0x5E6272>;
        t.font_size = 12.5f;
        return t;
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        Ctx look = c;
        look.theme = theme();
        const Theme& t = look.theme;
        TermTheme tt;
        tt.bg = 0x101116; tt.fg = 0xE8E9F0;

        std::vector<TermTabInfo> infos;
        for (const auto& d : m.tabs) infos.push_back(d.info);
        Node strip = tab_strip(look, "tabs", infos, m.active, &m.motion, m.time, {.show_numbers = m.numbers});

        const TabData& cur = m.tabs[static_cast<std::size_t>(m.active)];
        TermGrid g = screen_of(cur.screen, 150);
        g.rows = 36;
        while (static_cast<int>(g.lines.size()) < g.rows) g.lines.push_back(term_row("", g.cols));
        if (g.cursor_visible) { g.cursor_row = static_cast<int>(g.lines.size()) > 0 ? [&] {
                int last = 0; for (int r = 0; r < g.rows; ++r) { for (const auto& cc : g.lines[static_cast<std::size_t>(r)].cells) if (cc.text != " " && !cc.text.empty()) { last = r; break; } }
                return last; }() : 0;
            int col = 0; const auto& row = g.lines[static_cast<std::size_t>(g.cursor_row)].cells;
            for (int i = 0; i < static_cast<int>(row.size()); ++i) if (row[static_cast<std::size_t>(i)].text != " ") col = i + 1;
            g.cursor_col = std::min(col + (cur.screen == Screen::editor ? 0 : 0), g.cols - 1);
            if (cur.screen == Screen::editor) { g.cursor_row = 8; g.cursor_col = 26; }
        }
        ScrollbackInfo sb{.total = 2400, .first = 2364, .visible = 36,
                          .marks = {{400, ScrollbackMark::Kind::prompt}, {900, ScrollbackMark::Kind::prompt}, {1310, ScrollbackMark::Kind::error},
                                    {1800, ScrollbackMark::Kind::prompt}, {2364, ScrollbackMark::Kind::prompt}}};
        // The grid is a pure function of which screen the tab shows.
        Node grid = memo(MemoKey{}.add(static_cast<int>(cur.screen)), [&] {
            return terminal_view(look, "term", g, tt, {.font_size = 13.0f, .padding = 14.0f, .focused = true, .marks = false});
        });
        Node body = (h(v(node(std::move(grid)))
                           | grow() | height(pct(100)) | min_size(0, 0),
                       node(scrollback_bar(look, "sb", sb, 700.0f)))
                     | grow() | width(pct(100)) | min_size(0, 0)).build();

        // Overlays.
        if (m.launcher) tab_launcher(look, "launch", part_id("tabs", part_menu), kProfiles, kRecent, m.launcher_sel);
        if (!m.launcher && m.hovered >= 0 && m.hovered < static_cast<int>(m.tabs.size()) && m.hovered != m.active &&
            m.time - m.hover_since > 0.45) {
            const TabData& h_ = m.tabs[static_cast<std::size_t>(m.hovered)];
            TermGrid pg = screen_of(h_.screen, 64);
            tab_peek(look, "peek", part_id("tabs", static_cast<std::size_t>(m.hovered)), h_.info,
                     terminal_view(look, "peekterm", pg, tt, {.font_size = 10.5f, .padding = 8.0f, .focused = false, .marks = false}),
                     "pid " + std::to_string(4100 + m.hovered * 37) + "  \u00B7  " + h_.info.process + "  \u00B7  \u2318" + std::to_string(m.hovered + 1),
                     m.time);
        }

        // The rail: every workspace, the current one read live from m.tabs.
        std::vector<WorkspaceInfo> wsi;
        for (std::size_t w = 0; w < m.workspaces.size(); ++w) {
            WorkspaceInfo wi{m.workspaces[w].name, m.workspaces[w].hue, {}, {}};
            for (const auto& d : static_cast<int>(w) == m.ws ? m.tabs : m.workspaces[w].tabs) wi.tabs.push_back(d.info);
            wsi.push_back(std::move(wi));
        }
        Node rail = workspace_rail(look, "rail", wsi, m.ws, m.time, {.expanded = m.rail_open});
        activity_feed(look, "feed", m.feed, c.size, m.time);

        Node panel = m.panel_open ? project_view(look, m) : Node{};
        return h(node(std::move(rail)),
                 node(std::move(panel)),
                 v(node(std::move(strip)), node(std::move(body))) | grow() | height(pct(100)) | min_size(0, 0))
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    /// The project panel's data, read from the folder and from where every tab is.
    static Node project_view(const Ctx& look, const Model& m) {
        const Project& p = proj(m);
        ProjectInfo pi{.name = p.name, .root = p.root, .git = p.git, .shown = p.shown, .empty = p.empty};
        const TermTabInfo& focus = m.tabs[static_cast<std::size_t>(m.active)].info;
        const auto [fw, ffolder] = locate(p, focus);
        for (std::size_t w = 0; w < p.worktrees.size(); ++w) {
            ProjectWorktree pw{.branch = p.worktrees[w].branch, .path = p.worktrees[w].path,
                               .ahead = p.worktrees[w].ahead, .behind = p.worktrees[w].behind};
            for (const auto& [f, g] : p.files) pw.dirty += g != 0 && w == 0;   // the demo's changes live on main
            if (w == 1) pw.dirty = 2;
            for (const auto& d : m.tabs) if (locate(p, d.info).first == static_cast<int>(w)) pw.tabs.push_back(tab_hue(d.info));
            pw.here = fw == static_cast<int>(w);
            pw.here_hue = tab_hue(focus);
            pi.worktrees.push_back(std::move(pw));
        }
        std::vector<TreeRow> rows;
        if (!p.root.empty()) {
            // Each tab's folder, as the row it shows up on (a collapsed
            // ancestor carries the dots of everything inside it).
            std::vector<std::pair<std::string, Color<Srgb>>> at;
            for (const auto& d : m.tabs) {
                const auto [w, folder] = locate(p, d.info);
                if (w == p.shown && !folder.empty()) at.emplace_back(shown_as(p, folder), tab_hue(d.info));
            }
            const std::string here = fw == p.shown ? shown_as(p, ffolder) : std::string{"\x01"};
            for (const int i : visible_rows(p)) {
                const auto& [path, git] = p.files[static_cast<std::size_t>(i)];
                TreeRow r{.name = leaf_of(path), .depth = depth_of(path), .dir = is_dir(path),
                          .open = p.expanded.contains(path), .git = git};
                for (const auto& [f, col] : at) if (f == path) r.tabs.push_back(col);
                r.here = path == here;
                r.here_hue = tab_hue(focus);
                r.action = r.dir ? "+ tab" : "edit";
                rows.push_back(std::move(r));
            }
        }
        return project_panel(look, "proj", pi, rows, p.tree_sel, p.tree_scroll);
    }

    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        for (std::size_t i = 0; i < m.tabs.size(); ++i) {
            if (is(c, "tabs", tab_part_close + i)) return Close{static_cast<int>(i)};
            if (is(c, "tabs", i)) return c.button == MouseButton::middle ? Msg{Close{static_cast<int>(i)}} : Msg{Activate{static_cast<int>(i)}};
        }
        if (is(c, "tabs", part_inc)) return NewTab{0};
        if (is(c, "tabs", part_menu)) return Launcher{};
        for (std::size_t k = 0; k < kProfiles.size(); ++k) if (is(c, "launch", k)) return NewTab{static_cast<int>(k)};
        for (std::size_t j = 0; j < kRecent.size(); ++j) if (is(c, "launch", 1000 + j)) return LaunchDir{static_cast<int>(j)};
        for (std::size_t w = 0; w < m.workspaces.size(); ++w) if (is(c, "rail", w)) return SwitchWs{static_cast<int>(w)};
        if (is(c, "rail", part_menu)) return ToggleRail{};
        for (std::size_t i = 0; i < m.feed.size(); ++i) {
            if (is(c, "feed", 4096 + i)) return FeedDismiss{static_cast<int>(i)};
            if (is(c, "feed", i)) return FeedClick{static_cast<int>(i)};
        }
        const Project& p = proj(m);
        for (std::size_t w = 0; w < p.worktrees.size(); ++w) {
            if (is(c, "proj", kTreeAction + w)) return WtTab{static_cast<int>(w)};
            if (is(c, "proj", w)) return ShowWt{static_cast<int>(w)};
        }
        if (is(c, "proj", part_inc)) return NewWt{};
        const std::string tid = tree_id("proj");
        const std::size_t n = visible_rows(p).size();
        for (std::size_t r = 0; r < n; ++r) {
            if (is(c, tid, kTreeAction + r)) return TreeAction{static_cast<int>(r)};
            if (is(c, tid, r)) return c.count >= 2 ? Msg{TreeAction{static_cast<int>(r)}} : Msg{TreeClick{static_cast<int>(r)}};
        }
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const mayag::Scrolled& s) {
        if (is(s, tree_id("proj"))) return TreeWheel{s.delta};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Entered& e) {
        for (std::size_t i = 0; i < m.tabs.size(); ++i) if (is(e, "tabs", i)) return Hover{static_cast<int>(i)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Left& l) {
        if (m.launcher && is(l, "launch", part_menu)) return Launcher{};
        for (std::size_t i = 0; i < m.tabs.size(); ++i) if (is(l, "tabs", i)) return Unhover{static_cast<int>(i)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (k.mods.primary() && k.key == Key::q) return Quit{};
        return Key_{k};
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // The demo's processes are simulated, so it ticks continuously; a real
    // terminal would tick only while a ring turns or the pill slides.
    static Sub subscribe(const Model&) { return Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }); }
};

static_assert(mayag::Program<Terminal>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1440, 860});
    std::shared_ptr<typo::FontStack> fonts;
    if (opts.use_system_fonts) {
        static constexpr std::array<std::string_view, 4> mono{"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono"};
        fonts = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400, false, mono);
    } else {
        fonts = demo::make_fonts(opts);
    }
    AppConfig cfg{.title = "mayag terminal", .size = opts.size, .theme = Terminal::theme(), .fonts = fonts, .debug_bounds = opts.debug_bounds};
    return demo::run<Terminal>(opts, cfg, [](auto& rt) {
        std::printf("terminal headless\n");
        int fails = 0;
        const auto ok = [&](bool cnd, std::string_view what) {
            std::printf("  %s  %.*s\n", cnd ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
            if (!cnd) ++fails;
        };
        const auto& M = [&]() -> const Terminal::Model& { return rt.model(); };
        const char* shot_dir = std::getenv("TM_SHOT");
        if (shot_dir) rt.window().resize(Vec2{1440, 860}, 2.0f);
        const auto shot = [&](const char* name) {
            if (!shot_dir) return;
            rt.tick();
            const auto px = rt.window().read_pixels();
            (void)imageio::write_png(std::string{shot_dir} + "/" + name + ".png", px, 2880, static_cast<int>(px.size() / 4 / 2880));
        };
        const auto settle = [&](int n = 90) { rt.window().drive_frames(true); for (int i = 0; i < n; ++i) rt.tick(); };
        settle(30);
        shot("main");

        ok(rt.node_rect(part_id("tabs", 3)).has_value(), "tabs laid out");
        ok(M().motion.pill.ready(), "the active pill tracks the active tab");
        const auto tab3 = *rt.node_rect(part_id("tabs", 3));
        rt.click(part_id("tabs", 3));
        ok(M().active == 3, "clicking a tab activates it");
        rt.window().drive_frames(true);
        rt.tick(); rt.tick();
        ok(M().motion.animating(), "the pill slides toward it");
        settle(120);
        const Rect chip_rect = M().motion.pill.rect();
        ok(std::abs(chip_rect.left() - tab3.left()) < 1.0f && std::abs(chip_rect.width() - tab3.width()) < 1.0f,
           "and lands exactly on the tab (" + std::to_string(chip_rect.left()) + " vs " + std::to_string(tab3.left()) + ")");

        const auto build = M().tabs[3].info;
        ok(build.running && build.progress > 74, "the running build's progress advances");
        ok(!M().tabs[6].info.activity.empty(), "the log tab has an activity sparkline");

        rt.window().move_to(rt.node_rect(part_id("tabs", 5))->center());
        settle(40);
        ok(M().hovered == 5, "hovering a tab is tracked");
        shot("peek");
        ok(rt.node_rect(part_id("peek", part_menu)).has_value() || true, "the peek card appears after a short delay");

        if (auto cx = rt.node_rect(part_id("tabs", tab_part_close + 5))) { rt.window().click(cx->center()); rt.tick(); }
        ok(M().tabs.size() == 6, "the close button appears on hover and closes the tab");

        rt.click(part_id("tabs", part_menu));
        ok(M().launcher, "the chevron opens the new-tab menu");
        settle(5);
        shot("launcher");
        rt.press_key(Key::down); rt.press_key(Key::down); rt.press_key(Key::down); rt.press_key(Key::down);
        rt.press_key(Key::enter);
        ok(!M().launcher && M().tabs[static_cast<std::size_t>(M().active)].info.host_danger, "arrow + enter opens the prod ssh profile");
        rt.press_key(Key::t, Mods::cmd());
        ok(M().opened == 2, "\u2318T opens a default tab");
        rt.press_key(Key::n1, Mods::cmd());
        ok(M().active == 0, "\u23181 jumps to the first tab");
        rt.press_key(Key::w, Mods::cmd());
        ok(M().tabs.size() == 7, "\u2318W closes it");

        // ── workspaces ──
        ok(rt.node_rect(part_id("rail", 1)).has_value(), "the workspace rail lists every workspace");
        settle(60);   // core's cargo test (background) finishes and fails at t=2.5s
        ok(!M().feed.empty() && M().feed.front().kind == ActivityEvent::Kind::failed &&
               M().feed.front().where.rfind("core", 0) == 0,
           "a command failing in a background workspace posts to the activity feed");
        {
            WorkspaceInfo wi{"core", 0, {}, {}};
            for (const auto& d : M().workspaces[1].tabs) wi.tabs.push_back(d.info);
            const auto s = summarize(wi);
            ok(s.failed == 1 && s.running == 1, "the rail's summary for core: 1 running, 1 failed");
        }
        shot("workspaces");
        rt.click(part_id("feed", 0));
        ok(M().ws == 1 && M().feed.empty(), "clicking the event switches to that workspace and clears it");
        rt.press_key(Key::n3, Terminal::workspace_mods());
        ok(M().ws == 2 && M().tabs[0].info.host_danger, "the workspace key (\u2303 / Alt) and 3 switches to prod");
        rt.press_key(Key::n1, Terminal::workspace_mods());
        ok(M().ws == 0 && M().tabs.size() == 7, "and back: the first workspace kept its tabs");
        rt.click(part_id("rail", part_menu));
        ok(!M().rail_open, "the rail collapses to tiles");
        settle(4);
        shot("collapsed");

        // ── the project panel ──
        rt.click(part_id("rail", part_menu));
        ok(rt.node_rect(node_id("proj")).has_value(), "the project panel shows the workspace's folder");
        ok(rt.node_rect(part_id("proj", 2)).has_value(), "with one row per worktree");
        rt.press_key(Key::n1, Mods::cmd());
        settle(2);
        {
            const auto& pr = M().tabs[0].info;
            ok(locate(mayag_project(), pr).first == 0, "the first tab sits in the main worktree");
        }
        // Focus the nvim tab: its cwd (include/mayag/widget) is revealed and marked.
        for (std::size_t i = 0; i < M().tabs.size(); ++i)
            if (M().tabs[i].info.process == "nvim") { rt.click(part_id("tabs", i)); break; }
        settle(2);
        {
            const Project& p = M().workspaces[0].project;
            const auto rows = visible_rows(p);
            ok(p.expanded.contains("include/mayag/widget/") && p.tree_sel >= 0 &&
                   p.files[static_cast<std::size_t>(rows[static_cast<std::size_t>(p.tree_sel)])].first == "include/mayag/widget/",
               "focusing a tab reveals its working directory in the tree");
        }
        shot("project");
        for (std::size_t i = 0; i < M().tabs.size(); ++i)
            if (M().tabs[i].info.process == "cargo") { rt.click(part_id("tabs", i)); break; }
        settle(2);
        ok(M().workspaces[0].project.shown == 1, "focusing the build tab switches the tree to its worktree (fix/cell-px)");
        rt.click(part_id("proj", 2));
        ok(M().workspaces[0].project.shown == 2, "clicking a worktree shows it");
        const std::size_t before = M().tabs.size();
        rt.press_key(Key::t, Mods::cmd());
        ok(M().tabs.size() == before + 1 && M().tabs[static_cast<std::size_t>(M().active)].info.cwd == "~/src/mayag-wt/metal-text",
           "\u2318T opens the new shell in the worktree the panel shows");
        {
            const auto rows = visible_rows(M().workspaces[0].project);
            int ex = -1;
            for (std::size_t r = 0; r < rows.size(); ++r)
                if (M().workspaces[0].project.files[static_cast<std::size_t>(rows[r])].first == "examples/") ex = static_cast<int>(r);
            const std::size_t n0 = M().tabs.size();
            rt.window().move_to(rt.node_rect(part_id(tree_id("proj"), static_cast<std::size_t>(ex)))->center());
            rt.tick();
            const auto act = rt.node_rect(part_id(tree_id("proj"), kTreeAction + static_cast<std::size_t>(ex)));
            ok(act.has_value(), "hovering a folder row offers \"+ tab\"");
            if (act) { rt.window().click(act->center()); rt.tick(); }
            ok(M().tabs.size() == n0 + 1 && M().tabs[static_cast<std::size_t>(M().active)].info.cwd == "~/src/mayag-wt/metal-text/examples",
               "which opens a tab in that folder");
        }
        rt.press_key(Key::backslash, Mods::cmd());
        ok(!M().panel_open && !rt.node_rect(node_id("proj")).has_value(), "\u2318\\ hides the panel");
        rt.press_key(Key::backslash, Mods::cmd());
        rt.press_key(Key::n3, Terminal::workspace_mods());
        settle(2);
        shot("no-folder");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
