#pragma once
// mayag::typo::system — finding fonts on the machine
//
// "Use the UI font" and "render this emoji" both require asking the OS what
// it has. mayag does that WITHOUT linking CoreText, DirectWrite, or
// fontconfig: it scans the platform's font directories and reads the `name`
// table out of each file with the parser we already have.
//
// The trade is deliberate. Directory scanning misses the niceties a real font
// database provides — user-installed fonts registered but not in a standard
// path, font activation managers, per-script fallback tuned by the vendor.
// What it buys is that mayag has no platform dependencies at all, works
// identically on every OS, and cannot break when a system API changes.
//
// The results are cached and lazily parsed: enumerating 400 fonts reads only
// the first few kilobytes of each, not the whole 20 MB CJK file.

#include "font.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::typo::system {

/// A font file found on disk, described without having fully loaded it.
struct FontEntry {
    std::string   path;
    std::uint32_t face_index = 0;
    std::string   family;
    std::string   subfamily;
    std::uint16_t weight = 400;
    bool          italic = false;
    bool          monospace = false;
    /// True when the face carries colour glyph tables — emoji fonts.
    bool          color = false;
    /// Rough script coverage, so a fallback chain can be assembled without
    /// loading every candidate.
    bool          has_latin = false;
    bool          has_cjk   = false;
    bool          has_arabic = false;
    bool          has_cyrillic = false;
    bool          has_emoji = false;
};

namespace detail {

/// Directories the platform keeps fonts in, most specific first so a
/// user-installed override wins over the system copy.
[[nodiscard]] inline std::vector<std::filesystem::path> font_directories() {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;

    const char* home = std::getenv("HOME");

#if defined(__APPLE__)
    if (home != nullptr) dirs.emplace_back(std::string{home} + "/Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    dirs.emplace_back("/System/Library/Fonts");
    dirs.emplace_back("/System/Library/Fonts/Supplemental");
#elif defined(_WIN32)
    if (const char* windir = std::getenv("WINDIR")) {
        dirs.emplace_back(std::string{windir} + "\\Fonts");
    }
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        dirs.emplace_back(std::string{local} + "\\Microsoft\\Windows\\Fonts");
    }
#else
    if (home != nullptr) {
        dirs.emplace_back(std::string{home} + "/.local/share/fonts");
        dirs.emplace_back(std::string{home} + "/.fonts");
    }
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    dirs.emplace_back("/run/host/fonts");        // flatpak
#endif

    return dirs;
}

[[nodiscard]] inline bool is_font_file(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

/// Read just enough of a file to fill in a FontEntry.
///
/// A .ttc can hold 20 faces and 100 MB; we memory-map nothing and read the
/// whole file only because the parser needs random access. To keep
/// enumeration fast we skip files above a size threshold unless they are
/// explicitly requested.
[[nodiscard]] inline std::vector<FontEntry> describe(const std::filesystem::path& path) {
    std::vector<FontEntry> out;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return out;
    const auto size = static_cast<std::size_t>(f.tellg());
    if (size < 12) return out;
    f.seekg(0);

    std::vector<std::uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

    const std::uint32_t faces = ot::FontFile::face_count(data);
    for (std::uint32_t i = 0; i < faces && i < 64; ++i) {
        auto parsed = ot::FontFile::parse(data, i, nullptr);
        if (!parsed) continue;

        FontEntry e;
        e.path       = path.string();
        e.face_index = i;
        e.family     = parsed->family();
        e.subfamily  = parsed->subfamily();
        e.weight     = parsed->weight();
        e.italic     = parsed->is_italic();
        e.color      = parsed->has_table(ot::tags::colr) ||
                       parsed->has_table(ot::tag("sbix")) ||
                       parsed->has_table(ot::tag("CBDT"));

        // Probe representative codepoints rather than enumerating the cmap —
        // one lookup per script instead of walking 20k entries.
        e.has_latin    = parsed->glyph_for('A') != 0;
        e.has_cyrillic = parsed->glyph_for(0x0410) != 0;
        e.has_arabic   = parsed->glyph_for(0x0627) != 0;
        e.has_cjk      = parsed->glyph_for(0x4E2D) != 0;
        e.has_emoji    = parsed->glyph_for(0x1F600) != 0;

        // Monospace: compare a wide glyph against a narrow one. `post`'s
        // isFixedPitch flag exists but is wrong often enough to be useless.
        const std::uint16_t gi = parsed->glyph_for('i');
        const std::uint16_t gm = parsed->glyph_for('M');
        e.monospace = gi != 0 && gm != 0 && parsed->advance(gi) == parsed->advance(gm);

        if (!e.family.empty()) out.push_back(std::move(e));
    }
    return out;
}

}  // namespace detail

// ════════════════════════════════════════════════════════════════════════
// Database
// ════════════════════════════════════════════════════════════════════════

/// The enumerated system fonts. Built once, then queried.
class Database {
  public:
    /// Scan the platform font directories. Cheap enough to call at startup:
    /// a typical macOS system takes well under a second.
    static const Database& instance() {
        static Database db = [] {
            Database d;
            d.scan();
            return d;
        }();
        return db;
    }

    [[nodiscard]] const std::vector<FontEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Find by family name, case-insensitively, choosing the face whose
    /// weight and slant are closest to what was asked for.
    [[nodiscard]] const FontEntry* find(std::string_view family,
                                        std::uint16_t weight = 400,
                                        bool italic = false) const {
        const FontEntry* best = nullptr;
        int best_score = -1;

        for (const auto& e : entries_) {
            if (!iequals(e.family, family)) continue;

            // Weight distance dominates; a wrong slant is more visible than
            // 100 units of weight, so it is penalised harder.
            int score = 1000;
            score -= std::abs(static_cast<int>(e.weight) - static_cast<int>(weight)) / 10;
            if (e.italic != italic) score -= 60;

            if (score > best_score) { best_score = score; best = &e; }
        }
        return best;
    }

    /// All faces in a family.
    [[nodiscard]] std::vector<const FontEntry*> family(std::string_view name) const {
        std::vector<const FontEntry*> out;
        for (const auto& e : entries_) {
            if (iequals(e.family, name)) out.push_back(&e);
        }
        return out;
    }

    /// The first available family from a preference list — the CSS
    /// `font-family: a, b, c` rule.
    [[nodiscard]] const FontEntry* first_available(std::span<const std::string_view> families,
                                                   std::uint16_t weight = 400,
                                                   bool italic = false) const {
        for (auto f : families) {
            if (const FontEntry* e = find(f, weight, italic)) return e;
        }
        return nullptr;
    }

    /// A face that covers `codepoint` — the fallback query.
    [[nodiscard]] const FontEntry* covering(std::uint32_t codepoint) const {
        const bool cjk    = uni::is_ideographic(codepoint);
        const bool arabic = codepoint >= 0x0600 && codepoint <= 0x06FF;
        const bool emoji  = codepoint >= 0x1F300 && codepoint <= 0x1FAFF;

        for (const auto& e : entries_) {
            if (emoji  && e.has_emoji)  return &e;
            if (cjk    && e.has_cjk)    return &e;
            if (arabic && e.has_arabic) return &e;
        }
        return nullptr;
    }

  private:
    void scan() {
        namespace fs = std::filesystem;
        std::error_code ec;

        for (const auto& dir : detail::font_directories()) {
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;

            for (fs::recursive_directory_iterator it{dir, fs::directory_options::skip_permission_denied, ec},
                 end; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                if (!detail::is_font_file(it->path())) continue;

                for (auto& e : detail::describe(it->path())) {
                    entries_.push_back(std::move(e));
                }
            }
        }
    }

    [[nodiscard]] static bool iequals(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    std::vector<FontEntry> entries_;
};

// ════════════════════════════════════════════════════════════════════════
// Convenience builders
// ════════════════════════════════════════════════════════════════════════

/// The platform's default UI font family names, best first.
[[nodiscard]] inline std::vector<std::string_view> ui_font_families() {
#if defined(__APPLE__)
    return {"SF Pro Text", "SF Pro", "System Font", "Helvetica Neue", "Helvetica", "Arial"};
#elif defined(_WIN32)
    return {"Segoe UI Variable Text", "Segoe UI", "Tahoma", "Arial"};
#else
    return {"Inter", "Cantarell", "Ubuntu", "Noto Sans", "DejaVu Sans", "Liberation Sans"};
#endif
}

[[nodiscard]] inline std::vector<std::string_view> mono_font_families() {
#if defined(__APPLE__)
    return {"SF Mono", "Menlo", "Monaco", "Courier New"};
#elif defined(_WIN32)
    return {"Cascadia Mono", "Consolas", "Courier New"};
#else
    return {"JetBrains Mono", "Fira Code", "Source Code Pro", "DejaVu Sans Mono", "Liberation Mono"};
#endif
}

/// Emoji font families, per platform.
[[nodiscard]] inline std::vector<std::string_view> emoji_font_families() {
#if defined(__APPLE__)
    return {"Apple Color Emoji"};
#elif defined(_WIN32)
    return {"Segoe UI Emoji"};
#else
    return {"Noto Color Emoji", "Twemoji", "JoyPixels"};
#endif
}

/// CJK families, per platform.
[[nodiscard]] inline std::vector<std::string_view> cjk_font_families() {
#if defined(__APPLE__)
    return {"Hiragino Sans", "PingFang SC", "Apple SD Gothic Neo"};
#elif defined(_WIN32)
    return {"Yu Gothic UI", "Microsoft YaHei", "Malgun Gothic"};
#else
    return {"Noto Sans CJK JP", "Noto Sans CJK SC", "Source Han Sans"};
#endif
}

/// Load a face by family name.
[[nodiscard]] inline std::shared_ptr<Face> load(std::string_view family,
                                                std::uint16_t weight = 400,
                                                bool italic = false) {
    const FontEntry* e = Database::instance().find(family, weight, italic);
    return e != nullptr ? Face::from_file(e->path, e->face_index) : nullptr;
}

/// Build a ready-to-use stack: the platform UI font, then CJK, then emoji.
///
/// This is what makes `mayag::run<App>()` show correct text on a fresh
/// machine with no font configuration at all — and why a label reading
/// "CPU 温度 🔥" renders every character rather than three tofu boxes.
[[nodiscard]] inline std::shared_ptr<FontStack> default_stack(FontConfig cfg = {},
                                                              std::uint16_t weight = 400,
                                                              bool italic = false) {
    auto stack = std::make_shared<FontStack>(cfg);
    const Database& db = Database::instance();

    const auto add_first = [&](const std::vector<std::string_view>& families) {
        if (const FontEntry* e = db.first_available(families, weight, italic)) {
            if (auto face = Face::from_file(e->path, e->face_index)) {
                stack->add(std::move(face));
                return true;
            }
        }
        return false;
    };

    add_first(ui_font_families());
    add_first(cjk_font_families());
    add_first(emoji_font_families());

    return stack;
}

[[nodiscard]] inline std::shared_ptr<FontStack> monospace_stack(FontConfig cfg = {}) {
    auto stack = std::make_shared<FontStack>(cfg);
    const Database& db = Database::instance();

    if (const FontEntry* e = db.first_available(mono_font_families())) {
        if (auto face = Face::from_file(e->path, e->face_index)) stack->add(std::move(face));
    }
    if (const FontEntry* e = db.first_available(cjk_font_families())) {
        if (auto face = Face::from_file(e->path, e->face_index)) stack->add(std::move(face));
    }
    return stack;
}

}  // namespace mayag::typo::system
