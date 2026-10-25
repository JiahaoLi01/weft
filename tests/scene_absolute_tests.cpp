#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

// Absolutely positioned boxes of auto width in scenes against Chrome (tools/browser_scene_absolute_reference.mjs,
// tests/fixtures/scene_absolute_reference.json): every element's bounding client rect, the boxes shrunk to fit the
// width their containing block leaves beside their insets, or their content's min-content width when that is wider
// (CSS 2 10.3.7). A containing block 1px wide gave them 1px (nowrap text past its box, inline-blocks from its left
// instead of at the right inset); a wide one gave them all of its width, so text did not wrap at the inset.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    void build(weft::testing::NativeScene& s, weft::dom::Node& parent, const JsonValue& child) {
        if (child.kind == JsonValue::Kind::String) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = child.string;
            s.tree.insert(parent, t, nullptr);
            return;
        }
        auto& e = s.html(child["tag"].string);
        s.set(e, "style", child["style"].string);
        s.tree.insert(parent, e, nullptr);
        for (const auto& c : child["children"].array) {
            build(s, e, c);
        }
    }
} // namespace

int main() {
    try {
        auto& scene_text = weft::testing::thread_scene_text();
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(weft::testing::source_path("tests/fixtures/scene_absolute_reference.json")));
        const unsigned width = unsigned(fixture["width"].number);
        const unsigned height = unsigned(fixture["height"].number);

        // The page: the tree's root is the scene's.
        weft::testing::NativeScene s;
        const auto& tree = fixture["tree"];
        s.set(*s.root, "style", tree["style"].string);
        for (const auto& c : tree["children"].array) {
            build(s, *s.root, c);
        }
        weft::SceneSolver solver("", "", scene_text);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
        weft::SceneGeometry geometry(scene_text);
        const auto elements = geometry.geometry(frame);
        const auto& rects = fixture["rects"].array;
        require(elements.size() == rects.size(), "an element for each of Chrome's");
        constexpr double tolerance = 1.0 / 64 + 1e-3;
        std::size_t differ{};
        for (std::size_t i = 0; i < rects.size(); ++i) {
            const auto& r = elements[i].rect;
            double worst{};
            for (std::size_t k = 0; k < 4; ++k) {
                worst = std::max(worst, std::abs(r[k] - rects[i][k].number));
            }
            if (worst > tolerance) {
                ++differ;
                std::cout << "  element " << i << ": (" << r[0] << ", " << r[1] << ", " << r[2] << ", " << r[3]
                          << "), Chrome (" << rects[i][0].number << ", " << rects[i][1].number << ", "
                          << rects[i][2].number << ", " << rects[i][3].number << ")\n";
            }
        }
        std::cout << rects.size() << " elements against " << fixture["browser"].string << ": " << differ << " differ\n";
        require(differ == 0, "absolutely positioned boxes as wide as Chrome makes them, where it places them");
        std::cout << "PASS scene absolute boxes: shrink-to-fit widths against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
