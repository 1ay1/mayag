// MUST NOT COMPILE.
//
// Colouring a border that was never created. In a stringly-typed framework
// this silently does nothing and you spend twenty minutes wondering why your
// border is invisible. In mayag it is a compile error that says so.
//
// Expected: "mayag: `| border_color` is not valid here — it requires a border
//            (add `| border(width, color)` first)."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | border_color(colors::red);

int main() {}
