// MUST NOT COMPILE.
//
// `text_of()` stores a `string_view`. Handing it a temporary means the
// element outlives its own bytes — and the symptom is not a crash but
// garbage rendered as glyphs, which is far harder to trace.
//
// mayag shipped exactly this bug: `text_of(std::to_string(x) + "%")` in the
// gallery produced a text node holding three NUL bytes. It survived every
// test and every screenshot review, and only surfaced when the layout auditor
// flagged the resulting box as "too narrow to wrap".
//
// Expected: "mayag: text_of() stores a VIEW, and this string dies at the end
//            of the statement, so the text would dangle. Use text_owned()..."

#include <mayag/mayag.hpp>

#include <string>

using namespace mayag;
using namespace mayag::dsl;

auto make() {
    int n = 42;
    return text_of(std::to_string(n) + "%");
}

int main() { (void)make(); }
