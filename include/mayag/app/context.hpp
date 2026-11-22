#pragma once
// mayag/app/context.hpp — what view() and every widget may know besides the model.
//
//   ctx()            the frame's Ctx: size, dpi, time, theme, hover/press/focus,
//                    where overlays and tracked springs go
//   memo(key, f)     reuse a subtree across frames
//   themed(t, f)     build a subtree in another theme
//   is(g, "name")    which node a gesture is on
//
// Runtime-free: no jaal, no window, no host. The host (app/host.hpp) fills a
// Ctx each frame; widgets and views only read it. This is the line that
// keeps widgets from depending on the runtime that drives them.

#include "../core/motion.hpp"
#include "../dsl/dsl.hpp"
#include "../layout/text_metrics.hpp"
#include "../platform/types.hpp"
#include "../scene/memo.hpp"
#include "../scene/node.hpp"
#include "../scene/overlay.hpp"
#include "../style/theme.hpp"
#include "interaction.hpp"
#include "latency.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mayag {

// ── what view() may know ────────────────────────────────────────────────

/// Everything `view()` may know about the outside world, besides the model.
/// The view reads hover/press/focus from here instead of storing them, and
/// posts overlays and tracked springs into it.
struct Ctx {
    Vec2   size{};
    float  dpi_scale = 1.0f;
    double time = 0.0;          ///< seconds since start
    Theme  theme = themes::midnight;

    /// Last frames' measured budget, so an app can draw its own latency.
    const LatencyStats* latency = nullptr;

    /// Measured input to photon (the compositor's presentation feedback),
    /// when the window can report it.
    platform::PhotonStats photon{};

    const layout::TextMeasurer* text_measurer = nullptr;
    [[nodiscard]] const layout::TextMeasurer& measurer() const {
        return text_measurer != nullptr ? *text_measurer : layout::default_measurer();
    }

    TrackedSet*  tracked  = nullptr;
    OverlayList* overlays = nullptr;
    const Interaction* input = nullptr;
    /// The host's memo cache; null outside a live window.
    MemoCache* memos = nullptr;

    /// Follow a node's measured rect with a spring.
    void track(const Tracked& t, std::uint64_t node) const {
        t.follow(node);
        if (tracked != nullptr) tracked->register_tracked(&t);
        if (t.animating()) request_animation_frame();
    }
    /// Post a menu, modal or tooltip from anywhere in the view.
    void overlay(Overlay o) const {
        if (overlays != nullptr) overlays->add(std::move(o));
    }

    [[nodiscard]] bool hovered(std::uint64_t id) const noexcept { note_read(); return input && input->is_hovered(id); }
    [[nodiscard]] bool pressed(std::uint64_t id) const noexcept { note_read(); return input && input->is_pressed(id); }
    [[nodiscard]] bool focused(std::uint64_t id) const noexcept { note_read(); return input && input->is_focused(id); }
    /// Counts hover/press/focus questions this frame, shared by every copy of
    /// the frame's Ctx; memo() uses it to see whether a subtree depends on them.
    std::uint64_t* interaction_reads = nullptr;
    void note_read() const noexcept { if (interaction_reads != nullptr) ++*interaction_reads; }
    [[nodiscard]] std::uint64_t reads() const noexcept { return interaction_reads != nullptr ? *interaction_reads : 0; }

    /// memo() against this context (its theme, its cache): what a widget
    /// calls on the context it was handed. See mayag::memo below.
    template <class F>
    [[nodiscard]] Node memoize(std::uint64_t key, F&& build) const;
    /// memoize at a named call site that adapts to churn (see below).
    template <class F>
    [[nodiscard]] Node memoize(std::uint64_t site, std::uint64_t key, F&& build) const;
    template <dsl::fixed_string Name>
    [[nodiscard]] bool hovered() const noexcept { return hovered(dsl::node_id(Name.view())); }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool pressed() const noexcept { return pressed(dsl::node_id(Name.view())); }
    template <dsl::fixed_string Name>
    [[nodiscard]] bool focused() const noexcept { return focused(dsl::node_id(Name.view())); }
};

// ── the program shape ───────────────────────────────────────────────────
//
// jaal's view hook is `static Out view(const Model&)`. The window facts a
// view may read (size, theme, hover, ...) are ambient: `mayag::ctx()` inside
// view() returns the frame's Ctx. Outside a frame it returns a default one,
// so calling view() directly in a test still works.

namespace detail {
inline thread_local const Ctx* current_ctx = nullptr;

/// Installs a Ctx for the duration of one view() call.
struct ctx_scope {
    const Ctx* saved;
    explicit ctx_scope(const Ctx& c) noexcept : saved(current_ctx) { current_ctx = &c; }
    ~ctx_scope() { current_ctx = saved; }
    ctx_scope(const ctx_scope&) = delete;
    ctx_scope& operator=(const ctx_scope&) = delete;
};

}  // namespace detail

/// The current frame's Ctx. Call it from view().
[[nodiscard]] inline const Ctx& ctx() noexcept {
    static const Ctx fallback{};
    return detail::current_ctx != nullptr ? *detail::current_ctx : fallback;
}

/// Build a subtree in another theme -- ALL of it, every widget included.
///
///     themed(themes::daylight, [&] { return settings_panel(m); })
///     themed(brutal, [&](const Ctx& c) { return slider(c, "vol", m.vol, 120); })
///
/// Inside `build`, `ctx().theme` IS `t`, so widgets that read the theme
/// directly draw in it; and the result is scoped with `| theme(t)`, so every
/// token, sheet look and motion below resolves against it too. The one call
/// that makes "this part of the app looks different" true for everything.
template <class F>
[[nodiscard]] auto themed(const Theme& t, F&& build) {
    Ctx c = ctx();
    c.theme = t;
    Node n = [&] {
        detail::ctx_scope scope{c};
        if constexpr (std::invocable<F&, const Ctx&>) return Node(build(c));
        else return Node(build());
    }();
    return dsl::node(std::move(n)) | dsl::theme(t);
}

namespace detail {
using mayag::memo_mix;
/// The Ctx facts a subtree can read without them being in its key. The theme
/// goes in by VALUE (MemoCache::theme_id), never by its bytes: Theme has
/// padding, and hashing padding made every key change at random.
[[nodiscard]] inline std::uint64_t ctx_fingerprint(const Ctx& c, MemoCache& cache) {
    std::uint64_t h = 0;
    std::uint32_t u;
    for (float f : {c.size.x, c.size.y, c.dpi_scale}) { std::memcpy(&u, &f, 4); h = memo_mix(h, u); }
    return memo_mix(h, cache.theme_id(c.theme));
}
}  // namespace detail


/// Reuse a subtree across frames while `key` is unchanged.
///
///     memo(MemoKey{}.add(m.rows_version), [&] { return big_list(m); })
///
/// On a hit the subtree comes back already styled and laid out, and the
/// cascade and layout skip it unless something around it changed (theme,
/// inherited text, hover or focus inside it, the space it's given). `key`
/// must cover everything `build` reads from the model, and anything it
/// reads from ctx() besides size, dpi, theme and hover/press/focus (those
/// are tracked: a subtree that asks about hover is rebuilt when it moves).
/// A subtree that posts overlays, tracks a spring or asks for animation
/// frames while building isn't kept.
/// The result is the subtree itself, so it lays out exactly as `build()`'s
/// root would. Styles piped onto it (`node(memo(...)) | width(300)`) go on a
/// plain box around it, never on the cached root.
/// Outside a live window (one-shot renders, tests) it just calls `build`.
template <class F>
[[nodiscard]] Node memo(std::uint64_t key, F&& build) {
    return ctx().memoize(key, std::forward<F>(build));
}

template <class F>
Node Ctx::memoize(std::uint64_t key, F&& build) const {
    return memoize(0, key, std::forward<F>(build));
}

/// memo with a SITE: `site` names the call (stable across frames), `key` the
/// content. A site whose key changes every frame stops paying to store what
/// it builds (MemoCache::worth_storing) and resumes once its content holds.
template <class F>
Node Ctx::memoize(std::uint64_t site, std::uint64_t key, F&& build) const {
    const Ctx& c = *this;
    MemoCache* cache = c.memos;
    if (cache == nullptr) return Node(build());   // no live window: nothing to reuse
    const std::uint64_t k = detail::memo_mix(detail::memo_mix(key, detail::ctx_fingerprint(c, *cache)), 1) | 1u;
    // A subtree that asked about hover/press/focus while building depends on
    // them, so a hit also needs them unchanged.
    const std::uint64_t interaction = c.input != nullptr
        ? detail::memo_mix(detail::memo_mix(detail::memo_mix(c.input->hovered(), c.input->pressed()),
                                            c.input->focused()), c.input->press_inside())
        : 0;
    if (cache != nullptr) {
        if (auto hit = cache->take(k, interaction)) {
            if (site != 0) cache->site_hit(site);
            hit->set_memo_result(true);
            return std::move(*hit);
        }
    }
    const bool store = cache != nullptr && (site == 0 || cache->worth_storing(site));
    if (!store) {
        if (cache != nullptr && site != 0) cache->site_miss(site, false);
        return Node(build());   // churning: build it like any subtree, no copies
    }
    const std::uint64_t reads0 = c.reads();
    const std::size_t overlays0 = c.overlays != nullptr ? c.overlays->size() : 0;
    const std::size_t tracked0 = c.tracked != nullptr ? c.tracked->size() : 0;
    const bool anim0 = animation_was_requested();
    clear_animation_request();
    Node n = Node(build());
    const bool side_effects = (c.overlays != nullptr && c.overlays->size() != overlays0) ||
                              (c.tracked != nullptr && c.tracked->size() != tracked0) || animation_was_requested();
    if (anim0) request_animation_frame();
    const bool reads_interaction = c.reads() != reads0;
    if (cache != nullptr && !side_effects && cache->store_built(k, n, reads_interaction, interaction)) n.set_memo_key(k);
    if (site != 0) cache->site_miss(site, true);
    n.set_memo_result(true);
    return n;
}

/// Which node id is a click on? `is(c, "save")` reads like the button name.
template <class G>
[[nodiscard]] bool is(const G& g, std::string_view name) noexcept {
    return g.node == dsl::node_id(name);
}
/// Indexed twin: `is(c, "row", i)` matches `id_of("row", i)` in the view.
template <class G>
[[nodiscard]] bool is(const G& g, std::string_view base, std::size_t index) noexcept {
    return g.node == dsl::node_id(base, index);
}

}  // namespace mayag
