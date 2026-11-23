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
/// `| vars(...)`) and consumed by the cascade (scene/cascade.hpp), which
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
        n.canvas_data_ = std::shared_ptr<const void>(std::shared_ptr<const void>{}, user);   // not owned
        return n;
    }
    /// A canvas that owns what it draws (a minimap's runs): the payload lives
    /// as long as any copy of the node, so a retained or memoised tree can
    /// repaint it without anything dangling. One allocation for the whole
    /// payload instead of a node per mark.
    [[nodiscard]] static Node make_canvas(CanvasFn fn, std::shared_ptr<const void> data, Style s) {
        Node n{NodeKind::canvas};
        n.style_ = std::move(s);
        n.canvas_ = fn;
        n.canvas_data_ = std::move(data);
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
    /// False while the node still uses the shared default style (no block).
    [[nodiscard]] bool has_own_style() const noexcept { return style_.allocated(); }
    /// Take a boxed style as-is (no copy): how the DSL hands a node its style.
    void adopt_style(StyleBox&& s) noexcept { style_ = std::move(s); }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] std::uint32_t texture() const noexcept { return texture_; }
    [[nodiscard]] CanvasFn canvas() const noexcept { return canvas_; }
    [[nodiscard]] const void* canvas_user() const noexcept { return canvas_data_.get(); }
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

    /// Layout's per-pass measure memo (layout/flex.hpp). Two entries, stamped
    /// with the pass that wrote them. Not part of a node's value: copying or
    /// moving a node starts it with an empty memo, so stamps only ever
    /// describe the node object (and subtree) they were computed on.
    struct MeasureMemo {
        std::uint32_t gen[2]{};
        float         cw[2]{}, ch[2]{};
        const void*   tm[2]{};
        Vec2          out[2]{};
        std::uint8_t  next = 0;
        std::uint32_t passes = 0;   ///< on a root: layout passes run on this tree
        MeasureMemo() noexcept = default;
        MeasureMemo(const MeasureMemo&) noexcept {}
        MeasureMemo& operator=(const MeasureMemo&) noexcept { gen[0] = gen[1] = 0; return *this; }
        friend constexpr bool operator==(const MeasureMemo&, const MeasureMemo&) noexcept { return true; }
    };
    [[nodiscard]] MeasureMemo& measure_memo() const noexcept { return memo_; }
    void forget_measure() const noexcept { memo_.gen[0] = memo_.gen[1] = 0; }
    /// A fresh, nonzero stamp for a layout pass over the tree rooted here.
    [[nodiscard]] std::uint32_t next_layout_pass() noexcept {
        if (++memo_.passes == 0) ++memo_.passes;
        return memo_.passes;
    }

    // -- cascade ------------------------------------------------------------

    [[nodiscard]] const std::shared_ptr<const Cascade>& cascade() const noexcept { return cascade_; }
    void set_cascade(std::shared_ptr<const Cascade> c) noexcept { cascade_ = std::move(c); }

    /// True once scene/cascade.hpp has resolved this tree (inheritance,
    /// variants, tokens). Set on the ROOT only; resolving twice would pipe
    /// every variant on twice, so everything that resolves checks it.
    [[nodiscard]] bool cascaded() const noexcept { return cascaded_; }
    void set_cascaded(bool on) noexcept { cascaded_ = on; }

    // -- memo ---------------------------------------------------------------

    /// Nonzero on the root of a `memo(key, ...)` subtree: the key. The host
    /// caches such subtrees cascaded and laid out, and reuses them on a hit.
    [[nodiscard]] std::uint64_t memo_key() const noexcept { return memo_state_.key; }
    void set_memo_key(std::uint64_t k) noexcept { memo_state_.key = k; }
    /// True while this memo subtree is last frame's, already resolved: the
    /// cascade, hit-id and transition passes leave it alone.
    [[nodiscard]] bool memo_reused() const noexcept { return memo_state_.reused; }
    void set_memo_reused(bool on) noexcept { memo_state_.reused = on; }
    /// A reused subtree's post-cascade, pre-layout copy (owned by the cache),
    /// for relaying it out at a new size.
    [[nodiscard]] const Node* memo_cascaded() const noexcept { return memo_state_.cascaded; }
    void set_memo_cascaded(const Node* n) noexcept { memo_state_.cascaded = n; }
    /// Returned by memo() (cached or not): styles piped onto it go on a box
    /// around it, so the tree is the same with or without the cache.
    [[nodiscard]] bool memo_result() const noexcept { return memo_state_.result; }
    void set_memo_result(bool on) noexcept { memo_state_.result = on; }
    /// The hit-id path this memo root was given this frame.
    [[nodiscard]] std::uint64_t memo_path() const noexcept { return memo_state_.path; }
    void set_memo_path(std::uint64_t p) noexcept { memo_state_.path = p; }

    /// A memo root's measured sizes, kept across frames with the subtree
    /// (unlike MeasureMemo, which is per pass).
    struct MemoSize { float cw = 0, ch = 0; Vec2 out{}; };
    [[nodiscard]] const MemoSize* memo_size(float cw, float ch) const noexcept {
        for (std::uint8_t i = 0; i < memo_state_.n; ++i)
            if (memo_state_.sizes[i].cw == cw && memo_state_.sizes[i].ch == ch) return &memo_state_.sizes[i];
        return nullptr;
    }
    void add_memo_size(float cw, float ch, Vec2 out) const noexcept {
        // Parents measure a child under a few constraints (intrinsic, then
        // the space it gets); keep four, replacing round robin.
        memo_state_.sizes[memo_state_.next] = {cw, ch, out};
        memo_state_.next = static_cast<std::uint8_t>((memo_state_.next + 1) % 4);
        if (memo_state_.n < 4) ++memo_state_.n;
    }

    /// Shift this subtree's frames by `d` (a reused subtree whose parent put
    /// it somewhere else at the same size).
    void translate(Vec2 d) noexcept {
        frame_.origin = frame_.origin + d;
        for (auto& c : children_) c.translate(d);
    }

  private:
    NodeKind          kind_ = NodeKind::box;
    StyleBox          style_{};
    std::string       text_{};
    std::uint32_t     texture_ = 0;
    CanvasFn          canvas_ = nullptr;
    std::shared_ptr<const void> canvas_data_{};
    std::vector<Node> children_{};
    Rect              frame_{};
    std::shared_ptr<const Cascade> cascade_{};
    bool              cascaded_ = false;
    // memo bookkeeping; not part of a node's value
    struct MemoState {
        std::uint64_t key = 0;
        std::uint64_t path = 0;
        bool          reused = false;
        bool          result = false;
        const Node*   cascaded = nullptr;
        mutable MemoSize      sizes[4]{};
        mutable std::uint8_t  n = 0, next = 0;
        friend constexpr bool operator==(const MemoState&, const MemoState&) noexcept { return true; }
    } memo_state_{};
    mutable MeasureMemo memo_{};
};

}  // namespace mayag
