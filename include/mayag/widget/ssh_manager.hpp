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
    enum class Kind : std::uint8_t { connect, connect_here, save, remove, pin, copy_command };
    Kind        kind = Kind::connect;
    int         host = -1;        ///< index into the hosts given to the view (-1: a new host)
    SshHost     draft;            ///< save: the host as edited
    std::string old_alias;        ///< save: what it was called ("" = new)
};

/// The editor's fields, in tab order.
enum class SshField : std::uint8_t { alias, hostname, user, port, jump, identity, env, tags, forwards, command, count_ };

struct SshManagerState {
    TextEditState filter;
    int           sel = 0;               ///< index into the hosts (the app's order)
    bool          filtering = false;     ///< the filter has the keyboard
    ScrollState   list_scroll, detail_scroll;
    // the editor: open when `editing`; `edit_host` -1 = a new host
    bool          editing = false;
    int           edit_host = -1;
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

/// Sidebar groups, in order: pinned, then each tag (alphabetical), then the
/// rest. A host sits in its first group only. Filtering: one flat ranked list.
struct Group { std::string title; std::vector<int> hosts; };
[[nodiscard]] inline std::vector<Group> groups_of(const std::vector<SshHost>& hosts, std::string_view q, std::int64_t now) {
    std::vector<Group> out;
    if (!q.empty()) {
        const auto r = rank_hosts(hosts, q, now);
        if (!r.empty()) out.push_back({std::to_string(r.size()) + (r.size() == 1 ? " match" : " matches"), r});
        return out;
    }
    const auto order = rank_hosts(hosts, "", now);
    std::vector<bool> placed(hosts.size(), false);
    Group pinned{"Pinned", {}};
    for (int i : order) if (hosts[static_cast<std::size_t>(i)].pinned) { pinned.hosts.push_back(i); placed[static_cast<std::size_t>(i)] = true; }
    if (!pinned.hosts.empty()) out.push_back(std::move(pinned));
    std::vector<std::string> tags;
    for (const auto& h : hosts) for (const auto& t : h.tags) if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    std::sort(tags.begin(), tags.end());
    for (const auto& t : tags) {
        Group g{"#" + t, {}};
        for (int i : order) {
            const auto& h = hosts[static_cast<std::size_t>(i)];
            if (!placed[static_cast<std::size_t>(i)] && std::find(h.tags.begin(), h.tags.end(), t) != h.tags.end()) {
                g.hosts.push_back(i);
                placed[static_cast<std::size_t>(i)] = true;
            }
        }
        if (!g.hosts.empty()) out.push_back(std::move(g));
    }
    Group mine{"Saved here", {}}, rest{"~/.ssh/config", {}}, known{"Seen before", {}};
    for (int i : order) {
        if (placed[static_cast<std::size_t>(i)]) continue;
        const auto& h = hosts[static_cast<std::size_t>(i)];
        (h.source == "known" ? known : h.editable ? mine : rest).hosts.push_back(i);
    }
    for (auto* g : {&mine, &rest, &known}) if (!g->hosts.empty()) out.push_back(std::move(*g));
    return out;
}
[[nodiscard]] inline std::vector<int> flat(const std::vector<Group>& gs) {
    std::vector<int> v;
    for (const auto& g : gs) v.insert(v.end(), g.hosts.begin(), g.hosts.end());
    return v;
}
}  // namespace ssh_manager_detail

// ── ops ────────────────────────────────────────────────────────────────────

namespace ssh_manager_ops {
namespace d = ssh_manager_detail;
using A = SshManagerAction;

/// Open the editor on host `i` (-1: a new one, prefilled from `seed`).
inline void begin_edit(SshManagerState& st, const std::vector<SshHost>& hosts, int i, std::string_view seed = {}) {
    st.editing = true;
    st.edit_host = i;
    st.error.clear();
    st.confirm_delete = false;
    st.filtering = false;
    for (auto& f : st.fields) f.clear();
    const auto put = [&](SshField f, std::string s) { auto& e = st.fields[static_cast<std::size_t>(f)]; e.text = std::move(s); e.caret = e.anchor = e.text.size(); };
    if (i >= 0 && i < static_cast<int>(hosts.size())) {
        const auto& h = hosts[static_cast<std::size_t>(i)];
        put(SshField::alias, h.alias);
        put(SshField::hostname, h.hostname == h.alias ? std::string{} : h.hostname);
        put(SshField::user, h.user);
        put(SshField::port, h.port != 22 || !h.editable ? std::to_string(h.port) : std::string{});
        put(SshField::jump, h.jump);
        put(SshField::identity, h.identity);
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
inline void save(SshManagerState& st, const std::vector<SshHost>& hosts) {
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
    const auto v = d::flat(d::groups_of(hosts, st.filter.text, now));
    if (v.empty()) return;
    auto it = std::find(v.begin(), v.end(), st.sel);
    long at = it == v.end() ? (dir > 0 ? -1 : 0) : static_cast<long>(it - v.begin());
    at = std::clamp(at + dir, 0L, static_cast<long>(v.size()) - 1);
    st.sel = v[static_cast<std::size_t>(at)];
    st.confirm_delete = false;
    // keep it in view: the row's y in the list (headers between groups)
    float y = 0.0f;
    for (const auto& g : d::groups_of(hosts, st.filter.text, now)) {
        y += d::kHeadH;
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
                                                                std::int64_t now, Key k, Mods md) {
    namespace o = ssh_manager_ops;
    namespace d = ssh_manager_detail;
    using A = SshManagerAction;
    auto hp = std::make_shared<const std::vector<SshHost>>(hosts);
    const bool primary = md.ctrl || md.super;
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
            if (k == Key::f && !primary) break;
            return SshManagerOp{[](SshManagerState& s) { s.filtering = true; }};
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
        if (text == "j" || text == "k" || text == "n" || text == "e" || text == "p" || text == "y") return SshManagerOp{[](SshManagerState&) {}};
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
    const float fs = t.font_size;
    const std::string base{id};
    const auto paper = t.background, ink = t.text_primary;
    const auto muted = mix(ink, paper, 0.45f), line = mix(paper, ink, 0.09f), faint = mix(ink, paper, 0.62f);
    const auto hp = std::make_shared<const std::vector<SshHost>>(hosts);
    const auto groups = d::groups_of(hosts, st.filter.text, p.now);
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
        parts.push_back((text_owned(std::move(caption)) | font(fs * 0.9f) | semibold | fg(txt)).build());
        if (!key.empty())
            parts.push_back((h(mono(std::move(key), fs * 0.7f, txt.fade(0.7f))) | height(16.0f) | pad(0, 5.0f) | center
                             | radius(4.0f) | bg(txt.fade(0.1f))).build());
        return (h_list(std::move(parts), 8.0f) | align(Align::center) | height(30.0f) | pad(0, primary ? 14.0f : 12.0f)
                | radius(t.rounded(7.0f)) | bg(fill) | border(1.0f, primary ? tone : mix(paper, ink, 0.12f))
                | pointer_cursor | role(a11y::Role::button) | label(a11y_name) | id_of_raw(nid)
                | on_click([op, click] { return op(click); })).build();
    };

    // ── sidebar ─────────────────────────────────────────────────────────────
    const bool filtering = st.filtering || !st.filter.text.empty();
    auto search = h(node(icon_node(Icon::search, 14.0f, filtering ? t.accent : muted)),
                    node((text_field(t, st.filter, st.filtering, node_id(base + ".filter"), "Filter hosts, #tag", fs * 0.9f)
                          | grow() | border(0, t.border) | bg(paper.fade(0.0f)) | pad(0)).build()),
                    mono("/", fs * 0.72f, faint))
                | gap(8.0f) | align(Align::center) | height(34.0f) | pad(0, 10.0f) | radius(t.rounded(8.0f))
                | bg(mix(paper, ink, 0.04f)) | border(1.0f, st.filtering ? t.accent.fade(0.6f) : line)
                | pointer_cursor | id_of(base + ".search")
                | on_click([op] { return op([](SshManagerState& s) { s.filtering = true; s.editing = false; }); });

    std::vector<Node> list;
    for (const auto& g : groups) {
        list.push_back((h(text_owned(g.title) | font(fs * 0.72f) | semibold | tracking(0.9f) | fg(muted),
                          spacer(), mono(std::to_string(g.hosts.size()), fs * 0.7f, faint))
                        | align(Align::end) | height(d::kHeadH) | pad(0, 10.0f, 6.0f, 10.0f)).build());
        for (int i : g.hosts) {
            const auto& hst = hosts[static_cast<std::size_t>(i)];
            const bool sel = i == st.sel;
            const auto rid = part_id(base + ".row", static_cast<std::size_t>(i));
            const bool hov = c.hovered(rid);
            const auto tone = env_tone(t, hst.env);
            std::vector<Node> top;
            if (hst.pinned) top.push_back((text_owned("\u2605") | font(fs * 0.8f) | fg(t.warning)).build());
            top.push_back((mono(hst.alias, fs * 0.92f, ink) | (sel ? semibold : weight(FontWeight::regular)) | ellipsis | min_size(0, 0) | grow()).build());
            if (hst.env != SshHost::Env::none)
                top.push_back((h(mono(env_name(hst.env), fs * 0.6f, tone) | bold | tracking(0.5f)) | height(15.0f) | pad(0, 5.0f) | center
                               | radius(3.0f) | bg(tone.fade(0.14f))).build());
            if (hst.connected)
                top.push_back((box() | size(7.0f, 7.0f) | radius(3.5f) | bg(t.success) | tooltip(std::to_string(hst.sessions) + " open")).build());
            const std::string age = detail::host_age(hst.last_used, p.now);
            std::vector<Node> sub;
            sub.push_back((mono(d::host_line(hst), fs * 0.76f, muted) | ellipsis | min_size(0, 0) | grow()).build());
            if (!age.empty()) sub.push_back(mono(age, fs * 0.7f, faint).build());
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
                                      if (twice) s.out.push_back({A::Kind::connect, i, {}, {}});
                                  });
                              })).build());
        }
    }
    if (shown.empty()) {
        const bool typed = !st.filter.text.empty();
        list.push_back((v(text_owned(typed ? "No host matches" : "No hosts yet") | font(fs * 0.92f) | fg(t.text_secondary),
                          text_owned(typed ? "Enter makes it a new host" : "Press n to add one") | font(fs * 0.8f) | fg(muted))
                        | gap(4.0f) | align(Align::center) | width(pct(100)) | pad(28.0f, 0)).build());
    }
    Node list_view = scroll_view(c, base + ".list", st.list_scroll, v(std::move(list)) | width(pct(100)) | pad(0, 0, 8.0f, 0),
                                 [op](ScrollOp so) { return op([so](SshManagerState& s) { so(s.list_scroll); }); });

    const auto new_id = node_id(base + ".new");
    auto new_btn = h(node(icon_node(Icon::plus, 13.0f, c.hovered(new_id) ? ink : t.text_secondary)),
                     text_owned("New") | font(fs * 0.85f) | semibold | fg(c.hovered(new_id) ? ink : t.text_secondary))
                 | gap(5.0f) | align(Align::center) | height(28.0f) | pad(0, 10.0f) | radius(t.rounded(7.0f))
                 | bg(c.hovered(new_id) ? mix(paper, ink, 0.09f) : mix(paper, ink, 0.05f)) | border(1.0f, line)
                 | pointer_cursor | tooltip("New host (n)") | id_of_raw(new_id)
                 | on_click([op, hp] { return op([hp](SshManagerState& s) { o::begin_edit(s, *hp, -1); }); });
    auto side_head = h(v(text_owned("SSH hosts") | font(fs * 1.15f) | semibold | fg(ink),
                         text_owned(std::to_string(hosts.size()) + (hosts.size() == 1 ? " host" : " hosts")) | font(fs * 0.76f) | fg(muted))
                           | gap(1.0f) | grow(),
                       std::move(new_btn))
                   | align(Align::center) | width(pct(100));
    auto sidebar = v(v(std::move(side_head), std::move(search)) | gap(12.0f) | pad(16.0f, 14.0f, 6.0f, 14.0f) | width(pct(100)),
                     node(std::move(list_view)) | grow() | min_size(0, 0) | width(pct(100)) | pad(0, 6.0f),
                     h(mono(p.source, fs * 0.72f, faint) | ellipsis | grow() | min_size(0, 0)) | pad(10.0f, 16.0f) | width(pct(100))
                         | inner_shadow(0.0f, line, {0.0f, 1.0f}))
                 | width(d::kSideW) | height(pct(100)) | bg(mix(paper, ink, 0.02f)) | inner_shadow(0.0f, line, {-1.0f, 0.0f});

    // ── detail / editor ────────────────────────────────────────────────────
    std::vector<Node> pane;
    const auto section = [&](std::string title) {
        return (text_owned(std::move(title)) | font(fs * 0.72f) | semibold | tracking(1.0f) | fg(muted) | pad(10.0f, 0, 2.0f, 0)).build();
    };
    const auto card = [&](std::vector<Node> rows) {
        return (v(std::move(rows)) | width(pct(100)) | pad(4.0f, 16.0f) | radius(t.rounded(10.0f))
                | bg(mix(paper, ink, 0.025f)) | border(1.0f, mix(paper, ink, 0.075f))).build();
    };
    const auto kv = [&](std::string k, std::string v_, bool last_row, Color<Srgb> col) {
        return (h(text_owned(std::move(k)) | font(fs * 0.85f) | fg(muted) | width(110.0f),
                  mono(v_.empty() ? std::string{"\u2014"} : std::move(v_), fs * 0.88f, v_.empty() ? faint : col) | grow() | ellipsis | min_size(0, 0))
                | align(Align::center) | height(36.0f) | width(pct(100))
                | inner_shadow(0.0f, last_row ? line.fade(0.0f) : line.fade(0.7f), {0.0f, -1.0f})).build();
    };

    if (st.editing) {
        const bool is_new = st.edit_host < 0 || st.edit_host >= static_cast<int>(hosts.size());
        const SshHost* orig = is_new ? nullptr : &hosts[static_cast<std::size_t>(st.edit_host)];
        const bool owns = is_new || orig->editable;
        std::string live_err;
        const SshHost draft = d::draft_of(st, hosts, &live_err);
        pane.push_back((v(text_owned(is_new ? "New host" : "Edit " + orig->alias) | font(fs * 1.45f) | semibold | fg(ink),
                          text_owned(owns ? "Saved in " + p.store + ". Tab moves between fields; Enter saves."
                                          : "Connection details come from " + p.source + "; hand keeps your notes, tunnels and command.")
                              | font(fs * 0.82f) | fg(muted) | wrap_text | width(pct(100)))
                        | gap(4.0f) | width(pct(100))).build());
        const auto field_row = [&](SshField f, bool last_row) {
            const auto fid = node_id(base + ".f", static_cast<std::size_t>(f));
            const bool focused = st.focus == f;
            const bool locked = !owns && d::field_owned_by_config(f);
            Node ctl;
            if (f == SshField::env) {
                static constexpr std::array<const char*, 4> names{"none", "dev", "staging", "prod"};
                std::vector<Node> segs;
                for (int q = 0; q < 4; ++q) {
                    const bool on_ = st.env == q;
                    const auto tone = q == 0 ? t.text_secondary : env_tone(t, static_cast<SshHost::Env>(q));
                    const auto sid = part_id(base + ".env", static_cast<std::size_t>(q));
                    segs.push_back((h(text_owned(names[static_cast<std::size_t>(q)]) | font(fs * 0.85f) | (on_ ? semibold : weight(FontWeight::regular))
                                      | fg(on_ ? (q == 0 ? ink : tone) : c.hovered(sid) ? ink : t.text_secondary))
                                    | grow() | height(26.0f) | center | radius(t.rounded(5.0f))
                                    | bg(on_ ? (q == 0 ? mix(paper, ink, 0.12f) : tone.fade(0.16f)) : paper.fade(0.0f))
                                    | pointer_cursor | id_of_raw(sid)
                                    | on_click([op, q] { return op([q](SshManagerState& s) { s.env = q; s.focus = SshField::env; }); })).build());
                }
                ctl = (h_list(std::move(segs), 2.0f) | pad(2.0f) | radius(t.rounded(7.0f)) | width(pct(100))
                       | bg(mix(paper, ink, 0.05f)) | border(1.0f, focused ? t.accent.fade(0.7f) : line)).build();
            } else if (locked) {
                ctl = (h(mono(d::text_of(st, f).empty() ? std::string{"\u2014"} : d::text_of(st, f), fs * 0.9f, muted) | grow() | ellipsis | min_size(0, 0),
                         node(icon_node(Icon::pin, 12.0f, faint)) | tooltip("from " + p.source))
                       | align(Align::center) | height(32.0f) | pad(0, 10.0f) | width(pct(100))).build();
            } else {
                ctl = (node((text_field(t, st.fields[static_cast<std::size_t>(f)], focused, fid, d::field_hint(f), fs * 0.9f)
                             | monospace | width(pct(100)) | border(1.0f, focused ? t.accent : line)
                             | radius(t.rounded(7.0f)) | bg(mix(paper, ink, focused ? 0.03f : 0.015f))).build())
                       | width(pct(100))
                       | on_click([op, f] { return op([f](SshManagerState& s) { s.focus = f; }); })).build();
            }
            // under the tunnels: what each one does, or why it is wrong
            std::vector<Node> under;
            if (f == SshField::forwards)
                for (const auto& fw : d::split_forwards(d::text_of(st, f))) {
                    std::string why;
                    const auto pf = ssh_forward_parse(fw, &why);
                    const auto col = pf ? t.text_secondary : t.danger;
                    under.push_back((h(mono(pf ? std::string{pf->flag() + 1} : "!", fs * 0.7f, pf ? t.accent : t.danger) | bold | width(14.0f),
                                       mono(pf ? pf->describe() : fw + "  \u2014  " + why, fs * 0.78f, col) | ellipsis | min_size(0, 0))
                                     | gap(6.0f) | align(Align::center)).build());
                }
            return (h(text_owned(d::field_label(f)) | font(fs * 0.88f) | fg(focused ? ink : t.text_secondary) | width(110.0f) | pad(8.0f, 0, 0, 0),
                      v(node(std::move(ctl)), v(std::move(under)) | gap(3.0f) | pad(under.empty() ? 0.0f : 4.0f, 2.0f, 0, 2.0f)) | grow() | min_size(0, 0))
                    | align(Align::start) | width(pct(100)) | pad(7.0f, 0)
                    | inner_shadow(0.0f, last_row ? line.fade(0.0f) : line.fade(0.6f), {0.0f, -1.0f})).build();
        };
        pane.push_back(section("CONNECTION"));
        {
            std::vector<Node> rows;
            for (SshField f : {SshField::alias, SshField::hostname, SshField::user, SshField::port, SshField::jump, SshField::identity})
                rows.push_back(field_row(f, f == SshField::identity));
            pane.push_back(card(std::move(rows)));
        }
        pane.push_back(section("ORGANISE"));
        pane.push_back(card({field_row(SshField::env, false), field_row(SshField::tags, true)}));
        pane.push_back(section("SESSION"));
        pane.push_back(card({field_row(SshField::forwards, false), field_row(SshField::command, true)}));
        // what saving gives you, as a command line; or what is wrong
        const std::string err = !st.error.empty() ? st.error : live_err;
        if (!err.empty())
            pane.push_back((h(node(icon_node(Icon::circle_alert, 14.0f, t.danger)), text_owned(err) | font(fs * 0.85f) | fg(t.danger) | wrap_text | grow())
                            | gap(8.0f) | align(Align::center) | pad(8.0f, 12.0f) | radius(t.rounded(8.0f)) | bg(t.danger.fade(0.08f))
                            | border(1.0f, t.danger.fade(0.25f)) | width(pct(100))).build());
        else
            pane.push_back((h(mono("$", fs * 0.85f, t.accent), mono(d::command_line(draft), fs * 0.82f, t.text_secondary) | wrap_text | grow() | min_size(0, 0))
                            | gap(8.0f) | pad(10.0f, 12.0f) | radius(t.rounded(8.0f)) | bg(mix(paper, colors::black, 0.15f))
                            | border(1.0f, line) | width(pct(100))).build());
    } else if (has) {
        const auto& hst = hosts[static_cast<std::size_t>(st.sel)];
        const auto tone = env_tone(t, hst.env);
        // the host, big, with its badges
        std::vector<Node> title;
        title.push_back((mono(hst.alias, fs * 1.5f, ink) | semibold | ellipsis | min_size(0, 0)).build());
        if (hst.env != SshHost::Env::none)
            title.push_back((h(mono(env_name(hst.env), fs * 0.68f, tone) | bold | tracking(0.6f)) | height(20.0f) | pad(0, 7.0f) | center
                             | radius(4.0f) | bg(tone.fade(0.15f)) | border(1.0f, tone.fade(0.35f))).build());
        if (hst.connected)
            title.push_back((h(box() | size(7.0f, 7.0f) | radius(3.5f) | bg(t.success),
                               text_owned(std::to_string(hst.sessions) + " open") | font(fs * 0.78f) | fg(t.success))
                             | gap(5.0f) | align(Align::center)).build());
        const auto pin_id = node_id(base + ".pin");
        title.push_back(spacer().build());
        title.push_back((h(hst.pinned ? (text_owned("\u2605") | font(fs * 1.25f) | fg(t.warning)).build()
                                      : icon_node(Icon::star, 16.0f, c.hovered(pin_id) ? ink : faint, 1.5f))
                         | size(30.0f, 30.0f) | center | radius(t.rounded(7.0f)) | bg(c.hovered(pin_id) ? mix(paper, ink, 0.07f) : paper.fade(0.0f))
                         | pointer_cursor | tooltip(hst.pinned ? "Unpin (p)" : "Pin (p)") | id_of_raw(pin_id)
                         | on_click([op, i = st.sel] { return op([i](SshManagerState& s) { s.out.push_back({A::Kind::pin, i, {}, {}}); }); })).build());
        const std::string src = hst.editable ? "saved in " + p.store : hst.source == "known" ? "from known_hosts" : "from " + p.source;
        const std::string age = detail::host_age(hst.last_used, p.now);
        pane.push_back((v(h_list(std::move(title), 10.0f) | align(Align::center) | width(pct(100)),
                          mono(d::host_line(hst), fs * 0.9f, t.text_secondary),
                          text_owned(src + (hst.uses > 0 ? "  \u00B7  " + std::to_string(hst.uses) + (hst.uses == 1 ? " connection" : " connections")
                                                               + (age.empty() ? "" : ", last " + age + " ago") : "  \u00B7  never connected"))
                              | font(fs * 0.78f) | fg(muted))
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
        pane.push_back(section("CONNECTION"));
        pane.push_back(card({kv("Host", hst.hostname, false, ink), kv("User", hst.user, false, ink),
                             kv("Port", std::to_string(hst.port), false, ink), kv("Jump host", hst.jump, false, ink),
                             kv("Key file", hst.identity, true, ink)}));
        // tunnels
        pane.push_back(section("TUNNELS"));
        {
            std::vector<Node> rows;
            for (std::size_t q = 0; q < hst.forwards.size(); ++q) {
                const auto fw = ssh_forward_parse(hst.forwards[q]);
                if (!fw) continue;
                const auto kind_col = fw->kind == SshForward::Kind::local ? t.accent : fw->kind == SshForward::Kind::remote ? t.warning : t.success;
                rows.push_back((h(h(mono(std::string{fw->flag() + 1}, fs * 0.72f, kind_col) | bold) | size(20.0f, 20.0f) | center | radius(5.0f) | bg(kind_col.fade(0.14f)),
                                  mono(fw->describe(), fs * 0.86f, ink) | grow() | ellipsis | min_size(0, 0),
                                  mono(fw->flag() + std::string{" "} + fw->arg(), fs * 0.74f, faint))
                                | gap(10.0f) | align(Align::center) | height(36.0f) | width(pct(100))
                                | inner_shadow(0.0f, q + 1 == hst.forwards.size() ? line.fade(0.0f) : line.fade(0.7f), {0.0f, -1.0f})).build());
            }
            if (rows.empty())
                rows.push_back((h(text_owned("No tunnels. Edit to forward a port (L5432:db:5432) or open a SOCKS proxy (D1080).")
                                  | font(fs * 0.82f) | fg(muted) | wrap_text | grow())
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
        pane.push_back((h(mono("$", fs * 0.85f, t.accent), mono(d::command_line(hst), fs * 0.82f, t.text_secondary) | wrap_text | grow() | min_size(0, 0),
                          node(icon_node(Icon::copy, 14.0f, c.hovered(cid) ? ink : muted)))
                        | gap(8.0f) | align(Align::center) | pad(10.0f, 12.0f) | radius(t.rounded(8.0f))
                        | bg(c.hovered(cid) ? mix(paper, colors::black, 0.1f) : mix(paper, colors::black, 0.15f)) | border(1.0f, line)
                        | width(pct(100)) | pointer_cursor | tooltip("Copy (y)") | id_of_raw(cid)
                        | on_click([op, i = st.sel] { return op([i](SshManagerState& s) { s.out.push_back({A::Kind::copy_command, i, {}, {}}); }); })).build());
    } else {
        pane.push_back((v(node(icon_node(Icon::server, 30.0f, muted.fade(0.6f))),
                          text_owned("No host selected") | font(fs * 1.05f) | fg(t.text_secondary),
                          text_owned("Pick one on the left, or press n to add a host") | font(fs * 0.85f) | fg(muted))
                        | gap(8.0f) | align(Align::center) | width(pct(100)) | pad(90.0f, 0)).build());
    }
    Node detail = scroll_view(c, base + ".detail", st.detail_scroll, v(std::move(pane)) | gap(8.0f) | pad(22.0f, 56.0f, 26.0f, 26.0f) | width(pct(100)),
                              [op](ScrollOp so) { return op([so](SshManagerState& s) { so(s.detail_scroll); }); });

    // ── footer: what the keys do here ─────────────────────────────────────────
    std::vector<Node> keys;
    const auto hint = [&](std::string k, std::string what) {
        return (h(h(mono(std::move(k), fs * 0.72f, t.text_secondary)) | height(18.0f) | pad(0, 5.0f) | center | radius(4.0f)
                    | bg(mix(paper, ink, 0.06f)) | border(1.0f, line),
                  text_owned(std::move(what)) | font(fs * 0.78f) | fg(muted))
                | gap(6.0f) | align(Align::center)).build();
    };
    if (st.editing) {
        keys.push_back(hint("tab", "next field")); keys.push_back(hint("\u23CE", "save")); keys.push_back(hint("esc", "cancel"));
    } else if (st.filtering) {
        keys.push_back(hint("\u2191\u2193", "pick")); keys.push_back(hint("\u23CE", shown.empty() && !st.filter.text.empty() ? "new host" : "done"));
        keys.push_back(hint("esc", "clear"));
    } else {
        keys.push_back(hint("\u23CE", "connect")); keys.push_back(hint("n", "new")); keys.push_back(hint("e", "edit"));
        keys.push_back(hint("p", "pin")); keys.push_back(hint("/", "filter")); keys.push_back(hint("esc", "close"));
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
