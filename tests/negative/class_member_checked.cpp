// MUST NOT COMPILE.
//
// A class is checked member by member against the element it is applied
// to. `bold` inside a class is still `bold`, and an empty box has no text.
//
// Expected: "mayag: `| weight` is not valid here — it requires a text element, ..."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

constexpr auto my_class = styles(bold, hovered(bg(colors::red)));
auto bad = box() | size(10, 10) | my_class;

int main() {}
