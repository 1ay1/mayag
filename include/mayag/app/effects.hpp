#pragma once
// mayag/app/effects.hpp — things only a WINDOW can do, as jaal effects.
//
// jaal runs the core effects (quit, after, task, perform, ...) itself. These
// are the window's, so a program lists the ones it uses in its Cmd:
//
//     using Cmd = jaal::Cmd<Msg, mayag::set_title, mayag::upload_image>;
//
// and the window host (host.hpp) carries them out. `window_cmd<Msg>` is the
// whole set, for a program that doesn't want to list them.

#include "../platform/types.hpp"
#include "../render/draw_list.hpp"

#include <jaal/jaal.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mayag {

struct SetTitle { std::string title; };
using set_title = jaal::pure_fx<SetTitle, "set_title">;

struct WriteClipboard { std::string text; };
using write_clipboard = jaal::pure_fx<WriteClipboard, "write_clipboard">;

struct SetFullscreen { bool on = true; };
using set_fullscreen = jaal::pure_fx<SetFullscreen, "set_fullscreen">;

/// Window-manager requests a client-decorated window needs: start an
/// interactive MOVE from the current press (a title bar drag), minimise,
/// toggle maximised. A host that cannot do one ignores it.
struct WindowAction {
    enum class Kind : std::uint8_t { move, minimize, toggle_maximize } kind = Kind::move;
};
using window_action = jaal::pure_fx<WindowAction, "window_action">;

struct SetCursor { CursorShape shape = CursorShape::arrow; };
using set_cursor = jaal::pure_fx<SetCursor, "set_cursor">;

/// Give keyboard focus to a node (0 clears it).
struct FocusNode { std::uint64_t node_id = 0; };
using focus = jaal::pure_fx<FocusNode, "focus">;

/// Hand pixels to the renderer so `image(id)` draws them. Re-uploading an id
/// replaces it. The pixels travel as jaal::shared, so a decoded frame moves
/// from a worker to the GPU without a copy.
struct UploadImage {
    std::uint32_t                id = 0;
    jaal::shared<ImageData>      image;
};
using upload_image = jaal::pure_fx<UploadImage, "upload_image">;

struct ReleaseImage { std::uint32_t id = 0; };
using release_image = jaal::pure_fx<ReleaseImage, "release_image">;

/// Rewrite rectangles of image `id` in place: only what changed crosses to
/// the GPU. `pixels` packs each rect's RGBA8 rows tightly, rect after rect.
/// `whole` is the full picture, uploaded instead where an in-place update
/// cannot happen (first frame, a resize, a backend without it).
struct PatchData {
    int width = 0, height = 0;
    /// Rows moved first (x, y, w, h, dy: rows y..y+h take what was at
    /// row + dy), on the GPU where the window can (move_image_rows).
    std::optional<std::array<int, 5>> move;
    std::vector<std::array<int, 4>> rects;  // x, y, w, h
    std::vector<std::uint8_t> pixels;
    std::optional<jaal::shared<ImageData>> whole;
};
struct PatchImage {
    std::uint32_t id = 0;
    jaal::shared<PatchData> patch;
};
using patch_image = jaal::pure_fx<PatchImage, "patch_image">;

[[nodiscard]] inline UploadImage upload(std::uint32_t id, ImageData img) {
    return {id, jaal::shared<ImageData>::make(std::move(img))};
}
[[nodiscard]] inline UploadImage upload(std::uint32_t id, jaal::shared<ImageData> img) {
    return {id, std::move(img)};
}

/// Read the system clipboard. The window answers in the same step with
/// `make(text)`, so a paste is a value, not a callback:
///
///     using Cmd = mayag::Cmd<Msg, read_clipboard>;
///     return ReadClipboard<Msg>{[](std::string s) -> Msg { return Pasted{std::move(s)}; }};
template <class Msg> struct ReadClipboard { std::function<Msg(std::string)> make; };
struct read_clipboard {
    static constexpr std::string_view name = "read_clipboard";
    template <class Msg> using type = ReadClipboard<Msg>;
    template <class F, class M>
    static auto fmap(F&& f, ReadClipboard<M> e) -> ReadClipboard<std::invoke_result_t<F, M>> {
        return {[f = std::forward<F>(f), g = std::move(e.make)](std::string s) { return f(g(std::move(s))); }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, ReadClipboard<M> e) -> ReadClipboard<std::invoke_result_t<F, const Id&, M>> {
        return {[id, f = std::forward<F>(f), g = std::move(e.make)](std::string s) { return f(id, g(std::move(s))); }};
    }
};

/// Do something to the world -- write a file, a pty, start a program -- and
/// optionally answer with a message. update() DESCRIBES it; the runtime runs
/// it after update() returns, in the order the Cmd lists its effects. So
/// update stays a pure function of (Model, Msg), and the world sees the
/// effects in exactly the order they were asked for.
///
/// WHERE it runs is the window host's business, and in a window it is never
/// the UI thread: the world is I/O, and I/O takes as long as the world likes
/// (a pipe whose reader is busy, a file a virus scanner holds, ShellExecute
/// waking a browser, a process snapshot). Every world effect goes, in order,
/// to ONE worker -- the host's world lane -- and its answer is folded on a
/// later step. A frame can never wait for one. (Scripted hosts, headless and
/// simulated, run them inline so a test is deterministic.) The lane is
/// drained, bounded, before the program exits, so `batch(save, quit)` still
/// saves.
///
///     using Cmd = mayag::Cmd<Msg, mayag::world>;
///     return Cmd::run([](std::string path, Config c) { save(path, c); }, path, m.cfg);
///     return Cmd::run([](std::string p) -> Msg { return Loaded{read(p)}; }, path);
///
/// The body is CAPTURELESS and takes Sendable arguments, copied when the
/// effect is made: it sees the values update chose, never the model, which
/// may have moved on by the time it runs. It returns void, a Msg, or
/// std::optional<Msg> (the answer is folded next, like any message).
template <class Msg> struct Run { std::function<std::optional<Msg>()> go; };
namespace detail {
// A mapper rides along to the world lane, so it may hold nothing the loop
// can still reach: a captureless function, as jaal requires of a task's.
template <class F, class From>
concept world_mapper = std::is_convertible_v<std::remove_cvref_t<F>, std::invoke_result_t<F, From> (*)(From)>;
}  // namespace detail
struct world {   // the descriptor; mayag::run<P>() is the entry point
    static constexpr std::string_view name = "run";
    template <class Msg> using type = Run<Msg>;
    template <class F, class M>
    static auto fmap(F&& f, Run<M> e) -> Run<std::invoke_result_t<F, M>> {
        using To = std::invoke_result_t<F, M>;
        static_assert(detail::world_mapper<F, M>,
                      "mayag: Cmd::map over a Cmd holding a world effect needs a captureless mapper "
                      "(it runs on the world lane, off the UI thread)");
        To (*fp)(M) = f;
        return {[fp, g = std::move(e.go)]() -> std::optional<To> {
            if (auto m = g()) return fp(std::move(*m));
            return std::nullopt;
        }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, Run<M> e) -> Run<std::invoke_result_t<F, const Id&, M>> {
        using To = std::invoke_result_t<F, const Id&, M>;
        static_assert(jaal::Sendable<Id>, "mayag: Cmd::map_with over a world effect: the id travels to the world lane, so it must be Sendable");
        static_assert(std::is_convertible_v<std::remove_cvref_t<F>, To (*)(const Id&, M)>,
                      "mayag: Cmd::map_with over a world effect needs a captureless mapper (it runs on the world lane)");
        To (*fp)(const Id&, M) = f;
        return {[id, fp, g = std::move(e.go)]() -> std::optional<To> {
            if (auto m = g()) return fp(id, std::move(*m));
            return std::nullopt;
        }};
    }
    template <class Self, class Msg> struct ctors {
        template <class F, class... A>
            requires std::is_empty_v<F> && std::default_initializable<F> && std::invocable<const F&, A...>
        [[nodiscard]] static Self run(F, A... args) {
            return make<F>(F{}, std::move(args)...);
        }
        /// A named function as the body (`Cmd::run(fx::save, path, cfg)`): a
        /// function pointer captures nothing either, so the rule holds.
        template <class R, class... P, class... A>
            requires std::invocable<R (*)(P...), A...>
        [[nodiscard]] static Self run(R (*f)(P...), A... args) {
            return make<R (*)(P...)>(f, std::move(args)...);
        }

      private:
        template <class F, class... A>
        static Self make(F f, A... args) {
            static_assert((jaal::Sendable<A> && ...),
                          "mayag::run: an argument is not Sendable -- pass owned values (the body must not "
                          "reach the model or anything else update can see later)");
            using R = std::invoke_result_t<const F&, A...>;
            static_assert(std::is_void_v<R> || std::convertible_to<R, Msg> || std::same_as<R, std::optional<Msg>>,
                          "mayag::run: the body returns void, a Msg, or std::optional<Msg>");
            return Self(Run<Msg>{[f, ... a = std::move(args)]() mutable -> std::optional<Msg> {
                if constexpr (std::is_void_v<R>) {
                    f(std::move(a)...);
                    return std::nullopt;
                } else if constexpr (std::same_as<R, std::optional<Msg>>) {
                    return f(std::move(a)...);
                } else {
                    return Msg(f(std::move(a)...));
                }
            }});
        }
    };
};

/// Every window effect, for a program that wants the whole set.
template <class Msg>
using window_cmd = jaal::Cmd<Msg, set_title, write_clipboard, set_fullscreen, set_cursor,
                             focus, upload_image, release_image, patch_image, read_clipboard, window_action, world>;

}  // namespace mayag

// A Run is safe to move to the world lane. It holds one std::function, which
// jaal can't see into, and every way to build one is checked: Cmd::run takes a
// captureless body (or a function pointer) and Sendable arguments, moved in;
// world::fmap / fmap_with add only a function pointer and a Sendable id. So
// nothing inside reaches the model or the loop. Keep it that way: a new
// constructor for Run must check the same two things.
namespace jaal {
template <class Msg> inline constexpr bool sendable_opt_in<mayag::Run<Msg>> = true;
}  // namespace jaal
