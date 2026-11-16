// MUST NOT COMPILE.
//
// Text properties inherit from a container to the text inside it, so
// `v(text<"a">) | fg(c)` is valid. An EMPTY box has no text anywhere below
// it, so a text property there can only be a mistake.
//
// Expected: "mayag: `| fg` is not valid here — it requires a text element,
//            or a container with children ..."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | fg(colors::white);

int main() {}
