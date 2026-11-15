// examples/terminal_widgets.cpp — the terminal-emulator widgets, together
//
// UI only. There is no emulator or pty here: a TermGrid is filled by hand
// with styled cells (the shape an emulator's snapshot would have), so the
// widgets can be seen and driven in isolation.
//
//   ./mayag_terminal_widgets              open a window
//   ./mayag_terminal_widgets --png o.png  one frame (TW_SHOW=menu|paste|hints|search)
//   ./mayag_terminal_widgets --headless   scripted, asserted
//
// On screen: term_tabs (process, activity, exit status, progress), two
// panes in a pane_split (one with command_blocks, one a full terminal_view
// with selection, search matches, hyperlink, wide chars, every underline
// style), scrollback_bar with prompt/error/match marks, term_search,
// context_menu, confirm_dialog (paste protection), resize_hud,
// link_tooltip, quick-select hint labels.

#include "harness.hpp"

#include <array>

using namespace mayag;
using namespace mayag::dsl;

namespace {

/// Build TermCells from a tiny markup: {k}=default {r}{g}{y}{b}{m}{c}{w}{d}
/// colours, {B} bold {I} italic {U} underline {~} curly {D} dim {S} strike
/// {R} reverse {L} hyperlink 1 {0} reset.
struct Ansi {
    std::array<std::uint32_t, 8> pal{0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5};
};
[[nodiscard]] TermRowView row_of(std::string_view markup, int cols, TermMark mark = TermMark::none) {
    static const Ansi a;
    TermRowView r;
    r.mark = mark;
    TermCell pen;
    for (std::size_t i = 0; i < markup.size();) {
        if (markup[i] == '{' && i + 2 < markup.size() && markup[i + 2] == '}') {
            const char k = markup[i + 1];
            i += 3;
            switch (k) {
                case '0': pen = TermCell{}; break;
                case 'k': pen.default_fg = true; break;
                case 'r': pen.fg = a.pal[1]; pen.default_fg = false; break;
                case 'g': pen.fg = a.pal[2]; pen.default_fg = false; break;
                case 'y': pen.fg = a.pal[3]; pen.default_fg = false; break;
                case 'b': pen.fg = a.pal[4]; pen.default_fg = false; break;
                case 'm': pen.fg = a.pal[5]; pen.default_fg = false; break;
                case 'c': pen.fg = a.pal[6]; pen.default_fg = false; break;
                case 'd': pen.fg = 0x565F89; pen.default_fg = false; break;
                case 'B': pen.flags |= term_attr::bold; break;
                case 'I': pen.flags |= term_attr::italic; break;
                case 'D': pen.flags |= term_attr::dim; break;
                case 'S': pen.flags |= term_attr::strike; break;
                case 'R': pen.flags |= term_attr::inverse; break;
                case 'U': pen.ul_style = TermUnderline::single; break;
                case '~': pen.ul_style = TermUnderline::curly; pen.ul_color = 0xF7768E; break;
                case '=': pen.ul_style = TermUnderline::double_; break;
                case '.': pen.ul_style = TermUnderline::dotted; break;
                case '-': pen.ul_style = TermUnderline::dashed; break;
                case 'L': pen.link = 1; break;
                case 'G': pen.bg = 0x2D3F2D; pen.default_bg = false; break;
                default: break;
            }
            continue;
        }
        // one UTF-8 character
        std::size_t n = 1;
        const auto c = static_cast<unsigned char>(markup[i]);
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        TermCell cell = pen;
        cell.text = std::string{markup.substr(i, n)};
        char32_t cp = c;
        if (n == 2) cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(markup[i + 1]) & 0x3Fu);
        if (n == 3) cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(markup[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(markup[i + 2]) & 0x3Fu);
        if (n == 4) cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(markup[i + 1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(markup[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(markup[i + 3]) & 0x3Fu);
        const bool wide = (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                          (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1FAFF);
        cell.width = wide ? 2 : 1;
        r.cells.push_back(cell);
        if (wide) { TermCell right = pen; right.text = ""; right.width = 0; r.cells.push_back(right); }
        i += n;
    }
    while (static_cast<int>(r.cells.size()) < cols) r.cells.push_back(TermCell{});
    r.cells.resize(static_cast<std::size_t>(cols));
    return r;
}

}  // namespace

struct TerminalWidgets {
    struct Model {
        int   active_tab = 0;
        int   focus = 1;               // 0 left pane, 1 right pane
        float ratio = 0.42f;
        int   selected_block = 1;
        bool  collapsed0 = false;
        bool  search = false;
        TextEditState query;
        bool  case_sensitive = false, regex = false;
        int   match = 0;
        std::optional<Vec2> menu_at;
        bool  paste_confirm = false, paste_remember = false;
        bool  hints = false;
        std::string hint_typed;
        bool  resizing = false;
        Vec2  mouse{};
        bool  hover_link = false;
        std::vector<std::string> log;   // what was clicked, for the headless script
        int   closed_tabs = 0, new_tabs = 0;
    };

    struct TabClick { int i; };   struct TabClose { int i; };   struct NewTab {};
    struct Focus { int p; };      struct SplitDrag { float dx; };
    struct BlockClick { int i; }; struct BlockAction { int i; int a; };
    struct OpenSearch {};         struct SearchKey { KeyEvent k; };  struct SearchPart { std::size_t p; };
    struct Typed { std::string s; };
    struct Menu { Vec2 at; };     struct MenuPick { int i; };        struct MenuClose {};
    struct Paste {};              struct PasteAnswer { int a; };
    struct Hints {};              struct HintKey { std::string s; };
    struct Resize {};             struct Mouse { Vec2 p; };          struct Quit {};

    using Msg = std::variant<TabClick, TabClose, NewTab, Focus, SplitDrag, BlockClick, BlockAction, OpenSearch,
                             SearchKey, SearchPart, Typed, Menu, MenuPick, MenuClose, Paste, PasteAnswer, Hints,
                             HintKey, Resize, Mouse, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m) {
        if (const char* s = std::getenv("TW_SHOW")) {
            const std::string_view v = s;
            if (v == "menu") m.menu_at = Vec2{900, 300};
            if (v == "paste") m.paste_confirm = true;
            if (v == "hints") m.hints = true;
            if (v == "search") { m.search = true; m.query.text = "error"; }
            if (v == "resize") m.resizing = true;
        }
        return SetTitle{"mayag terminal widgets"};
    }

    // The right pane's content: a hand-built screen.
    [[nodiscard]] static TermGrid right_grid(const Model& m) {
        TermGrid g;
        g.cols = 96; g.rows = 22;
        const std::vector<std::pair<std::string, TermMark>> rows{
            {"{b}{B}~/src/mayag{0} {m}main{0} {g}❯{0} cargo build --release", TermMark::ok},
            {"{g}{B}   Compiling{0} serde v1.0.210", TermMark::none},
            {"{g}{B}   Compiling{0} mayag-term v0.4.2 ({c}{U}{L}~/src/mayag{0})", TermMark::none},
            {"{y}{B}warning{0}{B}: unused variable: `cols`{0}", TermMark::none},
            {"  {b}-->{0} src/grid.rs:42:9", TermMark::none},
            {"{r}{B}error[E0308]{0}{B}: mismatched types{0}", TermMark::none},
            {"  {b}-->{0} src/render.rs:118:20   {~}expected `u32`, found `f32`{0}", TermMark::none},
            {"{r}{B}error{0}: could not compile `mayag-term` due to 1 previous error", TermMark::none},
            {"{b}{B}~/src/mayag{0} {m}main{0} {r}❯{0} ls -la", TermMark::error},
            {"{d}drwxr-xr-x{0}  12 ayush  staff   384 Sep  4 14:02 {b}{B}src{0}", TermMark::none},
            {"{d}-rw-r--r--{0}   1 ayush  staff  2.1K Sep  4 13:50 Cargo.toml", TermMark::none},
            {"{d}-rwxr-xr-x{0}   1 ayush  staff  8.4M Sep  4 14:03 {g}{B}mayag-term{0}", TermMark::none},
            {"{b}{B}~/src/mayag{0} {m}main{0} {g}❯{0} echo 'styles'", TermMark::ok},
            {"{B}bold{0} {D}dim{0} {I}italic{0} {U}under{0} {=}double{0} {.}dotted{0} {-}dashed{0} {~}curly{0} {S}strike{0} {R} inverse {0}", TermMark::none},
            {"wide: 終端エミュレータ  한국어  ⇒  →  λ  ✓  ✗", TermMark::none},
            {"{b}{B}~/src/mayag{0} {m}main{0} {g}❯{0} git log --oneline -3", TermMark::ok},
            {"{y}e0c69d9{0} editor: the widgets a Zed-class code editor needs", TermMark::none},
            {"{y}1cc0008{0} terminal settings: from a toy to a tool", TermMark::none},
            {"{y}303fe5d{0} text: optical sizing, so SF is drawn with its text design", TermMark::none},
            {"{b}{B}~/src/mayag{0} {m}main{0} {g}❯{0} ", TermMark::prompt},
            {"", TermMark::none},
            {"", TermMark::none},
        };
        for (const auto& [s, mk] : rows) g.lines.push_back(row_of(s, g.cols, mk));
        g.cursor_row = 19; g.cursor_col = 20;
        g.cursor_shape = TermCursorShape::block;
        g.selection = {{16, 0, 7}, {17, 0, 7}};
        if (m.search) {
            for (int r = 0; r < g.rows; ++r) {
                std::string line;
                for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) line += c.text.empty() ? "" : (c.text.size() == 1 ? c.text : "?");
                for (auto at = line.find("error"); at != std::string::npos && !m.query.text.empty(); at = line.find("error", at + 5))
                    g.matches.push_back({r, static_cast<int>(at), 5});
            }
            g.current_match = std::min(m.match, static_cast<int>(g.matches.size()) - 1);
        }
        if (m.hints) {
            g.hints = {{{2, 39, 11}, "a"}, {{4, 6, 17}, "s"}, {{6, 6, 20}, "d"}, {{16, 0, 7}, "f"}, {{17, 0, 7}, "j"}, {{18, 0, 7}, "k"}};
            g.hint_typed = m.hint_typed;
        }
        if (m.hover_link) g.hover_link = 1;
        return g;
    }
    [[nodiscard]] static TermGrid block_grid(std::initializer_list<std::string_view> lines, int cols) {
        TermGrid g;
        g.cols = cols; g.rows = static_cast<int>(lines.size());
        for (auto l : lines) g.lines.push_back(row_of(l, cols));
        g.cursor_visible = false;
        return g;
    }

    static Cmd update(Model& m, TabClick e) { m.active_tab = e.i; m.log.push_back("tab"); return {}; }
    static Cmd update(Model& m, TabClose) { ++m.closed_tabs; return {}; }
    static Cmd update(Model& m, NewTab) { ++m.new_tabs; return {}; }
    static Cmd update(Model& m, Focus e) { m.focus = e.p; return {}; }
    static Cmd update(Model& m, SplitDrag e) { m.ratio = std::clamp(m.ratio + e.dx / 1300.0f, 0.15f, 0.85f); return {}; }
    static Cmd update(Model& m, BlockClick e) { m.selected_block = e.i; m.focus = 0; return {}; }
    static Cmd update(Model& m, BlockAction e) {
        m.selected_block = e.i;
        if (e.a == static_cast<int>(block_action::collapse) && e.i == 0) m.collapsed0 = !m.collapsed0;
        m.log.push_back("block-action-" + std::to_string(e.a));
        return {};
    }
    static Cmd update(Model& m, OpenSearch) { m.search = true; m.query.select_all(); return {}; }
    static Cmd update(Model& m, SearchKey e) {
        if (e.k.key == Key::escape) { m.search = false; return {}; }
        if (e.k.key == Key::enter) { m.match += e.k.mods.shift ? -1 : 1; return {}; }
        m.query.handle_key(e.k.key, e.k.mods);
        return {};
    }
    static Cmd update(Model& m, SearchPart e) {
        switch (e.p) {
            case 2: m.case_sensitive = !m.case_sensitive; break;
            case 4: m.regex = !m.regex; break;
            case 5: --m.match; break;
            case 6: ++m.match; break;
            case 9: m.search = false; break;
            default: break;
        }
        if (m.match < 0) m.match = 0;
        return {};
    }
    static Cmd update(Model& m, Typed e) {
        if (m.hints) { m.hint_typed += e.s; return update(m, HintKey{e.s}); }
        if (m.search) m.query.insert(e.s);
        return {};
    }
    static Cmd update(Model& m, Menu e) { m.menu_at = e.at; return {}; }
    static Cmd update(Model& m, MenuPick e) {
        m.menu_at.reset();
        m.log.push_back("menu-" + std::to_string(e.i));
        if (e.i == 1) m.paste_confirm = true;
        if (e.i == 5) m.search = true;
        return {};
    }
    static Cmd update(Model& m, MenuClose) { m.menu_at.reset(); return {}; }
    static Cmd update(Model& m, Paste) { m.paste_confirm = true; return {}; }
    static Cmd update(Model& m, PasteAnswer e) {
        if (e.a == 2) { m.paste_remember = !m.paste_remember; return {}; }
        m.paste_confirm = false;
        m.log.push_back(e.a == 1 ? "pasted" : "paste-cancelled");
        return {};
    }
    static Cmd update(Model& m, Hints) { m.hints = !m.hints; m.hint_typed.clear(); return {}; }
    static Cmd update(Model& m, HintKey) {
        static constexpr std::array<std::string_view, 6> labels{"a", "s", "d", "f", "j", "k"};
        for (auto l : labels) if (l == m.hint_typed) { m.log.push_back("hint-" + std::string{l}); m.hints = false; m.hint_typed.clear(); }
        return {};
    }
    static Cmd update(Model& m, Resize) { m.resizing = !m.resizing; return {}; }
    static Cmd update(Model& m, Mouse e) { m.mouse = e.p; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    [[nodiscard]] static Theme theme() {
        Theme t = make_theme(rgb<0x7AA2F7>, mayag::Mode::dark);
        t.background = rgb<0x16161E>;
        t.surface = rgb<0x1A1B26>;
        t.surface_raised = rgb<0x292E42>;
        t.overlay = rgb<0x1F2335>;
        t.border = rgb<0x292E42>;
        t.border_strong = rgb<0x3B4261>;
        t.text_primary = rgb<0xC0CAF5>;
        t.text_secondary = rgb<0x9AA5CE>;
        t.text_disabled = rgb<0x565F89>;
        t.font_size = 12.5f;
        return t;
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        Ctx look = c;
        look.theme = theme();
        const Theme& t = look.theme;
        TermTheme tt;
        tt.bg = 0x1A1B26;

        // ── tabs ────────────────────────────────────────────────────────
        const std::vector<TermTab> tabs{
            {"~/src/mayag", "cargo", false, false, 101, -1, false, false, false, 0},
            {"~/src/mayag", "zsh", true, false, 0, 62, false, true, false, 0},
            {"prod-db-01", "ssh", false, true, -1, -1, false, false, false, 0xF7768E},
            {"htop", "htop", false, false, -1, -1, false, true, false, 0},
        };

        // ── left pane: command blocks ───────────────────────────────────
        TermViewOptions bo{.font_size = 12.5f, .line_height = 1.3f, .padding = 2.0f, .focused = false, .marks = false};
        auto b0 = block_grid({"{g}{B}   Compiling{0} serde v1.0.210", "{g}{B}    Finished{0} release [optimized] in 41.2s"}, 60);
        auto b1 = block_grid({"{r}{B}error[E0308]{0}{B}: mismatched types{0}", "  {b}-->{0} src/render.rs:118:20",
                              "  {~}expected `u32`, found `f32`{0}"}, 60);
        auto b2 = block_grid({"{y}e0c69d9{0} editor: widgets for a Zed-class editor", "{y}1cc0008{0} terminal settings"}, 60);
        const auto block_view = [&](const TermGrid& g) {
            return (v(node(terminal_view(look, "blockterm", g, tt, bo))) | height(static_cast<float>(g.rows) * 16.0f + 6.0f) | width(pct(100))).build();
        };
        std::vector<Node> blocks;
        blocks.push_back(command_block(look, "block", 0, {"~/src/mayag", "main", "cargo build --release", 0, 41.2, "14:01:30", m.selected_block == 0, m.collapsed0, 2}, block_view(b0)));
        blocks.push_back(command_block(look, "block", 1, {"~/src/mayag", "main", "cargo test --workspace", 101, 3.9, "14:02:11", m.selected_block == 1, false, 3}, block_view(b1)));
        blocks.push_back(command_block(look, "block", 2, {"~/src/mayag", "main", "git log --oneline -2", 0, 0.012, "14:02:40", m.selected_block == 2, false, 2}, block_view(b2)));
        blocks.push_back(command_block(look, "block", 3, {"~/src/mayag", "main", "cargo watch -x check", -1, 12.0, "14:03:02", m.selected_block == 3, false, 0},
                                       (text<"[Running] cargo check"> | font(12) | fg(t.text_secondary)).build()));
        Node left = pane_frame(look, "pane-l", {"blocks \u2014 ~/src/mayag", m.focus == 0, false, true, false, 0, false},
                               (v(v_list(std::move(blocks), 6.0f)) | pad(8) | width(pct(100)) | bg(rgb_of(tt.bg))).build());

        // ── right pane: full terminal + scrollback map ──────────────────
        TermMetrics tm;
        const TermGrid g = right_grid(m);
        TermViewOptions ro{.font_size = 13.0f, .focused = m.focus == 1, .cursor_on = true, .dim_unfocused = 0.6f};
        ScrollbackInfo sb{.total = 1200, .first = 1100, .visible = 22,
                          .marks = {{120, ScrollbackMark::Kind::prompt}, {480, ScrollbackMark::Kind::error}, {700, ScrollbackMark::Kind::prompt},
                                    {1105, ScrollbackMark::Kind::error}, {1110, ScrollbackMark::Kind::prompt}, {900, ScrollbackMark::Kind::match},
                                    {1108, ScrollbackMark::Kind::current_match}, {300, ScrollbackMark::Kind::bookmark}}};
        Node right_term = (h(v(node(terminal_view(look, "term", g, tt, ro, &tm))) | grow() | height(pct(100)) | min_size(0, 0),
                             node(scrollback_bar(look, "sb", sb, 520.0f)))
                           | grow() | width(pct(100)) | height(pct(100)) | min_size(0, 0)).build();
        Node right = pane_frame(look, "pane-r", {"zsh \u2014 ~/src/mayag", m.focus == 1, false, true, false, 0, false},
                                std::move(right_term));

        // ── overlays ────────────────────────────────────────────────────
        if (m.search) term_search(look, "search", node_id("pane-r"), m.query, m.case_sensitive, m.regex, m.match,
                                  static_cast<int>(g.matches.size()));
        if (m.menu_at) {
            const std::vector<MenuItem> items{
                {"Copy", "\u2318C"}, {"Paste", "\u2318V"}, {"Select All", "\u2318A"},
                {"", "", true}, {"Split Right", "\u2318D"}, {"Find\u2026", "\u2318F"}, {"Split Down", "\u2318\u21E7D"},
                {"", "", true}, {"Clear Scrollback", "\u2318K"}, {"Reset Terminal", "", false, true, false, true},
                {"Show Command Blocks", "", false, true, true, false, "VIEW"},
            };
            context_menu(look, "menu", items, m.menu_at);
        }
        if (m.paste_confirm)
            confirm_dialog(look, "paste", {.title = "Paste 4 lines into zsh?",
                                           .body = "Multi-line pastes run each line as a command. This one also contains sudo.",
                                           .preview = "cd /var/log\nsudo rm -rf old/\ntar czf logs.tgz *.log\nscp logs.tgz backup:/srv",
                                           .confirm = "Paste anyway", .danger = true, .remember = "Don't ask for this terminal",
                                           .remember_on = m.paste_remember});
        if (m.resizing) resize_hud(look, "hud", 132, 41, "1456 \u00D7 890 px");
        if (m.hover_link) link_tooltip(look, "link", m.mouse, "file:///Users/ayush/src/mayag", "\u2318-click to open");

        return v(node(term_tabs(look, "tabs", tabs, m.active_tab)),
                 node(pane_split(look, "split", Axis::horizontal, m.ratio, std::move(left), std::move(right))))
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (is(c, "tabs", i)) return TabClick{static_cast<int>(i)};
            if (is(c, "tabs", term_part_close + i)) return TabClose{static_cast<int>(i)};
        }
        if (is(c, "tabs", part_inc)) return NewTab{};
        for (std::size_t b = 0; b < 4; ++b) {
            for (std::size_t a = 0; a < block_action::count; ++a)
                if (is(c, "block", block_part(b, a))) return BlockAction{static_cast<int>(b), static_cast<int>(a)};
            if (is(c, "block", b)) return BlockClick{static_cast<int>(b)};
        }
        for (std::size_t i = 0; i < 12; ++i) if (is(c, "menu", i)) return MenuPick{static_cast<int>(i)};
        for (std::size_t p : {2, 4, 5, 6, 9}) if (is(c, "search", p)) return SearchPart{p};
        for (int a = 0; a < 3; ++a) if (is(c, "paste", static_cast<std::size_t>(a))) return PasteAnswer{a};
        if (c.button == MouseButton::right && (is(c, "term") || is(c, "pane-r"))) return Menu{m.mouse};
        if (is(c, "term") || is(c, "pane-r")) return Focus{1};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "split", part_bar)) return SplitDrag{d.delta.x};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Left& l) {
        if (is(l, "menu", part_menu)) return MenuClose{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const MouseMove& mv) { return Mouse{mv.position}; }
    static std::optional<Msg> on(const Model& m, const TextEvent& e) {
        if (m.search || m.hints) return Typed{e.text};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (m.paste_confirm) {
            if (k.key == Key::enter) return PasteAnswer{1};
            if (k.key == Key::escape) return PasteAnswer{0};
            return std::nullopt;
        }
        if (k.mods.primary() && k.key == Key::f) return OpenSearch{};
        if (k.mods.primary() && k.key == Key::v) return Paste{};
        if (k.mods.primary() && k.mods.shift && k.key == Key::space) return Hints{};
        if (k.mods.primary() && k.key == Key::w) return Quit{};
        if (m.search) return SearchKey{k};
        if (k.key == Key::escape && m.hints) return Hints{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
    static Sub subscribe(const Model&) { return Sub::none(); }
};

static_assert(mayag::Program<TerminalWidgets>);

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1400, 640});
    std::shared_ptr<typo::FontStack> fonts;
    if (opts.use_system_fonts) {
        static constexpr std::array<std::string_view, 4> mono{"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono"};
        fonts = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400, false, mono);
    } else {
        fonts = demo::make_fonts(opts);
    }
    AppConfig cfg{.title = "mayag terminal widgets", .size = opts.size, .theme = TerminalWidgets::theme(), .fonts = fonts,
                  .debug_bounds = opts.debug_bounds};
    using TW = TerminalWidgets;
    return demo::run<TW>(opts, cfg, [](auto& rt) {
        std::printf("terminal widgets headless\n");
        int fails = 0;
        const auto ok = [&](bool cnd, std::string_view what) {
            std::printf("  %s  %.*s\n", cnd ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
            if (!cnd) ++fails;
        };
        const auto& M = [&]() -> const TW::Model& { return rt.model(); };
        const char* shot_dir = std::getenv("TW_SHOT");
        if (shot_dir) rt.window().resize(Vec2{1400, 640}, 2.0f);
        const auto shot = [&](const char* name) {
            if (!shot_dir) return;
            rt.tick();
            const auto px = rt.window().read_pixels();
            (void)imageio::write_png(std::string{shot_dir} + "/" + name + ".png", px, 2800, static_cast<int>(px.size() / 4 / 2800));
        };
        rt.tick(); rt.tick();
        shot("main");

        ok(rt.node_rect("term").has_value() && rt.node_rect("pane-l").has_value(), "both panes laid out");
        ok(rt.node_rect(part_id("block", 1)).has_value(), "command blocks laid out");
        rt.click(part_id("tabs", 2));
        ok(M().active_tab == 2, "clicking a tab activates it");
        rt.window().move_to(rt.node_rect(part_id("tabs", 1))->center());
        rt.tick();
        if (auto cx = rt.node_rect(part_id("tabs", term_part_close + 1))) { rt.window().click(cx->center()); rt.tick(); }
        ok(M().closed_tabs == 1, "hovering a tab reveals its close button, which closes it");
        rt.click(part_id("tabs", part_inc));
        ok(M().new_tabs == 1, "+ opens a new tab");

        rt.click(part_id("block", 2));
        ok(M().selected_block == 2 && M().focus == 0, "clicking a block selects it and focuses its pane");
        rt.window().move_to(rt.node_rect(part_id("block", 0))->center());
        rt.tick();
        if (auto r = rt.node_rect(part_id("block", block_part(0, block_action::collapse)))) { rt.window().click(r->center()); rt.tick(); }
        ok(M().collapsed0, "the hover action collapses a block");

        const float before = M().ratio;
        if (auto bar = rt.node_rect(part_id("split", part_bar))) {
            rt.window().push(MouseDown{bar->center(), MouseButton::left, {}, 1});
            rt.tick();
            for (int i = 0; i < 5; ++i) { rt.window().push(MouseMove{bar->center() + Vec2{20.0f * static_cast<float>(i + 1), 0}, {20, 0}, {}}); rt.tick(); }
            rt.window().push(MouseUp{bar->center() + Vec2{100, 0}, MouseButton::left, {}});
            rt.tick();
        }
        ok(M().ratio > before, "dragging the divider resizes the split");

        rt.press_key(Key::f, Mods{.super = true});
        rt.type("error");
        ok(M().search && M().query.text == "error", "cmd-f opens scrollback search");
        shot("search");
        rt.press_key(Key::enter);
        ok(M().match == 1, "enter goes to the next match");
        rt.press_key(Key::escape);
        ok(!M().search, "esc closes it");

        if (auto tr = rt.node_rect("term")) {
            rt.window().move_to(tr->center());
            rt.window().click(tr->center(), MouseButton::right);
            rt.tick();
        }
        ok(M().menu_at.has_value(), "right-click opens the context menu");
        shot("menu");
        rt.click(part_id("menu", 1));
        ok(!M().menu_at && M().paste_confirm, "Paste in the menu asks first (multi-line protection)");
        shot("paste");
        rt.click(part_id("paste", 2));
        ok(M().paste_remember, "the don't-ask checkbox toggles");
        rt.press_key(Key::escape);
        ok(!M().paste_confirm && M().log.back() == "paste-cancelled", "esc cancels the paste");

        rt.press_key(Key::space, Mods{.shift = true, .super = true});
        ok(M().hints, "cmd-shift-space shows quick-select hints");
        shot("hints");
        rt.type("d");
        ok(!M().hints && M().log.back() == "hint-d", "typing a label picks that hint");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
