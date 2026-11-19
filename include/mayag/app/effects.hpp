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

/// Every window effect, for a program that wants the whole set.
template <class Msg>
using window_cmd = jaal::Cmd<Msg, set_title, write_clipboard, set_fullscreen, set_cursor,
                             focus, upload_image, release_image, patch_image, read_clipboard, window_action>;

}  // namespace mayag
