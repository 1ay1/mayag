#pragma once
// font_fingerprint.hpp — is the installed font the one test data came from?
//
// Shaping expectations (tests/data/shaping) and CoreText measurements pinned
// in test_font.cpp were captured from specific font FILES on one macOS
// release. Another release ships different builds at the same paths (SF on
// macOS 15 numbers its glyphs one lower), and comparing against those is
// testing Apple's font, not mayag. tests/data/fonts.txt records size + FNV-1a
// of the first and last 64 KB of every such file (tools/font_fingerprints.py
// writes it, this reads it); a case runs only when its file matches.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace mayag::test {

[[nodiscard]] inline bool fingerprint_of(const std::string& path, std::uint64_t& size, std::uint64_t& hash) {
    std::ifstream f{path, std::ios::binary};
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::uint64_t>(f.tellg());
    std::string data(65536, '\0');
    f.seekg(0);
    f.read(data.data(), 65536);
    data.resize(static_cast<std::size_t>(f.gcount()));
    f.clear();
    f.seekg(static_cast<std::streamoff>(n > 131072 ? n - 65536 : (n < 65536 ? n : 65536)));
    std::string tail(65536, '\0');
    f.read(tail.data(), 65536);
    tail.resize(static_cast<std::size_t>(f.gcount()));
    data += tail;
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : data) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    size = n;
    hash = h;
    return true;
}

/// Fonts whose installed file matches the manifest. Paths not listed in the
/// manifest are treated as matching (the data does not depend on them).
class FontManifest {
  public:
    explicit FontManifest(const std::string& manifest_path) {
        std::ifstream in{manifest_path};
        for (std::string l; std::getline(in, l);) {
            if (l.empty() || l[0] == '#') continue;
            const auto b = l.rfind(' ');
            const auto a = b == std::string::npos ? b : l.rfind(' ', b - 1);
            if (a == std::string::npos) continue;
            Entry e;
            e.size = std::stoull(l.substr(a + 1, b - a - 1));
            e.hash = std::stoull(l.substr(b + 1), nullptr, 16);
            want_[l.substr(0, a)] = e;
        }
    }
    /// True when `path` is the very file the data was captured from (or is
    /// not pinned). False when it is missing or a different build.
    [[nodiscard]] bool matches(const std::string& path) const {
        const auto it = want_.find(path);
        if (it == want_.end()) return true;
        auto& memo = seen_[path];
        if (memo == 0) {
            std::uint64_t n = 0, h = 0;
            memo = fingerprint_of(path, n, h) && n == it->second.size && h == it->second.hash ? 1 : 2;
        }
        return memo == 1;
    }
    [[nodiscard]] bool empty() const noexcept { return want_.empty(); }

  private:
    struct Entry { std::uint64_t size = 0, hash = 0; };
    std::map<std::string, Entry> want_;
    mutable std::map<std::string, int> seen_;
};

}  // namespace mayag::test
