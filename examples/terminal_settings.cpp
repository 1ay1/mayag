// examples/terminal_settings.cpp — the settings window of a terminal emulator
//
// Every control a terminal's preferences need, built from include/mayag/widget/,
// wired the way a real mayag app should be:
//
//   * ONE source of truth. Every setting is a field of `Settings`, held in a
//     History<Settings>, so undo/redo (Cmd/Ctrl+Z), "unsaved changes",
//     Revert and Reset all fall out of comparing and swapping values.
//   * NO widget state. Controls draw what they are given; update() decides
//     what a click, drag or key means. Hover/press/focus are read from the
//     frame context, never stored.
//   * A LIVE preview. The terminal mock re-renders from the same Settings:
//     scheme, opacity over a wallpaper, blur, padding, size, line height,
//     cursor shape and colour.
//   * The generated config file is a page, so you can see the model IS the
//     configuration.
//
//   ./mayag_terminal_settings              open a window
//   ./mayag_terminal_settings --png o.png  render one frame
//   ./mayag_terminal_settings --headless   scripted and asserted, for CI

#include "harness.hpp"

#include <array>
#include <cmath>

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
    {"Midnight", 0x0B0D10, 0xD7DAE0, 0x5B8CFF,
     {0x1C1F26, 0xE5534B, 0x57AB5A, 0xC69026, 0x539BF5, 0xB083F0, 0x39C5CF, 0xADBAC7,
      0x545D68, 0xFF938A, 0x6BC46D, 0xDAAA3F, 0x6CB6FF, 0xDCBDFB, 0x56D4DD, 0xCDD9E5}},
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

constexpr std::array<const char*, 6> kThemeNames{"midnight", "ember", "forest", "orchid",
                                                  "daylight", "paper"};
constexpr std::array<Theme, 6> kThemes{themes::midnight, themes::ember, themes::forest,
                                       themes::orchid, themes::daylight, themes::paper};

constexpr std::array<int, 6> kScrollback{1000, 5000, 10000, 50000, 100000, 0};   // 0 = unlimited

/// Monospace families worth offering, best first; only installed ones show.
constexpr std::array<std::string_view, 12> kMonoCandidates{
    "SF Mono", "Menlo", "Monaco", "JetBrains Mono", "Fira Code", "Cascadia Code",
    "Source Code Pro", "IBM Plex Mono", "DejaVu Sans Mono", "Ubuntu Mono",
    "Liberation Mono", "Courier New"};

[[nodiscard]] constexpr Mods primary_mods(bool shift = false) noexcept {
#if defined(__APPLE__)
    return Mods{.shift = shift, .super = true};
#else
    return Mods{.shift = true, .ctrl = true};   // terminals use Ctrl+Shift, Ctrl alone is the shell's
    (void)shift;
#endif
}

struct Action { const char* name; const char* key; Chord fallback; };

const std::array<Action, 14> kActions{{
    {"New tab",            "new_tab",       {Key::t, primary_mods()}},
    {"Close tab",          "close_tab",     {Key::w, primary_mods()}},
    {"New window",         "new_window",    {Key::n, primary_mods()}},
    {"Split right",        "split_right",   {Key::d, primary_mods()}},
    {"Split down",         "split_down",    {Key::d, primary_mods(true)}},
    {"Next tab",           "next_tab",      {Key::bracket_right, primary_mods(true)}},
    {"Previous tab",       "prev_tab",      {Key::bracket_left, primary_mods(true)}},
    {"Copy",               "copy",          {Key::c, primary_mods()}},
    {"Paste",              "paste",         {Key::v, primary_mods()}},
    {"Find",               "find",          {Key::f, primary_mods()}},
    {"Clear scrollback",   "clear",         {Key::k, primary_mods()}},
    {"Increase font size", "zoom_in",       {Key::equal, primary_mods()}},
    {"Decrease font size", "zoom_out",      {Key::minus, primary_mods()}},
    {"Reset font size",    "zoom_reset",    {Key::n0, primary_mods()}},
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

}  // namespace

struct TerminalSettings {
    // ── state ───────────────────────────────────────────────────────────

    /// Everything the user can change. Comparable, so "unsaved" is `!=`.
    struct Settings {
        int   scheme = 0;
        int   app_theme = 0;
        float opacity = 0.94f;       // 0.30 .. 1.00
        bool  blur = true;
        int   padding = 12;          // px, 0 .. 40
        int   font = 0;              // index into Model::fonts
        int   font_size = 13;        // 8 .. 32
        float line_height = 1.25f;   // 1.00 .. 2.00
        bool  ligatures = true;
        bool  bold_bright = false;
        int   smoothing = 1;         // none / grayscale / subpixel
        int   cursor_shape = 0;      // block / beam / underline
        bool  cursor_blink = true;
        int   cursor_colour = 0;     // 0 = scheme's; 1.. = ANSI 1..6
        std::string shell = "/bin/zsh";
        int   workdir = 0;           // home / previous tab / custom
        int   scrollback = 2;        // index into kScrollback
        int   bell = 1;              // off / visual / audible
        bool  copy_on_select = false;
        bool  option_as_meta = true;
        bool  confirm_close = true;
        std::array<Chord, kActions.size()> keys{};

        friend bool operator==(const Settings&, const Settings&) = default;
    };

    [[nodiscard]] static Settings defaults() {
        Settings s;
        for (std::size_t i = 0; i < kActions.size(); ++i) s.keys[i] = kActions[i].fallback;
        return s;
    }

    enum Page { appearance, text_page, cursor_page, shell_page, keys_page, file_page, page_count };

    struct Model {
        History<Settings>        s;
        Settings                 saved;
        std::vector<std::string> fonts;     // installed monospace families
        int                      page = appearance;
        ScrollState              scroll;
        Tracked                  nav;       // the sidebar's sliding marker
        std::uint64_t            open_drop = 0;
        int                      recording = -1;
        bool                     editing_shell = false;
        TextEditState            shell_edit;
        int                      last_slider = 0;   // 1 opacity, 2 line height
    };

    // ── messages ────────────────────────────────────────────────────────

    enum class Flag { blur, ligatures, bold_bright, cursor_blink, copy_on_select, option_as_meta,
                      confirm_close };
    enum class Choice { scheme, app_theme, smoothing, cursor_shape, cursor_colour, workdir, bell, font };
    enum class Num { font_size, padding, scrollback };
    enum class Slide { opacity = 1, line_height = 2 };

    struct Go       { int page; };
    struct Flip     { Flag f; };
    struct Choose   { Choice c; int v; };
    struct Step     { Num n; int dir; };
    struct SetFrac  { Slide s; float frac; };
    struct Nudge    { int dir; };
    struct EndDrag  {};
    struct OpenDrop { std::uint64_t id; };
    struct CloseDrop {};
    struct StartRecord { int index; };
    struct Record   { KeyEvent k; };
    struct ResetKey { int index; };
    struct StartEdit {};
    struct EditKey  { KeyEvent k; };
    struct Typed    { std::string s; };
    struct Undo {}; struct Redo {}; struct Save {}; struct Revert {}; struct Reset {};
    struct ScrollBy { float dy; };
    struct Tick     { double dt; };
    struct Quit     {};

    using Msg = std::variant<Go, Flip, Choose, Step, SetFrac, Nudge, EndDrag, OpenDrop, CloseDrop,
                             StartRecord, Record, ResetKey, StartEdit, EditKey, Typed, Undo, Redo,
                             Save, Revert, Reset, ScrollBy, Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title, focus>;
    using Sub = mayag::Sub<Msg>;

    static constexpr float kTrack = 220.0f;   // slider track width

    static Cmd init(Model& m) {
        const auto& db = typo::system::Database::instance();
        for (auto fam : kMonoCandidates) {
            for (const auto& e : db.entries()) {
                if (e.family == fam) { m.fonts.emplace_back(fam); break; }
            }
        }
        if (m.fonts.empty()) m.fonts.emplace_back("monospace");
        m.s = History<Settings>{defaults()};
        m.saved = *m.s;
        m.shell_edit.text = m.s->shell;
        // TS_PAGE=n opens on a page, so `--png` can screenshot each one.
        if (const char* p = std::getenv("TS_PAGE")) m.page = std::clamp(std::atoi(p), 0, page_count - 1);
        if (const char* d = std::getenv("TS_OPEN")) m.open_drop = node_id(d);
        return SetTitle{"Settings"};
    }

    // ── update ──────────────────────────────────────────────────────────

    /// Leaving a transient mode: a click anywhere else ends shortcut recording
    /// and commits the shell field. Called by every click-derived message.
    static void settle(Model& m) {
        m.recording = -1;
        if (m.editing_shell) {
            m.editing_shell = false;
            if (m.shell_edit.text != m.s->shell) {
                const std::string v = m.shell_edit.text;
                m.s.edit([&](Settings& x) { x.shell = v; });
            }
        }
    }
    /// After undo/redo/revert the field must show the restored value.
    static void resync(Model& m) {
        m.shell_edit.text = m.s->shell;
        m.shell_edit.caret = m.shell_edit.anchor = m.shell_edit.text.size();
    }

    static Cmd update(Model& m, Go e) {
        settle(m);
        m.open_drop = 0;
        if (e.page != m.page) { m.page = e.page; m.scroll.scroll_to_top(); }
        return {};
    }
    static Cmd update(Model& m, Flip e) {
        settle(m);
        m.s.edit([&](Settings& x) {
            switch (e.f) {
                case Flag::blur:           x.blur = !x.blur; break;
                case Flag::ligatures:      x.ligatures = !x.ligatures; break;
                case Flag::bold_bright:    x.bold_bright = !x.bold_bright; break;
                case Flag::cursor_blink:   x.cursor_blink = !x.cursor_blink; break;
                case Flag::copy_on_select: x.copy_on_select = !x.copy_on_select; break;
                case Flag::option_as_meta: x.option_as_meta = !x.option_as_meta; break;
                case Flag::confirm_close:  x.confirm_close = !x.confirm_close; break;
            }
        });
        return {};
    }
    static Cmd update(Model& m, Choose e) {
        settle(m);
        m.open_drop = 0;
        int* field = nullptr;
        Settings next = *m.s;
        switch (e.c) {
            case Choice::scheme:        field = &next.scheme; break;
            case Choice::app_theme:     field = &next.app_theme; break;
            case Choice::smoothing:     field = &next.smoothing; break;
            case Choice::cursor_shape:  field = &next.cursor_shape; break;
            case Choice::cursor_colour: field = &next.cursor_colour; break;
            case Choice::workdir:       field = &next.workdir; break;
            case Choice::bell:          field = &next.bell; break;
            case Choice::font:          field = &next.font; break;
        }
        if (*field == e.v) return {};   // re-picking the same value is not an undo step
        *field = e.v;
        m.s.set(std::move(next));
        return {};
    }
    static Cmd update(Model& m, Step e) {
        settle(m);
        Settings next = *m.s;
        switch (e.n) {
            case Num::font_size:  next.font_size = std::clamp(next.font_size + e.dir, 8, 32); break;
            case Num::padding:    next.padding = std::clamp(next.padding + 2 * e.dir, 0, 40); break;
            case Num::scrollback: next.scrollback = std::clamp(next.scrollback + e.dir, 0,
                                                               static_cast<int>(kScrollback.size()) - 1); break;
        }
        if (next != *m.s) m.s.set(std::move(next));
        return {};
    }
    static Cmd update(Model& m, SetFrac e) {
        settle(m);
        const float f = std::clamp(e.frac, 0.0f, 1.0f);
        m.last_slider = static_cast<int>(e.s);
        // One drag is ONE undo step: the group coalesces every move of the
        // gesture, and EndDrag breaks it so the next drag is a new step.
        if (e.s == Slide::opacity) {
            const float v = std::round((0.30f + 0.70f * f) * 100.0f) / 100.0f;
            if (v != m.s->opacity) m.s.edit([&](Settings& x) { x.opacity = v; }, "opacity");
        } else {
            const float v = std::round((1.0f + f) * 20.0f) / 20.0f;
            if (v != m.s->line_height) m.s.edit([&](Settings& x) { x.line_height = v; }, "line-height");
        }
        return {};
    }
    static Cmd update(Model& m, Nudge e) {
        if (m.last_slider == static_cast<int>(Slide::opacity)) {
            m.s.edit([&](Settings& x) {
                x.opacity = std::clamp(std::round((x.opacity + 0.05f * static_cast<float>(e.dir)) * 100.0f) / 100.0f, 0.30f, 1.0f);
            });
        } else if (m.last_slider == static_cast<int>(Slide::line_height)) {
            m.s.edit([&](Settings& x) {
                x.line_height = std::clamp(std::round((x.line_height + 0.05f * static_cast<float>(e.dir)) * 20.0f) / 20.0f, 1.0f, 2.0f);
            });
        }
        return {};
    }
    static Cmd update(Model& m, EndDrag) { m.s.break_coalescing(); return {}; }
    static Cmd update(Model& m, OpenDrop e) {
        settle(m);
        m.open_drop = m.open_drop == e.id ? 0 : e.id;
        return {};
    }
    static Cmd update(Model& m, CloseDrop) { m.open_drop = 0; return {}; }

    static Cmd update(Model& m, StartRecord e) {
        settle(m);
        m.recording = m.recording == e.index ? -1 : e.index;
        return {};
    }
    static Cmd update(Model& m, Record e) {
        const int i = m.recording;
        if (i < 0) return {};
        const auto& k = e.k;
        if (is_modifier_key(k.key)) return {};               // keep listening
        if (k.key == Key::escape && k.mods.none()) { m.recording = -1; return {}; }
        const Chord ch = (k.key == Key::backspace && k.mods.none()) ? Chord{} : Chord{k.key, k.mods};
        m.recording = -1;
        if (m.s->keys[static_cast<std::size_t>(i)] != ch)
            m.s.edit([&](Settings& x) { x.keys[static_cast<std::size_t>(i)] = ch; });
        return {};
    }
    static Cmd update(Model& m, ResetKey e) {
        settle(m);
        const auto i = static_cast<std::size_t>(e.index);
        if (m.s->keys[i] != kActions[i].fallback)
            m.s.edit([&](Settings& x) { x.keys[i] = kActions[i].fallback; });
        return {};
    }

    static Cmd update(Model& m, StartEdit) {
        m.recording = -1;
        m.open_drop = 0;
        if (!m.editing_shell) { m.editing_shell = true; m.shell_edit.select_all(); }
        return FocusNode{node_id("shell")};
    }
    static Cmd update(Model& m, Typed e) {
        if (m.editing_shell) m.shell_edit.insert(e.s);
        return {};
    }
    static Cmd update(Model& m, EditKey e) {
        if (!m.editing_shell) return {};
        if (e.k.key == Key::escape) {          // cancel: back to the saved value
            m.editing_shell = false;
            resync(m);
            return {};
        }
        if (e.k.key == Key::enter) { settle(m); return {}; }
        (void)m.shell_edit.handle_key(e.k.key, e.k.mods);
        return {};
    }

    static Cmd update(Model& m, Undo) { settle(m); if (m.s.undo()) resync(m); return {}; }
    static Cmd update(Model& m, Redo) { settle(m); if (m.s.redo()) resync(m); return {}; }
    static Cmd update(Model& m, Save) { settle(m); m.saved = *m.s; return {}; }
    static Cmd update(Model& m, Revert) {
        settle(m);
        if (m.saved != *m.s) { m.s.set(m.saved); resync(m); }
        return {};
    }
    static Cmd update(Model& m, Reset) {
        settle(m);
        if (defaults() != *m.s) { m.s.set(defaults()); resync(m); }
        return {};
    }
    static Cmd update(Model& m, ScrollBy e) { m.scroll.scroll_by(Vec2{0.0f, e.dy}); return {}; }
    static Cmd update(Model&, Tick) { return {}; }   // the host steps Tracked itself
    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    // ── derived ─────────────────────────────────────────────────────────

    [[nodiscard]] static bool dirty(const Model& m) { return m.saved != *m.s; }

    /// Which shortcuts collide. A conflict is shown on BOTH rows, because
    /// either one is the thing the user may want to change.
    [[nodiscard]] static std::array<bool, kActions.size()> conflicts(const Settings& s) {
        std::array<bool, kActions.size()> out{};
        for (std::size_t i = 0; i < s.keys.size(); ++i)
            for (std::size_t j = i + 1; j < s.keys.size(); ++j)
                if (!s.keys[i].empty() && s.keys[i] == s.keys[j]) out[i] = out[j] = true;
        return out;
    }

    // ── view ────────────────────────────────────────────────────────────

    /// The controls take any ControlContext. This one is the frame's Ctx with
    /// the theme the USER picked, which is how a settings window can re-skin
    /// itself live without the host knowing.
    struct Look {
        const Ctx& c;
        Theme      theme;
        [[nodiscard]] bool hovered(std::uint64_t n) const noexcept { return c.hovered(n); }
        [[nodiscard]] bool pressed(std::uint64_t n) const noexcept { return c.pressed(n); }
        [[nodiscard]] bool focused(std::uint64_t n) const noexcept { return c.focused(n); }
        void overlay(Overlay o) const { c.overlay(std::move(o)); }
    };
    static_assert(ControlContext<Look>);

    static constexpr std::array<const char*, page_count> kPageTitle{
        "Appearance", "Text", "Cursor", "Shell", "Keyboard", "Config file"};
    static constexpr std::array<const char*, page_count> kPageIcon{
        "\u25D0", "A", "\u258D", "$", "\u2318", "\u2261"};
    static constexpr std::array<const char*, page_count> kPageBlurb{
        "Colour scheme, transparency and the window around the text.",
        "The face, size and rhythm of the terminal grid.",
        "How the insertion point looks and behaves.",
        "What runs in a new tab, and how the terminal talks back.",
        "Click a shortcut, then press the new keys. Backspace clears, Esc cancels.",
        "Exactly what Save writes. Generated from the same model the window draws."};

    /// The terminal mock: every visual setting, rendered.
    static Node preview(const Look& L, const Model& m) {
        const Settings& s = *m.s;
        const Scheme& sc = kSchemes[static_cast<std::size_t>(s.scheme)];
        const auto ansi = [&](int i) { return hx(sc.ansi[static_cast<std::size_t>(i)]); };
        const float fs = static_cast<float>(s.font_size);
        const float lh = std::round(fs * s.line_height);
        const float cw = std::round(fs * 0.62f);   // one terminal cell
        const auto fgc = hx(sc.fg);

        // A terminal lays text out on a grid of identical cells, whatever the
        // face. Each character gets its own cell box, so columns line up even
        // though the preview is drawn with the UI font.
        using Run = std::vector<Node>;
        const auto seg = [&](std::string_view str, Color<Srgb> col, bool strong = false) {
            const Color<Srgb> c = strong && s.bold_bright ? mix(col, colors::white, 0.25f) : col;
            Run cells;
            for (char ch : str) {
                if (ch == ' ') { cells.push_back((box() | size(cw, lh)).build()); continue; }
                cells.push_back((h(text_owned(std::string(1, ch)) | font(fs) | fg(c)
                                   | (strong ? bold : weight(FontWeight::regular)))
                                 | size(cw, lh) | center).build());
            }
            return cells;
        };
        const auto line = [&](std::initializer_list<Run> runs) {
            Run all;
            for (const auto& r : runs) all.insert(all.end(), r.begin(), r.end());
            return h_list(std::move(all)).build();
        };
        const auto prompt = [&] {
            Run p = seg("~/src/mayag", ansi(4), true);
            const Run b = seg(" main", ansi(5)), arrow = seg(" $ ", ansi(2), true);
            p.insert(p.end(), b.begin(), b.end());
            p.insert(p.end(), arrow.begin(), arrow.end());
            return p;
        };

        const Color<Srgb> cur = s.cursor_colour == 0 ? hx(sc.cursor) : ansi(s.cursor_colour);
        Node cursor_node =
            s.cursor_shape == 0 ? (box() | size(cw, lh) | bg(cur)).build()
          : s.cursor_shape == 1 ? (box() | size(2, lh) | bg(cur)).build()
          : (z(box() | size(cw, 2) | bg(cur) | absolute(0, lh - 2)) | size(cw, lh)).build();

        std::vector<Node> lines;
        lines.push_back(line({prompt(), seg("git status -sb", fgc)}));
        lines.push_back(line({seg("## main", ansi(2)), seg("...", fgc), seg("origin/main", ansi(1))}));
        lines.push_back(line({seg(" M ", ansi(1)), seg("include/mayag/widget/slider.hpp", fgc)}));
        lines.push_back(line({seg("?? ", ansi(1)), seg("examples/terminal_settings.cpp", fgc)}));
        lines.push_back(line({prompt(), seg("ls -la | wc -l", fgc)}));
        lines.push_back(line({seg("      42", ansi(3), true)}));
        lines.push_back(line({prompt(), Run{std::move(cursor_node)}}));

        std::vector<Node> pal;
        for (int i = 0; i < 16; ++i)
            pal.push_back((box() | size(14, 14) | bg(ansi(i)) | radius(3)).build());

        const Theme& t = L.theme;
        auto chrome = h(dot(hx(0xFF5F57), 11), dot(hx(0xFEBC2E), 11), dot(hx(0x28C840), 11),
                        spacer(),
                        text_owned(m.s->shell + "  \u2014  " +
                                   m.fonts[static_cast<std::size_t>(s.font)] + " " +
                                   std::to_string(s.font_size))
                            | font(11) | fg(fgc.fade(0.6f)),
                        spacer(), strut(45))
                    | gap(7) | align(Align::center) | pad(9, 12) | width(pct(100));

        auto term = v(chrome,
                      v(v_list(std::move(lines))) | pad(static_cast<float>(s.padding)) | width(pct(100)),
                      h(h_list(std::move(pal), 4.0f)) | pad(0, static_cast<float>(s.padding), 12,
                                                             static_cast<float>(s.padding)))
                  | width(pct(100))
                  | bg(hx(sc.bg).fade(s.opacity))
                  | backdrop_blur(s.blur ? 24.0f : 0.0f)
                  | border(1, colors::white.fade(0.08f))
                  | radius(10) | clip | elevation(10);

        // A wallpaper behind it, so opacity and blur have something to show.
        return (v(term) | pad(22) | width(pct(100))
                | linear_gradient(mix(t.accent, hx(0xFF6AA2), 0.35f), hx(0x1D976C), {0, 0}, {1, 1})
                | radius(t.radius_medium) | border(1, t.border)).build();
    }

    static Node scheme_card(const Look& L, const Model& m, std::size_t i) {
        const Theme& t = L.theme;
        const Scheme& sc = kSchemes[i];
        const auto pid = part_id("scheme", i);
        const bool on = static_cast<int>(i) == m.s->scheme;
        const bool hov = L.hovered(pid);
        std::vector<Node> dots;
        for (int k = 1; k < 7; ++k) dots.push_back((box() | size(10, 10) | bg(hx(sc.ansi[static_cast<std::size_t>(k)])) | radius(5)).build());
        return (v(text<"$ ls"> | font(11) | fg(hx(sc.fg)),
                  h(h_list(std::move(dots), 3.0f)),
                  text_owned(sc.name) | font(11) | semibold | fg(hx(sc.fg)) | ellipsis)
                | gap(6) | pad(10) | size(122, 76)
                | bg(hx(sc.bg))
                | border(on ? 2.5f : 1.0f, on ? t.accent : (L.focused(pid) ? t.focus_ring
                                                         : hov ? t.border_strong : t.border),
                         on ? StrokeAlign::outside : StrokeAlign::inside)
                | radius(t.radius_small) | pointer_cursor
                | role(a11y::Role::radio) | checked(on) | label(sc.name)
                | id_of("scheme", i)).build();
    }

    static Node page_body(const Look& L, const Model& m) {
        const Theme& t = L.theme;
        const Settings& s = *m.s;
        char buf[32];
        switch (m.page) {
            case appearance: {
                std::vector<Node> cards;
                for (std::size_t i = 0; i < kSchemes.size(); ++i) cards.push_back(scheme_card(L, m, i));
                std::vector<Node> themes_;
                for (std::size_t i = 0; i < kThemes.size(); ++i)
                    themes_.push_back(swatch(L, "apptheme", i, kThemes[i].accent,
                                             static_cast<int>(i) == s.app_theme, 24).build());
                std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(s.opacity * 100.0f)));
                return v_list({preview(L, m),
                               section(L, "COLOUR SCHEME", {
                                   (h(h_list(std::move(cards), 10.0f)) | wrap | gap(10) | pad(16)
                                    | width(pct(100))).build()}),
                               section(L, "WINDOW", {
                                   setting_row(L, "Background opacity", buf,
                                               slider(L, "opacity", (s.opacity - 0.30f) / 0.70f, kTrack)),
                                   setting_row(L, "Blur behind window",
                                               "Frosted glass when the background is translucent.",
                                               switch_control(L, "blur", s.blur)),
                                   setting_row(L, "Padding", "Space between the window edge and the grid.",
                                               stepper(L, "padding", std::to_string(s.padding) + " px",
                                                       s.padding <= 0, s.padding >= 40)),
                                   setting_row(L, "Settings theme", kThemeNames[static_cast<std::size_t>(s.app_theme)],
                                               h(h_list(std::move(themes_), 8.0f))),
                               })}, 22.0f).build();
            }
            case text_page: {
                std::snprintf(buf, sizeof buf, "%.2f\u00D7", static_cast<double>(s.line_height));
                return v_list({preview(L, m),
                               section(L, "FONT", {
                                   setting_row(L, "Family", "Installed monospace faces.",
                                               dropdown(L, "font", m.fonts, s.font,
                                                        m.open_drop == node_id("font"), 220)),
                                   setting_row(L, "Size", "Points. Cmd/Ctrl + = and - change it live.",
                                               stepper(L, "fontsize", std::to_string(s.font_size) + " pt",
                                                       s.font_size <= 8, s.font_size >= 32)),
                                   setting_row(L, "Line height", buf,
                                               slider(L, "lineheight", s.line_height - 1.0f, kTrack)),
                               }),
                               section(L, "RENDERING", {
                                   setting_row(L, "Ligatures", "Join => != === into single glyphs, where the face has them.",
                                               switch_control(L, "ligatures", s.ligatures)),
                                   setting_row(L, "Bold text uses bright colours",
                                               "The old xterm convention some programs still expect.",
                                               checkbox(L, "boldbright", s.bold_bright, "Brighten bold")),
                                   setting_row(L, "Antialiasing", "Subpixel is sharpest on low-DPI panels.",
                                               segmented(L, "smoothing", {"None", "Grayscale", "Subpixel"},
                                                         s.smoothing)),
                               })}, 22.0f).build();
            }
            case cursor_page: {
                const Scheme& sc = kSchemes[static_cast<std::size_t>(s.scheme)];
                std::vector<Node> cols;
                for (std::size_t i = 0; i < 7; ++i)
                    cols.push_back(swatch(L, "cursorcol", i, i == 0 ? hx(sc.cursor) : hx(sc.ansi[i]),
                                          static_cast<int>(i) == s.cursor_colour, 24).build());
                const Color<Srgb> cur = s.cursor_colour == 0 ? hx(sc.cursor)
                                                             : hx(sc.ansi[static_cast<std::size_t>(s.cursor_colour)]);
                return v_list({preview(L, m),
                               section(L, "CURSOR", {
                                   setting_row(L, "Shape", "Block, vertical bar or underline.",
                                               segmented(L, "cursorshape", {"\u2588 Block", "\u258F Beam", "\u2581 Underline"},
                                                         s.cursor_shape)),
                                   setting_row(L, "Blink", "Stops blinking after 15 s idle to save power.",
                                               switch_control(L, "blink", s.cursor_blink)),
                                   setting_row(L, "Colour", s.cursor_colour == 0 ? "From the colour scheme."
                                                                                  : hex(cur),
                                               h(h_list(std::move(cols), 8.0f))),
                               })}, 22.0f).build();
            }
            case shell_page: {
                const int sb = kScrollback[static_cast<std::size_t>(s.scrollback)];
                const bool field_focus = m.editing_shell;
                return v_list({
                    section(L, "STARTUP", {
                        setting_row(L, "Shell", "Login shell to run in new tabs. Enter to apply.",
                                    node((text_field(t, m.shell_edit, field_focus, node_id("shell"),
                                                     "/bin/zsh")
                                          | width(240)
                                          | role(a11y::Role::text_field) | label("Shell")).build())),
                        setting_row(L, "Working directory", "Where a new tab starts.",
                                    dropdown(L, "workdir",
                                             {"Home folder", "Same as current tab", "Project root"},
                                             s.workdir, m.open_drop == node_id("workdir"), 220)),
                        setting_row(L, "Scrollback", "Lines kept above the screen.",
                                    stepper(L, "scrollback", sb == 0 ? std::string{"\u221E"} : std::to_string(sb),
                                            s.scrollback <= 0,
                                            s.scrollback >= static_cast<int>(kScrollback.size()) - 1)),
                    }),
                    section(L, "BEHAVIOUR", {
                        setting_row(L, "Bell", "What happens on \\a.",
                                    radio_group(L, "bell", {"Off", "Flash the window", "Play a sound"},
                                                s.bell)),
                        setting_row(L, "Copy on select", "Selecting text puts it on the clipboard.",
                                    switch_control(L, "copyselect", s.copy_on_select)),
                        setting_row(L, "Option key as Meta", "Send Esc-prefixed keys instead of special characters.",
                                    switch_control(L, "optionmeta", s.option_as_meta)),
                        setting_row(L, "Confirm before closing", "Only when a process is still running.",
                                    checkbox(L, "confirmclose", s.confirm_close, "Ask first")),
                    })}, 22.0f).build();
            }
            case keys_page: {
                const auto clash = conflicts(s);
                std::vector<Node> rows;
                for (std::size_t i = 0; i < kActions.size(); ++i) {
                    const bool changed = s.keys[i] != kActions[i].fallback;
                    const auto rid = part_id("keyreset", i);
                    rows.push_back((h(text_owned(kActions[i].name) | font(t.font_size) | fg(t.text_primary) | grow(),
                                      text_owned(clash[i] ? "Conflict" : "") | font(11) | semibold | fg(t.danger),
                                      h(text_owned(changed ? "\u21BA" : " ") | font(t.font_size)
                                            | fg(L.hovered(rid) ? t.text_primary : t.text_secondary))
                                          | size(24, 24) | center | radius(4)
                                          | pointer_cursor | label("Reset shortcut")
                                          | id_of("keyreset", i),
                                      node(keybind(L, "key", i, s.keys[i], m.recording == static_cast<int>(i),
                                                   clash[i]).build()))
                                    | gap(10) | align(Align::center) | pad(8, 18) | width(pct(100))).build());
                }
                return v_list({section(L, "SHORTCUTS", std::move(rows))}).build();
            }
            default: {
                std::vector<Node> lines;
                const auto ln = [&](std::string str, Color<Srgb> col) {
                    lines.push_back((text_owned(std::move(str)) | font(12.5f) | fg(col) | no_ligatures
                                     | line_height(1.5f)).build());
                };
                const auto kv = [&](const char* k, std::string val) {
                    lines.push_back((h(text_owned(k) | font(12.5f) | fg(t.text_primary) | width(150),
                                       text_owned("= " + std::move(val)) | font(12.5f) | no_ligatures
                                           | fg(t.text_secondary))
                                     | height(19) | align(Align::center)).build());
                };
                const auto b = [](bool x) { return std::string{x ? "true" : "false"}; };
                std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(s.opacity));
                ln("# ~/.config/term/config.toml", t.text_secondary);
                ln("[appearance]", t.accent);
                kv("color_scheme", std::string{"\""} + kSchemes[static_cast<std::size_t>(s.scheme)].name + "\"");
                kv("opacity", buf);
                kv("blur", b(s.blur));
                kv("padding", std::to_string(s.padding));
                ln("[font]", t.accent);
                kv("family", "\"" + m.fonts[static_cast<std::size_t>(s.font)] + "\"");
                kv("size", std::to_string(s.font_size));
                std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(s.line_height));
                kv("line_height", buf);
                kv("ligatures", b(s.ligatures));
                kv("antialias", std::array{"\"none\"", "\"grayscale\"", "\"subpixel\""}[static_cast<std::size_t>(s.smoothing)]);
                ln("[cursor]", t.accent);
                kv("shape", std::array{"\"block\"", "\"beam\"", "\"underline\""}[static_cast<std::size_t>(s.cursor_shape)]);
                kv("blink", b(s.cursor_blink));
                ln("[shell]", t.accent);
                kv("program", "\"" + s.shell + "\"");
                kv("scrollback", std::to_string(kScrollback[static_cast<std::size_t>(s.scrollback)]));
                kv("bell", std::array{"\"off\"", "\"visual\"", "\"audible\""}[static_cast<std::size_t>(s.bell)]);
                kv("copy_on_select", b(s.copy_on_select));
                kv("option_as_meta", b(s.option_as_meta));
                ln("[keys]", t.accent);
                for (std::size_t i = 0; i < kActions.size(); ++i)
                    kv(kActions[i].key, "\"" + chord_text(s.keys[i]) + "\"");
                return (v(v_list(std::move(lines)))
                        | pad(18, 22) | width(pct(100))
                        | bg(t.surface) | border(1, t.border) | radius(t.radius_medium)).build();
            }
        }
    }

    static Node view(const Model& m) {
        const Ctx& c = ctx();
        Theme theme = kThemes[static_cast<std::size_t>(m.s->app_theme)];
        theme.font_size = 13.5f;
        const Look L{c, theme};
        const Theme& t = L.theme;

        // ── sidebar ─────────────────────────────────────────────────────
        std::vector<Node> nav;
        for (std::size_t i = 0; i < page_count; ++i)
            nav.push_back(nav_item(L, "nav", i, kPageIcon[i], kPageTitle[i], m.page == static_cast<int>(i)));
        c.track(m.nav, part_id("nav", static_cast<std::size_t>(m.page)));

        auto sidebar = v(text<"Settings"> | font(19) | bold | fg(t.text_primary) | pad(4, 10)
                             | role(a11y::Role::heading),
                         v(v_list(std::move(nav), 2.0f)) | width(pct(100)),
                         spacer(),
                         text<"mayag \u00B7 terminal"> | font(11) | fg(t.text_disabled) | pad(0, 10))
                     | gap(14) | pad(22, 12) | width(212) | height(pct(100))
                     | bg(t.surface) | border(1, t.border);

        // ── header ──────────────────────────────────────────────────────
        const bool can_undo = m.s.can_undo(), can_redo = m.s.can_redo(), unsaved = dirty(m);
        auto header =
            h(v(text_owned(kPageTitle[static_cast<std::size_t>(m.page)]) | font(22) | bold
                    | fg(t.text_primary) | role(a11y::Role::heading),
                text_owned(kPageBlurb[static_cast<std::size_t>(m.page)]) | font(12.5f)
                    | fg(t.text_secondary) | wrap_text)
                  | gap(4) | grow() | min_size(0, 0),
              node(unsaved ? (badge<"Unsaved">(t, t.warning)).build() : Node{}),
              button<"Undo">(t, ButtonVariant::ghost) | opacity(can_undo ? 1.0f : 0.35f) | dsl::id<"undo">,
              button<"Redo">(t, ButtonVariant::ghost) | opacity(can_redo ? 1.0f : 0.35f) | dsl::id<"redo">,
              button<"Defaults">(t, ButtonVariant::secondary) | dsl::id<"reset">,
              button<"Revert">(t, ButtonVariant::secondary) | opacity(unsaved ? 1.0f : 0.35f) | dsl::id<"revert">,
              button<"Save">(t, ButtonVariant::primary) | opacity(unsaved ? 1.0f : 0.45f) | dsl::id<"save">)
            | gap(8) | align(Align::center) | pad(22, 32, 16, 32) | width(pct(100));

        auto content = v(header,
                         v(node(page_body(L, m))) | pad(4, 32, 40, 32) | width(pct(100))
                             | scroll(m.scroll) | grow() | dsl::id<"content">)
                     | grow() | height(pct(100));

        // The sliding nav marker lives in the root z-stack, whose origin is
        // the window's, so the Tracked rect (window coordinates) can be used
        // as-is.
        Node marker = m.nav.ready()
            ? (box() | size(3, m.nav.rect().height() - 14) | bg(t.accent) | radius(2)
               | absolute(m.nav.rect().left() + 2, m.nav.rect().top() + 7)).build()
            : Node{};

        return z(h(sidebar, content) | width(pct(100)) | height(pct(100)) | bg(t.background),
                 node(std::move(marker)))
             | width(pct(100)) | height(pct(100));
    }

    // ── input ───────────────────────────────────────────────────────────

    /// One mapping for pointer clicks AND keyboard activation (Tab to a
    /// control, Enter/Space), so every control is keyboard-operable for free.
    static std::optional<Msg> activate(const Model& m, std::uint64_t n) {
        const auto eq = [n](std::string_view name) { return n == node_id(name); };
        const auto part = [n](std::string_view base, std::size_t count) -> int {
            for (std::size_t i = 0; i < count; ++i) if (n == part_id(base, i)) return static_cast<int>(i);
            return -1;
        };
        if (int i = part("nav", page_count); i >= 0) return Go{i};
        if (eq("undo")) return Undo{};
        if (eq("redo")) return Redo{};
        if (eq("save")) return Save{};
        if (eq("revert")) return Revert{};
        if (eq("reset")) return Reset{};
        if (eq("blur")) return Flip{Flag::blur};
        if (eq("ligatures")) return Flip{Flag::ligatures};
        if (eq("boldbright")) return Flip{Flag::bold_bright};
        if (eq("blink")) return Flip{Flag::cursor_blink};
        if (eq("copyselect")) return Flip{Flag::copy_on_select};
        if (eq("optionmeta")) return Flip{Flag::option_as_meta};
        if (eq("confirmclose")) return Flip{Flag::confirm_close};
        if (int i = part("scheme", kSchemes.size()); i >= 0) return Choose{Choice::scheme, i};
        if (int i = part("apptheme", kThemes.size()); i >= 0) return Choose{Choice::app_theme, i};
        if (int i = part("smoothing", 3); i >= 0) return Choose{Choice::smoothing, i};
        if (int i = part("cursorshape", 3); i >= 0) return Choose{Choice::cursor_shape, i};
        if (int i = part("cursorcol", 7); i >= 0) return Choose{Choice::cursor_colour, i};
        if (int i = part("bell", 3); i >= 0) return Choose{Choice::bell, i};
        if (int i = part("font", m.fonts.size()); i >= 0) return Choose{Choice::font, i};
        if (int i = part("workdir", 3); i >= 0) return Choose{Choice::workdir, i};
        if (eq("font")) return OpenDrop{node_id("font")};
        if (eq("workdir")) return OpenDrop{node_id("workdir")};
        if (n == part_id("fontsize", part_dec)) return Step{Num::font_size, -1};
        if (n == part_id("fontsize", part_inc)) return Step{Num::font_size, +1};
        if (n == part_id("padding", part_dec)) return Step{Num::padding, -1};
        if (n == part_id("padding", part_inc)) return Step{Num::padding, +1};
        if (n == part_id("scrollback", part_dec)) return Step{Num::scrollback, -1};
        if (n == part_id("scrollback", part_inc)) return Step{Num::scrollback, +1};
        if (int i = part("key", kActions.size()); i >= 0) return StartRecord{i};
        if (int i = part("keyreset", kActions.size()); i >= 0) return ResetKey{i};
        if (eq("shell")) return StartEdit{};
        return std::nullopt;
    }

    static std::optional<Msg> on(const Model& m, const Clicked& c) {
        if (c.button != MouseButton::left) return std::nullopt;
        if (auto msg = activate(m, c.node)) return msg;
        // A click on nothing in particular still ends recording / editing.
        if (m.recording >= 0 || m.editing_shell) return Go{m.page};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const Activated& a) {
        // While the shell field is being edited, Enter/Space on it are text
        // keys. Declining here lets the host pass the raw key through.
        if (m.editing_shell && a.node == node_id("shell")) return std::nullopt;
        return activate(m, a.node);
    }

    static std::optional<Msg> on(const Model&, const Pressed& p) {
        if (is(p, "opacity")) return SetFrac{Slide::opacity, p.local.x / kTrack};
        if (is(p, "lineheight")) return SetFrac{Slide::line_height, p.local.x / kTrack};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Dragged& d) {
        if (is(d, "opacity")) return SetFrac{Slide::opacity, d.local.x / kTrack};
        if (is(d, "lineheight")) return SetFrac{Slide::line_height, d.local.x / kTrack};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const Released&) { return EndDrag{}; }

    // A click outside an open dropdown dismisses its overlay and reports Left.
    static std::optional<Msg> on(const Model& m, const Left& l) {
        if (m.open_drop != 0 &&
            (l.node == part_id("font", part_menu) || l.node == part_id("workdir", part_menu)))
            return CloseDrop{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::Scrolled& s) {
        if (is(s, "content") && m.open_drop == 0) return ScrollBy{s.delta.y};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const TextEvent& e) {
        if (m.editing_shell) return Typed{e.text};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        if (m.recording >= 0) return Record{k};             // the recorder sees EVERYTHING
        if (k.mods.primary() && k.key == Key::z) return k.mods.shift ? Msg{Redo{}} : Msg{Undo{}};
        if (k.mods.primary() && k.key == Key::s) return Save{};
        if (k.mods.primary() && k.key == Key::w) return Quit{};
        if (m.editing_shell) return EditKey{k};
        if (k.key == Key::escape && m.open_drop != 0) return CloseDrop{};
        if (k.mods.none() && (k.key == Key::left || k.key == Key::right))
            return Nudge{k.key == Key::left ? -1 : 1};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }

    // ── subscribe ───────────────────────────────────────────────────────

    // Frames only while the nav marker slides; otherwise 0% CPU.
    static Sub subscribe(const Model& m) {
        return when(m.nav.animating(), Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }
};

static_assert(mayag::Program<TerminalSettings>, "TerminalSettings must satisfy Program");

int main(int argc, char** argv) {
    const auto opts = demo::parse_args(argc, argv, Vec2{1120, 780});
    auto fonts = demo::make_fonts(opts);

    AppConfig cfg{
        .title = "Settings",
        .size  = opts.size,
        .theme = themes::midnight,
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
        const auto& S = [&]() -> const TS::Settings& { return *rt.model().s; };
        // A user scrolls a control into view before clicking it; so does the
        // script. Wheel events over the content area, like a trackpad.
        const auto reveal = [&](std::uint64_t target) {
            for (int i = 0; i < 40; ++i) {
                const auto r = rt.node_rect(target);
                if (!r || (r->top() >= 0 && r->bottom() <= 760)) return;
                rt.window().scroll(Vec2{700, 400}, Vec2{0, r->bottom() > 760 ? -120.0f : 120.0f});
                rt.tick();
            }
        };
        rt.tick();
        rt.tick();

        // ── appearance ──────────────────────────────────────────────────
        ok(rt.node_rect("blur").has_value(), "the appearance page lays out its switch");
        reveal(part_id("scheme", 1));
        rt.click(part_id("scheme", 1));
        ok(S().scheme == 1, "clicking a scheme card selects it");
        ok(TS::dirty(rt.model()), "and the window now has unsaved changes");

        reveal(part_id("padding", part_inc));
        rt.click("blur");
        ok(!S().blur, "the switch flips blur off");
        rt.window().press_key(Key::space);
        rt.tick();
        ok(S().blur, "Space on the focused switch flips it back (keyboard activation)");

        if (auto r = rt.node_rect("opacity")) {
            rt.window().click(Vec2{r->left() + r->width() * 0.5f, r->top() + 12});
            rt.tick();
            ok(std::abs(S().opacity - 0.65f) < 0.011f,
               "clicking the slider's middle sets opacity to 65% (got " + std::to_string(S().opacity) + ")");
        } else ok(false, "opacity slider laid out");

        rt.click(part_id("padding", part_inc));
        ok(S().padding == 14, "the stepper's + adds 2 px of padding");

        // ── undo / redo ─────────────────────────────────────────────────
        rt.press_key(Key::z, primary_mods());
        ok(S().padding == 12, "undo takes back the padding step");
        rt.press_key(Key::z, primary_mods(true));
        ok(S().padding == 14, "redo puts it back");

        // ── text: dropdown in an overlay ────────────────────────────────
        rt.click(part_id("nav", TS::text_page));
        ok(rt.model().page == TS::text_page, "the sidebar switches pages");
        reveal(node_id("font"));
        rt.click("font");
        ok(rt.model().open_drop == node_id("font"), "clicking the font field opens the dropdown");
        rt.tick();
        if (rt.model().fonts.size() > 1) {
            rt.click(part_id("font", 1));
            ok(S().font == 1 && rt.model().open_drop == 0,
               "picking an option from the overlay selects it and closes the list");
        }
        rt.click("font");
        rt.tick();
        rt.window().click(Vec2{1100, 760});
        rt.tick();
        ok(rt.model().open_drop == 0, "a click outside dismisses the dropdown");

        rt.click(part_id("fontsize", part_inc));
        ok(S().font_size == 14, "font size steps up");
        reveal(part_id("smoothing", 2));
        rt.click(part_id("smoothing", 2));
        ok(S().smoothing == 2, "the segmented control selects Subpixel");

        // ── cursor ──────────────────────────────────────────────────────
        rt.click(part_id("nav", TS::cursor_page));
        reveal(part_id("cursorcol", 3));
        rt.click(part_id("cursorshape", 1));
        ok(S().cursor_shape == 1, "cursor shape: beam");
        rt.click(part_id("cursorcol", 3));
        ok(S().cursor_colour == 3, "cursor colour swatch");

        // ── shell: text field ───────────────────────────────────────────
        rt.click(part_id("nav", TS::shell_page));
        reveal(node_id("shell"));
        rt.click("shell");
        ok(rt.model().editing_shell, "clicking the shell field starts editing");
        rt.type("/opt/homebrew/bin/fish");
        rt.press_key(Key::enter);
        ok(S().shell == "/opt/homebrew/bin/fish", "typing replaces the selection and Enter applies it");
        reveal(part_id("bell", 2));
        rt.click(part_id("bell", 2));
        ok(S().bell == 2, "radio group: audible bell");
        reveal(part_id("scrollback", part_inc));
        rt.click(part_id("scrollback", part_inc));
        ok(S().scrollback == 3, "scrollback steps through its presets");

        // ── keyboard: recorder + conflicts ──────────────────────────────
        rt.click(part_id("nav", TS::keys_page));
        reveal(part_id("key", 0));
        rt.click(part_id("key", 0));
        ok(rt.model().recording == 0, "clicking a shortcut starts recording");
        rt.press_key(Key::shift, Mods{.shift = true});
        ok(rt.model().recording == 0, "a bare modifier keeps the recorder listening");
        rt.press_key(Key::w, primary_mods());
        ok(rt.model().recording == -1 && S().keys[0] == (Chord{Key::w, primary_mods()}),
           "the next chord is recorded (and does NOT quit the app)");
        ok(TS::conflicts(S())[0] && TS::conflicts(S())[1], "a duplicate shortcut flags both rows");
        rt.click(part_id("keyreset", 0));
        ok(!TS::conflicts(S())[0], "the row's reset restores the default and clears the conflict");

        // ── save / revert / defaults ────────────────────────────────────
        rt.click("save");
        ok(!TS::dirty(rt.model()), "Save makes the window clean");
        rt.click(part_id("nav", TS::appearance));
        reveal(node_id("blur"));
        rt.click("blur");
        ok(TS::dirty(rt.model()), "a change after saving is dirty again");
        rt.click("revert");
        ok(!TS::dirty(rt.model()) && S().blur, "Revert returns to the saved settings");
        rt.click("reset");
        ok(S() == TS::defaults(), "Defaults restores every setting");
        rt.press_key(Key::z, primary_mods());
        ok(S().shell == "/opt/homebrew/bin/fish", "and even that is one undo step");
        ok(rt.model().shell_edit.text == S().shell, "the shell field follows undo");

        // ── idle ────────────────────────────────────────────────────────
        rt.click(part_id("nav", TS::file_page));
        rt.window().drive_frames(true);
        for (int i = 0; i < 300 && rt.model().nav.animating(); ++i) rt.tick();
        rt.window().drive_frames(false);
        ok(!rt.model().nav.animating(), "the nav marker settles");
        ok(TS::subscribe(rt.model()).is_none(), "and a settled window requests no frames");

        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        if (fails > 0) std::exit(1);
    });
}
