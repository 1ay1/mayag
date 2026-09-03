#pragma once
// mayag::a11y — the semantic tree
//
// A UI that only exists as pixels is unusable for anyone relying on a screen
// reader, and untestable without comparing images. Both problems have the
// same solution: alongside the visual tree, expose what each node MEANS.
//
//     button<"Save">(t) | role(Role::button) | label("Save document")
//
// The result is queryable:
//
//     auto tree = a11y::snapshot(root);
//     tree.find_by_label("Save document");     // in a test
//     tree.to_text();                          // a readable dump
//
// mayag derives most of this automatically — a text node is `Role::text` with
// its own string as the label, a node with a click subscription is a button —
// so the common case costs nothing and only ambiguous cases need annotating.
//
// This is deliberately a PLAIN TREE rather than a platform bridge. Feeding it
// to NSAccessibility / UIA / AT-SPI is a per-platform adapter that reads this
// structure; keeping the structure independent means the semantics are
// testable on any machine, including CI with no accessibility stack at all.

#include "../core/geometry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::a11y {

/// What a node is, semantically. Deliberately small: these are the roles that
/// change how a screen reader announces something, not an exhaustive taxonomy.
enum class Role : std::uint8_t {
    none,        ///< purely decorative; skipped by assistive tech
    group,       ///< a container with no meaning of its own
    text,        ///< static text
    heading,
    button,
    link,
    checkbox,
    radio,
    text_field,
    slider,
    list,
    list_item,
    tab,
    tab_panel,
    dialog,
    menu,
    menu_item,
    progress,
    image,
    separator,
};

[[nodiscard]] constexpr std::string_view role_name(Role r) noexcept {
    switch (r) {
        case Role::none:       return "none";
        case Role::group:      return "group";
        case Role::text:       return "text";
        case Role::heading:    return "heading";
        case Role::button:     return "button";
        case Role::link:       return "link";
        case Role::checkbox:   return "checkbox";
        case Role::radio:      return "radio";
        case Role::text_field: return "textfield";
        case Role::slider:     return "slider";
        case Role::list:       return "list";
        case Role::list_item:  return "listitem";
        case Role::tab:        return "tab";
        case Role::tab_panel:  return "tabpanel";
        case Role::dialog:     return "dialog";
        case Role::menu:       return "menu";
        case Role::menu_item:  return "menuitem";
        case Role::progress:   return "progress";
        case Role::image:      return "image";
        case Role::separator:  return "separator";
    }
    return "unknown";
}

/// Semantic state a screen reader announces alongside the role.
struct State {
    bool selected = false;
    bool checked  = false;
    bool disabled = false;
    bool expanded = false;
    bool focused  = false;
    /// For sliders and progress bars.
    float value = 0.0f;
    bool  has_value = false;

    friend constexpr bool operator==(const State&, const State&) = default;
};

}  // namespace mayag::a11y
