#pragma once
// mayag::TextEditState — a text field's state, as plain data
//
// Same philosophy as ScrollState: the contents, the caret, and the selection
// are APPLICATION state. They live in your Model, they serialise, they can be
// asserted on in a test, and only `update()` changes them. mayag does not
// hide a mutable string inside a widget.
//
//     struct Model { TextEditState name; };
//
//     // view
//     text_field(m.name, theme) | id<"name">
//
//     // subscribe
//     Sub<Msg>::on_text([](std::string_view s) { return Typed{std::string{s}}; }),
//     Sub<Msg>::on_any_key([](const KeyEvent& k) { return Pressed{k}; })
//
//     // update
//     m.name.insert(e.text);        // or .backspace(), .move(...), etc.
//
// Everything here works in BYTE offsets but moves in GRAPHEMES, so a caret
// never lands inside a multi-byte character and one press of Left never eats
// half an emoji.

#include "../core/geometry.hpp"
#include "../app/event.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace mayag {

/// How the caret should move.
enum class Motion : std::uint8_t {
    left, right, up, down,
    word_left, word_right,
    line_start, line_end,
    doc_start, doc_end,
};

struct TextEditState {
    std::string text;

    /// Caret position, as a BYTE offset into `text`. Always on a codepoint
    /// boundary — every mutation below maintains that.
    std::size_t caret = 0;

    /// The other end of the selection. When equal to `caret` there is no
    /// selection. Keeping it as an anchor (rather than a start/end pair) is
    /// what makes shift-extension work naturally in both directions.
    std::size_t anchor = 0;

    /// Preferred column for vertical motion, in bytes from the line start.
    /// Remembered so moving down through a short line and back up returns to
    /// the original column — the behaviour every editor has and every naive
    /// implementation loses.
    std::size_t goal_column = 0;
    bool        goal_valid = false;

    bool multiline = false;
    /// Maximum bytes; 0 means unlimited.
    std::size_t max_length = 0;

    // ── queries ─────────────────────────────────────────────────────────

    [[nodiscard]] bool has_selection() const noexcept { return caret != anchor; }
    [[nodiscard]] std::size_t selection_start() const noexcept { return std::min(caret, anchor); }
    [[nodiscard]] std::size_t selection_end() const noexcept { return std::max(caret, anchor); }

    [[nodiscard]] std::string_view selected() const noexcept {
        return std::string_view{text}.substr(selection_start(),
                                             selection_end() - selection_start());
    }
    [[nodiscard]] bool empty() const noexcept { return text.empty(); }

    // ── codepoint-safe navigation ───────────────────────────────────────

    /// Byte offset of the codepoint boundary before `i`.
    ///
    /// Stepping one BYTE would land inside a multi-byte sequence and render
    /// the string unusable from that point on. This is the invariant every
    /// mutation below relies on.
    [[nodiscard]] std::size_t prev_boundary(std::size_t i) const noexcept {
        if (i == 0) return 0;
        --i;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        return i;
    }

    [[nodiscard]] std::size_t next_boundary(std::size_t i) const noexcept {
        if (i >= text.size()) return text.size();
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        return i;
    }

    [[nodiscard]] std::size_t line_start_of(std::size_t i) const noexcept {
        while (i > 0 && text[i - 1] != '\n') --i;
        return i;
    }
    [[nodiscard]] std::size_t line_end_of(std::size_t i) const noexcept {
        while (i < text.size() && text[i] != '\n') ++i;
        return i;
    }

    /// Word boundaries, using the classification every editor agrees on:
    /// runs of alphanumerics are words, runs of punctuation are words, and
    /// whitespace is skipped on the way.
    [[nodiscard]] std::size_t word_left_of(std::size_t i) const noexcept {
        while (i > 0 && is_space(text[i - 1])) --i;
        if (i == 0) return 0;
        const bool word = is_word(text[i - 1]);
        while (i > 0 && !is_space(text[i - 1]) && is_word(text[i - 1]) == word) --i;
        return i;
    }
    [[nodiscard]] std::size_t word_right_of(std::size_t i) const noexcept {
        const std::size_t n = text.size();
        while (i < n && is_space(text[i])) ++i;
        if (i >= n) return n;
        const bool word = is_word(text[i]);
        while (i < n && !is_space(text[i]) && is_word(text[i]) == word) ++i;
        return i;
    }

    // ── caret movement ──────────────────────────────────────────────────

    /// Move the caret. `extend` keeps the anchor, producing a selection.
    void move(Motion m, bool extend = false) {
        const std::size_t before = caret;

        switch (m) {
            case Motion::left:
                // Collapsing a selection with an unmodified arrow moves to
                // its EDGE, not one character from the caret. Every editor
                // does this and it is the first thing users notice missing.
                caret = (has_selection() && !extend) ? selection_start()
                                                     : prev_boundary(caret);
                break;
            case Motion::right:
                caret = (has_selection() && !extend) ? selection_end()
                                                     : next_boundary(caret);
                break;

            case Motion::word_left:  caret = word_left_of(caret); break;
            case Motion::word_right: caret = word_right_of(caret); break;
            case Motion::line_start: caret = line_start_of(caret); break;
            case Motion::line_end:   caret = line_end_of(caret); break;
            case Motion::doc_start:  caret = 0; break;
            case Motion::doc_end:    caret = text.size(); break;

            case Motion::up:
            case Motion::down: {
                if (!multiline) break;
                const std::size_t ls = line_start_of(caret);
                if (!goal_valid) { goal_column = caret - ls; goal_valid = true; }

                if (m == Motion::up) {
                    if (ls == 0) { caret = 0; break; }
                    const std::size_t prev_ls = line_start_of(ls - 1);
                    caret = std::min(prev_ls + goal_column, ls - 1);
                } else {
                    const std::size_t le = line_end_of(caret);
                    if (le >= text.size()) { caret = text.size(); break; }
                    const std::size_t next_ls = le + 1;
                    caret = std::min(next_ls + goal_column, line_end_of(next_ls));
                }
                break;
            }
        }

        // Horizontal motion invalidates the remembered column.
        if (m != Motion::up && m != Motion::down) goal_valid = false;
        if (!extend) anchor = caret;
        (void)before;
    }

    void select_all() { anchor = 0; caret = text.size(); }
    void collapse() { anchor = caret; }

    /// Select the word under the caret — what a double click does.
    void select_word() {
        anchor = word_left_of(next_boundary(caret) > caret ? next_boundary(caret) : caret);
        anchor = word_left_of(caret);
        caret  = word_right_of(anchor);
    }

    /// Select the whole line — what a triple click does.
    void select_line() {
        anchor = line_start_of(caret);
        caret  = line_end_of(caret);
    }

    // ── editing ─────────────────────────────────────────────────────────

    /// Insert text at the caret, replacing any selection.
    void insert(std::string_view s) {
        delete_selection();

        std::string filtered;
        filtered.reserve(s.size());
        for (char ch : s) {
            // A single-line field must not accept newlines even from a paste;
            // silently swallowing them is better than storing a value the
            // field cannot display.
            if (!multiline && (ch == '\n' || ch == '\r')) continue;
            filtered.push_back(ch);
        }

        if (max_length > 0 && text.size() + filtered.size() > max_length) {
            const std::size_t room = max_length > text.size() ? max_length - text.size() : 0;
            filtered.resize(room);
            // Do not truncate mid-codepoint.
            while (!filtered.empty() &&
                   (static_cast<unsigned char>(filtered.back()) & 0xC0) == 0x80) {
                filtered.pop_back();
            }
        }
        if (filtered.empty()) return;

        text.insert(caret, filtered);
        caret += filtered.size();
        anchor = caret;
        goal_valid = false;
    }

    /// Delete the selection if there is one; otherwise the codepoint before
    /// the caret.
    void backspace() {
        if (delete_selection()) return;
        if (caret == 0) return;
        const std::size_t from = prev_boundary(caret);
        text.erase(from, caret - from);
        caret = from;
        anchor = caret;
        goal_valid = false;
    }

    /// Forward delete.
    void del() {
        if (delete_selection()) return;
        if (caret >= text.size()) return;
        const std::size_t to = next_boundary(caret);
        text.erase(caret, to - caret);
        anchor = caret;
        goal_valid = false;
    }

    void delete_word_left() {
        if (delete_selection()) return;
        const std::size_t from = word_left_of(caret);
        if (from == caret) return;
        text.erase(from, caret - from);
        caret = from;
        anchor = caret;
    }

    void delete_word_right() {
        if (delete_selection()) return;
        const std::size_t to = word_right_of(caret);
        if (to == caret) return;
        text.erase(caret, to - caret);
        anchor = caret;
    }

    /// Remove the selection. Returns true if anything was deleted.
    bool delete_selection() {
        if (!has_selection()) return false;
        const std::size_t s = selection_start(), e = selection_end();
        text.erase(s, e - s);
        caret = anchor = s;
        goal_valid = false;
        return true;
    }

    void set(std::string s) {
        text = std::move(s);
        caret = anchor = text.size();
        goal_valid = false;
    }
    void clear() { text.clear(); caret = anchor = 0; goal_valid = false; }

    // ── the standard key map ────────────────────────────────────────────

    /// Apply a key press using the platform's conventions.
    ///
    /// Having this in ONE place is the point: every text field in every mayag
    /// app gets word-motion, shift-selection, select-all and the right
    /// modifier for the platform, without each author reimplementing them
    /// (and getting Home/End or word-delete subtly wrong).
    ///
    /// Returns true when the key was consumed.
    bool handle_key(Key k, Mods mods) {
        const bool shift = mods.shift;
        const bool word  = mods.alt || (mods.ctrl && !mods.primary());

        switch (k) {
            case Key::left:   move(word ? Motion::word_left  : Motion::left,  shift); return true;
            case Key::right:  move(word ? Motion::word_right : Motion::right, shift); return true;
            case Key::up:     move(Motion::up,   shift); return true;
            case Key::down:   move(Motion::down, shift); return true;

            case Key::home:   move(mods.primary() ? Motion::doc_start : Motion::line_start, shift); return true;
            case Key::end:    move(mods.primary() ? Motion::doc_end   : Motion::line_end,   shift); return true;

            case Key::backspace:
                if (word) delete_word_left(); else backspace();
                return true;
            case Key::del:
                if (word) delete_word_right(); else del();
                return true;

            case Key::a:
                if (mods.primary()) { select_all(); return true; }
                return false;

            case Key::enter:
                if (multiline) { insert("\n"); return true; }
                return false;   // let the app treat Enter as submit

            default:
                return false;
        }
    }

  private:
    [[nodiscard]] static bool is_space(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }
    [[nodiscard]] static bool is_word(char c) noexcept {
        const auto u = static_cast<unsigned char>(c);
        return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
               (u >= 'a' && u <= 'z') || u == '_' || u >= 0x80;
    }
};

}  // namespace mayag
