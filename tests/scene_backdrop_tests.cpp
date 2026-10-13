#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_filter.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <windows.h>

// backdrop-filter as scene elements take it: the filter functions and their matrices before and after the blur, the
// values the scene refuses, the display list's backdrop (before the element's painting, or beneath its group's content
// when its opacity or a descendant's backdrop isolates it), layers, damage and the reference raster's refusal.
namespace {
    using weft::testing::require;
    using Kind = weft::paint::Item::Kind;

    bool close_to(float a, float b) {
        return std::abs(a - b) < 1e-5f;
    }

    weft::SceneBackdropFilter parsed(const char* value) {
        const auto r = weft::scene_backdrop_filter(value);
        require(r && *r, std::string("backdrop-filter ") + value);
        return **r;
    }

    void refused(const char* value, const std::string& expected) {
        const auto r = weft::scene_backdrop_filter(value);
        if (r || r.error().code != weft::Diag::SceneProfile || r.error().message.find(expected) == std::string::npos) {
            std::cerr << value << ": " << (r ? std::string("accepted") : r.error().message) << '\n';
            require(false, std::string("refused backdrop-filter ") + value);
        }
    }

    std::vector<Kind> kinds(const weft::paint::DisplayList& list) {
        std::vector<Kind> out;
        for (const auto& item : list.items) {
            out.push_back(item.kind);
        }
        return out;
    }
} // namespace

int main() {
    try {
        // The reference raster draws with D2D, which needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        // Values: none; a blur alone; color functions before and after it, composed in order.
        require(!*weft::scene_backdrop_filter("none") && !*weft::scene_backdrop_filter(" "), "none");
        const auto blur = parsed("blur(12px)");
        require(blur.blur == 12 && blur.pre == weft::paint::identity_color_matrix &&
                    blur.post == weft::paint::identity_color_matrix,
                "a blur");
        require(parsed("blur(0)").blur == 0 && parsed("blur()").blur == 0, "a blur of 0, or left out");
        const auto saturate = parsed("blur(20px) saturate(180%)");
        require(close_to(saturate.post[0], .213f + .787f * 1.8f) && close_to(saturate.post[1], .715f - .715f * 1.8f) &&
                    saturate.pre == weft::paint::identity_color_matrix,
                "saturate after the blur");
        const auto before = parsed("brightness(50%) blur(4px)");
        require(close_to(before.pre[0], .5f) && close_to(before.pre[6], .5f) && close_to(before.pre[18], 1) &&
                    before.post == weft::paint::identity_color_matrix && before.blur == 4,
                "brightness before the blur");
        // Composed in order: brightness then contrast is contrast's slope times brightness's, and contrast's offset.
        const auto chain = parsed("brightness(2) contrast(0.5)");
        require(close_to(chain.pre[0], 1) && close_to(chain.pre[4], .25f) && chain.blur == 0,
                "functions composed in order");
        require(close_to(parsed("opacity(0.25)").pre[18], .25f) && close_to(parsed("opacity(3)").pre[18], 1),
                "opacity, capped at 1");
        require(close_to(parsed("grayscale()").pre[0], .2126f) && close_to(parsed("grayscale(2)").pre[1], .7152f),
                "grayscale, 1 when left out, capped");
        require(close_to(parsed("invert(1)").pre[0], -1) && close_to(parsed("invert(1)").pre[4], 1), "invert");
        require(close_to(parsed("hue-rotate(0.5turn)").pre[0], .213f - .787f) &&
                    close_to(parsed("hue-rotate(180deg)").pre[0], .213f - .787f) &&
                    close_to(parsed("hue-rotate(0)").pre[0], 1),
                "hue-rotate's units");
        refused("blur(2px) blur(3px)", "one blur()");
        refused("blur(-2px)", "blur() takes a length");
        refused("blur(2em)", "blur() takes a length");
        refused("blur(300px)", "blur() takes a length");
        refused("saturate(-1)", "saturate() takes a number or a percentage");
        refused("drop-shadow(1px 1px red)", "drop-shadow() is not a filter function");
        refused("url(#f)", "url() is not a filter function");
        refused("hue-rotate(10)", "hue-rotate() takes an angle");
        refused("blur", "filter functions only");

        // A scene: the stripes behind, a glass card over them, a translucent card with content, and a card whose
        // descendant also filters its backdrop.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:300px;height:200px;background-color:#202020");
        auto& glass = s.html("div");
        s.set(glass, "style",
              "position:absolute;left:10px;top:10px;width:80px;height:60px;border-radius:12px;"
              "backdrop-filter:blur(10px) saturate(150%);background-color:rgba(255,255,255,0.1)");
        s.tree.insert(*s.root, glass, nullptr);
        auto& translucent = s.html("div");
        s.set(translucent, "style",
              "position:absolute;left:100px;top:10px;width:80px;height:60px;opacity:0.5;backdrop-filter:blur(4px)");
        s.tree.insert(*s.root, translucent, nullptr);
        auto& label = s.html("div");
        s.set(label, "style", "width:20px;height:10px;background-color:#ff0000");
        s.tree.insert(translucent, label, nullptr);
        auto& outer = s.html("div");
        s.set(outer, "style",
              "position:absolute;left:190px;top:10px;width:100px;height:100px;backdrop-filter:blur(2px)");
        s.tree.insert(*s.root, outer, nullptr);
        auto& inner = s.html("div");
        s.set(inner, "style", "position:absolute;left:10px;top:10px;width:50px;height:50px;backdrop-filter:blur(3px)");
        s.tree.insert(outer, inner, nullptr);
        auto& text = weft::testing::thread_scene_text();
        weft::SceneSolver solver("", "", text);
        const auto f = solver.frame(s.tree.snapshot(*s.root), 300, 200, 0);
        const auto listed = weft::scene_display_list(f, {}, &text, 1);
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 4)), "a native list");
        const auto& list = listed.list;
        // Root box; the glass: its backdrop, then its box; the translucent card's group: its label, its backdrop
        // beneath, the end; the outer card's group: its box-less self, the inner card's backdrop over the group's
        // content, the outer backdrop beneath, the end.
        const std::vector<Kind> expected{Kind::Box,      Kind::Backdrop, Kind::Box,      Kind::BeginGroup,
                                         Kind::Box,      Kind::Backdrop, Kind::EndGroup, Kind::BeginGroup,
                                         Kind::Backdrop, Kind::Backdrop, Kind::EndGroup};
        if (kinds(list) != expected) {
            for (const auto k : kinds(list)) {
                std::cerr << int(k) << ' ';
            }
            std::cerr << '\n';
            require(false, "the backdrops' places in the list");
        }
        const auto& glass_backdrop = list.backdrops[list.items[1].payload];
        require(glass_backdrop.blur == 10 && !glass_backdrop.under &&
                    glass_backdrop.radii == std::array<float, 4>{12, 12, 12, 12} &&
                    close_to(glass_backdrop.post[0], .213f + .787f * 1.5f) && list.items[1].width == 80 &&
                    list.items[1].transform == weft::paint::Affine{1, 0, 0, 1, 10, 10},
                "the glass card's backdrop in its box");
        require(list.backdrops[list.items[5].payload].under && list.items[6].opacity == .5f,
                "a translucent card's backdrop beneath its group's content");
        require(!list.backdrops[list.items[8].payload].under && list.backdrops[list.items[8].payload].blur == 3 &&
                    list.backdrops[list.items[9].payload].under && list.backdrops[list.items[9].payload].blur == 2 &&
                    list.items[10].opacity == 1,
                "a card whose descendant filters: the descendant reads the group, the card's backdrop beneath it");
        // A card that filters its backdrop contains its fixed descendants, as CSS has it.
        {
            auto& pinned = s.html("div");
            s.set(pinned, "style", "position:fixed;left:5px;top:5px;width:10px;height:10px");
            s.tree.insert(outer, pinned, nullptr);
            weft::SceneSolver containing("", "", text);
            const auto g = containing.frame(s.tree.snapshot(*s.root), 300, 200, 0);
            const auto& placed = g.root.children[2].children[1];
            require(placed.identity == pinned.identity && placed.box == std::array<float, 4>{5, 5, 10, 10},
                    "a fixed descendant of a card that filters its backdrop");
            s.tree.detach(pinned);
        }
        // An inline element's boxes filter what lies behind them, one backdrop per fragment and none for the box of
        // its text.
        {
            auto& line = s.html("div");
            s.set(line, "style", "position:absolute;left:0;top:120px;width:280px;height:30px;font-size:16px");
            s.tree.insert(*s.root, line, nullptr);
            auto& span = s.html("span");
            s.set(span, "style", "backdrop-filter:blur(2px)");
            s.tree.insert(line, span, nullptr);
            auto& words = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            words.data = "glass";
            s.tree.insert(span, words, nullptr);
            weft::SceneSolver inline_solver("", "", text);
            const auto g = inline_solver.frame(s.tree.snapshot(*s.root), 300, 200, 0);
            const auto inline_list = weft::scene_display_list(g, {}, &text, 1);
            // The four cards' and the span's.
            require(inline_list.fallback.empty() && inline_list.list.backdrops.size() == 5,
                    "an inline element's fragment filters what lies behind it");
            s.tree.detach(line);
        }
        // An explicit layer of a card that filters what lies behind it draws in the pass.
        const auto layered = weft::scene_display_list(f, {glass.identity}, &text, 1);
        require(std::none_of(layered.list.items.begin(), layered.list.items.end(),
                             [](const weft::paint::Item& i) {
                                 return i.kind == Kind::Layer;
                             }) &&
                    layered.list.backdrops.size() == 4,
                "a layer that filters its backdrop draws in the pass");
        // Damage: with a backdrop in the frame, any change repaints all of it.
        {
            weft::SceneFrame earlier;
            solver.update(s.tree.snapshot(*s.root), earlier, 300, 200, 16);
            s.set(label, "style", "width:20px;height:10px;background-color:#00ff00");
            auto after = earlier;
            solver.update(s.tree.snapshot(*s.root), after, 300, 200, 32);
            const auto damage = weft::scene_damage(earlier, after, 1);
            require(damage.changed && damage.full, "a change in a frame with backdrops repaints all of it");
            require(!weft::scene_damage(after, after, 1).changed, "no change, no damage");
        }
        // The reference raster refuses backdrops; the native passes draw them.
        {
            weft::SceneRaster reference(text);
            bool refused_raster{};
            try {
                (void)reference.raster(f, 1);
            } catch (const std::exception& e) {
                refused_raster = std::string(e.what()).find("backdrop-filter") != std::string::npos;
            }
            require(refused_raster, "the reference raster refuses backdrop-filter");
        }
        // The solver refuses what the display list could not draw.
        for (const auto& [value, expected_message] : std::vector<std::pair<std::string, std::string>>{
                 {"blur(1px) blur(2px)", "one blur()"}, {"sepia(0.5) url(#x)", "url() is not a filter function"}}) {
            s.set(glass, "style", "width:10px;height:10px;backdrop-filter:" + value);
            weft::SceneSolver refusing("", "", text);
            std::string message;
            try {
                refusing.frame(s.tree.snapshot(*s.root), 300, 200, 0);
            } catch (const weft::DiagnosticError& e) {
                message = e.what();
            }
            require(message.find(expected_message) != std::string::npos, "the solver refuses " + value);
        }
        s.set(glass, "style", "width:10px;height:10px");
        {
            auto& svg = s.svg("svg");
            s.set(svg, "style", "position:absolute;left:0;top:150px;width:40px;height:40px;backdrop-filter:blur(2px)");
            s.tree.insert(*s.root, svg, nullptr);
            weft::SceneSolver svg_solver("", "", text);
            const auto svg_frame = svg_solver.frame(s.tree.snapshot(*s.root), 300, 200, 0);
            const auto svg_list = weft::scene_display_list(svg_frame, {}, &text, 1);
            require(svg_list.fallback.empty() && svg_list.list.backdrops.size() == 4, "an svg root filters");
            auto& rect = s.svg("rect");
            s.set(rect, "style", "backdrop-filter:blur(2px)");
            s.tree.insert(svg, rect, nullptr);
            weft::SceneSolver inside("", "", text);
            std::string message;
            try {
                inside.frame(s.tree.snapshot(*s.root), 300, 200, 0);
            } catch (const weft::DiagnosticError& e) {
                message = e.what();
            }
            require(message.find("backdrop-filter on an SVG element inside svg") != std::string::npos,
                    "an SVG element inside svg does not filter");
        }
        std::cout << "PASS scene backdrops: filter functions and matrices, refusals, places in the display list, "
                     "groups, layers, damage, the reference raster\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
