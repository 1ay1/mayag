// tests/test_unicode.cpp — UCD tables and the bidi algorithm
//
// 1. spot checks of every property accessor in unicode_data.hpp
// 2. UAX #9 conformance: tests/data/bidi_character_test_subset.txt always;
//    the full BidiCharacterTest.txt when MAYAG_BIDI_TEST_FULL points to it,
//    and BidiTest.txt when MAYAG_BIDI_CLASS_TEST points to it. 100% required.

#include <mayag/font/bidi.hpp>
#include <mayag/font/unicode_data.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace mayag::typo;

int checks = 0;
int failures = 0;

void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* n) { std::printf("\n%s\n", n); }

void test_properties() {
    using namespace ucd;
    section("ucd properties");
    check(script(0x0627) == Script::Arabic, "script(alef) == Arabic");
    check(script('A') == Script::Latin, "script(A) == Latin");
    check(script(' ') == Script::Common, "script(space) == Common");
    check(script(0x0301) == Script::Inherited, "script(U+0301) == Inherited");
    check(script(0x0378) == Script::Unknown, "unassigned => Unknown");
    check(iso15924_tag(Script::Arabic) == 0x41726162u, "tag Arab");
    check(iso15924_tag(Script::Common) == 0x5A797979u, "tag Zyyy");
    check(iso15924_tag(Script::Devanagari) == 0x44657661u, "tag Deva");
    {
        auto x = script_extensions(0x0964);  // DEVANAGARI DANDA: many scripts
        bool deva = false;
        for (auto s : x) deva |= s == Script::Devanagari;
        check(x.size() > 1 && deva, "scx(U+0964) includes Devanagari");
        auto y = script_extensions('A');
        check(y.size() == 1 && y[0] == Script::Latin, "scx fallback == script");
    }
    check(bidi_class(0x05D0) == BidiClass::R, "bidi(alef hebrew) == R");
    check(bidi_class(0x0627) == BidiClass::AL, "bidi(alef arabic) == AL");
    check(bidi_class('1') == BidiClass::EN, "bidi(1) == EN");
    check(bidi_class(0x0661) == BidiClass::AN, "bidi(arabic 1) == AN");
    check(bidi_class(0x2067) == BidiClass::RLI, "bidi(U+2067) == RLI");
    check(bidi_class(0x05FF) == BidiClass::R, "unassigned Hebrew block => R");
    check(bidi_class(0x08A0) == BidiClass::AL, "0x08A0 => AL");
    check(bidi_class(0x0870 + 0x1F) == BidiClass::AL, "0x088F (unassigned Arabic Ext-B) => AL");
    check(bidi_class(0x07BF) == BidiClass::AL, "unassigned 0x07BF (Thaana tail) => AL");
    check(bidi_class(0xFDD0) == BidiClass::BN, "noncharacter FDD0 => BN");
    check(bidi_class(0x20C1) == BidiClass::ET || bidi_class(0x20CF) == BidiClass::ET, "currency block @missing ET");
    check(bidi_mirror('(') == ')' && bidi_mirror('a') == 'a', "mirror");
    check(bidi_bracket('[').type == BracketType::open && bidi_bracket('[').pair == ']', "bracket [");
    check(bidi_bracket(0x232A).type == BracketType::close, "bracket U+232A close");
    check(joining_type(0x0628) == JoiningType::D, "jt(beh) == D");
    check(joining_type(0x0627) == JoiningType::R, "jt(alef) == R");
    check(joining_type(0x064B) == JoiningType::T, "jt(fathatan) == T");
    check(joining_type(0x200D) == JoiningType::C, "jt(ZWJ) == C");
    check(joining_type('a') == JoiningType::U, "jt(a) == U");
    check(joining_group(0x0628) == JoiningGroup::BEH, "jg(beh) == BEH");
    check(general_category('A') == GeneralCategory::Lu, "gc(A) == Lu");
    check(general_category(0x4E00 + 5) == GeneralCategory::Lo, "gc(CJK range) == Lo");
    check(general_category(0xAC01) == GeneralCategory::Lo, "gc(Hangul range) == Lo");
    check(general_category(0x0378) == GeneralCategory::Cn, "gc(unassigned) == Cn");
    check(is_mark(0x0301) && !is_mark('a'), "is_mark");
    check(combining_class(0x0301) == 230, "ccc(U+0301) == 230");
    check(combining_class(0x093C) == 7, "ccc(nukta) == 7");
    check(indic_syllabic(0x0915) == IndicSyllabic::Consonant, "isc(KA) == Consonant");
    check(indic_syllabic(0x094D) == IndicSyllabic::Virama, "isc(virama) == Virama");
    check(indic_positional(0x093F) == IndicPositional::Left, "ipc(U+093F) == Left");
    check(indic_positional('a') == IndicPositional::Not_Applicable, "ipc(a) == NA");
    check(grapheme_break(0x1F1E6) == GraphemeBreak::Regional_Indicator, "gcb(RI)");
    check(grapheme_break('\r') == GraphemeBreak::CR, "gcb(CR)");
    check(grapheme_break(0x1100) == GraphemeBreak::L, "gcb(Hangul L)");
    check(grapheme_break(0xAC00) == GraphemeBreak::LV, "gcb(LV)");
    check(is_extended_pictographic(0x1F600) && !is_extended_pictographic('a'), "ExtPict");
    check(is_default_ignorable(0x200D) && !is_default_ignorable('a'), "DI(ZWJ)");
    {
        auto d = canonical_decomposition(0x00E9);
        check(d.a == 'e' && d.b == 0x0301, "decomp(e acute)");
        auto h = canonical_decomposition(0xAC01);
        check(h.a == 0xAC00 && h.b == 0x11A8, "decomp(Hangul LVT)");
        auto h2 = canonical_decomposition(0xAC00);
        check(h2.a == 0x1100 && h2.b == 0x1161, "decomp(Hangul LV)");
        auto s = canonical_decomposition(0x212B);
        check(s.a == 0x00C5 && s.b == 0, "decomp(Angstrom) singleton");
        check(canonical_decomposition('a').a == 0, "decomp(a) none");
        check(canonical_composition('e', 0x0301) == 0x00E9, "comp(e, acute)");
        check(canonical_composition(0x1100, 0x1161) == 0xAC00, "comp(L, V)");
        check(canonical_composition(0xAC00, 0x11A8) == 0xAC01, "comp(LV, T)");
        check(canonical_composition(0x0915, 0x093C) == 0, "comp(KA, nukta) excluded");
        check(canonical_composition('a', 'b') == 0, "comp(a, b) none");
    }
}

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// levels/order comparison shared by both test formats; "x" positions skipped.
bool compare(const std::vector<std::uint8_t>& lv, const std::vector<std::string>& want_lv,
             const std::vector<std::uint32_t>& want_ord) {
    if (lv.size() != want_lv.size()) return false;
    for (std::size_t i = 0; i < lv.size(); ++i)
        if (want_lv[i] != "x" && std::stoi(want_lv[i]) != lv[i]) return false;
    auto ord = bidi::visual_order(lv);
    std::vector<std::uint32_t> got;
    for (auto o : ord)
        if (want_lv[o] != "x") got.push_back(o);
    return got == want_ord;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> v;
    std::string w;
    while (is >> w) v.push_back(w);
    return v;
}

void run_character_test(const std::string& path, const char* label) {
    std::ifstream f(path);
    check(f.good(), std::string("open ") + path);
    std::string line;
    long total = 0, pass = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fl = split(line, ';');
        if (fl.size() < 5) continue;
        ++total;
        std::vector<char32_t> text;
        for (auto& w : words(fl[0])) text.push_back(static_cast<char32_t>(std::stoul(w, nullptr, 16)));
        const int pd = std::stoi(fl[1]);
        const auto dir = pd == 0 ? bidi::Direction::ltr : pd == 1 ? bidi::Direction::rtl : bidi::Direction::auto_detect;
        auto p = bidi::resolve(text, dir);
        auto lv = bidi::line_levels(text, p, 0, text.size());
        std::vector<std::uint32_t> ord;
        for (auto& w : words(fl[4])) ord.push_back(static_cast<std::uint32_t>(std::stoul(w)));
        const bool ok = p.base_level == std::stoi(fl[2]) && compare(lv, words(fl[3]), ord);
        if (ok) ++pass;
        else if (total - pass <= 5) {
            std::printf("  mismatch: %s\n    got base %d levels", line.c_str(), p.base_level);
            for (auto l : lv) std::printf(" %d", l);
            std::printf("\n");
        }
    }
    std::printf("  %s: %ld / %ld\n", label, pass, total);
    check(total > 0 && pass == total, std::string(label) + " 100%");
}

void run_class_test(const std::string& path) {
    using ucd::BidiClass;
    static const char* names[] = {"L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S",
                                  "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"};
    std::ifstream f(path);
    check(f.good(), std::string("open ") + path);
    std::string line;
    std::vector<std::string> want_lv;
    std::vector<std::uint32_t> want_ord;
    long total = 0, pass = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("@Levels:", 0) == 0) { want_lv = words(line.substr(8)); continue; }
        if (line.rfind("@Reorder:", 0) == 0) {
            want_ord.clear();
            for (auto& w : words(line.substr(9))) want_ord.push_back(static_cast<std::uint32_t>(std::stoul(w)));
            continue;
        }
        if (line[0] == '@') continue;
        auto fl = split(line, ';');
        if (fl.size() < 2) continue;
        std::vector<BidiClass> cls;
        for (auto& w : words(fl[0]))
            for (int k = 0; k < 23; ++k)
                if (w == names[k]) cls.push_back(BidiClass(k));
        const int bits = std::stoi(fl[1]);
        const bidi::Direction dirs[] = {bidi::Direction::auto_detect, bidi::Direction::ltr, bidi::Direction::rtl};
        for (int k = 0; k < 3; ++k) {
            if (!(bits & (1 << k))) continue;
            ++total;
            auto p = bidi::resolve_classes(cls, dirs[k]);
            auto lv = bidi::line_levels_classes(cls, p, 0, cls.size());
            if (compare(lv, want_lv, want_ord)) ++pass;
            else if (total - pass <= 5) std::printf("  mismatch (dir bit %d): %s\n", k, line.c_str());
        }
    }
    std::printf("  BidiTest.txt: %ld / %ld\n", pass, total);
    check(total > 0 && pass == total, "BidiTest.txt 100%");
}

void test_bidi_api() {
    section("bidi api");
    const std::u32string t = U"abc \u05D0\u05D1\u05D2 def";
    check(!bidi::is_trivially_ltr(t) && bidi::is_trivially_ltr(U"hello, world"), "is_trivially_ltr");
    auto p = bidi::resolve(t, bidi::Direction::auto_detect);
    check(p.base_level == 0, "auto base level LTR");
    auto ord = bidi::visual_order(bidi::line_levels(t, p, 0, t.size()));
    check(ord.size() == t.size() && ord[4] == 6 && ord[6] == 4, "hebrew run reversed");
    const std::u32string x = U"\u05D0\u202Aa\u202Cb";
    auto px = bidi::resolve(x, bidi::Direction::auto_detect);
    auto ox = bidi::visual_order(px.levels);
    std::vector<bool> seen(x.size(), false);
    for (auto o : ox) seen[o] = true;
    bool all = true;
    for (bool s : seen) all &= s;
    check(px.base_level == 1 && all, "removed chars appear once in visual order");
}

} // namespace

int main() {
    test_properties();
    test_bidi_api();
    section("bidi conformance");
    run_character_test(std::string(MAYAG_SOURCE_DIR) + "/tests/data/bidi_character_test_subset.txt",
                       "BidiCharacterTest subset");
    if (const char* full = std::getenv("MAYAG_BIDI_TEST_FULL")) run_character_test(full, "BidiCharacterTest.txt");
    if (const char* ct = std::getenv("MAYAG_BIDI_CLASS_TEST")) run_class_test(ct);
    if (failures == 0) std::printf("\nPASS  %d checks, 0 failures\n", checks);
    else std::printf("\nFAIL  %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
