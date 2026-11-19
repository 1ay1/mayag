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
#include "../core/locale.hpp"
#include "mapped_file.hpp"
#include "last_resort.hpp"
#include "apple_cascade.hpp"   // used wherever the Mac font files are present

#include <jaal/kernel/pool.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

/// OpenType script tags under which a font shapes `s`, or none when `s`
/// needs no contextual shaping (any face that covers it can draw it).
[[nodiscard]] inline std::span<const std::uint32_t> ot_tags_for(Script s) noexcept {
    using ot::make_tag;
    static constexpr std::uint32_t arab[] = {make_tag('a','r','a','b')};
    static constexpr std::uint32_t syrc[] = {make_tag('s','y','r','c')};
    static constexpr std::uint32_t thaa[] = {make_tag('t','h','a','a')};
    static constexpr std::uint32_t hebr[] = {make_tag('h','e','b','r')};
    static constexpr std::uint32_t deva[] = {make_tag('d','e','v','2'), make_tag('d','e','v','a'), make_tag('d','e','v','3')};
    static constexpr std::uint32_t beng[] = {make_tag('b','n','g','2'), make_tag('b','e','n','g'), make_tag('b','n','g','3')};
    static constexpr std::uint32_t guru[] = {make_tag('g','u','r','2'), make_tag('g','u','r','u'), make_tag('g','u','r','3')};
    static constexpr std::uint32_t gujr[] = {make_tag('g','j','r','2'), make_tag('g','u','j','r'), make_tag('g','j','r','3')};
    static constexpr std::uint32_t taml[] = {make_tag('t','m','l','2'), make_tag('t','a','m','l'), make_tag('t','m','l','3')};
    static constexpr std::uint32_t telu[] = {make_tag('t','e','l','2'), make_tag('t','e','l','u'), make_tag('t','e','l','3')};
    static constexpr std::uint32_t knda[] = {make_tag('k','n','d','2'), make_tag('k','n','d','a'), make_tag('k','n','d','3')};
    static constexpr std::uint32_t mlym[] = {make_tag('m','l','m','2'), make_tag('m','l','y','m'), make_tag('m','l','m','3')};
    static constexpr std::uint32_t sinh[] = {make_tag('s','i','n','h')};
    static constexpr std::uint32_t thai[] = {make_tag('t','h','a','i')};
    static constexpr std::uint32_t lao[]  = {make_tag('l','a','o',' ')};
    static constexpr std::uint32_t tibt[] = {make_tag('t','i','b','t')};
    static constexpr std::uint32_t mymr[] = {make_tag('m','y','m','2'), make_tag('m','y','m','r')};
    static constexpr std::uint32_t khmr[] = {make_tag('k','h','m','r')};
    static constexpr std::uint32_t mong[] = {make_tag('m','o','n','g')};
    switch (s) {
        case Script::arabic: return arab;   case Script::syriac: return syrc;
        case Script::thaana: return thaa;   case Script::hebrew: return hebr;
        case Script::devanagari: return deva; case Script::bengali: return beng;
        case Script::gurmukhi: return guru; case Script::gujarati: return gujr;
        case Script::tamil: return taml;    case Script::telugu: return telu;
        case Script::kannada: return knda;  case Script::malayalam: return mlym;
        case Script::sinhala: return sinh;  case Script::thai: return thai;
        case Script::lao: return lao;       case Script::tibetan: return tibt;
        case Script::myanmar: return mymr;  case Script::khmer: return khmr;
        case Script::mongolian: return mong;
        default: return {};
    }
}

/// The subset of `covered` this font can shape: scripts with no contextual
/// shaping need only coverage; the rest need a GSUB or GPOS script record
/// under one of their OpenType tags. (Thai and Hebrew shape acceptably
/// with their fallbacks, but a face that encodes them in GSUB still wins.)
[[nodiscard]] inline ScriptSet shapeable_scripts(const ot::FontFile& f, ScriptSet covered) {
    // A live `morx` drives shaping on its own (HarfBuzz's apply_morx, ported
    // in aat_morx.hpp), so an AAT face shapes everything it covers.
    if (const auto morx = f.table(ot::tag("morx")); morx.size() >= 8 && ot::Reader{morx}.u32_at(0) != 0)
        return covered;
    std::vector<std::uint32_t> tags;
    for (const std::uint32_t t : {ot::tags::gsub, ot::tags::gpos}) {
        const auto data = f.table(t);
        const ot::Reader r{data};
        if (data.size() < 10) continue;
        const std::size_t sl = r.u16_at(4);
        const std::uint16_t n = r.u16_at(sl);
        for (std::size_t i = 0; i < n; ++i) tags.push_back(r.u32_at(sl + 2 + i * 6));
    }
    ScriptSet out = 0;
    for (std::size_t si = 0; si < script_count; ++si) {
        const auto s = static_cast<Script>(si);
        if (!(covered & script_bit(s))) continue;
        const auto want = ot_tags_for(s);
        bool ok = want.empty();
        for (std::uint32_t w : want)
            for (std::uint32_t t : tags) ok = ok || t == w;
        if (ok) out |= script_bit(s);
    }
    return out;
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
    /// Scripts this face can SHAPE: it covers them and has OpenType layout
    /// (GSUB/GPOS) or a live Apple `morx` for them, or they need none. A
    /// face with neither draws Arabic, Hebrew or Thai as isolated, unjoined
    /// letters, so for scripts that need contextual forms such a face must
    /// lose to one that has them.
    ScriptSet     shapes = 0;

    [[nodiscard]] bool can_shape(Script s) const noexcept { return (shapes & script_bit(s)) != 0; }

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

    // MAP the file rather than read it.
    //
    // Discovery wants a family name, a weight, and which scripts the cmap
    // covers -- the table directory and a few tables, scattered through the
    // file. Reading the whole thing to get them meant a cold start pulled in
    // every installed font in full: measured at 801 MB across 972 files on an
    // ordinary Linux box, including 20 MB CJK faces read entirely to learn
    // their name.
    //
    // Mapping makes the parser's access pattern the I/O pattern. It already
    // takes a std::span, so it is handed the mapping unchanged and only the
    // pages it actually touches are faulted in.
    const MappedFile file{path.string()};
    const std::span<const std::uint8_t> data = file.data();
    if (data.size() < 12) return out;

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
        // Outlines THIS engine rasterises: glyf, CFF and CFF2 (macOS's
        // SFIndia.ttc, variable CJK faces). Only count what GlyphSource can
        // load, or covering() picks a face Face::from_file then rejects.
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
        e.shapes = shapeable_scripts(*parsed, e.scripts);

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

namespace detail {

// ── discovery cache ───────────────────────────────────────────────────────
// Parsing every installed font's header and cmap costs tens of ms per launch
// (1000 files here). The result is saved and reused while every font
// directory's mtime is unchanged. MAYAG_FONT_CACHE=0 turns it off.

inline constexpr std::uint32_t kCacheMagic = 0x4D464331;   // "MFC1"
inline constexpr std::uint32_t kCacheVersion = 3;          // 2: + shapes, 3: morx shapes

[[nodiscard]] inline std::int64_t mtime_ns(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

[[nodiscard]] inline std::string cache_path() {
    if (const char* off = std::getenv("MAYAG_FONT_CACHE"); off != nullptr && std::string_view{off} == "0") return {};
    std::filesystem::path base;
#if defined(_WIN32)
    if (const char* a = std::getenv("LOCALAPPDATA")) base = std::filesystem::path{a} / "mayag";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) base = std::filesystem::path{h} / "Library/Caches/mayag";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x != nullptr && *x != '\0') base = std::filesystem::path{x} / "mayag";
    else if (const char* h = std::getenv("HOME")) base = std::filesystem::path{h} / ".cache/mayag";
#endif
    if (base.empty()) return {};
    return (base / "fonts.cache").string();
}

class CacheWriter {
  public:
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void str(std::string_view s) { u32(static_cast<std::uint32_t>(s.size())); raw(s.data(), s.size()); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const char*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    [[nodiscard]] const std::vector<char>& bytes() const noexcept { return buf_; }
  private:
    std::vector<char> buf_;
};

class CacheReader {
  public:
    explicit CacheReader(std::span<const std::uint8_t> d) noexcept : d_{d} {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    std::uint32_t u32() { std::uint32_t v = 0; raw(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; raw(&v, 8); return v; }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok_ || n > d_.size() - pos_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(d_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    void raw(void* p, std::size_t n) {
        if (!ok_ || n > d_.size() - pos_) { ok_ = false; return; }
        std::memcpy(p, d_.data() + pos_, n);
        pos_ += n;
    }
  private:
    std::span<const std::uint8_t> d_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

[[nodiscard]] inline bool load_cache(const std::string& path,
                                     const std::vector<std::pair<std::string, std::int64_t>>& dirs,
                                     std::vector<FontEntry>& out) {
    const MappedFile file{path};
    CacheReader r{file.data()};
    if (file.data().size() < 8 || r.u32() != kCacheMagic || r.u32() != kCacheVersion) return false;
    if (r.u32() != static_cast<std::uint32_t>(script_count)) return false;
    const std::uint32_t nd = r.u32();
    if (!r.ok() || nd != dirs.size()) return false;
    for (const auto& [dir, mt] : dirs) {
        if (r.str() != dir || static_cast<std::int64_t>(r.u64()) != mt) return false;
    }
    const std::uint32_t ne = r.u32();
    if (!r.ok() || ne > 1'000'000) return false;
    std::vector<FontEntry> entries;
    entries.reserve(ne);
    for (std::uint32_t i = 0; i < ne && r.ok(); ++i) {
        FontEntry e;
        e.path = r.str();
        e.face_index = r.u32();
        e.family = r.str();
        e.subfamily = r.str();
        const std::uint32_t packed = r.u32();
        e.weight = static_cast<std::uint16_t>(packed & 0xFFFF);
        e.italic = (packed >> 16) & 1;
        e.monospace = (packed >> 17) & 1;
        e.color = (packed >> 18) & 1;
        e.outlines = (packed >> 19) & 1;
        e.glyphs = static_cast<std::uint16_t>(r.u32());
        e.scripts = r.u64();
        e.shapes = r.u64();
        entries.push_back(std::move(e));
    }
    if (!r.ok()) return false;
    out = std::move(entries);
    return true;
}

inline void save_cache(const std::string& path,
                       const std::vector<std::pair<std::string, std::int64_t>>& dirs,
                       const std::vector<FontEntry>& entries) {
    CacheWriter w;
    w.u32(kCacheMagic); w.u32(kCacheVersion); w.u32(static_cast<std::uint32_t>(script_count));
    w.u32(static_cast<std::uint32_t>(dirs.size()));
    for (const auto& [dir, mt] : dirs) { w.str(dir); w.u64(static_cast<std::uint64_t>(mt)); }
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.str(e.path); w.u32(e.face_index); w.str(e.family); w.str(e.subfamily);
        w.u32(std::uint32_t{e.weight} | std::uint32_t{e.italic} << 16 | std::uint32_t{e.monospace} << 17 |
              std::uint32_t{e.color} << 18 | std::uint32_t{e.outlines} << 19);
        w.u32(e.glyphs);
        w.u64(e.scripts);
        w.u64(e.shapes);
    }
    // Write to a temp file and rename, so a crash or a parallel launch never
    // leaves a half-written cache behind.
    std::error_code ec;
    const std::filesystem::path p{path};
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = p.string() + ".tmp" + std::to_string(reinterpret_cast<std::uintptr_t>(&w));
    {
        std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
        if (!f) return;
        f.write(w.bytes().data(), static_cast<std::streamsize>(w.bytes().size()));
        if (!f) { f.close(); std::filesystem::remove(tmp, ec); return; }
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

}  // namespace detail

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
                if (e.can_shape(s)) score += 500;           // and shapeable (joins, reorders)
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
            if (!ea && a->can_shape(s) != b->can_shape(s)) return a->can_shape(s);   // shapeable first
            return popcount(a->scripts) < popcount(b->scripts);        // specialised first
        });
        return out;
    }

  private:
    void scan() {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Every directory under the font roots, with its mtime. Adding or
        // removing a font changes its directory's mtime, so this list is the
        // cache key (fontconfig's trick).
        std::vector<std::pair<std::string, std::int64_t>> dirs;
        for (const auto& root : detail::font_directories()) {
            if (!fs::is_directory(root, ec)) continue;
            dirs.emplace_back(root.string(), detail::mtime_ns(root));
            for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec},
                 end; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it->is_directory(ec)) dirs.emplace_back(it->path().string(), detail::mtime_ns(it->path()));
            }
        }
        const std::string cache = detail::cache_path();
        if (!cache.empty() && detail::load_cache(cache, dirs, entries_)) return;

        for (const auto& root : detail::font_directories()) {
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

            for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec},
                 end; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                if (!detail::is_font_file(it->path())) continue;

                for (auto& e : detail::describe(it->path())) {
                    entries_.push_back(std::move(e));
                }
            }
        }
        if (!cache.empty()) detail::save_cache(cache, dirs, entries_);
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
    // The macOS faces first when installed (SF Pro from developer.apple.com,
    // or a Mac's System Font copied over): the macOS look is the default.
    return {"System Font", "SF Pro Text", "SF Pro", "Inter", "Cantarell", "Ubuntu", "Noto Sans", "DejaVu Sans", "Liberation Sans"};
#endif
}

[[nodiscard]] inline std::vector<std::string_view> mono_font_families() {
#if defined(__APPLE__)
    return {"SF Mono", "Menlo", "Monaco", "Courier New"};
#elif defined(_WIN32)
    return {"Cascadia Mono", "Consolas", "Courier New"};
#else
    return {"SF Mono", "Menlo", "JetBrains Mono", "Fira Code", "Source Code Pro", "DejaVu Sans Mono", "Liberation Mono"};
#endif
}

/// Emoji font families, per platform.
[[nodiscard]] inline std::vector<std::string_view> emoji_font_families() {
#if defined(__APPLE__)
    return {"Apple Color Emoji"};
#elif defined(_WIN32)
    return {"Segoe UI Emoji"};
#else
    return {"Apple Color Emoji", "Noto Color Emoji", "Twemoji", "JoyPixels"};
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

namespace detail {

/// The user's language for font selection (core/locale.hpp, shared with
/// Compose).
[[nodiscard]] inline std::string user_language() { return mayag::locale::language(); }

/// Noto CJK region for Han text, from the user's language: the same
/// codepoint is drawn differently in China, Taiwan, Hong Kong, Japan and
/// Korea, so this is a correctness choice, not a style one. Other locales
/// keep Japanese, mayag's long-standing default. Hangul and kana are the
/// same design in every region, so ONE face serves all of CJK (a second
/// would only shadow the first's ideographs).
[[nodiscard]] inline std::string_view han_region(std::string_view lang) noexcept {
    if (lang.starts_with("ko")) return "KR";
    if (lang == "zh-tw" || lang.starts_with("zh-hant")) return "TC";
    if (lang == "zh-hk" || lang == "zh-mo") return "HK";
    if (lang.starts_with("zh")) return "SC";
    return "JP";
}

}  // namespace detail

#if !defined(__APPLE__) && !defined(_WIN32)
/// The face a Linux desktop draws `s` with, best first: the standard text
/// designs fontconfig's defaults, GNOME and KDE, Chrome and Firefox resolve
/// to (Noto Sans <Script>, then the long-standing distro faces). Without
/// this the most SPECIALISED covering face wins, which for Arabic is Noto
/// Kufi (a display design) and for Hebrew Noto Rashi (a historical script).
/// Empty: no convention, the database's own ranking decides.
[[nodiscard]] inline std::vector<std::string> linux_script_families(Script s, std::string_view lang) {
    using V = std::vector<std::string>;
    switch (s) {
        case Script::arabic:     return V{"Noto Sans Arabic", "Noto Naskh Arabic", "Noto Sans Arabic UI",
                                          "Noto Naskh Arabic UI", "DejaVu Sans", "Amiri"};
        case Script::hebrew:     return V{"Noto Sans Hebrew", "Noto Serif Hebrew", "DejaVu Sans", "Culmus"};
        case Script::armenian:   return V{"Noto Sans Armenian", "DejaVu Sans"};
        case Script::georgian:   return V{"Noto Sans Georgian", "DejaVu Sans"};
        case Script::syriac:     return V{"Noto Sans Syriac", "Noto Sans Syriac Eastern", "Noto Sans Syriac Western"};
        case Script::thaana:     return V{"Noto Sans Thaana"};
        case Script::devanagari: return V{"Noto Sans Devanagari", "Lohit Devanagari"};
        case Script::bengali:    return V{"Noto Sans Bengali", "Lohit Bengali"};
        case Script::gurmukhi:   return V{"Noto Sans Gurmukhi", "Lohit Gurmukhi"};
        case Script::gujarati:   return V{"Noto Sans Gujarati", "Lohit Gujarati"};
        case Script::tamil:      return V{"Noto Sans Tamil", "Lohit Tamil"};
        case Script::telugu:     return V{"Noto Sans Telugu", "Lohit Telugu"};
        case Script::kannada:    return V{"Noto Sans Kannada", "Lohit Kannada"};
        case Script::malayalam:  return V{"Noto Sans Malayalam", "Lohit Malayalam"};
        case Script::sinhala:    return V{"Noto Sans Sinhala", "Noto Sans Sinhala UI"};
        case Script::thai:       return V{"Noto Sans Thai", "Noto Sans Thai UI", "Noto Sans Thai Looped"};
        case Script::lao:        return V{"Noto Sans Lao", "Noto Sans Lao UI"};
        case Script::khmer:      return V{"Noto Sans Khmer", "Noto Sans Khmer UI"};
        case Script::myanmar:    return V{"Noto Sans Myanmar", "Noto Sans Myanmar UI", "Padauk"};
        case Script::tibetan:    return V{"Noto Serif Tibetan", "Noto Sans Tibetan", "Jomolhari"};
        case Script::ethiopic:   return V{"Noto Sans Ethiopic", "Abyssinica SIL"};
        case Script::mongolian:  return V{"Noto Sans Mongolian"};
        case Script::cherokee:   return V{"Noto Sans Cherokee"};
        case Script::canadian:   return V{"Noto Sans Canadian Aboriginal"};
        case Script::math:       return V{"Noto Sans Math", "STIX Two Math", "DejaVu Math TeX Gyre", "DejaVu Sans"};
        case Script::symbols:    return V{"Noto Sans Symbols", "Noto Sans Symbols 2", "DejaVu Sans"};
        case Script::hangul:
        case Script::han:
        case Script::hiragana:
        case Script::katakana: {
            const std::string r{detail::han_region(lang)};
            V v{"Noto Sans CJK " + r, "Source Han Sans " + r};
            for (std::string_view other : {"JP", "SC", "TC", "KR", "HK"})
                if (other != r) v.push_back("Noto Sans CJK " + std::string{other});
            v.insert(v.end(), {"WenQuanYi Micro Hei", "WenQuanYi Zen Hei", "Droid Sans Fallback",
                               "NanumGothic", "UnDotum"});
            return v;
        }
        default: return {};
    }
}
#endif

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
                                                              bool italic = false,
                                                              std::span<const std::string_view> primary = {}) {
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
        // The UI font's emoji fallback is face 1 of Apple Color Emoji.ttc,
        // .AppleColorEmojiUI: the same images, spaced by its own UI `trak`
        // (CTFontCreateUIFontForLanguage cascades to it, never to face 0).
        if (e->color && e->face_index == 0 && e->family == "Apple Color Emoji") {
            FontEntry ui = *e;
            ui.face_index = 1;
            if (!already(&ui))
                if (auto face = Face::from_file(ui.path, 1)) {
                    added.emplace_back(ui.path, 1);
                    added.emplace_back(e->path, 0);
                    stack->add(std::move(face));
                    return true;
                }
        }
        if (auto face = Face::from_file(e->path, e->face_index)) {
            added.emplace_back(e->path, e->face_index);
            stack->add(std::move(face));
            return true;
        }
        return false;
    };

    // 1. The primary UI font, honouring the requested weight/slant. This is
    //    the face that renders body text, so it leads the stack. A caller may
    //    lead with its own families (a monospace-first app); the system UI
    //    font still follows as the first fallback.
    if (!primary.empty()) add_entry(db.first_available(primary, weight, italic));
    if (const FontEntry* ui = db.first_available(ui_font_families(), weight, italic)) {
        add_entry(ui);
    }
    // The leading face's other static weights and slants (Menlo Bold, Menlo
    // Italic), so bold text uses the real Bold instead of an emboldened
    // Regular. Variable faces reach every weight on their own axis.
    if (stack->face_count() > 0 && !stack->primary()->is_variable()) {
        const std::string fam = stack->primary()->family();
        for (const auto& e : db.entries()) {
            if (e.family != fam || already(&e)) continue;
            if (auto face = Face::from_file(e.path, e.face_index)) {
                added.emplace_back(e.path, e.face_index);
                stack->add_sibling(0, std::move(face), e.weight, e.italic);
            }
        }
    } else if (stack->face_count() > 0) {
        // A variable UI font still has a separate italic design (SF ->
        // SFNSItalic.ttf, its own variable font with true italic glyphs),
        // which CoreText uses for italic instead of shearing the upright.
        const std::string fam = stack->primary()->family();
        const bool up = !stack->primary()->is_italic();
        for (const auto& e : db.entries()) {
            if (e.family != fam || e.italic != up || already(&e)) continue;
            auto face = Face::from_file(e.path, e.face_index);
            if (face == nullptr || !face->is_variable()) continue;
            added.emplace_back(e.path, e.face_index);
            stack->add_sibling(0, std::move(face), e.weight, e.italic);
            break;
        }
    }

    // 1b. macOS: the UI font's REAL fallback order, measured from CoreText
    //     (apple_cascade.hpp): PingFang UI for Chinese, SF Arabic/Hebrew/
    //     India for those scripts, Apple SD Gothic NeoI for Hangul, Apple
    //     Symbols, STIX Two Math... 97.8% of all assigned codepoints resolve
    //     to the very face CTFontCreateForString picks. Faces are found by
    //     PostScript family (variable fonts list named instances, so the
    //     measured index is not a face index), so a moved file still matches.
    //     Not Apple-only: on Linux/Windows the same files, copied from a Mac
    //     into any font directory (~/.local/share/fonts/...), are found by
    //     file name and give the identical cascade. Absent files are skipped.
    if (primary.empty()) {
        std::unordered_map<std::string, std::string> by_name;   // file name -> path, built once
        const auto locate = [&](std::string_view measured) -> std::string {
            std::error_code ec;
            if (std::filesystem::exists(std::string{measured}, ec)) return std::string{measured};
            if (by_name.empty())
                for (const auto& e : db.entries())
                    by_name.emplace(std::filesystem::path{e.path}.filename().string(), e.path);
            const auto it = by_name.find(std::filesystem::path{std::string{measured}}.filename().string());
            return it != by_name.end() ? it->second : std::string{};
        };
        const auto family_of = [](std::string_view ps) {
            const auto dash = ps.rfind('-');
            return dash == std::string_view::npos ? ps : ps.substr(0, dash);
        };
        // stack face index per ui_cascade entry, for the exception ranges
        std::vector<std::size_t> face_at_entry(std::size(apple::ui_cascade), static_cast<std::size_t>(-1));
        std::size_t entry_no = 0;
        for (const auto& c : apple::ui_cascade) {
            const std::size_t this_entry = entry_no++;
            const std::string path = locate(c.path);
            if (path.empty()) continue;
            const std::string_view want = family_of(c.postscript);
            // Most files hold one face, or keep the measured index; try it
            // first, then every face of the collection.
            std::uint32_t found = UINT32_MAX;
            const auto matches = [&](std::uint32_t idx) {
                auto f = Face::from_file(path, idx);
                return f != nullptr && family_of(f->file().postscript_name()) == want;
            };
            if (matches(c.face_index)) found = c.face_index;
            else
                for (std::uint32_t idx = 0; idx < 64 && found == UINT32_MAX; ++idx)
                    if (Face::from_file(path, idx) == nullptr) break;
                    else if (matches(idx)) found = idx;
            if (found == UINT32_MAX) continue;
            FontEntry e;
            e.path = path;
            e.face_index = found;
            if (already(&e)) {
                for (std::size_t k = 0; k < stack->face_count(); ++k)
                    if (const Face* f = stack->face_at(k);
                        f != nullptr && f->file().postscript_name() == Face::from_file(path, found)->file().postscript_name()) {
                        face_at_entry[this_entry] = k;
                        break;
                    }
                continue;
            }
            if (auto face = Face::from_file(path, found)) {
                face_at_entry[this_entry] = stack->face_count();
                added.emplace_back(path, found);
                if (c.only.empty()) stack->add(std::move(face));
                else stack->add_limited(std::move(face), c.only);
            }
        }
        for (const auto& x : apple::ui_exceptions)
            if (x.entry < face_at_entry.size() && face_at_entry[x.entry] != static_cast<std::size_t>(-1))
                stack->prefer_range(x.lo, x.hi, face_at_entry[x.entry]);
        // CoreText draws PingFang UI's "Text" design below 20 pt and its
        // "Display" design (a sibling face of the same collection) from 20 pt.
        const std::size_t n_faces = stack->face_count();
        for (std::size_t k = 0; k < n_faces; ++k) {
            const Face* f = stack->face_at(k);
            if (f == nullptr) continue;
            const std::string& ps = f->file().postscript_name();
            const auto at = ps.find("UIText");
            if (at == std::string::npos || ps.rfind(".PingFang", 0) != 0) continue;
            const std::string want = ps.substr(0, at) + "UIDisplay" + ps.substr(at + 6, ps.find('-', at) - at - 6);
            for (const auto& [path, idx] : added) {
                if (auto probe = Face::from_file(path, idx); probe == nullptr || probe->file().postscript_name() != ps) continue;
                for (std::uint32_t j = 0; j < 64; ++j) {
                    auto d = Face::from_file(path, j);
                    if (d == nullptr) break;
                    const std::string& dps = d->file().postscript_name();
                    if (dps.rfind(want, 0) == 0 && dps.size() > want.size() && dps[want.size()] == '-') {
                        stack->add_size_variant(k, std::move(d), 20.0f);
                        break;
                    }
                }
                break;
            }
        }
    }

    // 2. One covering face for EVERY script the machine has a font for. This
    //    is what makes tofu structurally impossible: if any installed face
    //    can draw a script, it is in the chain. Scripts already covered by
    //    the primary are skipped by add_entry's dedup, and the most
    //    specialised face per script is chosen by covering().
#if !defined(__APPLE__) && !defined(_WIN32)
    //    On Linux "most specialised" picks badly (Noto Kufi, a display face,
    //    for all Arabic; Noto Rashi, a historical script, for Hebrew; an icon
    //    font for symbols the UI font already has), so a script the stack
    //    already draws is skipped, and the standard text design a Linux
    //    desktop uses (linux_script_families, locale-aware for CJK) is tried
    //    before the generic ranking.
    const std::string lang = detail::user_language();
    const auto served = [&](char32_t cp) {
        for (std::size_t i = 0; i < stack->face_count(); ++i)
            if (stack->serves(i, cp)) return true;
        return false;
    };
#endif
    // Linux: symbols and math first, so ∀ or → the UI font lacks comes from
    // a symbol design, not from the full-width ideograph-sized one in a CJK
    // face that happens to sit earlier in the chain.
    std::vector<ScriptProbe> probes{std::begin(script_probes), std::end(script_probes)};
#if !defined(__APPLE__) && !defined(_WIN32)
    std::stable_partition(probes.begin(), probes.end(), [](const ScriptProbe& p) {
        return p.script == Script::math || p.script == Script::symbols;
    });
#endif
    for (const auto& probe : probes) {
        if (probe.script == Script::emoji) continue;   // handled last, in colour
#if !defined(__APPLE__) && !defined(_WIN32)
        // Symbols and math are huge blocks one probe cannot stand for (a UI
        // font has ∀ but not 𝔸): always add their face.
        const bool broad = probe.script == Script::math || probe.script == Script::symbols;
        if (!broad && served(probe.codepoint)) continue;
        bool placed = false;
        for (const std::string& fam : linux_script_families(probe.script, lang)) {
            const FontEntry* e = db.find(fam, 400, false);
            if (e == nullptr || !e->covers(probe.script) || !e->outlines) continue;
            if (broad) {
                // Limited to the symbol/math blocks: Noto Sans Math also maps
                // Arabic letters (the Arabic math alphabet) and must not
                // shadow the Arabic text face.
                static constexpr std::array<std::array<char32_t, 2>, 6> math_blocks{{
                    {0x2100, 0x214F}, {0x2200, 0x22FF}, {0x27C0, 0x27EF}, {0x2980, 0x2AFF},
                    {0x1D400, 0x1D7FF}, {0x1EE00, 0x1EEFF}}};
                static constexpr std::array<std::array<char32_t, 2>, 9> symbol_blocks{{
                    {0x2190, 0x21FF}, {0x2300, 0x23FF}, {0x2400, 0x24FF}, {0x25A0, 0x27BF},
                    {0x2900, 0x297F}, {0x2B00, 0x2BFF}, {0x1F000, 0x1F0FF}, {0x1F700, 0x1F8FF},
                    {0x1FB00, 0x1FBFF}}};
                if (already(e)) { placed = true; break; }
                if (auto face = Face::from_file(e->path, e->face_index)) {
                    added.emplace_back(e->path, e->face_index);
                    if (probe.script == Script::math) stack->add_limited(std::move(face), math_blocks);
                    else stack->add_limited(std::move(face), symbol_blocks);
                    placed = true;
                    break;
                }
                continue;
            }
            if (add_entry(e)) { placed = true; break; }
        }
        if (placed) continue;
#endif
        // The best candidate first; if it will not load, the next one. One
        // unloadable face must not leave a whole script as tofu.
        if (add_entry(db.covering(probe.codepoint))) continue;
        for (const FontEntry* e : db.covering_script(probe.script)) {
            if (already(e)) break;          // the script is covered already
            if (add_entry(e)) break;
        }
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

/// The default stack, built on a background thread.
///
/// Font discovery and the window/GPU bring-up don't depend on each other, so
/// starting this first and collecting it after the window is up overlaps
/// the two instead of paying for them back to back.
class FontLoad {
  public:
    using Stack = std::shared_ptr<FontStack>;
    // A shared_ptr is a shared owner, which jaal won't send between threads.
    // This box is the one owner, moved out of the job exactly once.
    struct Result { std::unique_ptr<Stack> stack; };

    explicit FontLoad(FontConfig cfg = {}) : cfg_{cfg}, pool_{1} {
        job_ = pool_.submit([](std::stop_token, FontConfig c) {
            return Result{std::make_unique<Stack>(default_stack(c))};
        }, cfg);
    }
    FontLoad(const FontLoad&) = delete;
    FontLoad& operator=(const FontLoad&) = delete;

    /// The stack: the worker's if it's done, otherwise built here (the
    /// worker's copy is then dropped). Never blocks on the worker, so it needs
    /// no lock capability; in practice the scan finishes long before the GPU.
    [[nodiscard]] Stack get() {
        if (auto r = job_.try_take(); r && r->stack) return std::move(*r->stack);
        return default_stack(cfg_);
    }

  private:
    FontConfig cfg_;
    jaal::kernel::pool pool_;
    jaal::kernel::job_result<Result> job_;
};


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
