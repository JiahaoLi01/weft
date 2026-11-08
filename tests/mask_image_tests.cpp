#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// Hits on masked elements against Chrome: tests/fixtures/mask_image/page.js (boxes masked by gradient layers,
// tools/browser_mask_image_reference.mjs) in a View, a press at each of the points cases.json lists, and the element
// each reaches (the page logs mousedown targets) against what Chrome's presses reached (hits.json): a mask's values
// change no hit, but its element's border box clips them unless a layer is no-clip. The drawing of the same page is
// tests/mask_image_metal_tests.mm's. Then where groups composite: a masked element's, a faded one's and a blended
// one's over no more than they paint (a mask's per-pixel evaluation over the whole target cost an iPad 1.6 ms a frame
// each), and one around a filtered child over its clip (what it paints is not bounded). Portable.
namespace {
    using weft::testing::require;

    // Presses Chrome reaches the element with that a View does not, for a reason other than the mask: they lie within
    // a pixel outside the edge of the rotated box (Chrome takes such a press for a rotated box without a mask too, and
    // not through clip-path: inset(0); a View tests the box's exact edge), by case name and press.
    struct Known {
        const char* name;
        double x;
        double y;
    };
    constexpr Known known[]{{"image rotated", 607.5, 1367.5}, {"image rotated", 517.5, 1389.5}};

    // The mask images by their paths, resolved against the page's base (the fixture folder).
    std::optional<std::vector<std::uint8_t>> read_resource(std::string_view path) {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
} // namespace

int main() {
    try {
        const auto directory = weft::testing::source_path("tests/fixtures/mask_image");
        const auto manifest = weft::testing::parse_json(weft::testing::read_file(directory / "cases.json"));
        const auto chrome = weft::testing::parse_json(weft::testing::read_file(directory / "hits.json"));
        const auto width = float(manifest["width"].number);
        const auto height = float(manifest["height"].number);
        const auto fonts = std::make_shared<weft::FontCatalog>();
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, width, height, 1};
        view.set_resource_reader(read_resource);
        weft::ViewPage page;
        page.base = directory.generic_string() + "/";
        page.scripts.push_back({"page.js", weft::testing::read_text_file(directory / "page.js")});
        page.stylesheet = "#root{position:relative}";
        page.forgiving_css = true;
        page.web_platform = true;
        view.load(page);
        double time = 16;
        view.advance(time);
        const auto diagnostics = view.take_diagnostics();
        require(diagnostics.empty(), diagnostics.empty() ? std::string() : diagnostics.front().message);
        const auto& points = manifest["points"].array;
        for (const auto& p : points) {
            weft::PointerInput down;
            down.kind = weft::PointerKind::Down;
            down.x = float(p[1].number);
            down.y = float(p[2].number);
            down.buttons = 1;
            view.pointer(down);
            auto up = down;
            up.kind = weft::PointerKind::Up;
            up.buttons = 0;
            view.pointer(up);
            time += 16;
            view.advance(time);
        }
        const auto [ok, log] = view.evaluate("JSON.stringify(window.__hits)");
        require(ok, "the hits read: " + log);
        const auto native = weft::testing::parse_json(log);
        const auto& want = chrome["hits"].array;
        require(native.array.size() == want.size(),
                std::to_string(native.array.size()) + " presses logged of " + std::to_string(want.size()));
        const auto& cases = manifest["cases"].array;
        std::vector<int> wrong(cases.size());
        std::size_t failures{};
        std::size_t known_differences{};
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (native.array[i].string != want[i].string) {
                const auto c = std::size_t(points[i][0].number);
                if (std::any_of(std::begin(known), std::end(known), [&](const Known& k) {
                        return cases[c]["name"].string == k.name && points[i][1].number == k.x &&
                               points[i][2].number == k.y;
                    })) {
                    ++known_differences;
                    continue;
                }
                if (wrong[c]++ < 3) {
                    std::cout << "  " << cases[c]["name"].string << " at (" << points[i][1].number << ", "
                              << points[i][2].number << "): weft " << native.array[i].string << ", Chrome "
                              << want[i].string << '\n';
                }
                ++failures;
            }
        }
        for (std::size_t c = 0; c < cases.size(); ++c) {
            if (wrong[c]) {
                std::cout << "FAIL " << cases[c]["name"].string << ": " << wrong[c] << " presses\n";
            }
        }
        std::cout << want.size() << " presses over " << cases.size() << " cases vs " << chrome["browser"].string
                  << ": " << failures << " reach another element (" << known_differences
                  << " known: within a pixel outside a rotated box's edge)\n";
        require(failures == 0, "mask hits match Chrome");

        // Groups composite over what their subtrees paint (with scene list_cull_guard_px around it), within the page.
        {
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative;width:800px;height:600px;background:#202020");
            const auto box = [&](const std::string& style) -> weft::dom::Node& {
                auto& e = s.html("div");
                s.set(e, "style", "position:absolute;width:60px;height:40px;background:#3b82f6;" + style);
                s.tree.insert(*s.root, e, nullptr);
                return e;
            };
            box("left:100px;top:50px;mask-image:linear-gradient(#000, transparent)");
            auto& faded = box("left:300px;top:50px;opacity:.5");
            auto& child = s.html("div");
            s.set(child, "style", "width:10px;height:10px;background:#ffffff");
            s.tree.insert(faded, child, nullptr);
            box("left:500px;top:50px;mix-blend-mode:multiply");
            box("left:100px;top:300px;mask-image:linear-gradient(#000, transparent);transform:rotate(45deg)");
            auto& filtering = box("left:500px;top:300px;opacity:.5");
            auto& filtered = s.html("div");
            s.set(filtered, "style", "width:10px;height:10px;background:#ffffff;filter:blur(2px)");
            s.tree.insert(filtering, filtered, nullptr);
            const auto fonts = std::make_shared<weft::FontCatalog>();
            weft::SceneText scene_text{*fonts};
            weft::SceneSolver solver("", "", scene_text, true, true);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 800, 600, 0);
            const auto listed = weft::scene_display_list(frame, {}, &scene_text);
            require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 1)), "groups: a list");
            std::vector<std::array<float, 4>> composites;
            for (const auto& item : listed.list.items) {
                if (item.kind == weft::paint::Item::Kind::EndGroup) {
                    composites.push_back(item.clip);
                }
            }
            // Left, top, right, bottom: each box with the guard around it (the rotated one's bounds, its guard turned
            // with it); the group around a filtered child (which paints beyond its box), its clip: the page.
            constexpr float g = 8;
            const float reach = (50 + 2 * g) * std::sqrt(2.f) / 2;
            const std::vector<std::array<float, 4>> expected{{100 - g, 50 - g, 160 + g, 90 + g},
                                                             {300 - g, 50 - g, 360 + g, 90 + g},
                                                             {500 - g, 50 - g, 560 + g, 90 + g},
                                                             {130 - reach, 320 - reach, 130 + reach, 320 + reach},
                                                             {0, 0, 800, 600}};
            if (composites.size() != expected.size()) {
                for (const auto& c : composites) {
                    std::cout << "  group over " << c[0] << ", " << c[1] << ", " << c[2] << ", " << c[3] << '\n';
                }
            }
            require(composites.size() == expected.size(),
                    std::to_string(composites.size()) + " groups of " + std::to_string(expected.size()));
            for (std::size_t k = 0; k < expected.size(); ++k) {
                for (std::size_t c = 0; c < 4; ++c) {
                    if (std::abs(composites[k][c] - expected[k][c]) > .01f) {
                        throw std::runtime_error("group " + std::to_string(k) + " composites over " +
                                                 std::to_string(composites[k][0]) + ", " +
                                                 std::to_string(composites[k][1]) + ", " +
                                                 std::to_string(composites[k][2]) + ", " +
                                                 std::to_string(composites[k][3]));
                    }
                }
            }
            std::cout << "groups composite over what they paint: " << composites.size() << " checked\n";
        }

        std::cout << "PASS mask hits against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
