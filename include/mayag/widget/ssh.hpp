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
[[nodiscard]] inline std::vector<int> rank_hosts(const std::vector<SshHost>& hosts, std::string_view q) {
    std::vector<std::pair<int, int>> hits;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        const auto& h_ = hosts[i];
        std::string hay = h_.alias + " " + h_.user + "@" + h_.hostname;
        for (const auto& tg : h_.tags) hay += " #" + tg;
        const auto m = fuzzy_match(q, hay);
        if (m.ok) hits.emplace_back(static_cast<int>(i), m.score + (h_.connected ? 3 : 0));
    }
    if (!q.empty()) std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::vector<int> out;
    for (auto& [i, s] : hits) out.push_back(i);
    return out;
}

/// The host picker: a modal list from ~/.ssh/config with fuzzy filter.
/// Interaction: part_id(id, k) connect ranked item k; enter = new tab,
/// cmd-enter = split (app keymap); Left on part_id(id, part_menu) dismiss.
template <ControlContext C>
inline void ssh_hosts(const C& c, std::string_view id, const TextEditState& query, const std::vector<SshHost>& hosts,
                      const std::vector<int>& ranked, int sel) {
    const Theme& t = c.theme;
    std::vector<Node> rows;
    const int n = static_cast<int>(ranked.size());
    const int s = n ? std::clamp(sel, 0, n - 1) : -1;
    const int top = std::clamp(s - 8, 0, std::max(0, n - 10));
    for (int k = top; k < std::min(n, top + 10); ++k) {
        const SshHost& h_ = hosts[static_cast<std::size_t>(ranked[static_cast<std::size_t>(k)])];
        const auto pid = part_id(id, static_cast<std::size_t>(k));
        const bool on = k == s || c.hovered(pid);
        const Color<Srgb> tone = env_tone(t, h_.env);
        std::string addr = (h_.user.empty() ? "" : h_.user + "@") + h_.hostname + (h_.port != 22 ? ":" + std::to_string(h_.port) : "");
        if (!h_.jump.empty()) addr += "  via " + h_.jump;
        std::vector<Node> tags;
        for (const auto& tg : h_.tags) tags.push_back((text_owned("#" + tg) | font(t.font_size * 0.75f) | fg(var::text_disabled)).build());
        rows.push_back((h(box() | size(3, 30) | bg(tone) | radius(t.rounded(1)),
                          v(h(text_owned(h_.alias) | font(t.font_size) | semibold | fg(var::text_primary),
                              text_owned(env_name(h_.env)) | font(t.font_size * 0.68f) | bold | tracking(0.6f) | fg(tone),
                              h(h_list(std::move(tags), 6.0f)))
                                | gap(t.spaced(8)) | align(Align::center),
                            text_owned(addr) | font(t.font_size * 0.82f) | fg(var::text_secondary) | ellipsis)
                              | gap(t.spaced(2)) | grow() | min_size(0, 0),
                          v(text_owned(h_.connected ? "\u25CF connected" + (h_.sessions > 1 ? " \u00D7" + std::to_string(h_.sessions) : std::string{}) : h_.last)
                                | font(t.font_size * 0.78f) | fg(h_.connected ? t.success : t.text_disabled),
                            text_owned(h_.identity) | font(t.font_size * 0.72f) | fg(var::text_disabled))
                              | gap(t.spaced(2)) | align(Align::end))
                        | gap(t.spaced(10)) | align(Align::center) | pad(t.spaced(6), t.spaced(10)) | width(pct(100)) | radius(t.radius_small)
                        | bg(on ? t.tint(t.accent, 0.18f) : t.overlay.fade(0.0f)) | pointer_cursor
                        | role(a11y::Role::menu_item) | label(h_.alias) | id_of(id, static_cast<std::size_t>(k))).build());
    }
    if (rows.empty()) rows.push_back((h(text_owned("No host matches \u201C" + query.text + "\u201D \u2014 \u23CE to ssh " + query.text)
                                        | font(t.font_size) | fg(var::text_secondary)) | pad(t.spaced(14), t.spaced(12))).build());
    Vec2 sz{1200.0f, 800.0f};
    if constexpr (requires { c.c.size; }) sz = c.c.size;
    else if constexpr (requires { c.size; }) sz = c.size;
    c.overlay(Overlay{
        .content = (v(h(text<"ssh"> | font(t.font_size) | bold | fg(var::accent),
                        node((text_field(t, query, true, part_id(id, part_inc), "host, user@host, or #tag", t.font_size * 1.05f)
                              | grow() | border(0, t.border) | bg(t.overlay.fade(0.0f))).build()))
                        | gap(t.spaced(6)) | align(Align::center) | pad(t.spaced(6), t.spaced(12)) | width(pct(100)),
                      box() | height(1) | width(pct(100)) | bg(var::border),
                      v(v_list(std::move(rows), 2.0f)) | pad(t.spaced(6)) | width(pct(100)),
                      h(node(key_hint(t, "\u23CE", "new tab").build()), node(key_hint(t, "\u2318\u23CE", "split").build()),
                        node(key_hint(t, "\u2318E", "edit config").build()), spacer(),
                        text_owned(std::to_string(n) + " hosts \u00B7 ~/.ssh/config") | font(t.font_size * 0.78f) | fg(var::text_disabled))
                        | gap(t.spaced(12)) | align(Align::center) | pad(t.spaced(6), t.spaced(14)) | width(pct(100)) | bg(var::surface))
                    | width(640) | bg(var::overlay) | border(t.stroke(1), t.border_strong) | radius(t.radius_medium) | clip | elevation(t.lift(24))
                    | role(a11y::Role::dialog) | label("ssh hosts")).build(),
        .id = part_id(id, part_menu),
        .placement = Placement::cursor,
        .dismiss = Dismiss::on_click_outside,
        .layer = 10,
        .at = Vec2{std::max(0.0f, (sz.x - 640.0f) * 0.5f), std::round(sz.y * 0.12f)},
    });
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
