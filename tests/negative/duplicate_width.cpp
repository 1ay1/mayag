// MUST NOT COMPILE.
//
// Specifying a width twice is always a mistake: the answer ("the last one
// wins") is invisible at the call site, and the two numbers disagree for a
// reason. This ban caught a real bug in mayag's own dashboard, where the root
// element was sized once in a local and again at the return.
//
// Expected: "mayag: `| width` conflicts with an explicit width which is
//            already set on this element."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | width(200) | width(300);

int main() {}
