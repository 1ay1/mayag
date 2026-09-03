#pragma once
// mayag::Node — the runtime scene tree
//
// The DSL produces these. They are a plain value type: copyable, movable,
// comparable, with no back-pointers and no shared mutable state. A view
// function returns a Node by value, the runtime diffs it against last frame's
// Node, and only the differences reach the GPU.

#include "../style/style.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mayag {

enum class NodeKind : std::uint8_t {
    box,      ///< a rounded rect: fill, stroke, shadows, children
    text,     ///< a glyph run
    image,    ///< a sampled texture
    canvas,   ///< user-supplied draw callback (escape hatch)
    spacer,   ///< invisible flex filler
};

class Node;

/// Immediate-mode escape hatch. When the declarative model does not fit
/// (a plot, a custom SDF, a video frame), you get the resolved rect and the
/// draw list, and you emit whatever you want.
class DrawList;
using CanvasFn = void (*)(DrawList&, const Rect&, const void* user_data);

class Node {
  public:
    Node() = default;

    explicit Node(NodeKind k) noexcept : kind_{k} {}

    // -- construction helpers used by the DSL -----------------------------

    [[nodiscard]] static Node make_box(Style s, std::vector<Node> kids = {}) {
        Node n{NodeKind::box};
        n.style_ = s;
        n.children_ = std::move(kids);
        return n;
    }

    [[nodiscard]] static Node make_text(std::string content, Style s) {
        Node n{NodeKind::text};
        n.style_ = s;
        n.text_ = std::move(content);
        return n;
    }

    [[nodiscard]] static Node make_image(std::uint32_t texture, Style s) {
        Node n{NodeKind::image};
        n.style_ = s;
        n.texture_ = texture;
        return n;
    }

    [[nodiscard]] static Node make_canvas(CanvasFn fn, const void* user, Style s) {
        Node n{NodeKind::canvas};
        n.style_ = s;
        n.canvas_ = fn;
        n.canvas_user_ = user;
        return n;
    }

    [[nodiscard]] static Node make_spacer(float grow = 1.0f) {
        Node n{NodeKind::spacer};
        n.style_.layout.grow = grow;
        return n;
    }

    // -- accessors --------------------------------------------------------

    [[nodiscard]] NodeKind kind() const noexcept { return kind_; }
    [[nodiscard]] const Style& style() const noexcept { return style_; }
    [[nodiscard]] Style& style() noexcept { return style_; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] std::uint32_t texture() const noexcept { return texture_; }
    [[nodiscard]] CanvasFn canvas() const noexcept { return canvas_; }
    [[nodiscard]] const void* canvas_user() const noexcept { return canvas_user_; }
    [[nodiscard]] const std::vector<Node>& children() const noexcept { return children_; }
    [[nodiscard]] std::vector<Node>& children() noexcept { return children_; }

    /// Resolved layout rect, filled in by the layout pass. Absolute, in
    /// logical (pre-DPI-scale) pixels.
    [[nodiscard]] const Rect& frame() const noexcept { return frame_; }
    void set_frame(const Rect& r) noexcept { frame_ = r; }

    [[nodiscard]] std::uint64_t id() const noexcept { return style_.id; }

    void add_child(Node c) { children_.push_back(std::move(c)); }

    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t n = 1;
        for (const auto& c : children_) n += c.count();
        return n;
    }

    /// Deepest node containing `p`, searched front-to-back so the visually
    /// topmost sibling wins — the same ordering the painter uses.
    [[nodiscard]] const Node* hit_test(Vec2 p) const noexcept {
        if (!frame_.contains(p)) return nullptr;
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            if (const Node* hit = it->hit_test(p)) return hit;
        }
        return this;
    }

    [[nodiscard]] const Node* find(std::uint64_t node_id) const noexcept {
        if (node_id != 0 && style_.id == node_id) return this;
        for (const auto& c : children_) {
            if (const Node* f = c.find(node_id)) return f;
        }
        return nullptr;
    }

    friend bool operator==(const Node&, const Node&) = default;

  private:
    NodeKind          kind_ = NodeKind::box;
    Style             style_{};
    std::string       text_{};
    std::uint32_t     texture_ = 0;
    CanvasFn          canvas_ = nullptr;
    const void*       canvas_user_ = nullptr;
    std::vector<Node> children_{};
    Rect              frame_{};
};

}  // namespace mayag
