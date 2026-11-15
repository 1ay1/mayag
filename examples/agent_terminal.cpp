// examples/agent_terminal.cpp — a terminal where running coding agents (and
// ssh) is a great experience: the widgets, together
//
// UI only. No pty, no ssh, no agent: every pane is a hand-built TermGrid
// showing what Claude Code / Codex / a shell / an ssh session would print,
// and the agent states are the ones the terminal would infer from their
// notifications. The point is the experience around them:
//
//   * tabs that show each agent's state (spinner, "your turn", "needs
//     approval") and each ssh tab's host, prod in red
//   * a workspaces sidebar: one git worktree (or remote box) per agent, its
//     state and its diff
//   * the attention queue: who is waiting on you, oldest first, ⌘J to jump
//   * agent toasts with Approve / Deny that act on a background pane
//   * the prompt editor: write a long message, send it as one paste (⌘⏎)
//   * pane overview (⌘.): every pane as a live thumbnail
//   * changes panel: review what an agent changed before committing
//   * ssh: host picker (⌘⇧S), connection bar (reconnecting), host-key
//     dialog, port forwards
//
//   ./mayag_agent_terminal              open a window
//   ./mayag_agent_terminal --headless   scripted, asserted
//   AT_SHOT=<dir> ./mayag_agent_terminal --headless

#include "harness.hpp"

#include <array>

using namespace mayag;
using namespace mayag::dsl;

namespace {

SshHost host(std::string alias, std::string user, std::string hostname, SshHost::Env env, std::vector<std::string> tags = {},
             std::string last = {}, bool connected = false, std::string jump = {}) {
    SshHost h;
    h.alias = std::move(alias); h.user = std::move(user); h.hostname = std::move(hostname); h.env = env;
    h.tags = std::move(tags); h.last = std::move(last); h.connected = connected; h.jump = std::move(jump);
    h.identity = "~/.ssh/id_ed25519";
    return h;
}

const std::vector<SshHost> kHosts{
    host("prod-db-01", "deploy", "10.0.4.21", SshHost::Env::prod, {"db", "postgres"}, "3h ago", true, "bastion"),
    host("gpu-box", "ayush", "gpu.lan", SshHost::Env::dev, {"cuda", "agents"}, "", true),
    host("staging-web", "deploy", "staging.mayag.dev", SshHost::Env::staging, {"web"}, "yesterday"),
    host("bastion", "ayush", "bastion.mayag.dev", SshHost::Env::none, {}, "3h ago"),
    host("pi", "pi", "raspberrypi.local", SshHost::Env::none, {"home"}, ""),
};

}  // namespace

struct AgentTerminal {
    struct Model {
        int    tab = 0;
        int    workspace = 0;
        float  ratio = 0.5f;
        std::vector<AttentionItem> queue;
        std::vector<AgentNote>     notes;
        bool   overview = false;
        bool   prompt = false;
        TextEditState draft;
        bool   hosts = false;
        TextEditState host_q;
        int    host_sel = 0;
        bool   hostkey = false;
        bool   changes = false;
        int    change_file = 0;
        bool   reconnecting = true;
        std::vector<PortForward> fwd{{PortForward::Kind::local, 5432, "localhost:5432", true, "", 2},
                                     {PortForward::Kind::local, 8080, "localhost:3000", true, "", 0},
                                     {PortForward::Kind::dynamic, 1080, "", false, "", 0}};
        double time = 0.0;
        std::vector<std::string> log;
    };

    struct Tab { int i; };              struct Workspace { int i; };       struct Jump { int i; };
    struct Dismiss { int i; };          struct Note { int i; int a; };     struct Overview {};
    struct OverviewPick { int i; };     struct Prompt {};                  struct PromptSend {};
    struct Typed { std::string s; };    struct Key_ { KeyEvent k; };       struct Hosts {};
    struct HostPick { int k; };         struct HostKey { int a; };         struct Changes {};
    struct ChangeFile { int i; };       struct ChangeAction { int k; };    struct Reconnect {};
    struct Fwd { int i; int k; };       struct SplitDrag { float dx; };    struct Close {};
    struct Tick { double dt; };         struct Quit {};

    using Msg = std::variant<Tab, Workspace, Jump, Dismiss, Note, Overview, OverviewPick, Prompt, PromptSend, Typed, Key_,
                             Hosts, HostPick, HostKey, Changes, ChangeFile, ChangeAction, Reconnect, Fwd, SplitDrag, Close, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
        m.queue = {
            {{"claude", AgentState::needs_approval, "Claude needs your permission to use Bash: rm -rf target/", 94}, "tab 2 \u00B7 fix/cell-px", 2},
            {{"codex", AgentState::needs_input, "Should the cache be per-user or global?", 410}, "tab 3 \u00B7 feat/cache", 3},
            {{"claude", AgentState::done, "Finished: refactored the tile scheduler, all tests pass", 25}, "gpu-box \u00B7 perf/tiles", 4},
        };
        sort_attention(m.queue);
        m.notes = {{{"claude", AgentState::needs_approval, "Claude needs your permission to use Bash: rm -rf target/", 94}, "tab 2 \u00B7 fix/cell-px"}};
        if (const char* s = std::getenv("AT_SHOW")) {
            const std::string_view v = s;
            m.overview = v == "overview"; m.prompt = v == "prompt"; m.hosts = v == "hosts"; m.hostkey = v == "hostkey"; m.changes = v == "changes";
        }
        return SetTitle{"mayag \u2014 agents"};
    }

    static Cmd update(Model& m, Tab e) { m.tab = e.i; return {}; }
    static Cmd update(Model& m, Workspace e) { m.workspace = e.i; return {}; }
    static Cmd update(Model& m, Jump e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.queue.size())) return {};
        m.tab = m.queue[static_cast<std::size_t>(e.i)].pane;
        m.log.push_back("jump-" + std::to_string(m.tab));
        m.queue.erase(m.queue.begin() + e.i);
        return {};
    }
    static Cmd update(Model& m, Dismiss e) {
        if (e.i >= 0 && e.i < static_cast<int>(m.queue.size())) m.queue.erase(m.queue.begin() + e.i);
        return {};
    }
    static Cmd update(Model& m, Note e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.notes.size())) return {};
        m.log.push_back("note-" + std::to_string(e.a));
        if (e.a == static_cast<int>(note_action::go)) m.tab = 2;
        // Approve/deny answer the agent in ITS pane (the app writes "y"/"n"
        // to that pty); the matching queue entry is resolved too.
        if (e.a == static_cast<int>(note_action::approve) || e.a == static_cast<int>(note_action::deny))
            std::erase_if(m.queue, [](const AttentionItem& q) { return q.agent.state == AgentState::needs_approval; });
        m.notes.erase(m.notes.begin() + e.i);
        return {};
    }
    static Cmd update(Model& m, Overview) { m.overview = !m.overview; return {}; }
    static Cmd update(Model& m, OverviewPick e) { m.overview = false; m.tab = e.i; return {}; }
    static Cmd update(Model& m, Prompt) { m.prompt = !m.prompt; m.draft.multiline = true; return {}; }
    static Cmd update(Model& m, PromptSend) {
        if (m.draft.text.empty()) return {};
        m.log.push_back("sent:" + m.draft.text);
        m.draft = TextEditState{};
        m.prompt = false;
        return {};
    }
    static Cmd update(Model& m, Typed e) {
        if (m.hosts) { m.host_q.insert(e.s); m.host_sel = 0; }
        else if (m.prompt) { m.draft.multiline = true; m.draft.insert(e.s); }
        return {};
    }
    static Cmd update(Model& m, Key_ e) {
        const auto& k = e.k;
        if (m.hostkey) {
            if (k.key == Key::escape) return update(m, HostKey{0});
            if (k.key == Key::enter) return update(m, HostKey{2});
            return {};
        }
        if (m.hosts) {
            const auto r = rank_hosts(kHosts, m.host_q.text);
            if (k.key == Key::escape) { m.hosts = false; return {}; }
            if (k.key == Key::down) { m.host_sel = std::min(m.host_sel + 1, static_cast<int>(r.size()) - 1); return {}; }
            if (k.key == Key::up) { m.host_sel = std::max(m.host_sel - 1, 0); return {}; }
            if (k.key == Key::enter) return update(m, HostPick{m.host_sel});
            m.host_q.handle_key(k.key, k.mods);
            return {};
        }
        if (m.prompt) {
            if (k.key == Key::escape) { m.prompt = false; return {}; }
            if (k.key == Key::enter && k.mods.primary()) return update(m, PromptSend{});
            if (k.key == Key::enter) { m.draft.insert("\n"); return {}; }
            m.draft.handle_key(k.key, k.mods);
            return {};
        }
        if (m.overview && k.key == Key::escape) { m.overview = false; return {}; }
        if (m.changes && k.key == Key::escape) { m.changes = false; return {}; }
        return {};
    }
    static Cmd update(Model& m, Hosts) { m.hosts = !m.hosts; m.host_q = {}; m.host_sel = 0; return {}; }
    static Cmd update(Model& m, HostPick e) {
        const auto r = rank_hosts(kHosts, m.host_q.text);
        if (e.k >= 0 && e.k < static_cast<int>(r.size())) {
            m.log.push_back("ssh:" + kHosts[static_cast<std::size_t>(r[static_cast<std::size_t>(e.k)])].alias);
            // First connection to a host: verify its key before anything else.
            const auto& hh = kHosts[static_cast<std::size_t>(r[static_cast<std::size_t>(e.k)])];
            if (!hh.connected && hh.last.empty()) m.hostkey = true;
        }
        m.hosts = false;
        return {};
    }
    static Cmd update(Model& m, HostKey e) { m.hostkey = false; m.log.push_back("hostkey-" + std::to_string(e.a)); return {}; }
    static Cmd update(Model& m, Changes) { m.changes = !m.changes; return {}; }
    static Cmd update(Model& m, ChangeFile e) { m.change_file = e.i; return {}; }
    static Cmd update(Model& m, ChangeAction e) { m.log.push_back("changes-" + std::to_string(e.k)); if (e.k == 0) m.changes = false; return {}; }
    static Cmd update(Model& m, Reconnect) { m.reconnecting = false; m.log.push_back("reconnect"); return {}; }
    static Cmd update(Model& m, Fwd e) {
        if (e.i < 0 || e.i >= static_cast<int>(m.fwd.size())) return {};
        if (e.k == 0) m.fwd[static_cast<std::size_t>(e.i)].active = !m.fwd[static_cast<std::size_t>(e.i)].active;
        if (e.k == 2) m.fwd.erase(m.fwd.begin() + e.i);
        return {};
    }
    static Cmd update(Model& m, SplitDrag e) { m.ratio = std::clamp(m.ratio + e.dx / 1000.0f, 0.2f, 0.8f); return {}; }
    static Cmd update(Model& m, Close) { m.overview = m.prompt = m.hosts = m.changes = false; return {}; }
    static Cmd update(Model& m, Tick e) { m.time += e.dt; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    [[nodiscard]] static Theme theme() {
        Theme t = make_theme(rgb<0x9D7CF7>, mayag::Mode::dark);
        t.background = rgb<0x0F0F14>;
        t.surface = rgb<0x16161D>;
        t.surface_raised = rgb<0x22222C>;
        t.overlay = rgb<0x1A1A22>;
        t.border = rgb<0x24242D>;
        t.border_strong = rgb<0x383845>;
        t.text_primary = rgb<0xE6E6EC>;
        t.text_secondary = rgb<0xA3A3B0>;
        t.text_disabled = rgb<0x60606E>;
        t.info = rgb<0x7DCFFF>;
        t.font_size = 12.5f;
        return t;
    }

    // ── pane contents (what the programs would have printed) ────────────

    [[nodiscard]] static TermGrid claude_pane() {
        auto g = term_grid({
            "{m}{B}\u273B Claude Code{0}  {d}~/src/mayag-wt/fix-cell-px{0}",
            "",
            "{d}>{0} fix the f32/u32 mismatch in render.rs and make the tests pass",
            "",
            "{m}\u23FA{0} I'll round at the call site so {c}cell_px{0} keeps its {c}u32{0} contract.",
            "",
            "{g}\u23FA{0} {B}Update{0}(src/render.rs)",
            "  {d}\u23BF{0}  Updated src/render.rs with {g}1 addition{0} and {r}1 removal{0}",
            "       {d}118{0} {r}-    let px = grid.cell_px(scale * 16.0);{0}",
            "       {d}118{0} {g}+    let px = grid.cell_px((scale * 16.0).round() as u32);{0}",
            "",
            "{g}\u23FA{0} {B}Bash{0}(cargo test --workspace)",
            "  {d}\u23BF{0}  test result: {g}ok{0}. 12 passed; 0 failed",
            "",
            "{y}\u256D\u2500 Bash command \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u256E{0}",
            "{y}\u2502{0}  rm -rf target/                                 {y}\u2502{0}",
            "{y}\u2502{0}  {d}Clean build artifacts before the release build{0} {y}\u2502{0}",
            "{y}\u2502{0}  Do you want to proceed?                        {y}\u2502{0}",
            "{y}\u2502{0}  {B}\u276F 1. Yes{0}                                       {y}\u2502{0}",
            "{y}\u2502{0}    2. Yes, and don't ask again for rm           {y}\u2502{0}",
            "{y}\u2502{0}    3. No, and tell Claude what to do differently {y}\u2502{0}",
            "{y}\u2570\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u256F{0}",
        }, 110);
        return g;
    }
    [[nodiscard]] static TermGrid shell_pane() {
        auto g = term_grid({
            "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ psql -c 'select count(*) from events'",
            "  count",
            "{d}---------{0}",
            " 48211907",
            "{d}(1 row){0}",
            "",
            "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ df -h /var/lib/postgresql",
            "Filesystem      Size  Used Avail Use% Mounted on",
            "/dev/nvme1n1    1.8T  1.4T  402G  {y}78%{0} /var/lib/postgresql",
            "",
            "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ ",
        }, 90, true);
        g.cursor_row = 10; g.cursor_col = 22;
        return g;
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        Ctx look = c;
        look.theme = theme();
        const Theme& t = look.theme;
        TermTheme tt; tt.bg = 0x0F0F14; tt.fg = 0xE6E6EC;

        // ── tabs ────────────────────────────────────────────────────────
        std::vector<TermTab> tabs(5);
        tabs[0].title = "~/src/mayag"; tabs[0].process = "zsh";
        tabs[1].title = "prod-db-01"; tabs[1].process = "ssh"; tabs[1].host = kHosts[0];
        tabs[2].title = "fix/cell-px"; tabs[2].process = "claude";
        tabs[2].agent = {"claude", AgentState::needs_approval, "", 94};
        tabs[3].title = "feat/cache"; tabs[3].process = "codex"; tabs[3].agent = {"codex", AgentState::needs_input, "", 410};
        tabs[4].title = "perf/tiles"; tabs[4].process = "claude"; tabs[4].host = kHosts[1];
        tabs[4].agent = {"claude", AgentState::working, "", 25};
        tabs[4].progress = 64;

        // ── sidebar: workspaces + attention ─────────────────────────────
        std::vector<Worktree> trees(4);
        trees[0].branch = "main"; trees[0].path = "~/src/mayag";
        trees[1].branch = "fix/cell-px"; trees[1].path = "~/src/mayag-wt/fix-cell-px";
        trees[1].agent = {"claude", AgentState::needs_approval, "", 94}; trees[1].added = 1; trees[1].removed = 1; trees[1].files = 1; trees[1].ahead = 1;
        trees[2].branch = "feat/cache"; trees[2].path = "~/src/mayag-wt/feat-cache";
        trees[2].agent = {"codex", AgentState::needs_input, "", 410}; trees[2].added = 214; trees[2].removed = 37; trees[2].files = 6;
        trees[2].checks = Worktree::Checks::pending; trees[2].pr = "#131";
        trees[3].branch = "perf/tiles"; trees[3].host = kHosts[1];
        trees[3].agent = {"claude", AgentState::working, "", 25}; trees[3].added = 88; trees[3].removed = 120; trees[3].files = 4;
        trees[3].checks = Worktree::Checks::passing;
        auto side = v(node(worktree_list(look, "trees", trees, m.workspace, m.time)),
                      box() | height(1) | width(pct(100)) | bg(t.border),
                      node(attention_queue(look, "queue", m.queue, m.time)),
                      spacer(),
                      v(text<"FORWARDS \u00B7 prod-db-01"> | font(t.font_size * 0.72f) | semibold | tracking(0.8f) | fg(t.text_secondary),
                        node(port_forwards(look, "fwd", m.fwd)))
                          | gap(6) | width(pct(100)))
                  | gap(14) | pad(12, 10) | width(300) | height(pct(100)) | bg(t.surface) | border(1, t.border);

        // ── panes: claude (local worktree) | ssh to prod ────────────────
        Node left = pane_frame(look, "pane-l", {"claude \u2014 fix/cell-px", true, false, true, false, 0, false},
                               (v(h(node(agent_badge(look, {"claude", AgentState::needs_approval, "", 94}, m.time))) | pad(6, 8),
                                  v(node(terminal_view(look, "term-l", claude_pane(), tt, {.font_size = 12.5f, .focused = true, .marks = false})))
                                      | grow() | min_size(0, 0))
                                | grow() | width(pct(100)) | height(pct(100))).build());
        SshConnection conn{m.reconnecting ? SshConnection::State::reconnecting : SshConnection::State::connected, "prod-db-01",
                           "Connection reset by peer \u2014 scrollback kept", 38, 3, 2, false};
        Node right = pane_frame(look, "pane-r", {"ssh \u2014 deploy@prod-db-01", false, false, true, false, 0, false},
                                (v(h(node(host_badge(look, kHosts[0]))) | pad(6, 8),
                                   node(connection_bar(look, "conn", conn, m.time)),
                                   v(node(terminal_view(look, "term-r", shell_pane(), tt, {.font_size = 12.5f, .focused = false, .dim_unfocused = 0.4f, .marks = false})))
                                       | grow() | min_size(0, 0))
                                 | grow() | width(pct(100)) | height(pct(100))
                                 | border(m.reconnecting ? 0.0f : 0.0f, t.danger)).build());

        // ── overlays ────────────────────────────────────────────────────
        agent_toasts(look, "notes", m.notes, c.size, m.time);
        if (m.prompt)
            prompt_editor(look, "prompt", m.draft, {"claude \u00B7 tab 3 \u00B7 feat/cache", {"src/cache.rs", "docs/cache.md"}, false, 0,
                                                    std::fmod(m.time, 1.06) < 0.53, 0});
        if (m.hosts) ssh_hosts(look, "hosts", m.host_q, kHosts, rank_hosts(kHosts, m.host_q.text), m.host_sel);
        if (m.hostkey)
            host_key_dialog(look, "hostkey", {"pi", "192.168.1.44", "ED25519", "SHA256:9xQm3vR8p0Z2yKf7tLw1bN4cH6dJ5sA8eU2gV0iO3qE", false, "", 0});
        if (m.overview) {
            std::vector<PaneCard> cards;
            cards.push_back({"claude \u2014 fix/cell-px", {"claude", AgentState::needs_approval, "needs permission: rm -rf target/", 94}, std::nullopt,
                             terminal_view(look, "ov0", term_grid({"{y}\u2502{0}  rm -rf target/", "{y}\u2502{0}  Do you want to proceed?",
                                                                   "{y}\u2502{0}  {B}\u276F 1. Yes{0}", "{y}\u2502{0}    2. Yes, don't ask again"}, 60),
                                           tt, {.font_size = 11.0f, .focused = false, .marks = false})});
            cards.push_back({"codex \u2014 feat/cache", {"codex", AgentState::needs_input, "Should the cache be per-user or global?", 410}, std::nullopt,
                             terminal_view(look, "ov1", term_grid({"{c}codex{0} Added CacheKey and an LRU with 1024 entries.",
                                                                   "Question: should the cache be per-user or global?", "{d}\u203A{0} "}, 60),
                                           tt, {.font_size = 11.0f, .focused = false, .marks = false})});
            cards.push_back({"claude \u2014 perf/tiles", {"claude", AgentState::working, "", 25}, kHosts[1],
                             terminal_view(look, "ov2", term_grid({"{g}\u23FA{0} Bash(cargo bench --bench tiles)", "  {d}\u23BF{0}  Running...",
                                                                   "{m}\u273B{0} Benchmarking\u2026 (31s \u00B7 \u2191 4.1k tokens)"}, 60),
                                           tt, {.font_size = 11.0f, .focused = false, .marks = false})});
            cards.push_back({"ssh \u2014 prod-db-01", {}, kHosts[0],
                             terminal_view(look, "ov3", term_grid({" 48211907", "{d}(1 row){0}", "{b}{B}deploy@prod-db-01{0}:{c}~{0}$ "}, 60),
                                           tt, {.font_size = 11.0f, .focused = false, .marks = false})});
            cards.push_back({"zsh \u2014 ~/src/mayag", {}, std::nullopt,
                             terminal_view(look, "ov4", term_grid({"{b}{B}~/src/mayag{0} {m}main{0} {g}\u276F{0} git status -sb", "## main...origin/main"}, 60),
                                           tt, {.font_size = 11.0f, .focused = false, .marks = false})});
            std::stable_sort(cards.begin(), cards.end(), [](const PaneCard& a, const PaneCard& b) {
                return wants_attention(a.agent.state) > wants_attention(b.agent.state); });
            pane_overview(look, "overview", std::move(cards), m.time);
        }

        Node main_area;
        if (m.changes) {
            const std::vector<ChangedFile> files{{"src/cache.rs", 'A', 168, 0}, {"src/lib.rs", 'M', 4, 1}, {"src/render.rs", 'M', 22, 31},
                                                 {"tests/cache.rs", 'A', 20, 0}, {"Cargo.toml", 'M', 1, 0}, {"src/old_cache.rs", 'D', 0, 5}};
            const std::vector<DiffLine> diff{
                {DiffLine::Op::same, "use crate::grid::Grid;", 1, 1},
                {DiffLine::Op::add, "use crate::cache::{CacheKey, Lru};", 0, 2},
                {DiffLine::Op::same, "", 2, 3},
                {DiffLine::Op::same, "pub struct Renderer {", 3, 4},
                {DiffLine::Op::del, "    glyphs: HashMap<u32, Glyph>,", 4, 0},
                {DiffLine::Op::add, "    glyphs: Lru<CacheKey, Glyph>,", 0, 5},
                {DiffLine::Op::same, "    atlas: Atlas,", 5, 6},
                {DiffLine::Op::same, "}", 6, 7},
            };
            main_area = changes_panel(look, "changes", "feat/cache \u2190 codex", files, m.change_file, diff);
        } else {
            main_area = pane_split(look, "split", Axis::horizontal, m.ratio, std::move(left), std::move(right));
        }

        std::vector<StatusItem> sl{{"3 agents", "\u2726", StatusItem::Tone::accent, false},
                                   {std::to_string(m.queue.size()) + " waiting", "\u25C6", m.queue.empty() ? StatusItem::Tone::dim : StatusItem::Tone::warning, false},
                                   {"2 ssh", "\u21C4", StatusItem::Tone::normal, false}};
        std::vector<StatusItem> sr{{"\u2318J next", "", StatusItem::Tone::dim, false}, {"\u2318. overview", "", StatusItem::Tone::dim, false},
                                   {"\u2318\u21E7E prompt", "", StatusItem::Tone::dim, false}, {"\u2318\u21E7S ssh", "", StatusItem::Tone::dim, false}};

        return v(node(term_tabs(look, "tabs", tabs, m.tab, m.time)),
                 h(std::move(side), v(node(std::move(main_area))) | grow() | height(pct(100)) | min_size(0, 0))
                     | grow() | width(pct(100)) | min_size(0, 0),
                 node(status_bar(look, "status", sl, sr)))
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    // ── input ───────────────────────────────────────────────────────────

    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        for (std::size_t i = 0; i < 5; ++i) if (is(c, "tabs", i)) return Tab{static_cast<int>(i)};
        for (std::size_t i = 0; i < 4; ++i) {
            if (is(c, "trees", 4096 + i)) return Changes{};
            if (is(c, "trees", i)) return Workspace{static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < m.queue.size(); ++i) {
            if (is(c, "queue", 4096 + i)) return Dismiss{static_cast<int>(i)};
            if (is(c, "queue", i)) return Jump{static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < m.notes.size(); ++i)
            for (std::size_t a = 0; a < 4; ++a) if (is(c, "notes", i * 4 + a)) return Note{static_cast<int>(i), static_cast<int>(a)};
        for (std::size_t i = 0; i < 5; ++i) if (is(c, "overview", i)) return OverviewPick{static_cast<int>(i)};
        if (is(c, "prompt", 1)) return PromptSend{};
        for (std::size_t k = 0; k < 10; ++k) if (is(c, "hosts", k)) return HostPick{static_cast<int>(k)};
        for (std::size_t a = 0; a < 3; ++a) if (is(c, "hostkey", a)) return HostKey{static_cast<int>(a)};
        for (std::size_t i = 0; i < 6; ++i) if (is(c, "changes", i)) return ChangeFile{static_cast<int>(i)};
        for (std::size_t k = 0; k < 4; ++k) if (is(c, "changes", 4096 + k)) return ChangeAction{static_cast<int>(k)};
        if (is(c, "conn", 0)) return Reconnect{};
        for (std::size_t i = 0; i < m.fwd.size(); ++i)
            for (std::size_t k = 0; k < 3; ++k) if (is(c, "fwd", i * 4 + k)) return Fwd{static_cast<int>(i), static_cast<int>(k)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "split", part_bar)) return SplitDrag{d.delta.x};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Left& l) {
        if (m.overview && is(l, "overview", part_menu)) return Overview{};
        if (m.hosts && is(l, "hosts", part_menu)) return Hosts{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const TextEvent& e) {
        if (m.hosts || m.prompt) return Typed{e.text};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        const bool cmd = k.mods.primary();
        if (cmd && k.key == Key::w) return Quit{};
        if (cmd && k.key == Key::j && !m.queue.empty()) return Jump{0};
        if (cmd && k.key == Key::period) return Overview{};
        if (cmd && k.mods.shift && k.key == Key::e) return Prompt{};
        if (cmd && k.mods.shift && k.key == Key::s) return Hosts{};
        if (cmd && k.mods.shift && k.key == Key::d) return Changes{};
        if (!m.notes.empty() && !m.prompt && !m.hosts && k.mods.none()) {
            if (k.key == Key::y) return Note{0, static_cast<int>(note_action::approve)};
            if (k.key == Key::n) return Note{0, static_cast<int>(note_action::deny)};
        }
        if (cmd && k.key >= Key::n1 && k.key <= Key::n5) return Tab{static_cast<int>(k.key) - static_cast<int>(Key::n1)};
        return Key_{k};
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // Spinners turn only while some agent works or a connection retries.
    static Sub subscribe(const Model&) { return Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }); }
};

static_assert(mayag::Program<AgentTerminal>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1480, 860});
    std::shared_ptr<typo::FontStack> fonts;
    if (opts.use_system_fonts) {
        static constexpr std::array<std::string_view, 4> mono{"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono"};
        fonts = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400, false, mono);
    } else {
        fonts = demo::make_fonts(opts);
    }
    AppConfig cfg{.title = "mayag \u2014 agents", .size = opts.size, .theme = AgentTerminal::theme(), .fonts = fonts,
                  .debug_bounds = opts.debug_bounds};
    using AT = AgentTerminal;
    return demo::run<AT>(opts, cfg, [](auto& rt) {
        std::printf("agent terminal headless\n");
        int fails = 0;
        const auto ok = [&](bool cnd, std::string_view what) {
            std::printf("  %s  %.*s\n", cnd ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
            if (!cnd) ++fails;
        };
        const auto& M = [&]() -> const AT::Model& { return rt.model(); };
        const char* shot_dir = std::getenv("AT_SHOT");
        if (shot_dir) rt.window().resize(Vec2{1480, 860}, 2.0f);
        const auto shot = [&](const char* name) {
            if (!shot_dir) return;
            rt.tick();
            const auto px = rt.window().read_pixels();
            (void)imageio::write_png(std::string{shot_dir} + "/" + name + ".png", px, 2960, static_cast<int>(px.size() / 4 / 2960));
        };
        const Mods cmd{.super = true}, cmd_sh{.shift = true, .super = true};
        rt.tick(); rt.tick();
        shot("main");

        ok(rt.node_rect("term-l").has_value() && rt.node_rect("term-r").has_value(), "agent pane and ssh pane laid out");
        ok(M().queue.size() == 3 && M().queue[0].agent.state == AgentState::needs_approval, "the queue puts the approval first");
        rt.press_key(Key::j, cmd);
        ok(M().tab == 2 && M().queue.size() == 2, "\u2318J jumps to the agent that needs approval");
        rt.click(part_id("queue", 4096 + 0));
        ok(M().queue.size() == 1 || M().queue.size() == 2, "a queue entry can be dismissed (when hovered)");

        rt.press_key(Key::y);
        ok(M().notes.empty() && M().log.back() == "note-1", "y on the toast approves the background agent");

        rt.click(part_id("trees", 4096 + 2));
        ok(M().changes, "clicking a workspace's diff opens the changes panel");
        shot("changes");
        rt.click(part_id("changes", 2));
        ok(M().change_file == 2, "selecting a changed file");
        rt.click(part_id("changes", 4096 + changes_action::commit));
        ok(!M().changes && M().log.back() == "changes-0", "Commit closes the review");

        rt.press_key(Key::e, cmd_sh);
        rt.type("Make the cache per-user.");
        rt.press_key(Key::enter);
        rt.type("Keep the global one for tests.");
        ok(M().prompt && M().draft.text == "Make the cache per-user.\nKeep the global one for tests.", "the prompt editor takes multi-line input");
        shot("prompt");
        rt.press_key(Key::enter, cmd);
        ok(!M().prompt && M().log.back().rfind("sent:Make", 0) == 0, "\u2318\u23CE sends it as one message");

        rt.press_key(Key::period, cmd);
        ok(M().overview, "\u2318. opens the pane overview");
        shot("overview");
        rt.click(part_id("overview", 1));
        ok(!M().overview, "clicking a card opens that pane");

        rt.press_key(Key::s, cmd_sh);
        rt.type("gpu");
        ok(M().hosts && AT::Model{}.host_q.text.empty() && rank_hosts(kHosts, M().host_q.text).front() == 1, "the host picker fuzzy-finds gpu-box");
        shot("hosts");
        rt.press_key(Key::enter);
        ok(M().log.back() == "ssh:gpu-box" && !M().hostkey, "enter connects to a known host");
        rt.press_key(Key::s, cmd_sh);
        rt.type("pi");
        rt.press_key(Key::enter);
        ok(M().hostkey, "a first connection asks to verify the host key");
        shot("hostkey");
        rt.press_key(Key::enter);
        ok(!M().hostkey && M().log.back() == "hostkey-2", "enter trusts and saves it");

        if (auto r = rt.node_rect(part_id("conn", 0))) { rt.window().click(r->center()); rt.tick(); }
        ok(!M().reconnecting && M().log.back() == "reconnect", "Reconnect on the connection bar");
        rt.click(part_id("fwd", 0 * 4 + 0));
        ok(!M().fwd[0].active, "a port forward toggles off");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
