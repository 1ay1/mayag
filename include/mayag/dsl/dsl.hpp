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

#include "../core/animation.hpp"
#include "../scene/node.hpp"
#include "../style/state.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <span>
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
/// Has children, so text properties set here are inherited by the text
/// inside it. An empty `box()` has no glyphs anywhere below it, so `bold`
/// on one is still a compile error.
inline constexpr Set parent      = 1u << 14;

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
        case parent:      return "a container with children";
        case text | parent:
            return "a text element, or a container with children (text properties set on a container are inherited by the text inside it)";
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

/// static_assert with a computed message (P2741) where the compiler has it;
/// elsewhere (MSVC) the same check with a fixed message, so the DSL still
/// rejects the misuse, only less specifically.
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
#  define MAYAG_DSL_ASSERT(cond, msg, fallback) static_assert(cond, msg)
#else
#  define MAYAG_DSL_ASSERT(cond, msg, fallback) static_assert(cond, fallback)
#endif

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

/// A text property: valid on text, and on any container with children,
/// which then passes it down to the text inside (CSS inheritance). An empty
/// `box()` has no text anywhere below it, so it is still rejected there.
#define MAYAG_TEXT_MODIFIER(Name, Label)                                     \
    MAYAG_MODIFIER(Name, caps::none, caps::none, caps::none, Label);         \
    static constexpr caps::Set needs_any = caps::text | caps::parent

/// Optional "at least one of" requirement, alongside `needs` ("all of").
template <typename M>
[[nodiscard]] consteval caps::Set needs_any_of() noexcept {
    if constexpr (requires { M::needs_any; }) return M::needs_any;
    else return caps::none;
}

/// The capability proof, shared by every element type's pipe.
///
/// A COMPOSITE (a bundle, a `when`, a class) checks each member in turn,
/// with the capabilities earlier members gave, so `styles(fg(c))` is
/// rejected on an empty box exactly like `fg(c)` alone, and
/// `styles(border(1, c), border_color(d))` is accepted.
template <caps::Set C, typename M>
consteval void check_modifier() {
    if constexpr (requires { M::is_composite; }) {
        M::template check<C>();
    } else {
        MAYAG_DSL_ASSERT((M::needs & ~C) == 0,
                         missing_capability(M::label, caps::first_missing(M::needs, C)),
                         "mayag: this modifier is not valid on this element (missing capability)");
        MAYAG_DSL_ASSERT(needs_any_of<M>() == 0 || (needs_any_of<M>() & C) != 0,
                         missing_capability(M::label, needs_any_of<M>()),
                         "mayag: this modifier is not valid on this element (missing capability)");
        MAYAG_DSL_ASSERT((M::bans & C) == 0,
                         forbidden_capability(M::label, M::bans & C),
                         "mayag: this modifier conflicts with one already set on this element");
    }
}

/// Check a sequence, threading capabilities through (`border` then
/// `border_color`). `Bans` off for state variants: a variant REPLACES a
/// value, so `hovered(width(120))` on a sized box is the point, not a clash.
template <caps::Set C, bool Bans, typename M, typename... Rest>
consteval void check_sequence() {
    if constexpr (Bans || requires { M::is_composite; }) {
        check_modifier<C, M>();
    } else {
        MAYAG_DSL_ASSERT((M::needs & ~C) == 0,
                         missing_capability(M::label, caps::first_missing(M::needs, C)),
                         "mayag: this modifier is not valid on this element (missing capability)");
        MAYAG_DSL_ASSERT(needs_any_of<M>() == 0 || (needs_any_of<M>() & C) != 0,
                         missing_capability(M::label, needs_any_of<M>()),
                         "mayag: this modifier is not valid on this element (missing capability)");
    }
    if constexpr (sizeof...(Rest) > 0) check_sequence<C | M::gives, Bans, Rest...>();
}

/// A RESPONDER changes how an element responds rather than (only) how it
/// looks now: a state variant (`hovered(...)`, `when(sel, ...)`), a variable
/// binding (`vars(...)`), a transition, or a class mixing all of those with
/// plain modifiers. It is checked against the element like a modifier, and
/// its rules are recorded; the cascade applies them when their state holds.
template <typename R>
concept Responder = requires(const R r, Cascade& c, Style& s) {
    requires R::is_responder;
    { R::gives } -> std::convertible_to<caps::Set>;
    r.on_pipe(s);
    r.attach(c, std::shared_ptr<const R>{});
};

template <caps::Set C, typename R>
consteval void check_responder() {
    R::template check<C>();
}

/// The responders piped onto an element, kept in its TYPE so a styled
/// element stays a literal (constexpr) value: nothing is allocated until
/// `build()`.
template <typename... Rs>
struct RuleSet {
    // No `{}` initializer: a responder may hold a capturing lambda (an
    // inline handler) that cannot be default-constructed, and clang
    // instantiates a default member initializer even when the aggregate is
    // initialized explicitly.
    std::tuple<Rs...> rs;

    /// Compile the rules into a Cascade. One allocation holds them all --
    /// or none: an element whose only responder is a theme look shares the
    /// look's pre-built Cascade outright (a refcount bump), which is the
    /// common case for every widget instance.
    [[nodiscard]] std::shared_ptr<const Cascade> compile() const& { return compile_from(rs); }
    [[nodiscard]] std::shared_ptr<const Cascade> compile() && { return compile_from(std::move(rs)); }

    template <class T>
    [[nodiscard]] static std::shared_ptr<const Cascade> compile_from(T&& rules) {
        if constexpr (sizeof...(Rs) == 0) {
            return nullptr;
        } else {
            if constexpr (sizeof...(Rs) == 1) {
                if constexpr (requires { std::get<0>(rules).shared_rules(); }) {
                    if (auto shared = std::get<0>(rules).shared_rules()) return shared;
                }
            }
            auto c = std::make_shared<Cascade>();
            auto self = std::make_shared<const std::tuple<Rs...>>(std::forward<T>(rules));
            c->keep.push_back(self);
            // `r` lives inside *self, so an aliasing pointer keeps it alive
            // exactly as long as the Cascade does.
            std::apply([&](const auto&... r) {
                (r.attach(*c, std::shared_ptr<const std::remove_cvref_t<decltype(r)>>(self, &r)), ...);
            }, *self);
            return c;
        }
    }
};

// ════════════════════════════════════════════════════════════════════════
// Elem — the compile-time element
// ════════════════════════════════════════════════════════════════════════

template <typename Rules, NodeKind Kind, caps::Set Caps, typename... Kids>
struct ElemR;

namespace detail {
/// The usual visitor-overload set.
template <typename... F>
struct overloaded : F... { using F::operator()...; };
template <typename... F>
overloaded(F...) -> overloaded<F...>;

/// Add one child of any accepted kind to `parent`: a compile-time element
/// (built), a runtime Node, a std::vector<Node> or a fragment (both spliced,
/// so the parent's gap and wrap apply across them). Defined below.
template <class K>
void append_child(Node& parent, K&& k);
/// How many nodes a children tuple adds (ranges and fragments counted), so
/// the parent reserves once.
template <class... K>
std::size_t child_count_hint(const std::tuple<K...>& kids);

/// An element's children, on the heap. Each pipe stage makes a new element
/// type, so inline children would be moved (Style and all) once per
/// modifier per level; boxed, that's one pointer.
template <class... Kids>
class KidBox {
  public:
    using tuple_type = std::tuple<Kids...>;
    constexpr KidBox() requires std::default_initializable<tuple_type> : p_{new tuple_type{}} {}
    constexpr explicit KidBox(tuple_type&& t) : p_{new tuple_type(std::move(t))} {}
    constexpr KidBox(const KidBox& o) : p_{o.p_ ? new tuple_type(*o.p_) : nullptr} {}
    constexpr KidBox(KidBox&& o) noexcept : p_{std::exchange(o.p_, nullptr)} {}
    constexpr KidBox& operator=(KidBox o) noexcept { std::swap(p_, o.p_); return *this; }
    constexpr ~KidBox() { delete p_; }
    [[nodiscard]] constexpr tuple_type& get() noexcept { return *p_; }
    [[nodiscard]] constexpr const tuple_type& get() const noexcept { return *p_; }
  private:
    tuple_type* p_;
};
template <>
class KidBox<> {
  public:
    constexpr KidBox() = default;
    constexpr explicit KidBox(std::tuple<>&&) {}
    [[nodiscard]] constexpr std::tuple<> get() const noexcept { return {}; }
};
}  // namespace detail

template <NodeKind Kind, caps::Set Caps, typename... Kids>
struct Elem {
    static constexpr NodeKind  kind = Kind;
    static constexpr caps::Set capabilities = Caps;

    StyleBox                  style{};
    std::string_view          content{};   ///< static text / image key
    std::uint32_t             texture = 0;
    detail::KidBox<Kids...>   kids;   // no `{}`: children may not be default-constructible

    /// Materialise the runtime scene node. A temporary element (the usual
    /// case) moves its children in; an lvalue copies.
    [[nodiscard]] Node build() const& {
        Node n{Kind};
        build_into(n);
        return n;
    }
    [[nodiscard]] Node build() && {
        Node n{Kind};
        std::move(*this).build_into(n);
        return n;
    }
    void build_into(Node& n) const& {
        n.adopt_style(StyleBox{style});
        if constexpr (Kind == NodeKind::text) {
            n.set_text(std::string{content});
        } else if constexpr (Kind == NodeKind::image) {
            n.set_texture(texture);
        }
        if constexpr (sizeof...(Kids) > 0) {
            n.children().reserve(detail::child_count_hint(kids.get()));
            std::apply([&](const auto&... k) { (detail::append_child(n, k), ...); }, kids.get());
        }
    }
    void build_into(Node& n) && {
        n.adopt_style(std::move(style));
        if constexpr (Kind == NodeKind::text) {
            n.set_text(std::string{content});
        } else if constexpr (Kind == NodeKind::image) {
            n.set_texture(texture);
        }
        if constexpr (sizeof...(Kids) > 0) {
            n.children().reserve(detail::child_count_hint(kids.get()));
            std::apply([&](auto&... k) { (detail::append_child(n, std::move(k)), ...); }, kids.get());
        }
    }

    /// Implicit conversion so an Elem can be returned where a Node is wanted.
    operator Node() const& { return build(); }
    operator Node() && { return std::move(*this).build(); }
};

/// An element with responders (state variants, variable bindings). A
/// separate template so the 99% of elements that have none pay nothing:
/// `Elem` keeps its exact layout, and existing code naming it is untouched.
template <typename Rules, NodeKind Kind, caps::Set Caps, typename... Kids>
struct ElemR {
    static constexpr NodeKind  kind = Kind;
    static constexpr caps::Set capabilities = Caps;

    StyleBox                  style{};
    std::string_view          content{};
    std::uint32_t             texture = 0;
    detail::KidBox<Kids...>   kids;   // no `{}`: children may not be default-constructible
    Rules                     rules;

    [[nodiscard]] Node build() const& {
        Node n{Kind};
        build_into(n);
        return n;
    }
    [[nodiscard]] Node build() && {
        Node n{Kind};
        std::move(*this).build_into(n);
        return n;
    }
    void build_into(Node& n) const& {
        n.adopt_style(StyleBox{style});
        if constexpr (Kind == NodeKind::text) n.set_text(std::string{content});
        else if constexpr (Kind == NodeKind::image) n.set_texture(texture);
        if constexpr (sizeof...(Kids) > 0) {
            n.children().reserve(detail::child_count_hint(kids.get()));
            std::apply([&](const auto&... k) { (detail::append_child(n, k), ...); }, kids.get());
        }
        n.set_cascade(rules.compile());
    }
    void build_into(Node& n) && {
        n.adopt_style(std::move(style));
        if constexpr (Kind == NodeKind::text) n.set_text(std::string{content});
        else if constexpr (Kind == NodeKind::image) n.set_texture(texture);
        if constexpr (sizeof...(Kids) > 0) {
            n.children().reserve(detail::child_count_hint(kids.get()));
            std::apply([&](auto&... k) { (detail::append_child(n, std::move(k)), ...); }, kids.get());
        }
        n.set_cascade(std::move(rules).compile());
    }
    operator Node() const& { return build(); }
    operator Node() && { return std::move(*this).build(); }
};

template <typename T>
concept Element = requires { T::kind; T::capabilities; } &&
                  std::same_as<std::remove_cvref_t<decltype(T::kind)>, NodeKind>;

// ── children ──────────────────────────────────────────────────────────────────

/// Zero or more nodes spliced into the parent in place: what `each(...)`
/// and `when(cond, child)` produce. Spliced rather than wrapped, so the
/// parent's gap, wrap and justify apply across them exactly as if written
/// inline.
struct Fragment {
    std::vector<Node> nodes;
};

/// Anything that can be a child of `v`, `h` or `z`:
///   an element                 text<"a">, box() | ..., a widget's return
///   a runtime Node             a helper function's result
///   a range of either          std::vector<Node>, std::vector<OwnedText>
///   a Fragment                 each(...), when(cond, child)
template <typename T>
concept Child =
    Element<T> || std::same_as<T, Node> || std::same_as<T, Fragment> ||
    (std::ranges::input_range<T> &&
     (std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<T>>, Node> ||
      Element<std::remove_cvref_t<std::ranges::range_reference_t<T>>>));

namespace detail {
template <class K>
void append_child(Node& parent, K&& k) {
    using T = std::remove_cvref_t<K>;
    constexpr bool rv = !std::is_lvalue_reference_v<K>;
    if constexpr (std::same_as<T, Node>) {
        parent.add_child(std::forward<K>(k));
    } else if constexpr (std::same_as<T, Fragment>) {
        if constexpr (rv) for (auto& n : k.nodes) parent.add_child(std::move(n));
        else for (const auto& n : k.nodes) parent.add_child(n);
    } else if constexpr (Element<T>) {
        if constexpr (requires(Node& n) { std::forward<K>(k).build_into(n); }) {
            // Construct in place in the parent's (reserved) storage.
            std::forward<K>(k).build_into(parent.children().emplace_back(T::kind));
        } else {
            parent.add_child(std::forward<K>(k).build());
        }
    } else {
        if constexpr (rv) for (auto& x : k) append_child(parent, std::move(x));
        else for (const auto& x : k) append_child(parent, x);
    }
}

template <class K>
std::size_t count_one(const K& k) {
    if constexpr (std::same_as<K, Fragment>) return k.nodes.size();
    else if constexpr (std::ranges::sized_range<K>) return std::ranges::size(k);
    else return 1;
}
template <class... K>
std::size_t child_count_hint(const std::tuple<K...>& kids) {
    return std::apply([](const auto&... k) { return (std::size_t{0} + ... + count_one(k)); }, kids);
}
}  // namespace detail

// ── the pipe: capability proof + style application ──────────────────────

template <NodeKind K, caps::Set C, typename... Kids, Modifier M>
[[nodiscard]] constexpr auto operator|(Elem<K, C, Kids...> e, M m) {
    check_modifier<C, M>();
    Elem<K, C | M::gives, Kids...> out{std::move(e.style), e.content, e.texture, std::move(e.kids)};
    m.apply(out.style.mut());
    return out;
}

template <typename... Rs, NodeKind K, caps::Set C, typename... Kids, Modifier M>
[[nodiscard]] constexpr auto operator|(ElemR<RuleSet<Rs...>, K, C, Kids...> e, M m) {
    check_modifier<C, M>();
    ElemR<RuleSet<Rs...>, K, C | M::gives, Kids...> out{std::move(e.style), e.content, e.texture, std::move(e.kids), std::move(e.rules)};
    m.apply(out.style.mut());
    return out;
}

template <NodeKind K, caps::Set C, typename... Kids, Responder R>
[[nodiscard]] constexpr auto operator|(Elem<K, C, Kids...> e, R r) {
    check_responder<C, R>();
    ElemR<RuleSet<R>, K, C | R::gives, Kids...> out{std::move(e.style), e.content, e.texture, std::move(e.kids), RuleSet<R>{{r}}};
    r.on_pipe(out.style.mut());
    return out;
}

template <typename... Rs, NodeKind K, caps::Set C, typename... Kids, Responder R>
[[nodiscard]] constexpr auto operator|(ElemR<RuleSet<Rs...>, K, C, Kids...> e, R r) {
    check_responder<C, R>();
    ElemR<RuleSet<Rs..., R>, K, C | R::gives, Kids...> out{
        std::move(e.style), e.content, e.texture, std::move(e.kids),
        RuleSet<Rs..., R>{std::tuple_cat(std::move(e.rules.rs), std::tuple<R>{r})}};
    r.on_pipe(out.style.mut());
    return out;
}

// ════════════════════════════════════════════════════════════════════════
// Element constructors
// ════════════════════════════════════════════════════════════════════════

namespace detail {

template <Axis A, typename... Kids>
[[nodiscard]] constexpr auto make_stack(Kids&&... kids) {
    // Built in one go, never default-then-assign: children may hold
    // capturing lambdas (inline handlers), which have neither.
    StyleBox st;
    st.mut().layout.axis = A;
    using E = Elem<NodeKind::box, (sizeof...(Kids) > 0 ? caps::container | caps::parent : caps::container),
                   std::remove_cvref_t<Kids>...>;
    if constexpr (sizeof...(Kids) == 0) return E{std::move(st), {}, 0, {}};
    else return E{std::move(st), {}, 0, KidBox<std::remove_cvref_t<Kids>...>{
                                 std::tuple<std::remove_cvref_t<Kids>...>{std::move(kids)...}}};
}

}  // namespace detail

/// Vertical stack. Children may be elements, runtime `Node`s, vectors or
/// ranges of either, `each(...)` and `when(...)` -- mixed freely:
///
///     v(text<"Files"> | bold,
///       each(m.files, [](const File& f) { return text_of(f.name); }),
///       when(m.loading, spinner_node))
template <Child... Kids>
[[nodiscard]] constexpr auto v(Kids... kids) {
    return detail::make_stack<Axis::vertical>(std::move(kids)...);
}

/// Horizontal stack. Same children as `v`.
template <Child... Kids>
[[nodiscard]] constexpr auto h(Kids... kids) {
    return detail::make_stack<Axis::horizontal>(std::move(kids)...);
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
template <Child... Kids>
[[nodiscard]] constexpr auto z(Kids... kids) {
    auto e = detail::make_stack<Axis::vertical>(std::move(kids)...);
    e.style.mut().layout.align = Align::stretch;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        // Everything after the very first child floats. A runtime child that
        // expands to several nodes (a vector, each(), when()) is pinned node
        // by node, so `z(base, each(badges, ...))` floats every badge.
        bool first = true;
        const auto pin_node = [&](Node& n) {
            if (!first) n.style().layout.position = Positioning::absolute;
            first = false;
        };
        const auto pin_elem = [&](auto& el) {
            if (!first) el.style.mut().layout.position = Positioning::absolute;
            first = false;
        };
        // One overload per element kind of a runtime range, chosen by overload
        // resolution instead of `if constexpr`: MSVC instantiated the discarded
        // branch of the generic version for vector<Node> (C2228).
        const auto pin_one = detail::overloaded{
            [&](Node& n) { pin_node(n); },
            [&](auto& el) { pin_elem(el); },
        };
        const auto pin = [&](auto& kid) {
            using K = std::remove_cvref_t<decltype(kid)>;
            // For a wrapped runtime node, mark the PAYLOAD absolute. Writing to
            // the wrapper's style would make it non-default, and NodeElem::build
            // then replaces the payload's whole style with it -- every overlay
            // in `z(content, node(x))` silently lost its size, fill and radius.
            if constexpr (requires { kid.payload.style(); }) {
                pin_node(kid.payload);
            } else if constexpr (Element<K>) {
                pin_elem(kid);
            } else if constexpr (std::same_as<K, Node>) {
                pin_node(kid);
            } else if constexpr (std::same_as<K, Fragment>) {
                for (auto& n : kid.nodes) pin_node(n);
            } else {
                for (auto& x : kid) pin_one(x);
            }
        };
        if constexpr (sizeof...(Kids) > 0) (pin(std::get<I>(e.kids.get())), ...);
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

    StyleBox         style{};
    std::string_view content{};   ///< unused; the owned string below is the source
    std::uint32_t    texture = 0;
    std::tuple<>     kids{};

    std::string owned;
    std::shared_ptr<const Cascade> cascade{};

    [[nodiscard]] Node build() const& {
        Node n{NodeKind::text};
        n.adopt_style(StyleBox{style});
        n.set_text(owned);
        n.set_cascade(cascade);
        return n;
    }
    [[nodiscard]] Node build() && {
        Node n{NodeKind::text};
        n.adopt_style(std::move(style));
        n.set_text(std::move(owned));
        n.set_cascade(std::move(cascade));
        return n;
    }
    operator Node() const& { return build(); }
    operator Node() && { return std::move(*this).build(); }
};

[[nodiscard]] inline OwnedText text_owned(std::string s) {
    OwnedText t{};
    t.owned = std::move(s);
    return t;
}

namespace detail {
/// Runtime elements (owned text, wrapped nodes) cannot grow their type, so
/// their responders compile straight into a Cascade as they are piped.
template <typename R>
void add_responder(std::shared_ptr<const Cascade>& slot, const R& r) {
    auto c = slot ? std::make_shared<Cascade>(*slot) : std::make_shared<Cascade>();
    auto self = std::make_shared<const R>(r);
    c->keep.push_back(self);
    self->attach(*c, self);
    slot = std::move(c);
}
}  // namespace detail

/// OwnedText participates in the modifier pipeline exactly like `text<>`,
/// with the same capability checks — it is a text element that happens to
/// carry its own bytes.
template <Modifier M>
[[nodiscard]] inline OwnedText operator|(OwnedText e, M m) {
    check_modifier<OwnedText::capabilities, M>();
    m.apply(e.style.mut());
    return e;
}
template <Responder R>
[[nodiscard]] inline OwnedText operator|(OwnedText e, R r) {
    check_responder<OwnedText::capabilities, R>();
    r.on_pipe(e.style.mut());
    detail::add_responder(e.cascade, r);
    return e;
}

/// Flexible empty space — the idiomatic way to push siblings apart.
[[nodiscard]] constexpr auto spacer(float grow = 1.0f) {
    Elem<NodeKind::box, caps::flexible> e{};
    e.style.mut().layout.grow = grow;
    return e;
}

/// Fixed-size gap as an element (when `gap()` on the parent is too uniform).
[[nodiscard]] constexpr auto strut(float size) {
    Elem<NodeKind::box, caps::none> e{};
    e.style.mut().layout.width  = px(size);
    e.style.mut().layout.height = px(size);
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
    static constexpr caps::Set capabilities = caps::container | caps::parent;

    StyleBox         style{};
    std::string_view content{};
    std::uint32_t    texture = 0;
    std::tuple<>     kids{};

    Node payload{};
    std::shared_ptr<const Cascade> cascade{};

    [[nodiscard]] Node build() const& { return finish(Node{payload}); }
    [[nodiscard]] Node build() && { return finish(std::move(payload)); }
    operator Node() const& { return build(); }
    operator Node() && { return std::move(*this).build(); }

  private:
    [[nodiscard]] Node finish(Node n) const {
        // Style piped onto the wrapper wins over whatever the node carried,
        // so `node(x) | pad(8)` behaves like any other element. The payload's
        // own children and layout axis survive, because those came from how
        // the subtree was CONSTRUCTED, not from the pipeline.
        if (!style.is_default()) {
            const Style& ws = style.get();
            const Axis axis = n.style().layout.axis;
            const float gap = n.style().layout.gap;
            const std::uint64_t keep_id = n.style().id != 0 ? n.style().id : ws.id;
            n.style() = ws;
            n.style().layout.axis = axis;
            if (n.style().layout.gap == 0.0f) n.style().layout.gap = gap;
            n.style().id = keep_id;
        }
        if (cascade) {
            if (n.cascade()) {
                // Merge: the payload's own rules first, then the wrapper's.
                auto merged = std::make_shared<Cascade>(*n.cascade());
                for (const auto& k : cascade->keep) merged->keep.push_back(k);
                for (const auto& d : cascade->vars) merged->vars.push_back(d);
                for (auto r : cascade->rules) merged->add(r);
                merged->handlers.insert(merged->handlers.end(), cascade->handlers.begin(),
                                        cascade->handlers.end());
                // The wrapper's theme scope wins: `node(x) | theme(t)` must
                // re-theme x even when x carries rules of its own.
                if (cascade->theme) merged->theme = cascade->theme;
                n.set_cascade(std::move(merged));
            } else {
                n.set_cascade(cascade);
            }
        }
        return n;
    }
};

/// NodeElem participates in the modifier pipeline like any other element.
///
/// Without this, a runtime-built subtree could not be styled or sized, which
/// defeats the point of having an escape hatch: real UIs need `list(...) |
/// scroll(state) | size(...)`.
namespace detail {
/// A memo() root is shared with last frame's cache, so styles piped onto it
/// go on a plain box around it instead.
inline void wrap_memo_root(NodeElem& e) {
    if (!e.payload.memo_result()) return;
    Node w{NodeKind::box};
    w.add_child(std::move(e.payload));
    e.payload = std::move(w);
}
}  // namespace detail

template <Modifier M>
[[nodiscard]] inline NodeElem operator|(NodeElem e, M m) {
    check_modifier<NodeElem::capabilities, M>();
    detail::wrap_memo_root(e);
    // Onto the PAYLOAD, exactly as piping onto the element that built it
    // would. (This used to collect into a separate Style that build() then
    // swapped in wholesale, so `node(card) | grow()` erased the card's fill,
    // padding and radius -- every helper-returned Node lost its look the
    // moment a caller sized it.)
    m.apply(e.payload.style());
    return e;
}
template <Responder R>
[[nodiscard]] inline NodeElem operator|(NodeElem e, R r) {
    check_responder<NodeElem::capabilities, R>();
    detail::wrap_memo_root(e);
    r.on_pipe(e.payload.style());
    detail::add_responder(e.cascade, r);
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

/// Column / row of a RUNTIME-sized list of children — the dynamic-list twins of
/// `v(...)` / `h(...)`, which take a fixed set of compile-time children. This is
/// what a view does with `model.items`: build a std::vector<Node> in a loop,
/// then `v_list(rows)`. Further modifiers pipe on as usual.
[[nodiscard]] inline auto v_list(std::vector<Node> children, float gap_px = 0.0f) {
    return list(Axis::vertical, std::move(children), gap_px);
}
[[nodiscard]] inline auto h_list(std::vector<Node> children, float gap_px = 0.0f) {
    return list(Axis::horizontal, std::move(children), gap_px);
}

// ── dynamic children: each / when ────────────────────────────────────────────────
//
// The two things every real view does with data, as one expression, in
// place, beside static children:
//
//     v(text<"Inbox"> | bold,
//       each(m.mails, [](const Mail& x, std::size_t i) {
//           return h(text_of(x.from), spacer(), text_of(x.when)) | id_of("mail", i);
//       }),
//       when(m.mails.empty(), text<"Nothing here"> | fg(var::text_disabled)))
//
// Both produce a Fragment, which the parent SPLICES: the rows are the
// stack's own children, so its gap, wrap and justify apply across them.

namespace detail {
template <class R>
void splice_into(std::vector<Node>& out, R&& r) {
    using T = std::remove_cvref_t<R>;
    if constexpr (std::same_as<T, Fragment>) {
        for (auto& n : r.nodes) out.push_back(std::move(n));
    } else if constexpr (std::same_as<T, Node>) {
        out.push_back(std::forward<R>(r));
    } else if constexpr (Element<T>) {
        out.push_back(r.build());
    } else {
        static_assert(Child<T>, "mayag: each()'s function must return an element, a Node, or a Fragment.");
        for (auto&& x : r) splice_into(out, x);
    }
}
}  // namespace detail

/// One child per item. `fn` takes `(item)` or `(item, index)` and returns an
/// element, a Node, or another fragment (nested `each` / `when`).
template <std::ranges::input_range R, class F>
[[nodiscard]] Fragment each(R&& items, F&& fn) {
    Fragment f;
    if constexpr (std::ranges::sized_range<R>) f.nodes.reserve(std::ranges::size(items));
    std::size_t i = 0;
    for (auto&& x : items) {
        if constexpr (std::invocable<F&, decltype(x), std::size_t>) detail::splice_into(f.nodes, fn(x, i));
        else detail::splice_into(f.nodes, fn(x));
        ++i;
    }
    return f;
}

/// A child that exists only while `cond` holds. Nothing at all otherwise:
/// no empty box, no gap left behind.
template <Child C>
[[nodiscard]] Fragment when(bool cond, C child) {
    Fragment f;
    if (cond) detail::splice_into(f.nodes, std::move(child));
    return f;
}
/// One of two children.
template <Child A, Child B>
[[nodiscard]] Fragment when(bool cond, A then_child, B else_child) {
    Fragment f;
    if (cond) detail::splice_into(f.nodes, std::move(then_child));
    else      detail::splice_into(f.nodes, std::move(else_child));
    return f;
}

/// Formatted runtime text: `textf("{} of {} files", done, total)`. Owns its
/// bytes, so it can never dangle (see text_owned).
template <class... Args>
[[nodiscard]] inline OwnedText textf(std::format_string<Args...> f, Args&&... args) {
    return text_owned(std::format(f, std::forward<Args>(args)...));
}

/// A runtime Node pipes like any element: `helper(m) | pad(8) | hovered(...)`
/// instead of `node(helper(m)) | ...`. It is checked as a container.
template <Modifier M>
[[nodiscard]] inline NodeElem operator|(Node n, M m) { return node(std::move(n)) | m; }
template <Responder R>
[[nodiscard]] inline NodeElem operator|(Node n, R r) { return node(std::move(n)) | r; }

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

/// Change a container's main axis. `v(...)` and `h(...)` choose it at
/// construction; this is for choosing it by STATE, which is what a
/// responsive layout is: `h(sidebar, main) | when(state::narrower(640),
/// direction(Axis::vertical))` stacks on a narrow window.
struct Direction {
    MAYAG_MODIFIER(Direction, caps::container, caps::none, caps::none, "direction");
    Axis v;
    constexpr void apply(Style& s) const { s.layout.axis = v; }
};
[[nodiscard]] constexpr Direction direction(Axis a) { return {a}; }

/// Declare capabilities a theme look will supply (a fill, a border, a
/// shadow), so callers can keep piping `| border_color(...)` onto a widget
/// whose border comes from the Sheet. Does nothing at runtime.
template <caps::Set C>
struct Grant {
    MAYAG_MODIFIER(Grant, caps::none, C, caps::none, "grant");
    constexpr void apply(Style&) const noexcept {}
};
template <caps::Set C>
inline constexpr Grant<C> grant{};
/// What a surface look (button, card, field) provides.
inline constexpr Grant<caps::fill | caps::stroke | caps::shadow> surface_caps{};

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

/// Align a row's children on their shared TEXT BASELINE — the correct way to
/// mix font sizes on one line (a big number beside its small unit, a heading
/// beside a caption). `h(price | font(32), unit | font(14)) | baseline`.
inline constexpr AlignMod baseline{Align::baseline};

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

/// Take the whole width / height / both of the parent: the three sizes
/// every screen root and panel wants, without spelling `pct(100)`.
struct FillAxis {
    MAYAG_MODIFIER(FillAxis, caps::none, caps::none, caps::none, "fill");
    bool x, y;
    constexpr void apply(Style& s) const {
        if (x) s.layout.width = pct(100);
        if (y) s.layout.height = pct(100);
    }
};
[[nodiscard]] constexpr FillAxis fill_width()  { return {true, false}; }
[[nodiscard]] constexpr FillAxis fill_height() { return {false, true}; }
[[nodiscard]] constexpr FillAxis fill_both()   { return {true, true}; }

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

// ---- multi-stop gradients: same stop list, any geometry ----
//
// One family: list the colour stops once, choose the shape. All perceptual
// (Oklch) by default; `| srgb_interpolation` opts out. Stops are checked at
// compile time to be ordered, so a mis-ordered gradient never ships. Stops are
// non-type template args, so use the compile-time colour form rgb<0xRRGGBB>:
//
//   box | radial_gradient<GradientStop{0.0f, rgb<0xFFDD88>},
//                         GradientStop{1.0f, rgb<0x3C1478>}>()
//   ring | conic_gradient<GradientStop{0.0f,  rgb<0xFF0000>},
//                         GradientStop{0.5f,  rgb<0x00FF00>},
//                         GradientStop{1.0f,  rgb<0xFF0000>}>()   // seamless

namespace detail {
template <GradientStop... Stops>
[[nodiscard]] constexpr Fill make_gradient(FillKind kind, Vec2 a, Vec2 b, float r) {
    static_assert(stops_ordered<Stops...>(),
                  "mayag: gradient stop positions must be non-decreasing (0.0 -> 1.0).");
    static_assert(sizeof...(Stops) <= max_gradient_stops,
                  "mayag: too many gradient stops (max 8).");
    constexpr std::array<GradientStop, sizeof...(Stops)> arr{Stops...};
    Fill f{.kind = kind, .from = a, .to = b, .radius = r};
    for (std::size_t i = 0; i < arr.size(); ++i) f.stops[i] = arr[i];
    f.stop_count = static_cast<std::uint8_t>(arr.size());
    return f;
}
}  // namespace detail

/// Multi-stop linear gradient from `a` to `b` in normalised box coords.
template <GradientStop... Stops>
[[nodiscard]] constexpr LinearGradient linear_gradient(Vec2 a = {0.0f, 0.0f},
                                                       Vec2 b = {0.0f, 1.0f}) {
    return {detail::make_gradient<Stops...>(FillKind::linear_gradient, a, b, 0.5f)};
}

/// Multi-stop radial gradient centred at `center_`, reaching `r` (fraction of
/// the box). stop(0) is the centre, stop(1) the rim.
template <GradientStop... Stops>
[[nodiscard]] constexpr LinearGradient radial_gradient(Vec2 center_ = {0.5f, 0.5f},
                                                       float r = 0.5f) {
    return {detail::make_gradient<Stops...>(FillKind::radial_gradient, center_, {1.0f, 1.0f}, r)};
}

/// Two-colour conic (angular) sweep around `center_` — spinners, hue wheels.
/// The sweep runs the full turn; stop 0 is the start angle, stop 1 the end.
[[nodiscard]] constexpr LinearGradient conic_gradient(Color<Srgb> from, Color<Srgb> to,
                                                      Vec2 center_ = {0.5f, 0.5f}) {
    Fill f{.kind = FillKind::angular_gradient, .from = center_};
    f.stops[0] = {0.0f, from};
    f.stops[1] = {1.0f, to};
    f.stop_count = 2;
    return {f};
}

/// Multi-stop conic (angular) sweep around `center_`. Repeat the first colour
/// as the last stop for a seamless wheel.
template <GradientStop... Stops>
[[nodiscard]] constexpr LinearGradient conic_gradient(Vec2 center_ = {0.5f, 0.5f}) {
    return {detail::make_gradient<Stops...>(FillKind::angular_gradient, center_, {1.0f, 1.0f}, 0.5f)};
}

// ---- multi-stop gradients from RUNTIME stops ----
//
// For colours computed from a model (a hue strip from the user's chroma, a
// preview of a palette the user picked). Same geometry as above; stops past
// max_gradient_stops are dropped. A mis-ordered stop fails a constexpr call
// outright and, at run time, is clamped to its predecessor -- a gradient a
// user configured must never take the window down.
namespace detail {
[[nodiscard]] constexpr Fill make_gradient(FillKind kind, std::span<const GradientStop> stops, Vec2 a, Vec2 b,
                                           float r) {
    Fill f{.kind = kind, .from = a, .to = b, .radius = r};
    const std::size_t n = stops.size() < max_gradient_stops ? stops.size() : max_gradient_stops;
    for (std::size_t i = 0; i < n; ++i) {
        f.stops[i] = stops[i];
        if (i > 0 && f.stops[i].position < f.stops[i - 1].position) {
            if consteval { throw "mayag: gradient stop positions must be non-decreasing"; }
            f.stops[i].position = f.stops[i - 1].position;
        }
    }
    f.stop_count = static_cast<std::uint8_t>(n);
    return f;
}
}  // namespace detail

[[nodiscard]] constexpr LinearGradient linear_gradient(std::span<const GradientStop> stops,
                                                       Vec2 a = {0.0f, 0.0f}, Vec2 b = {0.0f, 1.0f}) {
    return {detail::make_gradient(FillKind::linear_gradient, stops, a, b, 0.5f)};
}
[[nodiscard]] constexpr LinearGradient radial_gradient(std::span<const GradientStop> stops,
                                                       Vec2 center_ = {0.5f, 0.5f}, float r = 0.5f) {
    return {detail::make_gradient(FillKind::radial_gradient, stops, center_, {1.0f, 1.0f}, r)};
}
/// Runtime conic sweep: progress rings, hue wheels from computed colours.
[[nodiscard]] constexpr LinearGradient conic_gradient(std::span<const GradientStop> stops,
                                                      Vec2 center_ = {0.5f, 0.5f}) {
    return {detail::make_gradient(FillKind::angular_gradient, stops, center_, {1.0f, 1.0f}, 0.5f)};
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
        if (var::is_num(dp)) {
            // Themed depth: the geometry waits for the theme. Each shadow
            // carries the token in `spread` and its slot in `offset.x`; the
            // cascade expands them with the formula below.
            s.shadows[s.shadow_count++] = Shadow{.offset = {0.0f, 0.0f}, .blur = 0.0f, .spread = dp,
                                                 .color = colors::black};
            s.shadows[s.shadow_count++] = Shadow{.offset = {1.0f, 0.0f}, .blur = 0.0f, .spread = dp,
                                                 .color = colors::black};
            return;
        }
        s.shadows[s.shadow_count++] = elevation_shadow(dp, 0);
        s.shadows[s.shadow_count++] = elevation_shadow(dp, 1);
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
/// Move the painted element without moving its layout slot (CSS
/// `translate`). The tool for motion: siblings do not reflow while it moves.
[[nodiscard]] constexpr Transform translate(float x, float y) { return {Affine::translation({x, y})}; }

// ════════════════════════════════════════════════════════════════════════
// Modifiers — text
// ════════════════════════════════════════════════════════════════════════

/// Every text modifier requires `caps::text`. Piping `bold` onto a box is a
/// compile error naming the mistake, not a setting that quietly does nothing.
struct Font {
    MAYAG_TEXT_MODIFIER(Font, "font");
    float sz;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::size; s.text.size = sz; }
};
[[nodiscard]] constexpr Font font(float sz) { return {sz}; }

struct Weight {
    MAYAG_TEXT_MODIFIER(Weight, "weight");
    FontWeight w;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::weight; s.text.weight = w; }
};
[[nodiscard]] constexpr Weight weight(FontWeight w) { return {w}; }
inline constexpr Weight bold{FontWeight::bold};
inline constexpr Weight semibold{FontWeight::semi_bold};
inline constexpr Weight light{FontWeight::light};

// ---- variable fonts: continuous axes ----
//
// A variable font is a continuous design space. weight(number) drives the
// wght axis to ANY value (not just the nine FontWeight steps) and interpolates
// smoothly — perfect for animation. variation()/optical_size()/font_width()/
// slant() drive the other registered axes, and font_axis("abcd", v) reaches a
// custom one. All are plain values, so animate them straight off model state.

/// Compile-time 4-char-code -> big-endian axis tag, e.g. axis_tag("wght").
[[nodiscard]] constexpr std::uint32_t axis_tag(const char (&t)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(t[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(t[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(t[2])) << 8)  |
            static_cast<std::uint32_t>(static_cast<unsigned char>(t[3]));
}

/// weight(number): a continuous wght override (100..900+, interpolates).
struct WeightValue {
    MAYAG_TEXT_MODIFIER(WeightValue, "weight");
    float w;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::variations; s.text.variations.wght = w; }
};
[[nodiscard]] constexpr WeightValue weight(float w) { return {w}; }
[[nodiscard]] constexpr WeightValue weight(int w) { return {static_cast<float>(w)}; }

/// variation(tag, value): drive any variable-font axis in USER units.
struct Variation {
    MAYAG_TEXT_MODIFIER(Variation, "variation");
    std::uint32_t tag;
    float value;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::variations;
        if (tag == axis_tag("wght")) s.text.variations.wght = value;
        else                         s.text.variations.set(tag, value);
    }
};
[[nodiscard]] constexpr Variation variation(std::uint32_t tag, float value) {
    return {tag, value};
}
[[nodiscard]] constexpr Variation font_axis(const char (&tag)[5], float value) {
    return {axis_tag(tag), value};
}
/// Optical size (opsz): tune the design for its rendered size.
[[nodiscard]] constexpr Variation optical_size(float v) { return {axis_tag("opsz"), v}; }
/// Width (wdth): condensed <100 < normal < expanded.
[[nodiscard]] constexpr Variation font_width(float v) { return {axis_tag("wdth"), v}; }
/// Slant (slnt): oblique angle in degrees (usually negative).
[[nodiscard]] constexpr Variation slant(float v) { return {axis_tag("slnt"), v}; }

/// Toggle GSUB ligatures (fi, ffi, ...). On by default; `no_ligatures` is the
/// one code/monospace UIs reach for so every character stays distinct.
struct Ligatures {
    MAYAG_TEXT_MODIFIER(Ligatures, "ligatures");
    bool on;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::ligatures; s.text.ligatures = on; }
};
[[nodiscard]] constexpr Ligatures ligatures(bool on = true) { return {on}; }
inline constexpr Ligatures no_ligatures{false};

/// Set text in a family ROLE: `| family(FontFamily::mono)`, or `| monospace`.
/// The font stack maps the role to a face (FontStack::add_family).
struct Family {
    MAYAG_TEXT_MODIFIER(Family, "family");
    FontFamily f;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::family; s.text.family = f; }
};
[[nodiscard]] constexpr Family family(FontFamily f) { return {f}; }
inline constexpr Family monospace{FontFamily::mono};

struct Italic {
    MAYAG_TEXT_MODIFIER(Italic, "italic");
    constexpr void apply(Style& s) const { s.text.set |= text_bits::italic; s.text.italic = true; }
};
inline constexpr Italic italic{};

struct Underline {
    MAYAG_TEXT_MODIFIER(Underline, "underline");
    constexpr void apply(Style& s) const { s.text.set |= text_bits::underline; s.text.underline = true; }
};
inline constexpr Underline underline{};

struct Strike {
    MAYAG_TEXT_MODIFIER(Strike, "strikethrough");
    constexpr void apply(Style& s) const { s.text.set |= text_bits::strike; s.text.strikethrough = true; }
};
inline constexpr Strike strikethrough{};

struct Fg {
    MAYAG_TEXT_MODIFIER(Fg, "fg");
    Color<Srgb> c;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::color; s.text.color = c; }
};
[[nodiscard]] constexpr Fg fg(Color<Srgb> c) { return {c}; }
template <ColorSpace S>
[[nodiscard]] constexpr Fg fg(Color<S> c) { return {c.template to<Srgb>()}; }

struct LineHeight {
    MAYAG_TEXT_MODIFIER(LineHeight, "line_height");
    float v;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::line_height; s.text.line_height = v; }
};
[[nodiscard]] constexpr LineHeight line_height(float v) { return {v}; }

struct Tracking {
    MAYAG_TEXT_MODIFIER(Tracking, "tracking");
    float v;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::tracking; s.text.letter_spacing = v; }
};
[[nodiscard]] constexpr Tracking tracking(float v) { return {v}; }

/// `| lang("sr")`: the text's language (BCP 47), for the font's localized
/// forms (Serbian Cyrillic, Turkish i, Romanian s-comma, Traditional Chinese
/// punctuation). Inherited by every text node below; longer tags truncate
/// at 15 characters.
struct Lang {
    MAYAG_TEXT_MODIFIER(Lang, "lang");
    std::array<char, 16> tag{};
    constexpr void apply(Style& s) const { s.text.set |= text_bits::language; s.text.language = tag; }
};
[[nodiscard]] constexpr Lang lang(std::string_view bcp47) {
    Lang l;
    for (std::size_t i = 0; i < bcp47.size() && i + 1 < l.tag.size(); ++i) l.tag[i] = bcp47[i];
    return l;
}

struct TextAlignMod {
    MAYAG_TEXT_MODIFIER(TextAlignMod, "text_align");
    TextAlign v;
    constexpr void apply(Style& s) const { s.text.set |= text_bits::align; s.text.align = v; }
};
[[nodiscard]] constexpr TextAlignMod text_align(TextAlign a) { return {a}; }

/// Text-alignment shorthands. `text_left / text_center / text_right` set the
/// alignment; `justify_text` fills each line to the box edge — and since a
/// justified line only has slack to spread when text wraps, it also opts INTO
/// wrapping, so `text<"…"> | justify_text` just works instead of silently
/// looking left-aligned.
inline constexpr TextAlignMod text_left{TextAlign::left};
inline constexpr TextAlignMod text_center{TextAlign::center};
inline constexpr TextAlignMod text_right{TextAlign::right};

struct JustifyText {
    MAYAG_MODIFIER(JustifyText, caps::text, caps::none, caps::none, "justify_text");
    constexpr void apply(Style& s) const {
        s.text.align = TextAlign::justify;
        s.text.overflow = TextOverflow::wrap;   // justify implies multi-line
    }
};
/// Justify prose: fill every line but the last to the box edge. Named
/// `justify_text` because `justify` is the flexbox main-axis distribution.
/// Opts into wrapping too, so `text<"…"> | justify_text` just works.
inline constexpr JustifyText justify_text{};

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

/// Clamp wrapped prose to at most `n` lines, ellipsising the last visible one
/// (CSS line-clamp) — the "3 lines then …" card-description pattern. Implies
/// wrapping, so `text<"…"> | clamp_lines(3)` just works.
struct ClampLines {
    MAYAG_MODIFIER(ClampLines, caps::text, caps::none, caps::none, "clamp_lines");
    std::uint16_t n;
    constexpr void apply(Style& s) const {
        s.text.overflow  = TextOverflow::wrap;
        s.text.max_lines = n;
    }
};
[[nodiscard]] constexpr ClampLines clamp_lines(int n) {
    return {static_cast<std::uint16_t>(n < 0 ? 0 : n)};
}

/// Hard clip with no ellipsis.
struct ClipText {
    MAYAG_MODIFIER(ClipText, caps::text, caps::none, caps::none, "clip_text");
    constexpr void apply(Style& s) const { s.text.overflow = TextOverflow::clip; }
};
inline constexpr ClipText clip_text{};

// ════════════════════════════════════════════════════════════════════════
// Accessibility
// ════════════════════════════════════════════════════════════════════════

/// Declare what this element MEANS.
///
/// Most nodes need nothing: a text node announces its own text, a named leaf
/// is treated as a button. Annotate only where the automatic answer would be
/// wrong or ambiguous — an icon-only button, a checkbox drawn as a box, a
/// group that should be announced as a list.
struct RoleMod {
    MAYAG_MODIFIER(RoleMod, caps::none, caps::none, caps::none, "role");
    a11y::Role v;
    constexpr void apply(Style& s) const { s.a11y_role = v; }
};
[[nodiscard]] constexpr RoleMod role(a11y::Role r) { return {r}; }

/// `| caret_at(rect)`: where a text-entry node's caret sits, relative to the
/// node (logical px). Lets the input method put its candidate window at the
/// caret. `text_field` sets it itself.
struct CaretAt {
    MAYAG_MODIFIER(CaretAt, caps::none, caps::none, caps::none, "caret_at");
    Rect v;
    constexpr void apply(Style& s) const { s.caret_rect = v; }
};
[[nodiscard]] constexpr CaretAt caret_at(Rect r) { return {r}; }

/// What a screen reader announces. Essential for anything whose meaning is
/// carried by an icon or a colour rather than by text.
struct LabelMod {
    MAYAG_MODIFIER(LabelMod, caps::none, caps::none, caps::none, "label");
    std::string_view v;
    void apply(Style& s) const { s.a11y_label = std::string{v}; }
};
[[nodiscard]] inline LabelMod label(std::string_view text) { return {text}; }

struct DescriptionMod {
    MAYAG_MODIFIER(DescriptionMod, caps::none, caps::none, caps::none, "description");
    std::string_view v;
    void apply(Style& s) const { s.a11y_description = std::string{v}; }
};
[[nodiscard]] inline DescriptionMod description(std::string_view text) { return {text}; }

/// Pointer shape while hovering.
///
/// Cursor feedback is how a UI tells you what is interactive BEFORE you click.
/// A text field showing an arrow reads as decoration; an I-beam reads as
/// "you can type here".
struct CursorMod {
    MAYAG_MODIFIER(CursorMod, caps::none, caps::none, caps::none, "cursor");
    CursorShape v;
    constexpr void apply(Style& s) const { s.cursor = v; }
};
[[nodiscard]] constexpr CursorMod cursor(CursorShape c) { return {c}; }
inline constexpr CursorMod pointer_cursor{CursorShape::pointer};
inline constexpr CursorMod text_cursor{CursorShape::text};

/// Semantic state, announced alongside the role, and matched by the cascade
/// (`state::selected`, `state::checked`, ...).
///
/// Each modifier sets ONLY its own field: `selected(true) | disabled(false)`
/// is selected and enabled. (It used to replace the whole state, so the last
/// one piped silently cleared the others: harmless while the state was only
/// announced, a styling bug the moment rules match on it.)
struct StateMod {
    MAYAG_MODIFIER(StateMod, caps::none, caps::none, caps::none, "a11y_state");
    enum Field : std::uint8_t { f_checked, f_selected, f_disabled, f_expanded, f_value };
    Field field;
    bool  on = true;
    float value = 0.0f;
    constexpr void apply(Style& s) const {
        switch (field) {
            case f_checked:  s.a11y_state.checked = on; break;
            case f_selected: s.a11y_state.selected = on; break;
            case f_disabled: s.a11y_state.disabled = on; break;
            case f_expanded: s.a11y_state.expanded = on; break;
            case f_value:    s.a11y_state.value = value; s.a11y_state.has_value = true; break;
        }
    }
};
[[nodiscard]] constexpr StateMod checked(bool on = true)  { return {StateMod::f_checked, on}; }
[[nodiscard]] constexpr StateMod selected(bool on = true) { return {StateMod::f_selected, on}; }
[[nodiscard]] constexpr StateMod disabled(bool on = true) { return {StateMod::f_disabled, on}; }
/// A disclosure (accordion header, tree row, dropdown) is open.
[[nodiscard]] constexpr StateMod expanded(bool on = true) { return {StateMod::f_expanded, on}; }
[[nodiscard]] constexpr StateMod a11y_value(float v) { return {StateMod::f_value, true, v}; }

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

/// Indexed id for a dynamic list row: `node_id("row", i)`. Folds the index into
/// the base hash without building a string, so the SAME call in the view loop
/// and the subscribe loop yields the same id — no `"row-" + std::to_string(i)`
/// spelled twice and no chance of the two drifting.
[[nodiscard]] constexpr std::uint64_t node_id(std::string_view base, std::size_t index) noexcept {
    std::uint64_t hash = node_id(base);
    hash ^= 0x9E3779B97F4A7C15ull + index;   // mix the index in (golden-ratio salt)
    hash *= 1099511628211ull;
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
/// Indexed row id in the view, matching `node_id(base, index)` in subscribe:
/// `row_box | id_of("row", i)`.
[[nodiscard]] constexpr Id id_of(std::string_view base, std::size_t index) {
    return {node_id(base, index)};
}
/// When the caller already hashed the name (a widget that took a node id as a
/// parameter), skip re-hashing.
[[nodiscard]] constexpr Id id_of_raw(std::uint64_t v) { return {v}; }

// ════════════════════════════════════════════════════════════════════════
// Composition
// ════════════════════════════════════════════════════════════════════════

/// A reusable bundle of modifiers — the mayag equivalent of a CSS class.
/// Applying a bundle is checked exactly as strictly as applying each piece,
/// in order (see check_modifier).
template <Modifier... Ms>
struct Bundle {
    static constexpr bool is_composite = true;
    static constexpr caps::Set needs = (caps::none | ... | Ms::needs);
    static constexpr caps::Set gives = (caps::none | ... | Ms::gives);
    static constexpr caps::Set bans  = (caps::none | ... | Ms::bans);
    static constexpr std::string_view label = "style bundle";
    template <caps::Set C>
    static consteval void check() {
        if constexpr (sizeof...(Ms) > 0) check_sequence<C, true, Ms...>();
    }

    std::tuple<Ms...> mods;
    constexpr void apply(Style& s) const {
        std::apply([&](const auto&... m) { (m.apply(s), ...); }, mods);
    }
};

/// Conditional modifier — applies only when the flag is set, without losing
/// the capability check (the check happens regardless of the runtime flag,
/// which is what you want: correctness should not depend on a bool).
template <Modifier M>
struct When {
    static constexpr bool is_composite = true;
    static constexpr caps::Set needs = M::needs;
    static constexpr caps::Set gives = M::gives;
    static constexpr caps::Set bans  = M::bans;
    static constexpr std::string_view label = M::label;
    template <caps::Set C>
    static consteval void check() { check_modifier<C, M>(); }

    bool cond;
    M    mod;
    constexpr void apply(Style& s) const { if (cond) mod.apply(s); }
};
template <Modifier M>
[[nodiscard]] constexpr auto when(bool cond, M m) { return When<M>{cond, m}; }

// ════════════════════════════════════════════════════════════════════════
// The cascade: state variants, classes, variables, transitions
// ════════════════════════════════════════════════════════════════════════
//
// A view describes how an element looks in EVERY state; scene/cascade.hpp
// decides which state it is in each frame. Nothing here reads the pointer:
//
//     constexpr auto button = styles(
//         pad(8, 14) | radius(8) | bg(var::surface_raised) | fg(var::text_primary),
//         hovered(bg(var::tint(var::accent, 0.18f))),
//         pressed(bg(var::accent), fg(var::on_accent), scale(0.97f)),
//         focus_visible(border(2, var::focus_ring)),
//         when(state::disabled, opacity(0.45f)),
//         transition());
//
//     h(text<"Save">) | button | id<"save">
//
// Rules are resolved in a FIXED order (state::priority), never by
// declaration order or selector cleverness: pressed beats hover, disabled
// beats both. See style/state.hpp.

/// Style that applies only while `sel` holds. Built by `when(sel, ...)`,
/// `hovered(...)`, `pressed(...)` and friends.
///
/// Its modifiers are checked against the element like any modifier (a
/// border colour needs a border), except that a variant may REPLACE a value
/// the base already set: `hovered(width(120))` on a sized box is the point.
template <Modifier... Ms>
struct Variant {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set C>
    static consteval void check() {
        if constexpr (sizeof...(Ms) > 0) check_sequence<C, false, Ms...>();
    }

    state::Sel        sel{};
    std::tuple<Ms...> mods{};

    constexpr void on_pipe(Style&) const noexcept {}
    void attach(Cascade& c, std::shared_ptr<const Variant> self) const {
        c.add({sel, state::priority(sel), 0, &run, self.get()});
    }
    static void run(Style& s, const void* data) {
        const auto* v = static_cast<const Variant*>(data);
        std::apply([&](const auto&... m) { (m.apply(s), ...); }, v->mods);
    }
};

/// `when(state::selected && !state::hover, bg(...), fg(...))`
template <Modifier... Ms>
[[nodiscard]] constexpr auto when(state::Sel sel, Ms... ms) {
    return Variant<Ms...>{sel, std::tuple<Ms...>{ms...}};
}

/// Pointer is over the element (or over anything inside it).
template <Modifier... Ms>
[[nodiscard]] constexpr auto hovered(Ms... ms) { return when(state::hover, ms...); }
/// A press began on the element and the pointer is still over it. Dragging
/// off releases the look, which is the cue that letting go will cancel.
template <Modifier... Ms>
[[nodiscard]] constexpr auto pressed(Ms... ms) { return when(state::pressed, ms...); }
/// Has keyboard focus, however it arrived.
template <Modifier... Ms>
[[nodiscard]] constexpr auto focused(Ms... ms) { return when(state::focus, ms...); }
/// Has focus that arrived by KEYBOARD. Where focus rings belong: a ring
/// after every mouse click is noise; no ring after Tab is inaccessible.
template <Modifier... Ms>
[[nodiscard]] constexpr auto focus_visible(Ms... ms) { return when(state::focus_visible, ms...); }
/// Focus is on the element or anywhere inside it (a field's container).
template <Modifier... Ms>
[[nodiscard]] constexpr auto focus_within(Ms... ms) { return when(state::focus_within, ms...); }
/// The first frame the element exists. With `transition()`, this is an
/// entry animation: `starting(opacity(0), translate(0, 8))` fades it in.
template <Modifier... Ms>
[[nodiscard]] constexpr auto starting(Ms... ms) { return when(state::starting, ms...); }
/// The nearest `| group()` ancestor is hovered: a card's chevron that
/// brightens when the CARD is hovered, not only when the chevron is.
template <Modifier... Ms>
[[nodiscard]] constexpr auto group_hovered(Ms... ms) { return when(state::group(state::hover), ms...); }

/// Mark the element as a group: descendants may style on its state with
/// `state::group(...)` / `group_hovered(...)`.
struct GroupMod {
    MAYAG_MODIFIER(GroupMod, caps::none, caps::none, caps::none, "group");
    constexpr void apply(Style& s) const { s.group = true; }
};
/// A function, not a constant: a `group` object would shadow every variable
/// and parameter named `group` in code that uses the DSL.
[[nodiscard]] constexpr GroupMod group() { return {}; }

/// App-defined state N (0..7), matched by `state::flag<N>`. For states the
/// framework cannot know ("dirty", "has error", "is drop target").
template <int N>
    requires (N >= 0 && N < 8)
struct FlagMod {
    MAYAG_MODIFIER(FlagMod, caps::none, caps::none, caps::none, "flag");
    bool on;
    constexpr void apply(Style& s) const {
        if (on) s.flags = static_cast<std::uint8_t>(s.flags | (1u << N));
        else    s.flags = static_cast<std::uint8_t>(s.flags & ~(1u << N));
    }
};
template <int N>
[[nodiscard]] constexpr FlagMod<N> flag(bool on = true) { return {on}; }

/// Spring toward each new resolved style instead of jumping to it.
///
/// Springs, not durations: a hover that ends halfway through its fade-in
/// reverses from where it is, with its velocity, instead of restarting.
///
///     transition()                        the theme's motion_ui
///     transition(var::motion_emphasis)    the theme's lively spring
///     transition(Spring::bouncy())        a fixed spring, whatever the theme
///
/// Theme springs follow the theme's motion_scale, so one setting speeds up,
/// slows down or (at 0) switches off every transition in the app.
struct TransitionMod {
    MAYAG_MODIFIER(TransitionMod, caps::none, caps::none, caps::none, "transition");
    Transition v;
    constexpr void apply(Style& s) const { s.transition = v; }
};
[[nodiscard]] constexpr TransitionMod transition(var::Motion m = var::motion_ui,
                                                 std::uint8_t props = transition_props::all) {
    return {Transition{0.0f, 0.0f, props, static_cast<std::uint8_t>(m)}};
}
[[nodiscard]] constexpr TransitionMod transition(Spring spring,
                                                 std::uint8_t props = transition_props::all) {
    return {Transition{spring.stiffness, spring.damping, props, 0}};
}
[[nodiscard]] constexpr TransitionMod transition(std::uint8_t props) {
    return transition(var::motion_ui, props);
}

/// Resolve this element and everything inside it against another theme:
/// colours, sizes, type, depth, motion and widget looks, all at once.
///
///     v(settings(m)) | theme(themes::daylight)          // a light island
///     sidebar(m)     | theme(derive({.roundness = 0}))  // a square one
///
/// The theme is copied once, when the element is built.
struct ThemeScope {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set>
    static consteval void check() {}

    Theme t;

    constexpr void on_pipe(Style&) const noexcept {}
    void attach(Cascade& c, const std::shared_ptr<const ThemeScope>& self) const {
        c.theme = std::shared_ptr<const Theme>(self, &self->t);
    }
};
[[nodiscard]] constexpr ThemeScope theme(const Theme& t) { return {t}; }

/// Bind theme variables for this element and everything inside it.
///
///     vars(var::accent = rgb<0xF7768E>)            // a red-accented subtree
///     vars(var::named("brand") = rgb<0x00D1B2>)    // an app token
///     vars(var::accent = var::danger)              // alias another role
///
/// Resolved at render time, so theming a subtree costs nothing at view time
/// and a theme switch restyles every token without rebuilding the view.
template <std::size_t N>
struct Vars {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = caps::none;
    template <caps::Set>
    static consteval void check() {}

    std::array<var::TokenDef, N> defs{};

    constexpr void on_pipe(Style&) const noexcept {}
    void attach(Cascade& c, const std::shared_ptr<const Vars>&) const {
        c.vars.insert(c.vars.end(), defs.begin(), defs.end());
    }
};
template <std::same_as<var::TokenDef>... Ds>
[[nodiscard]] constexpr auto vars(Ds... ds) {
    return Vars<sizeof...(Ds)>{{ds...}};
}

/// A CLASS: plain modifiers and responders in one reusable value.
/// `styles(...)` returns a Bundle when given only modifiers, and a Class as
/// soon as one argument responds to state.
template <typename... Xs>
struct Class {
    static constexpr bool is_responder = true;
    static constexpr caps::Set gives = (caps::none | ... | Xs::gives);

    template <caps::Set C, typename X, typename... Rest>
    static consteval void check_items() {
        if constexpr (Responder<X>) X::template check<C>();
        else check_modifier<C, X>();
        if constexpr (sizeof...(Rest) > 0) check_items<C | X::gives, Rest...>();
    }
    template <caps::Set C>
    static consteval void check() {
        if constexpr (sizeof...(Xs) > 0) check_items<C, Xs...>();
    }

    std::tuple<Xs...> items;

    constexpr void on_pipe(Style& s) const {
        std::apply([&](const auto&... x) {
            ([&] {
                if constexpr (Responder<std::remove_cvref_t<decltype(x)>>) x.on_pipe(s);
                else x.apply(s);
            }(), ...);
        }, items);
    }
    void attach(Cascade& c, const std::shared_ptr<const Class>& self) const {
        std::apply([&](const auto&... x) {
            ([&] {
                using X = std::remove_cvref_t<decltype(x)>;
                if constexpr (Responder<X>) x.attach(c, std::shared_ptr<const X>(self, &x));
            }(), ...);
        }, items);
    }
};

/// `constexpr auto card = styles(bg(...), radius(12), elevation(4));`
template <Modifier... Ms>
[[nodiscard]] constexpr auto styles(Ms... ms) {
    return Bundle<Ms...>{std::tuple<Ms...>{ms...}};
}
template <typename... Xs>
    requires ((Responder<Xs> || ...) && ((Responder<Xs> || Modifier<Xs>) && ...))
[[nodiscard]] constexpr auto styles(Xs... xs) {
    return Class<Xs...>{std::tuple<Xs...>{xs...}};
}

// ---- rich inline text ----

/// Rich inline text — flow styled fragments across wrapping lines that share a
/// baseline. Each child keeps its own style, so prose can mix weight, size,
/// colour, and italics on one line and wrap like a real paragraph:
///
///     paragraph(text<"The ">, text<"quick "> | bold,
///               text<"brown "> | fg(accent), text<"fox.">) | width(320)
///
/// It is exactly `h(kids...) | wrap | baseline` — the two primitives rich text
/// needs — named so the intent and the correct defaults are one call. Add any
/// further modifiers (width, gap, padding) with the usual `|`.
template <typename... Kids>
[[nodiscard]] constexpr auto paragraph(Kids... kids) {
    return h(std::move(kids)...) | wrap | baseline;
}

}  // namespace mayag::dsl
