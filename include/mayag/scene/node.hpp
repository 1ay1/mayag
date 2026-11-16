#pragma once
// mayag::Node — the runtime scene tree
//
// The DSL produces these. They are a plain value type: copyable, movable,
// comparable, with no back-pointers and no shared mutable state. A view
// function returns a Node by value, the runtime diffs it against last frame's
// Node, and only the differences reach the GPU.

#include "../style/state.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mayag {

/// What happened to a node, for inline handlers (`| on_click(Save{})`).
enum class On : std::uint8_t { click, press, release, drag, scroll, enter, leave };

/// A message a handler produced, type-erased: the host turns it into the
/// program's Msg by matching `type` against the Msg variant's alternatives.
struct HandlerResult {
    std::shared_ptr<const void> msg{};
    const void*                 type = nullptr;   ///< &type_tag<T>
};

/// One address per type, identical across translation units.
template <class T>
inline constexpr char type_tag = 0;

/// How a node's style RESPONDS: its state variants and variable bindings.
///
/// Produced by the DSL (`| hovered(...)`, `| when(state::selected, ...)`,
/// `| vars(...)`) and consumed by the cascade (style/cascade.hpp), which
/// pipes every matching rule onto the node's base style in priority order
/// before layout. Immutable and shared, so copying a Node never copies it.
struct Cascade {
    struct Rule {
        state::Sel sel{};
        int     priority = 0;   ///< state::priority(sel); rules are kept sorted by it
        int     order    = 0;   ///< declaration order, the tie-break
        void  (*apply)(Style&, const void*) = nullptr;
        const void* data = nullptr;   ///< the variant's modifiers, owned by `keep`
    };
    std::vector<Rule>          rules;
    std::vector<var::TokenDef> vars;
    std::vector<std::shared_ptr<const void>> keep;   ///< owners of every Rule::data
    /// `| theme(t)`: this subtree resolves every token -- colour, number,
    /// motion, widget looks -- against `theme` instead of the frame's.
    std::shared_ptr<const Theme> theme{};
    /// Inline event handlers, run by the host when the gesture lands here.
    struct Handler {
        On on = On::click;
        HandlerResult (*call)(const void* fn, const void* event) = nullptr;
        const void* fn = nullptr;   ///< owned by `keep`
    };
    std::vector<Handler> handlers;
    state::Mask uses = 0;        ///< states some rule asks of THIS node
    state::Mask group_uses = 0;  ///< states some rule asks of its nearest group

    /// Insert keeping (priority, order) ascending, so the cascade can pipe
    /// matching rules in a single forward pass.
    void add(Rule r) {
        r.order = static_cast<int>(rules.size());
        uses |= r.sel.need | r.sel.deny;
        group_uses |= r.sel.gneed | r.sel.gdeny;
        auto at = std::upper_bound(rules.begin(), rules.end(), r, [](const Rule& a, const Rule& b) {
            return a.priority < b.priority;
        });
        rules.insert(at, r);
    }
};

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
        n.style_ = std::move(s);
        n.children_ = std::move(kids);
        return n;
    }

    [[nodiscard]] static Node make_text(std::string content, Style s) {
        Node n{NodeKind::text};
        n.style_ = std::move(s);
        n.text_ = std::move(content);
        return n;
    }

    [[nodiscard]] static Node make_image(std::uint32_t texture, Style s) {
        Node n{NodeKind::image};
        n.style_ = std::move(s);
        n.texture_ = texture;
        return n;
    }

    [[nodiscard]] static Node make_canvas(CanvasFn fn, const void* user, Style s) {
        Node n{NodeKind::canvas};
        n.style_ = std::move(s);
        n.canvas_ = fn;
        n.canvas_user_ = user;
        return n;
    }

    [[nodiscard]] static Node make_spacer(float grow = 1.0f) {
        Node n{NodeKind::spacer};
        n.style_.mut().layout.grow = grow;
        return n;
    }

    // -- accessors --------------------------------------------------------

    [[nodiscard]] NodeKind kind() const noexcept { return kind_; }
    [[nodiscard]] const Style& style() const noexcept { return style_.get(); }
    [[nodiscard]] Style& style() { return style_.mut(); }
    /// Take a boxed style as-is (no copy): how the DSL hands a node its style.
    void adopt_style(StyleBox&& s) noexcept { style_ = std::move(s); }
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

    [[nodiscard]] std::uint64_t id() const noexcept { return style_.get().id; }

    void add_child(Node c) { children_.push_back(std::move(c)); }
    void set_text(std::string s) { text_ = std::move(s); }
    void set_texture(std::uint32_t t) noexcept { texture_ = t; }

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
        if (node_id != 0 && style_.get().id == node_id) return this;
        for (const auto& c : children_) {
            if (const Node* f = c.find(node_id)) return f;
        }
        return nullptr;
    }

    friend bool operator==(const Node&, const Node&) = default;

    // -- cascade ------------------------------------------------------------

    [[nodiscard]] const std::shared_ptr<const Cascade>& cascade() const noexcept { return cascade_; }
    void set_cascade(std::shared_ptr<const Cascade> c) noexcept { cascade_ = std::move(c); }

    /// True once style/cascade.hpp has resolved this tree (inheritance,
    /// variants, tokens). Set on the ROOT only; resolving twice would pipe
    /// every variant on twice, so everything that resolves checks it.
    [[nodiscard]] bool cascaded() const noexcept { return cascaded_; }
    void set_cascaded(bool on) noexcept { cascaded_ = on; }

  private:
    NodeKind          kind_ = NodeKind::box;
    StyleBox          style_{};
    std::string       text_{};
    std::uint32_t     texture_ = 0;
    CanvasFn          canvas_ = nullptr;
    const void*       canvas_user_ = nullptr;
    std::vector<Node> children_{};
    Rect              frame_{};
    std::shared_ptr<const Cascade> cascade_{};
    bool              cascaded_ = false;
};

}  // namespace mayag
