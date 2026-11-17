// UAX #14 line breaking and UAX #29 grapheme clusters: conformance against
// the official LineBreakTest.txt / GraphemeBreakTest.txt (Unicode 18).
#include <mayag/font/segment.hpp>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MAYAG_SOURCE_DIR
#define MAYAG_SOURCE_DIR "."
#endif

namespace {
int checks = 0, failures = 0;
void check(bool cond, const std::string& what) {
    ++checks;
    if (!cond) { ++failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* name) { std::printf("\n%s\n", name); }

/// Parse "÷ 0041 × 0308 ÷ 0020 ÷" into codepoints and expected breaks
/// (breaks[i] = break before cps[i]; breaks.back() is the end).
bool parse(const std::string& line, std::vector<char32_t>& cps, std::vector<bool>& breaks) {
    cps.clear();
    breaks.clear();
    std::string body = line.substr(0, line.find('#'));
    std::istringstream in{body};
    std::string tok;
    while (in >> tok) {
        if (tok == "\xC3\xB7") breaks.push_back(true);          // ÷
        else if (tok == "\xC3\x97") breaks.push_back(false);    // ×
        else cps.push_back(static_cast<char32_t>(std::strtoul(tok.c_str(), nullptr, 16)));
    }
    return !cps.empty() && breaks.size() == cps.size() + 1;
}

using namespace mayag::typo;

int run_line_test(const std::string& path, int max_report) {
    std::ifstream f{path};
    if (!f) return -1;
    int pass = 0, total = 0, reported = 0;
    std::string line;
    std::vector<char32_t> cps;
    std::vector<bool> want;
    while (std::getline(f, line)) {
        if (!parse(line, cps, want)) continue;
        ++total;
        std::vector<seg::Break> got(cps.size());
        seg::line_breaks(cps, got);
        bool ok = true;
        for (std::size_t i = 1; i < cps.size(); ++i) ok &= (got[i] != seg::Break::none) == want[i];
        if (ok) { ++pass; continue; }
        if (reported++ < max_report) std::printf("  mismatch: %s\n", line.c_str());
    }
    std::printf("  %s: %d / %d\n", path.substr(path.rfind('/') + 1).c_str(), pass, total);
    return total - pass;
}

int run_grapheme_test(const std::string& path, int max_report) {
    std::ifstream f{path};
    if (!f) return -1;
    int pass = 0, total = 0, reported = 0;
    std::string line;
    std::vector<char32_t> cps;
    std::vector<bool> want;
    while (std::getline(f, line)) {
        if (!parse(line, cps, want)) continue;
        ++total;
        std::vector<char> raw(cps.size());
        std::span<bool> got{reinterpret_cast<bool*>(raw.data()), raw.size()};
        seg::grapheme_boundaries(cps, got);
        bool ok = true;
        for (std::size_t i = 1; i < cps.size(); ++i) ok &= got[i] == want[i];
        if (ok) { ++pass; continue; }
        if (reported++ < max_report) std::printf("  mismatch: %s\n", line.c_str());
    }
    std::printf("  %s: %d / %d\n", path.substr(path.rfind('/') + 1).c_str(), pass, total);
    return total - pass;
}
}  // namespace

int main() {
    std::printf("mayag text segmentation\n=======================");
    const std::string data = std::string{MAYAG_SOURCE_DIR} + "/tests/data/";

    section("UAX #14 line breaking");
    {
        const int bad = run_line_test(data + "line_break_test.txt", 12);
        check(bad == 0, "LineBreakTest (committed copy) passes in full");
        if (const char* full = std::getenv("MAYAG_LB_TEST_FULL"))
            check(run_line_test(full, 12) == 0, "LineBreakTest.txt passes in full");
    }
    section("UAX #29 grapheme clusters");
    {
        const int bad = run_grapheme_test(data + "grapheme_break_test.txt", 12);
        check(bad == 0, "GraphemeBreakTest (committed copy) passes in full");
        if (const char* full = std::getenv("MAYAG_GB_TEST_FULL"))
            check(run_grapheme_test(full, 12) == 0, "GraphemeBreakTest.txt passes in full");
    }

    section("helpers and tailoring");
    {
        const auto offs = seg::line_break_offsets("hello world, again");
        check(offs.size() == 2 && offs[0] == 6 && offs[1] == 13, "UTF-8 offsets: after each space");
        std::vector<std::size_t> hard;
        (void)seg::line_break_offsets("a\nb", seg::Tailoring::none, &hard);
        check(hard.size() == 1 && hard[0] == 2, "a newline is a mandatory break");
        // Thai: no break anywhere by default (SA -> AL), syllable breaks with the tailoring.
        const std::string thai = "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a";   // สวัสดีครับ
        check(seg::line_break_offsets(thai).empty(), "Thai has no break opportunity without a dictionary");
        const auto syl = seg::line_break_offsets(thai, seg::Tailoring::sea_syllables);
        check(!syl.empty(), "the SEA tailoring breaks between Thai syllables");
        bool never_before_mark = true;
        for (std::size_t o : syl) never_before_mark &= !(o + 2 < thai.size() && static_cast<unsigned char>(thai[o + 2]) == 0xb1);
        check(never_before_mark, "...but never before a combining vowel");
        const std::string fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7x";   // 👨‍👩‍👧x
        check(seg::next_grapheme(fam, 0) == fam.size() - 1, "an emoji ZWJ family is one grapheme");
        check(seg::prev_grapheme(fam, fam.size()) == fam.size() - 1, "prev_grapheme steps back over x");
        check(seg::prev_grapheme(fam, fam.size() - 1) == 0, "...and over the whole family");
        const std::string ksha = "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7";   // क्ष
        check(seg::next_grapheme(ksha, 0) == ksha.size(), "a Devanagari conjunct is one grapheme (GB9c)");

        // The windowed caret helpers agree with segmenting the whole string,
        // on long random strings mixing every hard case (RI runs, ZWJ
        // emoji, conjuncts, Hangul, CRLF).
        const char32_t pool[] = {U'a', U' ', 0x0301, 0x1F1E6, 0x1F1F7, 0x200D, 0x1F468, 0x1F3FD, 0x0915, 0x094D,
                                 0x0937, 0x1100, 0x1161, 0x11A8, U'\r', U'\n', 0x0E33, 0xFE0F};
        std::uint32_t rng = 7;
        bool agree = true;
        for (int trial = 0; trial < 40 && agree; ++trial) {
            std::vector<char32_t> cps;
            std::string s;
            std::vector<std::size_t> at;
            for (int k = 0; k < 900; ++k) {
                rng = rng * 1664525u + 1013904223u;
                const char32_t c = pool[(rng >> 8) % std::size(pool)];
                at.push_back(s.size());
                cps.push_back(c);
                if (c < 0x80) s += static_cast<char>(c);
                else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
                else { s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
            }
            std::vector<char> raw(cps.size());
            std::span<bool> full{reinterpret_cast<bool*>(raw.data()), raw.size()};
            seg::grapheme_boundaries(cps, full);
            std::vector<std::size_t> truth;
            for (std::size_t k = 1; k < cps.size(); ++k) if (full[k]) truth.push_back(at[k]);
            truth.push_back(s.size());
            std::size_t p = 0;
            for (std::size_t t : truth) { agree &= seg::next_grapheme(s, p) == t; p = t; }
            std::size_t q = s.size();
            for (std::size_t k = truth.size() - 1; k-- > 0;) { agree &= seg::prev_grapheme(s, q) == truth[k]; q = truth[k]; }
            agree &= seg::prev_grapheme(s, q) == 0;
        }
        check(agree, "windowed next/prev_grapheme agree with whole-string segmentation");
    }

    std::printf("\n%s  %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
