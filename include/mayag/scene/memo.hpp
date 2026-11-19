#pragma once
// mayag::MemoCache — reuse unchanged subtrees across frames.
//
// `memo(key, build)` in a view hands back last frame's subtree, already
// cascaded and laid out, when the key matches and nothing the cascade reads
// from outside the subtree changed. Each entry keeps the subtree three ways:
//
//   built     as build() returned it (the fallback when the outside changed)
//   cascaded  after the cascade, before layout (relaid out on a size change)
//   laid_out  last frame's final tree, moved in and out of the tree each
//             frame, so a steady hit copies nothing
//
// The key must cover everything build() reads (model fields, hover queried
// through ctx(), time). What it reads from the cascade's environment is
// checked here.

#include "node.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mayag {

[[nodiscard]] inline std::uint64_t memo_mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

/// Builds a memo() key from the values a subtree depends on.
///
///     MemoKey k; k.add(m.sel).add(m.scroll.offset).add(rows);  memo(k, ...)
class MemoKey {
  public:
    MemoKey& add(std::uint64_t v) noexcept { h_ = memo_mix(h_, v); return *this; }
    // Every other integer width goes through one template: size_t is
    // unsigned long on macOS but uint64_t is unsigned long long, so a fixed
    // set of overloads is ambiguous on one platform or the other. add(int)
    // stays a plain overload so unscoped enums still promote to it.
    MemoKey& add(int v) noexcept { return add(static_cast<std::uint64_t>(static_cast<std::int64_t>(v))); }
    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, std::uint64_t> && !std::is_same_v<T, int>)
    MemoKey& add(T v) noexcept {
        if constexpr (std::is_same_v<T, char>)
            return add(static_cast<std::uint64_t>(static_cast<unsigned char>(v)));
        else if constexpr (std::is_signed_v<T>)
            return add(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
        else
            return add(static_cast<std::uint64_t>(v));
    }
    MemoKey& add(float v) noexcept { std::uint32_t u; std::memcpy(&u, &v, 4); return add(std::uint64_t{u}); }
    MemoKey& add(double v) noexcept { std::uint64_t u; std::memcpy(&u, &v, 8); return add(u); }
    MemoKey& add(Vec2 v) noexcept { return add(v.x).add(v.y); }
    MemoKey& add(Color<Srgb> c) noexcept { return add(c.c0).add(c.c1).add(c.c2).add(c.a); }
    MemoKey& add(std::string_view s) noexcept {
        std::uint64_t x = 0xcbf29ce484222325ull ^ s.size();
        for (unsigned char ch : s) x = (x ^ ch) * 0x100000001b3ull;
        return add(x);
    }
    MemoKey& add(const std::string& s) noexcept { return add(std::string_view{s}); }
    MemoKey& add(const char* s) noexcept { return add(std::string_view{s}); }
    template <class T>
    MemoKey& add(const std::vector<T>& v) {
        add(static_cast<std::uint64_t>(v.size()));
        for (const auto& x : v) add(x);
        return *this;
    }
    [[nodiscard]] operator std::uint64_t() const noexcept { return h_; }

  private:
    std::uint64_t h_ = 0x51ED27A1u;
};

class MemoCache {
  public:
    /// What the cascade handed the subtree's root. Equal envs resolve the
    /// same subtree to the same result.
    struct Env {
        bool          has_parent_text = false;
        TextStyle     parent_text{};
        std::uint64_t scope = 0;          ///< theme + var bindings above
        std::uint32_t group_state = 0;
        bool          has_group = false;
        bool          parent_disabled = false;
        std::uint64_t path = 0;           ///< hit-id path the root was given
        // Interaction that lands INSIDE the subtree (0 when outside).
        std::uint64_t hovered = 0, pressed = 0, focused = 0;
        bool          press_inside = true, focus_visible = false;
        friend bool operator==(const Env&, const Env&) = default;
    };

    struct Entry {
        Node                       built;
        Node                       cascaded;
        Node                       laid_out;
        bool                       has_cascaded = false;
        bool                       has_laid_out = false;
        bool                       taken = false;      ///< handed out this frame
        std::uint64_t              frame = 0;          ///< last frame it was used
        std::uint64_t              path = 0;           ///< hit-id path, once known
        bool                       has_path = false;
        Env                        env{};
        std::vector<std::uint64_t> ids;                ///< every node id inside, sorted
        std::vector<std::uint64_t> starting_ids;       ///< ids with `starting` rules
        bool                       animating = false;  ///< a spring under it moved last frame
        bool                       reads_interaction = false;   ///< build() asked about hover etc.
        std::uint64_t              interaction = 0;             ///< what it saw
    };

    // ── frame lifecycle (the host) ──────────────────────────────────────

    /// Before view(): take last frame's laid-out memo subtrees back out of
    /// the old tree, which view() is about to replace.
    void begin_frame(std::uint64_t frame) {
        frame_ = frame;
        for (const auto& [ptr, key] : live_) {
            if (ptr->memo_key() != key) continue;
            auto it = entries_.find(key);
            if (it == entries_.end() || !it->second.has_cascaded) continue;
            it->second.laid_out = std::move(*ptr);
            it->second.has_laid_out = true;
        }
        live_.clear();
        for (auto& [k, e] : entries_) e.taken = false;
    }

    /// After paint: forget subtrees nobody asked for this frame.
    void end_frame() {
        std::erase_if(entries_, [this](const auto& kv) { return kv.second.frame != frame_; });
        if (sites_.size() > 4096) std::erase_if(sites_, [this](const auto& kv) { return frame_ - kv.second.frame > 600; });
    }

    // ── view side (memo()) ──────────────────────────────────────────────

    /// The reusable subtree for `key`, or nothing (build it).
    /// `interaction` fingerprints the current hover/press/focus, for
    /// subtrees whose build() read them.
    [[nodiscard]] std::optional<Node> take(std::uint64_t key, std::uint64_t interaction) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return std::nullopt;
        Entry& e = it->second;
        if (e.taken) return std::nullopt;   // the same key twice in a frame
        e.taken = true;
        e.frame = frame_;
        if (!e.has_laid_out || !e.has_cascaded) return std::nullopt;
        if (e.reads_interaction && e.interaction != interaction) return std::nullopt;
        e.has_laid_out = false;
        Node n = std::move(e.laid_out);
        n.set_memo_key(key);
        n.set_memo_reused(true);
        n.set_memo_cascaded(&e.cascaded);
        return n;
    }

    /// A freshly built subtree for `key`: remember it as built. False when
    /// the key is already in use this frame (the caller then doesn't cache).
    bool store_built(std::uint64_t key, const Node& built, bool reads_interaction, std::uint64_t interaction) {
        auto [it, fresh] = entries_.try_emplace(key);
        Entry& e = it->second;
        if (!fresh && e.taken && e.frame == frame_ && e.has_cascaded && !e.has_laid_out) {
            // take() already handed this key out this frame.
            return false;
        }
        e.taken = true;
        e.frame = frame_;
        e.built = built;
        e.reads_interaction = reads_interaction;
        e.interaction = interaction;
        e.has_cascaded = false;
        e.has_laid_out = false;
        e.has_path = false;
        e.ids.clear();
        return true;
    }

    void forget(std::uint64_t key) { entries_.erase(key); }

    // ── churn: memo sites whose key changes every frame ─────────────────────────
    //
    // Storing a subtree costs copies of it (built, then cascaded). Worth it
    // when the next frame reuses it; pure overhead when the key changes
    // every frame (a minimap while output streams). A SITE (one memo call
    // in the code, whatever its key) that misses several frames running
    // stops storing -- it just builds -- and tries again every few frames,
    // so it resumes caching as soon as its content settles.

    /// Should this site store what it builds this frame?
    [[nodiscard]] bool worth_storing(std::uint64_t site) {
        Site& s = sites_[site];
        if (s.misses < kChurnAfter) return true;
        return (frame_ - s.last_try) >= kRetryEvery;   // a probe: maybe it settled
    }
    void site_hit(std::uint64_t site) { Site& s = sites_[site]; s.misses = 0; s.frame = frame_; }
    void site_miss(std::uint64_t site, bool stored) {
        Site& s = sites_[site];
        if (s.frame != frame_) ++s.misses;   // count frames, not calls
        s.frame = frame_;
        if (stored) s.last_try = frame_;
    }

    // ── cascade side ────────────────────────────────────────────────────

    [[nodiscard]] Entry* find(std::uint64_t key) noexcept {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Entry* find(std::uint64_t key) const noexcept {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    void set_animating(std::uint64_t key, bool on) noexcept {
        if (Entry* e = find(key)) e->animating = on;
    }

    /// The memo node at `n` is part of this frame's tree.
    void live(Node* n) { live_.emplace_back(n, n->memo_key()); }

    /// `n` was just cascaded fresh: keep a copy and what it was resolved in.
    /// `env` carries everything but the interaction, which is restricted to
    /// the subtree's ids here.
    void cascaded(Node& n, Env env, std::uint64_t hovered, std::uint64_t pressed, std::uint64_t focused,
                  bool press_inside, bool focus_visible) {
        Entry* e = find(n.memo_key());
        if (e == nullptr) return;
        e->ids.clear();
        e->starting_ids.clear();
        collect_ids(n, e->ids, e->starting_ids);
        std::sort(e->ids.begin(), e->ids.end());
        interaction_inside(e->ids, hovered, pressed, focused, press_inside, focus_visible, env);
        e->cascaded = n;
        e->cascaded.set_memo_reused(false);
        e->has_cascaded = true;
        e->env = env;
        live(&n);
    }

    /// Interaction restricted to the subtree, as stored in an Env.
    static void interaction_inside(const std::vector<std::uint64_t>& ids, std::uint64_t hovered,
                                   std::uint64_t pressed, std::uint64_t focused, bool press_inside,
                                   bool focus_visible, Env& env) {
        const auto in = [&](std::uint64_t id) { return id != 0 && std::binary_search(ids.begin(), ids.end(), id); };
        env.hovered = in(hovered) ? hovered : 0;
        env.pressed = in(pressed) ? pressed : 0;
        env.focused = in(focused) ? focused : 0;
        env.press_inside = env.pressed != 0 ? press_inside : true;
        env.focus_visible = env.focused != 0 ? focus_visible : false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  private:
    static void collect_ids(const Node& n, std::vector<std::uint64_t>& out, std::vector<std::uint64_t>& starting) {
        if (const std::uint64_t id = n.style().id; id != 0) {
            out.push_back(id);
            const Cascade* cc = n.cascade().get();
            if (cc != nullptr && (cc->uses & state::bit::starting) != 0) starting.push_back(id);
        }
        for (const auto& k : n.children()) collect_ids(k, out, starting);
    }

    struct Site {
        std::uint32_t misses = 0;     ///< frames in a row without a hit
        std::uint64_t frame = 0;      ///< last frame it was seen
        std::uint64_t last_try = 0;   ///< last frame it stored
    };
    static constexpr std::uint32_t kChurnAfter = 3;
    static constexpr std::uint64_t kRetryEvery = 8;

    std::unordered_map<std::uint64_t, Entry> entries_;
    std::unordered_map<std::uint64_t, Site> sites_;
    std::vector<std::pair<Node*, std::uint64_t>> live_;
    std::uint64_t frame_ = 0;
};

}  // namespace mayag
