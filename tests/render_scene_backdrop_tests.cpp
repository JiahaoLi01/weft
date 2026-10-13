#include "render/scene_pass.hpp"
#include "scene/scene_display_list.hpp"
#include "support/backdrop_reference.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

// backdrop-filter in the native scene passes (D3D11 on WARP, and Vulkan on this machine's device when built): what lies
// behind an element, copied from the target, filtered and drawn in its shape, against the same algorithm on the CPU
// (tests/support/backdrop_reference.hpp) over the pass's own frame without the element. A small blur and large ones
// (downsampled), at scale 1 and 2; rounded corners; color matrices before and after the blur; mirrored edges; an element
// with opacity (its group, the backdrop beneath its content); a backdrop inside an opacity group, which reads only the
// group; an unreadable Vulkan target; the two passes against each other.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Pixels = std::vector<std::uint8_t>;
    constexpr unsigned width = 160;
    constexpr unsigned height = 120;
    // The passes and the CPU differ in rounding alone, away from the shape's antialiased edges.
    constexpr int max_difference = 3;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    // Sharp stripes of five colors, a white bar across them and a dark backdrop: what a blur visibly changes.
    weft::SceneFrame frame(std::vector<weft::SceneNode> over) {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {{"background-color", "#101820"}});
        constexpr const char* colors[]{"#e04040", "#40c060", "#4060e0", "#f0c030", "#c040c0"};
        for (unsigned i = 0; i < width / 10; ++i) {
            if (i % 2 == 0) {
                f.root.children.push_back(
                    node(10 + i, {i * 10.f, 0, 10, float(height)}, {{"background-color", colors[(i / 2) % 5]}}));
            }
        }
        f.root.children.push_back(node(40, {0, 54, float(width), 12}, {{"background-color", "#ffffff"}}));
        for (auto& n : over) {
            f.root.children.push_back(std::move(n));
        }
        return f;
    }

    using Draw = std::function<Pixels(const weft::SceneFrame&, float scale)>;

    std::array<int, 4> pixel(const Pixels& p, unsigned w, int x, int y) {
        const auto* q = p.data() + (std::size_t(y) * w + std::size_t(x)) * 4;
        return {q[2], q[1], q[0], q[3]};
    }

    // The frame with the element against the CPU's composite over the frame without it, at the pixels whose centres lie
    // inside the element's box at least margin device px from its edges (and its corners' arcs). The pixels checked.
    std::size_t compare(const char* what, const Draw& draw, const weft::SceneNode& element, float scale,
                        float margin = 1.5f, float opacity = 1, std::array<float, 4> skip = {}) {
        const auto w = unsigned(width * scale);
        const auto h = unsigned(height * scale);
        const auto with = frame({element});
        const auto drawn = draw(with, scale);
        const auto below = draw(frame({}), scale);
        const auto listed = weft::scene_display_list(with, {}, &weft::testing::thread_scene_text(), scale);
        require(listed.fallback.empty(), std::string(what) + ": a native list");
        const auto planned = weft::render::plan_scene_pass(listed.list, scale, w, h);
        require(planned && planned->backdrops.size() == 1, std::string(what) + ": one backdrop");
        const auto& b = planned->backdrops.front();
        const std::array<int, 4> region{b.region[0], b.region[1], b.region[2], b.region[3]};
        const auto& backdrop = listed.list.backdrops.front();
        const auto filtered = weft::testing::filtered(weft::testing::copied(below, int(w), region), b);
        const float radius = backdrop.radii[0] * scale;
        const float left = element.box[0] * scale;
        const float top = element.box[1] * scale;
        const float right = (element.box[0] + element.box[2]) * scale;
        const float bottom = (element.box[1] + element.box[3]) * scale;
        std::size_t checked{};
        int worst{};
        for (int y = 0; y < int(h); ++y) {
            for (int x = 0; x < int(w); ++x) {
                const float cx = x + .5f;
                const float cy = y + .5f;
                if (cx < left + margin || cx > right - margin || cy < top + margin || cy > bottom - margin) {
                    continue;
                }
                // Inside the corner arcs, margin in; outside them, margin out, what lies behind as it is.
                const float ax = std::clamp(cx, left + radius, right - radius);
                const float ay = std::clamp(cy, top + radius, bottom - radius);
                if (std::hypot(cx - ax, cy - ay) > radius - margin && (ax != cx || ay != cy)) {
                    if (std::hypot(cx - ax, cy - ay) > radius + margin) {
                        const auto a = pixel(drawn, w, x, y);
                        const auto u = pixel(below, w, x, y);
                        for (unsigned c = 0; c < 4; ++c) {
                            require(std::abs(a[c] - u[c]) <= 1, std::string(what) + ": nothing outside the corners");
                        }
                    }
                    continue;
                }
                // Not where the element's own content is (skip: left, top, width, height in its px), a pixel around.
                if (skip[2] > 0 && cx > left + (skip[0] - 1) * scale && cx < left + (skip[0] + skip[2] + 1) * scale &&
                    cy > top + (skip[1] - 1) * scale && cy < top + (skip[1] + skip[3] + 1) * scale) {
                    continue;
                }
                const auto under = pixel(below, w, x, y);
                const weft::testing::Rgba below_color{under[0] / 255.f, under[1] / 255.f, under[2] / 255.f,
                                                      under[3] / 255.f};
                const auto expected =
                    weft::testing::composite(filtered, region, backdrop.post, x, y, below_color, opacity);
                const auto actual = pixel(drawn, w, x, y);
                for (unsigned c = 0; c < 4; ++c) {
                    const int d = std::abs(actual[c] - int(std::lround(expected[c] * 255)));
                    worst = std::max(worst, d);
                    if (d > max_difference) {
                        std::cerr << what << ": pixel (" << x << ", " << y << ") is " << actual[0] << ' ' << actual[1]
                                  << ' ' << actual[2] << ' ' << actual[3] << ", expected "
                                  << std::lround(expected[0] * 255) << ' ' << std::lround(expected[1] * 255) << ' '
                                  << std::lround(expected[2] * 255) << ' ' << std::lround(expected[3] * 255) << '\n';
                        require(false, std::string(what) + " against the CPU");
                    }
                }
                ++checked;
            }
        }
        std::cout << what << ": " << checked << " pixels as the CPU, worst " << worst << " (at " << b.size[0] << 'x'
                  << b.size[1] << ", sigma " << b.sigma[0] << ", " << b.sigma[1] << ")\n";
        require(checked > 200, std::string(what) + ": pixels checked");
        return checked;
    }

    // The checks every pass passes; the frames it drew, for the comparison of passes.
    std::vector<Pixels> check(const char* pass, const Draw& draw) {
        std::vector<Pixels> drawn;
        const auto record = [&](const weft::SceneFrame& f, float scale) {
            drawn.push_back(draw(f, scale));
            return drawn.back();
        };
        const auto glass = [](const char* filter, std::array<float, 4> box = {30, 20, 90, 70},
                              const char* radius = "0") {
            return node(100, box,
                        {{"backdrop-filter", filter},
                         {"border-top-left-radius", radius},
                         {"border-top-right-radius", radius},
                         {"border-bottom-right-radius", radius},
                         {"border-bottom-left-radius", radius}});
        };
        const std::string name = pass;
        compare((name + " blur(3px)").c_str(), record, glass("blur(3px)"), 1);
        compare((name + " blur(20px), downsampled").c_str(), record, glass("blur(20px)"), 1);
        compare((name + " blur(8px) at scale 2").c_str(), record, glass("blur(8px)"), 2);
        compare((name + " rounded").c_str(), record, glass("blur(6px)", {30, 20, 90, 70}, "16px"), 1, 2.5f);
        compare((name + " saturate and brightness after the blur").c_str(), record,
                glass("blur(5px) saturate(180%) brightness(1.1)"), 1);
        compare((name + " grayscale before the blur").c_str(), record, glass("grayscale(0.8) blur(5px)"), 1);
        // At the target's corner: the region stops at the target's edges, and the blur mirrors there.
        compare((name + " at the target's edge").c_str(), record, glass("blur(6px)", {-20, -20, 70, 60}), 1);
        // With opacity and content: the element's group holds its content, the backdrop beneath it, and composites at
        // the opacity; where there is no content, the backdrop at the opacity over what lies behind.
        {
            auto translucent = node(100, {30, 20, 90, 70}, {{"backdrop-filter", "blur(6px)"}, {"opacity", "0.6"}});
            translucent.children.push_back(node(101, {10, 10, 20, 20}, {{"background-color", "#00ffff"}}));
            compare((name + " with opacity and content").c_str(), record, translucent, 1, 1.5f, .6f, {10, 10, 20, 20});
            // Over its content, the content alone at the opacity: the backdrop lies beneath it.
            const auto f = frame({translucent});
            const auto drawn_frame = draw(f, 1);
            const auto below = draw(frame({}), 1);
            const auto inside = pixel(drawn_frame, width, 30 + 20, 20 + 20);
            const auto under = pixel(below, width, 30 + 20, 20 + 20);
            for (unsigned c = 0; c < 3; ++c) {
                const int cyan = c == 0 ? 0 : 255;
                require(std::abs(inside[c] - int(std::lround(.6 * cyan + .4 * under[c]))) <= 1,
                        name + ": the content over its backdrop, at the opacity");
            }
        }
        // Inside an opacity group, a backdrop reads only what the group drew: the group's own box, and transparency
        // where it drew nothing (not the stripes behind the group). A translucent box there goes through a matrix
        // before the blur on its straight colors.
        for (const bool translucent : {false, true}) {
            auto group = node(110, {20, 10, 120, 100}, {{"opacity", "0.8"}});
            group.children.push_back(
                node(111, {0, 0, 60, 100}, {{"background-color", translucent ? "rgba(32,80,255,0.5)" : "#2050ff"}}));
            group.children.push_back(node(
                112, {20, 20, 80, 60}, {{"backdrop-filter", translucent ? "brightness(1.6) blur(5px)" : "blur(5px)"}}));
            const auto f = frame({group});
            const auto drawn_frame = record(f, 1);
            const auto below = record(frame({}), 1);
            const auto listed = weft::scene_display_list(f, {}, &weft::testing::thread_scene_text(), 1);
            const auto planned = weft::render::plan_scene_pass(listed.list, 1, width, height);
            require(planned && planned->backdrops.size() == 1 && !planned->backdrops.front().under,
                    name + ": a backdrop inside a group");
            const auto& b = planned->backdrops.front();
            const std::array<int, 4> region{b.region[0], b.region[1], b.region[2], b.region[3]};
            // The group's target as the backdrop finds it: the blue box over [20, 80) x [10, 110) (premultiplied, the
            // translucent one quantized as the pass quantizes it), clear elsewhere.
            Pixels target(std::size_t(width) * height * 4);
            for (int y = 10; y < 110; ++y) {
                for (int x = 20; x < 80; ++x) {
                    auto* p = target.data() + (std::size_t(y) * width + std::size_t(x)) * 4;
                    const double a = translucent ? 128 / 255.0 : 1;
                    p[0] = std::uint8_t(std::lround(255 * a));
                    p[1] = std::uint8_t(std::lround(0x50 * a));
                    p[2] = std::uint8_t(std::lround(0x20 * a));
                    p[3] = std::uint8_t(std::lround(255 * a));
                }
            }
            const auto filtered = weft::testing::filtered(weft::testing::copied(target, int(width), region), b);
            int worst{};
            std::size_t checked{};
            for (int y = 32; y < 88; ++y) {
                for (int x = 42; x < 118; ++x) {
                    // Away from the blue box's edge, which the reference fills whole pixels of.
                    if (x >= 79 && x <= 80) {
                        continue;
                    }
                    const auto g = pixel(target, width, x, y);
                    const weft::testing::Rgba group_color{g[0] / 255.f, g[1] / 255.f, g[2] / 255.f, g[3] / 255.f};
                    const auto in_group =
                        weft::testing::composite(filtered, region, paint::identity_color_matrix, x, y, group_color);
                    const auto under = pixel(below, width, x, y);
                    const auto actual = pixel(drawn_frame, width, x, y);
                    for (unsigned c = 0; c < 4; ++c) {
                        const double expected = in_group[c] * .8 * 255 + under[c] * (1 - in_group[3] * .8);
                        const int d = std::abs(actual[c] - int(std::lround(expected)));
                        worst = std::max(worst, d);
                        if (d > max_difference) {
                            std::cerr << name << " group: pixel (" << x << ", " << y << ") channel " << c << " is "
                                      << actual[c] << ", expected " << expected << '\n';
                            require(false, name + ": a backdrop reads only its group");
                        }
                    }
                    ++checked;
                }
            }
            std::cout << name
                      << (translucent ? " a translucent group's backdrop through a matrix: "
                                      : " a backdrop inside an opacity group: ")
                      << checked << " pixels, worst " << worst << '\n';
        }
        return drawn;
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        weft::testing::D3D11Scene one(width, height);
        weft::testing::D3D11Scene two(width * 2, height * 2);
        const auto warp = check("D3D11 WARP", [&](const weft::SceneFrame& f, float scale) {
            return scale == 1 ? one.draw(f) : two.draw(f, scale);
        });
        require(one.scene.backdrop_passes > 0, "D3D11 backdrop passes counted");
        one.gpu.check_debug_messages();
        two.gpu.check_debug_messages();
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        if (const auto device = vk::Device::open({}, unavailable)) {
            vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
            const auto drawn = check("Vulkan", [&](const weft::SceneFrame& f, float scale) {
                auto pixels =
                    scene.draw(f, scale, unsigned(width * scale), unsigned(height * scale), std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                return std::move(*pixels);
            });
            int worst{};
            for (std::size_t i = 0; i < drawn.size(); ++i) {
                for (std::size_t k = 0; k < drawn[i].size(); ++k) {
                    worst = std::max(worst, std::abs(int(drawn[i][k]) - int(warp[i][k])));
                }
            }
            std::cout << "Vulkan vs D3D11 WARP: worst " << worst << '\n';
            require(worst <= 2, "Vulkan against D3D11 WARP");
            // A target the pass may not read: the backdrop is transparent, a counted reason, and what lies behind
            // shows unblurred.
            const auto f = frame({node(100, {30, 20, 90, 70}, {{"backdrop-filter", "blur(6px)"}})});
            const auto before = scene.reasons["backdrop of an unreadable target"];
            const auto unread = scene.draw(f, 1, width, height, std::nullopt, 0, {}, false);
            const auto plain = scene.draw(frame({}), 1, width, height, std::nullopt, 0, {});
            require(unread && plain && *unread == *plain &&
                        scene.reasons["backdrop of an unreadable target"] == before + 1 && !scene.fallbacks,
                    "an unreadable Vulkan target: a transparent backdrop and its reason");
            require(device->validation_messages == 0, "Vulkan validation");
            vulkan = device->name;
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        std::cout << "PASS backdrop filters in native scene passes: D3D11 WARP, Vulkan " << vulkan << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
