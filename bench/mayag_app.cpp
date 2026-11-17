// bench/mayag_app.cpp — mayag version of the comparison app.
//   mayag_bench_app [animate]
#include <mayag/mayag.hpp>
#include <mayag/widget/scroll_view.hpp>
#include <cmath>
#include <cstring>
#include <string>

using namespace mayag;
using namespace mayag::dsl;

static bool g_animate = false;

struct BenchApp {
    struct Model { double t = 0.0; ScrollState list{}; };
    struct Tick { double dt; }; struct Quit {};
    using Msg = std::variant<Tick, Quit>;
    using Cmd = mayag::Cmd<Msg, set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model&) { return SetTitle{"bench"}; }
    static Cmd update(Model& m, Tick t) { m.t += t.dt; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }
    static std::optional<Msg> on(const Model&, const CloseRequest&) { return Quit{}; }
    static Sub subscribe(const Model&) {
        return when(g_animate, Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }
    static Node view(const Model& m) {
        const Theme& t = ctx().theme;
        std::vector<Node> rows;
        rows.reserve(200);
        for (int i = 0; i < 200; ++i)
            rows.push_back(v(text_owned("Track " + std::to_string(i)) | fg(t.text_primary),
                             text<"Artist name"> | fg(t.text_secondary)).build());
        const float frac = static_cast<float>(0.5 + 0.5 * std::sin(m.t * 3.0));
        return (v(text<"Library"> | font(28) | bold | fg(t.text_primary),
                  z(box() | height(6) | width(pct(100)) | bg(t.surface_raised) | radius(3),
                    box() | height(6) | width(pct(100 * frac)) | bg(t.accent) | radius(3)),
                  v(scroll_view(ctx(), "list", m.list, node((v(std::move(rows)) | gap(4)).build())) | grow() | min_size(0, 0)) | grow() | min_size(0, 0))
                | gap(8) | pad(12) | bg(t.background))
            .build();
    }
};

int main(int argc, char** argv) {
    g_animate = argc > 1 && std::strcmp(argv[1], "animate") == 0;
    AppConfig cfg;
    cfg.size = {1000, 720};
    return mayag::run<BenchApp>(std::move(cfg));
}
