// HarfBuzz conformance: mayag's OpenType shaping must produce exactly what
// hb-shape (HarfBuzz 11.2.1) produces, glyph for glyph and unit for unit.
//
// tests/data/shaping/expected.txt holds FONT|TEXT|ARGS|HB-OUTPUT lines
// captured from hb-shape over 500 cases: Latin/Greek/Cyrillic with marks
// and variations, opt-in features, Arabic, Hebrew, Thai, Hangul, every
// Indic script, Myanmar, and the USE scripts, across ~200 system fonts.
// Lines whose font is not installed (Linux CI) are skipped. Regenerate with
// tools/hb_capture.sh after adding cases to corpus.txt.
#include "../tools/hb_compat.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef MAYAG_SOURCE_DIR
#define MAYAG_SOURCE_DIR "."
#endif

int main() {
    using namespace mayag::typo;
    std::ifstream in{std::string{MAYAG_SOURCE_DIR} + "/tests/data/shaping/expected.txt"};
    if (!in) { std::printf("FAIL  cannot read expected.txt\n"); return 1; }
    std::map<std::string, std::shared_ptr<Face>> faces;
    int pass = 0, fail = 0, skipped = 0;
    std::string line;
    while (std::getline(in, line)) {
        // FONT|TEXT|ARGS|EXPECTED (texts never contain '|')
        const auto a = line.find('|');
        const auto b = line.find('|', a + 1);
        const auto c = line.find('|', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
        const std::string font = line.substr(0, a), text = line.substr(a + 1, b - a - 1);
        const std::string args = line.substr(b + 1, c - b - 1), want = line.substr(c + 1);
        std::string path = font;
        std::uint32_t index = 0;
        if (auto k = path.rfind(':'); k != std::string::npos && k > 1) {
            index = static_cast<std::uint32_t>(std::stoul(path.substr(k + 1)));
            path = path.substr(0, k);
        }
        std::vector<std::string> argv;
        for (std::size_t p = 0; p < args.size();) {
            std::size_t e = args.find(' ', p);
            if (e == std::string::npos) e = args.size();
            if (e > p) argv.push_back(args.substr(p, e - p));
            p = e + 1;
        }
        // A fresh face per variation setting; plain faces are reused.
        const bool varies = args.find("--variations") != std::string::npos;
        std::shared_ptr<Face> face;
        if (!varies) {
            auto& slot = faces[font];
            if (!slot) slot = Face::from_file(path, index);
            face = slot;
        } else {
            face = Face::from_file(path, index);
        }
        if (!face) { ++skipped; continue; }
        const std::string got = hbcompat::shape(*face, text, hbcompat::parse_args(argv));
        if (got == want) { ++pass; continue; }
        ++fail;
        if (fail <= 20)
            std::printf("  FAIL  %s | %s | %s\n        hb: %s\n        mg: %s\n", font.c_str(), text.c_str(),
                        args.c_str(), want.c_str(), got.c_str());
    }
    std::printf("HarfBuzz conformance: %d / %d match (%d skipped: font not installed)\n", pass, pass + fail, skipped);
    std::printf("\n%s  %d checks, %d failures\n", fail == 0 ? "PASS" : "FAIL", pass + fail, fail);
    return fail == 0 ? 0 : 1;
}
