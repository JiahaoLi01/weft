#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// position: fixed in scenes against Chrome (tools/browser_scene_fixed_reference.mjs,
// tests/fixtures/scene_fixed_reference.json): every element's bounding client rect, fixed boxes placed in the viewport
// or in a box with a transform or will-change: transform; fixed boxes hit and drawn by the native pass where they sit,
// one inside a box that clips and holds it; and the diagnostics where the scene would lay a fixed box out differently
// (its static position away from its containing block), cannot place it (inside an inline-block) or would paint it
// differently (a clip it escapes that does not hold it).
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

    template <class F>
    std::string diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return e.what();
        }
        return {};
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& scene_text = weft::testing::thread_scene_text();
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(weft::testing::source_path("tests/fixtures/scene_fixed_reference.json")));
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
        require(differ == 0, "fixed boxes where Chrome places them");

        // Hit where they sit, the one inside a box that clips and holds it too; drawn there by the native pass.
        const auto at = [&](std::size_t element) {
            const auto& r = elements[element].rect;
            return std::array<float, 2>{float(r[0] + r[2] / 2), float(r[1] + r[3] / 2)};
        };
        // Tree order: 2 the viewport's fixed box, 3 its percentages, 8 the transformed box's, 10 will-change's,
        // 13 the one inside the clip.
        weft::testing::D3D11Scene d3d11(width, height);
        const auto native = d3d11.draw(frame);
        d3d11.gpu.check_debug_messages();
        for (const auto& [element, color] :
             std::vector<std::pair<std::size_t, std::array<int, 3>>>{{2, {0xd4, 0xa8, 0x43}},
                                                                     {3, {0x60, 0xa5, 0xfa}},
                                                                     {8, {0xc0, 0x84, 0xfc}},
                                                                     {10, {0xfb, 0x92, 0x3c}},
                                                                     {13, {0xe0, 0xe0, 0xe0}}}) {
            const auto p = at(element);
            require(geometry.hit_test(frame, p[0], p[1]) == elements[element].identity,
                    "a fixed box is hit where it sits (element " + std::to_string(element) + ")");
            const auto* px = native.data() + (std::size_t(p[1]) * width + std::size_t(p[0])) * 4;
            require(std::abs(px[2] - color[0]) <= 1 && std::abs(px[1] - color[1]) <= 1 &&
                        std::abs(px[0] - color[2]) <= 1,
                    "the native pass draws a fixed box where it sits (element " + std::to_string(element) + ")");
        }

        // Outside the profile.
        const auto refused = [&](const std::string& html_style, const std::string& expected, auto&& make) {
            weft::testing::NativeScene t;
            t.set(*t.root, "style", "position:relative;" + html_style);
            make(t);
            weft::SceneSolver refusing("", "", scene_text);
            const auto message = diagnostic([&] {
                refusing.frame(t.tree.snapshot(*t.root), 200, 100, 0);
            });
            require(message.find(expected) != std::string::npos, "diagnostic: " + expected + " (got " + message + ")");
        };
        const auto child = [](weft::testing::NativeScene& t, weft::dom::Node& parent, const std::string& tag,
                              const std::string& style) -> weft::dom::Node& {
            auto& e = t.html(tag);
            t.set(e, "style", style);
            t.tree.insert(parent, e, nullptr);
            return e;
        };
        refused("", "position: fixed inside an inline-block", [&](weft::testing::NativeScene& t) {
            auto& line = child(t, *t.root, "div", "");
            auto& inline_block = child(t, line, "span", "display:inline-block");
            child(t, inline_block, "div", "position:fixed;left:0;top:0;width:10px;height:10px");
            auto& words = t.tree.create(weft::constants::text_node_type, "#text", "", t.document->identity);
            words.data = " words";
            t.tree.insert(line, words, nullptr);
        });
        refused("", "position: fixed with a static position", [&](weft::testing::NativeScene& t) {
            auto& padded = child(t, *t.root, "div", "padding:10px");
            child(t, padded, "div", "position:fixed;width:10px;height:10px");
        });
        refused("", "position: fixed beyond the overflow clip", [&](weft::testing::NativeScene& t) {
            auto& clipping = child(t, *t.root, "div", "width:50px;height:50px;overflow:hidden");
            child(t, clipping, "div", "position:fixed;left:40px;top:0;width:30px;height:10px");
        });
        // Accepted: a fixed box in a transformed box inside a box that clips (its containing block is inside the clip,
        // which CSS applies to it too), and a fixed box far from its parent in a root whose content must fit (it fits
        // its containing block, the root).
        const auto accepted = [&](const std::string& root_style, auto&& make, const std::string& what) {
            weft::testing::NativeScene t;
            t.set(*t.root, "style", "position:relative;" + root_style);
            make(t);
            weft::SceneSolver accepting("", "", scene_text);
            const auto message = diagnostic([&] {
                accepting.frame(t.tree.snapshot(*t.root), 200, 100, 0);
            });
            require(message.empty(), what + " (got " + message + ")");
        };
        accepted(
            "",
            [&](weft::testing::NativeScene& t) {
                auto& clipping = child(t, *t.root, "div", "width:50px;height:50px;overflow:hidden");
                auto& turned = child(t, clipping, "div", "width:40px;height:40px;transform:rotate(10deg)");
                child(t, turned, "div", "position:fixed;left:30px;top:0;width:30px;height:10px");
            },
            "a fixed box in a transformed box inside a clip");
        accepted(
            "overflow:auto",
            [&](weft::testing::NativeScene& t) {
                auto& distant =
                    child(t, *t.root, "div", "position:absolute;left:150px;top:60px;width:40px;height:30px");
                child(t, distant, "div", "position:fixed;left:10px;top:10px;width:20px;height:20px");
            },
            "a fixed box far from its parent in a root whose content must fit");
        std::cout << "PASS scene position: fixed against Chrome; hits, native pass, diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
