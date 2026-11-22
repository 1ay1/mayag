#pragma once
// mayag::widget ssh_manager — every SSH host you have, organised and editable
//
//   ┌─────────────────────────┬───────────────────────────────────────────────┐
//   │ hosts      [+ New host] │  ⇄  prod-db                        PROD  ★   │
//   │ ⌕ filter…               │     deploy@10.0.3.4 · via bastion             │
//   │ PINNED                  │  [ Connect  ⏎ ]  [ Here ]  [ Copy ]  [ Edit ] │
//   │ ★ bastion        3h     │                                               │
//   │ ★ prod-db  PROD  3h  ●  │  CONNECTION                                   │
//   │ GROUPS · #db            │   Host      10.0.3.4                          │
//   │   api.staging    5d     │   User      deploy           Port   22        │
//   │ ALL HOSTS               │   Jump      bastion          Key    ~/.ssh/.. │
//   │   build-01              │  TUNNELS                                      │
//   │   ...                   │   L  localhost:5432 → db:5432                 │
//   │                         │  ON CONNECT   tmux new -A -s main             │
//   │ 9 hosts · ~/.ssh/config │  ssh -L 5432:db:5432 deploy@10.0.3.4      ⧉   │
//   └─────────────────────────┴───────────────────────────────────────────────┘
//
// MobaXterm's session manager, kept a terminal's: a searchable sidebar of
// every host (~/.ssh/config, the ones made here, known_hosts), grouped by
// pin, tag and environment; a detail pane that shows exactly what connecting
// will run; and an editor for hosts the app owns (and for the notes it keeps
// on ~/.ssh/config hosts: environment, tags, tunnels, a command on connect)
// that validates as you type.
//
// Data in, ops out, like settings_panel: the app owns one SshManagerState
// and runs every SshManagerOp on it, then drains `state.out` (connect, save,
// delete, pin, copy) and applies them to its own store.

#include "control.hpp"
#include "icons.hpp"
#include "scroll_view.hpp"
#include "ssh.hpp"
#include "ssh_forward.hpp"
#include "text_field.hpp"
#include "../app/handlers.hpp"
#include "../core/text_edit.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

/// What the app is asked to do. Drained from SshManagerState::out.
struct SshManagerAction {
    enum class Kind : std::uint8_t {
        connect, connect_here, save, remove, pin, copy_command,
        save_folder, remove_folder,       ///< folder: draft_folder / old_alias
        import_source,                    ///< host = index into props.imports
        files_open,                       ///< list `path` ("" = home) on `host`
        files_get, files_put, files_mkdir, files_terminal, files_copy_path,
        open_url,                         ///< a tunnel's local port in the browser (path = url)
    };
    Kind        kind = Kind::connect;
    int         host = -1;        ///< index into the hosts given to the view (-1: a new host)
    SshHost     draft;            ///< save: the host as edited
    std::string old_alias;        ///< save: what it was called ("" = new); folders: the old name
    std::string path;             ///< files_*: the remote path; open_url: the url
    struct Folder { std::string name, user, jump, identity; int port = 0; } folder;   ///< save_folder
};

/// The editor's fields, in tab order.
enum class SshField : std::uint8_t { alias, hostname, user, port, jump, identity, folder, env, tags, forwards, command, count_ };

/// A folder as the sidebar shows it, with what its hosts inherit.
struct SshFolder {
    std::string name, user, jump, identity;
    int         port = 0;
};
/// Another client's saved sessions, offered for import.
struct SshImportSource {
    std::string name;     ///< "PuTTY"
    std::string where;    ///< the registry key / file
    int         total = 0, fresh = 0;   ///< sessions, and those not here yet
};
/// The remote file browser's contents, as the app last listed them.
struct SshRemoteEntry {
    std::string   name, when, target;
    bool          dir = false, link = false;
    std::uint64_t size = 0;
};
struct SshFilesView {
    std::string alias;               ///< whose files these are ("" = nothing listed yet)
    std::string path;
    std::vector<SshRemoteEntry> entries;
    bool        loading = false;
    std::string error;
    std::string busy;                ///< "Downloading report.pdf…"
};

struct SshManagerState {
    TextEditState filter;
    int           sel = 0;               ///< index into the hosts (the app's order)
    bool          filtering = false;     ///< the filter has the keyboard
    ScrollState   list_scroll, detail_scroll, files_scroll;
    int           tab = 0;               ///< detail pane: 0 overview, 1 files
    int           files_sel = 0;
    bool          show_hidden = false;   ///< the file list shows dotfiles (`.` toggles)
    std::vector<std::string> collapsed;  ///< folders folded shut in the sidebar
    bool          import_open = false;
    // the editor: open when `editing`; `edit_host` -1 = a new host
    bool          editing = false;
    int           edit_host = -1;
    bool          editing_folder = false;   ///< editing a folder's defaults (fields: alias = name, user, port, jump, identity)
    std::string   edit_folder;              ///< its name before the edit ("" = new)
    std::array<TextEditState, static_cast<std::size_t>(SshField::count_)> fields;
    int           env = 0;               ///< 0 none, 1 dev, 2 staging, 3 prod
    SshField      focus = SshField::alias;
    bool          confirm_delete = false;
    std::string   error;                 ///< why save was refused, shown at the field
    // outputs
    std::vector<SshManagerAction> out;
    bool          close = false;
};

using SshManagerOp = Edit<SshManagerState>;

struct SshManagerProps {
    std::int64_t now = 0;
    std::string  source = "~/.ssh/config";
    std::string  store = "hosts.vibe";
    float        width = 940.0f, height = 600.0f;
    std::vector<SshFolder>       folders;   ///< those with defaults (others are implied by their hosts)
    std::vector<SshImportSource> imports;
    SshFilesView                 files;
    bool                         tunnels_known = true;   ///< the OS lets the app see listening ports
};

// ── pure helpers ───────────────────────────────────────────────────────────

namespace ssh_manager_detail {
inline constexpr float kSideW = 300.0f, kRowH = 44.0f, kHeadH = 30.0f;

[[nodiscard]] inline std::string join(const std::vector<std::string>& v, std::string_view sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) o += (i ? std::string{sep} : "") + v[i];
    return o;
}
[[nodiscard]] inline std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',' || ch == ' ' || ch == '\t') { if (!cur.empty()) out.push_back(cur), cur.clear(); }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
/// Forwards are typed one per comma ("L5432:db:5432, D1080").
[[nodiscard]] inline std::vector<std::string> split_forwards(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',' || ch == ';' || ch == '\n') { if (!cur.empty()) out.push_back(cur), cur.clear(); }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    for (auto& f : out) {
        while (!f.empty() && f.front() == ' ') f.erase(f.begin());
        while (!f.empty() && f.back() == ' ') f.pop_back();
    }
    std::erase_if(out, [](const std::string& f) { return f.empty(); });
    return out;
}
[[nodiscard]] inline const char* field_label(SshField f) {
    switch (f) {
        case SshField::alias: return "Name";
        case SshField::hostname: return "Host";
        case SshField::user: return "User";
        case SshField::port: return "Port";
        case SshField::jump: return "Jump host";
        case SshField::identity: return "Key file";
        case SshField::folder: return "Folder";
        case SshField::env: return "Environment";
        case SshField::tags: return "Tags";
        case SshField::forwards: return "Tunnels";
        case SshField::command: return "On connect";
        default: return "";
    }
}
[[nodiscard]] inline const char* field_hint(SshField f) {
    switch (f) {
        case SshField::alias: return "what you type to reach it";
        case SshField::hostname: return "address or DNS name";
        case SshField::user: return "default: your login";
        case SshField::port: return "22";
        case SshField::jump: return "bastion, or user@host:port";
        case SshField::identity: return "~/.ssh/id_ed25519";
        case SshField::folder: return "Work/EU (shares its user, jump, key)";
        case SshField::tags: return "db, eu, gpu";
        case SshField::forwards: return "L5432:db:5432, R8080:localhost:80, D1080";
        case SshField::command: return "tmux new -A -s main";
        default: return "";
    }
}
/// Fields that come from ~/.ssh/config and are not hand's to change there.
[[nodiscard]] inline bool field_owned_by_config(SshField f) {
    return f == SshField::alias || f == SshField::hostname || f == SshField::user || f == SshField::port ||
           f == SshField::jump || f == SshField::identity;
}
[[nodiscard]] inline std::string& text_of(SshManagerState& st, SshField f) {
    return st.fields[static_cast<std::size_t>(f)].text;
}
[[nodiscard]] inline const std::string& text_of(const SshManagerState& st, SshField f) {
    return st.fields[static_cast<std::size_t>(f)].text;
}

/// The draft as the editor has it, and what is wrong with it ("" = fine).
[[nodiscard]] inline SshHost draft_of(const SshManagerState& st, const std::vector<SshHost>& hosts, std::string* err) {
    SshHost h;
    const bool is_new = st.edit_host < 0 || st.edit_host >= static_cast<int>(hosts.size());
    if (!is_new) h = hosts[static_cast<std::size_t>(st.edit_host)];
    else h.editable = true;
    const auto get = [&](SshField f) {
        std::string s = text_of(st, f);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    const auto bad = [&](std::string why) { if (err != nullptr && err->empty()) *err = std::move(why); };
    const auto shell_safe = [](std::string_view s) { return s.find_first_of(" \t'\"`$;|&<>\\") == std::string_view::npos; };
    if (h.editable) {
        h.alias = get(SshField::alias);
        h.hostname = get(SshField::hostname);
        h.user = get(SshField::user);
        h.jump = get(SshField::jump);
        h.identity = get(SshField::identity);
        if (h.hostname.empty()) h.hostname = h.alias;
        if (h.alias.empty()) h.alias = (h.user.empty() ? "" : h.user + "@") + h.hostname;
        const std::string p = get(SshField::port);
        h.port = 22;
        if (!p.empty()) {
            const bool digits = std::all_of(p.begin(), p.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
            const int n = digits && p.size() <= 5 ? std::atoi(p.c_str()) : 0;
            if (n <= 0 || n > 65535) bad("Port: a number 1-65535");
            else h.port = n;
        }
        if (h.hostname.empty()) bad("Host: where to connect");
        for (const auto* s : {&h.alias, &h.hostname, &h.user, &h.jump, &h.identity})
            if (!shell_safe(*s)) bad(std::string{"no spaces or shell characters in "} + (s == &h.alias ? "Name" : s == &h.hostname ? "Host" : s == &h.user ? "User" : s == &h.jump ? "Jump host" : "Key file"));
    }
    h.env = static_cast<SshHost::Env>(std::clamp(st.env, 0, 3));
    {
        std::string f = get(SshField::folder);
        for (char& ch : f) if (ch == '\\') ch = '/';
        while (!f.empty() && f.back() == '/') f.pop_back();
        while (!f.empty() && f.front() == '/') f.erase(f.begin());
        h.folder = f;
    }
    h.tags = split_list(get(SshField::tags));
    for (auto& t : h.tags) if (!t.empty() && t[0] == '#') t.erase(0, 1);
    h.forwards.clear();
    for (const auto& f : split_forwards(get(SshField::forwards))) {
        std::string why;
        if (auto fw = ssh_forward_parse(f, &why)) h.forwards.push_back(fw->spec());
        else bad("Tunnel \u201C" + f + "\u201D: " + why);
    }
    h.command = get(SshField::command);
    return h;
}

[[nodiscard]] inline std::string host_line(const SshHost& h) {
    std::string a = (h.user.empty() ? "" : h.user + "@") + h.hostname + (h.port != 22 ? ":" + std::to_string(h.port) : "");
    if (!h.jump.empty()) a += "  \u00B7  via " + h.jump;
    return a;
}

/// The command connecting will run, as a person would type it.
[[nodiscard]] inline std::string command_line(const SshHost& h) {
    std::string s = "ssh";
    for (const auto& f : h.forwards)
        if (auto fw = ssh_forward_parse(f)) s += std::string{" "} + fw->flag() + " " + fw->arg();
    if (!h.editable && h.source != "known") s += " " + h.alias;
    else {
        if (h.port != 22) s += " -p " + std::to_string(h.port);
        if (!h.jump.empty()) s += " -J " + h.jump;
        if (!h.identity.empty()) s += " -i " + h.identity;
        s += " " + (h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
    }
    if (!h.command.empty()) s += " -t '" + h.command + "'";
    return s;
}

/// Sidebar groups, in order: pinned, each folder (alphabetical; "Work/EU"
/// right after "Work"), then the unfiled by where they come from. A host sits
/// in its first group only. Filtering: one flat ranked list.
struct Group { std::string title; std::vector<int> hosts; std::string folder; bool collapsed = false; };
[[nodiscard]] inline std::vector<Group> groups_of(const std::vector<SshHost>& hosts, std::string_view q, std::int64_t now,
                                                  const std::vector<std::string>& collapsed = {}) {
    std::vector<Group> out;
    if (!q.empty()) {
        const auto r = rank_hosts(hosts, q, now);
        if (!r.empty()) out.push_back({std::to_string(r.size()) + (r.size() == 1 ? " match" : " matches"), r, {}, false});
        return out;
    }
    const auto order = rank_hosts(hosts, "", now);
    std::vector<bool> placed(hosts.size(), false);
    Group pinned{"Pinned", {}, {}, false};
    for (int i : order) if (hosts[static_cast<std::size_t>(i)].pinned) { pinned.hosts.push_back(i); placed[static_cast<std::size_t>(i)] = true; }
    if (!pinned.hosts.empty()) out.push_back(std::move(pinned));
    std::vector<std::string> folders;
    for (const auto& h : hosts) if (!h.folder.empty() && std::find(folders.begin(), folders.end(), h.folder) == folders.end()) folders.push_back(h.folder);
    std::sort(folders.begin(), folders.end());
    for (const auto& f : folders) {
        const bool shut = std::find(collapsed.begin(), collapsed.end(), f) != collapsed.end();
        Group g{f, {}, f, shut};
        for (int i : order) {
            if (placed[static_cast<std::size_t>(i)] || hosts[static_cast<std::size_t>(i)].folder != f) continue;
            g.hosts.push_back(i);
            placed[static_cast<std::size_t>(i)] = true;
        }
        if (!g.hosts.empty()) out.push_back(std::move(g));
    }
    Group mine{"Saved here", {}, {}, false}, rest{"~/.ssh/config", {}, {}, false}, known{"Seen before", {}, {}, false};
    for (int i : order) {
        if (placed[static_cast<std::size_t>(i)]) continue;
        const auto& h = hosts[static_cast<std::size_t>(i)];
        (h.source == "known" ? known : h.editable ? mine : rest).hosts.push_back(i);
    }
    for (auto* g : {&mine, &rest, &known}) if (!g->hosts.empty()) out.push_back(std::move(*g));
    return out;
}
/// The selectable rows, in order (a folded folder's hosts are hidden).
[[nodiscard]] inline std::vector<int> flat(const std::vector<Group>& gs) {
    std::vector<int> v;
    for (const auto& g : gs) if (!g.collapsed) v.insert(v.end(), g.hosts.begin(), g.hosts.end());
    return v;
}
/// "1.2 MB", "340 KB", "12 B"
[[nodiscard]] inline std::string human_size(std::uint64_t n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int k = 0;
    while (v >= 1024.0 && k < 4) { v /= 1024.0; ++k; }
    char b[32];
    std::snprintf(b, sizeof b, k == 0 ? "%.0f %s" : v < 10 ? "%.1f %s" : "%.0f %s", v, u[k]);
    return b;
}
/// The folder defaults that apply to `folder` (nearest first), for hints.
[[nodiscard]] inline SshFolder inherited_for(const std::vector<SshFolder>& folders, std::string folder) {
    SshFolder out;
    while (!folder.empty()) {
        for (const auto& f : folders) {
            if (f.name != folder) continue;
            if (out.user.empty()) out.user = f.user;
            if (out.jump.empty()) out.jump = f.jump;
            if (out.identity.empty()) out.identity = f.identity;
            if (out.port == 0) out.port = f.port;
            if (out.name.empty()) out.name = f.name;
        }
        const auto s = folder.rfind('/');
        folder = s == std::string::npos ? std::string{} : folder.substr(0, s);
    }
    return out;
}
/// A local forward to a web-looking port: offer to open it.
[[nodiscard]] inline bool webby(int port) {
    for (int p : {80, 443, 3000, 3001, 4200, 5000, 5173, 8000, 8080, 8081, 8443, 8888, 9000, 9090, 9200, 15672})
        if (p == port) return true;
    return false;
}
}  // namespace ssh_manager_detail

// ── ops ────────────────────────────────────────────────────────────────────

namespace ssh_manager_ops {
namespace d = ssh_manager_detail;
using A = SshManagerAction;

/// Open the editor on host `i` (-1: a new one, prefilled from `seed`).
inline void begin_edit(SshManagerState& st, const std::vector<SshHost>& hosts, int i, std::string_view seed = {}) {
    st.editing = true;
    st.editing_folder = false;
    st.edit_host = i;
    st.error.clear();
    st.confirm_delete = false;
    st.filtering = false;
    for (auto& f : st.fields) f.clear();
    const auto put = [&](SshField f, std::string s) { auto& e = st.fields[static_cast<std::size_t>(f)]; e.text = std::move(s); e.caret = e.anchor = e.text.size(); };
    if (i >= 0 && i < static_cast<int>(hosts.size())) {
        const auto& h = hosts[static_cast<std::size_t>(i)];
        // shown values may come from the folder (inherited bits): editing
        // must not copy them into the host, or it stops following the folder
        put(SshField::alias, h.alias);
        put(SshField::hostname, h.hostname == h.alias ? std::string{} : h.hostname);
        put(SshField::user, h.inherited & 1 ? std::string{} : h.user);
        put(SshField::port, h.inherited & 8 ? std::string{} : h.port != 22 || !h.editable ? std::to_string(h.port) : std::string{});
        put(SshField::jump, h.inherited & 2 ? std::string{} : h.jump);
        put(SshField::identity, h.inherited & 4 ? std::string{} : h.identity);
        put(SshField::folder, h.folder);
        put(SshField::tags, d::join(h.tags, ", "));
        put(SshField::forwards, d::join(h.forwards, ", "));
        put(SshField::command, h.command);
        st.env = static_cast<int>(h.env);
        st.focus = h.editable ? SshField::alias : SshField::tags;
        // a config host starts on its notes: show them, not the locked fields
        st.detail_scroll.offset.y = h.editable ? 0.0f : 330.0f;
    } else {
        st.env = 0;
        st.detail_scroll.offset.y = 0.0f;
        // "deploy@10.0.3.4:2222" typed in the filter: start from it
        std::string s{seed};
        if (const auto at = s.find('@'); at != std::string::npos) { put(SshField::user, s.substr(0, at)); s = s.substr(at + 1); }
        if (const auto c = s.rfind(':'); c != std::string::npos && s.find(']') == std::string::npos) {
            put(SshField::port, s.substr(c + 1));
            s = s.substr(0, c);
        }
        if (!s.empty()) put(SshField::hostname, s);
        st.focus = SshField::alias;
    }
}
inline void cancel_edit(SshManagerState& st) { st.editing = false; st.error.clear(); st.confirm_delete = false; }
/// Fields the folder editor uses: Name (alias), User, Port, Jump, Key.
[[nodiscard]] inline bool folder_field(SshField f) {
    return f == SshField::alias || f == SshField::user || f == SshField::port || f == SshField::jump || f == SshField::identity;
}
/// Edit folder `name`'s defaults ("" with `seed`: a new folder).
inline void begin_folder_edit(SshManagerState& st, const std::vector<SshFolder>& folders, const std::string& name) {
    st.editing = true;
    st.editing_folder = true;
    st.edit_folder = name;
    st.error.clear();
    st.filtering = false;
    st.import_open = false;
    for (auto& f : st.fields) f.clear();
    const auto put = [&](SshField f, std::string s) { auto& e = st.fields[static_cast<std::size_t>(f)]; e.text = std::move(s); e.caret = e.anchor = e.text.size(); };
    put(SshField::alias, name);
    for (const auto& f : folders)
        if (f.name == name) {
            put(SshField::user, f.user); put(SshField::jump, f.jump); put(SshField::identity, f.identity);
            if (f.port > 0) put(SshField::port, std::to_string(f.port));
        }
    st.focus = name.empty() ? SshField::alias : SshField::user;
    st.detail_scroll.offset.y = 0.0f;
}
inline void save_folder(SshManagerState& st) {
    const auto get = [&](SshField f) {
        std::string s = d::text_of(st, f);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    A a;
    a.kind = A::Kind::save_folder;
    a.old_alias = st.edit_folder;
    a.folder.name = get(SshField::alias);
    for (char& ch : a.folder.name) if (ch == '\\') ch = '/';
    while (!a.folder.name.empty() && a.folder.name.back() == '/') a.folder.name.pop_back();
    a.folder.user = get(SshField::user); a.folder.jump = get(SshField::jump); a.folder.identity = get(SshField::identity);
    if (a.folder.name.empty()) { st.error = "Name: what the folder is called"; return; }
    for (const auto* s : {&a.folder.user, &a.folder.jump, &a.folder.identity})
        if (s->find_first_of(" \t'\"`$;|&<>") != std::string::npos) { st.error = "no spaces or shell characters"; return; }
    if (const std::string p = get(SshField::port); !p.empty()) {
        const int n = std::all_of(p.begin(), p.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }) && p.size() <= 5 ? std::atoi(p.c_str()) : 0;
        if (n <= 0 || n > 65535) { st.error = "Port: a number 1-65535"; return; }
        a.folder.port = n;
    }
    st.out.push_back(std::move(a));
    st.editing = false;
    st.editing_folder = false;
    st.error.clear();
}
/// Show the Files tab (listing the host's files if they are not shown yet).
inline void open_files(SshManagerState& st, const std::vector<SshHost>& hosts, const std::string& listed_alias) {
    st.tab = 1;
    st.editing = false;
    st.files_sel = 0;
    if (st.sel >= 0 && st.sel < static_cast<int>(hosts.size()) && hosts[static_cast<std::size_t>(st.sel)].alias != listed_alias)
        st.out.push_back({A::Kind::files_open, st.sel, {}, {}, "", {}});
}
inline void save(SshManagerState& st, const std::vector<SshHost>& hosts) {
    if (st.editing_folder) { save_folder(st); return; }
    std::string err;
    SshHost h = d::draft_of(st, hosts, &err);
    if (!err.empty()) { st.error = err; return; }
    std::string old;
    if (st.edit_host >= 0 && st.edit_host < static_cast<int>(hosts.size())) old = hosts[static_cast<std::size_t>(st.edit_host)].alias;
    st.out.push_back({A::Kind::save, st.edit_host, std::move(h), std::move(old)});
    st.editing = false;
    st.error.clear();
}
/// Next / previous field the user may edit for this host, scrolled into view.
inline void step_focus(SshManagerState& st, const std::vector<SshHost>& hosts, int dir) {
    const bool editable = st.edit_host < 0 || (st.edit_host < static_cast<int>(hosts.size()) && hosts[static_cast<std::size_t>(st.edit_host)].editable);
    const int n = static_cast<int>(SshField::count_);
    int f = static_cast<int>(st.focus);
    for (int k = 0; k < n; ++k) {
        f = (f + dir + n) % n;
        const auto sf = static_cast<SshField>(f);
        if (st.editing_folder) { if (folder_field(sf)) { st.focus = sf; return; } continue; }
        if (editable || !d::field_owned_by_config(sf)) { st.focus = sf; break; }
    }
    // The editor's rows are one height (fields are single-line); a section
    // heading + card edge sits before CONNECTION, ORGANISE and SESSION.
    constexpr float kTop = 80.0f, kRow = 48.0f, kSection = 44.0f;
    const int i = static_cast<int>(st.focus);
    const int sections = i <= static_cast<int>(SshField::identity) ? 1 : i <= static_cast<int>(SshField::tags) ? 2 : 3;
    const float y = kTop + kSection * static_cast<float>(sections) + kRow * static_cast<float>(i);
    const float view = st.detail_scroll.viewport.y > 0.0f ? st.detail_scroll.viewport.y : 480.0f;
    if (y - 40.0f < st.detail_scroll.offset.y) st.detail_scroll.offset.y = std::max(0.0f, y - 40.0f);
    else if (y + kRow + 90.0f > st.detail_scroll.offset.y + view) st.detail_scroll.offset.y = y + kRow + 90.0f - view;
}
inline void move_sel(SshManagerState& st, const std::vector<SshHost>& hosts, std::int64_t now, int dir) {
    const auto groups = d::groups_of(hosts, st.filter.text, now, st.collapsed);
    const auto v = d::flat(groups);
    if (v.empty()) return;
    auto it = std::find(v.begin(), v.end(), st.sel);
    long at = it == v.end() ? (dir > 0 ? -1 : 0) : static_cast<long>(it - v.begin());
    at = std::clamp(at + dir, 0L, static_cast<long>(v.size()) - 1);
    st.sel = v[static_cast<std::size_t>(at)];
    st.confirm_delete = false;
    st.tab = 0;
    // keep it in view: the row's y in the list (headers between groups)
    float y = 0.0f;
    for (const auto& g : groups) {
        y += d::kHeadH;
        if (g.collapsed) continue;
        for (int i : g.hosts) {
            if (i == st.sel) {
                if (y < st.list_scroll.offset.y) st.list_scroll.offset.y = std::max(0.0f, y - d::kHeadH);
                else if (y + d::kRowH > st.list_scroll.offset.y + st.list_scroll.viewport.y)
                    st.list_scroll.offset.y = y + d::kRowH - st.list_scroll.viewport.y;
                return;
            }
            y += d::kRowH;
        }
    }
}
}  // namespace ssh_manager_ops

/// The keyboard. `hosts` as given to the view. nullopt: not for the manager.
[[nodiscard]] inline std::optional<SshManagerOp> ssh_manager_key(const SshManagerState& st, const std::vector<SshHost>& hosts,
                                                                std::int64_t now, Key k, Mods md, const SshFilesView* files = nullptr) {
    namespace o = ssh_manager_ops;
    namespace d = ssh_manager_detail;
    using A = SshManagerAction;
    auto hp = std::make_shared<const std::vector<SshHost>>(hosts);
    const bool primary = md.ctrl || md.super;
    if (st.import_open && !st.editing) {
        if (k == Key::escape) return SshManagerOp{[](SshManagerState& s) { s.import_open = false; }};
        return SshManagerOp{[](SshManagerState&) {}};
    }
    // the Files tab: a file list with its own keys
    if (!st.editing && !st.filtering && st.tab == 1 && files != nullptr) {
        const int n = static_cast<int>(files->entries.size());
        const int cur = std::clamp(st.files_sel, 0, std::max(n - 1, 0));
        const SshRemoteEntry* e = n > 0 ? &files->entries[static_cast<std::size_t>(cur)] : nullptr;
        const std::string here = files->path;
        const auto act = [&](A::Kind kind, std::string path) {
            return SshManagerOp{[kind, i = st.sel, p = std::move(path)](SshManagerState& s) { s.out.push_back({kind, i, {}, {}, p, {}}); }};
        };
        const auto into = [&](const SshRemoteEntry& x) { return here.empty() ? x.name : (here.back() == '/' ? here + x.name : here + "/" + x.name); };
        const auto up_dir = [&] {
            std::string p = here;
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            const auto sl = p.rfind('/');
            return sl == std::string::npos ? std::string{"/"} : sl == 0 ? std::string{"/"} : p.substr(0, sl);
        };
        switch (k) {
            case Key::escape: return SshManagerOp{[](SshManagerState& s) { s.tab = 0; }};
            case Key::down: case Key::j:
                return SshManagerOp{[n](SshManagerState& s) { s.files_sel = std::min(s.files_sel + 1, std::max(n - 1, 0));
                    const float y = 36.0f * static_cast<float>(s.files_sel);
                    if (y + 36.0f > s.files_scroll.offset.y + s.files_scroll.viewport.y) s.files_scroll.offset.y = y + 36.0f - s.files_scroll.viewport.y; }};
            case Key::up: case Key::k:
                return SshManagerOp{[](SshManagerState& s) { s.files_sel = std::max(s.files_sel - 1, 0);
                    const float y = 36.0f * static_cast<float>(s.files_sel);
                    if (y < s.files_scroll.offset.y) s.files_scroll.offset.y = y; }};
            case Key::enter: case Key::right:
                if (!e) return std::nullopt;
                if (e->dir || e->link) return act(A::Kind::files_open, into(*e));
                return act(A::Kind::files_get, into(*e));
            case Key::backspace: case Key::left: return act(A::Kind::files_open, up_dir());
            case Key::d: if (e) return act(A::Kind::files_get, into(*e)); return std::nullopt;
            case Key::u: return act(A::Kind::files_put, here);
            case Key::t: return act(A::Kind::files_terminal, e && e->dir ? into(*e) : here);
            case Key::y: return act(A::Kind::files_copy_path, e ? into(*e) : here);
            case Key::r: case Key::f5: return act(A::Kind::files_open, here);
            case Key::period: return SshManagerOp{[](SshManagerState& s) { s.show_hidden = !s.show_hidden; s.files_sel = 0; }};
            default: break;
        }
    }
    if (st.editing) {
        if (k == Key::escape) return SshManagerOp{[](SshManagerState& s) { o::cancel_edit(s); }};
        if (k == Key::enter || (primary && k == Key::s)) return SshManagerOp{[hp](SshManagerState& s) { o::save(s, *hp); }};
        if (k == Key::tab) return SshManagerOp{[hp, dir = md.shift ? -1 : 1](SshManagerState& s) { o::step_focus(s, *hp, dir); }};
        if (st.focus == SshField::env) {
            if (k == Key::left) return SshManagerOp{[](SshManagerState& s) { s.env = std::max(0, s.env - 1); }};
            if (k == Key::right) return SshManagerOp{[](SshManagerState& s) { s.env = std::min(3, s.env + 1); }};
            if (k == Key::down) return SshManagerOp{[hp](SshManagerState& s) { o::step_focus(s, *hp, 1); }};
            if (k == Key::up) return SshManagerOp{[hp](SshManagerState& s) { o::step_focus(s, *hp, -1); }};
            return SshManagerOp{[](SshManagerState&) {}};
        }
        if (k == Key::down) return SshManagerOp{[hp](SshManagerState& s) { o::step_focus(s, *hp, 1); }};
        if (k == Key::up) return SshManagerOp{[hp](SshManagerState& s) { o::step_focus(s, *hp, -1); }};
        return SshManagerOp{[k, md](SshManagerState& s) {
            (void)s.fields[static_cast<std::size_t>(s.focus)].handle_key(k, md);
            s.error.clear();
        }};
    }
    if (st.filtering) {
        if (k == Key::escape) return SshManagerOp{[](SshManagerState& s) { if (!s.filter.text.empty()) s.filter.clear(); else s.filtering = false; }};
        if (k == Key::down || k == Key::up)
            return SshManagerOp{[hp, now, dir = k == Key::down ? 1 : -1](SshManagerState& s) { o::move_sel(s, *hp, now, dir); }};
        if (k == Key::enter) {
            const auto v = d::flat(d::groups_of(hosts, st.filter.text, now));
            if (v.empty() && !st.filter.text.empty())   // nothing matches: make it a host
                return SshManagerOp{[hp, t = st.filter.text](SshManagerState& s) { o::begin_edit(s, *hp, -1, t); }};
            return SshManagerOp{[hp, now](SshManagerState& s) {
                s.filtering = false;
                const auto f = d::flat(d::groups_of(*hp, s.filter.text, now));
                if (!f.empty() && std::find(f.begin(), f.end(), s.sel) == f.end()) s.sel = f.front();
            }};
        }
        return SshManagerOp{[hp, now, k, md](SshManagerState& s) {
            (void)s.filter.handle_key(k, md);
            const auto f = d::flat(d::groups_of(*hp, s.filter.text, now));
            if (!f.empty()) s.sel = f.front();
        }};
    }
    const bool has = st.sel >= 0 && st.sel < static_cast<int>(hosts.size());
    switch (k) {
        case Key::escape:
            if (st.confirm_delete) return SshManagerOp{[](SshManagerState& s) { s.confirm_delete = false; }};
            return SshManagerOp{[](SshManagerState& s) { s.close = true; }};
        case Key::down: case Key::j:
            if (k == Key::j && !md.none()) break;
            return SshManagerOp{[hp, now](SshManagerState& s) { o::move_sel(s, *hp, now, 1); }};
        case Key::up: case Key::k:
            if (k == Key::k && !md.none()) break;
            return SshManagerOp{[hp, now](SshManagerState& s) { o::move_sel(s, *hp, now, -1); }};
        case Key::enter:
            if (!has) return std::nullopt;
            return SshManagerOp{[i = st.sel, here = md.alt](SshManagerState& s) {
                s.out.push_back({here ? A::Kind::connect_here : A::Kind::connect, i, {}, {}});
            }};
        case Key::slash: case Key::f:
            if (k == Key::f && !primary) {
                if (!has) return std::nullopt;
                return SshManagerOp{[hp, listed = files ? files->alias : std::string{}](SshManagerState& s) { o::open_files(s, *hp, listed); }};
            }
            return SshManagerOp{[](SshManagerState& s) { s.filtering = true; }};
        case Key::i:
            if (primary) break;
            return SshManagerOp{[](SshManagerState& s) { s.import_open = !s.import_open; }};
        case Key::n:
            if (primary || md.none())
                return SshManagerOp{[hp](SshManagerState& s) { o::begin_edit(s, *hp, -1); }};
            break;
        case Key::e:
            if (!has) return std::nullopt;
            return SshManagerOp{[hp, i = st.sel](SshManagerState& s) { o::begin_edit(s, *hp, i); }};
        case Key::p:
            if (!has) return std::nullopt;
            return SshManagerOp{[i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::pin, i, {}, {}}); }};
        case Key::y: case Key::c:
            if (!has || (k == Key::c && !primary)) break;
            return SshManagerOp{[i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::copy_command, i, {}, {}}); }};
        case Key::del: case Key::backspace:
            if (!has) return std::nullopt;
            if (!st.confirm_delete) return SshManagerOp{[](SshManagerState& s) { s.confirm_delete = true; }};
            return SshManagerOp{[i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::remove, i, {}, {}}); s.confirm_delete = false; }};
        default: break;
    }
    return std::nullopt;
}

/// Typed text: into the focused field, or the filter (starting it).
[[nodiscard]] inline std::optional<SshManagerOp> ssh_manager_text(const SshManagerState& st, const std::vector<SshHost>& hosts,
                                                                 std::int64_t now, std::string_view text) {
    namespace d = ssh_manager_detail;
    if (st.editing) {
        if (st.focus == SshField::env) return SshManagerOp{[](SshManagerState&) {}};
        return SshManagerOp{[t = std::string{text}](SshManagerState& s) { s.fields[static_cast<std::size_t>(s.focus)].insert(t); s.error.clear(); }};
    }
    if (!st.filtering) {
        // single-key commands are keys, not the start of a filter
        if (text == "j" || text == "k" || text == "n" || text == "e" || text == "p" || text == "y" || text == "f" || text == "i")
            return SshManagerOp{[](SshManagerState&) {}};
        if (st.tab == 1 && text != "/") return SshManagerOp{[](SshManagerState&) {}};   // the file list's letters
        if (st.import_open) return SshManagerOp{[](SshManagerState&) {}};
    }
    // "/" opens the filter and is never part of it (a paste or an IME may
    // deliver "/gpu" as one event)
    std::string typed{text};
    if (st.filter.text.empty() && !typed.empty() && typed.front() == '/') typed.erase(0, 1);
    auto hp = std::make_shared<const std::vector<SshHost>>(hosts);
    return SshManagerOp{[hp, now, t = std::move(typed)](SshManagerState& s) {
        s.filtering = true;
        if (t.empty()) return;
        s.filter.insert(t);
        const auto f = d::flat(d::groups_of(*hp, s.filter.text, now));
        if (!f.empty()) s.sel = f.front();
    }};
}

// ── the view ───────────────────────────────────────────────────────────────

template <ControlContext C, class On>
    requires std::invocable<const On&, SshManagerOp>
[[nodiscard]] inline Node ssh_manager(const C& c, std::string_view id, const std::vector<SshHost>& hosts,
                                      const SshManagerState& st, const SshManagerProps& p, On on) {
    namespace d = ssh_manager_detail;
    namespace o = ssh_manager_ops;
    using A = SshManagerAction;
    const Theme& t = c.theme;
    const std::string base{id};
    const auto paper = t.background, ink = t.text_primary;
    const auto muted = mix(ink, paper, 0.45f), line = mix(paper, ink, 0.09f), faint = mix(ink, paper, 0.62f);
    const auto hp = std::make_shared<const std::vector<SshHost>>(hosts);
    const auto groups = d::groups_of(hosts, st.filter.text, p.now, st.collapsed);
    const auto shown = d::flat(groups);
    const bool has = st.sel >= 0 && st.sel < static_cast<int>(hosts.size());
    const auto op = [on](auto f) { return on(SshManagerOp{std::move(f)}); };
    const auto mono = [&](std::string s, float size, Color<Srgb> col) { return text_owned(std::move(s)) | font(size) | monospace | fg(col); };

    // A pill-shaped button: primary filled, others quiet until hovered.
    const auto button = [&](std::string_view bid, std::string caption, std::string key, bool primary, bool danger, auto click) {
        const auto nid = node_id(base + "." + std::string{bid});
        const bool hov = c.hovered(nid);
        const auto tone = danger ? t.danger : t.accent;
        const auto fill = primary ? (hov ? mix(tone, ink, 0.1f) : tone) : hov ? mix(paper, ink, 0.09f) : mix(paper, ink, 0.045f);
        const auto txt = primary ? readable_on(tone) : danger ? t.danger : ink;
        std::vector<Node> parts;
        const std::string a11y_name = caption;
        parts.push_back((text_owned(std::move(caption)) | font(t.type(Tier::body)) | semibold | fg(txt)).build());
        if (!key.empty())
            parts.push_back((h(mono(std::move(key), t.type(Tier::fine), txt.fade(0.7f))) | height(16.0f) | pad(0, 5.0f) | center
                             | radius(4.0f) | bg(txt.fade(0.1f))).build());
        return (h_list(std::move(parts), 8.0f) | align(Align::center) | height(30.0f) | pad(0, primary ? 14.0f : 12.0f)
                | radius(t.rounded(7.0f)) | bg(fill) | border(1.0f, primary ? tone : mix(paper, ink, 0.12f))
                | pointer_cursor | role(a11y::Role::button) | label(a11y_name) | id_of_raw(nid)
                | on_click([op, click] { return op(click); })).build();
    };

    // ── sidebar ─────────────────────────────────────────────────────────────
    const bool filtering = st.filtering || !st.filter.text.empty();
    auto search = h(node(icon_node(Icon::search, 14.0f, filtering ? t.accent : muted)),
                    node((text_field(t, st.filter, st.filtering, node_id(base + ".filter"), "Filter hosts, #tag", t.type(Tier::body))
                          | grow() | border(0, t.border) | bg(paper.fade(0.0f)) | pad(0)).build()),
                    mono("/", t.type(Tier::fine), faint))
                | gap(8.0f) | align(Align::center) | height(34.0f) | pad(0, 10.0f) | radius(t.rounded(8.0f))
                | bg(mix(paper, ink, 0.04f)) | border(1.0f, st.filtering ? t.accent.fade(0.6f) : line)
                | pointer_cursor | id_of(base + ".search")
                | on_click([op] { return op([](SshManagerState& s) { s.filtering = true; s.editing = false; }); });

    std::vector<Node> list;
    for (const auto& g : groups) {
        if (g.folder.empty()) {
            list.push_back((h(text_owned(g.title) | font(t.type(Tier::fine)) | semibold | tracking(0.9f) | fg(muted),
                              spacer(), mono(std::to_string(g.hosts.size()), t.type(Tier::fine), faint))
                            | align(Align::end) | height(d::kHeadH) | pad(0, 10.0f, 6.0f, 10.0f)).build());
        } else {
            // A folder: click folds it; its gear edits what its hosts share.
            const auto fid = node_id(base + ".folder." + g.folder);
            const auto gid = node_id(base + ".fset." + g.folder);
            const bool fh = c.hovered(fid) || c.hovered(gid);
            const bool shared = std::any_of(p.folders.begin(), p.folders.end(), [&](const SshFolder& f) {
                return f.name == g.folder && (!f.user.empty() || !f.jump.empty() || !f.identity.empty() || f.port > 0); });
            const auto depth = static_cast<float>(std::count(g.folder.begin(), g.folder.end(), '/'));
            const std::string leaf = g.folder.substr(g.folder.rfind('/') == std::string::npos ? 0 : g.folder.rfind('/') + 1);
            list.push_back((h(node(icon_node(g.collapsed ? Icon::chevron_right : Icon::chevron_down, 11.0f, muted)),
                              node(icon_node(g.collapsed ? Icon::folder : Icon::folder_open, 13.0f, fh ? t.accent : t.text_secondary)),
                              text_owned(leaf) | font(t.type(Tier::small)) | semibold | fg(fh ? ink : t.text_secondary) | ellipsis | min_size(0, 0) | grow(),
                              (h(node(icon_node(Icon::settings, 12.0f, c.hovered(gid) ? ink : shared ? t.accent.fade(0.8f) : faint)))
                               | size(22.0f, 22.0f) | center | radius(5.0f) | bg(c.hovered(gid) ? mix(paper, ink, 0.1f) : paper.fade(0.0f))
                               | tooltip(shared ? "Folder settings (its hosts share them)" : "Folder settings") | id_of_raw(gid)
                               | on_click([op, f = g.folder, fl = p.folders] { return op([f, fl](SshManagerState& s) { o::begin_folder_edit(s, fl, f); }); })),
                              mono(std::to_string(g.hosts.size()), t.type(Tier::fine), faint))
                            | gap(6.0f) | align(Align::center) | height(d::kHeadH) | pad(4.0f, 8.0f, 0, 6.0f + 12.0f * depth)
                            | radius(t.rounded(6.0f)) | bg(fh ? mix(paper, ink, 0.035f) : paper.fade(0.0f))
                            | pointer_cursor | id_of_raw(fid)
                            | on_click([op, f = g.folder] {
                                  return op([f](SshManagerState& s) {
                                      auto it = std::find(s.collapsed.begin(), s.collapsed.end(), f);
                                      if (it == s.collapsed.end()) s.collapsed.push_back(f); else s.collapsed.erase(it);
                                  });
                              })).build());
            if (g.collapsed) continue;
        }
        for (int i : g.hosts) {
            const auto& hst = hosts[static_cast<std::size_t>(i)];
            const bool sel = i == st.sel;
            const auto rid = part_id(base + ".row", static_cast<std::size_t>(i));
            const bool hov = c.hovered(rid);
            const auto tone = env_tone(t, hst.env);
            std::vector<Node> top;
            if (hst.pinned) top.push_back((text_owned("\u2605") | font(t.type(Tier::small)) | fg(t.warning)).build());
            top.push_back((mono(hst.alias, t.type(Tier::body), ink) | (sel ? semibold : weight(FontWeight::regular)) | ellipsis | min_size(0, 0) | grow()).build());
            if (hst.env != SshHost::Env::none)
                top.push_back((h(mono(env_name(hst.env), t.type(Tier::fine), tone) | bold | tracking(0.5f)) | height(15.0f) | pad(0, 5.0f) | center
                               | radius(3.0f) | bg(tone.fade(0.14f))).build());
            if (hst.connected)
                top.push_back((box() | size(7.0f, 7.0f) | radius(3.5f) | bg(t.success) | tooltip(std::to_string(hst.sessions) + " open")).build());
            {
                const auto up = std::count(hst.tunnel.begin(), hst.tunnel.end(), SshHost::Tunnel::up);
                const bool bad = std::any_of(hst.tunnel.begin(), hst.tunnel.end(), [](SshHost::Tunnel x) { return x == SshHost::Tunnel::down || x == SshHost::Tunnel::conflict; });
                if (up > 0 || bad)
                    top.push_back((h(mono("\u21C4", t.type(Tier::fine), bad ? t.warning : t.success), mono(std::to_string(up), t.type(Tier::fine), bad ? t.warning : t.success))
                                   | gap(2.0f) | align(Align::center) | height(15.0f) | pad(0, 4.0f) | radius(3.0f)
                                   | bg((bad ? t.warning : t.success).fade(0.12f))
                                   | tooltip(bad ? "a tunnel is not running" : std::to_string(up) + (up == 1 ? " tunnel up" : " tunnels up"))).build());
            }
            const std::string age = detail::host_age(hst.last_used, p.now);
            std::vector<Node> sub;
            sub.push_back((mono(d::host_line(hst), t.type(Tier::fine), muted) | ellipsis | min_size(0, 0) | grow()).build());
            if (!age.empty()) sub.push_back(mono(age, t.type(Tier::fine), faint).build());
            list.push_back((v(h_list(std::move(top), 6.0f) | align(Align::center) | width(pct(100)),
                              h_list(std::move(sub), 8.0f) | align(Align::center) | width(pct(100)))
                            | gap(2.0f) | justify(Justify::center) | height(d::kRowH) | width(pct(100)) | pad(0, 10.0f)
                            | radius(t.rounded(7.0f))
                            | bg(sel ? mix(paper, ink, 0.09f) : hov ? mix(paper, ink, 0.045f) : paper.fade(0.0f))
                            | when(sel, inner_shadow(0.0f, t.accent, {2.0f, 0.0f}))
                            | pointer_cursor | id_of_raw(rid)
                            | on_click([op, i](const Clicked& ck) {
                                  // click selects; a double-click connects
                                  return op([i, twice = ck.count >= 2](SshManagerState& s) {
                                      s.sel = i;
                                      s.editing = false;
                                      s.confirm_delete = false;
                                      s.tab = 0;
                                      if (twice) s.out.push_back({A::Kind::connect, i, {}, {}});
                                  });
                              })).build());
        }
    }
    if (shown.empty()) {
        const bool typed = !st.filter.text.empty();
        list.push_back((v(text_owned(typed ? "No host matches" : "No hosts yet") | font(t.type(Tier::body)) | fg(t.text_secondary),
                          text_owned(typed ? "Enter makes it a new host" : "Press n to add one") | font(t.type(Tier::small)) | fg(muted))
                        | gap(4.0f) | align(Align::center) | width(pct(100)) | pad(28.0f, 0)).build());
    }
    Node list_view = scroll_view(c, base + ".list", st.list_scroll, v(std::move(list)) | width(pct(100)) | pad(0, 0, 8.0f, 0),
                                 [op](ScrollOp so) { return op([so](SshManagerState& s) { so(s.list_scroll); }); });

    const auto new_id = node_id(base + ".new");
    auto new_btn = h(node(icon_node(Icon::plus, 13.0f, c.hovered(new_id) ? ink : t.text_secondary)),
                     text_owned("New") | font(t.type(Tier::small)) | semibold | fg(c.hovered(new_id) ? ink : t.text_secondary))
                 | gap(5.0f) | align(Align::center) | height(28.0f) | pad(0, 10.0f) | radius(t.rounded(7.0f))
                 | bg(c.hovered(new_id) ? mix(paper, ink, 0.09f) : mix(paper, ink, 0.05f)) | border(1.0f, line)
                 | pointer_cursor | tooltip("New host (n)") | id_of_raw(new_id)
                 | on_click([op, hp] { return op([hp](SshManagerState& s) { o::begin_edit(s, *hp, -1); }); });
    const auto imp_id = node_id(base + ".import");
    int fresh_total = 0;
    for (const auto& s : p.imports) fresh_total += s.fresh;
    auto import_btn = h(node(icon_node(Icon::arrow_down, 13.0f, st.import_open || c.hovered(imp_id) ? ink : t.text_secondary)),
                        when(fresh_total > 0, h(mono(std::to_string(fresh_total), t.type(Tier::fine), readable_on(t.accent)) | bold)
                                                 | height(14.0f) | pad(0, 4.0f) | center | radius(7.0f) | bg(t.accent)))
                    | gap(4.0f) | align(Align::center) | height(28.0f) | pad(0, 8.0f) | radius(t.rounded(7.0f))
                    | bg(st.import_open || c.hovered(imp_id) ? mix(paper, ink, 0.09f) : mix(paper, ink, 0.05f)) | border(1.0f, line)
                    | pointer_cursor | tooltip("Import from PuTTY, MobaXterm (i)") | id_of_raw(imp_id)
                    | on_click([op] { return op([](SshManagerState& s) { s.import_open = !s.import_open; }); });
    auto side_head = h(v(text_owned("SSH hosts") | font(t.type(Tier::large)) | semibold | fg(ink),
                         text_owned(std::to_string(hosts.size()) + (hosts.size() == 1 ? " host" : " hosts")) | font(t.type(Tier::fine)) | fg(muted))
                           | gap(1.0f) | grow(),
                       std::move(import_btn), std::move(new_btn))
                   | gap(6.0f) | align(Align::center) | width(pct(100));
    // The import drawer: under the header, one row per client found.
    std::vector<Node> drawer;
    if (st.import_open) {
        if (p.imports.empty())
            drawer.push_back((v(text_owned("Nothing to import") | font(t.type(Tier::small)) | semibold | fg(t.text_secondary),
                                text_owned("No PuTTY sessions or MobaXterm bookmarks were found on this machine.") | font(t.type(Tier::small)) | fg(muted) | wrap_text | width(pct(100)))
                              | gap(3.0f) | width(pct(100))).build());
        for (std::size_t q = 0; q < p.imports.size(); ++q) {
            const auto& s = p.imports[q];
            const auto rid = part_id(base + ".imp", q);
            const bool none = s.fresh == 0;
            drawer.push_back((h(v(text_owned(s.name) | font(t.type(Tier::small)) | semibold | fg(ink),
                                  text_owned(std::to_string(s.total) + (s.total == 1 ? " session" : " sessions")
                                             + (none ? ", all here already" : ", " + std::to_string(s.fresh) + " new")) | font(t.type(Tier::fine)) | fg(muted))
                                    | gap(1.0f) | grow() | min_size(0, 0),
                                h(text_owned(none ? "Done" : "Import") | font(t.type(Tier::small)) | semibold
                                      | fg(none ? muted : c.hovered(rid) ? readable_on(t.accent) : t.accent))
                                    | height(26.0f) | pad(0, 10.0f) | center | radius(t.rounded(6.0f))
                                    | bg(none ? paper.fade(0.0f) : c.hovered(rid) ? t.accent : t.accent.fade(0.12f))
                                    | when(!none, pointer_cursor) | id_of_raw(rid)
                                    | on_click([op, q, none] {
                                          return op([q, none](SshManagerState& s2) {
                                              if (none) return;
                                              s2.out.push_back({A::Kind::import_source, static_cast<int>(q), {}, {}, {}, {}});
                                              s2.import_open = false;
                                          });
                                      }))
                              | gap(8.0f) | align(Align::center) | width(pct(100)) | tooltip(s.where)).build());
        }
    }
    auto sidebar = v(v(std::move(side_head),
                       when(st.import_open, v(std::move(drawer)) | gap(10.0f) | pad(10.0f, 12.0f) | width(pct(100)) | radius(t.rounded(9.0f))
                                                | bg(mix(paper, ink, 0.045f)) | border(1.0f, t.accent.fade(0.35f))),
                       std::move(search))
                         | gap(12.0f) | pad(16.0f, 14.0f, 6.0f, 14.0f) | width(pct(100)),
                     node(std::move(list_view)) | grow() | min_size(0, 0) | width(pct(100)) | pad(0, 6.0f),
                     h(mono(p.source, t.type(Tier::fine), faint) | ellipsis | grow() | min_size(0, 0)) | pad(10.0f, 16.0f) | width(pct(100))
                         | inner_shadow(0.0f, line, {0.0f, 1.0f}))
                 | width(d::kSideW) | height(pct(100)) | bg(mix(paper, ink, 0.02f)) | inner_shadow(0.0f, line, {-1.0f, 0.0f});

    // ── detail / editor ────────────────────────────────────────────────────
    std::vector<Node> pane;
    const auto section = [&](std::string title) {
        return (text_owned(std::move(title)) | font(t.type(Tier::fine)) | semibold | tracking(1.0f) | fg(muted) | pad(10.0f, 0, 2.0f, 0)).build();
    };
    const auto card = [&](std::vector<Node> rows) {
        return (v(std::move(rows)) | width(pct(100)) | pad(4.0f, 16.0f) | radius(t.rounded(10.0f))
                | bg(mix(paper, ink, 0.025f)) | border(1.0f, mix(paper, ink, 0.075f))).build();
    };
    const auto kv = [&](std::string k, std::string v_, bool last_row, Color<Srgb> col) {
        return (h(text_owned(std::move(k)) | font(t.type(Tier::small)) | fg(muted) | width(110.0f),
                  mono(v_.empty() ? std::string{"\u2014"} : std::move(v_), t.type(Tier::small), v_.empty() ? faint : col) | grow() | ellipsis | min_size(0, 0))
                | align(Align::center) | height(36.0f) | width(pct(100))
                | inner_shadow(0.0f, last_row ? line.fade(0.0f) : line.fade(0.7f), {0.0f, -1.0f})).build();
    };

    if (st.editing) {
        const bool is_new = st.edit_host < 0 || st.edit_host >= static_cast<int>(hosts.size());
        const SshHost* orig = st.editing_folder || is_new ? nullptr : &hosts[static_cast<std::size_t>(st.edit_host)];
        const bool owns = st.editing_folder || is_new || orig->editable;
        std::string live_err;
        const SshHost draft = st.editing_folder ? SshHost{} : d::draft_of(st, hosts, &live_err);
        // what this host would inherit from its folder, as placeholders
        const SshFolder inh = st.editing_folder ? SshFolder{} : d::inherited_for(p.folders, draft.folder);
        std::vector<std::string> folder_names;
        for (const auto& h_ : hosts) if (!h_.folder.empty() && std::find(folder_names.begin(), folder_names.end(), h_.folder) == folder_names.end()) folder_names.push_back(h_.folder);
        for (const auto& f_ : p.folders) if (std::find(folder_names.begin(), folder_names.end(), f_.name) == folder_names.end()) folder_names.push_back(f_.name);
        std::sort(folder_names.begin(), folder_names.end());
        if (st.editing_folder) {
            std::size_t members = 0;
            for (const auto& h_ : hosts) members += h_.folder == st.edit_folder || h_.folder.rfind(st.edit_folder + "/", 0) == 0 ? 1 : 0;
            pane.push_back((v(h(node(icon_node(Icon::folder_open, 22.0f, t.accent)),
                                text_owned(st.edit_folder.empty() ? std::string{"New folder"} : st.edit_folder) | font(t.type(Tier::title)) | semibold | fg(ink))
                                  | gap(10.0f) | align(Align::center),
                              text_owned("What its " + std::to_string(members) + (members == 1 ? " host shares" : " hosts share")
                                         + " (and those in sub-folders): a host's own value always wins. Renaming moves them all.")
                                  | font(t.type(Tier::small)) | fg(muted) | wrap_text | width(pct(100)))
                            | gap(6.0f) | width(pct(100))).build());
        } else
        pane.push_back((v(text_owned(is_new ? "New host" : "Edit " + orig->alias) | font(t.type(Tier::title)) | semibold | fg(ink),
                          text_owned(owns ? "Saved in " + p.store + ". Tab moves between fields; Enter saves."
                                          : "Connection details come from " + p.source + "; hand keeps your notes, tunnels and command.")
                              | font(t.type(Tier::small)) | fg(muted) | wrap_text | width(pct(100)))
                        | gap(4.0f) | width(pct(100))).build());
        const auto field_row = [&](SshField f, bool last_row) {
            const auto fid = node_id(base + ".f", static_cast<std::size_t>(f));
            const bool focused = st.focus == f;
            const bool locked = !owns && d::field_owned_by_config(f);
            // a field left empty takes the folder's value: say which, in place
            std::string hint = d::field_hint(f);
            if (st.editing_folder && f == SshField::alias) hint = "Work/EU";
            else if (!st.editing_folder && !inh.name.empty()) {
                const auto from = [&](const std::string& v) { return v + "   \u2190 " + inh.name; };
                if (f == SshField::user && !inh.user.empty()) hint = from(inh.user);
                if (f == SshField::jump && !inh.jump.empty()) hint = from(inh.jump);
                if (f == SshField::identity && !inh.identity.empty()) hint = from(inh.identity);
                if (f == SshField::port && inh.port > 0) hint = from(std::to_string(inh.port));
            }
            Node ctl;
            if (f == SshField::env) {
                static constexpr std::array<const char*, 4> names{"none", "dev", "staging", "prod"};
                std::vector<Node> segs;
                for (int q = 0; q < 4; ++q) {
                    const bool on_ = st.env == q;
                    const auto tone = q == 0 ? t.text_secondary : env_tone(t, static_cast<SshHost::Env>(q));
                    const auto sid = part_id(base + ".env", static_cast<std::size_t>(q));
                    segs.push_back((h(text_owned(names[static_cast<std::size_t>(q)]) | font(t.type(Tier::small)) | (on_ ? semibold : weight(FontWeight::regular))
                                      | fg(on_ ? (q == 0 ? ink : tone) : c.hovered(sid) ? ink : t.text_secondary))
                                    | grow() | height(26.0f) | center | radius(t.rounded(5.0f))
                                    | bg(on_ ? (q == 0 ? mix(paper, ink, 0.12f) : tone.fade(0.16f)) : paper.fade(0.0f))
                                    | pointer_cursor | id_of_raw(sid)
                                    | on_click([op, q] { return op([q](SshManagerState& s) { s.env = q; s.focus = SshField::env; }); })).build());
                }
                ctl = (h_list(std::move(segs), 2.0f) | pad(2.0f) | radius(t.rounded(7.0f)) | width(pct(100))
                       | bg(mix(paper, ink, 0.05f)) | border(1.0f, focused ? t.accent.fade(0.7f) : line)).build();
            } else if (locked) {
                ctl = (h(mono(d::text_of(st, f).empty() ? std::string{"\u2014"} : d::text_of(st, f), t.type(Tier::body), muted) | grow() | ellipsis | min_size(0, 0),
                         node(icon_node(Icon::pin, 12.0f, faint)) | tooltip("from " + p.source))
                       | align(Align::center) | height(32.0f) | pad(0, 10.0f) | width(pct(100))).build();
            } else {
                ctl = (node((text_field(t, st.fields[static_cast<std::size_t>(f)], focused, fid, hint, t.type(Tier::body))
                             | monospace | width(pct(100)) | border(1.0f, focused ? t.accent : line)
                             | radius(t.rounded(7.0f)) | bg(mix(paper, ink, focused ? 0.03f : 0.015f))).build())
                       | width(pct(100))
                       | on_click([op, f] { return op([f](SshManagerState& s) { s.focus = f; }); })).build();
            }
            // under the tunnels: what each one does, or why it is wrong
            std::vector<Node> under;
            if (f == SshField::folder && focused) {
                // the folders that exist, filtered by what is typed: one click files it
                const std::string& typed = d::text_of(st, f);
                std::vector<Node> chips;
                for (const auto& name : folder_names) {
                    if (!typed.empty() && name.find(typed) == std::string::npos) continue;
                    if (name == typed || chips.size() >= 6) continue;
                    const auto cid = node_id(base + ".fchip." + name);
                    chips.push_back((h(node(icon_node(Icon::folder, 11.0f, c.hovered(cid) ? ink : muted)),
                                       mono(name, t.type(Tier::fine), c.hovered(cid) ? ink : t.text_secondary))
                                     | gap(4.0f) | align(Align::center) | height(22.0f) | pad(0, 7.0f) | radius(t.rounded(5.0f))
                                     | bg(c.hovered(cid) ? mix(paper, ink, 0.1f) : mix(paper, ink, 0.05f)) | pointer_cursor | id_of_raw(cid)
                                     | on_click([op, name] {
                                           return op([name](SshManagerState& s) {
                                               auto& e = s.fields[static_cast<std::size_t>(SshField::folder)];
                                               e.text = name; e.caret = e.anchor = name.size();
                                           });
                                       })).build());
                }
                if (!chips.empty()) under.push_back((h_list(std::move(chips), 6.0f) | wrap | width(pct(100))).build());
            }
            if (f == SshField::folder && !inh.name.empty() && (!inh.user.empty() || !inh.jump.empty() || !inh.identity.empty() || inh.port > 0))
                under.push_back((text_owned("shares " + std::string{!inh.user.empty() ? "user " + inh.user + "  " : ""} + (!inh.jump.empty() ? "jump " + inh.jump + "  " : "")
                                            + (!inh.identity.empty() ? "key " + inh.identity + "  " : "") + (inh.port > 0 ? "port " + std::to_string(inh.port) : ""))
                                 | font(t.type(Tier::fine)) | fg(t.accent) | ellipsis | width(pct(100))).build());
            if (f == SshField::forwards)
                for (const auto& fw : d::split_forwards(d::text_of(st, f))) {
                    std::string why;
                    const auto pf = ssh_forward_parse(fw, &why);
                    const auto col = pf ? t.text_secondary : t.danger;
                    under.push_back((h(mono(pf ? std::string{pf->flag() + 1} : "!", t.type(Tier::fine), pf ? t.accent : t.danger) | bold | width(14.0f),
                                       mono(pf ? pf->describe() : fw + "  \u2014  " + why, t.type(Tier::small), col) | ellipsis | min_size(0, 0))
                                     | gap(6.0f) | align(Align::center)).build());
                }
            return (h(text_owned(d::field_label(f)) | font(t.type(Tier::small)) | fg(focused ? ink : t.text_secondary) | width(110.0f) | pad(8.0f, 0, 0, 0),
                      v(node(std::move(ctl)), v(std::move(under)) | gap(3.0f) | pad(under.empty() ? 0.0f : 4.0f, 2.0f, 0, 2.0f)) | grow() | min_size(0, 0))
                    | align(Align::start) | width(pct(100)) | pad(7.0f, 0)
                    | inner_shadow(0.0f, last_row ? line.fade(0.0f) : line.fade(0.6f), {0.0f, -1.0f})).build();
        };
        if (st.editing_folder) {
            pane.push_back(section("FOLDER"));
            std::vector<Node> rows;
            for (SshField f : {SshField::alias, SshField::user, SshField::port, SshField::jump, SshField::identity})
                rows.push_back(field_row(f, f == SshField::identity));
            pane.push_back(card(std::move(rows)));
            if (!st.error.empty())
                pane.push_back((h(node(icon_node(Icon::circle_alert, 14.0f, t.danger)), text_owned(st.error) | font(t.type(Tier::small)) | fg(t.danger) | wrap_text | grow())
                                | gap(8.0f) | align(Align::center) | pad(8.0f, 12.0f) | radius(t.rounded(8.0f)) | bg(t.danger.fade(0.08f))
                                | border(1.0f, t.danger.fade(0.25f)) | width(pct(100))).build());
            if (!st.edit_folder.empty())
                pane.push_back((h(spacer(), button("delfolder", "Remove folder settings", "", false, true,
                                                   [f = st.edit_folder](SshManagerState& s) {
                                                       A a; a.kind = A::Kind::remove_folder; a.old_alias = f;
                                                       s.out.push_back(std::move(a)); s.editing = false; s.editing_folder = false;
                                                   })) | width(pct(100))).build());
        } else {
        pane.push_back(section("CONNECTION"));
        {
            std::vector<Node> rows;
            for (SshField f : {SshField::alias, SshField::hostname, SshField::user, SshField::port, SshField::jump, SshField::identity})
                rows.push_back(field_row(f, f == SshField::identity));
            pane.push_back(card(std::move(rows)));
        }
        pane.push_back(section("ORGANISE"));
        pane.push_back(card({field_row(SshField::folder, false), field_row(SshField::env, false), field_row(SshField::tags, true)}));
        pane.push_back(section("SESSION"));
        pane.push_back(card({field_row(SshField::forwards, false), field_row(SshField::command, true)}));
        // what saving gives you, as a command line; or what is wrong
        const std::string err = !st.error.empty() ? st.error : live_err;
        if (!err.empty())
            pane.push_back((h(node(icon_node(Icon::circle_alert, 14.0f, t.danger)), text_owned(err) | font(t.type(Tier::small)) | fg(t.danger) | wrap_text | grow())
                            | gap(8.0f) | align(Align::center) | pad(8.0f, 12.0f) | radius(t.rounded(8.0f)) | bg(t.danger.fade(0.08f))
                            | border(1.0f, t.danger.fade(0.25f)) | width(pct(100))).build());
        else
            pane.push_back((h(mono("$", t.type(Tier::small), t.accent), mono(d::command_line(draft), t.type(Tier::small), t.text_secondary) | wrap_text | grow() | min_size(0, 0))
                            | gap(8.0f) | pad(10.0f, 12.0f) | radius(t.rounded(8.0f)) | bg(mix(paper, colors::black, 0.15f))
                            | border(1.0f, line) | width(pct(100))).build());
        }
    } else if (has) {
        const auto& hst = hosts[static_cast<std::size_t>(st.sel)];
        const auto tone = env_tone(t, hst.env);
        // the host, big, with its badges
        std::vector<Node> title;
        title.push_back((mono(hst.alias, t.type(Tier::title), ink) | semibold | ellipsis | min_size(0, 0)).build());
        if (hst.env != SshHost::Env::none)
            title.push_back((h(mono(env_name(hst.env), t.type(Tier::fine), tone) | bold | tracking(0.6f)) | height(20.0f) | pad(0, 7.0f) | center
                             | radius(4.0f) | bg(tone.fade(0.15f)) | border(1.0f, tone.fade(0.35f))).build());
        if (hst.connected)
            title.push_back((h(box() | size(7.0f, 7.0f) | radius(3.5f) | bg(t.success),
                               text_owned(std::to_string(hst.sessions) + " open") | font(t.type(Tier::small)) | fg(t.success))
                             | gap(5.0f) | align(Align::center)).build());
        const auto pin_id = node_id(base + ".pin");
        title.push_back(spacer().build());
        title.push_back((h(hst.pinned ? (text_owned("\u2605") | font(t.type(Tier::large)) | fg(t.warning)).build()
                                      : icon_node(Icon::star, 16.0f, c.hovered(pin_id) ? ink : faint, 1.5f))
                         | size(30.0f, 30.0f) | center | radius(t.rounded(7.0f)) | bg(c.hovered(pin_id) ? mix(paper, ink, 0.07f) : paper.fade(0.0f))
                         | pointer_cursor | tooltip(hst.pinned ? "Unpin (p)" : "Pin (p)") | id_of_raw(pin_id)
                         | on_click([op, i = st.sel] { return op([i](SshManagerState& s) { s.out.push_back({A::Kind::pin, i, {}, {}}); }); })).build());
        const std::string src = hst.editable ? "saved in " + p.store : hst.source == "known" ? "from known_hosts" : "from " + p.source;
        const std::string age = detail::host_age(hst.last_used, p.now);
        pane.push_back((v(h_list(std::move(title), 10.0f) | align(Align::center) | width(pct(100)),
                          mono(d::host_line(hst), t.type(Tier::body), t.text_secondary),
                          text_owned(src + (hst.uses > 0 ? "  \u00B7  " + std::to_string(hst.uses) + (hst.uses == 1 ? " connection" : " connections")
                                                               + (age.empty() ? "" : ", last " + age + " ago") : "  \u00B7  never connected"))
                              | font(t.type(Tier::small)) | fg(muted))
                        | gap(5.0f) | width(pct(100))).build());
        // actions
        std::vector<Node> acts;
        acts.push_back(button("connect", "Connect", "\u23CE", true, false, [i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::connect, i, {}, {}}); }));
        acts.push_back(button("here", "In this tab", "\u2325\u23CE", false, false, [i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::connect_here, i, {}, {}}); }));
        acts.push_back(button("edit", "Edit", "e", false, false, [hp, i = st.sel](SshManagerState& s) { o::begin_edit(s, *hp, i); }));
        acts.push_back(button("copy", "Copy command", "y", false, false, [i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::copy_command, i, {}, {}}); }));
        acts.push_back(spacer().build());
        if (st.confirm_delete)
            acts.push_back(button("delete", hst.editable ? "Delete host?" : "Forget notes?", "\u232B", true, true,
                                  [i = st.sel](SshManagerState& s) { s.out.push_back({A::Kind::remove, i, {}, {}}); s.confirm_delete = false; }));
        else if (hst.editable || !hst.tags.empty() || hst.pinned || !hst.forwards.empty() || !hst.command.empty() || hst.source == "history")
            acts.push_back(button("delete", hst.editable ? "Delete" : "Forget", "", false, true, [](SshManagerState& s) { s.confirm_delete = true; }));
        pane.push_back((h_list(std::move(acts), 8.0f) | align(Align::center) | width(pct(100)) | pad(4.0f, 0)).build());
        // connection
        // ── tabs: what the host is / what is on it ────────────────────────
        {
            std::vector<Node> tabs;
            const auto tab = [&](int k, Icon ic, std::string label, std::string key) {
                const auto tid = part_id(base + ".tab", static_cast<std::size_t>(k));
                const bool on_ = st.tab == k, hov = c.hovered(tid);
                return (h(node(icon_node(ic, 13.0f, on_ ? t.accent : hov ? ink : muted)),
                          text_owned(std::move(label)) | font(t.type(Tier::small)) | (on_ ? semibold : weight(FontWeight::regular)) | fg(on_ || hov ? ink : t.text_secondary),
                          mono(std::move(key), t.type(Tier::fine), faint))
                        | gap(6.0f) | align(Align::center) | height(34.0f) | pad(0, 4.0f)
                        | inner_shadow(0.0f, on_ ? t.accent : paper.fade(0.0f), {0.0f, -2.0f})
                        | pointer_cursor | id_of_raw(tid)
                        | on_click([op, k, hp, listed = p.files.alias] {
                              return op([k, hp, listed](SshManagerState& s) { if (k == 1) o::open_files(s, *hp, listed); else s.tab = 0; });
                          })).build();
            };
            tabs.push_back(tab(0, Icon::info, "Overview", ""));
            tabs.push_back(tab(1, Icon::folder, "Files", "f"));
            pane.push_back((h_list(std::move(tabs), 20.0f) | width(pct(100)) | inner_shadow(0.0f, line, {0.0f, -1.0f})).build());
        }
        if (st.tab == 1) {
            // ── the remote file browser ─────────────────────────────────────
            const SshFilesView& fv = p.files;
            const bool mine = fv.alias == hst.alias;
            const std::string here = mine ? fv.path : std::string{};
            const auto act = [&](A::Kind kind, std::string path) {
                return [kind, i = st.sel, path](SshManagerState& s) { s.out.push_back({kind, i, {}, {}, path, {}}); };
            };
            // breadcrumb: "/ home / deploy / logs", every part a jump
            std::vector<Node> crumbs;
            {
                std::string acc;
                std::vector<std::pair<std::string, std::string>> parts{{"/", "/"}};
                std::size_t s0 = 1;
                while (s0 < here.size()) {
                    std::size_t e = here.find('/', s0);
                    if (e == std::string::npos) e = here.size();
                    acc += "/" + here.substr(s0, e - s0);
                    parts.push_back({here.substr(s0, e - s0), acc});
                    s0 = e + 1;
                }
                for (std::size_t q = 0; q < parts.size(); ++q) {
                    const auto bid = part_id(base + ".crumb", q);
                    const bool last = q + 1 == parts.size();
                    if (q > 0) crumbs.push_back(mono("/", t.type(Tier::small), faint).build());
                    crumbs.push_back((h(mono(q == 0 ? std::string{"\u2302"} : parts[q].first, t.type(Tier::small), last ? ink : c.hovered(bid) ? t.accent : t.text_secondary)
                                        | (last ? semibold : weight(FontWeight::regular)))
                                      | pad(2.0f, 4.0f) | radius(4.0f) | bg(c.hovered(bid) && !last ? mix(paper, ink, 0.07f) : paper.fade(0.0f))
                                      | pointer_cursor | id_of_raw(bid) | tooltip(parts[q].second)
                                      | on_click([op, f = act(A::Kind::files_open, parts[q].second)] { return op(f); })).build());
                }
            }
            std::vector<Node> bar;
            bar.push_back((h_list(std::move(crumbs), 2.0f) | align(Align::center) | grow() | min_size(0, 0)).build());
            bar.push_back(button("fup", "Up", "\u232B", false, false, act(A::Kind::files_open, here.empty() ? std::string{} : [&] {
                std::string q = here; while (q.size() > 1 && q.back() == '/') q.pop_back();
                const auto sl = q.rfind('/'); return sl == std::string::npos || sl == 0 ? std::string{"/"} : q.substr(0, sl); }())));
            bar.push_back(button("fupload", "Upload", "u", false, false, act(A::Kind::files_put, here)));
            bar.push_back(button("fterm", "Terminal here", "t", false, false, act(A::Kind::files_terminal, here)));
            bar.push_back(button("frefresh", "\u21BB", "r", false, false, act(A::Kind::files_open, here)));
            pane.push_back((h_list(std::move(bar), 8.0f) | align(Align::center) | width(pct(100)) | pad(6.0f, 0)).build());
            if (!fv.busy.empty() && mine)
                pane.push_back((h(node(icon_node(Icon::refresh, 13.0f, t.accent)), text_owned(fv.busy) | font(t.type(Tier::small)) | fg(t.accent) | grow())
                                | gap(8.0f) | align(Align::center) | pad(7.0f, 12.0f) | radius(t.rounded(7.0f)) | bg(t.accent.fade(0.08f)) | width(pct(100))).build());
            std::vector<Node> rows;
            if (!mine || (fv.loading && fv.entries.empty())) {
                rows.push_back((h(node(icon_node(Icon::refresh, 16.0f, muted)), text_owned("Listing files over SFTP\u2026") | font(t.type(Tier::small)) | fg(muted))
                                | gap(10.0f) | align(Align::center) | pad(26.0f, 0) | width(pct(100)) | justify(Justify::center)).build());
            } else if (!fv.error.empty()) {
                rows.push_back((v(h(node(icon_node(Icon::circle_alert, 16.0f, t.danger)), text_owned("Could not list " + (here.empty() ? std::string{"the files"} : here)) | font(t.type(Tier::body)) | semibold | fg(t.danger)) | gap(8.0f) | align(Align::center),
                                  text_owned(fv.error) | font(t.type(Tier::small)) | fg(t.text_secondary) | wrap_text | width(pct(100)))
                                | gap(6.0f) | pad(14.0f, 16.0f) | width(pct(100))).build());
            } else if (fv.entries.empty()) {
                rows.push_back((text_owned("Empty folder. Upload (u) puts files here.") | font(t.type(Tier::small)) | fg(muted) | pad(20.0f, 16.0f)).build());
            } else {
                for (std::size_t q = 0; q < fv.entries.size(); ++q) {
                    const auto& e = fv.entries[q];
                    const auto rid = part_id(base + ".file", q);
                    const bool sel = static_cast<int>(q) == st.files_sel, hov = c.hovered(rid);
                    const std::string full = here.empty() || here.back() == '/' ? here + e.name : here + "/" + e.name;
                    const auto dl = part_id(base + ".dl", q);
                    rows.push_back((h(node(icon_node(e.dir ? Icon::folder : e.link ? Icon::link : Icon::file, 15.0f, e.dir ? t.accent : e.link ? t.info : muted)),
                                      mono(e.name + (e.link && !e.target.empty() ? "  \u2192 " + e.target : ""), t.type(Tier::small), e.dir ? ink : t.text_secondary)
                                          | grow() | ellipsis | min_size(0, 0),
                                      mono(e.dir ? std::string{} : d::human_size(e.size), t.type(Tier::fine), faint) | width(70.0f) | text_align(TextAlign::right),
                                      mono(e.when, t.type(Tier::fine), faint) | width(96.0f) | text_align(TextAlign::right),
                                      (h(node(icon_node(Icon::arrow_down, 13.0f, c.hovered(dl) ? t.accent : sel || hov ? muted : paper.fade(0.0f))))
                                       | size(26.0f, 26.0f) | center | radius(5.0f) | bg(c.hovered(dl) ? mix(paper, ink, 0.1f) : paper.fade(0.0f))
                                       | tooltip("Download (d)") | id_of_raw(dl)
                                       | on_click([op, f = act(A::Kind::files_get, full)] { return op(f); })))
                                    | gap(10.0f) | align(Align::center) | height(36.0f) | width(pct(100)) | pad(0, 10.0f) | radius(t.rounded(6.0f))
                                    | bg(sel ? mix(paper, ink, 0.08f) : hov ? mix(paper, ink, 0.04f) : paper.fade(0.0f))
                                    | when(sel, inner_shadow(0.0f, t.accent, {2.0f, 0.0f}))
                                    | pointer_cursor | id_of_raw(rid)
                                    | on_click([op, q, i = st.sel, full, dir = e.dir || e.link](const Clicked& ck) {
                                          return op([q, i, full, dir, twice = ck.count >= 2](SshManagerState& s) {
                                              s.files_sel = static_cast<int>(q);
                                              if (twice) s.out.push_back({dir ? A::Kind::files_open : A::Kind::files_get, i, {}, {}, full, {}});
                                          });
                                      })).build());
                }
            }
            pane.push_back((v(std::move(rows)) | width(pct(100)) | pad(4.0f) | radius(t.rounded(10.0f))
                            | bg(mix(paper, ink, 0.02f)) | border(1.0f, mix(paper, ink, 0.075f))).build());
        } else {
        const auto inh_mark = [&](std::string v, std::uint8_t bit) {
            if (!(hst.inherited & bit) || v.empty()) return v;
            // the nearest folder that sets this field: the one it came from
            std::string f = hst.folder;
            while (!f.empty()) {
                for (const auto& x : p.folders) {
                    if (x.name != f) continue;
                    const bool sets = bit == 1 ? !x.user.empty() : bit == 2 ? !x.jump.empty() : bit == 4 ? !x.identity.empty() : x.port > 0;
                    if (sets) return v + "   \u2190 " + f;
                }
                const auto s = f.rfind('/');
                f = s == std::string::npos ? std::string{} : f.substr(0, s);
            }
            return v + "   \u2190 " + hst.folder;
        };
        pane.push_back(section("CONNECTION"));
        pane.push_back(card({kv("Host", hst.hostname, false, ink), kv("User", inh_mark(hst.user, 1), false, ink),
                             kv("Port", inh_mark(std::to_string(hst.port), 8), false, ink), kv("Jump host", inh_mark(hst.jump, 2), false, ink),
                             kv("Key file", inh_mark(hst.identity, 4), !hst.folder.empty() ? false : true, ink)}
                            ));
        if (!hst.folder.empty()) {
            const auto fid = node_id(base + ".infolder");
            pane.push_back((h(node(icon_node(Icon::folder, 13.0f, muted)), text_owned("In " + hst.folder) | font(t.type(Tier::small)) | fg(t.text_secondary),
                              text_owned(hst.editable ? "" : "\u00B7  its shared settings apply to hosts saved here; this one's come from " + p.source)
                                  | font(t.type(Tier::small)) | fg(faint) | ellipsis | min_size(0, 0) | grow(),
                              text_owned("folder settings") | font(t.type(Tier::small)) | fg(c.hovered(fid) ? t.accent : muted) | pointer_cursor | id_of_raw(fid)
                                  | on_click([op, f = hst.folder, fl = p.folders] { return op([f, fl](SshManagerState& s) { o::begin_folder_edit(s, fl, f); }); }))
                            | gap(8.0f) | align(Align::center) | pad(2.0f, 4.0f)).build());
        }
        // tunnels
        pane.push_back(section("TUNNELS"));
        {
            std::vector<Node> rows;
            for (std::size_t q = 0; q < hst.forwards.size(); ++q) {
                const auto fw = ssh_forward_parse(hst.forwards[q]);
                if (!fw) continue;
                const auto kind_col = fw->kind == SshForward::Kind::local ? t.accent : fw->kind == SshForward::Kind::remote ? t.warning : t.success;
                // live: is its local port really up, and whose is it?
                using T = SshHost::Tunnel;
                const T ts = q < hst.tunnel.size() ? hst.tunnel[q] : T::idle;
                const std::string note = q < hst.tunnel_note.size() ? hst.tunnel_note[q] : std::string{};
                const auto scol = ts == T::up ? t.success : ts == T::down ? t.warning : ts == T::conflict ? t.danger : muted;
                const std::string stext = ts == T::up ? "up" : ts == T::down ? "not running" : ts == T::conflict ? "port taken" : ts == T::remote ? "on the server" : "idle";
                std::vector<Node> right;
                right.push_back((h(box() | size(7.0f, 7.0f) | radius(3.5f) | bg(scol), mono(stext, t.type(Tier::fine), scol))
                                 | gap(5.0f) | align(Align::center) | tooltip(note.empty() ? stext : note)).build());
                if (fw->kind == SshForward::Kind::local && d::webby(fw->listen) && ts == T::up) {
                    const auto oid = part_id(base + ".open", q);
                    const std::string url = std::string{fw->listen == 443 || fw->listen == 8443 ? "https" : "http"} + "://localhost:" + std::to_string(fw->listen);
                    right.push_back((h(node(icon_node(Icon::external, 12.0f, c.hovered(oid) ? ink : t.accent)), mono("open", t.type(Tier::fine), c.hovered(oid) ? ink : t.accent))
                                     | gap(4.0f) | align(Align::center) | height(22.0f) | pad(0, 7.0f) | radius(5.0f)
                                     | bg(c.hovered(oid) ? t.accent.fade(0.2f) : t.accent.fade(0.1f)) | pointer_cursor | tooltip(url) | id_of_raw(oid)
                                     | on_click([op, i = st.sel, url] { return op([i, url](SshManagerState& s) { s.out.push_back({A::Kind::open_url, i, {}, {}, url, {}}); }); })).build());
                }
                rows.push_back((h(h(mono(std::string{fw->flag() + 1}, t.type(Tier::fine), kind_col) | bold) | size(20.0f, 20.0f) | center | radius(5.0f) | bg(kind_col.fade(0.14f)),
                                  mono(fw->describe(), t.type(Tier::small), ink) | grow() | ellipsis | min_size(0, 0),
                                  mono(fw->flag() + std::string{" "} + fw->arg(), t.type(Tier::fine), faint),
                                  h_list(std::move(right), 8.0f) | align(Align::center))
                                | gap(10.0f) | align(Align::center) | height(36.0f) | width(pct(100))
                                | inner_shadow(0.0f, q + 1 == hst.forwards.size() ? line.fade(0.0f) : line.fade(0.7f), {0.0f, -1.0f})).build());
            }
            if (rows.empty())
                rows.push_back((h(text_owned("No tunnels. Edit to forward a port (L5432:db:5432) or open a SOCKS proxy (D1080).")
                                  | font(t.type(Tier::small)) | fg(muted) | wrap_text | grow())
                                | pad(10.0f, 0) | width(pct(100))).build());
            pane.push_back(card(std::move(rows)));
        }
        if (!hst.command.empty() || !hst.tags.empty()) {
            pane.push_back(section("SESSION"));
            std::vector<Node> rows;
            if (!hst.command.empty()) rows.push_back(kv("On connect", hst.command, hst.tags.empty(), ink));
            if (!hst.tags.empty()) rows.push_back(kv("Tags", "#" + d::join(hst.tags, "  #"), true, t.text_secondary));
            pane.push_back(card(std::move(rows)));
        }
        // the exact command
        const auto cid = node_id(base + ".cmd");
        pane.push_back(section("COMMAND"));
        pane.push_back((h(mono("$", t.type(Tier::small), t.accent), mono(d::command_line(hst), t.type(Tier::small), t.text_secondary) | wrap_text | grow() | min_size(0, 0),
                          node(icon_node(Icon::copy, 14.0f, c.hovered(cid) ? ink : muted)))
                        | gap(8.0f) | align(Align::center) | pad(10.0f, 12.0f) | radius(t.rounded(8.0f))
                        | bg(c.hovered(cid) ? mix(paper, colors::black, 0.1f) : mix(paper, colors::black, 0.15f)) | border(1.0f, line)
                        | width(pct(100)) | pointer_cursor | tooltip("Copy (y)") | id_of_raw(cid)
                        | on_click([op, i = st.sel] { return op([i](SshManagerState& s) { s.out.push_back({A::Kind::copy_command, i, {}, {}}); }); })).build());
        }   // overview
    } else {
        pane.push_back((v(node(icon_node(Icon::server, 30.0f, muted.fade(0.6f))),
                          text_owned("No host selected") | font(t.type(Tier::body)) | fg(t.text_secondary),
                          text_owned("Pick one on the left, or press n to add a host") | font(t.type(Tier::small)) | fg(muted))
                        | gap(8.0f) | align(Align::center) | width(pct(100)) | pad(90.0f, 0)).build());
    }
    Node detail = scroll_view(c, base + ".detail", st.detail_scroll, v(std::move(pane)) | gap(8.0f) | pad(22.0f, 56.0f, 26.0f, 26.0f) | width(pct(100)),
                              [op](ScrollOp so) { return op([so](SshManagerState& s) { so(s.detail_scroll); }); });

    // ── footer: what the keys do here ─────────────────────────────────────────
    std::vector<Node> keys;
    const auto hint = [&](std::string k, std::string what) {
        return (h(h(mono(std::move(k), t.type(Tier::fine), t.text_secondary)) | height(18.0f) | pad(0, 5.0f) | center | radius(4.0f)
                    | bg(mix(paper, ink, 0.06f)) | border(1.0f, line),
                  text_owned(std::move(what)) | font(t.type(Tier::small)) | fg(muted))
                | gap(6.0f) | align(Align::center)).build();
    };
    if (st.editing) {
        keys.push_back(hint("tab", "next field")); keys.push_back(hint("\u23CE", "save")); keys.push_back(hint("esc", "cancel"));
    } else if (st.filtering) {
        keys.push_back(hint("\u2191\u2193", "pick")); keys.push_back(hint("\u23CE", shown.empty() && !st.filter.text.empty() ? "new host" : "done"));
        keys.push_back(hint("esc", "clear"));
    } else if (st.tab == 1) {
        keys.push_back(hint("\u23CE", "open / get")); keys.push_back(hint("\u232B", "up")); keys.push_back(hint("d", "download"));
        keys.push_back(hint("u", "upload")); keys.push_back(hint("t", "terminal"));
        keys.push_back(hint(".", st.show_hidden ? "hide dotfiles" : "dotfiles")); keys.push_back(hint("esc", "overview"));
    } else {
        keys.push_back(hint("\u23CE", "connect")); keys.push_back(hint("f", "files")); keys.push_back(hint("n", "new")); keys.push_back(hint("e", "edit"));
        keys.push_back(hint("i", "import")); keys.push_back(hint("/", "filter")); keys.push_back(hint("esc", "close"));
    }
    std::vector<Node> right;
    if (st.editing) {
        right.push_back(button("cancel", "Cancel", "", false, false, [](SshManagerState& s) { o::cancel_edit(s); }));
        right.push_back(button("save", "Save", "\u23CE", true, false, [hp](SshManagerState& s) { o::save(s, *hp); }));
    }
    auto footer = h(h_list(std::move(keys), 16.0f) | align(Align::center) | grow(), h_list(std::move(right), 8.0f) | align(Align::center))
                | align(Align::center) | height(50.0f) | pad(0, 16.0f, 0, 22.0f) | width(pct(100))
                | bg(mix(paper, colors::black, luminance(paper) < 0.4f ? 0.12f : 0.0f)) | inner_shadow(0.0f, line, {0.0f, 1.0f});

    const auto xid = node_id(base + ".close");
    auto close = h(node(icon_node(Icon::close, 14.0f, c.hovered(xid) ? ink : muted)))
               | size(30.0f, 30.0f) | center | radius(t.rounded(7.0f)) | bg(c.hovered(xid) ? mix(paper, ink, 0.1f) : paper.fade(0.0f))
               | pointer_cursor | tooltip("Close (esc)") | id_of_raw(xid)
               | on_click([op] { return op([](SshManagerState& s) { s.close = true; }); });

    auto main_col = v(z(node(std::move(detail)) | width(pct(100)) | height(pct(100)),
                        h(spacer(), std::move(close)) | width(pct(100)) | pad(12.0f, 12.0f, 0, 0) | absolute(0.0f, 0.0f))
                          | grow() | min_size(0, 0) | width(pct(100)),
                      std::move(footer))
                    | grow() | height(pct(100)) | min_size(0, 0);
    return (h(std::move(sidebar), std::move(main_col))
            | size(p.width, p.height) | clip | bg(paper) | border(1.0f, mix(paper, ink, 0.15f)) | radius(t.rounded(12.0f))
            | shadow(48.0f, colors::black.fade(0.55f), {0.0f, 18.0f}) | role(a11y::Role::dialog) | label("SSH hosts")).build();
}

}  // namespace mayag::dsl
