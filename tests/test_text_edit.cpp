// tests/test_text_edit.cpp — editing must operate on what the user SEES.
//
// TextEditState navigated by CODEPOINT, and a codepoint is not a character.
// That is invisible in English and broken almost everywhere else: one
// backspace split a family emoji into a dangling ZWJ, halved a flag into a
// stray regional indicator, stripped a skin-tone modifier so the emoji
// silently changed, and removed a combining accent leaving "cafe".
//
// None of that shows up in a test written in ASCII, which is exactly why it
// survived. These cases are the ones real users hit.

#include <mayag/mayag.hpp>

#include <cstdio>
#include <string>

using namespace mayag;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

// UTF-8 literals spelled as escapes so this file stays ASCII and cannot be
// corrupted by an editor or a a locale-confused toolchain.
const std::string kSmiley  = "\xF0\x9F\x98\x80";                       // U+1F600
const std::string kFamily  = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                             "\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D"
                             "\xF0\x9F\x91\xA6";                       // 4 people + ZWJ
const std::string kFlagJP  = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";       // 2 regional ind.
const std::string kThumbUp = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";       // + skin tone
const std::string kCombine = "e\xCC\x81";                              // e + U+0301
const std::string kCJK     = "\xE4\xB8\xAD";                           // U+4E2D

// One backspace at the end must remove exactly the last cluster.
void expect_backspace(const std::string& label, const std::string& prefix,
                      const std::string& cluster) {
    TextEditState st;
    st.set(prefix + cluster);
    st.caret = st.text.size();
    st.backspace();
    check(st.text == prefix,
          label + ": backspace removes the whole cluster (" +
              std::to_string(cluster.size()) + " bytes)");
}

}  // namespace

int main() {
    std::printf("mayag text editing\n==================\n");

    section("backspace deletes one GRAPHEME, not one codepoint");
    expect_backspace("ascii",        "a", "b");
    expect_backspace("CJK",          "a", kCJK);
    expect_backspace("emoji",        "a", kSmiley);
    expect_backspace("combining",    "caf", kCombine);
    expect_backspace("ZWJ family",   "a", kFamily);
    expect_backspace("flag",         "a", kFlagJP);
    expect_backspace("skin tone",    "a", kThumbUp);

    section("forward delete is symmetric");
    {
        // set() leaves caret == anchor == end. Moving only the caret would
        // leave a SELECTION, and del() deletes selections -- so collapse
        // both, exactly as a real caret-placement would.
        TextEditState st;
        st.set(kFamily + "a");
        st.caret = st.anchor = 0;
        st.del();
        check(st.text == "a", "del removes a whole ZWJ cluster forward");

        st.set(kFlagJP + "a");
        st.caret = st.anchor = 0;
        st.del();
        check(st.text == "a", "del removes a whole flag forward");
    }

    section("arrows step by grapheme");
    {
        TextEditState st;
        st.set(kFamily + kFlagJP);
        st.caret = st.anchor = 0;
        st.move(Motion::right, false);
        check(st.caret == kFamily.size(),
              "right arrow steps over the family in one move");
        st.move(Motion::right, false);
        check(st.caret == kFamily.size() + kFlagJP.size(),
              "and over the flag in one more");
        st.move(Motion::left, false);
        check(st.caret == kFamily.size(), "left arrow steps back symmetrically");
    }

    section("two flags are two clusters, not one run");
    {
        // Regional indicators pair up. Four of them are two flags, and
        // backspace must take exactly two -- an off-by-one here eats both.
        TextEditState st;
        st.set(kFlagJP + kFlagJP);
        st.caret = st.text.size();
        st.backspace();
        check(st.text == kFlagJP, "backspace removes one flag, leaving the other");
    }

    section("editing stays valid UTF-8");
    {
        // Whatever the rules do, they must never leave a truncated sequence:
        // a lone continuation byte makes the whole string unrenderable.
        TextEditState st;
        st.set("a" + kFamily + kCombine + kFlagJP + kCJK + kThumbUp);
        int steps = 0;
        while (!st.text.empty() && steps++ < 100) {
            st.caret = st.text.size();
            st.backspace();
            bool valid = true;
            for (std::size_t i = 0; i < st.text.size();) {
                const auto b = static_cast<unsigned char>(st.text[i]);
                const int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2
                              : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
                if (len == 0 || i + static_cast<std::size_t>(len) > st.text.size()) {
                    valid = false; break;
                }
                for (int k = 1; k < len; ++k) {
                    if ((static_cast<unsigned char>(st.text[i + static_cast<std::size_t>(k)])
                         & 0xC0) != 0x80) { valid = false; break; }
                }
                if (!valid) break;
                i += static_cast<std::size_t>(len);
            }
            if (!valid) {
                check(false, "string stayed valid UTF-8 while deleting");
                break;
            }
        }
        check(st.text.empty(), "deleting to empty terminates and stays valid");
    }

    section("IME preedit");
    {
        // Composing text is shown but NOT committed: display includes it,
        // the model's text does not, until commit.
        TextEditState st;
        st.set("a");
        st.caret = 1;
        st.set_preedit("nihon");
        check(st.composing(), "set_preedit puts the field in composing state");
        check(st.text == "a", "the model's text is untouched while composing");
        check(st.display_text().find("nihon") != std::string::npos,
              "display_text shows the preedit");

        st.commit_preedit();
        check(!st.composing(), "commit clears the composing state");
        check(st.text == "anihon", "and the text is now committed");

        st.set_preedit("xyz");
        st.cancel_preedit();
        check(!st.composing() && st.text == "anihon",
              "cancel discards the preedit without touching the text");
    }

    std::printf("\n%s  %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
