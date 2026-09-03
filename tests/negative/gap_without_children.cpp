// MUST NOT COMPILE.
//
// `gap` is spacing BETWEEN children. A text node has no children, so this is
// always a no-op — and a silent no-op in a layout system is how you end up
// with a spacing bug you cannot find.
//
// Expected: "mayag: `| gap` is not valid here — it requires a container
//            (built with v/h/z)."

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = text<"hello"> | gap(8);

int main() {}
