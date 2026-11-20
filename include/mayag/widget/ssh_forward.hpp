#pragma once
// mayag::widget ssh_forward — what an ssh port forward means, from how it is typed
//
//   ssh_forward_parse("L5432:db:5432")   local  5432 -> db:5432       -L 5432:db:5432
//   ssh_forward_parse("5432")            local  5432 -> localhost:5432
//   ssh_forward_parse("R8080:localhost:80")  remote 8080 -> localhost:80
//   ssh_forward_parse("D1080")           dynamic (SOCKS) on 1080       -D 1080
//   ssh_forward_parse("-L 127.0.0.1:5432:db:5432")   with a bind address
//
// One parser for both ends: the host manager shows what a forward does as
// you type it, and the app builds ssh's argv from the same result. No
// dependencies beyond the standard library.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::dsl {

struct SshForward {
    enum class Kind : unsigned char { local, remote, dynamic };
    Kind        kind = Kind::local;
    std::string bind;       ///< listen address ("" = ssh's default)
    int         listen = 0;
    std::string host;       ///< target host (local / remote)
    int         port = 0;   ///< target port
    /// ssh's flag: "-L", "-R", "-D".
    [[nodiscard]] const char* flag() const noexcept { return kind == Kind::local ? "-L" : kind == Kind::remote ? "-R" : "-D"; }
    /// ssh's argument for the flag: "5432:db:5432", "127.0.0.1:1080".
    [[nodiscard]] std::string arg() const {
        std::string b = bind.empty() ? "" : bind + ":";
        if (kind == Kind::dynamic) return b + std::to_string(listen);
        return b + std::to_string(listen) + ":" + host + ":" + std::to_string(port);
    }
    /// The canonical spelling, as stored: "L5432:db:5432", "D1080".
    [[nodiscard]] std::string spec() const { return std::string{flag() + 1} + arg(); }
    /// For a person: "localhost:5432 -> db:5432", "SOCKS on :1080".
    [[nodiscard]] std::string describe() const {
        const std::string here = (bind.empty() ? std::string{"localhost"} : bind) + ":" + std::to_string(listen);
        if (kind == Kind::dynamic) return "SOCKS proxy on " + here;
        if (kind == Kind::local) return here + "  \u2192  " + host + ":" + std::to_string(port);
        return "remote :" + std::to_string(listen) + "  \u2192  " + host + ":" + std::to_string(port);
    }
};

namespace ssh_forward_detail {
[[nodiscard]] inline std::optional<int> port_of(std::string_view p) {
    if (p.empty() || p.size() > 5) return std::nullopt;
    for (char ch : p) if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
    const int n = std::atoi(std::string{p}.c_str());
    if (n <= 0 || n > 65535) return std::nullopt;
    return n;
}
}  // namespace ssh_forward_detail

/// Parse a forward as a person types it. nullopt (and why, in `err`) when it
/// is not one ssh would accept -- including anything with spaces or shell
/// characters, since it ends up on ssh's command line.
[[nodiscard]] inline std::optional<SshForward> ssh_forward_parse(std::string_view in, std::string* err = nullptr) {
    using ssh_forward_detail::port_of;
    const auto fail = [&](const char* why) -> std::optional<SshForward> {
        if (err != nullptr) *err = why;
        return std::nullopt;
    };
    std::string s;
    for (char ch : in) s += ch;
    const auto trim = [](std::string& x) {
        while (!x.empty() && std::isspace(static_cast<unsigned char>(x.front()))) x.erase(x.begin());
        while (!x.empty() && std::isspace(static_cast<unsigned char>(x.back()))) x.pop_back();
    };
    trim(s);
    if (s.size() > 1 && s[0] == '-') s.erase(0, 1);
    SshForward f;
    if (!s.empty()) {
        const char k = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        if (k == 'L' || k == 'R' || k == 'D') {
            f.kind = k == 'L' ? SshForward::Kind::local : k == 'R' ? SshForward::Kind::remote : SshForward::Kind::dynamic;
            s.erase(0, 1);
            trim(s);
        }
    }
    if (s.empty()) return fail("a port to listen on");
    if (s.find_first_of(" \t'\"`$;|&<>\\") != std::string::npos) return fail("no spaces or shell characters");
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char ch : s) {
        if (ch == '[') ++depth;
        if (ch == ']') --depth;
        if (ch == ':' && depth == 0) { parts.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    parts.push_back(cur);
    if (f.kind == SshForward::Kind::dynamic) {
        if (parts.size() > 2) return fail("D and a port: D1080");
        const auto p = port_of(parts.back());
        if (!p) return fail("D and a port 1-65535: D1080");
        f.listen = *p;
        if (parts.size() == 2) f.bind = parts[0];
        return f;
    }
    if (parts.size() == 1) {   // a lone port: the same port on the far side's localhost
        const auto p = port_of(parts[0]);
        if (!p) return fail("a port, or port:host:port");
        f.listen = *p;
        f.host = "localhost";
        f.port = *p;
        return f;
    }
    if (parts.size() < 3 || parts.size() > 4) return fail("port:host:port");
    const std::size_t o = parts.size() - 3;
    const auto lp = port_of(parts[o]), tp = port_of(parts[o + 2]);
    if (!lp || !tp) return fail("ports are 1-65535");
    if (parts[o + 1].empty()) return fail("a target host: port:host:port");
    if (o == 1) f.bind = parts[0];
    f.listen = *lp;
    f.host = parts[o + 1];
    f.port = *tp;
    return f;
}

}  // namespace mayag::dsl
