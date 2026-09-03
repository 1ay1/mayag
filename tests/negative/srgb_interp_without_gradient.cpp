// MUST NOT COMPILE.
//
// `srgb_interpolation` chooses how a GRADIENT ramps between stops. On a solid
// fill there is nothing to interpolate, so accepting it would be a lie about
// what the renderer is going to do.
//
// Expected: "mayag: `| srgb_interpolation` is not valid here — it requires a
//            gradient fill (add `| linear_gradient(...)` first)."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | bg(colors::red) | srgb_interpolation;

int main() {}
