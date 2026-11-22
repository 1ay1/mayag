// examples/terminal_settings.cpp — a terminal emulator's settings, done seriously
//
// The settings ARE the config file. One table of typed keys (`font.size: int
// 8..32 step 1 pt`) drives everything: the list, the controls, the inspector,
// the generated TOML, the diff against what is on disk, and the `:set`
// command. Adding a setting is one line in make_specs().
//
//   keyboard first   j/k move, h/l adjust, ⏎ edit, d back to default,
//                    u / ctrl-r undo/redo, / fuzzy search, : command line
//                    (:w :q :wq :e! :reset :set font.size=14), 0-9 sections
//   mouse too        every control is clickable/draggable, rows select
//   honest state     every value lives in one History<Settings>; "modified",
//                    "unsaved", the diff and undo are comparisons, not flags
//   live preview     scheme, opacity, blur, padding, size, line height,
//                    cursor style and colour, rendered as you change them
//
//   ./mayag_terminal_settings              open a window
//   ./mayag_terminal_settings --png o.png  render one frame (TS_DEMO=1 adds edits)
//   ./mayag_terminal_settings --headless   scripted and asserted, for CI

#include "harness.hpp"
#include <mayag/widget/color_swatch.hpp>
#include <mayag/widget/diff_view.hpp>
#include <mayag/widget/dot.hpp>
#include <mayag/widget/dropdown.hpp>
#include <mayag/widget/fuzzy.hpp>
#include <mayag/widget/key_hint.hpp>
#include <mayag/widget/keybind.hpp>
#include <mayag/widget/nav_item.hpp>
#include <mayag/widget/scroll_view.hpp>
#include <mayag/widget/segmented.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/stepper.hpp>
#include <mayag/widget/text_field.hpp>
#include <mayag/widget/toggle.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace mayag;
using namespace mayag::dsl;

namespace {

[[nodiscard]] constexpr Color<Srgb> hx(std::uint32_t v) noexcept {
    return rgb8(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
                static_cast<std::uint8_t>(v));
}

struct Scheme {
    const char* name;
    std::uint32_t bg, fg, cursor;
    std::array<std::uint32_t, 16> ansi;
};

constexpr std::array<Scheme, 6> kSchemes{{
    {"Tokyo Night", 0x1A1B26, 0xC0CAF5, 0xC0CAF5,
     {0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6,
      0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5}},
    {"Dracula", 0x282A36, 0xF8F8F2, 0xF8F8F2,
     {0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2,
      0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF}},
    {"Gruvbox", 0x282828, 0xEBDBB2, 0xEBDBB2,
     {0x282828, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0xA89984,
      0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2}},
    {"Nord", 0x2E3440, 0xD8DEE9, 0xD8DEE9,
     {0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0,
      0x4C566A, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4}},
    {"Solarized Dark", 0x002B36, 0x839496, 0x93A1A1,
     {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
      0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}},
    {"Solarized Light", 0xFDF6E3, 0x657B83, 0x586E75,
     {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
      0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}},
}};

constexpr std::array<std::uint32_t, 5> kAccents{0x7AA2F7, 0xE0AF68, 0x9ECE6A, 0xBB9AF7, 0x7DCFFF};

/// Monospace families worth offering, best first; only installed ones show.
constexpr std::array<std::string_view, 12> kMonoCandidates{
    "SF Mono", "JetBrains Mono", "Menlo", "Monaco", "Fira Code", "Cascadia Code",
    "Source Code Pro", "IBM Plex Mono", "DejaVu Sans Mono", "Ubuntu Mono",
    "Liberation Mono", "Courier New"};

[[nodiscard]] constexpr Mods primary_mods(bool shift = false) noexcept {
#if defined(__APPLE__)
    return Mods{.shift = shift, .super = true};
#else
    (void)shift;
    return Mods{.shift = true, .ctrl = true};   // Ctrl alone belongs to the shell
#endif
}

struct Action { const char* key; Chord fallback; };
const std::array<Action, 14> kActions{{
    {"new_tab",     {Key::t, primary_mods()}},
    {"close_tab",   {Key::w, primary_mods()}},
    {"new_window",  {Key::n, primary_mods()}},
    {"split_right", {Key::d, primary_mods()}},
    {"split_down",  {Key::d, primary_mods(true)}},
    {"next_tab",    {Key::bracket_right, primary_mods(true)}},
    {"prev_tab",    {Key::bracket_left, primary_mods(true)}},
    {"copy",        {Key::c, primary_mods()}},
    {"paste",       {Key::v, primary_mods()}},
    {"find",        {Key::f, primary_mods()}},
    {"clear",       {Key::k, primary_mods()}},
    {"zoom_in",     {Key::equal, primary_mods()}},
    {"zoom_out",    {Key::minus, primary_mods()}},
    {"zoom_reset",  {Key::n0, primary_mods()}},
}};

[[nodiscard]] std::string chord_text(Chord ch) {
    if (ch.empty()) return "none";
    std::string s;
    if (ch.mods.ctrl)  s += "ctrl+";
    if (ch.mods.alt)   s += "alt+";
    if (ch.mods.shift) s += "shift+";
    if (ch.mods.super) s += "cmd+";
    std::string k = key_label(ch.key);
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s + k;
}

// ── the schema ──────────────────────────────────────────────────────────

enum class Kind : std::uint8_t { toggle, choice, number, fraction, color, text, chord };
constexpr std::array<const char*, 7> kKindName{"bool", "enum", "int", "float", "color", "path", "chord"};

struct Spec {
    std::string              key;          // "font.size"
    int                      section = 1;
    Kind                     kind = Kind::toggle;
    std::string              doc;
    float                    def = 0.0f, lo = 0.0f, hi = 1.0f, step = 1.0f;
    std::string              unit;
    std::vector<std::string> options;      // choice / color
    int                      slot = 0;     // chord: index into Settings::keys
};

// Section 0 lists everything; 9 is the generated file.
constexpr std::array<const char*, 10> kSections{"all", "theme", "window", "font", "cursor",
                                                "shell", "term", "render", "keys", "config.toml"};
constexpr std::array<const char*, 10> kSectionIcon{"*", "\u25D0", "\u25A1", "A", "\u258D",
                                                   "$", ">", "\u2261", "\u2318", "\u00B6"};
constexpr int kConfigPage = 9;
constexpr float kRowH = 36.0f;
constexpr float kSlider = 150.0f;
constexpr std::size_t kMaxSpecs = 64;

[[nodiscard]] std::vector<Spec> make_specs(const std::vector<std::string>& fonts) {
    std::vector<Spec> s;
    const auto add = [&](Spec x) { s.push_back(std::move(x)); };
    const auto choice = [&](std::string k, int sec, std::string doc, std::vector<std::string> o, int def) {
        Spec x{.key = std::move(k), .section = sec, .kind = Kind::choice, .doc = std::move(doc)};
        x.def = static_cast<float>(def); x.hi = static_cast<float>(o.size() - 1); x.options = std::move(o);
        add(std::move(x));
    };
    const auto toggle = [&](std::string k, int sec, std::string doc, bool def) {
        add(Spec{.key = std::move(k), .section = sec, .kind = Kind::toggle, .doc = std::move(doc),
                 .def = def ? 1.0f : 0.0f});
    };
    const auto number = [&](std::string k, int sec, std::string doc, float def, float lo, float hi,
                            float step, std::string unit, Kind kind = Kind::number) {
        add(Spec{.key = std::move(k), .section = sec, .kind = kind, .doc = std::move(doc),
                 .def = def, .lo = lo, .hi = hi, .step = step, .unit = std::move(unit)});
    };

    std::vector<std::string> schemes;
    for (const auto& sc : kSchemes) schemes.emplace_back(sc.name);
    choice("theme.scheme", 1, "Colour scheme: background, foreground, cursor and the 16 ANSI colours.",
           schemes, 0);
    choice("theme.ui_accent", 1, "Accent colour of this window. Does not affect the terminal.",
           {"blue", "amber", "green", "violet", "cyan"}, 0);
    number("window.opacity", 2, "Background opacity. Text stays fully opaque.",
           0.94f, 0.30f, 1.00f, 0.01f, "", Kind::fraction);
    toggle("window.blur", 2, "Blur what is behind a translucent window (frosted glass).", true);
    number("window.padding", 2, "Space between the window edge and the cell grid.",
           12, 0, 40, 2, "px");
    choice("window.decorations", 2, "Title bar and border. 'none' leaves resizing to the WM.",
           {"full", "minimal", "none"}, 0);
    choice("font.family", 3, "Installed monospace faces, best first.", fonts, 0);
    number("font.size", 3, "Point size of the grid. cmd/ctrl + = and - change it live.",
           13, 8, 32, 1, "pt");
    number("font.line_height", 3, "Line height as a multiple of the font's own.",
           1.25f, 1.00f, 2.00f, 0.05f, "\u00D7", Kind::fraction);
    toggle("font.ligatures", 3, "Shape => != === into single glyphs where the face has them.", true);
    toggle("font.bold_is_bright", 3, "Draw bold text in the bright ANSI colours (xterm convention).",
           false);
    choice("font.antialias", 3, "Glyph edge smoothing. Subpixel is sharpest on low-DPI panels.",
           {"none", "gray", "subpixel"}, 1);
    choice("cursor.style", 4, "Shape of the insertion point.", {"block", "beam", "underline"}, 0);
    toggle("cursor.blink", 4, "Blink the cursor. Stops after 15 s idle to save power.", true);
    number("cursor.blink_ms", 4, "Half-period of the blink.", 600, 200, 1500, 50, "ms");
    {
        Spec x{.key = "cursor.color", .section = 4, .kind = Kind::color,
               .doc = "Cursor colour: the scheme's own, or one of its ANSI colours."};
        x.options = {"scheme", "red", "green", "yellow", "blue", "magenta", "cyan"};
        x.hi = 6;
        add(std::move(x));
    }
    add(Spec{.key = "shell.program", .section = 5, .kind = Kind::text,
             .doc = "Program to run in a new tab. Enter applies, Esc cancels."});
    choice("shell.cwd", 5, "Working directory of a new tab.", {"home", "inherit", "project"}, 0);
    choice("shell.scrollback", 5, "Lines kept above the screen.",
           {"1000", "5000", "10000", "50000", "100000", "unlimited"}, 2);
    toggle("shell.login", 5, "Start the shell as a login shell (sources profile files).", true);
    choice("term.bell", 6, "What \\a does.", {"off", "visual", "audible"}, 1);
    toggle("term.copy_on_select", 6, "Selecting text copies it to the clipboard.", false);
    toggle("term.option_as_meta", 6, "Option sends Esc-prefixed keys instead of special characters.",
           true);
    toggle("term.confirm_close", 6, "Ask before closing a tab whose process is still running.", true);
    toggle("term.mouse_reporting", 6, "Let full-screen programs (vim, tmux) receive the mouse.", true);
    choice("term.term", 6, "Value of $TERM for child processes.",
           {"xterm-256color", "tmux-256color", "xterm-kitty", "screen-256color"}, 0);
    choice("render.backend", 7, "Renderer. 'auto' prefers the GPU and falls back to software.",
           {"auto", "metal", "vulkan", "software"}, 0);
    choice("render.present", 7, "mailbox: newest frame wins, lowest latency. fifo: queued, vsynced.",
           {"mailbox", "fifo"}, 0);
    choice("render.max_fps", 7, "Frame cap while animating. Idle is 0 fps regardless.",
           {"60", "120", "144", "240", "unlimited"}, 1);
    toggle("render.subpixel_positioning", 7, "Place glyphs at fractional pixel positions.", true);
    number("render.atlas_px", 7, "Glyph atlas edge length. Bigger holds more glyphs.",
           1024, 512, 4096, 512, "px");
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        add(Spec{.key = std::string{"keys."} + kActions[i].key, .section = 8, .kind = Kind::chord,
                 .doc = "Click (or ⏎) and press the new chord. Backspace clears, Esc cancels.",
                 .slot = static_cast<int>(i)});
    }
    return s;
}

[[nodiscard]] std::string leaf_of(const std::string& key) {
    const auto dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(dot + 1);
}

}  // namespace

struct TerminalSettings {
    // ── state ───────────────────────────────────────────────────────────

    struct Settings {
        std::array<float, kMaxSpecs>        v{};              // by spec index
        std::string                         shell = "/bin/zsh";
        std::array<Chord, kActions.size()>  keys{};
        friend bool operator==(const Settings&, const Settings&) = default;
    };

    enum class Mode : std::uint8_t { normal, search, insert, record, command };

    struct Model {
        std::vector<Spec>        specs;
        std::vector<std::string> fonts;
        History<Settings>        s;
        Settings                 saved;
        Settings                 defaults;
        int                      section = 0;
        int                      row = 0;
        Mode                     mode = Mode::normal;
        TextEditState            search, cmd, shell_edit;
        std::string              eat;             // the char that opened a mode
        std::string              flash;           // status-line message
        bool                     flash_err = false;
        ScrollState              list;
        std::uint64_t            open_drop = 0;
        int                      recording = -1;  // spec index
    };

    // ── messages ────────────────────────────────────────────────────────

    struct Move       { int d; };
    struct Jump       { int row; };
    struct FocusSpec  { int si; };
    struct Adjust     { int d; };
    struct AdjustSpec { int si; int d; };
    struct SetChoice  { int si; int v; };
    struct FlipSpec   { int si; };
    struct SetFrac    { int si; float f; };
    struct EndDrag    {};
    struct Activate   {};
    struct ResetRow   {};
    struct ResetSpec  { int si; };
    struct OpenDrop   { int si; };
    struct CloseDrop  {};
    struct Section    { int s; };
    struct SearchStart {};
    struct SearchKey  { KeyEvent k; };
    struct ClearSearch {};
    struct CmdStart   {};
    struct CmdKey     { KeyEvent k; };
    struct StartEdit  { int si; };
    struct EditKey    { KeyEvent k; };
    struct StartRecord { int si; };
    struct Record     { KeyEvent k; };
    struct Typed      { std::string s; };
    struct Undo {}; struct Redo {}; struct Save {}; struct Revert {}; struct Defaults {};
    struct ScrollBy   { float dy; };
    struct BarPress   { float y; };
    struct BarDrag   { DragTravel t; };
    struct Tick       { double dt; };
    struct Quit       {};

    using Msg = std::variant<Move, Jump, FocusSpec, Adjust, AdjustSpec, SetChoice, FlipSpec, SetFrac,
                             EndDrag, Activate, ResetRow, ResetSpec, OpenDrop, CloseDrop, Section,
                             SearchStart, SearchKey, ClearSearch, CmdStart, CmdKey, StartEdit, EditKey,
                             StartRecord, Record, Typed, Undo, Redo, Save, Revert, Defaults, ScrollBy,
                             BarPress, BarDrag, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, focus>;
    using Sub = mayag::Sub<Msg>;

    // ── schema helpers ──────────────────────────────────────────────────

    [[nodiscard]] static int idx(const Model& m, std::string_view key) {
        for (std::size_t i = 0; i < m.specs.size(); ++i)
            if (m.specs[i].key == key) return static_cast<int>(i);
        return -1;
    }
    [[nodiscard]] static float val(const Model& m, const Settings& s, std::string_view key) {
        const int i = idx(m, key);
        return i >= 0 ? s.v[static_cast<std::size_t>(i)] : 0.0f;
    }
    [[nodiscard]] static int ival(const Model& m, std::string_view key) {
        return static_cast<int>(std::lround(val(m, *m.s, key)));
    }
    [[nodiscard]] static const Spec& spec(const Model& m, int si) { return m.specs[static_cast<std::size_t>(si)]; }

    [[nodiscard]] static bool same_value(const Model& m, const Settings& a, const Settings& b, int si) {
        const Spec& sp = spec(m, si);
        if (sp.kind == Kind::text) return a.shell == b.shell;
        if (sp.kind == Kind::chord) return a.keys[static_cast<std::size_t>(sp.slot)] == b.keys[static_cast<std::size_t>(sp.slot)];
        return a.v[static_cast<std::size_t>(si)] == b.v[static_cast<std::size_t>(si)];
    }
    [[nodiscard]] static bool modified(const Model& m, int si) { return !same_value(m, *m.s, m.defaults, si); }

    [[nodiscard]] static std::string fmt_num(const Spec& sp, float v, bool unit = true) {
        char b[32];
        if (sp.step < 1.0f) std::snprintf(b, sizeof b, "%.2f", static_cast<double>(v));
        else std::snprintf(b, sizeof b, "%ld", std::lround(v));
        std::string s = b;
        if (unit && !sp.unit.empty()) s += (sp.unit == "\u00D7" ? "" : " ") + sp.unit;
        return s;
    }
    [[nodiscard]] static std::string value_text(const Model& m, const Settings& s, int si) {
        const Spec& sp = spec(m, si);
        const float v = s.v[static_cast<std::size_t>(si)];
        switch (sp.kind) {
            case Kind::toggle:   return v != 0.0f ? "true" : "false";
            case Kind::choice:
            case Kind::color:    return sp.options[static_cast<std::size_t>(std::clamp<long>(std::lround(v), 0, static_cast<long>(sp.options.size()) - 1))];
            case Kind::number:
            case Kind::fraction: return fmt_num(sp, v);
            case Kind::text:     return s.shell;
            case Kind::chord:    return chord_text(s.keys[static_cast<std::size_t>(sp.slot)]);
        }
        return {};
    }
    [[nodiscard]] static std::string toml_value(const Model& m, const Settings& s, int si) {
        const Spec& sp = spec(m, si);
        if (sp.kind == Kind::toggle) return value_text(m, s, si);
        if (sp.kind == Kind::number || sp.kind == Kind::fraction)
            return fmt_num(sp, s.v[static_cast<std::size_t>(si)], false);
        const std::string v = value_text(m, s, si);
        if (sp.kind == Kind::choice && !v.empty() && std::all_of(v.begin(), v.end(), ::isdigit)) return v;
        return "\"" + v + "\"";
    }

    /// The file Save writes. Diffing two of these is the pending-changes view.
    [[nodiscard]] static std::vector<std::string> config_lines(const Model& m, const Settings& s) {
        std::vector<std::string> out{"# ~/.config/term/config.toml", "# generated by mayag settings"};
        for (int sec = 1; sec <= 8; ++sec) {
            out.emplace_back("");
            out.push_back(std::string{"["} + kSections[static_cast<std::size_t>(sec)] + "]");
            for (std::size_t i = 0; i < m.specs.size(); ++i) {
                if (m.specs[i].section != sec) continue;
                std::string k = leaf_of(m.specs[i].key);
                k.resize(std::max<std::size_t>(k.size(), 20), ' ');
                out.push_back(k + "= " + toml_value(m, s, static_cast<int>(i)));
            }
        }
        return out;
    }

    [[nodiscard]] static Settings make_defaults(const Model& m) {
        Settings s;
        for (std::size_t i = 0; i < m.specs.size(); ++i) s.v[i] = m.specs[i].def;
        for (std::size_t i = 0; i < kActions.size(); ++i) s.keys[i] = kActions[i].fallback;
        return s;
    }

    [[nodiscard]] static std::array<bool, kActions.size()> conflicts(const Settings& s) {
        std::array<bool, kActions.size()> out{};
        for (std::size_t i = 0; i < s.keys.size(); ++i)
            for (std::size_t j = i + 1; j < s.keys.size(); ++j)
                if (!s.keys[i].empty() && s.keys[i] == s.keys[j]) out[i] = out[j] = true;
        return out;
    }

    /// The rows on screen: spec indices, with fuzzy matches when searching.
    struct Visible { std::vector<int> specs; std::vector<FuzzyMatch> matches; };
    [[nodiscard]] static Visible visible(const Model& m) {
        Visible v;
        if (!m.search.text.empty()) {
            std::vector<std::pair<int, FuzzyMatch>> hits;
            for (std::size_t i = 0; i < m.specs.size(); ++i)
                if (auto f = fuzzy_match(m.search.text, m.specs[i].key); f.ok)
                    hits.emplace_back(static_cast<int>(i), std::move(f));
            std::stable_sort(hits.begin(), hits.end(),
                             [](const auto& a, const auto& b) { return a.second.score > b.second.score; });
            for (auto& [i, f] : hits) { v.specs.push_back(i); v.matches.push_back(std::move(f)); }
            return v;
        }
        if (m.section == kConfigPage) return v;
        for (std::size_t i = 0; i < m.specs.size(); ++i)
            if (m.section == 0 || m.specs[i].section == m.section) v.specs.push_back(static_cast<int>(i));
        return v;
    }
    [[nodiscard]] static int current(const Model& m) {
        const auto v = visible(m);
        return m.row >= 0 && m.row < static_cast<int>(v.specs.size()) ? v.specs[static_cast<std::size_t>(m.row)] : -1;
    }
    [[nodiscard]] static bool dirty(const Model& m) { return m.saved != *m.s; }
    [[nodiscard]] static int modified_count(const Model& m, int sec) {
        int n = 0;
        for (std::size_t i = 0; i < m.specs.size(); ++i)
            if ((sec == 0 || m.specs[i].section == sec) && modified(m, static_cast<int>(i))) ++n;
        return n;
    }

    // ── init ────────────────────────────────────────────────────────────

    static Cmd init(Model& m) {
        const auto& db = typo::system::Database::instance();
        for (auto fam : kMonoCandidates)
            for (const auto& e : db.entries())
                if (e.family == fam) { m.fonts.emplace_back(fam); break; }
        if (m.fonts.empty()) m.fonts.emplace_back("monospace");
        m.specs = make_specs(m.fonts);
        m.defaults = make_defaults(m);
        m.s = History<Settings>{m.defaults};
        m.saved = m.defaults;
        m.shell_edit.text = m.s->shell;
        m.flash = std::to_string(m.specs.size()) + " keys loaded from ~/.config/term/config.toml";

        // Screenshot helpers: TS_SECTION=n, TS_SEARCH=text, TS_DEMO=1 (some edits).
        if (const char* p = std::getenv("TS_SECTION")) m.section = std::clamp(std::atoi(p), 0, kConfigPage);
        if (const char* q = std::getenv("TS_SEARCH")) { m.search.text = q; m.search.caret = m.search.anchor = m.search.text.size(); }
        if (std::getenv("TS_DEMO") != nullptr) {
            m.s.edit([&](Settings& x) {
                x.v[static_cast<std::size_t>(idx(m, "font.size"))] = 14;
                x.v[static_cast<std::size_t>(idx(m, "cursor.style"))] = 1;
                x.v[static_cast<std::size_t>(idx(m, "theme.scheme"))] = 3;
                x.v[static_cast<std::size_t>(idx(m, "window.opacity"))] = 0.86f;
            });
            m.row = 7;
        }
        return SetTitle{"term \u2014 settings"};
    }

    // ── update helpers ──────────────────────────────────────────────────

    static void say(Model& m, std::string s, bool err = false) { m.flash = std::move(s); m.flash_err = err; }

    /// Leave any transient mode: commit a shell edit, stop recording.
    static void settle(Model& m) {
        if (m.mode == Mode::insert) {
            if (m.shell_edit.text != m.s->shell) {
                const std::string v = m.shell_edit.text;
                m.s.edit([&](Settings& x) { x.shell = v; });
                say(m, "shell.program = \"" + v + "\"");
            }
        }
        m.recording = -1;
        if (m.mode != Mode::normal) m.mode = Mode::normal;
    }
    static void resync(Model& m) {
        m.shell_edit.text = m.s->shell;
        m.shell_edit.caret = m.shell_edit.anchor = m.shell_edit.text.size();
    }
    /// Keep the cursor row inside the scroll viewport (rows are a fixed height,
    /// so this is arithmetic, not a layout query).
    static void reveal(Model& m) {
        const float y = static_cast<float>(m.row) * kRowH;
        const float vp = m.list.viewport.y;
        if (vp <= 0.0f) return;
        if (y < m.list.offset.y) m.list.scroll_to(Vec2{0.0f, y});
        else if (y + kRowH > m.list.offset.y + vp) m.list.scroll_to(Vec2{0.0f, y + kRowH - vp});
    }
    static void set_row(Model& m, int r) {
        const int n = static_cast<int>(visible(m).specs.size());
        m.row = n == 0 ? 0 : std::clamp(r, 0, n - 1);
        reveal(m);
    }
    static void focus_spec(Model& m, int si) {
        const auto v = visible(m);
        for (std::size_t r = 0; r < v.specs.size(); ++r)
            if (v.specs[r] == si) { m.row = static_cast<int>(r); return; }
    }

    static void set_num(Model& m, int si, float x, std::string_view group = {}) {
        const Spec& sp = spec(m, si);
        if (sp.kind == Kind::choice || sp.kind == Kind::color) {
            const int n = static_cast<int>(sp.options.size());
            x = static_cast<float>(((static_cast<int>(std::lround(x)) % n) + n) % n);
        } else if (sp.kind == Kind::toggle) {
            x = x != 0.0f ? 1.0f : 0.0f;
        } else if (sp.kind == Kind::number || sp.kind == Kind::fraction) {
            x = std::clamp(std::round((x - sp.lo) / sp.step) * sp.step + sp.lo, sp.lo, sp.hi);
            x = std::round(x * 1000.0f) / 1000.0f;
        } else {
            return;
        }
        const auto i = static_cast<std::size_t>(si);
        if (m.s->v[i] == x) return;
        m.s.edit([&](Settings& s) { s.v[i] = x; }, group);
        say(m, sp.key + " = " + value_text(m, *m.s, si));
    }
    static void adjust(Model& m, int si, int d) {
        const Spec& sp = spec(m, si);
        const float v = m.s->v[static_cast<std::size_t>(si)];
        switch (sp.kind) {
            case Kind::toggle:   set_num(m, si, 1.0f - v); break;
            case Kind::choice:
            case Kind::color:    set_num(m, si, v + static_cast<float>(d)); break;
            case Kind::number:
            case Kind::fraction: set_num(m, si, v + static_cast<float>(d) * sp.step); break;
            default: break;
        }
    }
    static void reset_spec(Model& m, int si) {
        if (!modified(m, si)) return;
        const Spec& sp = spec(m, si);
        m.s.edit([&](Settings& s) {
            if (sp.kind == Kind::text) s.shell = m.defaults.shell;
            else if (sp.kind == Kind::chord) s.keys[static_cast<std::size_t>(sp.slot)] = m.defaults.keys[static_cast<std::size_t>(sp.slot)];
            else s.v[static_cast<std::size_t>(si)] = sp.def;
        });
        resync(m);
        say(m, sp.key + " reset to default (" + value_text(m, *m.s, si) + ")");
    }
    [[nodiscard]] static bool dropdown_style(const Spec& sp) {
        if (sp.kind != Kind::choice) return false;
        if (sp.options.size() > 4) return true;
        for (const auto& o : sp.options) if (o.size() > 9) return true;
        return false;
    }
    /// ⏎ on a row: the natural "do it" for each kind.
    static Cmd activate_spec(Model& m, int si) {
        const Spec& sp = spec(m, si);
        switch (sp.kind) {
            case Kind::toggle: adjust(m, si, 1); return {};
            case Kind::choice:
                if (dropdown_style(sp)) { m.open_drop = m.open_drop == node_id(sp.key) ? 0 : node_id(sp.key); return {}; }
                adjust(m, si, 1); return {};
            case Kind::color: adjust(m, si, 1); return {};
            case Kind::text:
                m.mode = Mode::insert;
                m.shell_edit.select_all();
                return FocusNode{node_id(sp.key)};
            case Kind::chord:
                m.mode = Mode::record;
                m.recording = si;
                return {};
            default: return {};
        }
    }

    // ── the command line ────────────────────────────────────────────────

    [[nodiscard]] static bool parse_into(Model& m, int si, std::string value) {
        const Spec& sp = spec(m, si);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        while (!value.empty() && value.back() == ' ') value.pop_back();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        std::string low = value;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        switch (sp.kind) {
            case Kind::toggle:
                if (low == "true" || low == "on" || low == "1" || low == "yes") { set_num(m, si, 1); return true; }
                if (low == "false" || low == "off" || low == "0" || low == "no") { set_num(m, si, 0); return true; }
                return false;
            case Kind::choice:
            case Kind::color:
                for (std::size_t i = 0; i < sp.options.size(); ++i) {
                    std::string o = sp.options[i];
                    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (o == low) { set_num(m, si, static_cast<float>(i)); return true; }
                }
                return false;
            case Kind::number:
            case Kind::fraction: {
                char* end = nullptr;
                const float f = std::strtof(value.c_str(), &end);
                if (end == value.c_str()) return false;
                set_num(m, si, f);
                return true;
            }
            case Kind::text:
                if (value.empty()) return false;
                m.s.edit([&](Settings& s) { s.shell = value; });
                resync(m);
                say(m, sp.key + " = \"" + value + "\"");
                return true;
            case Kind::chord:
                return false;
        }
        return false;
    }

    /// vim-flavoured: :w :q :wq :x :e! :reset :set key=value :set key! :set key?
    static Cmd run_command(Model& m, std::string line) {
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) return {};
        const auto write = [&] {
            m.saved = *m.s;
            say(m, "\"~/.config/term/config.toml\" " + std::to_string(config_lines(m, m.saved).size()) +
                       "L written");
        };
        if (line == "w") { write(); return {}; }
        if (line == "q" || line == "q!") {
            if (line == "q" && dirty(m)) { say(m, "E37: No write since last change (add ! to override)", true); return {}; }
            return Cmd::quit();
        }
        if (line == "wq" || line == "x") { write(); return Cmd::quit(); }
        if (line == "e!") { if (dirty(m)) { m.s.set(m.saved); resync(m); } say(m, "reverted to the saved file"); return {}; }
        if (line == "reset" || line == "defaults") {
            if (*m.s != m.defaults) { m.s.set(m.defaults); resync(m); }
            say(m, "all keys reset to defaults");
            return {};
        }
        if (line.rfind("set ", 0) == 0 || line.rfind("se ", 0) == 0) {
            std::string rest = line.substr(line.find(' ') + 1);
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
            std::string key = rest, value;
            char op = 0;
            if (auto eq = rest.find_first_of("= "); eq != std::string::npos) {
                key = rest.substr(0, eq); value = rest.substr(eq + 1); op = '=';
            } else if (!rest.empty() && (rest.back() == '!' || rest.back() == '?')) {
                op = rest.back(); key = rest.substr(0, rest.size() - 1);
            }
            int si = idx(m, key);
            if (si < 0) {   // unambiguous fuzzy key, like a shell completing it
                int best = -1, best_score = -1000, ties = 0;
                for (std::size_t i = 0; i < m.specs.size(); ++i)
                    if (auto f = fuzzy_match(key, m.specs[i].key); f.ok) {
                        if (f.score > best_score) { best_score = f.score; best = static_cast<int>(i); ties = 0; }
                        else if (f.score == best_score) ++ties;
                    }
                if (best >= 0 && ties == 0) si = best;
            }
            if (si < 0) { say(m, "E518: Unknown option: " + key, true); return {}; }
            const Spec& sp = spec(m, si);
            focus_spec(m, si);
            if (op == '?' || op == 0) { say(m, sp.key + " = " + value_text(m, *m.s, si)); return {}; }
            if (op == '!') {
                if (sp.kind != Kind::toggle) { say(m, "E488: " + sp.key + " is not a bool", true); return {}; }
                adjust(m, si, 1);
                return {};
            }
            if (!parse_into(m, si, value)) {
                say(m, "E474: Invalid argument: " + sp.key + "=" + value + "  (" + kKindName[static_cast<std::size_t>(sp.kind)] + ")", true);
            }
            return {};
        }
        say(m, "E492: Not an editor command: " + line, true);
        return {};
    }

    // ── update ──────────────────────────────────────────────────────────

    // Keyboard navigation drops whatever control a click focused, so ⏎/space
    // act on the cursor row, not on a control clicked minutes ago.
    static Cmd update(Model& m, Move e) { m.open_drop = 0; set_row(m, m.row + e.d); return FocusNode{0}; }
    static Cmd update(Model& m, Jump e) { m.open_drop = 0; set_row(m, e.row); return FocusNode{0}; }
    static Cmd update(Model& m, FocusSpec e) { settle(m); m.open_drop = 0; focus_spec(m, e.si); return {}; }
    static Cmd update(Model& m, Adjust e) {
        if (const int si = current(m); si >= 0) adjust(m, si, e.d);
        return {};
    }
    static Cmd update(Model& m, AdjustSpec e) { settle(m); focus_spec(m, e.si); adjust(m, e.si, e.d); return {}; }
    static Cmd update(Model& m, SetChoice e) {
        settle(m); m.open_drop = 0; focus_spec(m, e.si); set_num(m, e.si, static_cast<float>(e.v)); return {};
    }
    static Cmd update(Model& m, FlipSpec e) { settle(m); focus_spec(m, e.si); adjust(m, e.si, 1); return {}; }
    static Cmd update(Model& m, SetFrac e) {
        settle(m);
        focus_spec(m, e.si);
        const Spec& sp = spec(m, e.si);
        // One drag is ONE undo step: the group coalesces every move of the
        // gesture; EndDrag breaks it.
        set_num(m, e.si, sp.lo + std::clamp(e.f, 0.0f, 1.0f) * (sp.hi - sp.lo), sp.key);
        return {};
    }
    static Cmd update(Model& m, EndDrag) { m.s.break_coalescing(); return {}; }
    static Cmd update(Model& m, Activate) {
        if (const int si = current(m); si >= 0) return activate_spec(m, si);
        return {};
    }
    static Cmd update(Model& m, ResetRow) {
        if (const int si = current(m); si >= 0) reset_spec(m, si);
        return {};
    }
    static Cmd update(Model& m, ResetSpec e) { settle(m); focus_spec(m, e.si); reset_spec(m, e.si); return {}; }
    static Cmd update(Model& m, OpenDrop e) {
        settle(m); focus_spec(m, e.si);
        const auto drop = node_id(spec(m, e.si).key);
        m.open_drop = m.open_drop == drop ? 0 : drop;
        return {};
    }
    static Cmd update(Model& m, CloseDrop) { m.open_drop = 0; return {}; }
    static Cmd update(Model& m, Section e) {
        settle(m);
        m.open_drop = 0;
        m.search = TextEditState{};
        m.section = e.s;
        m.row = 0;
        m.list.scroll_to_top();
        return FocusNode{0};
    }
    static Cmd update(Model& m, SearchStart) {
        settle(m);
        m.open_drop = 0;
        m.mode = Mode::search;
        m.eat = "/";
        m.search.select_all();
        return FocusNode{node_id("search")};
    }
    static Cmd update(Model& m, ClearSearch) {
        m.search = TextEditState{};
        m.mode = Mode::normal;
        m.row = 0;
        m.list.scroll_to_top();
        return FocusNode{0};
    }
    static Cmd update(Model& m, SearchKey e) {
        const auto& k = e.k;
        if (k.key == Key::escape) return update(m, ClearSearch{});
        if (k.key == Key::enter) { m.mode = Mode::normal; return FocusNode{0}; }
        if (k.key == Key::down) { set_row(m, m.row + 1); return {}; }
        if (k.key == Key::up) { set_row(m, m.row - 1); return {}; }
        if (m.search.handle_key(k.key, k.mods)) { m.row = 0; m.list.scroll_to_top(); }
        return {};
    }
    static Cmd update(Model& m, CmdStart) {
        settle(m);
        m.open_drop = 0;
        m.mode = Mode::command;
        m.eat = ":";
        m.cmd = TextEditState{};
        return {};
    }
    static Cmd update(Model& m, CmdKey e) {
        const auto& k = e.k;
        if (k.key == Key::escape) { m.mode = Mode::normal; return {}; }
        if (k.key == Key::enter) {
            m.mode = Mode::normal;
            return run_command(m, m.cmd.text);
        }
        if (k.key == Key::backspace && m.cmd.text.empty()) { m.mode = Mode::normal; return {}; }
        (void)m.cmd.handle_key(k.key, k.mods);
        return {};
    }
    static Cmd update(Model& m, StartEdit e) {
        if (m.mode == Mode::insert) return {};
        settle(m); m.open_drop = 0; focus_spec(m, e.si);
        return activate_spec(m, e.si);
    }
    static Cmd update(Model& m, EditKey e) {
        if (e.k.key == Key::escape) { m.mode = Mode::normal; resync(m); return FocusNode{0}; }
        if (e.k.key == Key::enter) { settle(m); return FocusNode{0}; }
        (void)m.shell_edit.handle_key(e.k.key, e.k.mods);
        return {};
    }
    static Cmd update(Model& m, StartRecord e) {
        const bool was = m.recording == e.si;
        settle(m); m.open_drop = 0; focus_spec(m, e.si);
        if (!was) { m.mode = Mode::record; m.recording = e.si; }
        return {};
    }
    static Cmd update(Model& m, Record e) {
        const int si = m.recording;
        if (si < 0) { m.mode = Mode::normal; return {}; }
        const auto& k = e.k;
        if (is_modifier_key(k.key)) return {};
        m.recording = -1;
        m.mode = Mode::normal;
        if (k.key == Key::escape && k.mods.none()) { say(m, "recording cancelled"); return {}; }
        const Chord ch = (k.key == Key::backspace && k.mods.none()) ? Chord{} : Chord{k.key, k.mods};
        const auto slot = static_cast<std::size_t>(spec(m, si).slot);
        if (m.s->keys[slot] != ch) m.s.edit([&](Settings& s) { s.keys[slot] = ch; });
        say(m, spec(m, si).key + " = \"" + chord_text(ch) + "\"" +
                   (conflicts(*m.s)[slot] ? "  (conflicts with another binding)" : ""),
            conflicts(*m.s)[slot]);
        return {};
    }
    static Cmd update(Model& m, Typed e) {
        std::string s = std::move(e.s);
        // The key that opened a mode also arrives as text; it is not input.
        if (!m.eat.empty() && s.rfind(m.eat, 0) == 0) s.erase(0, m.eat.size());
        m.eat.clear();
        if (s.empty()) return {};
        if (m.mode == Mode::search) { m.search.insert(s); m.row = 0; m.list.scroll_to_top(); }
        else if (m.mode == Mode::command) m.cmd.insert(s);
        else if (m.mode == Mode::insert) m.shell_edit.insert(s);
        return {};
    }
    static Cmd update(Model& m, Undo) {
        settle(m);
        if (m.s.undo()) { resync(m); say(m, "undo  (" + std::to_string(m.s.undo_depth()) + " more)"); }
        else say(m, "Already at oldest change", true);
        return {};
    }
    static Cmd update(Model& m, Redo) {
        settle(m);
        if (m.s.redo()) { resync(m); say(m, "redo  (" + std::to_string(m.s.redo_depth()) + " more)"); }
        else say(m, "Already at newest change", true);
        return {};
    }
    static Cmd update(Model& m, Save) { settle(m); return run_command(m, "w"); }
    static Cmd update(Model& m, Revert) { settle(m); return run_command(m, "e!"); }
    static Cmd update(Model& m, Defaults) { settle(m); return run_command(m, "reset"); }
    static Cmd update(Model& m, ScrollBy e) { m.list.scroll_by(Vec2{0.0f, e.dy}); return {}; }
    static Cmd update(Model& m, BarPress e) { scrollbar_press(m.list, e.y); return {}; }
    static Cmd update(Model& m, BarDrag e) { scrollbar_drag(m.list, e.t); return {}; }
    static Cmd update(Model&, Tick) { return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── view ────────────────────────────────────────────────────────────

    /// The frame's Ctx with the theme the user picked: the controls take any
    /// ControlContext, so the window re-skins itself live.
    struct Look {
        const Ctx& c;
        Theme      theme;
        [[nodiscard]] bool hovered(std::uint64_t n) const noexcept { return c.hovered(n); }
        [[nodiscard]] bool pressed(std::uint64_t n) const noexcept { return c.pressed(n); }
        [[nodiscard]] bool focused(std::uint64_t n) const noexcept { return c.focused(n); }
        void overlay(Overlay o) const { c.overlay(std::move(o)); }
    };
    static_assert(ControlContext<Look>);

    [[nodiscard]] static Theme theme_of(const Model& m) {
        Theme t = make_theme(hx(kAccents[static_cast<std::size_t>(std::clamp(ival(m, "theme.ui_accent"), 0, 4))]), mayag::Mode::dark);
        t.background = mix(t.background, colors::black, 0.45f);
        t.surface = mix(t.surface, colors::black, 0.30f);
        t.font_size = 12.5f;
        t.radius_small = 4.0f;
        t.radius_medium = 6.0f;
        return t;
    }
    /// The theme is application state: picking an accent re-themes the whole
    /// window -- every widget, the clear colour -- and the host morphs to it.
    static Theme theme(const Model& m) { return theme_of(m); }

    static Node control_for(const Look& L, const Model& m, int si) {
        const Theme& t = L.theme;
        const Spec& sp = spec(m, si);
        const auto i = static_cast<std::size_t>(si);
        const float v = m.s->v[i];
        switch (sp.kind) {
            case Kind::toggle:
                return h(switch_control(L, sp.key, v != 0.0f),
                         text_owned(v != 0.0f ? "true" : "false") | font(12) | fg(v != 0.0f ? t.success : t.text_disabled))
                       | gap(10) | align(Align::center);
            case Kind::choice: {
                const int sel = static_cast<int>(std::lround(v));
                if (dropdown_style(sp))
                    return dropdown(L, sp.key, sp.options, sel, m.open_drop == node_id(sp.key), 210).build();
                std::vector<std::string_view> opts(sp.options.begin(), sp.options.end());
                return segmented(L, sp.key, opts, sel).build();
            }
            case Kind::number:
                return stepper(L, sp.key, fmt_num(sp, v), v <= sp.lo, v >= sp.hi).build();
            case Kind::fraction:
                return h(slider(L, sp.key, (v - sp.lo) / (sp.hi - sp.lo), kSlider),
                         text_owned(fmt_num(sp, v)) | font(12) | fg(t.text_primary) | width(52))
                       | gap(12) | align(Align::center);
            case Kind::color: {
                const Scheme& sc = kSchemes[static_cast<std::size_t>(ival(m, "theme.scheme"))];
                std::vector<Node> sw;
                for (std::size_t k = 0; k < sp.options.size(); ++k)
                    sw.push_back(swatch(L, sp.key, k, k == 0 ? hx(sc.cursor) : hx(sc.ansi[k]),
                                        static_cast<int>(k) == std::lround(v), 18).build());
                return h_list(std::move(sw), 7.0f).build();
            }
            case Kind::text:
                return (text_field(t, m.shell_edit, m.mode == Mode::insert, node_id(sp.key), "/bin/zsh", 12.5f)
                        | width(260) | role(a11y::Role::text_field) | label(sp.key)).build();
            case Kind::chord: {
                const auto slot = static_cast<std::size_t>(sp.slot);
                return keybind(L, sp.key, 0, m.s->keys[slot], m.recording == si, conflicts(*m.s)[slot]).build();
            }
        }
        return Node{};
    }

    static Node key_cell(const Theme& t, const Spec& sp, const FuzzyMatch* fm) {
        if (fm != nullptr) return fuzzy_text(sp.key, fm->positions, 12.5f, t.text_secondary, t.accent).build();
        const auto dot = sp.key.find('.');
        return h(text_owned(sp.key.substr(0, dot + 1)) | font(12.5f) | fg(t.text_disabled),
                 text_owned(sp.key.substr(dot + 1)) | font(12.5f) | fg(t.text_primary)).build();
    }

    static Node row_view(const Look& L, const Model& m, int si, int r, const FuzzyMatch* fm) {
        const Theme& t = L.theme;
        const Spec& sp = spec(m, si);
        const bool cur = r == m.row;
        const bool mod = modified(m, si);
        const auto rid = node_id("row", static_cast<std::size_t>(si));
        const bool hov = L.hovered(rid);
        return (h(box() | size(3, kRowH) | bg(cur ? t.accent : t.accent.fade(0.0f)),
                  h(node(mod ? dot(t.warning, 6).build() : (box() | size(6, 6)).build())) | width(22) | center,
                  node(key_cell(t, sp, fm)) | width(262),
                  text_owned(kKindName[static_cast<std::size_t>(sp.kind)]) | font(11)
                      | fg(cur ? t.accent : t.text_disabled) | width(54),
                  h(node(control_for(L, m, si))) | grow() | align(Align::center),
                  text_owned(mod ? value_text(m, m.defaults, si) : "") | font(11.5f)
                      | fg(t.text_disabled) | text_align(TextAlign::right) | width(120) | ellipsis)
                | height(kRowH) | align(Align::center) | pad(0, 16, 0, 0) | width(pct(100))
                | bg(cur ? t.surface_raised : (hov ? t.surface_raised.fade(0.45f) : t.surface.fade(0.0f)))
                | id_of("row", static_cast<std::size_t>(si))).build();
    }

    static Node list_view(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const auto vis = visible(m);
        std::vector<Node> rows;
        rows.reserve(vis.specs.size());
        for (std::size_t r = 0; r < vis.specs.size(); ++r)
            rows.push_back(row_view(L, m, vis.specs[r], static_cast<int>(r),
                                    vis.matches.empty() ? nullptr : &vis.matches[r]));
        if (rows.empty())
            rows.push_back((v(text_owned("no keys match \"" + m.search.text + "\"") | font(12.5f)
                                  | fg(t.text_secondary),
                              text<"esc clears the search"> | font(11.5f) | fg(t.text_disabled))
                            | gap(6) | pad(40, 30)).build());
        return v_list(std::move(rows)).build();
    }

    /// The generated file, with changed-since-save lines marked in the gutter.
    static Node config_view(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const auto lines = line_diff(config_lines(m, m.saved), config_lines(m, *m.s));
        std::vector<Node> rows;
        for (const auto& l : lines) {
            if (l.op == DiffLine::Op::del) continue;
            const bool changed = l.op == DiffLine::Op::add;
            const bool header = !l.text.empty() && l.text.front() == '[';
            const bool comment = !l.text.empty() && l.text.front() == '#';
            Color<Srgb> col = header ? t.accent : comment ? t.text_disabled : t.text_primary;
            rows.push_back((h(box() | size(2, 18) | bg(changed ? t.warning : t.warning.fade(0.0f)),
                              text_owned(std::to_string(l.new_no)) | font(11.5f) | fg(t.text_disabled)
                                  | text_align(TextAlign::right) | width(30),
                              text_owned(l.text.empty() ? std::string{" "} : l.text) | font(12.5f) | no_ligatures
                                  | fg(col) | (header ? semibold : weight(FontWeight::regular)))
                            | gap(14) | align(Align::center) | height(19) | width(pct(100))
                            | bg(changed ? t.tint(t.warning, 0.08f) : t.surface.fade(0.0f))).build());
        }
        return (v(v_list(std::move(rows))) | pad(10, 0) | width(pct(100))).build();
    }

    /// The terminal itself, rendered from the same Settings.
    static Node preview(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const Settings& s = *m.s;
        const Scheme& sc = kSchemes[static_cast<std::size_t>(ival(m, "theme.scheme"))];
        const auto ansi = [&](int i) { return hx(sc.ansi[static_cast<std::size_t>(i)]); };
        const float fs = std::min(val(m, s, "font.size"), 15.0f);   // the preview is a thumbnail
        const float lh = std::round(fs * val(m, s, "font.line_height"));
        const float cw = std::round(fs * 0.6f);
        const float pad_px = val(m, s, "window.padding") * 0.6f;
        const bool bright = val(m, s, "font.bold_is_bright") != 0.0f;
        const auto fgc = hx(sc.fg);

        const auto seg = [&](std::string str, Color<Srgb> col, bool strong = false) {
            const Color<Srgb> c = strong && bright ? mix(col, colors::white, 0.25f) : col;
            return (text_owned(std::move(str)) | font(fs) | fg(c) | (strong ? bold : weight(FontWeight::regular))
                    | no_ligatures).build();
        };
        const auto line = [&](std::vector<Node> runs) {
            return (h(h_list(std::move(runs))) | height(lh) | align(Align::center)).build();
        };
        const auto prompt = [&](std::vector<Node> rest) {
            std::vector<Node> p{seg("~/src/mayag", ansi(4), true), seg(" main", ansi(5)), seg(" \u276F ", ansi(2), true)};
            for (auto& n : rest) p.push_back(std::move(n));
            return line(std::move(p));
        };
        const int cs = ival(m, "cursor.style"), cc = ival(m, "cursor.color");
        const Color<Srgb> cur = cc == 0 ? hx(sc.cursor) : ansi(cc);
        Node cursor_node = cs == 0 ? (box() | size(cw, lh - 2) | bg(cur)).build()
                         : cs == 1 ? (box() | size(2, lh - 2) | bg(cur)).build()
                                   : (z(box() | size(cw, 2) | bg(cur) | absolute(0, lh - 4)) | size(cw, lh - 2)).build();

        std::vector<Node> lines;
        lines.push_back(prompt({seg("git status -sb", fgc)}));
        lines.push_back(line({seg("## main", ansi(2)), seg("...", fgc), seg("origin/main", ansi(1))}));
        lines.push_back(line({seg(" M ", ansi(1)), seg("examples/terminal_settings.cpp", fgc)}));
        lines.push_back(line({seg("?? ", ansi(1)), seg("include/mayag/widget/fuzzy.hpp", fgc)}));
        lines.push_back(prompt({seg("cargo build", fgc)}));
        lines.push_back(line({seg("   Compiling ", ansi(2), true), seg("term v0.4.2", fgc)}));
        lines.push_back(line({seg("    Finished ", ansi(2), true), seg("release [optimized] in 3.81s", fgc)}));
        lines.push_back(prompt({std::move(cursor_node)}));

        std::vector<Node> pal;
        for (int i = 0; i < 16; ++i) pal.push_back((box() | size(12, 12) | bg(ansi(i)) | radius(2)).build());

        const int deco = ival(m, "window.decorations");
        auto chrome = h(dot(hx(0xFF5F57), 9), dot(hx(0xFEBC2E), 9), dot(hx(0x28C840), 9), spacer(),
                        text_owned(s.shell + " \u2014 " + m.fonts[static_cast<std::size_t>(ival(m, "font.family"))] +
                                   " " + std::to_string(static_cast<int>(val(m, s, "font.size"))))
                            | font(10.5f) | fg(fgc.fade(0.55f)),
                        spacer(), strut(33))
                    | gap(6) | align(Align::center) | pad(7, 10) | width(pct(100));

        auto term = v(node(deco == 0 ? chrome.build() : (box() | height(deco == 1 ? 6.0f : 0.0f)).build()),
                      v(v_list(std::move(lines))) | pad(pad_px) | width(pct(100)) | clip,
                      h(h_list(std::move(pal), 3.0f)) | pad(0, pad_px, pad_px, pad_px))
                  | width(pct(100))
                  | bg(hx(sc.bg).fade(val(m, s, "window.opacity")))
                  | backdrop_blur(val(m, s, "window.blur") != 0.0f ? 20.0f : 0.0f)
                  | border(1, colors::white.fade(0.08f))
                  | radius(deco == 2 ? 0.0f : 8.0f) | clip | elevation(8);

        return (v(term) | pad(16) | width(pct(100))
                | linear_gradient(mix(t.accent, hx(0xFF6AA2), 0.4f), hx(0x0E4D64), {0, 0}, {1, 1})
                | radius(t.radius_medium) | border(1, t.border)).build();
    }

    static Node panel_title(const Theme& t, std::string s, Node right = Node{}) {
        return (h(text_owned(std::move(s)) | font(10.5f) | semibold | tracking(1.2f) | fg(t.text_disabled),
                  spacer(), node(std::move(right)))
                | align(Align::center) | width(pct(100))).build();
    }

    static Node inspector(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const int si = current(m);
        if (si < 0) {
            return v(node(panel_title(t, "INSPECT")),
                     text<"select a key"> | font(12.5f) | fg(t.text_disabled))
                   | gap(10) | width(pct(100));
        }
        const Spec& sp = spec(m, si);
        std::string sig = kKindName[static_cast<std::size_t>(sp.kind)];
        if (sp.kind == Kind::number || sp.kind == Kind::fraction)
            sig += "  " + fmt_num(sp, sp.lo, false) + ".." + fmt_num(sp, sp.hi, false) + "  step " +
                   fmt_num(sp, sp.step, false) + (sp.unit.empty() ? "" : "  " + sp.unit);
        if (sp.kind == Kind::choice || sp.kind == Kind::color) {
            sig += "  ";
            for (std::size_t i = 0; i < sp.options.size() && i < 5; ++i) sig += (i ? " | " : "") + sp.options[i];
            if (sp.options.size() > 5) sig += " | \u2026";
        }
        const bool mod = modified(m, si);
        const auto kv = [&](const char* k, std::string v, Color<Srgb> c) {
            return (h(text_owned(k) | font(11.5f) | fg(t.text_disabled) | width(70),
                      text_owned(std::move(v)) | font(12.5f) | fg(c) | ellipsis | grow())
                    | width(pct(100))).build();
        };
        return v(node(panel_title(t, "INSPECT",
                                  (text_owned(mod ? "\u25CF modified" : "default") | font(11)
                                       | fg(mod ? t.warning : t.text_disabled)).build())),
                 text_owned(sp.key) | font(15) | semibold | fg(t.accent),
                 text_owned(sig) | font(11.5f) | fg(t.text_secondary) | ellipsis,
                 text_owned(sp.doc) | font(12) | fg(t.text_primary) | wrap_text | line_height(1.4f),
                 node(kv("value", value_text(m, *m.s, si), mod ? t.warning : t.text_primary)),
                 node(kv("default", value_text(m, m.defaults, si), t.text_secondary)),
                 node(kv("on disk", value_text(m, m.saved, si),
                         same_value(m, m.saved, m.defaults, si) ? t.text_secondary : t.info)))
               | gap(7) | width(pct(100));
    }

    static Node pending(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const auto lines = line_diff(config_lines(m, m.saved), config_lines(m, *m.s));
        const auto st = diff_stats(lines);
        Node right = st.added + st.removed == 0
            ? (text<"clean"> | font(11) | fg(t.success)).build()
            : (h(text_owned("+" + std::to_string(st.added)) | font(11) | semibold | fg(t.success),
                 text_owned("\u2212" + std::to_string(st.removed)) | font(11) | semibold | fg(t.danger))
               | gap(8)).build();
        Node body = st.added + st.removed == 0
            ? (v(text<"matches config.toml on disk"> | font(12) | fg(t.text_disabled)) | pad(8, 0)).build()
            : (v(node(diff_view(t, lines, 11.5f, 1))) | width(pct(100))
               | bg(t.surface) | border(1, t.border) | radius(t.radius_small) | clip).build();
        return (v(node(panel_title(t, "PENDING  \u2192  :w", std::move(right))), node(std::move(body)))
                | gap(10) | width(pct(100)) | grow() | min_size(0, 0) | clip).build();
    }

    static Node status_bar(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const Ctx& c = L.c;
        static constexpr std::array<const char*, 5> names{"NORMAL", "SEARCH", "INSERT", "RECORD", "COMMAND"};
        const std::array<Color<Srgb>, 5> tones{t.accent, t.info, t.success, t.danger, t.warning};
        const auto mi = static_cast<std::size_t>(m.mode);
        auto name_chip = h(text_owned(names[mi]) | font(11) | bold | tracking(0.8f) | fg(readable_on(tones[mi])))
                  | pad(0, 10) | height(pct(100)) | center | bg(tones[mi]);

        Node middle;
        if (m.mode == Mode::command) {
            const std::string& s = m.cmd.text;
            const std::size_t cpos = std::min(m.cmd.caret, s.size());
            middle = (h(text_owned(":" + s.substr(0, cpos)) | font(12.5f) | fg(t.text_primary),
                        box() | size(7, 15) | bg(t.text_primary),
                        text_owned(s.substr(cpos)) | font(12.5f) | fg(t.text_primary))
                      | align(Align::center)).build();
        } else {
            const int si = current(m);
            const int nmod = modified_count(m, 0);
            int clash = 0;
            for (bool b : conflicts(*m.s)) clash += b ? 1 : 0;
            middle = (h(text_owned(si >= 0 ? spec(m, si).key : std::string{kSections[static_cast<std::size_t>(m.section)]})
                            | font(12) | fg(t.text_primary),
                        text_owned(m.flash) | font(12) | fg(m.flash_err ? t.danger : t.text_secondary) | ellipsis
                            | grow() | min_size(0, 0),
                        text_owned(std::to_string(nmod) + " modified") | font(11.5f)
                            | fg(nmod ? t.warning : t.text_disabled),
                        text_owned(std::to_string(clash / 2) + " conflicts") | font(11.5f)
                            | fg(clash ? t.danger : t.text_disabled))
                      | gap(18) | align(Align::center) | grow() | min_size(0, 0)).build();
        }

        std::vector<Node> hints;
        const auto hint = [&](std::string_view k, std::string_view a) { hints.push_back(key_hint(t, k, a, 11).build()); };
        switch (m.mode) {
            case Mode::normal:
                hint("j/k", "move"); hint("h/l", "adjust"); hint("\u23CE", "edit"); hint("d", "default");
                hint("u", "undo"); hint("/", "find"); hint(":", "cmd"); break;
            case Mode::search: hint("\u2191\u2193", "move"); hint("\u23CE", "done"); hint("esc", "clear"); break;
            case Mode::insert: hint("\u23CE", "apply"); hint("esc", "cancel"); break;
            case Mode::record: hint("chord", "bind"); hint("\u232B", "unbind"); hint("esc", "cancel"); break;
            case Mode::command: hint("\u23CE", "run"); hint("esc", "cancel");
                                hint(":w :q :e!", ""); hint(":set k=v", ""); break;
        }
        char perf[64] = "";
        const double cpu = c.latency != nullptr ? c.latency->mean_cpu_ms() : 0.0;
        if (c.photon.samples > 0) std::snprintf(perf, sizeof perf, "%.2fms cpu  %.1fms \u2192 photon", cpu, c.photon.mean_ms);
        else if (cpu > 0.0) std::snprintf(perf, sizeof perf, "%.2fms cpu", cpu);
        const auto vis = visible(m);
        return (h(std::move(name_chip),
                  v(node(std::move(middle))) | grow() | min_size(0, 0) | pad(0, 14),
                  h(h_list(std::move(hints), 12.0f)),
                  text_owned(perf) | font(11) | fg(t.text_disabled) | pad(0, 14),
                  h(text_owned(vis.specs.empty() ? std::string{"--"}
                                                 : std::to_string(m.row + 1) + "/" + std::to_string(vis.specs.size()))
                        | font(11) | fg(t.text_secondary))
                      | pad(0, 12) | height(pct(100)) | center | bg(t.surface_raised))
                | height(28) | width(pct(100)) | align(Align::center)
                | bg(t.surface) | border(1, t.border)).build();
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        const Look L{c, theme_of(m)};
        const Theme& t = L.theme;

        // ── sidebar ─────────────────────────────────────────────────────
        std::vector<Node> nav;
        for (std::size_t i = 0; i < kSections.size(); ++i) {
            const int sec = static_cast<int>(i);
            const int nmod = sec == kConfigPage ? 0 : modified_count(m, sec);
            int total = 0;
            for (const auto& sp : m.specs) total += (sec == 0 || sp.section == sec) ? 1 : 0;
            const std::string trail = sec == kConfigPage ? (dirty(m) ? "[+]" : "")
                                    : nmod > 0 ? std::to_string(nmod) + "/" + std::to_string(total)
                                               : std::to_string(total);
            nav.push_back(nav_item(L, "nav", i, kSectionIcon[i], kSections[i],
                                   m.search.text.empty() && m.section == sec, trail,
                                   nmod > 0 || (sec == kConfigPage && dirty(m))));
            if (i == 0 || i == 8)
                nav.push_back((box() | height(1) | width(pct(100)) | bg(t.border.fade(0.6f)) | margin(4, 0)).build());
        }
        auto sidebar = v(text<"term"> | font(16) | bold | fg(t.text_primary) | pad(2, 10),
                         text<"settings  v0.4.2"> | font(11) | fg(t.text_disabled) | pad(0, 10),
                         strut(6),
                         v(v_list(std::move(nav), 1.0f)) | width(pct(100)),
                         spacer(),
                         text<"0-9 jump  \u00B7  / find"> | font(10.5f) | fg(t.text_disabled) | pad(0, 10))
                     | gap(4) | pad(16, 8) | width(196) | height(pct(100))
                     | bg(t.surface) | border(1, t.border);

        // ── header ──────────────────────────────────────────────────────
        const bool unsaved = dirty(m);
        const auto chip = [&](std::string_view label_, std::string_view id_, bool enabled, bool primary_ = false) {
            const auto nid = node_id(id_);
            const bool hov = enabled && c.hovered(nid);
            return (h(text_owned(std::string{label_}) | font(12) | (primary_ ? semibold : weight(FontWeight::regular))
                          | fg(!enabled ? t.text_disabled : primary_ ? t.on_accent : hov ? t.text_primary : t.text_secondary))
                    | pad(5, 11) | center
                    | bg(primary_ && enabled ? t.accent : hov ? t.surface_raised : t.surface)
                    | border(1, primary_ && enabled ? t.accent : t.border)
                    | radius(t.radius_small)
                    | cursor(enabled ? CursorShape::pointer : CursorShape::arrow)
                    | role(a11y::Role::button) | disabled(!enabled)
                    | id_of(id_)).build();
        };
        auto header =
            h(h(text<"~/.config/term/"> | font(12.5f) | fg(t.text_disabled),
                text<"config.toml"> | font(12.5f) | semibold | fg(t.text_primary),
                text_owned(unsaved ? " [+]" : "") | font(12.5f) | bold | fg(t.warning))
                  | align(Align::center),
              spacer(),
              node((text_field(t, m.search, m.mode == Mode::search, node_id("search"),
                               "/  fuzzy-find a key", 12.5f)
                    | width(300) | role(a11y::Role::text_field) | label("search")).build()),
              spacer(),
              node(chip("u  undo", "undo", m.s.can_undo())),
              node(chip("^r redo", "redo", m.s.can_redo())),
              node(chip(":reset", "defaults", *m.s != m.defaults)),
              node(chip(":e!", "revert", unsaved)),
              node(chip(":w  write", "save", unsaved, true)))
            | gap(6) | align(Align::center) | pad(10, 16) | height(54) | width(pct(100))
            | bg(t.background) | border(1, t.border);

        // ── centre: the table (or the file) ─────────────────────────────
        const bool file = m.section == kConfigPage && m.search.text.empty();
        auto columns = h(strut(25),
                         text<"KEY"> | font(10.5f) | semibold | tracking(1.2f) | fg(t.text_disabled) | width(262),
                         text<"TYPE"> | font(10.5f) | semibold | tracking(1.2f) | fg(t.text_disabled) | width(54),
                         text<"VALUE"> | font(10.5f) | semibold | tracking(1.2f) | fg(t.text_disabled) | grow(),
                         text<"DEFAULT"> | font(10.5f) | semibold | tracking(1.2f) | fg(t.text_disabled)
                             | width(120) | text_align(TextAlign::right))
                     | align(Align::center) | pad(0, 16, 0, 0) | height(30) | width(pct(100))
                     | border(1, t.border.fade(0.6f));
        auto centre = v(node(file ? panel_title(t, "  ~/.config/term/config.toml  \u00B7  generated, read-only") : columns.build()),
                        scroll_view(L, "list", m.list,
                                    v(node(file ? config_view(L, m) : list_view(L, m))) | width(pct(100)))
                            | pad(0, 4, 4, 0))
                    | grow() | height(pct(100)) | min_size(0, 0);

        // ── right: inspect, preview, pending ────────────────────────────
        auto right = v(node(inspector(L, m)),
                       v(node(panel_title(t, "PREVIEW")), node(preview(L, m))) | gap(10) | width(pct(100)),
                       node(pending(L, m)))
                   | gap(20) | pad(16) | width(420) | height(pct(100))
                   | bg(t.background) | border(1, t.border);

        return v(std::move(header),
                 h(std::move(sidebar), std::move(centre), std::move(right))
                     | grow() | width(pct(100)) | min_size(0, 0),
                 node(status_bar(L, m)))
             | width(pct(100)) | height(pct(100)) | bg(t.background);
    }

    // ── input ───────────────────────────────────────────────────────────

    /// One mapping for pointer clicks AND keyboard activation of a focused
    /// control, so every control is operable both ways.
    static std::optional<Msg> activate(const Model& m, std::uint64_t n) {
        for (std::size_t i = 0; i < kSections.size(); ++i)
            if (n == part_id("nav", i)) return Section{static_cast<int>(i)};
        if (n == node_id("search")) return SearchStart{};
        if (n == node_id("undo")) return Undo{};
        if (n == node_id("redo")) return Redo{};
        if (n == node_id("save")) return Save{};
        if (n == node_id("revert")) return Revert{};
        if (n == node_id("defaults")) return Defaults{};
        for (std::size_t i = 0; i < m.specs.size(); ++i) {
            const Spec& sp = m.specs[i];
            const int si = static_cast<int>(i);
            if (n == node_id("row", i)) return FocusSpec{si};
            if (n == node_id(sp.key)) {
                switch (sp.kind) {
                    case Kind::toggle: return FlipSpec{si};
                    case Kind::choice: return OpenDrop{si};
                    case Kind::text:   return StartEdit{si};
                    default:           return FocusSpec{si};
                }
            }
            for (std::size_t k = 0; k < sp.options.size(); ++k)
                if (n == part_id(sp.key, k)) return SetChoice{si, static_cast<int>(k)};
            if (n == part_id(sp.key, part_dec)) return AdjustSpec{si, -1};
            if (n == part_id(sp.key, part_inc)) return AdjustSpec{si, +1};
            if (sp.kind == Kind::chord && n == part_id(sp.key, 0)) return StartRecord{si};
        }
        return std::nullopt;
    }

    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        if (c.button != MouseButton::left) return std::nullopt;
        if (auto msg = activate(m, c.node)) return msg;
        if (m.mode == Mode::insert || m.mode == Mode::record) return FocusSpec{current(m)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Activated& a) {
        if (m.mode != Mode::normal) return std::nullopt;   // Enter/Space are text there
        return activate(m, a.node);
    }
    [[nodiscard]] static int slider_spec(const Model& m, std::uint64_t n) {
        for (std::size_t i = 0; i < m.specs.size(); ++i)
            if (m.specs[i].kind == Kind::fraction && n == node_id(m.specs[i].key)) return static_cast<int>(i);
        return -1;
    }
    static std::optional<Msg> on(const Model& m, const Pressed& p) {
        if (is(p, "list", part_bar)) return BarPress{p.local.y};
        if (const int si = slider_spec(m, p.node); si >= 0) return SetFrac{si, p.local.x / kSlider};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Dragged& d) {
        if (is(d, "list", part_bar)) return BarDrag{d.travel};
        if (const int si = slider_spec(m, d.node); si >= 0) return SetFrac{si, d.local.x / kSlider};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Released&) { return EndDrag{}; }
    static std::optional<Msg> on(const Model& m, const Left& l) {
        if (m.open_drop == 0) return std::nullopt;
        for (const auto& sp : m.specs)
            if (l.node == part_id(sp.key, part_menu)) return CloseDrop{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::Scrolled& s) {
        if (is(s, "list") && m.open_drop == 0) return ScrollBy{s.delta.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const TextEvent& e) {
        if (m.mode == Mode::search || m.mode == Mode::command || m.mode == Mode::insert) return Typed{e.text};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (m.mode == Mode::record) return Record{k};   // the recorder sees EVERYTHING
        if (k.mods.primary() && k.key == Key::z) return k.mods.shift ? Msg{Redo{}} : Msg{Undo{}};
        if (k.mods.primary() && k.key == Key::s) return Save{};
        if (k.mods.primary() && k.key == Key::w) return Quit{};
        switch (m.mode) {
            case Mode::insert:  return EditKey{k};
            case Mode::search:  return SearchKey{k};
            case Mode::command: return CmdKey{k};
            default: break;
        }
        const bool plain = k.mods.none();
        if (k.key == Key::semicolon && k.mods.shift) return CmdStart{};
        if (k.mods.ctrl && k.key == Key::r) return Redo{};
        if (k.key == Key::g && k.mods.shift) return Jump{1 << 20};
        if (!plain) return std::nullopt;
        switch (k.key) {
            case Key::j: case Key::down:  return Move{+1};
            case Key::k: case Key::up:    return Move{-1};
            case Key::h: case Key::left:  return Adjust{-1};
            case Key::l: case Key::right: return Adjust{+1};
            case Key::page_down:          return Move{+10};
            case Key::page_up:            return Move{-10};
            case Key::g: case Key::home:  return Jump{0};
            case Key::end:                return Jump{1 << 20};
            case Key::enter: case Key::space: return Activate{};
            case Key::d:                  return ResetRow{};
            case Key::u:                  return Undo{};
            case Key::slash:              return SearchStart{};
            case Key::escape:
                if (m.open_drop != 0) return CloseDrop{};
                if (!m.search.text.empty()) return ClearSearch{};
                return std::nullopt;
            default: break;
        }
        if (k.key >= Key::n0 && k.key <= Key::n9)
            return Section{static_cast<int>(k.key) - static_cast<int>(Key::n0)};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    static Sub subscribe(const Model&) {
        return when(false, Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }
};

static_assert(mayag::Program<TerminalSettings>, "TerminalSettings must satisfy Program");

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1320, 840});

    // A monospace UI: the terminal's own typography, with the full system
    // fallback chain behind it (symbols, CJK, emoji).
    std::shared_ptr<typo::FontStack> fonts;
    if (opts.use_system_fonts) {
        static constexpr std::array<std::string_view, 4> mono{"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono"};
        fonts = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 1024},
                                            400, false, mono);
    } else {
        fonts = demo::make_fonts(opts);
    }

    AppConfig cfg{
        .title = "term \u2014 settings",
        .size  = opts.size,
        .theme = TerminalSettings::theme_of(TerminalSettings::Model{}),
        .fonts = fonts,
        .debug_bounds = opts.debug_bounds,
    };

    using TS = TerminalSettings;
    return demo::run<TS>(opts, cfg, [](auto& rt) {
        std::printf("terminal settings headless\n");
        int fails = 0;
        const auto ok = [&](bool c, std::string_view what) {
            std::printf("  %s  %.*s\n", c ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
            if (!c) ++fails;
        };
        const auto& M = [&]() -> const TS::Model& { return rt.model(); };
        const auto V = [&](std::string_view key) { return TS::val(M(), *M().s, key); };
        const auto cur_key = [&] { const int si = TS::current(M()); return si >= 0 ? M().specs[static_cast<std::size_t>(si)].key : std::string{}; };
        rt.tick();
        rt.tick();

        // ── the table ───────────────────────────────────────────────────
        ok(M().specs.size() > 40, "the schema lists " + std::to_string(M().specs.size()) + " keys");
        ok(M().mode == TS::Mode::normal && cur_key() == "theme.scheme", "starts in NORMAL on the first key");
        if (auto bar = rt.node_rect(part_id("list", part_bar)))
            ok(M().list.scrollable_y() && bar->height() > 300.0f, "the full list scrolls, with a visible bar");
        else ok(false, "scrollbar laid out");

        // ── vim keys ────────────────────────────────────────────────────
        rt.press_key(Key::j); rt.press_key(Key::j);
        ok(cur_key() == "window.opacity", "j j moves to window.opacity");
        rt.press_key(Key::l);
        ok(std::abs(V("window.opacity") - 0.95f) < 1e-4f, "l steps it up by its own step (0.01)");
        ok(TS::modified(M(), TS::idx(M(), "window.opacity")) && TS::dirty(M()), "the row is modified, the file unsaved");
        rt.press_key(Key::d);
        ok(!TS::modified(M(), TS::idx(M(), "window.opacity")), "d puts it back to the default");
        rt.press_key(Key::u);
        ok(std::abs(V("window.opacity") - 0.95f) < 1e-4f, "u undoes the reset");
        rt.press_key(Key::r, Mods{.ctrl = true});
        ok(std::abs(V("window.opacity") - 0.94f) < 1e-4f, "ctrl-r redoes it");
        rt.press_key(Key::g, Mods{.shift = true});
        ok(cur_key() == "keys.zoom_reset", "G jumps to the last key");
        if (auto r = rt.node_rect(node_id("row", static_cast<std::size_t>(TS::current(M())))))
            ok(r->bottom() <= 840.0f - 28.0f + 0.5f && r->top() >= 0, "and scrolls it into view");
        else ok(false, "last row laid out");
        rt.press_key(Key::g);
        ok(cur_key() == "theme.scheme" && M().list.offset.y == 0.0f, "g goes back to the top");

        // ── mouse ───────────────────────────────────────────────────────
        rt.click("window.blur");
        ok(V("window.blur") == 0.0f && cur_key() == "window.blur", "clicking a switch flips it and selects its row");
        if (auto r = rt.node_rect("window.opacity")) {
            rt.window().click(Vec2{r->left() + r->width() * 0.5f, r->top() + 12});
            rt.tick();
            ok(std::abs(V("window.opacity") - 0.65f) < 0.011f, "clicking the slider's middle sets 65%");
        } else ok(false, "opacity slider laid out");
        rt.click(part_id("window.padding", part_inc));
        ok(V("window.padding") == 14.0f, "the stepper's + adds one step (2 px)");

        // ── fuzzy search ────────────────────────────────────────────────
        rt.press_key(Key::slash);
        ok(M().mode == TS::Mode::search, "/ enters SEARCH");
        rt.type("/fsz");   // the '/' arrives as text too, and must be eaten
        ok(M().search.text == "fsz", "the opening '/' is not typed into the query");
        ok(cur_key() == "font.size", "\"fsz\" ranks font.size first");
        rt.press_key(Key::enter);
        rt.press_key(Key::l);
        ok(V("font.size") == 14.0f && M().mode == TS::Mode::normal, "enter leaves search, l adjusts the hit");
        rt.press_key(Key::escape);
        ok(M().search.text.empty(), "esc clears the filter");

        // ── command line ────────────────────────────────────────────────
        rt.press_key(Key::semicolon, Mods{.shift = true});
        rt.type(":set cursor.style=beam");
        rt.press_key(Key::enter);
        ok(V("cursor.style") == 1.0f, ":set cursor.style=beam");
        rt.press_key(Key::semicolon, Mods{.shift = true});
        rt.type(":set fsize 18");
        rt.press_key(Key::enter);
        ok(V("font.size") == 18.0f, ":set accepts an unambiguous fuzzy key (fsize -> font.size)");
        rt.press_key(Key::semicolon, Mods{.shift = true});
        rt.type(":set nope=1");
        rt.press_key(Key::enter);
        ok(M().flash_err && M().flash.rfind("E518", 0) == 0, "an unknown key is an E518 error");
        rt.press_key(Key::semicolon, Mods{.shift = true});
        rt.type(":q");
        rt.press_key(Key::enter);
        ok(M().flash.rfind("E37", 0) == 0, ":q refuses with unsaved changes");

        // ── pending diff + :w ───────────────────────────────────────────
        {
            const auto st = diff_stats(line_diff(TS::config_lines(M(), M().saved), TS::config_lines(M(), *M().s)));
            ok(st.added == st.removed && st.added >= 4, "the pending diff shows each changed line (" +
                                                            std::to_string(st.added) + " changed)");
        }
        rt.press_key(Key::semicolon, Mods{.shift = true});
        rt.type(":w");
        rt.press_key(Key::enter);
        ok(!TS::dirty(M()), ":w writes: nothing pending");

        // ── dropdown in an overlay ──────────────────────────────────────
        rt.click("theme.scheme");
        ok(M().open_drop == node_id("theme.scheme"), "clicking the scheme opens its list");
        rt.tick();
        rt.click(part_id("theme.scheme", 3));
        ok(V("theme.scheme") == 3.0f && M().open_drop == 0, "picking Nord selects it and closes the list");

        // ── text field ──────────────────────────────────────────────────
        rt.press_key(Key::n5);
        rt.press_key(Key::enter);
        ok(M().mode == TS::Mode::insert, "section 5 (shell), ⏎ on shell.program enters INSERT");
        rt.type("/opt/homebrew/bin/fish");
        rt.press_key(Key::enter);
        ok(M().s->shell == "/opt/homebrew/bin/fish" && M().mode == TS::Mode::normal, "typing + ⏎ applies the path");

        // ── keybinding recorder ─────────────────────────────────────────
        rt.press_key(Key::n8);
        rt.press_key(Key::enter);
        ok(M().mode == TS::Mode::record, "section 8 (keys), ⏎ starts recording");
        rt.press_key(Key::shift, Mods{.shift = true});
        ok(M().mode == TS::Mode::record, "a bare modifier keeps listening");
        rt.press_key(Key::w, primary_mods());
        ok(M().s->keys[0] == (Chord{Key::w, primary_mods()}), "the chord is recorded (and does NOT quit)");
        ok(TS::conflicts(*M().s)[0] && TS::conflicts(*M().s)[1], "a duplicate binding flags both");
        rt.press_key(Key::d);
        ok(!TS::conflicts(*M().s)[0], "d restores the default binding");

        // ── config page ─────────────────────────────────────────────────
        rt.press_key(Key::n9);
        ok(M().section == 9 && TS::visible(M()).specs.empty(), "section 9 shows the generated file");
        rt.press_key(Key::n0);
        ok(M().section == 0, "0 goes back to all keys");

        ok(TS::subscribe(M()).is_none(), "an idle window requests no frames");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
