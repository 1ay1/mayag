// MUST NOT COMPILE.
//
// A font weight on a container. There are no glyphs here to embolden, so this
// modifier can only be a mistake — either the wrong node or a forgotten
// `text<>`.
//
// Expected: "mayag: `| weight` is not valid here — it requires a text element."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | bold;

int main() {}
