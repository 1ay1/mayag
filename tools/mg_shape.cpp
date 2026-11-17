// mg_shape FONT[:index] TEXT [--script=Xxxx] [--direction=ltr|rtl] [--features=+a,-b]
//          [--variations=wght=700,...] [--bot]
// Shapes TEXT with mayag's OpenType pipeline and prints hb-shape's
// --no-glyph-names format at font-units scale, for tools/hb_compare.sh.
#include "hb_compat.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace mayag::typo;
    if (argc < 3) return 2;
    std::string path = argv[1];
    std::uint32_t index = 0;
    if (auto c = path.rfind(':'); c != std::string::npos && c > 1) {
        index = static_cast<std::uint32_t>(std::stoul(path.substr(c + 1)));
        path = path.substr(0, c);
    }
    auto face = Face::from_file(path, index);
    if (!face) { std::fprintf(stderr, "cannot open font\n"); return 1; }
    const std::vector<std::string> args(argv + 3, argv + argc);
    std::printf("%s\n", hbcompat::shape(*face, argv[2], hbcompat::parse_args(args)).c_str());
}
