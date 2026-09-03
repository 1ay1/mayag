// MUST NOT COMPILE.
//
// An absolutely positioned node is OUT OF FLOW: it is not a flex item, so
// there is no leftover main-axis space for it to take a share of. `grow` and
// `absolute` are mutually exclusive, and mayag catches the contradiction in
// EITHER order — the ban is declared on both modifiers.
//
// Expected: "mayag: `| grow` conflicts with absolute or fixed positioning
//            which is already set on this element."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | absolute(10, 10) | grow();

int main() {}
