// MUST NOT COMPILE.
//
// `!(a && b)` would need OR, which the cascade deliberately does not have:
// an OR of states is two rules, and two rules read better than one clever
// one. Negate each condition instead: `!state::hover`, `!state::selected`.
//
// Expected: a consteval failure naming
//   negating_a_compound_selector_is_unsupported_negate_each_condition_instead

#include <mayag/mayag.hpp>

using namespace mayag;
using namespace mayag::dsl;

auto bad = box() | size(10, 10)
         | when(!(state::hover && state::selected), bg(colors::red));

int main() {}
