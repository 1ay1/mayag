#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  mayag — a GPU UI framework for C++26
//
//  #include <mayag/mayag.hpp> is the framework; widgets are opt-in, one
//  header each, under <mayag/widget/>.
//
//      using namespace mayag;
//      using namespace mayag::dsl;
//
//      constexpr auto card =
//          v(text<"Deploy"> | font(20) | bold | fg(colors::white),
//            text<"3 services"> | font(13) | fg(colors::slate))
//          | gap(6) | pad(20)
//          | bg(rgb<0x16181D>)
//          | border(1, rgb<0x2A2E37>)
//          | radius(14)
//          | elevation(8);
//
//      auto png = mayag::render_to_png(card, {320, 120});
//
//  Design commitments:
//    * Type-state DSL — invalid widget configurations do not compile, and the
//      error message is a sentence, not a template dump.
//    * Colour spaces are types. sRGB, Linear, Oklab and Oklch are distinct;
//      blending is only defined where it is physically correct.
//    * One shape kernel. Every primitive is a signed distance field, so a
//      frame is one draw call and antialiasing is analytic at any zoom.
//    * Backend-agnostic. Vulkan / Metal / D3D12 / WebGPU / OpenGL, plus a
//      pure-CPU reference rasteriser so mayag runs with no GPU at all.
// ─────────────────────────────────────────────────────────────────────────

// ── core ────────────────────────────────────────────────────────────────
#include <mayag/core/math.hpp>
#include <mayag/core/geometry.hpp>
#include <mayag/core/color.hpp>
#include <mayag/core/scroll_state.hpp>
#include <mayag/core/text_edit.hpp>
#include <mayag/core/animation.hpp>
#include <mayag/core/motion.hpp>
#include <mayag/core/history.hpp>

// ── style & scene ───────────────────────────────────────────────────────
#include <mayag/style/style.hpp>
#include <mayag/style/theme.hpp>
#include <mayag/style/state.hpp>
#include <mayag/scene/cascade.hpp>
#include <mayag/scene/node.hpp>
#include <mayag/scene/a11y.hpp>

// ── the DSL ─────────────────────────────────────────────────────────────
#include <mayag/dsl/dsl.hpp>

// ── widgets ─────────────────────────────────────────────────────────────
// Widgets are NOT included here, as in maya. mayag.hpp is the framework:
// the DSL, styling, layout, rendering and the app runtime. Widgets are
// application vocabulary built ON it, one per header, so include the ones
// you use:
//
//   #include <mayag/widget/button.hpp>
//   #include <mayag/widget/scroll_view.hpp>
//   #include <mayag/widget/tab_strip.hpp>
//
// or every one of them, for prototyping:
//
//   #include <mayag/widget/all.hpp>
//
// See include/mayag/widget/ for the full list. Nothing outside that folder
// is a widget, and nothing outside it includes one.

// ── layout ──────────────────────────────────────────────────────────────
#include <mayag/layout/text_metrics.hpp>
#include <mayag/layout/flex.hpp>
#include <mayag/layout/audit.hpp>

// ── render ──────────────────────────────────────────────────────────────
#include <mayag/render/sdf.hpp>
#include <mayag/render/draw_list.hpp>
#include <mayag/render/painter.hpp>
#include <mayag/render/shader_source.hpp>

// ── backends ────────────────────────────────────────────────────────────
#include <mayag/backend/backend.hpp>
#include <mayag/backend/software.hpp>
#include <mayag/backend/tiled.hpp>

// ── app: mayag as a jaal host ────────────────────────────────────────────
// jaal runs the program (update, Cmd, Sub, timers, tasks, streams).
#include <mayag/core/event.hpp>
#include <mayag/app/interaction.hpp>
#include <mayag/app/latency.hpp>
#include <mayag/app/effects.hpp>
#include <mayag/app/sources.hpp>
#include <mayag/app/host.hpp>
#include <mayag/app/app.hpp>

// ── io ─────────────────────────────────────────────────────────────
#include <mayag/image/png.hpp>

// ── text ───────────────────────────────────────────────────────────
//
// One engine. `typo::FontStack` parses TrueType/OpenType, kerns, chains a
// complete per-script fallback, and rasterises through the SDF atlas. When a
// caller supplies no fonts, the helpers below discover the system's — and,
// on a machine with none, a synthesized last-resort face keeps text legible.
// There is no second text path to keep in sync.
#include <mayag/font/font.hpp>
#include <mayag/font/system.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mayag {

/// Options for the one-call rendering helpers.
struct RenderOptions {
    Color<Srgb> background = rgb<0x0B0D10>;
    float       dpi_scale  = 1.0f;
    bool        debug_bounds = false;

    /// The font engine. When null, the helpers build one with
    /// `typo::system::default_stack()` — so text renders correctly with no
    /// configuration at all.
    std::shared_ptr<typo::FontStack> fonts = nullptr;

    /// Resolves `var::` tokens and `state::dark` rules. A one-shot render has
    /// no pointer, so every element renders in its rest state.
    Theme theme = themes::midnight;
};

namespace detail {
// `StackBindings` lives in app/app.hpp — the runtime needs it too, and one
// definition serves both the one-call helpers and the event loop.
}  // namespace detail

/// Lay out, paint, and rasterise a UI into an RGBA8 buffer.
/// Accepts either a DSL expression or an already-built `Node`.
namespace detail {
/// A FontStack for one render_to_pixels call: borrowed from a process-wide
/// pool of forks of the default stack, and returned when the call ends.
class StackLoan {
  public:
    StackLoan() = default;
    StackLoan(const StackLoan&) = delete;
    StackLoan& operator=(const StackLoan&) = delete;
    ~StackLoan() {
        if (s_) pool().with([](Spare& v, Shared s) { v.push_back(std::move(s)); }, Shared{std::move(s_)});
    }
    [[nodiscard]] std::shared_ptr<typo::FontStack> borrow() {
        auto spare = pool().with([](Spare& v) -> std::optional<Shared> {
            if (v.empty()) return std::nullopt;
            Shared s = std::move(v.back());
            v.pop_back();
            return s;
        });
        s_ = spare ? std::move(spare->stack) : origin().stack->fork();
        return s_;
    }

  public:
    /// One owner moves a stack in and out of the pool; nobody else holds it
    /// meanwhile (the loan is the only reference while borrowed).
    struct Shared { std::shared_ptr<typo::FontStack> stack; };   // Sendable: see below
    using Spare = std::vector<Shared>;
    static Cell<Spare>& pool() { static Cell<Spare> p; return p; }
    /// Fonts are discovered once per process; every loan is a fork of this.
    static const Shared& origin() { static const Shared o{typo::system::default_stack()}; return o; }
    std::shared_ptr<typo::FontStack> s_;
};
}  // namespace detail
}  // namespace mayag
// A pooled stack has ONE user: the pool, or the one StackLoan that took it
// out (shared_ptr only because FontStack's owners are shared_ptr). Moving it
// in and out of the pool shares nothing, and each user draws under its own
// DrawLease -- so the handle may be sent between threads.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::detail::StackLoan::Shared> = true;
namespace mayag {

template <typename Ui>
[[nodiscard]] std::vector<std::uint8_t> render_to_pixels(const Ui& ui, Vec2 viewport,
                                                         const RenderOptions& opts = {}) {
    Node root = [&] {
        if constexpr (std::same_as<std::remove_cvref_t<Ui>, Node>) return ui;
        else return ui.build();
    }();

    // Discover the system fonts when the caller supplied none, so a bare
    // `render_to_pixels(ui, size)` call renders real text. The result is
    // cached across calls: enumerating fonts once per process, not per frame.
    // Concurrent calls each draw through a stack of their own: a pool of
    // forks of one discovered stack (forks share faces, own their atlases),
    // borrowed for the call and returned after.
    std::shared_ptr<typo::FontStack> stack = opts.fonts;
    detail::StackLoan loan;
    if (stack == nullptr) stack = loan.borrow();
    detail::StackBindings bound{*stack};

    const layout::TextMeasurer& measurer =
        static_cast<const layout::TextMeasurer&>(bound.measurer);

    cascade_static(root, opts.theme, viewport);
    layout::layout_tree(root, viewport, measurer);

    DrawList dl;
    render::PaintOptions po{};
    po.dpi_scale    = opts.dpi_scale;
    po.measurer     = &measurer;
    po.debug_bounds = opts.debug_bounds;
    po.glyphs       = static_cast<const render::GlyphRenderer*>(&bound.glyphs);
    po.clear_color  = opts.background;
    render::paint(root, dl, po);

    const int w = static_cast<int>(viewport.x * opts.dpi_scale);
    const int h = static_cast<int>(viewport.y * opts.dpi_scale);
    backend::Framebuffer fb{w, h};

    const backend::CoverageSampler* sampler =
        static_cast<const backend::CoverageSampler*>(&bound.sampler);

    // Tiled + parallel. Bit-identical to the reference path (asserted by
    // tests), so there is no reason to take the slow one.
    backend::Tiled::render(dl, fb, sampler, &backend::shared_pool(), opts.background);

    std::vector<std::uint8_t> out;
    backend::Tiled::encode_parallel(fb, out, &backend::shared_pool());
    return out;
}

/// Render straight to a .png file. The fastest way to see a mayag UI.
template <typename Ui>
bool render_to_png(const Ui& ui, Vec2 viewport, const std::string& path,
                   const RenderOptions& opts = {}) {
    const auto pixels = render_to_pixels(ui, viewport, opts);
    return imageio::write_png(path, pixels,
                            static_cast<int>(viewport.x * opts.dpi_scale),
                            static_cast<int>(viewport.y * opts.dpi_scale));
}

}  // namespace mayag
