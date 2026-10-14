#include "scene/scene_anchor.hpp"
#include "scene/scene_native.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Anchored nameplates on the native scene passes (step 13.1 of docs/refactoring-plan.md), CPU reference path: plates
// inside a box that moves its content and clips it to rounded corners, placed by a table of anchors (fractional points, scales, depths that
// overlap them, one clamped into the View, one hidden by its anchor, one off the View, one whose key is missing), draw
// exactly as the same plates placed there by CSS in the root, far to near. The same renderer then draws a table that
// moved them, and one that lacks a key a plate keeps for data-anchor-missing: stay. Vulkan, under validation, draws
// both frames alike, and the anchored one as D3D11 does.
namespace {
    using weft::SceneAnchor;
    using weft::SceneAnchoring;
    using weft::testing::require;

    constexpr unsigned width = 800;
    constexpr unsigned height = 600;
    constexpr float scale = 2;
    constexpr float view_width = width / scale;
    constexpr float view_height = height / scale;
    constexpr float plate_width = 96;
    constexpr float plate_height = 24;
    // The clamped plate's margin.
    constexpr float margin = 4;
    // Vulkan and D3D11 WARP filter alike within the gallery's near level (vulkan_scene_near_level).
    constexpr int max_vulkan_difference = 2;

    struct Plate {
        std::uint64_t key{};
        float health{};
        const char* name{};
        SceneAnchoring::Offscreen offscreen = SceneAnchoring::Offscreen::Hide;
    };

    const std::array<Plate, 10> plates{{{1, .9f, "Goblin"},
                                        {2, .55f, "Orc"},
                                        {3, .3f, "Troll"},
                                        {4, 1, "Wolf"},
                                        {5, .75f, "Bandit"},
                                        {6, .6f, "Archer", SceneAnchoring::Offscreen::Clamp},
                                        {7, .5f, "Ghost"},
                                        {8, .45f, "Spider"},
                                        {9, .2f, "Bat"},
                                        {10, .8f, "Rat"}}};

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style, std::string text = {}) {
        weft::SceneNode n;
        n.identity = {slot, 1, 1};
        n.tag = "div";
        n.box = box;
        n.text = std::move(text);
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    // Plate i at box: a translucent rounded plate, the name over a bordered bar and its fill.
    weft::SceneNode plate(unsigned i, std::array<float, 2> at) {
        const auto slot = 100 + i * 4;
        auto p = node(slot, {at[0], at[1], plate_width, plate_height},
                      {{"background-color", "rgba(10, 8, 4, 0.45)"},
                       {"border-top-left-radius", "3px"},
                       {"border-top-right-radius", "3px"},
                       {"border-bottom-right-radius", "3px"},
                       {"border-bottom-left-radius", "3px"}});
        p.children.push_back(node(slot + 1, {0, 0, plate_width, 16},
                                  {{"font-size", "12px"}, {"line-height", "16px"}, {"color", "#f5e6c8"}},
                                  plates[i].name));
        auto bar = node(slot + 2, {0, 17, plate_width, 6},
                        {{"background-color", "rgba(0, 0, 0, 0.6)"},
                         {"border-top-width", "1px"},
                         {"border-right-width", "1px"},
                         {"border-bottom-width", "1px"},
                         {"border-left-width", "1px"},
                         {"border-top-color", "#3a2a12"},
                         {"border-right-color", "#3a2a12"},
                         {"border-bottom-color", "#3a2a12"},
                         {"border-left-color", "#3a2a12"}});
        bar.children.push_back(
            node(slot + 3, {1, 1, (plate_width - 2) * plates[i].health, 4}, {{"background-color", "#c0392b"}}));
        p.children.push_back(std::move(bar));
        return p;
    }

    weft::SceneFrame base() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, view_width, view_height}, {{"background-color", "#1c2430"}});
        auto world = node(2, {20, 20, 200, 150},
                          {{"background-color", "#2a3d2a"},
                           {"overflow", "hidden"},
                           {"border-top-left-radius", "12px"},
                           {"border-top-right-radius", "12px"},
                           {"border-bottom-right-radius", "12px"},
                           {"border-bottom-left-radius", "12px"},
                           {"transform", "translate(3.5px, 2.25px)"}});
        world.children.push_back(node(3, {10, 10, 50, 30}, {{"background-color", "#556b2f"}}));
        f.root.children.push_back(std::move(world));
        return f;
    }

    // The plates inside the world, where layout puts them, each following its key's anchor.
    weft::SceneFrame anchored(SceneAnchoring::Missing missing = {}) {
        auto f = base();
        auto& world = f.root.children[0];
        for (unsigned i = 0; i < plates.size(); ++i) {
            auto p = plate(i, {8 + 9.f * float(i), 6 + 12.f * float(i)});
            p.anchoring.emplace();
            p.anchoring->key = plates[i].key;
            p.anchoring->offscreen = plates[i].offscreen;
            p.anchoring->margin = plates[i].offscreen == SceneAnchoring::Offscreen::Clamp ? margin : 0;
            p.anchoring->missing = missing;
            world.children.push_back(std::move(p));
        }
        return f;
    }

    // Where each plate's anchor puts it, worked out apart from scene_anchor_map: the pivot (the plate's centre) on the
    // anchor and scaled about it, clamped into the View inset by the margin, hidden by its anchor or off the View; far
    // to near. last keeps the anchors of plates whose key the table lacks (data-anchor-missing: stay).
    struct Placed {
        unsigned plate{};
        float x{};
        float y{};
        float scale{};
    };

    std::vector<Placed> placements(const weft::SceneAnchorTable& table, std::vector<SceneAnchor>* last = nullptr) {
        std::vector<std::pair<float, Placed>> out;
        for (unsigned i = 0; i < plates.size(); ++i) {
            const auto* a = table.find(plates[i].key);
            if (last) {
                if (a) {
                    (*last)[i] = *a;
                } else if ((*last)[i].key == plates[i].key) {
                    a = &(*last)[i];
                }
            }
            if (!a || (a->flags & weft::scene_anchor_flags::hidden)) {
                continue;
            }
            const bool off = a->x < 0 || a->x > view_width || a->y < 0 || a->y > view_height ||
                             (a->flags & (weft::scene_anchor_flags::behind | weft::scene_anchor_flags::offscreen));
            const bool clamp = plates[i].offscreen == SceneAnchoring::Offscreen::Clamp;
            if (off && !clamp) {
                continue;
            }
            double x = double(a->x) - double(a->scale) * plate_width / 2;
            double y = double(a->y) - double(a->scale) * plate_height / 2;
            if (clamp) {
                x = std::clamp(x, double(margin), view_width - margin - double(a->scale) * plate_width);
                y = std::clamp(y, double(margin), view_height - margin - double(a->scale) * plate_height);
            }
            out.push_back({a->depth, {i, float(x), float(y), a->scale}});
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.first > b.first;
        });
        std::vector<Placed> placed;
        for (const auto& [depth, p] : out) {
            placed.push_back(p);
        }
        return placed;
    }

    // The same plates in the root, placed by CSS: at their box, twice as large about its corner for scale 2.
    weft::SceneFrame placed(const std::vector<Placed>& at) {
        auto f = base();
        for (const auto& p : at) {
            auto n = plate(p.plate, {p.x, p.y});
            n.style["transform"] = "scale(" + std::to_string(p.scale) + ")";
            n.style["transform-origin"] = "0 0";
            f.root.children.push_back(std::move(n));
        }
        return f;
    }

    weft::SceneAnchorTable table(weft::SceneAnchorChannel& channel, const std::vector<SceneAnchor>& anchors) {
        require(channel.write(anchors) == weft::SceneAnchorWrite::Written, "anchors written");
        return channel.take(weft::SceneAnchorChannel::Reader::Render);
    }

    struct Difference {
        int worst{};
        std::size_t pixels{};
    };

    Difference difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        Difference d;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int worst{};
            for (unsigned c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
            }
            d.worst = std::max(d.worst, worst);
            d.pixels += worst > 0;
        }
        return d;
    }

    void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            rgba[i] = bgra[i + 2];
            rgba[i + 1] = bgra[i + 1];
            rgba[i + 2] = bgra[i];
            rgba[i + 3] = bgra[i + 3];
        }
        weft::testing::write_png(path, width, height, rgba);
    }

    void same(const std::string& what, const std::vector<std::uint8_t>& drawn,
              const std::vector<std::uint8_t>& expected, const std::filesystem::path& folder, int tolerance = 0) {
        const auto d = difference(drawn, expected);
        std::printf("%-52s worst %d, %zu px differ\n", what.c_str(), d.worst, d.pixels);
        if (d.worst > tolerance) {
            save(folder / ("unlike-" + what + "-anchored.png"), drawn);
            save(folder / ("unlike-" + what + "-expected.png"), expected);
            require(false, what + ": anchored plates unlike the same plates placed there");
        }
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_render_scene_anchor_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& text = weft::testing::thread_scene_text();
        weft::SceneAnchorChannel channel;
        // Fractional points and scales that floats hold exactly; 2 and 3 overlap; 6 is clamped from the left edge, 7
        // hidden by its anchor, 9 partly past the View's corner, 10 off the View; 8 has no anchor.
        const std::vector<SceneAnchor> first{{1, 60.25f, 40.5f, 1, 5},
                                             {2, 150.375f, 60.75f, 1.25f, 3},
                                             {3, 170.5f, 70.25f, .75f, 8},
                                             {4, 250.125f, 120.625f, 1.5f, 1},
                                             {5, 330.8125f, 200.4375f, 1, 2},
                                             {6, -30, 150, 1, 4},
                                             {7, 100, 250, 1, 6, weft::scene_anchor_flags::hidden},
                                             {9, 380, 280, 1, 7},
                                             {10, -50, -50, 1, 0}};
        auto moved = first;
        for (auto& a : moved) {
            a.x += 7.3125f;
            a.y -= 3.5625f;
            a.depth = 10 - a.depth;
        }
        moved.push_back({8, 200, 100, .5f, 3});
        // The moved table without plate 2's key.
        auto missing = moved;
        std::erase_if(missing, [](const SceneAnchor& a) {
            return a.key == 2;
        });
        const auto first_table = table(channel, first);
        const auto moved_table = table(channel, moved);
        const auto missing_table = table(channel, missing);
        const auto first_frame = placed(placements(first_table));
        const auto moved_frame = placed(placements(moved_table));
        std::vector<SceneAnchor> last(plates.size());
        (void)placements(moved_table, &last);
        const auto stay_frame = placed(placements(missing_table, &last));
        require(placements(first_table).size() == 7 && placements(moved_table).size() == 8, "plates the anchors place");

        weft::testing::D3D11Scene d3d11{width, height};
        const auto drawn = d3d11.draw(anchored(), scale, &first_table);
        save(folder / "anchored.png", drawn);
        same("D3D11 anchored plates", drawn, d3d11.draw(first_frame, scale), folder);
        const auto moved_pixels = d3d11.draw(anchored(), scale, &moved_table);
        same("D3D11 moved anchors", moved_pixels, d3d11.draw(moved_frame, scale), folder);
        // Missing: stay keeps plate 2 where the moved table put it, from the renderer's last pass of the view.
        const auto stay = anchored(SceneAnchoring::Missing::Stay);
        (void)d3d11.draw(stay, scale, &moved_table);
        const auto stay_pixels = d3d11.draw(stay, scale, &missing_table);
        same("D3D11 missing: stay", stay_pixels, d3d11.draw(stay_frame, scale), folder);
        d3d11.gpu.check_debug_messages();

        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            if (auto device = vk::Device::open({}, unavailable)) {
                vk::SceneRenderer scene(*device, text);
                const auto anchored_pixels =
                    scene.draw(anchored(), scale, width, height, std::nullopt, 0, {}, true, &first_table);
                const auto placed_pixels = scene.draw(first_frame, scale, width, height, std::nullopt, 0, {});
                require(anchored_pixels && placed_pixels, "native Vulkan pass");
                same("Vulkan anchored plates", *anchored_pixels, *placed_pixels, folder);
                same("Vulkan anchored plates against D3D11", *anchored_pixels, drawn, folder, max_vulkan_difference);
                vulkan = device->name;
            } else {
                vulkan = "skipped: " + unavailable;
            }
        }
#endif
        std::cout << "PASS anchored plates on D3D11 WARP; Vulkan " << vulkan << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
