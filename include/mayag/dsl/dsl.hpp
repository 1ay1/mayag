#pragma once
// mayag::dsl — the type-state widget language
//
// The premise: a widget's *shape* is known at compile time, so the compiler
// should be the one that catches nonsense. Setting a border colour on a box
// with no border, putting a gap on something that has no children, giving a
// font weight to an image, or asking an absolutely-positioned node to flex —
// these are not runtime no-ops in mayag. They do not compile.
//
// Every element carries a CAPABILITY SET in its type. Modifiers declare what
// they need, what they forbid, and what they grant. The pipe operator is the
// proof checker.
//
//     constexpr auto ui =
//         v(text<"Ready">   | font(14) | fg(colors::white),
//           h(text<"Run">, spacer(), text<"⌘R">) | gap(8))
//         | bg(colors::slate)
//         | border(1, colors::white)
//         | radius(12)
//         | pad(16);
//
//     Node scene = ui.build();
//
// `ui` above is a `constexpr` object: the entire style tree is folded into
// static data at compile time, and `build()` is the only thing that touches
// the heap.

#include "../scene/node.hpp"
#include "../style/style.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mayag::dsl {

// ════════════════════════════════════════════════════════════════════════
// Capabilities
// ════════════════════════════════════════════════════════════════════════

namespace caps {
using Set = std::uint32_t;

inline constexpr Set none        = 0;
inline constexpr Set container   = 1u << 0;   ///< holds children; flex modifiers apply
inline constexpr Set text        = 1u << 1;   ///< renders glyphs; font modifiers apply
inline constexpr Set image       = 1u << 2;
inline constexpr Set fill        = 1u << 3;   ///< a fill has been established
inline constexpr Set gradient    = 1u << 4;   ///< that fill is a gradient
inline constexpr Set stroke      = 1u << 5;   ///< a border has been established
inline constexpr Set shadow      = 1u << 6;
inline constexpr Set positioned  = 1u << 7;   ///< absolute/fixed: out of flow
inline constexpr Set flexible    = 1u << 8;   ///< grow/shrink set: in flow
inline constexpr Set sized_x     = 1u << 12;  ///< explicit width
inline constexpr Set sized_y     = 1u << 13;  ///< explicit height
inline constexpr Set clipped     = 1u << 9;
inline constexpr Set named       = 1u << 10;  ///< has a stable id
inline constexpr Set interactive = 1u << 11;

/// Human-readable name for a single bit, used in diagnostics.
constexpr std::string_view name_of(Set bit) noexcept {
    switch (bit) {
        case container:   return "a container (built with v/h/z)";
        case text:        return "a text element";
        case image:       return "an image element";
        case fill:        return "a fill (add `| bg(...)` first)";
        case gradient:    return "a gradient fill (add `| linear_gradient(...)` first)";
        case stroke:      return "a border (add `| border(width, color)` first)";
        case shadow:      return "a shadow (add `| shadow(...)` first)";
        case positioned:  return "absolute or fixed positioning";
        case flexible:    return "flex sizing (grow/shrink)";
        case clipped:     return "clipping enabled";
        case named:       return "a name (add `| id(...)` first)";
        case interactive: return "interactivity";
        case sized_x:     return "an explicit width";
        case sized_y:     return "an explicit height";
        default:          return "an unknown capability";
    }
}

constexpr Set first_missing(Set needed, Set have) noexcept {
    const Set missing = needed & ~have;
    return missing & (~missing + 1u);   // lowest set bit
}
}  // namespace caps

// ════════════════════════════════════════════════════════════════════════
// Compile-time strings & diagnostics
// ════════════════════════════════════════════════════════════════════════

template <std::size_t N>
struct fixed_string {
    std::array<char, N> chars{};

    consteval fixed_string(const char (&s)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) chars[i] = s[i];
    }

    [[nodiscard]] constexpr const char* data() const noexcept { return chars.data(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return N - 1; }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {chars.data(), N - 1}; }

    friend constexpr bool operator==(const fixed_string&, const fixed_string&) = default;
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

/// A C++26 user-generated `static_assert` message (P2741). The point is that
/// a misuse of the DSL reports *what you did wrong and how to fix it*, instead
/// of 200 lines of substitution failure.
struct Diagnostic {
    std::array<char, 512> buf{};
    std::size_t len = 0;

    constexpr void put(std::string_view s) noexcept {
        for (char ch : s) { if (len + 1 < buf.size()) buf[len++] = ch; }
    }
    [[nodiscard]] constexpr const char* data() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return len; }
};

constexpr Diagnostic missing_capability(std::string_view mod, caps::Set missing) noexcept {
    Diagnostic d;
    d.put("mayag: `| ");
    d.put(mod);
    d.put("` is not valid here — it requires ");
    d.put(caps::name_of(missing));
    d.put(".");
    return d;
}

constexpr Diagnostic forbidden_capability(std::string_view mod, caps::Set present) noexcept {
    Diagnostic d;
    d.put("mayag: `| ");
    d.put(mod);
    d.put("` conflicts with ");
    d.put(caps::name_of(present));
    d.put(" which is already set on this element.");
    return d;
}

// ════════════════════════════════════════════════════════════════════════
// Modifier protocol
// ════════════════════════════════════════════════════════════════════════

/// A modifier is any literal type that declares its capability contract and
/// knows how to mutate a Style. Users can write their own — nothing here is
/// privileged.
template <typename M>
concept Modifier = requires(const M m, Style& s) {
    { M::needs }  -> std::convertible_to<caps::Set>;
    { M::gives }  -> std::convertible_to<caps::Set>;
    { M::bans }   -> std::convertible_to<caps::Set>;
    { M::label }  -> std::convertible_to<std::string_view>;
    m.apply(s);
};

/// Boilerplate eliminator for the common case.
#define MAYAG_MODIFIER(Name, Needs, Gives, Bans, Label)          \
    static constexpr caps::Set needs = (Needs);                  \
    static constexpr caps::Set gives = (Gives);                  \
    static constexpr caps::Set bans  = (Bans);                   \
    static constexpr std::string_view label = (Label)

// ════════════════════════════════════════════════════════════════════════
// Elem — the compile-time element
// ════════════════════════════════════════════════════════════════════════

template <NodeKind Kind, caps::Set Caps, typename... Kids>
struct Elem {
    static constexpr NodeKind  kind = Kind;
    static constexpr caps::Set capabilities = Caps;

    Style                 style{};
    std::string_view      content{};   ///< static text / image key
    std::uint32_t         texture = 0;
    std::tuple<Kids...>   kids{};

    /// Materialise the runtime scene node. This is the only allocation.
    [[nodiscard]] Node build() const {
        Node n{Kind};
        n.style() = style;

        if constexpr (Kind == NodeKind::text) {
            n = Node::make_text(std::string{content}, style);
        } else if constexpr (Kind == NodeKind::image) {
            n = Node::make_image(texture, style);
        }

        if constexpr (sizeof...(Kids) > 0) {
            n.children().reserve(sizeof...(Kids));
            std::apply([&](const auto&... k) { (n.add_child(k.build()), ...); }, kids);
        }
        return n;
    }

    /// Implicit conversion so an Elem can be returned where a Node is wanted.
    operator Node() const { return build(); }
};

template <typename T>
concept Element = requires { T::kind; T::capabilities; } &&
                  std::same_as<std::remove_cvref_t<decltype(T::kind)>, NodeKind>;

// ── the pipe: capability proof + style application ──────────────────────

template <NodeKind K, caps::Set C, typename... Kids, Modifier M>
[[nodiscard]] constexpr auto operator|(Elem<K, C, Kids...> e, M m) {
    static_assert((M::needs & ~C) == 0,
                  missing_capability(M::label, caps::first_missing(M::needs, C)));
    static_assert((M::bans & C) == 0,
                  forbidden_capability(M::label, M::bans & C));

    Elem<K, C | M::gives, Kids...> out{e.style, e.content, e.texture, e.kids};
    m.apply(out.style);
    return out;
}

// ════════════════════════════════════════════════════════════════════════
// Element constructors
// ════════════════════════════════════════════════════════════════════════

namespace detail {

template <Axis A, typename... Kids>
[[nodiscard]] constexpr auto make_stack(Kids... kids) {
    Elem<NodeKind::box, caps::container, Kids...> e{};
    e.style.layout.axis = A;
    e.kids = std::tuple<Kids...>{kids...};
    return e;
}

}  // namespace detail

/// Vertical stack.
template <Element... Kids>
[[nodiscard]] constexpr auto v(Kids... kids) {
    return detail::make_stack<Axis::vertical>(kids...);
}

/// Horizontal stack.
template <Element... Kids>
[[nodiscard]] constexpr auto h(Kids... kids) {
    return detail::make_stack<Axis::horizontal>(kids...);
}

/// Z stack — children overlap, all pinned to the parent box.
///
/// The FIRST child stays in flow and therefore SIZES the stack; the rest are
/// absolutely positioned over it. That asymmetry is deliberate and is what
/// makes `z()` safe to use without an explicit size:
///
///     z(content, badge_over_it)   // sizes to `content`
///
/// Making every child absolute (as this once did) means the stack has nothing
/// to size from, collapses, and — since z-stacks are usually clipped — crops
/// its own contents. That is precisely how mayag's own text field ended up
/// 19.6 px tall around 22.4 px of text.
///
/// Put the thing that defines the size first. If you genuinely want every
/// child floating, give the stack an explicit size.
template <Element... Kids>
[[nodiscard]] constexpr auto z(Kids... kids) {
    auto e = detail::make_stack<Axis::vertical>(kids...);
    e.style.layout.align = Align::stretch;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((I == 0 ? void()
                 : void(std::get<I>(e.kids).style.layout.position = Positioning::absolute)), ...);
    }(std::index_sequence_for<Kids...>{});

    return e;
}

/// An empty, styleable box. The building block for dividers, swatches, bars.
[[nodiscard]] constexpr auto box() {
    return Elem<NodeKind::box, caps::container>{};
}

/// Compile-time text: `text<"Hello">`. The string lives in static storage;
/// no allocation happens until `build()`.
template <fixed_string S>
inline constexpr auto text = Elem<NodeKind::text, caps::text>{ .content = S.view() };

/// Runtime text that OUTLIVES the element.
///
/// Stores a `string_view`, which is free when the caller already owns the
/// bytes: a string literal, a `const char*`, a member `std::string`, a span
/// of a buffer.
///
/// A view onto a TEMPORARY string is rejected at compile time. Such an
/// element outlives its bytes, and the symptom is not a crash but garbage
/// rendered as glyphs — mayag shipped exactly that:
/// `text_of(std::to_string(x) + "%")` produced a node holding three NUL
/// bytes, and it only surfaced when the layout auditor flagged the resulting
/// box as too narrow to wrap. Use `text_owned()` for strings built on the fly.
template <typename S>
[[nodiscard]] inline auto text_of(S&& s) {
    static_assert(!(std::is_rvalue_reference_v<S&&> &&
                    std::same_as<std::remove_cvref_t<S>, std::string>),
        "mayag: text_of() stores a VIEW, and this string dies at the end of "
        "the statement, so the text would dangle. Use text_owned() for a "
        "string built on the fly.");
    return Elem<NodeKind::text, caps::text>{ .content = std::string_view{s} };
}

/// Runtime text built on the fly, e.g. `text_owned(std::to_string(n) + "%")`.
///
/// Copies. An element that borrowed from a temporary would dangle the instant
/// the full expression ended, and the symptom is the worst kind: not a crash
/// but garbage bytes rendered as a glyph. mayag shipped exactly that —
/// `text_of(std::to_string(x) + "%")` in the gallery produced a text node
/// holding three NUL bytes, which the layout auditor eventually caught as a
/// box too narrow to wrap.
struct OwnedText {
    static constexpr NodeKind  kind = NodeKind::text;
    static constexpr caps::Set capabilities = caps::text;

    Style            style{};
    std::string_view content{};   ///< unused; the owned string below is the source
    std::uint32_t    texture = 0;
    std::tuple<>     kids{};

    std::string owned;

    [[nodiscard]] Node build() const { return Node::make_text(owned, style); }
    operator Node() const { return build(); }
};

[[nodiscard]] inline OwnedText text_owned(std::string s) {
    OwnedText t{};
    t.owned = std::move(s);
    return t;
}

/// OwnedText participates in the modifier pipeline exactly like `text<>`,
/// with the same capability checks — it is a text element that happens to
/// carry its own bytes.
template <Modifier M>
[[nodiscard]] inline OwnedText operator|(OwnedText e, M m) {
    static_assert((M::needs & ~OwnedText::capabilities) == 0,
                  missing_capability(M::label,
                      caps::first_missing(M::needs, OwnedText::capabilities)));
    static_assert((M::bans & OwnedText::capabilities) == 0,
                  forbidden_capability(M::label, M::bans & OwnedText::capabilities));
    m.apply(e.style);
    return e;
}

/// Flexible empty space — the idiomatic way to push siblings apart.
[[nodiscard]] constexpr auto spacer(float grow = 1.0f) {
    Elem<NodeKind::box, caps::flexible> e{};
    e.style.layout.grow = grow;
    return e;
}

/// Fixed-size gap as an element (when `gap()` on the parent is too uniform).
[[nodiscard]] constexpr auto strut(float size) {
    Elem<NodeKind::box, caps::none> e{};
    e.style.layout.width  = px(size);
    e.style.layout.height = px(size);
    return e;
}

[[nodiscard]] constexpr auto image(std::uint32_t texture_id) {
    Elem<NodeKind::image, caps::image> e{};
    e.texture = texture_id;
    return e;
}

/// Wrap an already-built runtime `Node` so it can sit inside a DSL tree.
///
/// The DSL is compile-time, but real UIs have runtime-sized lists: table
/// rows, search results, a chip per theme. `node()` is the seam — build the
/// children however you like, then drop the result into `v(...)` / `h(...)`
/// alongside static elements. Capabilities are `container`, because that is
/// what a materialised subtree behaves like.
struct NodeElem {
    static constexpr NodeKind  kind = NodeKind::box;
    static constexpr caps::Set capabilities = caps::container;

    Style            style{};
    std::string_view content{};
    std::uint32_t    texture = 0;
    std::tuple<>     kids{};

    Node payload{};

    [[nodiscard]] Node build() const {
        Node n = payload;
        // Style piped onto the wrapper wins over whatever the node carried,
        // so `node(x) | pad(8)` behaves like any other element. The payload's
        // own children and layout axis survive, because those came from how
        // the subtree was CONSTRUCTED, not from the pipeline.
        if (style != Style{}) {
            const Axis axis = n.style().layout.axis;
            const float gap = n.style().layout.gap;
            const std::uint64_t keep_id = n.style().id != 0 ? n.style().id : style.id;
            n.style() = style;
            n.style().layout.axis = axis;
            if (n.style().layout.gap == 0.0f) n.style().layout.gap = gap;
            n.style().id = keep_id;
        }
        return n;
    }

    operator Node() const { return build(); }
};

/// NodeElem participates in the modifier pipeline like any other element.
///
/// Without this, a runtime-built subtree could not be styled or sized, which
/// defeats the point of having an escape hatch: real UIs need `list(...) |
/// scroll(state) | size(...)`.
template <Modifier M>
[[nodiscard]] inline NodeElem operator|(NodeElem e, M m) {
    static_assert((M::needs & ~NodeElem::capabilities) == 0,
                  missing_capability(M::label,
                      caps::first_missing(M::needs, NodeElem::capabilities)));
    static_assert((M::bans & NodeElem::capabilities) == 0,
                  forbidden_capability(M::label, M::bans & NodeElem::capabilities));
    m.apply(e.style);
    return e;
}

/// Lift a runtime `Node` into the DSL.
[[nodiscard]] inline auto node(Node n) {
    NodeElem e{};
    e.payload = std::move(n);
    return e;
}

/// Build a container from a runtime-sized list of children.
/// `list(Axis::horizontal, rows)` is the idiomatic "one row per record".
[[nodiscard]] inline auto list(Axis axis, std::vector<Node> children, float gap_px = 0.0f) {
    Node n{NodeKind::box};
    n.style().layout.axis = axis;
    n.style().layout.gap  = gap_px;
    n.children() = std::move(children);
    return node(std::move(n));
}

// ═══════════════════════════════════════════════════════════════════════════
// Modifiers — layout
// ═══════════════════════════════════════════════════════════════════════════

struct Pad {
    MAYAG_MODIFIER(Pad, caps::none, caps::none, caps::none, "pad");
    Insets v;
    constexpr void apply(Style& s) const { s.layout.padding = v; }
};
[[nodiscard]] constexpr Pad pad(float all) { return {Insets{all}}; }
[[nodiscard]] constexpr Pad pad(float vertical, float horizontal) { return {Insets{vertical, horizontal}}; }
[[nodiscard]] constexpr Pad pad(float t, float r, float b, float l) { return {Insets{t, r, b, l}}; }

struct Margin {
    MAYAG_MODIFIER(Margin, caps::none, caps::none, caps::none, "margin");
    Insets v;
    constexpr void apply(Style& s) const { s.layout.margin = v; }
};
[[nodiscard]] constexpr Margin margin(float all) { return {Insets{all}}; }
[[nodiscard]] constexpr Margin margin(float vertical, float horizontal) { return {Insets{vertical, horizontal}}; }
[[nodiscard]] constexpr Margin margin(float t, float r, float b, float l) { return {Insets{t, r, b, l}}; }

/// Spacing between children. Meaningless without children — hence the
/// `container` requirement, which turns a silent no-op into a compile error.
struct Gap {
    MAYAG_MODIFIER(Gap, caps::container, caps::none, caps::none, "gap");
    float v;
    constexpr void apply(Style& s) const { s.layout.gap = v; }
};
[[nodiscard]] constexpr Gap gap(float v) { return {v}; }

struct JustifyMod {
    MAYAG_MODIFIER(JustifyMod, caps::container, caps::none, caps::none, "justify");
    Justify v;
    constexpr void apply(Style& s) const { s.layout.justify = v; }
};
[[nodiscard]] constexpr JustifyMod justify(Justify j) { return {j}; }

struct AlignMod {
    MAYAG_MODIFIER(AlignMod, caps::container, caps::none, caps::none, "align");
    Align v;
    constexpr void apply(Style& s) const { s.layout.align = v; }
};
[[nodiscard]] constexpr AlignMod align(Align a) { return {a}; }

/// Centre children on both axes — by far the most-typed layout in any UI.
struct Center {
    MAYAG_MODIFIER(Center, caps::container, caps::none, caps::none, "center");
    constexpr void apply(Style& s) const {
        s.layout.justify = Justify::center;
        s.layout.align   = Align::center;
    }
};
inline constexpr Center center{};

struct Wrap {
    MAYAG_MODIFIER(Wrap, caps::container, caps::none, caps::none, "wrap");
    constexpr void apply(Style& s) const { s.layout.wrap = true; }
};
inline constexpr Wrap wrap{};

/// An explicit width.
///
/// Grants `sized_x` and BANS it, so specifying the width twice is a compile
/// error rather than a silent last-one-wins. Two `width()` calls on one
/// element always mean the author lost track of which is in effect — and the
/// answer ("the last one") is invisible at the call site.
struct Width  { MAYAG_MODIFIER(Width,  caps::none, caps::sized_x, caps::sized_x, "width");
                Length v; constexpr void apply(Style& s) const { s.layout.width = v; } };
struct Height { MAYAG_MODIFIER(Height, caps::none, caps::sized_y, caps::sized_y, "height");
                Length v; constexpr void apply(Style& s) const { s.layout.height = v; } };

[[nodiscard]] constexpr Width  width(Length l)  { return {l}; }
[[nodiscard]] constexpr Width  width(float v)   { return {px(v)}; }
[[nodiscard]] constexpr Height height(Length l) { return {l}; }
[[nodiscard]] constexpr Height height(float v)  { return {px(v)}; }

struct Size {
    MAYAG_MODIFIER(Size, caps::none, caps::sized_x | caps::sized_y,
                   caps::sized_x | caps::sized_y, "size");
    Length w, h;
    constexpr void apply(Style& s) const { s.layout.width = w; s.layout.height = h; }
};
[[nodiscard]] constexpr Size size(float w, float hgt) { return {px(w), px(hgt)}; }
[[nodiscard]] constexpr Size size(Length w, Length hgt) { return {w, hgt}; }

struct MinSize {
    MAYAG_MODIFIER(MinSize, caps::none, caps::none, caps::none, "min_size");
    Length w, h;
    constexpr void apply(Style& s) const { s.layout.min_width = w; s.layout.min_height = h; }
};
[[nodiscard]] constexpr MinSize min_size(float w, float hgt) { return {px(w), px(hgt)}; }

struct MaxSize {
    MAYAG_MODIFIER(MaxSize, caps::none, caps::none, caps::none, "max_size");
    Length w, h;
    constexpr void apply(Style& s) const { s.layout.max_width = w; s.layout.max_height = h; }
};
[[nodiscard]] constexpr MaxSize max_size(float w, float hgt) { return {px(w), px(hgt)}; }

/// Take a share of the leftover main-axis space.
///
/// Also makes the node SHRINKABLE. A node that fills the leftover room is,
/// by construction, the one that should give way when there is no leftover
/// room — and `grow()` without `shrink()` is a trap: the row silently
/// overflows instead of compressing, and the content ends up hundreds of
/// pixels off-screen. mayag's own dashboard shipped that bug: `grow()` on the
/// main column pushed its header to x=2976 in a 980px window.
///
/// Pass `grow(n, 0.0f)` for the rare node that must fill space but never
/// compress; `rigid` says the same thing more loudly.
///
/// Forbidden on an absolutely positioned element: such a node is out of flow,
/// so "grow" has nothing to grow relative to, and silently ignoring it hides
/// real layout bugs.
struct Grow {
    // Bans `sized_x | sized_y`: an element with BOTH dimensions pinned has no
    // axis left to grow along, so `grow()` there is always dead code. A single
    // fixed dimension is legitimate (a fixed-height row that grows in width),
    // and which axis is "main" depends on the parent, which an element cannot
    // see — so that case is left to `layout::audit()`.
    MAYAG_MODIFIER(Grow, caps::none, caps::flexible, caps::positioned, "grow");
    float g;
    float s;
    constexpr void apply(Style& st) const {
        st.layout.grow   = g;
        st.layout.shrink = s;
    }
};
[[nodiscard]] constexpr Grow grow(float v = 1.0f, float shrink_ = 1.0f) {
    return {v, shrink_};
}

/// Opt in to absorbing overflow.
///
/// mayag defaults `shrink` to 0 so an explicit size is a guarantee rather
/// than a suggestion (see LayoutStyle::shrink). Reach for this on the ONE
/// child that should give way when a row runs out of room — typically the
/// text-bearing pane next to a fixed sidebar.
struct Shrink {
    MAYAG_MODIFIER(Shrink, caps::none, caps::flexible, caps::positioned, "shrink");
    float v;
    constexpr void apply(Style& s) const { s.layout.shrink = v; }
};
[[nodiscard]] constexpr Shrink shrink(float v = 1.0f) { return {v}; }

/// Take leftover space AND give way under pressure — `grow() | shrink()`.
///
/// This is what most "fill the remaining room" children actually want, and
/// naming it once stops the common bug of writing `grow()` alone and then
/// wondering why the row overflows instead of compressing.
struct Flexible {
    MAYAG_MODIFIER(Flexible, caps::none, caps::flexible, caps::positioned, "flexible");
    float g, s;
    constexpr void apply(Style& st) const {
        st.layout.grow   = g;
        st.layout.shrink = s;
    }
};
[[nodiscard]] constexpr Flexible flexible(float grow_ = 1.0f, float shrink_ = 1.0f) {
    return {grow_, shrink_};
}

/// Never shrink, never grow — an explicit "this size is final".
///
/// Redundant given the defaults, but worth having: it documents intent at the
/// call site and survives a later refactor that adds `flexible()` to a parent.
struct Rigid {
    MAYAG_MODIFIER(Rigid, caps::none, caps::none, caps::none, "rigid");
    constexpr void apply(Style& s) const { s.layout.grow = 0.0f; s.layout.shrink = 0.0f; }
};
inline constexpr Rigid rigid{};

/// Position against the parent's padding box, out of flow. The reverse of
/// `grow`'s ban, so the conflict is caught whichever order you write it in.
/// Position against the parent's padding box, OUT OF FLOW.
///
/// Out of flow means the child does not contribute to the parent's size — so
/// the parent must get its size from somewhere else. In practice that means
/// one of:
///
///   * a sibling that IS in flow (the normal overlay case: content in flow,
///     a badge or caret absolutely placed over it), or
///   * an explicit size on the parent.
///
/// A container holding ONLY absolute children and no explicit size collapses
/// to nothing, and — because such containers are usually `clip`ped — silently
/// crops whatever was inside. `layout::audit()` reports both.
struct Absolute {
    MAYAG_MODIFIER(Absolute, caps::none, caps::positioned, caps::flexible, "absolute");
    Vec2 at;
    constexpr void apply(Style& s) const {
        s.layout.position = Positioning::absolute;
        s.layout.offset   = at;
    }
};
[[nodiscard]] constexpr Absolute absolute(float x, float y) { return {Vec2{x, y}}; }
[[nodiscard]] constexpr Absolute absolute() { return {Vec2{}}; }

struct Offset {
    MAYAG_MODIFIER(Offset, caps::none, caps::none, caps::none, "offset");
    Vec2 v;
    constexpr void apply(Style& s) const { s.layout.offset = v; }
};
[[nodiscard]] constexpr Offset offset(float x, float y) { return {Vec2{x, y}}; }

// ════════════════════════════════════════════════════════════════════════
// Modifiers — paint
// ════════════════════════════════════════════════════════════════════════

struct Bg {
    MAYAG_MODIFIER(Bg, caps::none, caps::fill, caps::none, "bg");
    Color<Srgb> c;
    constexpr void apply(Style& s) const { s.fill = solid_fill(c); }
};
[[nodiscard]] constexpr Bg bg(Color<Srgb> c) { return {c}; }
template <ColorSpace S>
[[nodiscard]] constexpr Bg bg(Color<S> c) { return {c.template to<Srgb>()}; }

/// Linear gradient between two normalised points in the box.
/// Stop positions are validated at compile time when the call is constexpr.
struct LinearGradient {
    MAYAG_MODIFIER(LinearGradient, caps::none, caps::fill | caps::gradient, caps::none, "linear_gradient");
    Fill f;
    constexpr void apply(Style& s) const { s.fill = f; }
};

[[nodiscard]] constexpr LinearGradient linear_gradient(Color<Srgb> from, Color<Srgb> to,
                                                       Vec2 a = {0.0f, 0.0f}, Vec2 b = {0.0f, 1.0f}) {
    Fill f{.kind = FillKind::linear_gradient, .from = a, .to = b};
    f.stops[0] = {0.0f, from};
    f.stops[1] = {1.0f, to};
    f.stop_count = 2;
    return {f};
}

namespace detail {
/// Non-decreasing check on a stop pack. A free function rather than a lambda
/// so it can be named in a `static_assert` without capturing anything.
template <GradientStop... Stops>
constexpr bool stops_ordered() noexcept {
    constexpr std::array<GradientStop, sizeof...(Stops)> arr{Stops...};
    for (std::size_t i = 1; i < arr.size(); ++i) {
        if (arr[i].position < arr[i - 1].position) return false;
    }
    return true;
}
}  // namespace detail

/// Multi-stop gradient. Positions must be non-decreasing — a scrambled stop
/// list produces a garbled ramp that is painful to debug at runtime, so it is
/// rejected here.
template <GradientStop... Stops>
    requires (sizeof...(Stops) >= 2 && sizeof...(Stops) <= max_gradient_stops)
[[nodiscard]] constexpr LinearGradient gradient(Vec2 a = {0.0f, 0.0f}, Vec2 b = {0.0f, 1.0f}) {
    static_assert(detail::stops_ordered<Stops...>(),
                  "mayag: gradient stop positions must be non-decreasing (0.0 -> 1.0).");

    constexpr std::array<GradientStop, sizeof...(Stops)> arr{Stops...};
    Fill f{.kind = FillKind::linear_gradient, .from = a, .to = b};
    for (std::size_t i = 0; i < arr.size(); ++i) f.stops[i] = arr[i];
    f.stop_count = static_cast<std::uint8_t>(arr.size());
    return {f};
}

[[nodiscard]] constexpr LinearGradient radial_gradient(Color<Srgb> inner, Color<Srgb> outer,
                                                       Vec2 center_ = {0.5f, 0.5f}, float r = 0.5f) {
    Fill f{.kind = FillKind::radial_gradient, .from = center_, .radius = r};
    f.stops[0] = {0.0f, inner};
    f.stops[1] = {1.0f, outer};
    f.stop_count = 2;
    return {f};
}

/// Interpolate this gradient in sRGB instead of Oklch. Requires a gradient —
/// on a solid fill it would be a lie.
struct SrgbInterp {
    MAYAG_MODIFIER(SrgbInterp, caps::gradient, caps::none, caps::none, "srgb_interpolation");
    constexpr void apply(Style& s) const { s.fill.interpolate_srgb = true; }
};
inline constexpr SrgbInterp srgb_interpolation{};

struct Border {
    MAYAG_MODIFIER(Border, caps::none, caps::stroke, caps::none, "border");
    Stroke v;
    constexpr void apply(Style& s) const { s.stroke = v; }
};
[[nodiscard]] constexpr Border border(float w, Color<Srgb> c,
                                      StrokeAlign a = StrokeAlign::inside) {
    return {Stroke{.width = w, .color = c, .align = a}};
}

/// The canonical type-state example: you cannot colour a border you never
/// created. In a stringly-typed framework this is a silent no-op.
struct BorderColor {
    MAYAG_MODIFIER(BorderColor, caps::stroke, caps::none, caps::none, "border_color");
    Color<Srgb> c;
    constexpr void apply(Style& s) const { s.stroke.color = c; }
};
[[nodiscard]] constexpr BorderColor border_color(Color<Srgb> c) { return {c}; }

struct Dashed {
    MAYAG_MODIFIER(Dashed, caps::stroke, caps::none, caps::none, "dashed");
    float len, gap_;
    constexpr void apply(Style& s) const { s.stroke.dash_length = len; s.stroke.dash_gap = gap_; }
};
[[nodiscard]] constexpr Dashed dashed(float len = 4.0f, float gap_ = 4.0f) { return {len, gap_}; }

struct Radius {
    MAYAG_MODIFIER(Radius, caps::none, caps::none, caps::none, "radius");
    Corners c;
    constexpr void apply(Style& s) const { s.corners = c; }
};
[[nodiscard]] constexpr Radius radius(float all) { return {Corners{all}}; }
[[nodiscard]] constexpr Radius radius(float tl, float tr, float br, float bl) {
    return {Corners{tl, tr, br, bl}};
}
/// Fully rounded ends; clamped to the box at layout time.
inline constexpr Radius pill{Corners{1.0e9f}};

struct ShadowMod {
    MAYAG_MODIFIER(ShadowMod, caps::none, caps::shadow, caps::none, "shadow");
    Shadow v;
    constexpr void apply(Style& s) const {
        if (s.shadow_count < max_shadows) s.shadows[s.shadow_count++] = v;
    }
};
[[nodiscard]] constexpr ShadowMod shadow(float blur, Color<Srgb> c = rgba<0x00000040>,
                                         Vec2 off = {0.0f, 2.0f}, float spread = 0.0f) {
    return {Shadow{.offset = off, .blur = blur, .spread = spread, .color = c}};
}
[[nodiscard]] constexpr ShadowMod inner_shadow(float blur, Color<Srgb> c = rgba<0x00000040>,
                                               Vec2 off = {0.0f, 1.0f}) {
    return {Shadow{.offset = off, .blur = blur, .color = c, .inset = true}};
}

/// Material-style elevation: a two-layer shadow whose spread and opacity are
/// derived from a single dp value, so the whole app stays consistent.
struct Elevation {
    MAYAG_MODIFIER(Elevation, caps::none, caps::shadow, caps::none, "elevation");
    float dp;
    constexpr void apply(Style& s) const {
        if (s.shadow_count + std::size_t{2} > max_shadows) return;
        const float a1 = num::min(0.06f + dp * 0.006f, 0.20f);
        const float a2 = num::min(0.10f + dp * 0.010f, 0.36f);
        s.shadows[s.shadow_count++] = Shadow{
            .offset = {0.0f, dp * 0.5f}, .blur = dp * 1.4f,
            .color = colors::black.fade(a1)};
        s.shadows[s.shadow_count++] = Shadow{
            .offset = {0.0f, dp * 0.15f}, .blur = dp * 0.4f,
            .color = colors::black.fade(a2)};
    }
};
[[nodiscard]] constexpr Elevation elevation(float dp) { return {dp}; }

struct Opacity {
    MAYAG_MODIFIER(Opacity, caps::none, caps::none, caps::none, "opacity");
    float v;
    constexpr void apply(Style& s) const { s.opacity = num::saturate(v); }
};
[[nodiscard]] constexpr Opacity opacity(float v) { return {v}; }

/// Make this element a scroll viewport driven by `state`.
///
/// The state lives in YOUR model, so the offset is ordinary application data:
/// saveable, restorable, assertable, and changed only by `update()`.
struct Scroll {
    MAYAG_MODIFIER(Scroll, caps::container, caps::clipped, caps::none, "scroll");
    const ScrollState* state;
    ScrollAxis         axis;
    constexpr void apply(Style& s) const {
        s.layout.scroll = state;
        s.clip = true;
        if (state != nullptr) state->axis = axis;
    }
};
[[nodiscard]] inline Scroll scroll(const ScrollState& s, ScrollAxis axis = ScrollAxis::vertical) {
    return {&s, axis};
}

struct Clip {
    MAYAG_MODIFIER(Clip, caps::container, caps::clipped, caps::none, "clip");
    constexpr void apply(Style& s) const { s.clip = true; }
};
inline constexpr Clip clip{};

/// Frosted glass. Only meaningful over something, so it requires a container
/// (the thing whose backdrop is being sampled has to have contents behind it).
struct Blur {
    MAYAG_MODIFIER(Blur, caps::none, caps::none, caps::none, "backdrop_blur");
    Backdrop v;
    constexpr void apply(Style& s) const { s.backdrop = v; }
};
[[nodiscard]] constexpr Blur backdrop_blur(float r, float saturation = 1.6f) {
    return {Backdrop{.blur = r, .saturation = saturation}};
}

struct Blend {
    MAYAG_MODIFIER(Blend, caps::none, caps::none, caps::none, "blend");
    BlendMode v;
    constexpr void apply(Style& s) const { s.blend = v; }
};
[[nodiscard]] constexpr Blend blend(BlendMode m) { return {m}; }

struct Transform {
    MAYAG_MODIFIER(Transform, caps::none, caps::none, caps::none, "transform");
    Affine m;
    constexpr void apply(Style& s) const { s.transform = m; }
};
[[nodiscard]] constexpr Transform rotate(float degrees) {
    return {Affine::rotation(num::radians(degrees))};
}
[[nodiscard]] constexpr Transform scale(float s) { return {Affine::scaling({s, s})}; }
[[nodiscard]] constexpr Transform transform(Affine m) { return {m}; }

// ════════════════════════════════════════════════════════════════════════
// Modifiers — text
// ════════════════════════════════════════════════════════════════════════

/// Every text modifier requires `caps::text`. Piping `bold` onto a box is a
/// compile error naming the mistake, not a setting that quietly does nothing.
struct Font {
    MAYAG_MODIFIER(Font, caps::text, caps::none, caps::none, "font");
    float sz;
    constexpr void apply(Style& s) const { s.text.size = sz; }
};
[[nodiscard]] constexpr Font font(float sz) { return {sz}; }

struct Weight {
    MAYAG_MODIFIER(Weight, caps::text, caps::none, caps::none, "weight");
    FontWeight w;
    constexpr void apply(Style& s) const { s.text.weight = w; }
};
[[nodiscard]] constexpr Weight weight(FontWeight w) { return {w}; }
inline constexpr Weight bold{FontWeight::bold};
inline constexpr Weight semibold{FontWeight::semi_bold};
inline constexpr Weight light{FontWeight::light};

struct Italic {
    MAYAG_MODIFIER(Italic, caps::text, caps::none, caps::none, "italic");
    constexpr void apply(Style& s) const { s.text.italic = true; }
};
inline constexpr Italic italic{};

struct Underline {
    MAYAG_MODIFIER(Underline, caps::text, caps::none, caps::none, "underline");
    constexpr void apply(Style& s) const { s.text.underline = true; }
};
inline constexpr Underline underline{};

struct Strike {
    MAYAG_MODIFIER(Strike, caps::text, caps::none, caps::none, "strikethrough");
    constexpr void apply(Style& s) const { s.text.strikethrough = true; }
};
inline constexpr Strike strikethrough{};

struct Fg {
    MAYAG_MODIFIER(Fg, caps::text, caps::none, caps::none, "fg");
    Color<Srgb> c;
    constexpr void apply(Style& s) const { s.text.color = c; }
};
[[nodiscard]] constexpr Fg fg(Color<Srgb> c) { return {c}; }
template <ColorSpace S>
[[nodiscard]] constexpr Fg fg(Color<S> c) { return {c.template to<Srgb>()}; }

struct LineHeight {
    MAYAG_MODIFIER(LineHeight, caps::text, caps::none, caps::none, "line_height");
    float v;
    constexpr void apply(Style& s) const { s.text.line_height = v; }
};
[[nodiscard]] constexpr LineHeight line_height(float v) { return {v}; }

struct Tracking {
    MAYAG_MODIFIER(Tracking, caps::text, caps::none, caps::none, "tracking");
    float v;
    constexpr void apply(Style& s) const { s.text.letter_spacing = v; }
};
[[nodiscard]] constexpr Tracking tracking(float v) { return {v}; }

struct TextAlignMod {
    MAYAG_MODIFIER(TextAlignMod, caps::text, caps::none, caps::none, "text_align");
    TextAlign v;
    constexpr void apply(Style& s) const { s.text.align = v; }
};
[[nodiscard]] constexpr TextAlignMod text_align(TextAlign a) { return {a}; }

struct Ellipsis {
    MAYAG_MODIFIER(Ellipsis, caps::text, caps::none, caps::none, "ellipsis");
    constexpr void apply(Style& s) const { s.text.overflow = TextOverflow::ellipsis; }
};
inline constexpr Ellipsis ellipsis{};

/// Opt IN to wrapping. For paragraphs and multi-line prose.
///
/// Text defaults to `ellipsis` because most UI text is a label, and a
/// wrapping label in a tight row becomes an unreadable vertical ribbon.
struct WrapText {
    MAYAG_MODIFIER(WrapText, caps::text, caps::none, caps::none, "wrap_text");
    constexpr void apply(Style& s) const { s.text.overflow = TextOverflow::wrap; }
};
inline constexpr WrapText wrap_text{};

/// Hard clip with no ellipsis.
struct ClipText {
    MAYAG_MODIFIER(ClipText, caps::text, caps::none, caps::none, "clip_text");
    constexpr void apply(Style& s) const { s.text.overflow = TextOverflow::clip; }
};
inline constexpr ClipText clip_text{};

// ════════════════════════════════════════════════════════════════════════
// Identity
// ════════════════════════════════════════════════════════════════════════

/// Stable 64-bit id from a name. FNV-1a: identical across runs, platforms,
/// and compilers, so an id computed in a test matches the one the app uses.
///
/// Public because widget subscriptions and `Ctx::hovered()` need it for
/// runtime-generated names (list rows, tabs) where `id<"...">` cannot work.
[[nodiscard]] constexpr std::uint64_t node_id(std::string_view s) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : s) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Compile-time form: `node_id_v<"save-btn">`.
template <fixed_string Name>
inline constexpr std::uint64_t node_id_v = node_id(Name.view());

namespace detail {
/// Retained spelling for internal callers.
constexpr std::uint64_t fnv1a(std::string_view s) noexcept { return node_id(s); }
}  // namespace detail

struct Id {
    MAYAG_MODIFIER(Id, caps::none, caps::named, caps::none, "id");
    std::uint64_t v;
    constexpr void apply(Style& s) const { s.id = v; }
};
template <fixed_string Name>
inline constexpr Id id{node_id(Name.view())};
[[nodiscard]] constexpr Id id_of(std::string_view s) { return {node_id(s)}; }
/// When the caller already hashed the name (a widget that took a node id as a
/// parameter), skip re-hashing.
[[nodiscard]] constexpr Id id_of_raw(std::uint64_t v) { return {v}; }

// ════════════════════════════════════════════════════════════════════════
// Composition
// ════════════════════════════════════════════════════════════════════════

/// A reusable bundle of modifiers — the mayag equivalent of a CSS class.
/// It carries the union of its members' capability requirements, so applying
/// a bundle is checked exactly as strictly as applying each piece.
template <Modifier... Ms>
struct Bundle {
    static constexpr caps::Set needs = (caps::none | ... | Ms::needs);
    static constexpr caps::Set gives = (caps::none | ... | Ms::gives);
    static constexpr caps::Set bans  = (caps::none | ... | Ms::bans);
    static constexpr std::string_view label = "style bundle";

    std::tuple<Ms...> mods;
    constexpr void apply(Style& s) const {
        std::apply([&](const auto&... m) { (m.apply(s), ...); }, mods);
    }
};

/// `constexpr auto card = styles(bg(...), radius(12), elevation(4));`
template <Modifier... Ms>
[[nodiscard]] constexpr auto styles(Ms... ms) {
    return Bundle<Ms...>{std::tuple<Ms...>{ms...}};
}

/// Conditional modifier — applies only when the flag is set, without losing
/// the capability check (the check happens regardless of the runtime flag,
/// which is what you want: correctness should not depend on a bool).
template <Modifier M>
struct When {
    static constexpr caps::Set needs = M::needs;
    static constexpr caps::Set gives = M::gives;
    static constexpr caps::Set bans  = M::bans;
    static constexpr std::string_view label = M::label;

    bool cond;
    M    mod;
    constexpr void apply(Style& s) const { if (cond) mod.apply(s); }
};
template <Modifier M>
[[nodiscard]] constexpr auto when(bool cond, M m) { return When<M>{cond, m}; }

}  // namespace mayag::dsl
