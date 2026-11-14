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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace mayag {

struct SetTitle { std::string title; };
using set_title = jaal::pure_fx<SetTitle, "set_title">;

struct WriteClipboard { std::string text; };
using write_clipboard = jaal::pure_fx<WriteClipboard, "write_clipboard">;

struct SetFullscreen { bool on = true; };
using set_fullscreen = jaal::pure_fx<SetFullscreen, "set_fullscreen">;

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

[[nodiscard]] inline UploadImage upload(std::uint32_t id, ImageData img) {
    return {id, jaal::shared<ImageData>::make(std::move(img))};
}
[[nodiscard]] inline UploadImage upload(std::uint32_t id, jaal::shared<ImageData> img) {
    return {id, std::move(img)};
}

/// Every window effect, for a program that wants the whole set.
template <class Msg>
using window_cmd = jaal::Cmd<Msg, set_title, write_clipboard, set_fullscreen, set_cursor,
                             focus, upload_image, release_image>;

}  // namespace mayag
