// MUST NOT COMPILE.
//
// `size(w, h)` sets BOTH axes, so a later `height()` silently overrides half
// of it. In mayag's gallery this exact shape meant every swatch's declared
// `size(92, 60)` was thrown away by a wrapper applying `height(72)` — the
// swatches were never the size the code said they were.
//
// Expected: "mayag: `| height` conflicts with an explicit height which is
//            already set on this element."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | size(100, 50) | height(72);

int main() {}
