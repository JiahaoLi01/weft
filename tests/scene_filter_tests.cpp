#include "base/diagnostic.hpp"
#include "paint/display_list.hpp"
#include "render/filter_chain.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_filter.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// CSS filter on scene elements (Filter Effects 1): the functions as the scene takes them (blur(), drop-shadow(), the
// color functions and url() of an SVG filter of feColorMatrix primitives), the display list's filtered items and the
// images they cover, the passes of their chains on the CPU against an exact Gaussian, the native D3D11 pass (WARP)
// drawing them as the reference raster draws them, pixel for pixel at scale 1 and 2 (and Vulkan against D3D11 when
// built), and the cache: an element's opacity and transform change without filtering it again; its content changes and
// it does.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Kind = paint::Item::Kind;
    using Op = paint::FilterOp;

    bool about(float a, float b, float epsilon = 1e-4f) {
        return std::abs(a - b) <= epsilon * std::max(1.f, std::abs(b));
    }

    weft::StyleValues style(std::initializer_list<std::pair<const char*, const char*>> values) {
        weft::StyleValues s;
        for (const auto& [k, v] : values) {
            s[k] = v;
        }
        return s;
    }

    weft::Result<std::vector<Op>> parse(const char* value, const weft::SceneFilterResolve& resolve = {},
                                        const weft::StyleValues& s = {}) {
        return weft::scene_filter(value, s, resolve);
    }

    std::vector<Op> parsed(const char* value, const weft::SceneFilterResolve& resolve = {},
                           const weft::StyleValues& s = {}) {
        auto out = parse(value, resolve, s);
        require(bool(out), std::string("filter ") + value + " parses: " + (out ? "" : out.error().message));
        return std::move(*out);
    }

    bool refused(const char* value, const char* why, const weft::SceneFilterResolve& resolve = {}) {
        const auto out = parse(value, resolve);
        return !out && out.error().code == weft::Diag::SceneProfile &&
               out.error().message.find(why) != std::string::npos;
    }

    // An SVG node of a frame.
    weft::SceneNode svg_node(const char* tag, std::initializer_list<std::pair<const char*, const char*>> attributes,
                             std::vector<weft::SceneNode> children = {}) {
        weft::SceneNode n;
        n.tag = tag;
        n.space = weft::constants::svg_namespace_uri;
        for (const auto& [k, v] : attributes) {
            n.attributes[k] = v;
        }
        n.children = std::move(children);
        return n;
    }

    // An HTML node at box with a style, and its children.
    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> s,
                         std::vector<weft::SceneNode> children = {}) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : s) {
            n.style[k] = v;
        }
        n.children = std::move(children);
        return n;
    }

    // The defs of the colour-blind preview (CRPG's ColorBlindPreview): a deuteranope matrix in sRGB, and one in
    // linearRGB (the initial value), a saturate and a luminanceToAlpha primitive.
    weft::SceneNode filter_defs() {
        return svg_node(
            "svg", {{"width", "0"}, {"height", "0"}},
            {svg_node(
                "defs", {},
                {svg_node("filter", {{"id", "deut"}, {"color-interpolation-filters", "sRGB"}},
                          {svg_node("feColorMatrix", {{"type", "matrix"},
                                                      {"values", "0.625 0.375 0 0 0 0.7 0.3 0 0 0 "
                                                                 "0 0.3 0.7 0 0 0 0 0 1 0"}})}),
                 svg_node("filter", {{"id", "lin"}},
                          {svg_node("feColorMatrix",
                                    {{"type", "matrix"}, {"values", "0.5 0 0 0 0, 0 1 0 0 0, 0 0 1 0 0, 0 0 0 1 0"}})}),
                 svg_node("filter", {{"id", "pair"}},
                          {svg_node("feColorMatrix", {{"type", "saturate"}, {"values", "0.25"}, {"result", "a"}}),
                           svg_node("feColorMatrix", {{"in", "a"}, {"type", "hueRotate"}, {"values", "90"}})}),
                 svg_node("filter", {{"id", "luma"}}, {svg_node("feColorMatrix", {{"type", "luminanceToAlpha"}})}),
                 svg_node("filter", {{"id", "empty"}}, {}),
                 svg_node("filter", {{"id", "gauss"}}, {svg_node("feGaussianBlur", {{"stdDeviation", "2"}})}),
                 svg_node("filter", {{"id", "alpha"}},
                          {svg_node("feColorMatrix", {{"in", "SourceAlpha"}, {"type", "saturate"}})}),
                 svg_node("filter", {{"id", "short"}},
                          {svg_node("feColorMatrix", {{"type", "matrix"}, {"values", "1 0 0"}})}),
                 svg_node("rect", {{"id", "not-a-filter"}})})});
    }

    // filter values: the functions in order, what changes nothing left out, url() filters, and refusals.
    void parses() {
        require(parsed("none").empty() && parsed("").empty() && parsed("   ").empty(), "none and empty values");
        const auto blur = parsed("blur(4px)");
        require(blur.size() == 1 && blur[0].kind == Op::Kind::Blur && blur[0].blur == 4, "blur(4px)");
        require(parsed("blur(0)").empty() && parsed("blur()").empty(), "blur(0) changes nothing");
        require(refused("blur(2em)", "blur() takes a length") && refused("blur(-1px)", "blur() takes a length") &&
                    refused("blur(300px)", "blur() takes a length"),
                "blur lengths the scene does not take");
        // Color functions: one matrix each, in order.
        const auto two = parsed("grayscale(1) sepia(50%)");
        require(two.size() == 2 && two[0].kind == Op::Kind::Matrix && two[1].kind == Op::Kind::Matrix &&
                    about(two[0].matrix[0], .2126f) && about(two[1].matrix[0], .393f + .607f * .5f),
                "grayscale then sepia, two matrices");
        require(parsed("grayscale(100%)")[0].matrix == parsed("grayscale(1)")[0].matrix &&
                    parsed("grayscale(2)")[0].matrix == parsed("grayscale(1)")[0].matrix,
                "a percentage, and grayscale capped at 1");
        require(parsed("brightness(1) contrast(100%) opacity(1) hue-rotate(0deg)").empty(),
                "identity color functions change nothing");
        const auto quarter = parsed("hue-rotate(.25turn)")[0].matrix;
        const auto right = parsed("hue-rotate(90deg)")[0].matrix;
        for (std::size_t i = 0; i < quarter.size(); ++i) {
            require(about(quarter[i], right[i], 1e-4f), "hue-rotate in turns and degrees");
        }
        require(parsed("opacity(.25)")[0].matrix[18] == .25f, "opacity scales alpha");
        require(refused("brightness(-1)", "brightness() takes a number") && refused("hue-rotate(4)", "angle") &&
                    refused("foo(1)", "foo() is not a filter function") && refused("blur(2px", "filter functions only"),
                "values the scene does not take");
        // drop-shadow(): a color before or after two or three lengths, the third the standard deviation;
        // currentcolor without a color; transparent changes nothing.
        const auto shadow = parsed("drop-shadow(0 1px 1px rgba(0, 0, 0, 0.8))");
        require(shadow.size() == 1 && shadow[0].kind == Op::Kind::DropShadow && shadow[0].x == 0 && shadow[0].y == 1 &&
                    shadow[0].blur == 1 && about(shadow[0].color[3], .8f) && shadow[0].color[0] == 0,
                "drop-shadow(0 1px 1px rgba(0, 0, 0, 0.8))");
        const auto current = parsed("drop-shadow(2px 3px)", {}, style({{"color", "#0000ff"}}));
        require(current[0].x == 2 && current[0].y == 3 && current[0].blur == 0 &&
                    current[0].color == paint::Color{0, 0, 1, 1},
                "currentcolor and no blur");
        const auto first = parsed("drop-shadow(#ff0000 1px 2px 3px)");
        require(first[0].color == paint::Color{1, 0, 0, 1} && first[0].blur == 3, "the color first");
        require(parsed("drop-shadow(1px 1px transparent)").empty(), "a transparent drop shadow");
        require(refused("drop-shadow(1px)", "two or three lengths") &&
                    refused("drop-shadow(1px 1px 1px 1px)", "two or three lengths") &&
                    refused("drop-shadow(1px 1px -1px)", "a blur of 0") &&
                    refused("drop-shadow(10% 1px)", "two or three lengths"),
                "drop shadows the scene does not take");
        // Tailwind's drop-shadow utility (two of them, their colors as a forgiving cascade computes them), functions
        // without space between, and the bound on their number.
        require(
            parsed("drop-shadow(0 1px 2px rgba(0, 0, 0, 0.102)) drop-shadow(0 1px 1px rgba(0, 0, 0, 0.0588))").size() ==
                2,
            "two drop shadows");
        require(parsed("blur(2px)brightness(1.5)").size() == 2, "functions without space between them");
        std::string many;
        for (int i = 0; i < 17; ++i) {
            many += "blur(1px) ";
        }
        require(refused(many.c_str(), "at most 16 functions"), "at most 16 functions");
        // url(#id) of an SVG filter element: feColorMatrix primitives, each a matrix in its interpolation space.
        weft::SceneFrame frame;
        frame.root = node(1, {0, 0, 10, 10}, {}, {filter_defs()});
        const auto resolve = weft::scene_filter_resolver(frame.root);
        const auto deut = parsed("url(#deut)", resolve);
        require(deut.size() == 1 && deut[0].kind == Op::Kind::Matrix && !deut[0].linear && deut[0].matrix[0] == .625f &&
                    deut[0].matrix[1] == .375f && deut[0].matrix[18] == 1,
                "a matrix in sRGB");
        const auto lin = parsed("url('#lin')", resolve);
        require(lin.size() == 1 && lin[0].linear && lin[0].matrix[0] == .5f, "linearRGB, the initial value; commas");
        const auto pair = parsed("url(#pair) blur(1px)", resolve);
        require(pair.size() == 3 && pair[0].matrix == parsed("saturate(.25)")[0].matrix &&
                    pair[2].kind == Op::Kind::Blur,
                "saturate, then hueRotate reading its result, then a blur");
        const auto luma = parsed("url(#luma)", resolve);
        require(luma[0].matrix[15] == .2125f && luma[0].matrix[0] == 0, "luminanceToAlpha");
        const auto empty = parsed("url(#empty)", resolve);
        require(empty.size() == 1 && empty[0].matrix == paint::ColorMatrix{}, "a filter of no primitives hides");
        require(parsed("url(#missing) blur(2px)", resolve).empty() && parsed("url(#not-a-filter)", resolve).empty(),
                "a url() of no filter element: the filter is ignored");
        require(refused("url(#gauss)", "feGaussianBlur is not a filter primitive", resolve) &&
                    refused("url(#alpha)", "SourceAlpha", resolve) && refused("url(#short)", "20 values", resolve) &&
                    refused("url(page.svg#f)", "url() of other than", resolve),
                "filters the scene does not take");
    }

    // The display list's filtered items: the content in its own list, the image's pixels, the item's map and opacity;
    // an svg icon's one drop shadow stays a path shadow; what the lists refuse.
    void lists() {
        weft::SceneFrame frame;
        frame.root = node(1, {0, 0, 200, 100}, {{"background-color", "#202020"}},
                          {node(2, {30, 20, 40, 20},
                                {{"background-color", "#ff8000"}, {"filter", "blur(4px)"}, {"opacity", "0.5"}})});
        const auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(listed.fallback.empty() && bool(paint::check(listed.list, 4)),
                "a well-formed list: " + listed.fallback);
        const auto& items = listed.list.items;
        require(items.size() == 2 && items[1].kind == Kind::Filter, "the root's box, then the filter");
        const auto& f = listed.list.filters[items[1].payload];
        // Its ink [0, 40] x [0, 20], 3.5 deviations of 4px and a guard of 2 device px around.
        require(f.pixels == std::array<std::uint32_t, 2>{72, 52} && f.resolution == 1 && items[1].width == 72 &&
                    items[1].height == 52 && items[1].transform[4] == 30 - 16 && items[1].transform[5] == 20 - 16,
                "the image: the box, the blur's reach and a guard, in device px of the element's space");
        require(items[1].opacity == .5f && f.ops.size() == 1 && f.ops[0].blur == 4 && f.source &&
                    f.source->style.find(weft::PropertyId::Opacity) == nullptr,
                "the element's opacity on the item; its source without it");
        require(f.content->items.size() == 1 && f.content->items[0].kind == Kind::Box &&
                    f.content->items[0].transform[4] == 16 && f.content->items[0].transform[5] == 16,
                "the content: the element's box, 16px into the image");
        // At scale 2 the image's pixels double, its box in item px stays.
        const auto twice = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 2);
        const auto& f2 = twice.list.filters.at(0);
        require(f2.pixels == std::array<std::uint32_t, 2>{140, 100} && f2.resolution == 2 &&
                    twice.list.items[1].width == 70,
                "at scale 2: twice the pixels");
        // A drop shadow's offset on every side, the deviation, and a guard.
        frame.root.children.edit(0).style["filter"] = "drop-shadow(3px -2px 1px #000000)";
        const auto shadowed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        const auto& fs = shadowed.list.filters.at(0);
        require(fs.pixels == std::array<std::uint32_t, 2>{40 + 2 * 9, 20 + 2 * 8} &&
                    shadowed.list.items[1].transform[4] == 30 - 9,
                "a drop shadow's image: its offset and 3.5 deviations on every side");

        // CRPG's icons: an svg element whose filter is one drop shadow of its shapes is a path shadow.
        weft::SceneNode icon =
            svg_node("svg", {{"viewBox", "0 0 10 10"}}, {svg_node("path", {{"d", "M1 1 L9 1 L5 9 Z"}})});
        icon.identity = {1, 1, 3};
        icon.box = {10, 10, 20, 20};
        icon.style["filter"] = "drop-shadow(0 1px 1px rgba(0,0,0,0.9))";
        icon.children.edit(0).style["fill"] = "#ff0000";
        frame.root.children = {icon};
        const auto icons = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(icons.fallback.empty() && icons.list.filters.empty() && icons.list.path_shadows.size() == 1,
                "an svg icon's drop shadow: a path shadow");
        // With a background, it is a filtered image (it was a fallback); a filter on svg content falls back.
        frame.root.children.edit(0).style["background-color"] = "#00ff00";
        const auto boxed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(boxed.fallback.empty() && boxed.list.filters.size() == 1 && boxed.list.path_shadows.empty(),
                "an svg element with a box: a filtered image");
        frame.root.children.edit(0).children.edit(0).style["filter"] = "blur(1px)";
        require(weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1).fallback ==
                    "filter on svg content",
                "a filter on svg content");

        // url() filters resolve in the frame (the colour-blind preview's defs beside the swatches).
        frame.root.children = {filter_defs(),
                               node(4, {10, 10, 40, 10}, {{"background-color", "#e74c3c"}, {"filter", "url(#deut)"}})};
        const auto preview = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(preview.fallback.empty() && preview.list.filters.size() == 1 &&
                    preview.list.filters[0].ops.at(0).matrix[0] == .625f,
                "a url() filter of the frame");
        frame.root.children.edit(1).style["filter"] = "url(#missing)";
        const auto ignored = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(ignored.fallback.empty() && ignored.list.filters.empty() && ignored.list.items.size() == 2,
                "a url() of no filter: the element drawn without one");

        // An anchored descendant moves on its own: no image holds it.
        auto anchored = node(5, {0, 0, 10, 10}, {{"background-color", "#ffffff"}});
        anchored.anchoring = weft::SceneAnchoring{};
        frame.root.children = {node(6, {10, 10, 40, 40}, {{"filter", "blur(1px)"}}, {anchored})};
        require(weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1).fallback ==
                    "anchored element in a filtered element",
                "an anchored element inside a filter");

        // The checks: a filter of no content, of a negative blur, nested too deep.
        auto list = listed.list;
        list.filters[0].content.reset();
        require(!paint::check(list, 4), "a filter without content");
        list = listed.list;
        list.filters[0].ops[0].blur = -1;
        require(!paint::check(list, 4), "a negative blur");
        list = listed.list;
        list.filters[0].ops.clear();
        require(!paint::check(list, 4), "a filter of no functions");
        auto nested = listed.list;
        for (unsigned depth = 0; depth < weft::constants::max_filter_depth; ++depth) {
            auto outer = listed.list;
            outer.filters[0].content = std::make_shared<paint::DisplayList>(nested);
            nested = std::move(outer);
        }
        require(!paint::check(nested, 4), "filters nested past the bound");
    }

    // The chain's passes on the CPU: a blur of at most four device px is an exact Gaussian of the backdrop's kernel; a
    // wider one is downsampled first and upsampled bilinearly; a drop shadow lies under its content, offset.
    void chains() {
        // A vertical line one texel wide, blurred by 2: each column takes the kernel's weight.
        {
            weft::render::FilterPixels image{41, 5, std::vector<std::array<float, 4>>(41 * 5)};
            for (unsigned y = 0; y < 5; ++y) {
                image.rgba[y * 41 + 20] = {1, 1, 1, 1};
            }
            Op op;
            op.kind = Op::Kind::Blur;
            op.blur = 2;
            const auto chain = weft::render::plan_filter_chain({op}, 41, 5, 1);
            require(chain.steps.size() == 2 && chain.images.size() == 3 && chain.result == 2,
                    "a small blur: two passes, no downsample");
            const auto out = weft::render::run_filter_chain(chain, image);
            float total{};
            for (int x = 0; x < 41; ++x) {
                const float expected = std::exp(-float((x - 20) * (x - 20)) / 8);
                total += x >= 14 && x <= 26 ? expected : 0;
            }
            for (int x = 0; x < 41; ++x) {
                const float expected = x >= 14 && x <= 26 ? std::exp(-float((x - 20) * (x - 20)) / 8) / total : 0;
                require(std::abs(out.rgba[2 * 41 + std::size_t(x)][3] - expected) <= 1.f / 255 + 1e-6f,
                        "the line's column " + std::to_string(x) + " takes the kernel's weight");
            }
        }
        // An edge blurred by 12 at a quarter of its pixels: within two levels of the Gaussian's error function.
        {
            constexpr unsigned w = 200;
            weft::render::FilterPixels image{w, 8, std::vector<std::array<float, 4>>(w * 8)};
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 100; x < w; ++x) {
                    image.rgba[y * w + x] = {1, 1, 1, 1};
                }
            }
            Op op;
            op.kind = Op::Kind::Blur;
            op.blur = 12;
            const auto chain = weft::render::plan_filter_chain({op}, w, 8, 1);
            require(chain.images.back() == std::array<std::uint32_t, 2>{66, 2}, "downsampled to a third");
            const auto out = weft::render::run_filter_chain(chain, image);
            // The result covers the same box: sample it bilinearly at each pixel, as the composite does.
            int worst{};
            for (unsigned x = 40; x < 160; ++x) {
                const float u = (x + .5f) * float(out.width) / float(w) - .5f;
                const auto x0 = std::clamp(int(std::floor(u)), 0, int(out.width) - 1);
                const auto x1 = std::clamp(x0 + 1, 0, int(out.width) - 1);
                const float t = u - std::floor(u);
                const float a = out.rgba[out.width + x0][3] * (1 - t) + out.rgba[out.width + x1][3] * t;
                const float expected = .5f * (1 + std::erf((float(x) + .5f - 100) / (12 * std::sqrt(2.f))));
                worst = std::max(worst, int(std::lround(std::abs(a - expected) * 255)));
            }
            std::cout << "a wide blur against the error function: worst " << worst << " levels\n";
            require(worst <= 3, "a wide blur within three levels of the Gaussian");
        }
        // A drop shadow: the content's alpha offset under it, tinted; the content over it.
        {
            weft::render::FilterPixels image{20, 20, std::vector<std::array<float, 4>>(400)};
            for (unsigned y = 8; y < 12; ++y) {
                for (unsigned x = 8; x < 12; ++x) {
                    image.rgba[y * 20 + x] = {.5f, .5f, .5f, .5f};
                }
            }
            Op op;
            op.kind = Op::Kind::DropShadow;
            op.x = 3;
            op.y = 2;
            op.color = {1, 0, 0, 1};
            const auto out = weft::render::run_filter_chain(weft::render::plan_filter_chain({op}, 20, 20, 1), image);
            const auto at = [&](unsigned x, unsigned y) {
                return out.rgba[y * 20 + x];
            };
            const auto red = [](const std::array<float, 4>& c, float r, float a) {
                return std::abs(c[0] - r) < .01f && c[1] < .01f && std::abs(c[3] - a) < .01f;
            };
            require(red(at(13, 12), .5f, .5f), "the shadow alone: red at the content's alpha");
            // The content (gray at half alpha) over the shadow beneath it: .5 + .5 x .5 red.
            require(std::abs(at(11, 11)[0] - .75f) < .01f && std::abs(at(11, 11)[1] - .5f) < .01f &&
                        std::abs(at(11, 11)[3] - .75f) < .01f,
                    "the content over its shadow");
            require(std::abs(at(8, 8)[0] - .5f) < .01f && std::abs(at(8, 8)[3] - .5f) < .01f,
                    "the content past its shadow");
        }
    }

    // How far two images (one encoded premultiplied BGRA from a pass, one straight RGBA from the reference raster, both
    // opaque) lie apart: the worst level and the pixels more than tolerance apart, and where the first lies.
    struct Apart {
        int worst{};
        std::size_t count{};
        std::string first;
    };

    Apart compare(const std::vector<std::uint8_t>& bgra, const std::vector<std::uint8_t>& rgba, unsigned w, unsigned h,
                  int tolerance, std::array<int, 4> region = {}) {
        Apart out;
        if (region[2] == 0) {
            region = {0, 0, int(w), int(h)};
        }
        for (int y = std::max(0, region[1]); y < std::min(int(h), region[3]); ++y) {
            for (int x = std::max(0, region[0]); x < std::min(int(w), region[2]); ++x) {
                const auto* n = bgra.data() + (std::size_t(y) * w + std::size_t(x)) * 4;
                const auto* r = rgba.data() + (std::size_t(y) * w + std::size_t(x)) * 4;
                int d{};
                for (unsigned c = 0; c < 3; ++c) {
                    d = std::max(d, std::abs(int(n[2 - c]) - int(r[c])));
                }
                out.worst = std::max(out.worst, d);
                if (d > tolerance && out.count++ == 0) {
                    out.first = std::to_string(x) + "," + std::to_string(y) + ": native " + std::to_string(n[2]) + " " +
                                std::to_string(n[1]) + " " + std::to_string(n[0]) + ", reference " +
                                std::to_string(r[0]) + " " + std::to_string(r[1]) + " " + std::to_string(r[2]);
                }
            }
        }
        return out;
    }

    // Stripes of three colors in a 48 x 24 box at (x, y): what color functions visibly change.
    weft::SceneNode stripes(std::uint32_t slot, float x, float y, const char* filter, const char* opacity = "1") {
        return node(slot, {x, y, 48, 24}, {{"filter", filter}, {"opacity", opacity}},
                    {node(slot + 1, {0, 0, 16, 24}, {{"background-color", "#e74c3c"}}),
                     node(slot + 2, {16, 0, 16, 24}, {{"background-color", "#2ecc71"}}),
                     node(slot + 3, {32, 0, 16, 24}, {{"background-color", "#3498db"}})});
    }

    // The cases drawn: blurs (one downsampled), drop shadows of boxes, each color function, url() filters, a chain, an
    // element's opacity over its filter, all on whole px over an opaque root.
    weft::SceneFrame cases() {
        weft::SceneFrame f;
        f.root =
            node(1, {0, 0, 320, 240}, {{"background-color", "#303848"}},
                 {filter_defs(), node(10, {12, 12, 40, 24}, {{"background-color", "#f0c030"}, {"filter", "blur(2px)"}}),
                  // The subtitle glow: a radial gradient blurred by 10px, then faded.
                  node(11, {80, 8, 96, 40},
                       {{"background-image",
                         "radial-gradient(ellipse at center, rgba(5, 3, 10, 0.7) 0%, rgba(5, 3, 10, 0.25) 50%, "
                         "transparent 80%)"},
                        {"filter", "blur(10px)"}}),
                  node(12, {200, 12, 40, 24},
                       {{"background-color", "#e0e0e0"}, {"filter", "drop-shadow(3px 4px 2px rgba(0,0,0,0.8))"}},
                       {node(13, {8, 6, 24, 12}, {{"background-color", "#c04040"}})}),
                  node(14, {260, 12, 40, 24},
                       {{"background-color", "#8080ff"},
                        {"filter",
                         "drop-shadow(0 1px 2px rgba(0, 0, 0, 0.102)) drop-shadow(0 1px 1px rgba(0, 0, 0, 0.627))"}}),
                  stripes(20, 12, 70, "grayscale(1)"), stripes(30, 72, 70, "sepia(1)"),
                  stripes(40, 132, 70, "saturate(3)"), stripes(50, 192, 70, "hue-rotate(90deg)"),
                  stripes(60, 252, 70, "invert(1)"), stripes(70, 12, 110, "brightness(1.5)"),
                  stripes(80, 72, 110, "contrast(1.5)"), stripes(90, 132, 110, "opacity(0.5)"),
                  stripes(100, 192, 110, "url(#deut)"), stripes(110, 252, 110, "url(#lin)"),
                  stripes(120, 12, 160, "blur(1px) drop-shadow(2px 2px 1px #000000) grayscale(0.5)"),
                  stripes(130, 72, 160, "url(#pair) contrast(0.8)", "0.6"),
                  stripes(140, 132, 160, "drop-shadow(-2px 3px 0 #ffffff)"), stripes(150, 192, 160, "blur(6px)")});
        return f;
    }

    // The native D3D11 pass draws every case as the reference raster draws it, pixel for pixel, at scale 1 and 2.
    void drawn(const std::filesystem::path& folder) {
        const auto frame = cases();
        for (const float scale : {1.f, 2.f}) {
            const auto w = unsigned(320 * scale);
            const auto h = unsigned(240 * scale);
            weft::testing::D3D11Scene d3d11{w, h};
            const auto native = d3d11.draw(frame, scale);
            d3d11.gpu.check_debug_messages();
            require(d3d11.scene.fallbacks == 0, "the native pass drew the frame");
            require(d3d11.scene.filter_builds == 18 && d3d11.scene.filter_passes == 18,
                    "eighteen filtered images: " + std::to_string(d3d11.scene.filter_builds));
            weft::SceneRaster reference(weft::testing::thread_scene_text());
            const auto raster = reference.raster(frame, scale);
            require(raster.width == w && raster.height == h, "the reference raster's size");
            const auto apart = compare(native, raster.rgba, w, h, 2);
            std::vector<std::uint8_t> pair(std::size_t(w) * h * 8);
            for (unsigned y = 0; y < h; ++y) {
                for (unsigned x = 0; x < w; ++x) {
                    const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                    auto* left = pair.data() + (std::size_t(y) * w * 2 + x) * 4;
                    auto* right = left + std::size_t(w) * 4;
                    left[0] = n[2];
                    left[1] = n[1];
                    left[2] = n[0];
                    left[3] = 255;
                    std::copy_n(raster.rgba.data() + (std::size_t(y) * w + x) * 4, 4, right);
                }
            }
            weft::testing::write_png(folder / ("filters-" + std::to_string(int(scale)) + ".png"), w * 2, h, pair);
            std::printf("scale %.0f: native against the reference raster, worst %d levels, %zu pixels past 2%s%s\n",
                        scale, apart.worst, apart.count, apart.count ? ", the first at " : "", apart.first.c_str());
            require(apart.count == 0, "the native pass draws the filters as the reference raster");
            // What the filters do, at a few points of the native image (scale 1 coordinates).
            const auto at = [&](float x, float y) {
                const auto* p = native.data() + (std::size_t(y * scale) * w + std::size_t(x * scale)) * 4;
                return std::array<int, 3>{p[2], p[1], p[0]};
            };
            const auto gray = [](const std::array<int, 3>& c) {
                return std::abs(c[0] - c[1]) <= 1 && std::abs(c[1] - c[2]) <= 1;
            };
            require(gray(at(20, 82)) && gray(at(36, 82)) && gray(at(52, 82)), "grayscale(1) grays the stripes");
            require(at(31, 13)[0] < 0xf0 && at(31, 13)[0] > 0x30, "blur(2px) softens the box's edge");
            require(at(241, 30)[0] < 0x28, "the drop shadow past the box");
        }
    }

    // A filtered element's image stays while its opacity and transform change (their own frames' passes draw it
    // again, moved and faded); its content changes and it is filtered again.
    void cache(const std::filesystem::path& folder) {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:200px;height:120px;background-color:#202020");
        auto& glow = s.html("div");
        s.set(glow, "style",
              "position:absolute;left:20px;top:20px;width:60px;height:30px;background-color:#ff8040;"
              "filter:blur(3px) drop-shadow(2px 2px 2px #000000)");
        s.tree.insert(*s.root, glow, nullptr);
        auto& inner = s.html("div");
        s.set(inner, "style", "position:absolute;left:10px;top:10px;width:20px;height:10px;background-color:#40c0ff");
        s.tree.insert(glow, inner, nullptr);
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        weft::testing::D3D11Scene d3d11{200, 120};
        weft::SceneRaster reference(weft::testing::thread_scene_text());
        const auto draw = [&](double time, const char* what) {
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 120, time);
            const auto native = d3d11.draw(frame);
            const auto apart = compare(native, reference.raster(frame, 1).rgba, 200, 120, 2);
            require(apart.count == 0,
                    std::string(what) + ": drawn as the reference raster draws it, the first apart at " + apart.first);
            return native;
        };
        const auto first = draw(0, "at first");
        require(d3d11.scene.filter_builds == 1 && d3d11.scene.filter_passes == 1, "filtered once");
        // Faded and moved: the same image.
        s.set(glow, "style",
              "position:absolute;left:20px;top:20px;width:60px;height:30px;background-color:#ff8040;"
              "filter:blur(3px) drop-shadow(2px 2px 2px #000000);opacity:0.5;transform:translate(30px, 20px)");
        const auto moved = draw(16, "faded and moved");
        require(d3d11.scene.filter_builds == 1 && d3d11.scene.filter_reuses == 1 && d3d11.scene.filter_passes == 1,
                "an opacity and a transform change: not filtered again");
        require(d3d11.scene.kept_patches == 1, "the kept pass patched: the filtered item's record alone");
        require(moved != first, "drawn where the transform puts it, faded");
        // Turned and scaled: the same image still, drawn through the transform.
        s.set(glow, "style",
              "position:absolute;left:20px;top:20px;width:60px;height:30px;background-color:#ff8040;"
              "filter:blur(3px) drop-shadow(2px 2px 2px #000000);opacity:0.8;transform:rotate(12deg) scale(1.2)");
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 120, 32);
        (void)d3d11.draw(frame);
        require(d3d11.scene.filter_builds == 1 && d3d11.scene.filter_reuses == 2 && d3d11.scene.filter_passes == 1 &&
                    d3d11.scene.kept_patches == 2,
                "a rotation and a scale: not filtered again");
        // Its content changes: filtered again.
        s.set(inner, "style", "position:absolute;left:10px;top:10px;width:20px;height:10px;background-color:#ffffff");
        (void)draw(48, "its content changed");
        require(d3d11.scene.filter_builds == 2 && d3d11.scene.filter_passes == 2, "content changed: filtered again");
        // Its filter changes: filtered again.
        s.set(glow, "style",
              "position:absolute;left:20px;top:20px;width:60px;height:30px;background-color:#ff8040;"
              "filter:blur(1px)");
        (void)draw(64, "its filter changed");
        require(d3d11.scene.filter_builds == 3 && d3d11.scene.filter_passes == 3 && d3d11.scene.kept_patches == 3,
                "filter changed: filtered again, in a patch");
        d3d11.gpu.check_debug_messages();
        std::vector<std::uint8_t> rgba(moved.size());
        for (std::size_t i = 0; i < moved.size(); i += 4) {
            rgba[i] = moved[i + 2];
            rgba[i + 1] = moved[i + 1];
            rgba[i + 2] = moved[i];
            rgba[i + 3] = moved[i + 3];
        }
        weft::testing::write_png(folder / "filter-moved.png", 200, 120, rgba);
    }

    // A forgiving page keeps the filters the scene draws and drops the others with a note, as CSS drops what it does
    // not take: a length it does not take, a url() of a filter of other primitives, a filter on svg content. url()
    // filters are looked up in the document the solver reads.
    void forgiving() {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:200px;height:100px;background-color:#202020");
        const auto add = [&](weft::dom::Node& parent, weft::dom::Node& child) -> weft::dom::Node& {
            s.tree.insert(parent, child, nullptr);
            return child;
        };
        auto& defs_svg = add(*s.root, s.svg("svg"));
        s.set(defs_svg, "style", "position:absolute;width:0;height:0");
        auto& defs = add(defs_svg, s.svg("defs"));
        auto& gauss = add(defs, s.svg("filter"));
        s.set(gauss, "id", "g");
        s.set(add(gauss, s.svg("feGaussianBlur")), "stdDeviation", "2");
        auto& matrix = add(defs, s.svg("filter"));
        s.set(matrix, "id", "m");
        s.set(matrix, "color-interpolation-filters", "sRGB");
        auto& primitive = add(matrix, s.svg("feColorMatrix"));
        s.set(primitive, "type", "saturate");
        s.set(primitive, "values", "0");
        const auto box = [&](const char* id, const char* filter) {
            auto& e = add(*s.root, s.html("div"));
            s.set(e, "id", id);
            s.set(e, "style",
                  std::string("position:absolute;left:10px;top:10px;width:20px;height:20px;"
                              "background-color:#ff0000;filter:") +
                      filter);
        };
        box("glow", "blur(4px)");
        box("em", "blur(1em)");
        box("gauss", "url(#g)");
        box("matrix", "url(#m)");
        auto& icon = add(*s.root, s.svg("svg"));
        s.set(icon, "width", "10");
        s.set(icon, "height", "10");
        s.set(icon, "style", "position:absolute;left:50px;top:10px");
        auto& path = add(icon, s.svg("path"));
        s.set(path, "d", "M0 0 L10 0 L5 10 Z");
        s.set(path, "style", "filter:drop-shadow(1px 1px #000000)");
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text(), true, true);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 100, 0);
        const auto warnings = solver.take_warnings();
        const auto says = [&](const std::string& message) {
            return std::find(warnings.begin(), warnings.end(), message) != warnings.end();
        };
        std::string all;
        for (const auto& w : warnings) {
            all += "\n  " + w;
        }
        require(warnings.size() == 3 && says("filter blur(1em) on <div> (blur() takes a length of 0 to 256px)") &&
                    says("filter url(#g) on <div> (url(#g): feGaussianBlur is not a filter primitive the scene "
                         "takes)") &&
                    says("filter drop-shadow(1px 1px #000000) on <path> (the scene draws no filter of an SVG element "
                         "inside svg)"),
                "three filters dropped with a note:" + all);
        const auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 1);
        require(listed.fallback.empty() && listed.list.filters.size() == 2 &&
                    listed.list.filters[1].ops.at(0).kind == Op::Kind::Matrix &&
                    !listed.list.filters[1].ops.at(0).linear,
                "the blur and the url() filter drawn: " + listed.fallback);
    }

#ifdef WEFT_TEST_VULKAN
    // The Vulkan pass draws the cases as the D3D11 pass does.
    std::string vulkan() {
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        const auto device = vk::Device::open({}, unavailable);
        if (!device) {
            return "skipped: " + unavailable;
        }
        vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
        const auto frame = cases();
        for (const float scale : {1.f, 2.f}) {
            const auto w = unsigned(320 * scale);
            const auto h = unsigned(240 * scale);
            const auto drawn = scene.draw(frame, scale, w, h, std::nullopt, 0, {});
            require(drawn.has_value() && !scene.fallbacks, "native Vulkan pass");
            weft::testing::D3D11Scene d3d11{w, h};
            const auto warp = d3d11.draw(frame, scale);
            int worst{};
            for (std::size_t i = 0; i < warp.size(); ++i) {
                worst = std::max(worst, std::abs(int((*drawn)[i]) - int(warp[i])));
            }
            std::cout << "Vulkan vs D3D11 WARP at scale " << scale << ": worst " << worst << '\n';
            require(worst <= 2, "Vulkan against D3D11 WARP");
        }
        require(scene.filter_passes == 36, "Vulkan filtered each image once per scale");
        require(device->validation_messages == 0, "Vulkan validation");
        return device->name;
    }
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_scene_filter_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        parses();
        lists();
        chains();
        drawn(folder);
        cache(folder);
        forgiving();
        std::string vulkan_device = "not built";
#ifdef WEFT_TEST_VULKAN
        vulkan_device = vulkan();
#endif
        std::cout
            << "PASS CSS filter in scenes: functions, url() filters, lists, chains, the native D3D11 pass against "
               "the reference raster, the cache, forgiving pages; Vulkan "
            << vulkan_device << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
