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
//
// The GPU path (13.1b): the same frames with a revision keep their pass, which the frame's anchors move as the shader
// draws it, as the CPU path draws them but for the rounding of where pixels sample: its first frame, and a kept pass
// moved and reordered, a stay, a translucent plate (a group, which a new order prepares again), and a plate with a
// backdrop filter, which the GPU cannot move (placed every frame, as the CPU path does). A kept pass draws only the
// scale and layers it was prepared for. A path reaching left of its anchored element's origin keeps its whole mask
// (anchored records have no target to cut it), at whole device pixels, where moving a mask equals drawing it there.
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
    // The GPU path maps pixels back through its anchors in the shader, where the CPU path's records hold the anchors'
    // maps: coordinates round otherwise in their last bits, which moves filtering weights at edges by a quantization
    // step.
    constexpr int max_gpu_difference = 2;
    // Revisions of the frames the GPU path keeps passes for.
    constexpr std::uint64_t revisions = std::uint64_t(1) << 40;

    weft::SceneFrame revised(weft::SceneFrame f, std::uint64_t revision) {
        f.revision = revision;
        return f;
    }

    // An anchored tag (its top-left on the anchor) whose triangle icon lies left of its origin.
    weft::SceneFrame tagged();
    // An anchored box with a border and a rounded child, to draw far smaller than its size.
    weft::SceneFrame speck();

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

    weft::SceneFrame speck() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, view_width, view_height}, {{"background-color", "#1c2430"}});
        auto box = node(2, {20, 20, 160, 90},
                        {{"background-color", "#d8e0ea"},
                         {"border-top-width", "6px"},
                         {"border-right-width", "6px"},
                         {"border-bottom-width", "6px"},
                         {"border-left-width", "6px"},
                         {"border-top-color", "#c0392b"},
                         {"border-right-color", "#c0392b"},
                         {"border-bottom-color", "#c0392b"},
                         {"border-left-color", "#c0392b"}});
        box.anchoring.emplace();
        box.anchoring->key = 1;
        box.children.push_back(node(3, {30, 20, 100, 50},
                                    {{"background-color", "#16a085"},
                                     {"border-top-left-radius", "20px"},
                                     {"border-top-right-radius", "20px"},
                                     {"border-bottom-right-radius", "20px"},
                                     {"border-bottom-left-radius", "20px"}}));
        f.root.children = {box};
        return f;
    }

    weft::SceneFrame tagged() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, view_width, view_height}, {{"background-color", "#1c2430"}});
        auto tag = node(2, {20, 20, 40, 20}, {{"background-color", "#3a6ea5"}});
        tag.anchoring.emplace();
        tag.anchoring->key = 1;
        tag.anchoring->pivot_fraction = {0, 0};
        auto svg = node(3, {-14, 4, 12, 12}, {});
        svg.tag = "svg";
        svg.space = weft::constants::svg_namespace_uri;
        svg.attributes = {{"width", "12"}, {"height", "12"}};
        auto triangle = node(4, {0, 0, 12, 12}, {{"fill", "#e8c060"}});
        triangle.tag = "path";
        triangle.space = weft::constants::svg_namespace_uri;
        triangle.attributes = {{"d", "M6 0 L12 12 L0 12 Z"}};
        svg.children = {triangle};
        tag.children = {svg};
        f.root.children = {tag};
        return f;
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

        // The GPU path on a renderer of its own: its first frame prepares the pass, later ones of the revision draw it.
        weft::testing::D3D11Scene gpu{width, height};
        const auto kept = revised(anchored(), revisions);
        const auto gpu_first = gpu.draw(kept, scale, &first_table);
        save(folder / "gpu-anchored.png", gpu_first);
        same("D3D11 GPU path", gpu_first, drawn, folder, max_gpu_difference);
        same("D3D11 GPU path, moved and reordered", gpu.draw(kept, scale, &moved_table), moved_pixels, folder,
             max_gpu_difference);
        require(gpu.scene.passes == 2 && gpu.scene.kept_passes == 1 && !gpu.scene.kept_rebuilds,
                "the moved anchors drew the kept pass");
        const auto kept_stay = revised(stay, revisions + 1);
        (void)gpu.draw(kept_stay, scale, &moved_table);
        same("D3D11 GPU path, missing: stay", gpu.draw(kept_stay, scale, &missing_table), stay_pixels, folder,
             max_gpu_difference);
        require(gpu.scene.kept_passes == 2, "stay drew the kept pass");
        // A translucent plate is a group, whose items no batch reorders: a new order prepares the pass again.
        auto translucent = anchored();
        translucent.root.children[0].children[1 + 3].style["opacity"] = "0.8";
        const auto kept_group = revised(translucent, revisions + 2);
        const auto group_first = d3d11.draw(translucent, scale, &first_table);
        same("D3D11 GPU path, translucent plate", gpu.draw(kept_group, scale, &first_table), group_first, folder,
             max_gpu_difference);
        const auto group_moved = d3d11.draw(translucent, scale, &moved_table);
        same("D3D11 GPU path, translucent plate reordered", gpu.draw(kept_group, scale, &moved_table), group_moved,
             folder, max_gpu_difference);
        require(gpu.scene.kept_rebuilds == 1 && gpu.scene.kept_passes == 2, "a group reordered on a kept pass");
        // A backdrop filter reads its region of the target: the frame is placed each time, as on the CPU path.
        auto filtered = anchored();
        filtered.root.children[0].children[1].style["backdrop-filter"] = "blur(2px)";
        const auto kept_filtered = revised(filtered, revisions + 3);
        for (const auto* t : {&first_table, &moved_table}) {
            const auto expected = d3d11.draw(filtered, scale, t);
            same("D3D11 GPU path, backdrop filter", gpu.draw(kept_filtered, scale, t), expected, folder);
        }
        require(gpu.scene.kept_passes == 2, "a backdrop filter moved on the GPU");
        // Another scale or other layers: the frame is prepared again, then kept for them.
        const auto passes = gpu.scene.passes;
        const auto layer = std::vector<weft::NodeIdentity>{kept.root.children[0].children[1].identity};
        (void)gpu.draw(kept, scale, &first_table);
        (void)gpu.draw(kept, scale, &first_table);
        (void)gpu.draw(kept, 1, &first_table);
        (void)gpu.draw(kept, 1, &first_table, layer);
        (void)gpu.draw(kept, 1, &first_table, layer);
        require(gpu.scene.passes == passes + 5 && gpu.scene.kept_passes == 4,
                "a pass kept for another scale or layers");
        // A path left of its element's origin, its anchor on whole device pixels.
        const auto tags = table(channel, {{1, 100, 60}});
        const auto moved_tags = table(channel, {{1, 140.5f, 90}});
        weft::testing::D3D11Scene reference{width, height};
        for (const auto* t : {&tags, &moved_tags}) {
            const auto expected = reference.draw(tagged(), scale, t);
            same("D3D11 GPU path, a path left of the origin", gpu.draw(revised(tagged(), revisions + 4), scale, t),
                 expected, folder, max_gpu_difference);
        }
        // Drawn at a tenth of its size, an element keeps its edges' antialiasing past its bounds.
        const auto tenth = table(channel, {{1, 100.3f, 60.7f, .1f}});
        const auto tenth_moved = table(channel, {{1, 140.55f, 90.15f, .15f}});
        for (const auto* t : {&tenth, &tenth_moved}) {
            const auto expected = reference.draw(speck(), scale, t);
            same("D3D11 GPU path, a tenth of the size", gpu.draw(revised(speck(), revisions + 5), scale, t), expected,
                 folder, max_gpu_difference);
        }
        gpu.gpu.check_debug_messages();

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
                // The GPU path, kept and moved, against the CPU path on Vulkan.
                const auto moved_cpu =
                    scene.draw(anchored(), scale, width, height, std::nullopt, 0, {}, true, &moved_table);
                vk::SceneRenderer kept_scene(*device, text);
                const auto kept_first =
                    kept_scene.draw(kept, scale, width, height, std::nullopt, 0, {}, true, &first_table);
                const auto kept_moved =
                    kept_scene.draw(kept, scale, width, height, std::nullopt, 0, {}, true, &moved_table);
                require(moved_cpu && kept_first && kept_moved && kept_scene.kept_passes == 1,
                        "the Vulkan GPU path drew the kept pass");
                same("Vulkan GPU path", *kept_first, *anchored_pixels, folder, max_gpu_difference);
                same("Vulkan GPU path, moved and reordered", *kept_moved, *moved_cpu, folder, max_gpu_difference);
                // A frame of another revision prepares its own pass.
                const auto group_cpu =
                    scene.draw(translucent, scale, width, height, std::nullopt, 0, {}, true, &first_table);
                const auto group_kept =
                    kept_scene.draw(kept_group, scale, width, height, std::nullopt, 0, {}, true, &first_table);
                require(group_cpu && group_kept, "native Vulkan pass");
                same("Vulkan GPU path, another revision", *group_kept, *group_cpu, folder, max_gpu_difference);
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
