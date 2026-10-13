#define NOMINMAX
#include "../probes/gallery_effects.hpp"
#include "../probes/gallery_scene_session.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

// The CRPG UI's nested tooltip page (11.3d) as a scene: the captured component under the gallery harness
// (tools/build_tooltip_page.mjs), body as the scene's root so the tooltips the page portals there show, its own
// stylesheet (tooltip-utilities.css) and the smoke as the crpg-smoke effect. Hovering a keyword opens its tooltip on
// body beside it; a keyword inside the tooltip opens the next; holding still for 3 s pins a window inside #root, which
// moves when dragged by its title; the native pass draws the smoke behind the item cards and the tooltips over the page.
namespace {
    using weft::testing::require;

    constexpr float width = 1596;
    constexpr float height = 800;
    constexpr double frame_ms = 1000.0 / 60;

    struct Rect {
        float x{};
        float y{};
        float w{};
        float h{};
    };

    Rect rect(GallerySceneSession& s, const std::string& expression) {
        std::istringstream in(s.vm.eval("(()=>{const r=(" + expression +
                                        ").getBoundingClientRect();return r.x+' '+r.y+' '+r.width+' '+r.height})()"));
        Rect r;
        in >> r.x >> r.y >> r.w >> r.h;
        return r;
    }

    void run(GallerySceneSession& s, double until) {
        while (s.time + frame_ms <= until + 1e-6) {
            s.step(s.time + frame_ms);
        }
    }

    std::size_t effects(const weft::SceneNode& n) {
        std::size_t count = n.effect ? 1 : 0;
        for (const auto& c : n.children) {
            count += effects(c);
        }
        return count;
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        GallerySceneSession s("TOOLTIP_TESTS", 0, width, height,
                              GallerySceneOptions{"tooltip-utilities.css", "TOOLTIP_TESTS.js", "body",
                                                  "position:relative;overflow:hidden;background-color:#050403",
                                                  &probes::gallery_effects()});
        run(s, 500);
        require(s.vm.eval("document.querySelectorAll('[data-keyword-span]').length") != "0", "keywords on the page");
        std::cout << "smoke effects " << effects(s.current.root) << ", keywords "
                  << s.vm.eval("document.querySelectorAll('[data-keyword-span]').length") << '\n';
        require(effects(s.current.root) == 3, "the three item cards' smoke as effects");

        // Hover the first keyword: its tooltip opens on body, beside it.
        const auto keyword = rect(s, "document.querySelectorAll('[data-keyword-span]')[0]");
        s.input_value(weft::MouseType::Move, keyword.x + keyword.w / 2, keyword.y + keyword.h / 2);
        run(s, s.time + 200);
        require(s.vm.eval("document.body.children.length") == "2", "the tooltip portals to body");
        const auto tooltip = rect(s, "document.body.children[1]");
        std::cout << "keyword (" << keyword.x << ", " << keyword.y << ", " << keyword.w << ", " << keyword.h
                  << "), tooltip (" << tooltip.x << ", " << tooltip.y << ", " << tooltip.w << ", " << tooltip.h
                  << ")\n";
        require(std::abs(tooltip.x - (keyword.x + keyword.w + 8)) < 1 && std::abs(tooltip.y - (keyword.y - 4)) < 1 &&
                    tooltip.w == 256 && tooltip.h > 40,
                "the tooltip fixed beside its keyword, 16 rem wide");

        // The native pass: the tooltip's panel over the page, the smoke behind a card.
        weft::testing::D3D11Scene d3d11{unsigned(width), unsigned(height)};
        const auto native = d3d11.draw(s.current);
        d3d11.gpu.check_debug_messages();
        const auto at = [&](float x, float y) {
            const auto* p = native.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
            return std::array<int, 3>{p[2], p[1], p[0]};
        };
        const auto inside = at(tooltip.x + tooltip.w / 2, tooltip.y + 4);
        std::cout << "tooltip pixel " << inside[0] << ' ' << inside[1] << ' ' << inside[2] << '\n';
        require(at(4, 4) == std::array<int, 3>{5, 4, 3}, "the page over the gallery's backdrop on body");
        require(inside != std::array<int, 3>{5, 4, 3}, "the native pass draws the tooltip");

        // Into the tooltip and onto a keyword inside it: the next tooltip opens.
        const auto nested = rect(s, "document.body.children[1].querySelector('[data-keyword-span]')");
        s.input_value(weft::MouseType::Move, tooltip.x + tooltip.w / 2, tooltip.y + 10);
        run(s, s.time + 100);
        s.input_value(weft::MouseType::Move, nested.x + nested.w / 2, nested.y + nested.h / 2);
        run(s, s.time + 200);
        require(s.vm.eval("document.body.children.length") == "3", "a nested tooltip opens on body");

        // Holding still for 3 s pins the nested keyword as a window inside #root.
        run(s, s.time + 3100);
        require(s.vm.eval("document.querySelectorAll('[data-pinned-tooltip]').length") == "1", "a pinned window");
        const auto pinned = rect(s, "document.querySelector('[data-pinned-tooltip]')");
        const auto handle = rect(s, "document.querySelector('[data-pinned-tooltip] .cursor-grab')");
        std::cout << "pinned (" << pinned.x << ", " << pinned.y << ", " << pinned.w << ", " << pinned.h << ")\n";

        // Dragged by its title, it moves with the pointer.
        const float hx = handle.x + handle.w / 2;
        const float hy = handle.y + handle.h / 2;
        s.input_value(weft::MouseType::Down, hx, hy, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Move, hx + 60, hy + 30, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Move, hx + 120, hy + 70, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Up, hx + 120, hy + 70);
        run(s, s.time + 50);
        const auto moved = rect(s, "document.querySelector('[data-pinned-tooltip]')");
        std::cout << "moved (" << moved.x << ", " << moved.y << ")\n";
        require(std::abs(moved.x - pinned.x - 120) < 0.5 && std::abs(moved.y - pinned.y - 70) < 0.5,
                "the pinned window follows the drag");
        std::cout << "PASS the nested tooltip page as a scene: portals, nested tooltips, pin, drag, native pass\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
