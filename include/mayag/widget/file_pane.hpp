#pragma once
// mayag::widget file_pane — the files beside a terminal, local or remote
//
//   ┌────────────────────────────────── ⟳ × ┐
//   │ ⌂ hand                                 │
//   │   ~ / src / hand                       │   crumbs: click one to go there
//   │ ⌖ follows the shell    ⇅ name ⌕ ⇡ >_ ◉ │
//   │ ▸ build                                │
//   │ ▸ src                           ●      │   git: something inside changed
//   │   CMakeLists.txt        M   2h  3.1 KB │
//   │   ▔▔▔▔▔▔▔▔▔▔                           │   size: its share of the folder
//   │   README.md                 5d  8.5 KB │
//   │ ◉ README.md ───────────────── 8.5 KB   │   preview: the selection's head
//   │  1 # hand                              │
//   │ ⏎ open  / filter  s sort  ⇥ cd         │
//   └────────────────────────────────────────┘
//
// A terminal's directory as a list: it follows the shell's cwd (OSC 7) until
// you browse elsewhere, opens folders in place, hands a path to the shell
// (cd there, or paste it at the prompt), and for a remote machine moves
// files over SFTP. `/` narrows the list as you type (fuzzy, matches lit);
// `s` cycles the order. Entries may carry git status, an age, and a `fresh`
// glow the app sets when a re-read finds them new or changed.
//
// Data in, ops out: the app lists directories (here or over ssh), reads the
// preview, and runs every FilePaneAction it finds in `state.out`.

#include "control.hpp"
#include "fuzzy.hpp"
#include "icons.hpp"
#include "scroll_view.hpp"
#include "virtual_list.hpp"
#include "../app/handlers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <numeric>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mayag::dsl {

struct FileEntry {
    std::string   name;
    bool          dir = false, link = false;
    std::uint64_t size = 0;
    std::string   when;        ///< as the source tells it ("Nov 18 22:01", "2024-11-19 03:00")
    std::string   target;      ///< a link's target
    std::int64_t  mtime = 0;   ///< unix seconds (0 = unknown): sorting by age, the age column
    char          git = 0;     ///< 'M' 'A' 'D' 'R' 'U'(conflict) '?'(untracked); a folder: what is inside; 0 = clean
    float         fresh = 0.0f;   ///< 0..1: it just appeared or changed (a glow the app fades out)
};

enum class FileSort : std::uint8_t { name, modified, size, type };
inline constexpr const char* file_sort_names[] = {"name", "modified", "size", "type"};

struct FilePaneAction {
    enum class Kind : std::uint8_t {
        list,        ///< show `path` (a folder)
        open,        ///< open a file: locally, in its app; remotely, download it
        cd,          ///< type `cd <path>` into the shell
        paste,       ///< type the path at the prompt
        download, upload, reveal, copy_path,
        follow,      ///< back to the shell's directory, and stay with it
        close,
    };
    Kind        kind = Kind::list;
    std::string path;
};

/// One row as shown: which entry, and which of its characters the filter matched.
struct FileShown {
    std::size_t i = 0;
    std::vector<std::uint16_t> hits;
    int score = 0;
};

struct FilePaneState {
    int           sel = 0;
    ScrollState   scroll;
    bool          show_hidden = false;
    bool          focused = false;          ///< has the keyboard
    bool          filtering = false;        ///< typing narrows the list (/)
    std::string   filter;
    FileSort      sort = FileSort::name;
    std::vector<FilePaneAction> out;

    /// The last file_pane_view answer and what it was computed from: derived
    /// data, like ScrollState's measurements, so `mutable` and never compared.
    /// A scroll, a hover or a selection does not change the order; only the
    /// entries and the four settings do.
    struct ViewMemo {
        std::uint64_t fingerprint = 0;
        std::size_t   size = 0;
        std::string   filter;
        bool          hidden = false, dirs = false, valid = false;
        FileSort      sort = FileSort::name;
        std::shared_ptr<const std::vector<FileShown>> out;
    };
    mutable ViewMemo view_memo;
};
using FilePaneOp = Edit<FilePaneState>;

/// The selected file's head, shown under the list.
struct FilePreview {
    std::string name;
    std::vector<std::string> lines;   ///< its first lines, tabs expanded (empty: binary / unreadable)
    std::string note;                 ///< "8.5 KB", "binary · 3.1 MB"
    bool listing = false;             ///< a folder's entries ("name/" for folders), not lines of text
};

struct FilePaneProps {
    std::string title;           ///< "~/src/hand", "deploy@prod-db:/srv"
    std::string path;            ///< the folder shown (absolute)
    std::string where;           ///< "" local, else the host ("deploy@prod-db")
    bool        where_danger = false;   ///< the host is production: its chip is red, like its tab
    std::string home;            ///< shown as ~ in the crumbs ("" = none)
    std::vector<FileEntry> entries;
    bool        loading = false;
    bool        following = true;   ///< the folder is the shell's
    std::string error, busy, note;  ///< note: a hint when the shell's directory is unknown
    char        sep = '/';
    float       width = 300.0f;
    bool        left = false;       ///< docked on the window's left: its edge and grip face right
    std::int64_t now = 0;           ///< unix seconds, for the age column
    bool        dirs_first = true, git = true, size_bars = true, age = true, type_colors = true;
    std::optional<FilePreview> preview;
    int         preview_lines = 12;
    /// The size of a file name, px (0: 10.5). Everything in the list --
    /// names, sizes, ages, icons, row height, the preview -- scales with it;
    /// the header and toolbar are interface text (the theme's size).
    float       text_size = 0.0f;
    /// The face of names, paths and sizes (an app passes its terminal's or its
    /// interface's role).
    FontFamily  family = FontFamily::mono;
};

namespace file_pane_detail {
inline constexpr float kRow = 26.0f;   ///< a row, at kRefName names
inline constexpr float kRefName = 10.5f;   ///< names when the app gives no text_size
/// The size file names are set in, px.
[[nodiscard]] inline float name_px(const FilePaneProps& p) { return p.text_size > 0.0f ? p.text_size : kRefName; }
/// A row's height for names of `name` px: a name and a size bar with air
/// around them; kRow at the reference size.
[[nodiscard]] inline float row_h(float name) { return std::max(22.0f, std::round(name * 1.9f + 6.0f)); }
[[nodiscard]] inline std::string human(std::uint64_t n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int k = 0;
    while (v >= 1024.0 && k < 4) { v /= 1024.0; ++k; }
    char b[32];
    std::snprintf(b, sizeof b, k == 0 ? "%.0f %s" : v < 10 ? "%.1f %s" : "%.0f %s", v, u[k]);
    return b;
}
[[nodiscard]] inline std::string join(std::string_view dir, std::string_view name, char sep) {
    std::string d{dir};
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += sep;
    return d + std::string{name};
}
[[nodiscard]] inline std::string parent(std::string_view path) {
    std::string p{path};
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const auto s = p.find_last_of("/\\");
    if (s == std::string::npos) return p;
    if (s == 0) return "/";
    if (s == 2 && p[1] == ':') return p.substr(0, 3);   // C:/
    return p.substr(0, s);
}
[[nodiscard]] constexpr char lc(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }
[[nodiscard]] inline bool ci_less(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (lc(a[i]) != lc(b[i])) return lc(a[i]) < lc(b[i]);
    return a.size() < b.size();
}
[[nodiscard]] inline std::string ext_of(std::string_view name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    std::string e{name.substr(dot + 1)};
    for (char& ch : e) ch = lc(ch);
    return e;
}

enum class Kind : std::uint8_t { dir, code, config, doc, image, archive, exec, other };
[[nodiscard]] inline Kind kind_of(const FileEntry& e) {
    if (e.dir) return Kind::dir;
    std::string low = e.name;
    for (char& ch : low) ch = lc(ch);
    const auto is = [](const std::string& x, std::initializer_list<std::string_view> l) {
        return std::find(l.begin(), l.end(), std::string_view{x}) != l.end();
    };
    if (is(low, {"makefile", "cmakelists.txt", "dockerfile", "justfile", "build.ninja", "meson.build", "cargo.toml", "package.json"}))
        return Kind::config;
    if (is(low, {"license", "readme", "changelog", "authors", "copying"})) return Kind::doc;
    const std::string x = ext_of(low);
    if (is(x, {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "hxx", "ipp", "inl", "rs", "go", "py", "js", "mjs", "cjs", "ts", "tsx", "jsx",
               "java", "kt", "swift", "m", "mm", "cs", "rb", "lua", "zig", "nim", "hs", "ml", "ex", "exs", "php", "scala", "dart",
               "vue", "svelte", "glsl", "hlsl", "wgsl", "metal", "sql", "el", "clj", "fs", "jl", "r", "pl"}))
        return Kind::code;
    if (is(x, {"json", "jsonc", "toml", "yaml", "yml", "ini", "cfg", "conf", "vibe", "xml", "cmake", "lock", "env", "gitignore",
               "gitmodules", "gitattributes", "editorconfig", "props", "plist", "mk", "ninja", "nix"}))
        return Kind::config;
    if (is(x, {"md", "markdown", "txt", "rst", "adoc", "org", "pdf", "tex", "doc", "docx", "rtf", "log", "csv", "html", "css"}))
        return Kind::doc;
    if (is(x, {"png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "ico", "tif", "tiff", "psd", "avif", "heic", "ttf", "otf", "woff", "woff2"}))
        return Kind::image;
    if (is(x, {"zip", "tar", "gz", "tgz", "bz2", "xz", "zst", "7z", "rar", "jar", "deb", "rpm", "iso", "dmg", "whl", "nupkg"}))
        return Kind::archive;
    if (is(x, {"exe", "dll", "so", "dylib", "sh", "bash", "zsh", "fish", "ps1", "bat", "cmd", "msi", "bin", "out", "a", "lib", "o", "obj", "pdb"}))
        return Kind::exec;
    return Kind::other;
}

/// "now", "4m", "2h", "5d", "3w", "8mo", "2y".
[[nodiscard]] inline std::string age(std::int64_t now, std::int64_t t) {
    if (t <= 0 || now <= 0) return {};
    const long long s = static_cast<long long>(std::max<std::int64_t>(now - t, 0));
    char b[16];
    if (s < 60) return "now";
    if (s < 3600) std::snprintf(b, sizeof b, "%lldm", s / 60);
    else if (s < 86400) std::snprintf(b, sizeof b, "%lldh", s / 3600);
    else if (s < 86400LL * 14) std::snprintf(b, sizeof b, "%lldd", s / 86400);
    else if (s < 86400LL * 60) std::snprintf(b, sizeof b, "%lldw", s / (86400LL * 7));
    else if (s < 86400LL * 730) std::snprintf(b, sizeof b, "%lldmo", s / (86400LL * 30));
    else std::snprintf(b, sizeof b, "%lldy", s / (86400LL * 365));
    return b;
}

/// The path as clickable steps: {label, the folder it opens}. A home prefix is "~".
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> crumbs(std::string_view path, std::string_view home, char sep) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto is_sep = [](char ch) { return ch == '/' || ch == '\\'; };
    std::string h{home};
    while (h.size() > 1 && is_sep(h.back())) h.pop_back();
    const auto ci_prefix = [&](std::string_view s, std::string_view pre) {
        if (s.size() < pre.size()) return false;
        for (std::size_t i = 0; i < pre.size(); ++i) {
            const bool both_sep = is_sep(s[i]) && is_sep(pre[i]);
            if (!both_sep && lc(s[i]) != lc(pre[i])) return false;
        }
        return true;
    };
    std::string acc;
    std::string_view rest = path;
    if (!h.empty() && ci_prefix(path, h) && (path.size() == h.size() || is_sep(path[h.size()]))) {
        acc = std::string{path.substr(0, h.size())};
        out.push_back({"~", acc});
        rest = path.substr(h.size());
    } else if (!path.empty() && is_sep(path[0])) {
        acc = std::string(1, path[0]);
        out.push_back({"/", acc});
        rest = path.substr(1);
    } else if (path.size() >= 2 && path[1] == ':') {
        acc = std::string{path.substr(0, 2)} + sep;
        out.push_back({std::string{path.substr(0, 2)}, acc});
        rest = path.substr(2);
    }
    std::size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && is_sep(rest[i])) ++i;
        std::size_t j = i;
        while (j < rest.size() && !is_sep(rest[j])) ++j;
        if (j > i) {
            if (!acc.empty() && !is_sep(acc.back())) acc += sep;
            acc += rest.substr(i, j - i);
            out.push_back({std::string{rest.substr(i, j - i)}, acc});
        }
        i = j;
    }
    return out;
}
}  // namespace file_pane_detail

/// The rows in the order shown: dotfiles hidden unless asked for (or the
/// filter starts with "."), then either the filter's best matches first or
/// the chosen sort, folders on top when `dirs_first`.
[[nodiscard]] inline std::vector<FileShown> file_pane_view(const std::vector<FileEntry>& all, const FilePaneState& st,
                                                           bool dirs_first = true) {
    namespace d = file_pane_detail;
    // The order depends only on the entries and four settings; a scroll, a
    // hover or a selection change does not touch it. So the last answer is
    // kept (per thread) and reused while those are the same -- sorting 3000
    // names case-insensitively every frame was most of a scrolling frame's
    // CPU in a big folder, and the key handler sorts again on every key.
    auto& memo = st.view_memo;
    std::uint64_t fp = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t x) { fp = (fp ^ x) * 1099511628211ull; };
    for (const auto& e : all) {
        mix(std::hash<std::string>{}(e.name));
        mix(static_cast<std::uint64_t>(e.mtime) ^ (e.size << 1) ^ static_cast<std::uint64_t>(e.dir));
    }
    if (memo.valid && memo.out && memo.size == all.size() && memo.fingerprint == fp && memo.filter == st.filter &&
        memo.hidden == st.show_hidden && memo.dirs == dirs_first && memo.sort == st.sort)
        return *memo.out;
    const auto remember = [&](std::vector<FileShown> v) {
        memo = {fp, all.size(), st.filter, st.show_hidden, dirs_first, true, st.sort,
                std::make_shared<const std::vector<FileShown>>(v)};
        return v;
    };
    std::vector<FileShown> v;
    v.reserve(all.size());
    const bool dot_query = !st.filter.empty() && st.filter[0] == '.';
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& e = all[i];
        if (!st.show_hidden && !dot_query && !e.name.empty() && e.name[0] == '.') continue;
        if (st.filter.empty()) { v.push_back({i, {}, 0}); continue; }
        auto fm = fuzzy_match(st.filter, e.name);
        if (fm.ok) v.push_back({i, std::move(fm.positions), fm.score});
    }
    if (!st.filter.empty()) {
        std::stable_sort(v.begin(), v.end(), [&](const FileShown& a, const FileShown& b) {
            if (a.score != b.score) return a.score > b.score;
            return d::ci_less(all[a.i].name, all[b.i].name);
        });
        return remember(std::move(v));
    }
    std::stable_sort(v.begin(), v.end(), [&](const FileShown& a, const FileShown& b) {
        const auto& x = all[a.i];
        const auto& y = all[b.i];
        if (dirs_first && x.dir != y.dir) return x.dir;
        switch (st.sort) {
            case FileSort::modified: if (x.mtime != y.mtime) return x.mtime > y.mtime; break;
            case FileSort::size: if (!x.dir && !y.dir && x.size != y.size) return x.size > y.size; break;
            case FileSort::type: {
                const auto ex = d::ext_of(x.name), ey = d::ext_of(y.name);
                if (ex != ey) return ex < ey;
                break;
            }
            case FileSort::name: break;
        }
        return d::ci_less(x.name, y.name);
    });
    return remember(std::move(v));
}

/// The keyboard, while the pane has it. nullopt: not the pane's key (while
/// filtering, letters arrive as text: file_pane_text).
[[nodiscard]] inline std::optional<FilePaneOp> file_pane_key(const FilePaneState& st, const FilePaneProps& p, Key k, Mods md) {
    namespace d = file_pane_detail;
    using A = FilePaneAction;
    const auto vis = file_pane_view(p.entries, st, p.dirs_first);
    const int n = static_cast<int>(vis.size());
    const int cur = std::clamp(st.sel, 0, std::max(n - 1, 0));
    const FileEntry* e = n > 0 ? &p.entries[vis[static_cast<std::size_t>(cur)].i] : nullptr;
    const std::string sel_path = e ? d::join(p.path, e->name, p.sep) : p.path;
    const auto emit = [](A::Kind kind, std::string path) {
        return FilePaneOp{[kind, path](FilePaneState& s) { s.out.push_back({kind, path}); }};
    };
    const auto step = [n, rh = d::row_h(d::name_px(p))](int by) {
        return FilePaneOp{[n, by, rh](FilePaneState& s) {
            s.sel = std::clamp(s.sel + by, 0, std::max(n - 1, 0));
            const float y = rh * static_cast<float>(s.sel);
            if (y < s.scroll.offset.y) s.scroll.offset.y = y;
            else if (y + rh > s.scroll.offset.y + s.scroll.viewport.y) s.scroll.offset.y = y + rh - s.scroll.viewport.y;
        }};
    };
    const auto open_sel = [&]() -> std::optional<FilePaneOp> {
        if (!e) return std::nullopt;
        const auto kind = e->dir || e->link ? A::Kind::list : A::Kind::open;
        return FilePaneOp{[kind, sel_path](FilePaneState& s) {
            s.out.push_back({kind, sel_path});
            if (kind == A::Kind::list) { s.filter.clear(); s.filtering = false; }
        }};
    };
    if (st.filtering) {
        switch (k) {
            case Key::escape: return FilePaneOp{[](FilePaneState& s) { s.filter.clear(); s.filtering = false; s.sel = 0; }};
            case Key::backspace:
                return FilePaneOp{[](FilePaneState& s) {
                    if (s.filter.empty()) { s.filtering = false; return; }
                    while (!s.filter.empty() && (static_cast<unsigned char>(s.filter.back()) & 0xC0) == 0x80) s.filter.pop_back();
                    if (!s.filter.empty()) s.filter.pop_back();
                    s.sel = 0;
                }};
            case Key::enter:
                if (md.alt && e) return emit(A::Kind::paste, sel_path);
                return open_sel();
            case Key::tab: return emit(A::Kind::cd, e && e->dir ? sel_path : p.path);
            case Key::down: return step(1);
            case Key::up: return step(-1);
            case Key::page_down: return step(10);
            case Key::page_up: return step(-10);
            default: return std::nullopt;
        }
    }
    switch (k) {
        case Key::escape:
            if (!st.filter.empty()) return FilePaneOp{[](FilePaneState& s) { s.filter.clear(); s.sel = 0; }};
            return FilePaneOp{[](FilePaneState& s) { s.focused = false; }};
        case Key::down: case Key::j: return step(1);
        case Key::up: case Key::k: return step(-1);
        case Key::page_down: return step(10);
        case Key::page_up: return step(-10);
        case Key::home: return step(-1 << 20);
        case Key::end: return step(1 << 20);
        case Key::enter:
            if (md.alt && e) return emit(A::Kind::paste, sel_path);
            return open_sel();
        case Key::right: case Key::l:
            if (e && (e->dir || e->link)) return emit(A::Kind::list, sel_path);
            return std::nullopt;
        case Key::left: case Key::h: case Key::backspace:
            return emit(A::Kind::list, d::parent(p.path));
        case Key::tab: return emit(A::Kind::cd, e && e->dir ? sel_path : p.path);
        case Key::slash: return FilePaneOp{[](FilePaneState& s) { s.filtering = true; s.sel = 0; s.scroll.offset.y = 0.0f; }};
        case Key::s:
            return FilePaneOp{[](FilePaneState& s) { s.sort = static_cast<FileSort>((static_cast<int>(s.sort) + 1) % 4); s.sel = 0; }};
        case Key::period: return FilePaneOp{[](FilePaneState& s) { s.show_hidden = !s.show_hidden; s.sel = 0; }};
        case Key::r: case Key::f5: return emit(A::Kind::list, p.path);
        case Key::y: return emit(A::Kind::copy_path, sel_path);
        case Key::f: return emit(A::Kind::follow, {});
        case Key::d: if (e && !p.where.empty()) return emit(A::Kind::download, sel_path); return std::nullopt;
        case Key::u: if (!p.where.empty()) return emit(A::Kind::upload, p.path); return std::nullopt;
        case Key::o: return emit(A::Kind::reveal, sel_path);
        default: break;
    }
    return std::nullopt;
}

/// Typed text, while the pane has the keyboard: the filter's, when filtering.
[[nodiscard]] inline std::optional<FilePaneOp> file_pane_text(const FilePaneState& st, std::string_view text) {
    if (!st.filtering) return std::nullopt;
    std::string add;
    for (char ch : text)
        if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7f) add.push_back(ch);
    if (add.empty() || (st.filter.empty() && add == "/")) return FilePaneOp{[](FilePaneState&) {}};   // the / that began it
    return FilePaneOp{[add](FilePaneState& s) { s.filter += add; s.sel = 0; s.scroll.offset.y = 0.0f; }};
}

/// Resizing by the grip. `begin`: the grip was pressed (remember the width
/// now); then `grow` is how many pixels wider than THAT width the pane should
/// be -- the pointer's travel since the press, so the edge stays under the
/// pointer (no summed deltas to drift, nothing lost to clamping).
struct FilePaneResize { bool begin = false; float grow = 0.0f; bool end = false; };

/// `on_resize(FilePaneResize)` (optional): the inner edge is a grip.
template <ControlContext C, class On, class OnResize = std::nullptr_t>
    requires std::invocable<const On&, FilePaneOp>
[[nodiscard]] inline Node file_pane(const C& c, std::string_view id, const FilePaneState& st, const FilePaneProps& p, On on,
                                    OnResize on_resize = nullptr) {
    namespace d = file_pane_detail;
    using A = FilePaneAction;
    const Theme& t = c.theme;
    // Every size is a tier of the theme's type scale (style/type.hpp):
    //
    //   interface face   ui    body    the title, the filter field
    //                    ui2   small   crumbs, chips, counts, notes
    //                    ui3   fine    the key hints: wanted small, and the
    //                                  scale's floor keeps them legible
    //   content face     nf    body    names, the preview's file name
    //   (p.text_size)    nf2   small   ages, sizes, git marks, the preview
    //
    // A size that is none of these is a bug: it reads as a size nobody chose.
    const float ui = t.type(Tier::body), ui2 = t.type(Tier::small), ui3 = t.type(Tier::fine);
    const float nf = t.type_at(d::name_px(p), Tier::body), nf2 = t.type_at(d::name_px(p), Tier::small);
    const float rh = d::row_h(nf), ri = std::round(nf * 1.38f);
    const std::string base{id};
    const auto paper = t.background, ink = t.text_primary;
    const auto muted = mix(ink, paper, 0.45f), faint = mix(ink, paper, 0.62f), line = mix(paper, ink, 0.09f);
    const bool remote = !p.where.empty();
    const auto vis = file_pane_view(p.entries, st, p.dirs_first);
    const auto op = [on](auto f) { return on(FilePaneOp{std::move(f)}); };
    const auto emit = [](A::Kind kind, std::string path) { return [kind, path](FilePaneState& s) { s.out.push_back({kind, path}); }; };
    const auto mono = [&](std::string s, float size, Color<Srgb> col) { return text_owned(std::move(s)) | font(size) | monospace | fg(col); };
    const auto cell = [&](std::string s, float size, Color<Srgb> col) { return text_owned(std::move(s)) | font(size) | family(p.family) | fg(col); };
    const auto icon_btn = [&](std::string_view bid, Icon ic, std::string tip, auto click, bool lit = false) {
        const auto nid = node_id(base + "." + std::string{bid});
        const bool hov = c.hovered(nid);
        return (h(node(icon_node(ic, 13.0f, lit ? t.accent : hov ? ink : muted)))
                | size(24.0f, 24.0f) | center | radius(t.rounded(5.0f)) | bg(hov ? mix(paper, ink, 0.09f) : paper.fade(0.0f))
                | pointer_cursor | tooltip(std::move(tip)) | id_of_raw(nid)
                | on_click([op, click] { return op(click); })).build();
    };
    const auto git_col = [&](char g) -> Color<Srgb> {
        switch (g) {
            case 'M': return t.warning;
            case 'A': return t.success;
            case 'D': case 'U': return t.danger;
            case 'R': return t.info;
            case '?': return mix(t.success, muted, 0.3f);
            default: return muted;
        }
    };
    const auto kind_col = [&](d::Kind k) -> Color<Srgb> {
        if (k == d::Kind::dir) return t.accent;
        if (!p.type_colors) return muted;
        switch (k) {
            case d::Kind::code: return mix(t.info, muted, 0.2f);
            case d::Kind::config: return mix(t.warning, muted, 0.35f);
            case d::Kind::doc: return mix(t.success, muted, 0.35f);
            case d::Kind::image: return mix(mix(t.danger, t.info, 0.5f), muted, 0.2f);
            case d::Kind::archive: return mix(t.danger, muted, 0.35f);
            case d::Kind::exec: return mix(t.success, muted, 0.1f);
            default: return muted;
        }
    };

    // ── head: the folder, and the path to it as crumbs ────────────────────
    const std::string leaf = [&] {
        std::string q = p.path;
        while (q.size() > 1 && (q.back() == '/' || q.back() == '\\')) q.pop_back();
        const auto s = q.find_last_of("/\\");
        return s == std::string::npos || s + 1 >= q.size() ? q : q.substr(s + 1);
    }();
    std::vector<Node> crumb_nodes;
    {
        const auto cr = d::crumbs(p.path, remote ? std::string_view{} : std::string_view{p.home}, p.sep);
        std::vector<std::size_t> keep;   // long paths: the root, an ellipsis, the last three
        constexpr std::size_t gap_mark = static_cast<std::size_t>(-1);
        if (cr.size() > 5) {
            keep = {0, gap_mark};
            for (std::size_t i = cr.size() - 3; i < cr.size(); ++i) keep.push_back(i);
        } else {
            for (std::size_t i = 0; i < cr.size(); ++i) keep.push_back(i);
        }
        bool after_root = false;
        for (std::size_t n = 0; n < keep.size(); ++n) {
            if (n > 0 && !after_root) crumb_nodes.push_back(mono("/", ui2, faint.fade(0.7f)).build());
            const std::size_t k = keep[n];
            if (k == gap_mark) { crumb_nodes.push_back(mono("\u2026", ui2, faint).build()); after_root = false; continue; }
            const bool last = k + 1 == cr.size();
            const auto cid = part_id(base + ".crumb", k);
            const bool hov = c.hovered(cid);
            crumb_nodes.push_back((h(mono(cr[k].first, ui2, last ? t.text_secondary : hov ? t.accent : muted))
                                   | pad(1.0f, 2.0f) | radius(t.rounded(3.0f)) | bg(hov && !last ? t.accent.fade(0.1f) : paper.fade(0.0f))
                                   | when(!last, pointer_cursor) | tooltip(cr[k].second) | id_of_raw(cid)
                                   | on_click([op, f = emit(A::Kind::list, cr[k].second)] { return op(f); })).build());
            after_root = cr[k].first == "/";
        }
    }
    std::vector<Node> title_bits;
    title_bits.push_back((text_owned(leaf.empty() ? std::string{"files"} : leaf) | font(ui) | semibold | fg(ink) | ellipsis | min_size(0, 0)).build());
    // the machine, in the colour its tab wears: red for production
    const auto host_tone = p.where_danger ? t.readable(t.danger) : t.info;
    if (remote) title_bits.push_back((h(node(icon_node(Icon::server, 10.0f, host_tone)), mono(p.where, ui2, host_tone) | ellipsis | min_size(0, 0))
                                      | gap(4.0f) | align(Align::center) | pad(1.0f, 5.0f) | radius(t.rounded(4.0f)) | bg(host_tone.fade(p.where_danger ? 0.16f : 0.1f))
                                      | min_size(0, 0)).build());
    auto head = h(node(icon_node(remote ? Icon::server : Icon::folder_open, 14.0f, remote ? host_tone : t.accent)),
                  v(h_list(std::move(title_bits), 6.0f) | align(Align::center) | min_size(0, 0),
                    h_list(std::move(crumb_nodes), 1.0f) | align(Align::center) | min_size(0, 0) | clip)
                      | gap(2.0f) | grow() | min_size(0, 0),
                  icon_btn("refresh", Icon::refresh, "Refresh (r)", emit(A::Kind::list, p.path)),
                  icon_btn("close", Icon::close, "Close (ctrl+shift+e)", emit(A::Kind::close, {})))
              | gap(8.0f) | align(Align::center) | width(pct(100)) | pad(10.0f, 8.0f, 6.0f, 12.0f);

    // ── toolbar: follow, sort, filter, up, cd, dotfiles, transfer ─────────
    Node toolbar;
    if (st.filtering || !st.filter.empty()) {
        const std::string count = std::to_string(vis.size()) + " of " + std::to_string(p.entries.size());
        toolbar = (h((h(node(icon_node(Icon::search, 12.0f, t.accent)),
                        mono(st.filter.empty() ? std::string{"type to filter"} : st.filter, ui, st.filter.empty() ? faint : ink)
                            | ellipsis | min_size(0, 0),
                        mono(st.filtering ? "\u258F" : "", ui, t.accent),
                        spacer(),
                        mono(count, ui2, muted),
                        mono("esc", ui2, faint))
                      | gap(6.0f) | align(Align::center) | height(26.0f) | width(pct(100)) | pad(0, 8.0f) | radius(t.rounded(6.0f))
                      | bg(t.accent.fade(0.07f)) | border(1.0f, t.accent.fade(st.filtering ? 0.45f : 0.2f))))
                   | width(pct(100)) | pad(0, 8.0f, 6.0f, 8.0f) | inner_shadow(0.0f, line, {0.0f, -1.0f})).build();
    } else {
        std::vector<Node> tools;
        // What fits decides what shows: the five actions are icons (26 px
        // each), and the follow and sort chips drop their words, follow's
        // first, when the pane is too narrow for them -- measured, so nothing
        // ever spills out over the terminal. Their tooltips still say it all.
        const std::string follow_word = p.following ? "follows the shell" : "follow the shell";
        const std::string sort_word = file_sort_names[static_cast<int>(st.sort) & 3];
        const float chip_pad = 7.0f * 2.0f + 11.0f + 5.0f;   // padding, icon, gap
        const float follow_full = chip_pad + detail::text_advance(c, follow_word, ui2);
        const float sort_full = 6.0f * 2.0f + 11.0f + 4.0f + detail::text_advance(c, sort_word, ui2);
        const float icons = 5.0f * 26.0f, room = p.width - 14.0f - 8.0f;   // pads, the spacer's gap
        const bool sort_words = icons + 26.0f + sort_full + 26.0f <= room;
        const bool follow_words = sort_words && icons + follow_full + sort_full + 4.0f <= room;
        {
            const auto fid = node_id(base + ".follow");
            const bool hov = c.hovered(fid);
            std::vector<Node> bits;
            bits.push_back(node(icon_node(Icon::link, follow_words ? 11.0f : 13.0f, p.following ? t.accent : hov ? ink : muted)).build());
            if (follow_words)
                bits.push_back((text_owned(follow_word) | font(ui2) | fg(p.following ? t.accent : hov ? ink : t.text_secondary)).build());
            tools.push_back((h_list(std::move(bits), 5.0f) | align(Align::center) | height(follow_words ? 22.0f : 24.0f)
                             | pad(0, follow_words ? 7.0f : 5.5f)
                             | radius(t.rounded(5.0f))
                             | bg(p.following ? t.accent.fade(0.1f) : hov ? mix(paper, ink, 0.07f) : paper.fade(0.0f))
                             | pointer_cursor | tooltip(p.following ? "Follows the shell: the list moves with its cd" : "Follow the shell's directory again (f)")
                             | id_of_raw(fid) | on_click([op, f = emit(A::Kind::follow, {})] { return op(f); })).build());
        }
        tools.push_back(spacer().build());
        {
            const auto sid = node_id(base + ".sort");
            const bool hov = c.hovered(sid);
            std::vector<Node> bits;
            bits.push_back(node(icon_node(Icon::list_tree, sort_words ? 11.0f : 13.0f, hov ? ink : muted)).build());
            if (sort_words) bits.push_back((text_owned(sort_word) | font(ui2) | fg(hov ? ink : muted)).build());
            tools.push_back((h_list(std::move(bits), 4.0f) | align(Align::center) | height(24.0f)
                             | pad(0, sort_words ? 6.0f : 5.5f)
                             | radius(t.rounded(5.0f))
                             | bg(hov ? mix(paper, ink, 0.09f) : paper.fade(0.0f)) | pointer_cursor
                             | tooltip("Sorted by " + sort_word + " \u2014 click or s for the next") | id_of_raw(sid)
                             | on_click([op] {
                                   return op([](FilePaneState& s) { s.sort = static_cast<FileSort>((static_cast<int>(s.sort) + 1) % 4); s.sel = 0; });
                               })).build());
        }
        tools.push_back(icon_btn("find", Icon::search, "Filter as you type (/)",
                                 [](FilePaneState& s) { s.filtering = true; s.focused = true; s.sel = 0; }));
        tools.push_back(icon_btn("up", Icon::arrow_up, "Up a folder (\u232B)", emit(A::Kind::list, d::parent(p.path))));
        tools.push_back(icon_btn("cd", Icon::terminal, "cd here in the shell (tab)", emit(A::Kind::cd, p.path)));
        tools.push_back(icon_btn("hidden", Icon::eye, st.show_hidden ? "Hide dotfiles (.)" : "Show dotfiles (.)",
                                 [](FilePaneState& s) { s.show_hidden = !s.show_hidden; s.sel = 0; }, st.show_hidden));
        if (remote) tools.push_back(icon_btn("upload", Icon::git_push, "Upload here (u)", emit(A::Kind::upload, p.path)));
        else tools.push_back(icon_btn("reveal", Icon::external, "Show in the file manager (o)", emit(A::Kind::reveal, p.path)));
        toolbar = (h_list(std::move(tools), 2.0f) | align(Align::center) | width(pct(100)) | pad(0, 6.0f, 6.0f, 8.0f) | clip
                   | inner_shadow(0.0f, line, {0.0f, -1.0f})).build();
    }

    // ── the list ───────────────────────────────────────────────────────────
    std::vector<Node> rows;
    const auto note_row = [&](Icon ic, Color<Srgb> col, std::string title, std::string body) {
        return (v(h(node(icon_node(ic, 14.0f, col)), text_owned(std::move(title)) | font(ui) | semibold | fg(col) | wrap_text | grow())
                      | gap(7.0f) | align(Align::center),
                  text_owned(std::move(body)) | font(ui2) | fg(t.text_secondary) | wrap_text | width(pct(100)))
                | gap(5.0f) | pad(12.0f, 12.0f) | width(pct(100))).build();
    };
    if (!p.error.empty()) rows.push_back(note_row(Icon::circle_alert, t.danger, "Could not list this folder", p.error));
    else if (p.loading && p.entries.empty()) rows.push_back(note_row(Icon::refresh, muted, remote ? "Listing over SFTP\u2026" : "Listing\u2026", ""));
    else if (vis.empty() && !st.filter.empty())
        rows.push_back(note_row(Icon::search, muted, "No match", "Nothing here matches \u201C" + st.filter + "\u201D."));
    else if (vis.empty()) rows.push_back(note_row(Icon::folder, muted, "Empty", p.entries.empty() ? "" : "Only dotfiles here: . shows them."));
    std::uint64_t biggest = 0;
    for (const auto& s : vis)
        if (!p.entries[s.i].dir) biggest = std::max(biggest, p.entries[s.i].size);
    // Only the rows on screen are built (virtual_list.hpp: visible_rows, the
    // one virtualisation rule in mayag; lint one_list). Every row is exactly
    // `rh`, so the rest are two spacers: same scroll extent, same positions,
    // same part ids. Building every row made a frame O(folder): 21 ms a
    // scroll step in a 3000-file folder. A screen of overscan each side, so a
    // fling never shows an unbuilt row; before the first layout the viewport
    // is unknown, so assume a tall one rather than build three rows.
    ScrollState window = st.scroll;
    if (window.viewport.y <= 0.0f) window.viewport.y = 2000.0f;
    const VisibleRange vr = visible_rows(window, static_cast<int>(vis.size()), rh, 0.0f,
                                         static_cast<int>(window.viewport.y / rh) + 1);
    const auto first_row = static_cast<std::size_t>(vr.first);
    const auto last_row = static_cast<std::size_t>(vr.last);
    const auto skipped_rows = [](float hpx) { return (box() | height(hpx) | width(pct(100))).build(); };
    if (vr.pad_before > 0.0f) rows.push_back(skipped_rows(vr.pad_before));
    for (std::size_t q = first_row; q < last_row; ++q) {
        const auto& e = p.entries[vis[q].i];
        const auto kind = d::kind_of(e);
        const auto rid = part_id(base + ".row", q);
        const bool sel = static_cast<int>(q) == st.sel, hov = c.hovered(rid);
        const std::string full = d::join(p.path, e.name, p.sep);
        const auto lit = sel && st.focused;
        Color<Srgb> name_col = e.dir ? ink : t.text_secondary;
        if (p.git && e.git) name_col = mix(name_col, git_col(e.git), e.git == '?' ? 0.4f : 0.55f);
        const Icon ic = e.dir ? Icon::folder : e.link ? Icon::link : kind == d::Kind::code ? Icon::file_code : Icon::file;

        // the cells right of the name: measured, so the name gets exactly the rest
        const std::string age_full = p.age && e.mtime > 0 ? d::age(p.now, e.mtime) : std::string{};
        const std::string size_full = e.dir ? std::string{} : d::human(e.size);
        const std::string git_s = p.git && e.git && !e.dir ? std::string(1, e.git == '?' ? 'U' : e.git == 'U' ? '!' : e.git) : std::string{};
        const float git_w = p.git && e.git ? (e.dir ? 5.0f : detail::text_advance(c, git_s, nf2, p.family)) + 7.0f : 0.0f;
        const float age_w = age_full.empty() ? 0.0f : detail::text_advance(c, age_full, nf2, p.family) + 7.0f;
        const float size_w = size_full.empty() ? 0.0f : detail::text_advance(c, size_full, nf2, p.family) + 7.0f;
        // the row's inner width: the pane, less the list's and the row's padding and the scrollbar's lane
        const float inner = p.width - 12.0f - 16.0f - 10.0f - ri - 7.0f;
        const float name_w = detail::text_advance(c, e.name, nf, p.family);
        // The name is what the row is for: when it doesn't fit, the age goes
        // first, then the size (the hover peek still shows both), and only
        // then is the name shortened.
        const bool keep_age = name_w <= inner - git_w - age_w - size_w;
        const bool keep_size = keep_age || name_w <= inner - git_w - size_w;
        const std::string age_s = keep_age ? age_full : std::string{};
        const std::string size_s = keep_size ? size_full : std::string{};
        const float meta = git_w + (keep_age ? age_w : 0.0f) + (keep_size ? size_w : 0.0f);
        const float meta_full = git_w + age_w + size_w;
        const float name_room = std::max(inner - meta, 24.0f);
        const bool cut = name_w > name_room + 0.5f;
        const bool hidden_meta = (!keep_age && age_w > 0.0f) || (!keep_size && size_w > 0.0f);
        const auto row_cells = [&](std::string shown, bool fuzzy, bool whole = false) {
            const std::string& ag = whole ? age_full : age_s;
            const std::string& sz = whole ? size_full : size_s;
            std::vector<Node> cells;
            cells.push_back(node(icon_node(ic, ri, e.link && !e.dir ? t.info : kind_col(kind))).build());
            cells.push_back((h(fuzzy ? (fuzzy_text(e.name, vis[q].hits, nf, name_col, t.accent) | family(p.family) | min_size(0, 0)).build()
                                     : (cell(std::move(shown), nf, name_col) | ellipsis | min_size(0, 0)).build())
                             | grow() | min_size(0, 0) | clip).build());
            if (p.git && e.git) {
                const auto gc = git_col(e.git);
                if (e.dir) cells.push_back((box() | size(5.0f, 5.0f) | radius(2.5f) | bg(gc)).build());
                else cells.push_back(cell(git_s, nf2, gc).build());
            }
            if (!ag.empty()) {
                const bool recent = p.now - e.mtime < 3600;
                cells.push_back(cell(ag, nf2, recent ? mix(t.accent, faint, 0.25f) : faint).build());
            }
            if (!sz.empty()) cells.push_back(cell(sz, nf2, faint).build());
            return cells;
        };
        // a name that doesn't fit is shortened in the middle -- "compile_comm…ands.json":
        // what it is and its extension both stay readable (a filter's highlighted
        // letters keep the plain end-ellipsis, so they stay where they match)
        std::vector<Node> cells = row_cells(cut && vis[q].hits.empty() ? detail::fit_middle(c, e.name, name_room, nf, p.family) : e.name,
                                            !vis[q].hits.empty());
        const bool bar = p.size_bars && !e.dir && biggest > 0 && e.size > 0;
        auto row_line = h_list(std::move(cells), 7.0f) | align(Align::center) | width(pct(100)) | height(bar ? rh - 3.0f : rh);
        Node content;
        if (bar) {
            const float f = std::sqrt(static_cast<float>(static_cast<double>(e.size) / static_cast<double>(biggest)));
            content = (v(std::move(row_line),
                         h(box() | width(ri + 7.0f) | height(2.0f),
                           box() | width(pct(std::max(f, 0.02f) * 80.0f)) | height(2.0f) | radius(1.0f) | bg(kind_col(kind).fade(sel || hov ? 0.45f : 0.24f)))
                             | height(3.0f) | width(pct(100)))
                       | width(pct(100))).build();
        } else {
            content = std::move(row_line).build();
        }
        const auto row_bg = lit ? mix(paper, ink, 0.1f) : sel ? mix(paper, ink, 0.06f) : hov ? mix(paper, ink, 0.04f)
                          : e.fresh > 0.0f ? t.accent.fade(0.22f * e.fresh) : paper.fade(0.0f);
        // Hovering a shortened row lifts it out whole, in place: the same row,
        // its full name, grown past the pane's edge over the terminal (VS Code's
        // tree does this). It never takes the pointer -- clicks, the drag, the
        // wheel all still land on the row under it.
        if (hov && (cut || hidden_meta) && vis[q].hits.empty()) {
            const float need = 16.0f + ri + 7.0f + name_w + 7.0f + meta_full + 2.0f;
            c.overlay(Overlay{
                .content = (h_list(row_cells(e.name, false, true), 7.0f) | align(Align::center) | height(rh) | width(std::max(need, p.width - 22.0f))
                            | pad(0, 8.0f) | radius(t.rounded(5.0f))
                            | bg(mix(paper, ink, lit ? 0.1f : sel ? 0.07f : 0.05f))
                            | border(t.stroke(1), mix(paper, ink, 0.14f)) | elevation(t.lift(8))
                            | when(lit, inner_shadow(0.0f, t.accent, {2.0f, 0.0f}))).build(),
                .id = part_id(base + ".peek", q),
                .anchor_id = rid,
                .placement = Placement::over_start,
                .dismiss = Dismiss::never,
                .offset = 0.0f,
                .layer = 30,
                .passthrough = true});
        }
        rows.push_back((v(std::move(content)) | height(rh) | width(pct(100)) | pad(0, 8.0f) | radius(t.rounded(5.0f))
                        | bg(row_bg)
                        | when(lit, inner_shadow(0.0f, t.accent, {2.0f, 0.0f}))
                        | when(!lit && e.fresh > 0.0f, inner_shadow(0.0f, t.accent.fade(e.fresh), {2.0f, 0.0f}))
                        | pointer_cursor | tooltip(e.link && !e.target.empty() ? e.name + " \u2192 " + e.target : e.when) | id_of_raw(rid)
                        | on_click([op, q, full, dir = e.dir || e.link](const Clicked& ck) {
                              // click selects and takes the keyboard; double opens;
                              // alt-click hands the path to the shell
                              return op([q, full, dir, twice = ck.count >= 2, alt = ck.mods.alt, mid = ck.button == MouseButton::middle](FilePaneState& s) {
                                  s.sel = static_cast<int>(q);
                                  s.focused = true;
                                  if (alt || mid) s.out.push_back({FilePaneAction::Kind::paste, full});
                                  else if (twice) {
                                      s.out.push_back({dir ? FilePaneAction::Kind::list : FilePaneAction::Kind::open, full});
                                      if (dir) { s.filter.clear(); s.filtering = false; }
                                  }
                              });
                          })).build());
    }
    if (vr.pad_after > 0.0f) rows.push_back(skipped_rows(vr.pad_after));
    Node list = scroll_view(c, base + ".list", st.scroll, v(std::move(rows)) | width(pct(100)) | pad(4.0f, 6.0f),
                            [op](ScrollOp so) { return op([so](FilePaneState& s) { so(s.scroll); }); });

    // ── preview: the selected file's first lines ──────────────────────────
    Node preview;
    if (p.preview) {
        const auto& pv = *p.preview;
        // the body is file content one step down; its line numbers the same
        // size, just fainter (a size of their own was a fourth size)
        const float lh = std::round(nf2 * 1.55f);
        const int n = std::min(static_cast<int>(pv.lines.size()), std::max(p.preview_lines, 0));
        std::vector<Node> ls;
        for (int i = 0; i < n; ++i) {
            const std::string& ln = pv.lines[static_cast<std::size_t>(i)];
            // a folder's entries: no line numbers (they are not lines), folders in the folder colour
            const bool sub = pv.listing && !ln.empty() && ln.back() == '/';
            Node lead = pv.listing ? node(icon_node(sub ? Icon::folder : Icon::file, std::round(nf2 * 0.95f), sub ? t.accent.fade(0.7f) : faint.fade(0.7f)))
                                   : (cell(std::to_string(i + 1), nf2, faint.fade(0.55f)) | width(std::round(nf2 * 2.4f))).build();
            ls.push_back((h(std::move(lead),
                            cell(ln.empty() ? std::string{" "} : sub ? ln.substr(0, ln.size() - 1) : ln, nf2, sub ? t.text_secondary : muted)
                                | ellipsis | grow() | min_size(0, 0))
                          | gap(pv.listing ? 7.0f : 4.0f) | align(Align::center) | height(lh) | width(pct(100)) | clip).build());
        }
        const float body_h = pv.lines.empty() ? 0.0f : lh * static_cast<float>(std::max(p.preview_lines, 1));
        preview = (v(h(node(icon_node(Icon::eye, 11.0f, t.accent.fade(0.8f))),
                       cell(pv.name, nf, t.text_secondary) | ellipsis | grow() | min_size(0, 0),
                       cell(pv.note, nf2, faint))
                         | gap(6.0f) | align(Align::center) | width(pct(100)) | height(22.0f),
                     v(std::move(ls)) | width(pct(100)) | height(body_h) | clip)
                   | width(pct(100)) | pad(4.0f, 10.0f, 6.0f, 10.0f) | bg(mix(paper, ink, 0.045f))
                   | inner_shadow(0.0f, line, {0.0f, 1.0f}) | clip).build();
    }

    // ── foot: busy / note, then the keys ──────────────────────────────────
    std::vector<Node> foot;
    if (!p.busy.empty())
        foot.push_back((h(node(icon_node(Icon::refresh, 12.0f, t.accent)), text_owned(p.busy) | font(ui2) | fg(t.accent) | ellipsis | grow() | min_size(0, 0))
                        | gap(6.0f) | align(Align::center) | width(pct(100)) | pad(6.0f, 10.0f) | bg(t.accent.fade(0.08f))).build());
    if (!p.note.empty())
        foot.push_back((text_owned(p.note) | font(ui2) | fg(muted) | wrap_text | width(pct(100)) | pad(6.0f, 10.0f)).build());
    const auto key = [&](std::string k, std::string what) {
        // one size for key and label (the fine tier); the key reads by colour
        return (h(mono(std::move(k), ui3, t.text_secondary), text_owned(std::move(what)) | font(ui3) | fg(faint))
                | gap(4.0f) | align(Align::center)).build();
    };
    std::vector<Node> keys;
    if (st.filtering) {
        keys.push_back(key("\u23CE", "open"));
        keys.push_back(key("\u2191\u2193", "move"));
        keys.push_back(key("esc", "clear"));
    } else if (st.focused) {
        keys.push_back(key("\u23CE", remote ? "open / get" : "open"));
        keys.push_back(key("/", "filter"));
        keys.push_back(key("s", "sort"));
        // a word, not U+21E5: the Mac's monospace faces have no ⇥, so it drew as nothing
        keys.push_back(key("tab", "cd"));
    } else {
        keys.push_back(key("click", "select"));
        keys.push_back(key("\u2325click", "paste path"));
    }
    foot.push_back((h_list(std::move(keys), 12.0f) | align(Align::center) | width(pct(100)) | pad(7.0f, 10.0f)
                    | inner_shadow(0.0f, line, {0.0f, 1.0f})).build());

    std::vector<Node> parts;
    parts.push_back(std::move(head).build());
    parts.push_back(std::move(toolbar));
    parts.push_back((node(std::move(list)) | grow() | min_size(0, 0) | width(pct(100))).build());
    if (p.preview) parts.push_back(std::move(preview));
    parts.push_back((v(std::move(foot)) | width(pct(100))).build());
    Node body = (v(std::move(parts))
                 | width(p.width) | height(pct(100)) | bg(mix(paper, ink, 0.025f))
                 | inner_shadow(0.0f, st.focused ? t.accent.fade(0.5f) : line, {p.left ? -1.0f : 1.0f, 0.0f})
                 | role(a11y::Role::group) | label("Files")).build();
    if constexpr (std::is_same_v<OnResize, std::nullptr_t>) {
        return body;
    } else {   // a 5 px grip over the inner edge: a hairline that lights while held
        const auto gid = node_id(base + ".grip");
        const bool hot = c.hovered(gid) || c.pressed(gid);
        Node hair = (box() | width(hot ? 2.0f : 0.0f) | height(pct(100)) | bg(t.accent)).build();
        Node inner = p.left ? h(spacer(), node(std::move(hair))).build() : h(node(std::move(hair))).build();
        Node grip = (node(std::move(inner)) | width(5.0f) | height(pct(100)) | cursor(CursorShape::resize_h) | role(a11y::Role::separator)
                     | label("resize files") | id_of_raw(gid)
                     | on_press([on_resize](const Pressed&) { return on_resize(FilePaneResize{.begin = true}); })
                     | on_release([on_resize](const Released&) { return on_resize(FilePaneResize{.end = true}); })
                     | on_drag([on_resize, left = p.left](const Dragged& dr) {
                           return on_resize(FilePaneResize{.grow = left ? dr.travel.x() : -dr.travel.x()});
                       })).build();
        return (z(std::move(body), node(std::move(grip)) | absolute(p.left ? p.width - 5.0f : 0.0f, 0.0f) | height(pct(100)))
                | width(p.width) | height(pct(100))).build();
    }
}

}  // namespace mayag::dsl
