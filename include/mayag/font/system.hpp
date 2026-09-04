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
#include "last_resort.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mayag::typo::system {

// ════════════════════════════════════════════════════════════════════════
// Script coverage
//
// The fallback chain lives or dies on knowing WHICH scripts a face can draw,
// and the old model probed exactly five codepoints (Latin, Cyrillic, Arabic,
// CJK, emoji) — so Devanagari, Hebrew, Thai, Korean, Greek and a dozen more
// were invisible, and any text in them fell straight to tofu even when a
// perfectly good Noto face was installed. This is the fix: a face is probed
// against a REPRESENTATIVE codepoint for every script mayag knows how to fall
// back for, and the result is a bitset. One cmap lookup per script at scan
// time buys correct routing for every codepoint at run time.
// ════════════════════════════════════════════════════════════════════════

enum class Script : std::uint8_t {
    latin, greek, cyrillic, armenian, hebrew, arabic, syriac, thaana,
    devanagari, bengali, gurmukhi, gujarati, tamil, telugu, kannada,
    malayalam, sinhala, thai, lao, tibetan, myanmar, khmer, georgian,
    hangul, han, hiragana, katakana, ethiopic, cherokee, canadian,
    mongolian, symbols, emoji, math,
    count
};

inline constexpr std::size_t script_count = static_cast<std::size_t>(Script::count);
using ScriptSet = std::uint64_t;

[[nodiscard]] constexpr ScriptSet script_bit(Script s) noexcept {
    return ScriptSet{1} << static_cast<std::uint8_t>(s);
}

/// A representative, near-universally-present codepoint for each script — the
/// letter a font MUST have if it claims to support that script at all. These
/// are deliberately common characters, not rare ones, so the probe is a fair
/// test of coverage rather than of completeness.
struct ScriptProbe { Script script; std::uint32_t codepoint; };

inline constexpr ScriptProbe script_probes[] = {
    {Script::latin,       'A'},
    {Script::greek,       0x0391},   // Alpha
    {Script::cyrillic,    0x0410},   // A
    {Script::armenian,    0x0531},
    {Script::hebrew,      0x05D0},   // Alef
    {Script::arabic,      0x0627},   // Alef
    {Script::syriac,      0x0710},
    {Script::thaana,      0x0780},
    {Script::devanagari,  0x0915},   // Ka
    {Script::bengali,     0x0995},
    {Script::gurmukhi,    0x0A15},
    {Script::gujarati,    0x0A95},
    {Script::tamil,       0x0B95},
    {Script::telugu,      0x0C15},
    {Script::kannada,     0x0C95},
    {Script::malayalam,   0x0D15},
    {Script::sinhala,     0x0D9A},
    {Script::thai,        0x0E01},   // Ko Kai
    {Script::lao,         0x0E81},
    {Script::tibetan,     0x0F40},
    {Script::myanmar,     0x1000},
    {Script::georgian,    0x10D0},
    {Script::hangul,      0xAC00},   // Ga (syllable)
    {Script::han,         0x4E2D},   // Zhong
    {Script::hiragana,    0x3042},   // A
    {Script::katakana,    0x30A2},   // A
    {Script::ethiopic,    0x1200},
    {Script::cherokee,    0x13A0},
    {Script::canadian,    0x1401},
    {Script::mongolian,   0x1820},
    {Script::khmer,       0x1780},
    {Script::symbols,     0x2600},   // ☀ misc symbols
    {Script::emoji,       0x1F600},  // grinning face
    {Script::math,        0x2200},   // ∀ for all
};

/// The script a codepoint belongs to, for run-time fallback routing. Ranges
/// are the primary + supplement blocks of each script; anything unmatched is
/// treated as Latin (the Basic-Latin / punctuation / Latin-Extended common
/// case), which every text face covers.
[[nodiscard]] inline Script script_of(std::uint32_t cp) noexcept {
    auto in = [cp](std::uint32_t lo, std::uint32_t hi) { return cp >= lo && cp <= hi; };
    if (in(0x0370, 0x03FF) || in(0x1F00, 0x1FFF)) return Script::greek;
    if (in(0x0400, 0x052F) || in(0x2DE0, 0x2DFF) || in(0xA640, 0xA69F)) return Script::cyrillic;
    if (in(0x0530, 0x058F)) return Script::armenian;
    if (in(0x0590, 0x05FF) || in(0xFB1D, 0xFB4F)) return Script::hebrew;
    if (in(0x0600, 0x06FF) || in(0x0750, 0x077F) || in(0x08A0, 0x08FF) ||
        in(0xFB50, 0xFDFF) || in(0xFE70, 0xFEFF)) return Script::arabic;
    if (in(0x0700, 0x074F)) return Script::syriac;
    if (in(0x0780, 0x07BF)) return Script::thaana;
    if (in(0x0900, 0x097F) || in(0xA8E0, 0xA8FF)) return Script::devanagari;
    if (in(0x0980, 0x09FF)) return Script::bengali;
    if (in(0x0A00, 0x0A7F)) return Script::gurmukhi;
    if (in(0x0A80, 0x0AFF)) return Script::gujarati;
    if (in(0x0B80, 0x0BFF)) return Script::tamil;
    if (in(0x0C00, 0x0C7F)) return Script::telugu;
    if (in(0x0C80, 0x0CFF)) return Script::kannada;
    if (in(0x0D00, 0x0D7F)) return Script::malayalam;
    if (in(0x0D80, 0x0DFF)) return Script::sinhala;
    if (in(0x0E00, 0x0E7F)) return Script::thai;
    if (in(0x0E80, 0x0EFF)) return Script::lao;
    if (in(0x0F00, 0x0FFF)) return Script::tibetan;
    if (in(0x1000, 0x109F) || in(0xA9E0, 0xA9FF) || in(0xAA60, 0xAA7F)) return Script::myanmar;
    if (in(0x10A0, 0x10FF) || in(0x1C90, 0x1CBF)) return Script::georgian;
    if (in(0x1200, 0x139F) || in(0x2D80, 0x2DDF)) return Script::ethiopic;
    if (in(0x13A0, 0x13FF) || in(0xAB70, 0xABBF)) return Script::cherokee;
    if (in(0x1400, 0x167F) || in(0x18B0, 0x18FF)) return Script::canadian;
    if (in(0x1780, 0x17FF) || in(0x19E0, 0x19FF)) return Script::khmer;
    if (in(0x1800, 0x18AF)) return Script::mongolian;
    if (in(0xAC00, 0xD7A3) || in(0x1100, 0x11FF) || in(0x3130, 0x318F) ||
        in(0xA960, 0xA97F)) return Script::hangul;
    if (in(0x3040, 0x309F)) return Script::hiragana;
    if (in(0x30A0, 0x30FF) || in(0x31F0, 0x31FF)) return Script::katakana;
    if (in(0x3400, 0x4DBF) || in(0x4E00, 0x9FFF) || in(0xF900, 0xFAFF) ||
        in(0x20000, 0x2FA1F)) return Script::han;
    // Emoji: the pictographic blocks plus the dingbats / misc-symbols that are
    // emoji-presented. Checked before the generic symbol block.
    if (in(0x1F000, 0x1FAFF) || in(0x2600, 0x27BF) || in(0x2B00, 0x2BFF) ||
        in(0xFE00, 0xFE0F) || cp == 0x203C || cp == 0x2049) return Script::emoji;
    if (in(0x2200, 0x22FF) || in(0x2A00, 0x2AFF) || in(0x27C0, 0x27EF) ||
        in(0x1D400, 0x1D7FF)) return Script::math;
    if (in(0x2000, 0x2BFF) || in(0x3000, 0x303F)) return Script::symbols;
    return Script::latin;
}

[[nodiscard]] constexpr int popcount(ScriptSet s) noexcept {
    return std::popcount(s);
}

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
    /// True when the face has vector outlines this engine can rasterise. A
    /// colour/bitmap-only face is measurable but draws blank here.
    bool          outlines = true;
    /// Number of glyphs — a cheap proxy for "pan-Unicode" faces that should
    /// sort last as a general fallback rather than first.
    std::uint16_t glyphs = 0;
    /// Which scripts this face covers, one bit per Script. This is the whole
    /// point of the rewrite: a complete answer instead of five booleans.
    ScriptSet     scripts = 0;

    [[nodiscard]] bool covers(Script s) const noexcept {
        return (scripts & script_bit(s)) != 0;
    }
    [[nodiscard]] bool has_latin() const noexcept { return covers(Script::latin); }
    [[nodiscard]] bool has_cjk()   const noexcept { return covers(Script::han); }
    [[nodiscard]] bool has_emoji() const noexcept { return covers(Script::emoji); }
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
        e.color      = parsed->has_color();
        e.outlines   = parsed->has_outlines();
        e.glyphs     = parsed->num_glyphs();

        // Probe one representative codepoint per script rather than
        // enumerating the cmap: a handful of lookups instead of walking
        // 20k+ entries, and it yields the full ScriptSet the fallback chain
        // needs. A colour/bitmap face reports coverage for whatever its cmap
        // maps (emoji, symbols) even though it has no outlines.
        for (const auto& probe : script_probes) {
            if (parsed->glyph_for(probe.codepoint) != 0) {
                e.scripts |= script_bit(probe.script);
            }
        }
        // A colour font that maps the emoji probe IS an emoji font even if the
        // `color` heuristic missed it, and vice versa — keep the two in sync.
        if (e.color && parsed->glyph_for(0x1F600) != 0) {
            e.scripts |= script_bit(Script::emoji);
        }

        // Monospace: compare a wide glyph against a narrow one. `post`'s
        // isFixedPitch flag exists but is wrong often enough to be useless.
        const std::uint16_t gi = parsed->glyph_for('i');
        const std::uint16_t gm = parsed->glyph_for('M');
        e.monospace = gi != 0 && gm != 0 && parsed->advance(gi) == parsed->advance(gm);

        // A face that covers no known script and has no colour glyphs is a
        // subsetted or icon-only artefact — keeping it would only add noise to
        // the fallback search. Symbol/emoji faces are kept (they cover those
        // scripts); everything else must cover at least one.
        if (!e.family.empty() && (e.scripts != 0 || e.color)) {
            out.push_back(std::move(e));
        }
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
    ///
    /// Routes the codepoint to its script, then returns the best entry whose
    /// coverage bitset includes that script. "Best" prefers a face that
    /// actually has outlines and, among those, one that is not a giant
    /// pan-Unicode fallback — a dedicated Devanagari face renders Devanagari
    /// better than a 60k-glyph Noto Sans that merely happens to include it.
    /// Emoji is the deliberate exception: there the colour face is what the
    /// user wants.
    [[nodiscard]] const FontEntry* covering(std::uint32_t codepoint) const {
        const Script s = script_of(codepoint);
        const bool want_color = (s == Script::emoji);

        const FontEntry* best = nullptr;
        int best_score = -1;
        for (const auto& e : entries_) {
            if (!e.covers(s)) continue;

            int score = 0;
            if (want_color) {
                if (e.color) score += 1000;                 // colour emoji wins
            } else {
                if (e.outlines) score += 1000;              // must be drawable
                // Prefer a focused face: fewer scripts covered = more
                // specialised. A dedicated face beats a pan-Unicode one.
                score += 64 - static_cast<int>(popcount(e.scripts));
                // Among equally specialised faces, prefer one near regular
                // weight and upright — a fallback should look like body text,
                // not the Black or Italic cut that happened to sort first.
                score += 20 - std::abs(static_cast<int>(e.weight) - 400) / 50;
                if (e.italic) score -= 4;
            }
            if (score > best_score) { best_score = score; best = &e; }
        }
        return best;
    }

    /// Every entry covering `script`, most specialised first — used to build a
    /// complete fallback chain at startup.
    [[nodiscard]] std::vector<const FontEntry*> covering_script(Script s) const {
        std::vector<const FontEntry*> out;
        for (const auto& e : entries_) {
            if (e.covers(s)) out.push_back(&e);
        }
        std::sort(out.begin(), out.end(), [s](const FontEntry* a, const FontEntry* b) {
            const bool ea = (s == Script::emoji);
            if (ea && a->color != b->color) return a->color;           // colour first for emoji
            if (!ea && a->outlines != b->outlines) return a->outlines; // drawable first
            return popcount(a->scripts) < popcount(b->scripts);        // specialised first
        });
        return out;
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

/// The always-available floor: a real TrueType face synthesized in memory,
/// so `default_stack()` is never empty even on a machine with no fonts. This
/// is what let the separate `strokefont` engine be deleted — the last-resort
/// path is now the same `Face`, measurer and rasteriser as every other font.
[[nodiscard]] inline std::shared_ptr<Face> last_resort_face() {
    return lastresort::face();
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

    // A face is added at most once, keyed by (path, index). The primary UI
    // font may already cover Latin/Greek/Cyrillic, so the per-script pass
    // below must not re-add it.
    std::vector<std::pair<std::string, std::uint32_t>> added;
    const auto already = [&](const FontEntry* e) {
        for (const auto& a : added) {
            if (a.second == e->face_index && a.first == e->path) return true;
        }
        return false;
    };
    const auto add_entry = [&](const FontEntry* e) {
        if (e == nullptr || already(e)) return false;
        if (auto face = Face::from_file(e->path, e->face_index)) {
            added.emplace_back(e->path, e->face_index);
            stack->add(std::move(face));
            return true;
        }
        return false;
    };

    // 1. The primary UI font, honouring the requested weight/slant. This is
    //    the face that renders body text, so it leads the stack.
    if (const FontEntry* ui = db.first_available(ui_font_families(), weight, italic)) {
        add_entry(ui);
    }

    // 2. One covering face for EVERY script the machine has a font for. This
    //    is what makes tofu structurally impossible: if any installed face
    //    can draw a script, it is in the chain. Scripts already covered by
    //    the primary are skipped by add_entry's dedup, and the most
    //    specialised face per script is chosen by covering().
    for (const auto& probe : script_probes) {
        if (probe.script == Script::emoji) continue;   // handled last, in colour
        add_entry(db.covering(probe.codepoint));
    }

    // 3. Colour emoji last: it must not shadow a text face for dual-mapped
    //    codepoints (☀, ❤️) that a text font also carries.
    for (const FontEntry* e : db.covering_script(Script::emoji)) {
        if (add_entry(e)) break;
    }

    // 4. A guaranteed non-empty result. On a machine with no usable fonts at
    //    all — a stripped container, a broken image — fall back to the
    //    embedded last-resort face so text still lays out and the app runs.
    if (stack->empty()) {
        if (auto face = last_resort_face()) stack->add(std::move(face));
    }

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
