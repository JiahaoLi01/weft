#include "scene/scene_display_list.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif
#ifdef WEFT_TEST_D3D12
#include "support/d3d12_scene.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// SVG shapes drawn as boxes (step 12.2 of docs/refactoring-plan.md): rects with square or circular corners, circles,
// their solid strokes and lines' strokes take box items, which the scene pass draws without rasterizing coverage. Each
// case drawn as a box matches the same shape written as a path (drawn as coverage), pixel for pixel but for the
// antialiasing of their edges, under translation, rotation and a non-uniform scale; shapes a box cannot draw (an
// elliptical corner, a bevel join, a dashed stroke, a line of no length) stay paths. Vulkan draws the boxes as D3D11.
namespace {
    using weft::testing::require;

    constexpr unsigned width = 800;
    constexpr unsigned height = 600;
    constexpr float scale = 2;
    // A box antialiases its edges by distance, a path by area: along a curved or slanted edge some pixels differ by a
    // quarter at most, and few by more than 24 levels. An edge a pixel off, a wrong radius or a missing stroke differs
    // by its whole color along its length.
    constexpr int max_edge_difference = 64;
    constexpr double max_mean_difference = .02;
    constexpr std::size_t max_far_pixels = 150;
    // Level and upright edges alike: rounding apart.
    constexpr int max_exact_difference = 4;

    weft::SceneNode element(std::uint32_t slot, std::string tag,
                            std::initializer_list<std::pair<const char*, const char*>> attributes,
                            std::initializer_list<std::pair<const char*, const char*>> style) {
        weft::SceneNode n;
        n.identity = {slot, 1, 1};
        n.tag = std::move(tag);
        n.space = weft::constants::svg_namespace_uri;
        for (const auto& [k, v] : attributes) {
            n.attributes[k] = v;
        }
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    weft::SceneFrame page(std::vector<weft::SceneNode> elements) {
        weft::SceneFrame f;
        f.root.identity = {1, 1, 1};
        f.root.tag = "div";
        f.root.box = {0, 0, width / scale, height / scale};
        f.root.style["background-color"] = "#ffffff";
        auto svg = element(2, "svg", {{"width", "400"}, {"height", "300"}}, {});
        svg.box = {0, 0, 400, 300};
        svg.children = std::move(elements);
        f.root.children.push_back(std::move(svg));
        return f;
    }

    struct Case {
        const char* name;
        weft::SceneNode shape;
        weft::SceneNode path;
        // Edges only level and upright: a box covers each pixel by its share as the path does, to the level.
        bool exact{};
    };

    // Each shape with the same outline as a path element of the same style.
    std::vector<Case> cases() {
        std::vector<Case> out;
        std::uint32_t slot = 10;
        const auto add = [&](const char* name, std::string tag,
                             std::initializer_list<std::pair<const char*, const char*>> attributes, const char* d,
                             std::initializer_list<std::pair<const char*, const char*>> style,
                             const char* transform = nullptr) {
            Case c{name, element(slot++, std::move(tag), attributes, style),
                   element(slot++, "path", {{"d", d}}, style)};
            if (transform) {
                c.shape.attributes["transform"] = transform;
                c.path.attributes["transform"] = transform;
            }
            c.exact = std::string_view(name).starts_with("rect fill") ||
                      std::string_view(name).starts_with("square stroke, miter") ||
                      std::string_view(name).starts_with("level") || std::string_view(name).starts_with("upright");
            out.push_back(std::move(c));
        };
        add("rect fill", "rect", {{"x", "10"}, {"y", "10"}, {"width", "60"}, {"height", "30"}}, "M10 10 H70 V40 H10 Z",
            {{"fill", "#2a9d8f"}});
        add("rounded rect fill and stroke", "rect",
            {{"x", "90"}, {"y", "10"}, {"width", "60"}, {"height", "40"}, {"rx", "8"}},
            "M98 10 H142 A8 8 0 0 1 150 18 V42 A8 8 0 0 1 142 50 H98 A8 8 0 0 1 90 42 V18 A8 8 0 0 1 98 10 Z",
            {{"fill", "rgba(230, 57, 70, 0.7)"}, {"stroke", "#1d3557"}, {"stroke-width", "3"}});
        add("square stroke, miter join", "rect", {{"x", "170"}, {"y", "10"}, {"width", "50"}, {"height", "30"}},
            "M170 10 H220 V40 H170 Z", {{"fill", "none"}, {"stroke", "#e76f51"}, {"stroke-width", "6"}});
        add("square stroke, round join", "rect", {{"x", "240"}, {"y", "10"}, {"width", "50"}, {"height", "30"}},
            "M240 10 H290 V40 H240 Z",
            {{"fill", "none"}, {"stroke", "#e76f51"}, {"stroke-width", "6"}, {"stroke-linejoin", "round"}});
        add("circle fill and stroke", "circle", {{"cx", "40"}, {"cy", "90"}, {"r", "22"}},
            "M62 90 A22 22 0 1 1 18 90 A22 22 0 1 1 62 90 Z",
            {{"fill", "#f4a261"},
             {"fill-opacity", "0.6"},
             {"stroke", "#264653"},
             {"stroke-width", "4"},
             {"stroke-opacity", "0.7"}});
        add("circle under a stroke wider than it", "circle", {{"cx", "110"}, {"cy", "90"}, {"r", "4"}},
            "M114 90 A4 4 0 1 1 106 90 A4 4 0 1 1 114 90 Z",
            {{"fill", "none"}, {"stroke", "#457b9d"}, {"stroke-width", "12"}});
        add("line, butt caps", "line", {{"x1", "150"}, {"y1", "70"}, {"x2", "210"}, {"y2", "110"}}, "M150 70 L210 110",
            {{"stroke", "#2a9d8f"}, {"stroke-width", "6"}});
        add("line, square caps", "line", {{"x1", "220"}, {"y1", "70"}, {"x2", "280"}, {"y2", "110"}},
            "M220 70 L280 110", {{"stroke", "#2a9d8f"}, {"stroke-width", "6"}, {"stroke-linecap", "square"}});
        add("line, round caps", "line", {{"x1", "300"}, {"y1", "70"}, {"x2", "360"}, {"y2", "110"}}, "M300 70 L360 110",
            {{"stroke", "#2a9d8f"}, {"stroke-width", "6"}, {"stroke-linecap", "round"}});
        add("rotated rect", "rect", {{"x", "20"}, {"y", "140"}, {"width", "50"}, {"height", "26"}},
            "M20 140 H70 V166 H20 Z", {{"fill", "#8338ec"}, {"stroke", "#ff006e"}, {"stroke-width", "3"}},
            "rotate(20 45 153)");
        add("stretched circle", "circle", {{"cx", "140"}, {"cy", "160"}, {"r", "18"}},
            "M158 160 A18 18 0 1 1 122 160 A18 18 0 1 1 158 160 Z", {{"fill", "#3a86ff"}},
            "translate(140 160) scale(2 1) translate(-140 -160)");
        add("hairline", "line", {{"x1", "200"}, {"y1", "150"}, {"x2", "290"}, {"y2", "155"}}, "M200 150 L290 155",
            {{"stroke", "#000000"}, {"stroke-width", "1"}});
        add("level line, butt caps", "line", {{"x1", "20.25"}, {"y1", "200.4"}, {"x2", "120.25"}, {"y2", "200.4"}},
            "M20.25 200.4 L120.25 200.4", {{"stroke", "#e63946"}, {"stroke-width", "3"}});
        add("upright line, square caps", "line", {{"x1", "150.25"}, {"y1", "190"}, {"x2", "150.25"}, {"y2", "260"}},
            "M150.25 190 L150.25 260", {{"stroke", "#1d3557"}, {"stroke-width", "1.5"}, {"stroke-linecap", "square"}});
        add("upright hairline, a fraction of a pixel", "line",
            {{"x1", "180.4"}, {"y1", "260"}, {"x2", "180.4"}, {"y2", "190"}}, "M180.4 260 L180.4 190",
            {{"stroke", "#000000"}, {"stroke-width", "0.3"}});
        add("pill", "rect", {{"x", "300"}, {"y", "140"}, {"width", "80"}, {"height", "20"}, {"rx", "10"}},
            "M310 140 H370 A10 10 0 0 1 380 150 A10 10 0 0 1 370 160 H310 A10 10 0 0 1 300 150 A10 10 0 0 1 310 140 Z",
            {{"fill", "#06d6a0"}});
        return out;
    }

    std::size_t count(const weft::paint::DisplayList& list, weft::paint::Item::Kind kind) {
        return std::size_t(std::count_if(list.items.begin(), list.items.end(), [&](const weft::paint::Item& item) {
            return item.kind == kind;
        }));
    }

    struct Difference {
        int worst{};
        double mean{};
        std::size_t beyond{};
    };

    // RGB differences between two encoded premultiplied BGRA frames.
    Difference difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        Difference d;
        double sum{};
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int worst{};
            for (unsigned c = 0; c < 3; ++c) {
                const int v = std::abs(int(a[i + c]) - int(b[i + c]));
                sum += v;
                worst = std::max(worst, v);
            }
            d.worst = std::max(d.worst, worst);
            d.beyond += worst > 24;
        }
        d.mean = sum / (3.0 * double(a.size() / 4));
        return d;
    }

    void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            rgba[i] = bgra[i + 2];
            rgba[i + 1] = bgra[i + 1];
            rgba[i + 2] = bgra[i];
            rgba[i + 3] = 255;
        }
        weft::testing::write_png(path, width, height, rgba);
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_render_scene_shape_boxes_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& text = weft::testing::thread_scene_text();
        const auto all = cases();
        weft::testing::D3D11Scene d3d11{width, height};
        // Each case alone: its shape a box, its path coverage, alike.
        std::string unlike;
        for (const auto& c : all) {
            const auto shapes = page({c.shape});
            const auto paths = page({c.path});
            const auto listed = weft::scene_display_list(shapes, {}, &text, scale);
            require(listed.fallback.empty() && count(listed.list, weft::paint::Item::Kind::Path) == 0 &&
                        count(listed.list, weft::paint::Item::Kind::Box) >= 2,
                    std::string(c.name) + ": drawn as boxes");
            const auto boxes = d3d11.draw(shapes, scale);
            const auto coverage = d3d11.draw(paths, scale);
            const auto d = difference(boxes, coverage);
            std::printf("%-40s worst %3d, mean %.4f, %zu px past 24 levels\n", c.name, d.worst, d.mean, d.beyond);
            if (!(d.worst <= (c.exact ? max_exact_difference : max_edge_difference) && d.mean <= max_mean_difference &&
                  d.beyond <= max_far_pixels)) {
                unlike += std::string(unlike.empty() ? "" : ", ") + c.name;
                // Evidence: the box drawn, the path drawn.
                const auto stem = folder / (std::string("unlike-") + std::to_string(&c - all.data()));
                save(stem.string() + "-box.png", boxes);
                save(stem.string() + "-path.png", coverage);
            }
        }
        require(unlike.empty(), "boxes unlike their paths: " + unlike);
        // All of them in one frame, for evidence.
        std::vector<weft::SceneNode> shape_elements;
        std::vector<weft::SceneNode> path_elements;
        for (const auto& c : all) {
            shape_elements.push_back(c.shape);
            path_elements.push_back(c.path);
        }
        const auto shapes = page(shape_elements);
        const auto boxes = d3d11.draw(shapes, scale);
        save(folder / "boxes.png", boxes);
        save(folder / "paths.png", d3d11.draw(page(path_elements), scale));
        // What a box cannot draw stays a path: an elliptical corner, a bevel join, a dashed stroke, a line of no
        // length, an ellipse.
        for (auto [what, n] : std::vector<std::pair<const char*, weft::SceneNode>>{
                 {"an elliptical corner",
                  element(90, "rect",
                          {{"x", "1"}, {"y", "1"}, {"width", "20"}, {"height", "20"}, {"rx", "4"}, {"ry", "6"}},
                          {{"fill", "#000000"}})},
                 {"a bevel join",
                  element(
                      91, "rect", {{"x", "1"}, {"y", "1"}, {"width", "20"}, {"height", "20"}},
                      {{"fill", "none"}, {"stroke", "#000000"}, {"stroke-width", "4"}, {"stroke-linejoin", "bevel"}})},
                 {"a dashed stroke", element(92, "circle", {{"cx", "10"}, {"cy", "10"}, {"r", "6"}},
                                             {{"fill", "none"}, {"stroke", "#000000"}, {"stroke-dasharray", "2 2"}})},
                 {"a line of no length",
                  element(93, "line", {{"x1", "5"}, {"y1", "5"}, {"x2", "5"}, {"y2", "5"}},
                          {{"stroke", "#000000"}, {"stroke-width", "4"}, {"stroke-linecap", "round"}})},
                 {"an ellipse", element(94, "ellipse", {{"cx", "10"}, {"cy", "10"}, {"rx", "6"}, {"ry", "4"}},
                                        {{"fill", "#000000"}})}}) {
            const auto listed = weft::scene_display_list(page({n}), {}, &text, scale);
            require(listed.fallback.empty() && count(listed.list, weft::paint::Item::Kind::Path) == 1,
                    std::string(what) + " stays a path");
        }
        std::string d3d12 = "skipped";
#ifdef WEFT_TEST_D3D12
        // The D3D12 pass runs the D3D11 pass's DXBC on the same rasterizer: the same bytes.
        if (const auto scene = weft::testing::D3D12Scene::open(width, height)) {
            require(scene->draw(shapes, scale) == boxes, "D3D12 WARP draws the boxes as D3D11 WARP");
            d3d12 = scene->debugged() ? "WARP with the debug layer" : "WARP";
        } else {
            std::cout << "D3D12 skipped: no WARP\n";
        }
#endif
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            if (const auto device = vk::Device::open({}, unavailable)) {
                vk::SceneRenderer scene(*device, text);
                const auto pixels = scene.draw(shapes, scale, width, height, std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                const auto d = difference(*pixels, boxes);
                std::cout << "Vulkan vs D3D11 WARP: worst " << d.worst << ", " << d.beyond << " px past 24 levels\n";
                require(d.worst <= 4, "Vulkan draws the boxes as D3D11");
                require(device->validation_messages == 0, "Vulkan validation");
                vulkan = device->name;
            } else {
                std::cout << "Vulkan skipped: " << unavailable << '\n';
            }
        }
#endif
        d3d11.gpu.check_debug_messages();
        std::cout << "PASS SVG shapes as boxes: " << all.size()
                  << " cases as their paths, what boxes cannot draw stays paths; D3D11 WARP, D3D12 " << d3d12
                  << ", Vulkan " << vulkan << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
