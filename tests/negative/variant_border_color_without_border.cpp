// MUST NOT COMPILE.
//
// A state variant is checked against the element exactly like a plain
// modifier: a hover border colour on a box with no border would be a
// silent no-op the moment the pointer arrives.
//
// Expected: "mayag: `| border_color` is not valid here — it requires a border ..."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | size(10, 10) | hovered(border_color(colors::red));

int main() {}
