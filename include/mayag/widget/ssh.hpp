#pragma once
// mayag::widget ssh — the pieces a terminal needs to be a good SSH client
//
//   host_badge(c, host)                      env-coloured chip (prod is red)
//   ssh_hosts(c, "hosts", query, hosts, ranked, sel)   the host picker
//   connection_bar(c, "conn", conn, time)    connecting / reconnecting / lost
//   host_key_dialog(c, "hostkey", key)       verify or reject a host key
//   port_forwards(c, "fwd", forwards)        -L / -R / -D forwards
//
// UI only: the app runs ssh (or libssh) and feeds these the state.

#include "control.hpp"
#include "agent_state.hpp"     // spinner_frame
#include "fuzzy.hpp"
#include "key_hint.hpp"
#include "text_field.hpp"
#include "../scene/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mayag::dsl {

// ── hosts ───────────────────────────────────────────────────────────────

struct SshHost {
    enum class Env : std::uint8_t { none, dev, staging, prod };
    std::string alias;         ///< the `Host` name in ~/.ssh/config
    std::string user, hostname;
    int         port = 22;
    std::string jump;          ///< ProxyJump
    std::string identity;      ///< key file
    Env         env = Env::none;
    std::string last;          ///< "2h ago"
    bool        connected = false;
    int         sessions = 0;  ///< open tabs/panes on it
    std::vector<std::string> tags;
    bool        pinned = false;
    int         uses = 0;          ///< connections made (frecency)
    std::int64_t last_used = 0;    ///< unix seconds of the last one; 0 = never
    std::string source;            ///< "" ssh config, "known_hosts", "history" (typed once, remembered)
};

[[nodiscard]] inline Color<Srgb> env_tone(const Theme& t, SshHost::Env e) {
    switch (e) {
        case SshHost::Env::prod: return t.danger;
        case SshHost::Env::staging: return t.warning;
        case SshHost::Env::dev: return t.success;
        default: return t.text_secondary;
    }
}
[[nodiscard]] inline const char* env_name(SshHost::Env e) {
    switch (e) {
        case SshHost::Env::prod: return "PROD";
        case SshHost::Env::staging: return "STAGING";
        case SshHost::Env::dev: return "DEV";
        default: return "";
    }
}

/// `user@host` chip in the environment's colour. Shown on tabs and panes so
/// you always know which machine a keystroke goes to.
template <ControlContext C>
[[nodiscard]] inline Node host_badge(const C& c, const SshHost& h_, bool compact = false) {
    const Theme& t = c.theme;
    const Color<Srgb> tone = env_tone(t, h_.env);
    return (h(text<"\u21C4"> | font(t.font_size * 0.78f) | fg(tone),
              text_owned(compact ? h_.alias : (h_.user.empty() ? h_.alias : h_.user + "@" + h_.alias)) | font(t.font_size * 0.8f)
                  | semibold | fg(var::text_primary) | ellipsis,
              text_owned(compact ? "" : env_name(h_.env)) | font(t.font_size * 0.68f) | bold | tracking(0.6f) | fg(tone))
            | gap(t.spaced(4)) | align(Align::center) | pad(t.spaced(1), t.spaced(6)) | radius(t.rounded(4))
            | bg(t.tint(tone, h_.env == SshHost::Env::prod ? 0.22f : 0.12f)) | border(t.stroke(1), t.tint(tone, 0.4f))).build();
}

/// Fuzzy rank hosts on alias, hostname, user and tags.
///
/// With a query: match quality first, then frecency (how often and how
/// recently you connected), pinned and connected hosts nudged up. Without
/// one: pinned, then connected, then by frecency, then file order -- the
/// list you see before typing is the list you actually use.
[[nodiscard]] inline int host_frecency(const SshHost& h_, std::int64_t now) {
    if (h_.uses <= 0 || h_.last_used <= 0) return 0;
    const double age_h = static_cast<double>(std::max<std::int64_t>(now - h_.last_used, 0)) / 3600.0;
    const double decay = age_h < 4 ? 1.0 : age_h < 24 ? 0.8 : age_h < 24 * 7 ? 0.5 : age_h < 24 * 30 ? 0.25 : 0.1;
    return static_cast<int>(std::lround(std::log2(1.0 + h_.uses) * 10.0 * decay));
}

[[nodiscard]] inline std::vector<int> rank_hosts(const std::vector<SshHost>& hosts, std::string_view q, std::int64_t now = 0) {
    std::vector<std::pair<int, int>> hits;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        const auto& h_ = hosts[i];
        std::string hay = h_.alias + " " + h_.user + "@" + h_.hostname;
        for (const auto& tg : h_.tags) hay += " #" + tg;
        const auto m = fuzzy_match(q, hay);
        if (!m.ok) continue;
        const int bias = (h_.pinned ? 6 : 0) + (h_.connected ? 3 : 0) + host_frecency(h_, now) / 4;
        const int rest = (h_.pinned ? 4000 : 0) + (h_.connected ? 2000 : 0) + host_frecency(h_, now) * 10
                       + (h_.source.empty() ? 5 : 0);
        hits.emplace_back(static_cast<int>(i), q.empty() ? rest : m.score + bias);
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::vector<int> out;
    for (auto& [i, s] : hits) out.push_back(i);
    return out;
}

/// What the picker shows around the list.
struct SshHostsProps {
    std::int64_t now = 0;                  ///< unix seconds: "3h" ages and frecency
    std::string  source = "~/.ssh/config"; ///< named in the footer
    bool         sections = true;          ///< "pinned" / "recent" / "all hosts" headers before typing
    std::string  action = "new tab";       ///< what enter does
    float        width = 680.0f;
    int          max_rows = 9;
};

namespace detail {
[[nodiscard]] inline std::string host_age(std::int64_t then, std::int64_t now) {
    if (then <= 0 || now <= 0) return {};
    const std::int64_t d = std::max<std::int64_t>(now - then, 0);
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + "m";
    if (d < 86400) return std::to_string(d / 3600) + "h";
    if (d < 86400 * 14) return std::to_string(d / 86400) + "d";
    return std::to_string(d / (86400 * 7)) + "w";
}
/// The address, as ssh would reach it: user@host:port via jump.
[[nodiscard]] inline std::string host_addr(const SshHost& h_) {
    std::string a = (h_.user.empty() ? "" : h_.user + "@") + h_.hostname + (h_.port != 22 ? ":" + std::to_string(h_.port) : "");
    if (!h_.jump.empty()) a += "  \u2192 via " + h_.jump;
    return a;
}
}  // namespace detail

/// The host picker: a modal list over ~/.ssh/config, known_hosts and the
/// hosts you typed once, fuzzy-filtered, frecency-ordered.
///
///   ┌──────────────────────────────────────────────────────────────────┐
///   │ ssh ▏prod                                               12 hosts │
///   ├──────────────────────────────────────────────────────────────────┤
///   │ PINNED                                                           │
///   │▌★ prod-db         PROD  deploy@10.0.3.4  → via bastion  ● 2  3h  │
///   │ RECENT                                                           │
///   │  gpu-box                 ayush@gpu.lan:2222                  2d  │
///   ├──────────────────────────────────────────────────────────────────┤
///   │ ⏎ new tab  ⌃S pin  ⌃Y copy  ⌃E config        ~/.ssh/config     │
///   └──────────────────────────────────────────────────────────────────┘
///
/// Rows are mono, the environment is a tag only when one is known, and the
/// accent marks the selection and nothing else. A query that matches no
/// host but reads as a destination offers to connect to it as typed.
///
/// Interaction: part_id(id, k) connect ranked item k; part_id(id,
/// part_inc) the typed destination; Left on part_id(id, part_menu) dismiss.
template <ControlContext C>
inline void ssh_hosts(const C& c, std::string_view id, const TextEditState& query, const std::vector<SshHost>& hosts,
                      const std::vector<int>& ranked, int sel, const SshHostsProps& p) {
    const Theme& t = c.theme;
    const float fs = t.font_size;
    const auto paper = t.overlay, ink = t.text_primary;
    const auto dim = t.text_secondary, faint = t.text_disabled;
    const auto mono = [&](std::string s, float size, Color<Srgb> col) {
        return text_owned(std::move(s)) | font(size) | monospace | fg(col);
    };
    const int n = static_cast<int>(ranked.size());
    const std::string typed = query.text;
    // a typed destination nothing matches: connect to it as written (the last row)
    const bool adhoc = !typed.empty() && typed.find_first_of(" \t'\"`$;|&<>") == std::string::npos;
    const int total = n + (adhoc && n == 0 ? 1 : 0);
    const int s = total ? std::clamp(sel, 0, total - 1) : -1;
    const int top = std::clamp(s - p.max_rows + 2, 0, std::max(0, n - p.max_rows));

    const auto header = [&](const char* label_) {
        return (h(mono(label_, fs * 0.68f, faint) | semibold | tracking(1.0f)) | pad(t.spaced(5), t.spaced(12), t.spaced(2), t.spaced(12))).build();
    };
    const auto group_of = [&](const SshHost& h_) {
        if (h_.pinned) return 0;
        if (h_.connected || h_.uses > 0) return 1;
        return 2;
    };
    const auto row_frame = [&](Node content, std::uint64_t pid, bool on) {
        return (h(box() | size(2, 18) | radius(1.0f) | bg(on ? t.accent : t.accent.fade(0.0f)), node(std::move(content)))
                | gap(t.spaced(8)) | align(Align::center) | pad(0, t.spaced(12), 0, t.spaced(4)) | height(34) | width(pct(100))
                | radius(t.rounded(4)) | bg(on ? t.tint(t.accent, 0.12f) : c.hovered(pid) ? mix(paper, ink, 0.04f) : paper.fade(0.0f))
                | pointer_cursor | role(a11y::Role::menu_item) | selected(on) | Id{pid}).build();
    };

    std::vector<Node> rows;
    int last_group = -1;
    // the alias column fits the longest alias on screen (bounded), measured in
    // monospace cells; the env column fits STAGING when any row has an env
    std::size_t longest = 6;
    bool any_env = false;
    for (int k = top; k < std::min(n, top + p.max_rows); ++k) {
        const SshHost& h_ = hosts[static_cast<std::size_t>(ranked[static_cast<std::size_t>(k)])];
        longest = std::max(longest, h_.alias.size());
        any_env = any_env || h_.env != SshHost::Env::none;
    }
    const float cell = fs * 0.62f;   // a monospace advance at fs
    const float alias_w = std::round(cell * static_cast<float>(std::min<std::size_t>(longest, 22)) + 4.0f);
    const float env_w = any_env ? std::round(fs * 0.62f * 0.66f * 7.0f + 14.0f) : 0.0f;
    for (int k = top; k < std::min(n, top + p.max_rows); ++k) {
        const SshHost& h_ = hosts[static_cast<std::size_t>(ranked[static_cast<std::size_t>(k)])];
        const auto pid = part_id(id, static_cast<std::size_t>(k));
        const bool on = k == s;
        if (p.sections && typed.empty()) {
            const int g = group_of(h_);
            if (g != last_group) {
                rows.push_back(header(g == 0 ? "PINNED" : g == 1 ? "RECENT" : "ALL HOSTS"));
                last_group = g;
            }
        }
        const Color<Srgb> tone = env_tone(t, h_.env);
        const auto m = fuzzy_match(typed, h_.alias);
        // columns, so addresses line up down the list:
        //   ★ | alias (fixed) | ENV (fixed) | address (grows) | #tags | ● n | age
        std::vector<Node> line;
        line.push_back((mono(h_.pinned ? "\u2605" : " ", fs * 0.8f, h_.pinned ? t.warning : faint) | width(fs)).build());
        line.push_back((h(node(fuzzy_text(h_.alias, m.ok ? m.positions : std::vector<std::uint16_t>{}, fs, ink, t.accent).build()) | monospace)
                        | width(alias_w) | clip).build());
        line.push_back((h(h_.env == SshHost::Env::none ? Node{}
                          : (h(mono(env_name(h_.env), fs * 0.62f, tone) | bold | tracking(0.8f))
                             | pad(1.0f, 5.0f) | radius(t.rounded(3)) | bg(t.tint(tone, h_.env == SshHost::Env::prod ? 0.20f : 0.12f))).build())
                        | width(env_w)).build());
        // the address only when it says more than the alias
        std::string addr = detail::host_addr(h_);
        if (addr == h_.alias) addr.clear();
        line.push_back((mono(addr, fs * 0.8f, dim) | ellipsis | grow() | min_size(0, 0)).build());
        std::string tags;
        for (std::size_t i = 0; i < std::min<std::size_t>(h_.tags.size(), 3); ++i) tags += (i ? " #" : "#") + h_.tags[i];
        if (!tags.empty()) line.push_back(mono(tags, fs * 0.74f, faint).build());
        if (h_.connected)
            line.push_back((h(mono("\u25CF", fs * 0.6f, t.success),
                              mono(h_.sessions > 1 ? std::to_string(h_.sessions) : "", fs * 0.74f, t.success))
                            | gap(3.0f) | align(Align::center)).build());
        const std::string age = !h_.last.empty() ? h_.last : detail::host_age(h_.last_used, p.now);
        line.push_back((mono(age.empty() && !h_.source.empty() ? h_.source : age, fs * 0.74f, faint) | min_size(28, 0)
                        | text_align(TextAlign::right)).build());
        rows.push_back(row_frame((h_list(std::move(line), t.spaced(8)) | align(Align::center) | grow() | min_size(0, 0)).build(),
                                 pid, on));
    }
    if (n == 0) {
        if (adhoc) {
            const auto pid = part_id(id, part_inc);
            rows.push_back(row_frame((h(mono("\u21B5", fs * 0.8f, t.accent) | width(fs),
                                        mono("ssh " + typed, fs, ink) | ellipsis | grow() | min_size(0, 0),
                                        mono("not in your hosts \u2014 connect as typed", fs * 0.74f, faint))
                                      | gap(t.spaced(8)) | align(Align::center) | grow()).build(),
                                     pid, s == 0));
        } else {
            rows.push_back((h(mono(hosts.empty() ? "No hosts yet \u2014 type user@host to connect" : "No hosts match", fs * 0.9f, dim))
                            | pad(t.spaced(14), t.spaced(14))).build());
        }
    }

    Vec2 sz{1200.0f, 800.0f};
    if constexpr (requires { c.c.size; }) sz = c.c.size;
    else if constexpr (requires { c.size; }) sz = c.size;
    const float w = std::min(p.width, sz.x - 32.0f);
    const std::string count = std::to_string(static_cast<int>(hosts.size())) + (hosts.size() == 1 ? " host" : " hosts");
    c.overlay(Overlay{
        .content = (v(h(mono("ssh", fs, t.accent) | bold,
                        node((text_field(t, query, true, part_id(id, part_dec), "host, user@host[:port], #tag", fs)
                              | monospace | grow() | border(0, t.border) | bg(paper.fade(0.0f))).build()),
                        mono(typed.empty() ? count : std::to_string(n) + " / " + count, fs * 0.74f, faint))
                        | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(8), t.spaced(14)) | width(pct(100)),
                      box() | height(1) | width(pct(100)) | bg(var::border),
                      v(v_list(std::move(rows), 1.0f)) | pad(t.spaced(4), t.spaced(6)) | width(pct(100)),
                      h(node(key_hint(t, "\u23CE", p.action, fs * 0.78f).build()),
                        node(key_hint(t, "\u2303S", "pin", fs * 0.78f).build()),
                        node(key_hint(t, "\u2303Y", "copy cmd", fs * 0.78f).build()),
                        node(key_hint(t, "\u2303E", "edit config", fs * 0.78f).build()),
                        spacer(),
                        mono(p.source, fs * 0.72f, faint) | ellipsis)
                        | gap(t.spaced(12)) | align(Align::center) | pad(t.spaced(6), t.spaced(14)) | width(pct(100))
                        | bg(mix(paper, ink, 0.025f)))
                    | width(w) | bg(paper) | border(t.stroke(1), t.border_strong) | radius(t.radius_medium) | clip
                    | elevation(t.lift(24)) | role(a11y::Role::dialog) | label("ssh hosts")).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::on_click_outside,
        .layer = 10,
        .at = Vec2{std::max(0.0f, (sz.x - w) * 0.5f), std::round(sz.y * 0.12f)},
    });
}

/// The original form: no clock, no sections.
template <ControlContext C>
inline void ssh_hosts(const C& c, std::string_view id, const TextEditState& query, const std::vector<SshHost>& hosts,
                      const std::vector<int>& ranked, int sel) {
    ssh_hosts(c, id, query, hosts, ranked, sel, SshHostsProps{.sections = false});
}

// ── connection state ────────────────────────────────────────────────────

struct SshConnection {
    enum class State : std::uint8_t { connecting, authenticating, connected, reconnecting, lost };
    State       state = State::connected;
    std::string host;          ///< alias
    std::string detail;        ///< "publickey ~/.ssh/id_ed25519", "Connection reset by peer"
    int         latency_ms = 0;
    int         retry_in = 0;  ///< seconds until the next reconnect attempt
    int         attempt = 0;
    bool        mosh = false;  ///< roaming session: keep typing while lost
};

/// A strip across the top of an SSH pane. Hidden when connected and
/// healthy; amber when slow; a spinner while connecting; a red bar with
/// Reconnect when lost (the pane keeps its scrollback underneath).
/// Interaction: part_id(id, 0) reconnect now, part_id(id, 1) close.
template <ControlContext C>
[[nodiscard]] inline Node connection_bar(const C& c, std::string_view id, const SshConnection& s, double time) {
    const Theme& t = c.theme;
    using S = SshConnection::State;
    if (s.state == S::connected && s.latency_ms < 300) return Node{};
    Color<Srgb> tone = t.info;
    std::string glyph = spinner_frame(time), words;
    switch (s.state) {
        case S::connecting: words = "Connecting to " + s.host; break;
        case S::authenticating: words = "Authenticating to " + s.host; break;
        case S::connected: words = "High latency to " + s.host + ": " + std::to_string(s.latency_ms) + " ms"; tone = t.warning; glyph = "\u25D4"; break;
        case S::reconnecting:
            words = "Connection to " + s.host + " lost \u2014 reconnecting" + (s.retry_in > 0 ? " in " + std::to_string(s.retry_in) + "s" : "\u2026")
                  + (s.attempt > 1 ? " (attempt " + std::to_string(s.attempt) + ")" : "");
            tone = t.warning; break;
        case S::lost: words = "Disconnected from " + s.host; tone = t.danger; glyph = "\u2716"; break;
    }
    const auto btn = [&](std::size_t k, const char* label_) {
        const auto pid = part_id(id, k);
        return (h(text_owned(label_) | font(t.font_size * 0.82f) | semibold | fg(c.hovered(pid) ? t.text_primary : t.text_secondary))
                | pad(t.spaced(2), t.spaced(9)) | radius(t.rounded(4)) | bg(c.hovered(pid) ? t.surface_raised : t.tint(tone, 0.12f)) | border(t.stroke(1), t.tint(tone, 0.35f))
                | pointer_cursor | id_of(id, k)).build();
    };
    const bool can_retry = s.state == S::lost || s.state == S::reconnecting;
    return (h(text_owned(glyph) | font(t.font_size * 0.9f) | bold | fg(tone) | width(14),
              text_owned(words) | font(t.font_size * 0.88f) | fg(var::text_primary),
              text_owned(s.detail) | font(t.font_size * 0.8f) | fg(var::text_secondary) | ellipsis | grow() | min_size(0, 0),
              text_owned(s.mosh && can_retry ? "mosh: keep typing, input is buffered" : "") | font(t.font_size * 0.78f) | fg(var::text_disabled),
              node(can_retry ? btn(0, "Reconnect") : Node{}),
              node(s.state == S::lost ? btn(1, "Close") : Node{}))
            | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(5), t.spaced(10)) | width(pct(100))
            | bg(t.tint(tone, 0.10f)) | border(t.stroke(1), t.tint(tone, 0.30f))
            | role(a11y::Role::progress) | label(words)).build();
}

// ── host keys ───────────────────────────────────────────────────────────

struct HostKeyPrompt {
    std::string host, address;
    std::string algorithm;     ///< "ED25519"
    std::string fingerprint;   ///< "SHA256:..."
    bool        changed = false;          ///< mismatch with known_hosts: possible MITM
    std::string known_fingerprint;        ///< the one on record, when changed
    int         known_line = 0;           ///< known_hosts line
};

/// Modal: trust-on-first-use, or the loud warning when a known key changed.
/// Interaction: part_id(id, 0) reject, 1 accept once, 2 accept and save
/// (replace, when changed).
template <ControlContext C>
inline void host_key_dialog(const C& c, std::string_view id, const HostKeyPrompt& k) {
    const Theme& t = c.theme;
    const Color<Srgb> tone = k.changed ? t.danger : t.info;
    const auto btn = [&](std::size_t part, const char* label_, bool primary, bool danger) {
        const auto pid = part_id(id, part);
        const bool hov = c.hovered(pid);
        const Color<Srgb> face = primary ? (danger ? t.danger : t.accent) : hov ? t.surface_raised : t.surface;
        return (h(text_owned(label_) | font(t.font_size * 0.92f) | semibold | fg(primary ? t.on_accent : t.text_primary))
                | pad(t.spaced(6), t.spaced(14)) | radius(t.radius_small) | bg(face) | border(t.stroke(1), primary ? face : t.border)
                | pointer_cursor | role(a11y::Role::button) | id_of(id, part)).build();
    };
    const auto fp_row = [&](const char* label_, const std::string& fp, Color<Srgb> col) {
        return (h(text_owned(label_) | font(t.font_size * 0.8f) | fg(var::text_disabled) | width(70),
                  text_owned(fp) | font(t.font_size * 0.88f) | fg(col) | no_ligatures | wrap_text | grow() | min_size(0, 0))
                | width(pct(100))).build();
    };
    c.overlay(Overlay{
        .content = (v(h(text_owned(k.changed ? "\u26A0" : "\U0001F511") | font(t.font_size * 1.2f) | fg(tone),
                        text_owned(k.changed ? "HOST KEY CHANGED for " + k.host : "First connection to " + k.host)
                            | font(t.font_size * 1.1f) | semibold | fg(k.changed ? t.danger : t.text_primary))
                        | gap(t.spaced(8)) | align(Align::center),
                      text_owned(k.changed
                                     ? "The key " + k.host + " presented does not match the one in known_hosts (line " +
                                           std::to_string(k.known_line) + "). Someone could be intercepting the connection, "
                                           "or the server was reinstalled. Verify the new fingerprint with its administrator."
                                     : "The authenticity of " + k.host + " (" + k.address + ") can't be established yet. "
                                       "Check that this fingerprint matches the server's before trusting it.")
                          | font(t.font_size * 0.92f) | fg(var::text_secondary) | wrap_text | line_height(1.45f),
                      v(node(fp_row(k.algorithm.c_str(), k.fingerprint, t.text_primary)),
                        node(k.changed ? fp_row("on record", k.known_fingerprint, t.text_disabled) : Node{}))
                          | gap(t.spaced(6)) | pad(t.spaced(10), t.spaced(12)) | width(pct(100)) | bg(var::background) | border(t.stroke(1), t.border) | radius(t.radius_small),
                      h(spacer(), node(btn(0, k.changed ? "Abort" : "Cancel", !k.changed ? false : true, false)),
                        node(btn(1, "Trust once", false, false)),
                        node(btn(2, k.changed ? "Replace key" : "Trust and save", !k.changed, k.changed)))
                          | gap(t.spaced(8)) | align(Align::center) | width(pct(100)))
                    | gap(t.spaced(12)) | pad(t.spaced(18), t.spaced(20)) | width(520)
                    | bg(var::overlay) | border(t.stroke(1), k.changed ? t.danger : t.border_strong) | radius(t.radius_medium) | elevation(t.lift(24))
                    | role(a11y::Role::dialog) | label("host key")).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::centered,
        .dismiss = Dismiss::modal,
        .layer = 30,
        .scrim = 0.5f,
    });
}

// ── port forwards ───────────────────────────────────────────────────────

struct PortForward {
    enum class Kind : std::uint8_t { local, remote, dynamic };
    Kind        kind = Kind::local;
    int         listen = 0;
    std::string target;        ///< "localhost:5432"
    bool        active = true;
    std::string error;
    int         connections = 0;
};

/// The forwards of one SSH session; local HTTP-looking ports get "open".
/// Interaction: part_id(id, i*4 + 0) toggle, +1 open in browser, +2 remove;
/// part_id(id, part_inc) add.
template <ControlContext C>
[[nodiscard]] inline Node port_forwards(const C& c, std::string_view id, const std::vector<PortForward>& fwd) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    for (std::size_t i = 0; i < fwd.size(); ++i) {
        const auto& f = fwd[i];
        const char* kind = f.kind == PortForward::Kind::local ? "L" : f.kind == PortForward::Kind::remote ? "R" : "D";
        const Color<Srgb> tone = !f.error.empty() ? t.danger : f.active ? t.success : t.text_disabled;
        const auto tid = part_id(id, i * 4), oid = part_id(id, i * 4 + 1), rid = part_id(id, i * 4 + 2);
        rows.push_back((h(h(text_owned(kind) | font(t.font_size * 0.75f) | bold | fg(var::on_accent)) | size(18, 18) | center | radius(t.rounded(4)) | bg(tone),
                          text_owned(std::to_string(f.listen)) | font(t.font_size * 0.92f) | semibold | fg(var::text_primary) | width(52),
                          text_owned(f.kind == PortForward::Kind::dynamic ? std::string{"SOCKS proxy"} : "\u2192 " + f.target)
                              | font(t.font_size * 0.88f) | fg(var::text_secondary) | ellipsis | grow() | min_size(0, 0),
                          text_owned(!f.error.empty() ? f.error : f.connections > 0 ? std::to_string(f.connections) + " conn" : "")
                              | font(t.font_size * 0.78f) | fg(f.error.empty() ? t.text_disabled : t.danger),
                          h(text_owned(f.active ? "on" : "off") | font(t.font_size * 0.75f) | fg(c.hovered(tid) ? t.text_primary : tone))
                              | pad(0, t.spaced(6)) | radius(t.rounded(8)) | border(t.stroke(1), tone) | pointer_cursor | id_of(id, i * 4),
                          node(f.kind == PortForward::Kind::local ? (h(text<"open \u2197"> | font(t.font_size * 0.78f) | fg(c.hovered(oid) ? t.text_primary : t.accent))
                                                                     | pointer_cursor | id_of(id, i * 4 + 1)).build() : Node{}),
                          h(text<"\u00D7"> | font(t.font_size * 0.9f) | fg(c.hovered(rid) ? t.danger : t.text_disabled)) | pointer_cursor | id_of(id, i * 4 + 2))
                        | gap(t.spaced(8)) | align(Align::center) | pad(t.spaced(4), t.spaced(8)) | width(pct(100)) | radius(t.rounded(4))
                        | bg(c.hovered(tid) || c.hovered(oid) || c.hovered(rid) ? t.surface_raised.fade(0.5f) : t.surface.fade(0.0f))).build());
    }
    const auto aid = part_id(id, part_inc);
    rows.push_back((h(text<"+ forward a port"> | font(t.font_size * 0.85f) | fg(c.hovered(aid) ? t.text_primary : t.text_disabled))
                    | pad(t.spaced(4), t.spaced(8)) | pointer_cursor | id_of(id, part_inc)).build());
    return v_list(std::move(rows), 1.0f).build();
}

}  // namespace mayag::dsl
