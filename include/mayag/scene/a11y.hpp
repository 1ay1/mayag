#pragma once
// mayag::a11y — the semantic tree (see a11y_types.hpp for the vocabulary)

#include "a11y_types.hpp"
#include "node.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mayag::a11y {

/// One node in the semantic tree.
struct Element {
    Role          role = Role::none;
    std::string   label;        ///< what is announced
    std::string   description;  ///< optional longer text
    State         state;
    Rect          bounds;
    std::uint64_t id = 0;

    std::vector<Element> children;

    [[nodiscard]] bool is_interactive() const noexcept {
        switch (role) {
            case Role::button: case Role::link: case Role::checkbox:
            case Role::radio:  case Role::text_field: case Role::slider:
            case Role::tab:    case Role::menu_item:
                return true;
            default:
                return false;
        }
    }
};

/// A snapshot of a laid-out tree's meaning.
class Tree {
  public:
    Element root;

    /// Find the first element with this label. The primary way a TEST talks
    /// about a UI: `tree.find_by_label("Save")` says what the user sees,
    /// not what the tree happens to be shaped like.
    [[nodiscard]] const Element* find_by_label(std::string_view label) const {
        return search(root, [&](const Element& e) { return e.label == label; });
    }

    [[nodiscard]] const Element* find_by_role(Role r) const {
        return search(root, [&](const Element& e) { return e.role == r; });
    }

    [[nodiscard]] const Element* find_by_id(std::uint64_t id) const {
        return search(root, [&](const Element& e) { return e.id == id; });
    }

    /// Every element with a given role, in tree order.
    [[nodiscard]] std::vector<const Element*> all_with_role(Role r) const {
        std::vector<const Element*> out;
        collect(root, [&](const Element& e) { return e.role == r; }, out);
        return out;
    }

    /// Everything a keyboard user can reach, in order.
    [[nodiscard]] std::vector<const Element*> interactive() const {
        std::vector<const Element*> out;
        collect(root, [](const Element& e) { return e.is_interactive(); }, out);
        return out;
    }

    /// A readable dump. Useful in a failing test, and the closest thing to
    /// "what a screen reader would say" without a platform bridge.
    [[nodiscard]] std::string to_text() const {
        std::string out;
        write(root, 0, out);
        return out;
    }

  private:
    template <typename Pred>
    [[nodiscard]] static const Element* search(const Element& e, Pred p) {
        if (p(e)) return &e;
        for (const auto& c : e.children) {
            if (const Element* f = search(c, p)) return f;
        }
        return nullptr;
    }

    template <typename Pred>
    static void collect(const Element& e, Pred p, std::vector<const Element*>& out) {
        if (p(e)) out.push_back(&e);
        for (const auto& c : e.children) collect(c, p, out);
    }

    static void write(const Element& e, int depth, std::string& out) {
        if (e.role != Role::none || !e.label.empty()) {
            out.append(static_cast<std::size_t>(depth) * 2, ' ');
            out += role_name(e.role);
            if (!e.label.empty()) out += " \"" + e.label + "\"";
            if (e.state.checked)  out += " [checked]";
            if (e.state.selected) out += " [selected]";
            if (e.state.disabled) out += " [disabled]";
            if (e.state.focused)  out += " [focused]";
            if (e.state.has_value) {
                out += " value=" + std::to_string(static_cast<int>(e.state.value * 100)) + "%";
            }
            out += "\n";
        }
        for (const auto& c : e.children) {
            write(c, e.role != Role::none ? depth + 1 : depth, out);
        }
    }
};

namespace detail {

/// Derive a role for a node that was not explicitly annotated.
///
/// Automatic derivation is what keeps this from being a tax: a text node is
/// text, a named clickable box is a button. Only genuinely ambiguous nodes
/// need `| role(...)`, so an app that annotates nothing still produces a
/// mostly-correct tree.
[[nodiscard]] inline Role infer_role(const Node& n) {
    const Style& st = n.style();

    if (st.a11y_role != Role::none) return st.a11y_role;
    if (n.kind() == NodeKind::text) return Role::text;
    if (n.kind() == NodeKind::image) return Role::image;

    // A named node with children and no text of its own is a group; a named
    // leaf that paints is most likely a control.
    if (st.id != 0) {
        return n.children().empty() ? Role::button : Role::group;
    }
    return Role::none;
}

/// The text a node announces: its explicit label, else the concatenation of
/// its text descendants — which is what a user actually reads off a button.
inline void gather_text(const Node& n, std::string& out) {
    if (n.kind() == NodeKind::text && !n.text().empty()) {
        if (!out.empty()) out += " ";
        out += std::string{n.text()};
    }
    for (const auto& c : n.children()) gather_text(c, out);
}

inline Element build(const Node& n, std::uint64_t focused) {
    Element e;
    e.role   = infer_role(n);
    e.id     = n.style().id;
    e.bounds = n.frame();

    // A GROUP is a container; its meaning comes from its children, so it
    // does not gather their text into a label of its own. Doing so makes a
    // screen reader read the entire subtree before announcing anything
    // inside it.
    //
    // A CONTROL does gather: a button's text IS its label, which is what
    // lets `button<"Save">(t)` be announced correctly with no annotation.
    e.label = n.style().a11y_label;
    const bool is_container = (e.role == Role::group || e.role == Role::none ||
                               e.role == Role::list  || e.role == Role::dialog ||
                               e.role == Role::menu);
    if (e.label.empty() && !is_container) gather_text(n, e.label);
    e.description = n.style().a11y_description;

    e.state = n.style().a11y_state;
    e.state.focused = (n.style().id != 0 && n.style().id == focused);

    for (const auto& c : n.children()) {
        Element child = build(c, focused);

        // Drop a child that only restates this element's own label: a
        // screen reader saying "button Save, text Save" is noise.
        if (!is_container && child.children.empty() && !e.label.empty() &&
            child.label == e.label) {
            continue;
        }

        // Collapse anonymous wrappers so the semantic tree is SHALLOW. A
        // screen reader user should not walk through six nested layout boxes
        // to reach a label — the visual tree's shape is an implementation
        // detail, not meaning.
        if (child.role == Role::none) {
            for (auto& grandchild : child.children) e.children.push_back(std::move(grandchild));
        } else {
            e.children.push_back(std::move(child));
        }
    }

    return e;
}

}  // namespace detail

/// Build the semantic tree for a laid-out node tree.
[[nodiscard]] inline Tree snapshot(const Node& root, std::uint64_t focused = 0) {
    Tree t;
    t.root = detail::build(root, focused);
    if (t.root.role == Role::none) t.root.role = Role::group;
    return t;
}

}  // namespace mayag::a11y
