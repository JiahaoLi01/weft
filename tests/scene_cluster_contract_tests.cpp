#define NOMINMAX
#include "../probes/gallery_cluster_input.hpp"
#include "support/scene_fonts.hpp"
#include "support/check.hpp"
#include "support/host_time.hpp"
#include "scene_test_image.hpp"
#include "cluster_visual_compare.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_path.hpp"
#include "test_constants/scene.hpp"
#include "constants/render.hpp"
#include <thread>
#include <cmath>
#include <cstring>
#include <numbers>

using weft::testing::format_host_time;
using weft::testing::require;

int main() {
    try {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        struct ComLifetime {
            ~ComLifetime() {
                CoUninitialize();
            }
        } com;

        GallerySceneSession s("ICON_TESTS", 0, 788, 288, "cluster-utilities.css");
        s.vm.eval("__ceGallery.unmount()");
        s.pump();
        {
            weft::SceneFrame frame;
            auto& n = frame.root;
            n.identity = {1, 1, 99};
            n.tag = "div";
            n.text = "🔥";
            n.box = {0, 0, 40, 40};
            n.style = {
                {"font-size", "24px"}, {"color", "#d4a843"}, {"transform", "translateY(20px)"}, {"opacity", "0.2"}};
            n.attributes["style"] = "transform:translateY(20px);opacity:0.2";
            auto source = [&] {
                auto plan = weft::scene_display_list(frame);
                require(plan.fallback.empty() && plan.list.items.size() == 1 &&
                            plan.list.items[0].kind == weft::paint::Item::Kind::Text,
                        "Unicode source primitive");
                return std::pair{plan.list.items[0], plan.list.rasters[plan.list.items[0].payload].source};
            };
            const auto first = source();
            n.style["transform"] = "translateY(3px)";
            n.style["opacity"] = "0.8";
            n.attributes["style"] = "transform:translateY(3px);opacity:0.8";
            const auto moved = source();
            require(weft::scene_paint_source_equal(*first.second, *moved.second) &&
                        first.first.transform != moved.first.transform && first.first.opacity != moved.first.opacity,
                    "Unicode motion must reuse source while changing composite");
            n.style["color"] = "#ff0000";
            require(!weft::scene_paint_source_equal(*first.second, *source().second),
                    "Unicode color must invalidate source");
            n.style["color"] = "#d4a843";
            n.style["font-size"] = "28px";
            require(!weft::scene_paint_source_equal(*first.second, *source().second),
                    "Unicode font must invalidate source");
            n.style["font-size"] = "24px";
            n.text = "⭐";
            require(!weft::scene_paint_source_equal(*first.second, *source().second),
                    "Unicode content must invalidate source");
        }
        {
            // Shadows come right under their box, bottom first (CSS lists the topmost first), with the radii as
            // specified; a transparent one is left out; with opacity, a box and its shadows are one group.
            weft::SceneFrame frame;
            auto& n = frame.root;
            n.identity = {1, 1, 130};
            n.tag = "div";
            n.box = {0, 0, 60, 40};
            n.style = {{"background-color", "#ffffff"},
                       {"opacity", "0.5"},
                       {"border-top-left-radius", "8px"},
                       {"box-shadow", "1px 2px 3px 4px #ff0000, 0 0 #0000, -5px -6px 7px 0px rgba(0, 0, 255, 0.5)"}};
            using Kind = weft::paint::Item::Kind;
            const auto plan = weft::scene_display_list(frame);
            const auto& items = plan.list.items;
            require(plan.fallback.empty() && items.size() == 5 && items[0].kind == Kind::BeginGroup &&
                        items[1].kind == Kind::Shadow && items[2].kind == Kind::Shadow && items[3].kind == Kind::Box &&
                        items[4].kind == Kind::EndGroup && items[4].opacity == .5f && items[1].opacity == 1,
                    "shadows under their box in the box's group");
            const auto& bottom = plan.list.shadows[items[1].payload];
            const auto& top = plan.list.shadows[items[2].payload];
            require(bottom.x == -5 && bottom.y == -6 && bottom.blur == 7 &&
                        bottom.color == weft::paint::Color{0, 0, 1, .5f},
                    "the last listed shadow is painted first");
            require(top.x == 1 && top.y == 2 && top.blur == 3 && top.spread == 4 &&
                        top.color == weft::paint::Color{1, 0, 0, 1} && top.radii == std::array<float, 4>{8, 0, 0, 0},
                    "the first listed shadow is painted last");
            // An inset shadow lies over the background and under the border, of the padding box with its inner radii;
            // the color may come first. Inside elliptical inner corners it stays a visible fallback.
            n.style.erase("opacity");
            n.style["box-shadow"] = "#000000 1px 2px 4px 3px inset";
            for (const auto* side : {"top", "right", "bottom", "left"}) {
                n.style[std::string("border-") + side + "-width"] = "2px";
                n.style[std::string("border-") + side + "-color"] = "#00ff00";
            }
            const auto inset = weft::scene_display_list(frame);
            const auto& listed = inset.list.items;
            require(inset.fallback.empty() && listed.size() == 3 && listed[0].kind == Kind::Box &&
                        listed[1].kind == Kind::Shadow && listed[2].kind == Kind::Box &&
                        inset.list.boxes[listed[0].payload].widths == std::array<float, 4>{} &&
                        inset.list.boxes[listed[2].payload].background[3] == 0,
                    "an inset shadow over the background, under the border");
            const auto& hollow = inset.list.shadows[listed[1].payload];
            require(hollow.inset && hollow.x == 1 && hollow.y == 2 && hollow.blur == 4 && hollow.spread == 3 &&
                        hollow.color == weft::paint::Color{0, 0, 0, 1} &&
                        hollow.radii == std::array<float, 4>{6, 0, 0, 0} && listed[1].width == 56 &&
                        listed[1].height == 36 && listed[1].transform[4] == 2 && listed[1].transform[5] == 2,
                    "an inset shadow of the padding box and its inner radii");
            n.style["border-left-width"] = "4px";
            require(weft::scene_display_list(frame).fallback.find("elliptical") != std::string::npos,
                    "an inset shadow inside elliptical corners must stay a visible fallback");
        }
        {
            // SVG content draws natively: the svg element's viewport maps user units into its box and clips to its
            // rounded border box; an opacity group, fills and strokes as path items, dashes, a clip-path rectangle and
            // text placed by its anchor. What the list cannot draw stays a visible fallback.
            using Kind = weft::paint::Item::Kind;
            using Pairs = std::initializer_list<std::pair<const char*, const char*>>;
            const auto node = [](const char* tag, std::uint32_t slot, Pairs attributes, Pairs style) {
                weft::SceneNode e;
                e.identity = {1, 1, slot};
                e.tag = tag;
                e.space = weft::constants::svg_namespace_uri;
                for (const auto& [k, v] : attributes) {
                    e.attributes[k] = v;
                }
                for (const auto& [k, v] : style) {
                    e.style[k] = v;
                }
                return e;
            };
            weft::SceneFrame frame;
            frame.root.identity = {1, 1, 140};
            frame.root.tag = "div";
            frame.root.box = {0, 0, 200, 120};
            auto svg = node("svg", 141, {{"viewBox", "0 0 50 40"}}, {{"border-top-left-radius", "6px"}});
            svg.box = {10, 10, 100, 80};
            auto group = node("g", 142, {{"transform", "translate(5, 5)"}}, {{"opacity", "0.5"}});
            group.children = {
                node("rect", 143, {{"x", "0"}, {"y", "0"}, {"width", "10"}, {"height", "8"}},
                     {{"fill", "#ff0000"}, {"stroke", "#0000ff"}, {"stroke-width", "2"}, {"stroke-opacity", "0.5"}}),
                node("circle", 144, {{"cx", "20"}, {"cy", "10"}, {"r", "4"}}, {{"fill", "#00ff00"}})};
            auto defs = node("defs", 147, {}, {});
            auto clip_path = node("clipPath", 148, {{"id", "c"}}, {});
            clip_path.children = {node("rect", 149, {{"x", "2"}, {"y", "2"}, {"width", "6"}, {"height", "6"}}, {})};
            defs.children = {clip_path};
            const auto label = node("text", 151, {{"x", "30"}, {"y", "30"}},
                                    {{"text-anchor", "middle"}, {"font-size", "10px"}, {"fill", "#123456"}});
            svg.children = {defs,
                            group,
                            node("path", 145, {{"d", "M0 0 L10 0 L10 10 Z"}},
                                 {{"fill", "none"}, {"stroke", "#000000"}, {"stroke-dasharray", "2 1"}}),
                            node("line", 146, {{"x1", "0"}, {"y1", "0"}, {"x2", "10"}, {"y2", "0"}}, {}),
                            node("rect", 150,
                                 {{"x", "0"}, {"y", "0"}, {"width", "10"}, {"height", "10"}, {"clip-path", "url(#c)"}},
                                 {{"fill", "#ffffff"}}),
                            label};
            svg.children.edit_back().text = "A";
            frame.root.children = {svg};
            auto& text = weft::testing::thread_scene_text();
            const auto plan = weft::scene_display_list(frame, {}, &text);
            const auto& items = plan.list.items;
            // Rects and circles are boxes (12.2): a rect's fill, its stroke as a border box half the stroke out, a
            // circle's fill; a dashed stroke stays a path, and so does text.
            require(plan.fallback.empty() && items.size() == 8 && items[0].kind == Kind::BeginGroup &&
                        items[1].kind == Kind::Box && items[2].kind == Kind::Box && items[3].kind == Kind::Box &&
                        items[4].kind == Kind::EndGroup && items[5].kind == Kind::Path && items[6].kind == Kind::Box &&
                        items[7].kind == Kind::Path,
                    "SVG items: a group of fills and strokes, a dashed stroke, a clipped fill, text; no line fill");
            const auto at = [&](std::size_t i) -> const weft::paint::PathPaint& {
                return plan.list.paths[items[i].payload];
            };
            const auto box = [&](std::size_t i) -> const weft::paint::Box& {
                return plan.list.boxes[items[i].payload];
            };
            const weft::paint::Color clear{};
            const weft::paint::Color blue_half{0, 0, 1, .5f};
            require(items[0].opacity == .5f && items[4].opacity == .5f &&
                        box(1).background == weft::paint::Color{1, 0, 0, 1} &&
                        box(1).widths == std::array<float, 4>{} && box(1).radii == std::array<float, 4>{} &&
                        box(2).background == clear && box(2).widths == std::array<float, 4>{2, 2, 2, 2} &&
                        box(2).edges == std::array<weft::paint::Color, 4>{blue_half, blue_half, blue_half, blue_half} &&
                        box(2).radii == std::array<float, 4>{} && box(3).background == weft::paint::Color{0, 1, 0, 1} &&
                        box(3).radii == std::array<float, 4>{4, 4, 4, 4},
                    "SVG fills and strokes");
            require(items[1].transform == weft::paint::Affine{2, 0, 0, 2, 20, 20} && items[1].width == 10 &&
                        items[1].height == 8 && items[2].transform == weft::paint::Affine{2, 0, 0, 2, 18, 18} &&
                        items[2].width == 12 && items[2].height == 10 &&
                        items[3].transform == weft::paint::Affine{2, 0, 0, 2, 52, 32} && items[3].width == 8 &&
                        items[3].height == 8 && items[5].transform == weft::paint::Affine{2, 0, 0, 2, 10, 10},
                    "SVG viewport and group transforms");
            require(at(5).stroke && at(5).stroke->dashes == std::vector<float>{2, 1} && at(5).path->verbs.size() == 4,
                    "SVG dashed stroke");
            require(items[1].clip == std::array<float, 4>{10, 10, 110, 90} && items[1].clips_count == 1 &&
                        plan.list.clips[items[1].clips_begin].radius_x[0] == 6,
                    "SVG viewport clips to the rounded svg box");
            require(items[6].clip == std::array<float, 4>{14, 14, 26, 26}, "SVG clip-path rectangle");
            // Text: its glyph outlines as a path in the line box, placed by the anchor and the baseline.
            const auto metrics = text.measure("A", label.style);
            const auto outline = text.outline("A", label.style);
            bool inside = outline && !outline->points.empty();
            for (const auto& point : outline ? outline->points : std::vector<std::array<float, 2>>{}) {
                inside = inside && point[0] >= -1 && point[0] <= metrics.width + 1 && point[1] >= 0 &&
                         point[1] <= metrics.height;
            }
            require(inside && at(7).path == outline && !at(7).stroke &&
                        at(7).color == weft::paint::Color{0x12 / 255.f, 0x34 / 255.f, 0x56 / 255.f, 1} &&
                        items[7].transform[4] == 10 + 2 * (30 - metrics.width / 2) &&
                        items[7].transform[5] == 10 + 2 * (30 - metrics.baseline),
                    "SVG text as glyph outlines placed by its anchor and baseline");
            // Arcs become cubics that stay on their circles: an annular sector as d3.arc writes it, both sweeps.
            {
                const auto sector = node(
                    "path", 160, {{"d", "M0,-300A300,300,0,0,1,259.8076,150L173.2051,100A200,200,0,0,0,0,-200Z"}}, {});
                const auto path = weft::scene_svg_path(sector);
                double worst{};
                std::size_t p{};
                std::array<float, 2> current{};
                for (const auto verb : path->verbs) {
                    using Verb = weft::paint::Path::Verb;
                    if (verb == Verb::Cubic) {
                        const auto p0 = current;
                        const auto c1 = path->points[p];
                        const auto c2 = path->points[p + 1];
                        const auto p3 = path->points[p + 2];
                        const double radius = std::hypot(p0[0], p0[1]);
                        for (double t = .125; t < 1; t += .125) {
                            const double u = 1 - t;
                            const double x =
                                u * u * u * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t * t * t * p3[0];
                            const double y =
                                u * u * u * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t * t * t * p3[1];
                            worst = std::max(worst, std::abs(std::hypot(x, y) - radius));
                        }
                        current = p3;
                        p += 3;
                    } else if (verb != Verb::Close) {
                        current = path->points[p++];
                    }
                }
                require(worst < .05, "SVG arcs stay on their circles: " + std::to_string(worst));
            }
            require(weft::scene_display_list(frame).fallback == "SVG text reference", "SVG text needs a text engine");
            auto foreign = frame;
            foreign.root.children.edit(0).children.push_back(node("foreignObject", 152, {}, {}));
            require(weft::scene_display_list(foreign, {}, &text).fallback == "SVG element foreignObject",
                    "unknown SVG element");
            auto rotated = frame;
            rotated.root.children.edit(0).children.edit(4).attributes["transform"] = "rotate(10)";
            // Under a rotation the clip-path rectangle clips in the element's own space: its map from target px is
            // the inverse of the element's transform.
            const auto turned = weft::scene_display_list(rotated, {}, &text);
            const auto& clipped = turned.list.items[6];
            require(turned.fallback.empty() && clipped.kind == Kind::Box && clipped.clips_count == 2,
                    "rotated clip-path keeps the viewport clip and adds its own");
            const auto& local = turned.list.clips[clipped.clips_begin + 1];
            const auto& m = clipped.transform;
            const auto& map = local.to_local;
            const float tx = 4 * m[0] + 3 * m[2] + m[4];
            const float ty = 4 * m[1] + 3 * m[3] + m[5];
            require(local.rect == std::array<float, 4>{2, 2, 6, 6} &&
                        std::abs(tx * map[0] + ty * map[2] + map[4] - 4) < 1e-3f &&
                        std::abs(tx * map[1] + ty * map[3] + map[5] - 3) < 1e-3f,
                    "rotated clip-path in its own space");
        }
        {
            // A rotated box with overflow hidden clips its content to its padding box with inner corners, in its own
            // space.
            weft::SceneFrame frame;
            frame.root.identity = {1, 1, 170};
            frame.root.tag = "div";
            frame.root.box = {0, 0, 200, 200};
            weft::SceneNode box;
            box.identity = {1, 1, 171};
            box.tag = "div";
            box.box = {50, 50, 80, 60};
            box.style = {{"overflow", "hidden"},
                         {"transform", "rotate(30deg)"},
                         {"border-top-left-radius", "12px"},
                         {"border-top-width", "2px"},
                         {"border-left-width", "4px"}};
            weft::SceneNode child;
            child.identity = {1, 1, 172};
            child.tag = "div";
            child.box = {0, 0, 80, 60};
            child.style = {{"background-color", "#ff0000"}};
            box.children = {child};
            frame.root.children = {box};
            const auto plan = weft::scene_display_list(frame);
            // The box (its borders) and its child; only the child is inside the clip.
            require(plan.fallback.empty() && plan.list.items.size() == 2 && plan.list.items[0].clips_count == 0 &&
                        plan.list.items[1].clips_count == 1,
                    "rotated overflow clip");
            const auto& c = plan.list.clips[plan.list.items[1].clips_begin];
            require(c.rect == std::array<float, 4>{4, 2, 76, 58} && c.radius_x[0] == 8 && c.radius_y[0] == 10 &&
                        c.to_local != weft::identity_clip_map,
                    "rotated overflow clips its padding box in its own space");
            // The local origin of the box maps back from where the box's transform puts it.
            const auto& m = plan.list.items[1].transform;
            const auto& map = c.to_local;
            require(std::abs(m[4] * map[0] + m[5] * map[2] + map[4]) < 1e-3f &&
                        std::abs(m[4] * map[1] + m[5] * map[3] + map[5]) < 1e-3f,
                    "rotated overflow clip map");
        }
        {
            // A perspective box is a plane: its subtree listed flat in its own px (the box at the origin without its
            // transform), drawn through the box's plane after its parent's transform, inside its parent's clips. An
            // edge-on plane draws nothing, a plane in a plane nests, a third plane inside falls back, and so does a
            // plane behind the viewer.
            using Kind = weft::paint::Item::Kind;
            const auto node = [](std::uint32_t slot, std::array<float, 4> box,
                                 std::initializer_list<std::pair<const char*, const char*>> style) {
                weft::SceneNode n;
                n.identity = {1, 1, slot};
                n.tag = "div";
                n.box = box;
                for (const auto& [k, v] : style) {
                    n.style[k] = v;
                }
                return n;
            };
            weft::SceneFrame frame;
            frame.root = node(180, {0, 0, 200, 200}, {});
            auto outer =
                node(181, {10, 20, 150, 150},
                     {{"transform", "translateX(5px)"}, {"overflow", "hidden"}, {"border-top-left-radius", "8px"}});
            auto card = node(182, {20, 30, 60, 40},
                             {{"transform", "perspective(200px) rotateY(30deg)"}, {"background-color", "#ff0000"}});
            card.children = {node(183, {5, 5, 10, 10}, {{"background-color", "#0000ff"}})};
            outer.children = {card};
            frame.root.children = {outer};
            const auto plan = weft::scene_display_list(frame);
            require(plan.fallback.empty() && plan.list.items.size() == 1 && plan.list.items[0].kind == Kind::Plane &&
                        plan.list.items[0].clips_count == 1 &&
                        bool(weft::paint::check(plan.list, weft::constants::render::scene::max_group_depth)),
                    "perspective box is a plane");
            const auto& plane = plan.list.planes[plan.list.items[0].payload];
            const auto& content = *plane.content;
            require(plane.width == 60 && plane.height == 40 && content.items.size() == 2 &&
                        content.items[0].transform == weft::paint::identity &&
                        content.items[1].transform == weft::paint::Affine{1, 0, 0, 1, 5, 5} &&
                        content.items[0].clip == weft::paint::ClipRect{0, 0, 60, 40},
                    "plane content in its own px");
            // The plane's corner (0, 0): rotated about the box's center (30, 20), at its offset (20, 30) in the parent,
            // which lies at (15, 20).
            const auto& h = plane.transform;
            const double c = std::cos(std::numbers::pi / 6);
            const double w = 1 + std::sin(std::numbers::pi / 6) * -30 / 200;
            require(std::abs(h[2] / h[8] - (15 + 20 + 30 + c * -30 / w)) < 1e-3 &&
                        std::abs(h[5] / h[8] - (20 + 30 + 20 - 20 / w)) < 1e-3,
                    "plane homography");
            auto edge_on = frame;
            edge_on.root.children.edit(0).children.edit(0).style["transform"] = "perspective(200px) rotateY(90deg)";
            const auto hidden = weft::scene_display_list(edge_on);
            require(hidden.fallback.empty() && hidden.list.items.empty(), "edge-on plane draws nothing");
            auto behind = frame;
            behind.root.children.edit(0).children.edit(0).style["transform"] = "perspective(20px) rotateY(80deg)";
            require(weft::scene_display_list(behind).fallback == "plane behind the viewer", "plane behind the viewer");
            auto nesting = frame;
            auto& first = nesting.root.children.edit(0).children.edit(0);
            first.children.push_back(
                node(184, {20, 10, 30, 20}, {{"transform", "perspective(100px) rotateY(-20deg)"}, {"opacity", "0.5"}}));
            const auto two = weft::scene_display_list(nesting);
            const auto& in = *two.list.planes[0].content;
            require(two.fallback.empty() && in.items.size() == 3 && in.items[2].kind == Kind::Plane &&
                        in.planes[in.items[2].payload].width == 30,
                    "plane in a plane");
            first.children.edit_back().children.push_back(
                node(185, {2, 2, 10, 10}, {{"transform", "perspective(100px) rotateY(10deg)"}}));
            require(weft::scene_display_list(nesting).fallback == "plane nesting bound", "plane nesting bound");
        }
        {
            // Short hex colors expand as the reference raster expands them: background, border and text tint.
            weft::SceneFrame frame;
            auto& n = frame.root;
            n.identity = {1, 1, 120};
            n.tag = "div";
            n.text = "hex";
            n.box = {0, 0, 60, 20};
            n.style = {{"background-color", "#3a7"},
                       {"border-top-width", "1px"},
                       {"border-top-color", "#f00"},
                       {"color", "#000"},
                       {"font-size", "12px"}};
            const auto plan = weft::scene_display_list(frame);
            require(plan.fallback.empty() && plan.list.items.size() == 2 &&
                        plan.list.items[1].kind == weft::paint::Item::Kind::Text,
                    "short hex colors fell back");
            const auto& box = plan.list.boxes[plan.list.items[0].payload];
            const float max = weft::constants::color_channel_max;
            require(box.background == std::array<float, 4>{0x33 / max, 0xaa / max, 0x77 / max, 1} &&
                        box.edges[0] == std::array<float, 4>{1, 0, 0, 1} &&
                        plan.list.rasters[plan.list.items[1].payload].tint == std::array<float, 4>{0, 0, 0, 1},
                    "short hex color values");
            // Length stops are fractions of the gradient line in the tile: a radial gradient's half diagonal, a
            // linear gradient's extent along its angle.
            n.text.clear();
            n.style["background-image"] = "radial-gradient(circle, #333 1px, transparent 1px)";
            n.style["background-size"] = "60px 60px";
            const auto radial = weft::scene_display_list(frame);
            const auto same_offset = [](float a, float b) {
                return std::abs(a - b) < 1e-5f;
            };
            const auto& dots = radial.list.boxes[radial.list.items[0].payload].gradient;
            require(radial.fallback.empty() && dots.kind == weft::paint::GradientKind::Radial &&
                        dots.stops.size() == 2 && same_offset(dots.stops[0].offset, 1 / std::hypot(30.f, 30.f)) &&
                        same_offset(dots.stops[1].offset, 1 / std::hypot(30.f, 30.f)) && dots.tile[0] == 60,
                    "radial length stops");
            n.style["background-image"] = "linear-gradient(90deg, #f00 6px, #00f 18px)";
            n.style["background-size"] = "60px 20px";
            const auto linear = weft::scene_display_list(frame);
            const auto& bands = linear.list.boxes[linear.list.items[0].payload].gradient;
            require(linear.fallback.empty() && bands.stops.size() == 2 && same_offset(bands.stops[0].offset, .1f) &&
                        same_offset(bands.stops[1].offset, .3f),
                    "linear length stops");
            n.style["background-image"] = "conic-gradient(#f00 6px, #00f 50%)";
            require(weft::scene_display_list(frame).fallback == "conic gradient length stop",
                    "conic length stop must stay a visible fallback");
        }
        {
            // Rounded overflow clips become clip chains, outermost first. A box's own background and its opacity
            // group take the parent's chain; its text and children add its clip. A mirroring scale moves each
            // corner's radii to where the corner lands and scales them per axis.
            weft::SceneFrame frame;
            auto& outer = frame.root;
            outer.identity = {1, 1, 100};
            outer.tag = "div";
            outer.box = {0, 0, 200, 120};
            outer.style = {{"overflow", "hidden"}, {"border-top-left-radius", "12px"}, {"background-color", "#202020"}};
            weft::SceneNode inner;
            inner.identity = {1, 1, 101};
            inner.tag = "div";
            inner.text = "clip";
            inner.box = {20, 10, 100, 80};
            inner.style = {{"overflow", "hidden"},
                           {"border-top-left-radius", "4px"},
                           {"opacity", "0.5"},
                           {"border-top-right-radius", "16px"},
                           {"font-size", "12px"},
                           {"transform", "scale(-2, 1)"},
                           {"background-color", "#ff0000"}};
            weft::SceneNode leaf;
            leaf.identity = {1, 1, 102};
            leaf.tag = "div";
            leaf.box = {0, 0, 10, 10};
            leaf.style = {{"background-color", "#00ff00"}};
            inner.children.push_back(leaf);
            outer.children.push_back(inner);
            using Kind = weft::paint::Item::Kind;
            const auto plan = weft::scene_display_list(frame);
            const auto& items = plan.list.items;
            const auto chain = [&](std::size_t i, std::uint32_t begin, std::uint32_t count) {
                return items[i].clips_begin == begin && items[i].clips_count == count;
            };
            require(plan.fallback.empty() && items.size() == 6 && items[0].kind == Kind::Box &&
                        items[1].kind == Kind::BeginGroup && items[2].kind == Kind::Box &&
                        items[3].kind == Kind::Text && items[4].kind == Kind::Box && items[5].kind == Kind::EndGroup,
                    "rounded overflow clip items");
            require(chain(0, 0, 0) && chain(1, 0, 1) && chain(2, 0, 1) && chain(5, 0, 1),
                    "own background and opacity group take the parent's rounded clip chain");
            require(chain(3, 1, 2) && chain(4, 1, 2), "text and children take the box's rounded clip chain");
            const auto& clips = plan.list.clips;
            const weft::RoundedClip outer_clip{{0, 0, 200, 120}, {12, 0, 0, 0}, {12, 0, 0, 0}};
            const auto same = [](const weft::RoundedClip& a, const weft::RoundedClip& b) {
                return a.rect == b.rect && a.radius_x == b.radius_x && a.radius_y == b.radius_y;
            };
            require(clips.size() == 3 && same(clips[0], outer_clip) && same(clips[1], outer_clip),
                    "a nested chain repeats the enclosing clips");
            require(same(clips[2], {{-30, 10, 200, 80}, {32, 8, 0, 0}, {16, 4, 0, 0}}),
                    "mirrored and scaled rounded clip corners");
            require(items[4].clip == std::array<float, 4>{0, 10, 170, 90}, "rounded clip's axis-aligned bounds");
            inner.style["transform"] = "rotate(10deg)";
            outer.children = {inner};
            // Rotated, the inner clip lies in the box's own space: the outer clip, then the inner one, unscaled.
            const auto rotated = weft::scene_display_list(frame);
            const auto& turned = rotated.list.items[4];
            require(rotated.fallback.empty() && turned.clips_count == 2 &&
                        rotated.list.clips[turned.clips_begin + 1].rect == std::array<float, 4>{0, 0, 100, 80} &&
                        rotated.list.clips[turned.clips_begin + 1].radius_x == std::array<float, 4>{4, 16, 0, 0} &&
                        rotated.list.clips[turned.clips_begin + 1].to_local != weft::identity_clip_map,
                    "rotated overflow clips in its own space");
        }
        {
            // An overflow clip is the padding box. Each inner radius is the outer one less the adjacent border width:
            // unequal widths make the corner elliptical, and a corner with either radius at zero is square.
            weft::SceneFrame frame;
            auto& n = frame.root;
            n.identity = {1, 1, 110};
            n.tag = "div";
            n.box = {0, 0, 100, 60};
            n.style = {{"overflow", "hidden"},
                       {"border-top-left-radius", "20px"},
                       {"border-top-right-radius", "6px"},
                       {"border-top-width", "4px"},
                       {"border-right-width", "8px"},
                       {"border-bottom-width", "2px"},
                       {"border-left-width", "10px"},
                       {"color", "#ffffff"}};
            weft::SceneNode child;
            child.identity = {1, 1, 111};
            child.tag = "div";
            child.box = {0, 0, 100, 60};
            child.style = {{"background-color", "#ff0000"}};
            n.children.push_back(child);
            const auto plan = weft::scene_display_list(frame);
            const auto& items = plan.list.items;
            const auto& clips = plan.list.clips;
            require(plan.fallback.empty() && items.size() == 2 && clips.size() == 1 && items[1].clips_begin == 0 &&
                        items[1].clips_count == 1,
                    "padding box overflow clip items");
            require(clips[0].rect == std::array<float, 4>{10, 4, 82, 54} &&
                        clips[0].radius_x == std::array<float, 4>{10, 0, 0, 0} &&
                        clips[0].radius_y == std::array<float, 4>{16, 0, 0, 0},
                    "overflow clip is the padding box with inner radii");
            require(items[1].clip == std::array<float, 4>{10, 4, 92, 58}, "overflow clip bounds are the padding box");
            n.style["border-top-left-radius"] = "4px";
            const auto square = weft::scene_display_list(frame);
            require(square.fallback.empty() && square.list.clips.empty() &&
                        square.list.items[1].clip == std::array<float, 4>{10, 4, 92, 58},
                    "radii inside the borders leave a square padding box clip");
        }
        {
            weft::ScriptDom cached;
            cached.eval(
                "globalThis.box=document.createElement('div');box.id='cachebox';box.setAttribute('data-width','10');document.appendChild(box)");
            std::uint64_t epoch = 1;
            unsigned reads{};
            unsigned styles{};
            double external{};
            auto width = [](const auto& document) {
                auto find = [&](auto&& self, const auto& n) -> double {
                    if (n.attributes.contains("id") && n.attributes.at("id") == "cachebox") {
                        return std::stod(n.attributes.at("data-width"));
                    }
                    for (const auto& child : n.children) {
                        if (auto value = self(self, child); value >= 0) {
                            return value;
                        }
                    }
                    return -1;
                };
                return find(find, document);
            };
            cached.install_layout_cache_host([&] {
                return epoch;
            });
            cached.install_layout_host([&](const auto& document) {
                ++reads;
                auto find = [&](auto&& self, const auto& n) -> weft::NodeIdentity {
                    if (n.attributes.contains("id") && n.attributes.at("id") == "cachebox") {
                        return n.identity;
                    }
                    for (const auto& child : n.children) {
                        if (auto id = self(self, child); id.generation) {
                            return id;
                        }
                    }
                    return {};
                };
                auto id = find(find, document);
                std::vector<weft::ElementGeometry> result;
                if (id.generation) {
                    weft::ElementGeometry g;
                    g.identity = id;
                    g.rect = {0, 0, width(document) + external, 10};
                    g.client_width = g.scroll_width = g.rect[2];
                    g.client_height = g.scroll_height = 10;
                    g.offset = g.rect;
                    g.client_rects = {g.rect};
                    result.push_back(g);
                }
                return result;
            });
            cached.install_computed_style_host([&](const auto& document, auto) {
                ++styles;
                return std::map<std::string, std::string>{
                    {"width", format_host_time(width(document) + external) + "px"}};
            });
            require(cached.eval("box.getBoundingClientRect().width+box.clientWidth+box.offsetWidth") == "30" &&
                        reads == 1,
                    "geometry cache unchanged reads");
            require(cached.eval("box.__ceComputedStyle().width+','+box.__ceComputedStyle().width") == "10px,10px" &&
                        styles == 1,
                    "computed cache unchanged reads");
            require(cached.eval("box.setAttribute('data-width','20');box.getBoundingClientRect().width") == "20" &&
                        reads == 2,
                    "geometry cache ignored DOM mutation");
            require(cached.eval("box.__ceComputedStyle().width") == "20px" && styles == 2,
                    "style cache ignored DOM mutation");
            external = 5;
            ++epoch;
            require(cached.eval("box.getBoundingClientRect().width") == "25" &&
                        cached.eval("box.__ceComputedStyle().width") == "25px",
                    "cache ignored external owner epoch");
            for (unsigned n = 0; n < 128; ++n) {
                auto w = 3 + (n * 73) % 181;
                auto text = std::to_string(w);
                require(cached.eval("box.setAttribute('data-width','" + text +
                                    "');box.getBoundingClientRect().width") == std::to_string(w + 5),
                        "synchronous mutation/read stale geometry");
            }
            cached.eval("document.removeChild(box)");
            require(cached.eval("box.getBoundingClientRect().width") == "0", "detached geometry survived cache");
            cached.eval("box=null");
            cached.collect_garbage();
            require(cached.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                    "owned layout cache retained native nodes");
            cached.reset_document();
            cached.collect_garbage();
            require(cached.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                    "layout cache reset epoch leak");
        }
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.createElement('button'),svg=document.createElementNS('http://www.w3.org/2000/svg','svg');return n.constructor!==Object&&n instanceof Element&&n instanceof EventTarget&&n instanceof HTMLElement&&n instanceof HTMLButtonElement&&!('ownerSVGElement' in n)&&svg instanceof SVGElement&&'ownerSVGElement' in svg&&'opacity' in n.style&&'backgroundColor' in n.style&&!('translateX' in n.style)})())JS") ==
                "true",
            "library brand/property detection");
        require(
            s.vm.eval(
                R"JS((()=>{const root=document.getElementById('root');root.innerHTML='<div id="fragment">A &amp; <b>B</b></div>';const before=root.innerHTML;let rejected=0;for(const value of ['bad<','<script>x</script>','<div><span></div>','<div onclick="x">bad</div>'])try{root.innerHTML=value}catch(e){++rejected}if(root.innerHTML!==before)throw Error('non-atomic fragment');document.getElementById('fragment').children[0].textContent='C';return rejected===4&&root.innerHTML.includes('<b>C</b>')&&root.innerHTML.includes('A &amp;')})())JS") ==
                "true",
            "bounded fragment atomicity/live serialization");
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.getElementById('fragment');n.classList.add('a','b','a');let rejected=false;try{n.classList.add('c','bad token')}catch(e){rejected=true}return rejected&&n.className==='a b'&&!n.classList.contains('c')&&n.classList.toggle('a')===false&&n.classList.value==='b'&&n.classList.toggle('a',true)===true})())JS") ==
                "true",
            "class token reflection/atomic validation");
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.getElementById('fragment'),controller=new AbortController();let calls=[];const handler={handleEvent(e){if(this!==handler)throw Error('wrong receiver');calls.push('one')}};n.addEventListener('check',handler);n.dispatchEvent(new Event('check'));handler.handleEvent=()=>calls.push('two');n.dispatchEvent(new Event('check'));n.removeEventListener('check',handler);n.addEventListener('check',()=>calls.push('aborted'),{signal:controller.signal});controller.abort();n.dispatchEvent(new Event('check'));let passive=new Event('passive',{cancelable:true});n.addEventListener('passive',e=>e.preventDefault(),{passive:true});n.dispatchEvent(passive);return calls.join(',')==='one,two'&&!passive.defaultPrevented})())JS") ==
                "true",
            "live handleEvent/abort/passive");
        s.vm.eval(
            "globalThis.events=[];globalThis.eventController=new AbortController();window.addEventListener('pointerdown',e=>events.push('capture:'+e.eventPhase+':'+(e instanceof PointerEvent)),{capture:true,signal:eventController.signal});window.addEventListener('pointerdown',e=>events.push('bubble:'+e.eventPhase),{signal:eventController.signal})");
        weft::MouseEvent event;
        event.type = weft::MouseType::PointerDown;
        s.vm.dispatch_mouse(s.vm.node_identity("#fragment"), event);
        require(s.vm.eval("events.join(',')") == "capture:1:true,bubble:3", "window pointer capture/brand/bubble");
        s.vm.eval("eventController.abort();events=[]");
        s.vm.dispatch_mouse(s.vm.node_identity("#fragment"), event);
        require(s.vm.eval("events.length") == "0", "window abort listeners survived");
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"box\" style=\"position:relative;width:100px;height:50px;padding:4px;border:2px solid #ffffff\"></div>';globalThis.observations=[];globalThis.observer=new ResizeObserver(es=>observations.push(es.map(e=>[e.contentRect.width,e.contentRect.height,e.borderBoxSize[0].inlineSize])));observer.observe(document.getElementById('box'))");
        s.step(1);
        require(s.vm.eval("JSON.stringify(observations)") == "[[[88,38,100]]]", "observer initial box");
        s.step(2);
        require(s.vm.eval("observations.length") == "1", "observer repeated unchanged box");
        s.vm.eval("document.getElementById('box').style.width='120px'");
        s.step(3);
        require(s.vm.eval("JSON.stringify(observations[1])") == "[[108,38,120]]", "observer fresh mutation box");
        s.vm.eval("observer.disconnect();document.getElementById('box').style.width='140px'");
        s.step(4);
        require(s.vm.eval("observations.length") == "2", "observer disconnect");
        require(s.vm.eval("(1234567).toLocaleString()==='1,234,567'&&(-1234).toLocaleString()==='-1,234'") == "true",
                "host integer locale formatting");
        require(
            s.vm.eval(
                "(()=>{let errors=0;try{(1.25).toLocaleString()}catch(e){++errors}try{matchMedia('(min-width: 10px)')}catch(e){++errors}return errors===2&&matchMedia('(pointer:fine)').matches&&!matchMedia('(prefers-reduced-motion: reduce)').matches})()") ==
                "true",
            "unsupported platform profile visible");
        s.vm.eval(
            "document.getElementById('box').style.transform='translateX(25px)';globalThis.geometry=[document.getElementById('box').offsetLeft,document.getElementById('box').getBoundingClientRect().x];document.getElementById('box').style.width='160px';globalThis.freshWidth=document.getElementById('box').offsetWidth");
        require(s.vm.eval("JSON.stringify(geometry)") == "[0,25]" && s.vm.eval("freshWidth") == "160",
                "offset excludes transforms / synchronous fresh geometry");
        s.vm.eval(
            "document.getElementById('box').style.cssText='width:100px;height:20px;overflow:auto';document.getElementById('box').innerHTML='<div style=\"width:100px;height:40px\"></div>'");
        bool scroll_rejected = false;
        try {
            s.flush_scene();
        } catch (const std::invalid_argument& e) {
            scroll_rejected = std::string(e.what()).find("scene scrolling") != std::string::npos;
        }
        require(scroll_rejected, "overflow auto silently omitted scrolling");
        s.vm.eval("document.getElementById('box').innerHTML='<div style=\"width:80px;height:10px\"></div>'");
        s.flush_scene();
        s.vm.eval(
            "document.getElementById('box').style.cssText='width:100px;height:20px;min-width:50%';document.getElementById('box').textContent=''");
        s.flush_scene();
        require(s.vm.eval("document.getElementById('box').offsetWidth") == "394",
                "percentage min-width uses containing width");
        // Same pinned, unmodified Anime package, with an independently valid
        // axis transform. The original throw case also remains unchanged/compared.
        s.vm.eval(scene_read(WEFT_SOURCE_DIR
                             "/third_party/gallery-runtime/source/animejs-4.3.6-3c7dc1c4/dist/bundles/anime.umd.js"));
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"arena\" style=\"position:relative;width:500px;height:240px;overflow:hidden\"><div id=\"ball\" style=\"position:absolute;left:200px;top:100px;width:40px;height:40px;background:#ff0000;transform:translateX(0px) translateY(0px)\"></div></div>';globalThis.drag=anime.createDraggable(document.getElementById('ball'),{container:document.getElementById('arena'),containerFriction:0.5,releaseEase:'outExpo',dragSpeed:0.5});globalThis.ballX=()=>document.getElementById('ball').getBoundingClientRect().x");
        s.step(5);
        const auto initial = std::stof(s.vm.eval("ballX()"));
        s.input_value(weft::MouseType::Down, 220, 120, 1);
        for (unsigned i = 1; i <= 5; ++i) {
            s.step(5 + i * weft::test_constants::scene::drag_frame_interval_ms);
            s.input_value(weft::MouseType::Move, 220 + i * 20.f, 120, 1);
        }
        s.step(105);
        const auto held = std::stof(s.vm.eval("ballX()"));
        s.input_value(weft::MouseType::Up, 320, 120);
        s.step(121.6666667);
        const auto release = std::stof(s.vm.eval("ballX()"));
        for (unsigned i = 1; i <= 90; ++i) {
            s.step(121.6666667 + i * weft::test_constants::scene::drag_frame_interval_ms);
        }
        const auto settled = std::stof(s.vm.eval("ballX()"));
        std::cout << "inertia positions " << initial << " / " << held << " / " << release << " / " << settled << '\n';
        require(held > initial + 10 && settled > release + 5 && settled <= 460.1f,
                "unmodified Anime held movement / release inertia / container bounds");
        s.vm.eval("drag.revert();globalThis.drag=null;globalThis.ballX=null");
        // The reference raster paints the shadow, and the native list carries it as a shadow item right under its
        // box. A white base makes a missing shadow visible, unlike the gallery's nearly black backdrop.
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"shadow\" style=\"position:absolute;left:40px;top:40px;width:40px;height:40px;background:#ff0000;border-radius:5px;box-shadow:0px 8px 12px 0px rgba(0,0,0,0.8)\"></div>';document.getElementById('root').style.backgroundColor='#ffffff'");
        s.step(1700);
        const auto with = s.bitmap(weft::test_constants::scene::gallery_raster_scale);
        {
            using Kind = weft::paint::Item::Kind;
            const auto native = weft::scene_display_list(s.current);
            const auto& items = native.list.items;
            const auto at = std::find_if(items.begin(), items.end(), [](const auto& item) {
                return item.kind == Kind::Shadow;
            });
            require(native.fallback.empty() && at != items.end() && at + 1 != items.end() &&
                        (at + 1)->kind == Kind::Box && native.list.shadows.size() == 1,
                    "shadow item under its box");
            const auto& shadow = native.list.shadows[at->payload];
            require(shadow.x == 0 && shadow.y == 8 && shadow.blur == 12 && shadow.spread == 0 &&
                        shadow.color == weft::paint::Color{0, 0, 0, .8f} &&
                        shadow.radii == std::array<float, 4>{5, 5, 5, 5} && at->width == 40 && at->height == 40,
                    "shadow item values");
        }
        s.vm.eval("document.getElementById('shadow').style.boxShadow='none'");
        s.step(1701);
        const auto without = s.bitmap(weft::test_constants::scene::gallery_raster_scale);
        require(with.rgba != without.rgba, "shadow emitted no pixels");
        // Differential mutation/reordering/detachment with actual shaping/layout.
        s.vm.eval(
            "document.getElementById('root').textContent='';globalThis.nodes=[];for(let i=0;i<24;++i){let n=document.createElement('div');n.style.cssText='position:absolute;left:'+i*10+'px;top:40px;width:12px;height:12px;background:#d4a843';nodes.push(n);document.getElementById('root').appendChild(n)}let seed=123;globalThis.mutate=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;let n=nodes[seed%nodes.length];switch(seed%4){case 0:n.style.transform='translateX('+seed%13+'px)';break;case 1:n.style.backgroundColor=seed%2?'#225577':'#ff7777';break;case 2:if(n.parentNode)n.remove();else document.getElementById('root').appendChild(n);break;case 3:document.getElementById('root').appendChild(n)}}");
        weft::SceneSolver full(scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/cluster-utilities.css"),
                               scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/cluster-keyframes.css"),
                               weft::testing::thread_scene_text(), false);
        weft::SceneRaster oracle{weft::testing::thread_scene_text()};
        for (unsigned i = 0; i < weft::test_constants::scene::cluster_mutation_iterations; ++i) {
            s.vm.eval("mutate()");
            s.step(1702 + i);
            const auto fresh = full.frame(s.vm.snapshot_subtree("#root"), s.width, s.height, s.time);
            require(s.bitmap().rgba == oracle.raster(fresh, weft::test_constants::scene::gallery_raster_scale).rgba,
                    "incremental/full mutation pixel mismatch");
        }
        const auto sparse = scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                            "tests/fixtures/gallery-cluster-reference/anime-05-24.png");
        auto blank = sparse;
        for (std::size_t i = 0; i < blank.rgba.size(); i += 4) {
            blank.rgba[i] = weft::test_constants::scene::backdrop_red;
            blank.rgba[i + 1] = weft::test_constants::scene::backdrop_green;
            blank.rgba[i + 2] = weft::test_constants::scene::backdrop_blue;
            blank.rgba[i + 3] = weft::constants::color_channel_max;
        }
        require(!cluster_image_match(blank, sparse), "sparse oracle accepted blank");
        auto displaced = blank;
        for (unsigned y = 0; y < sparse.height; ++y) {
            for (unsigned x = 8; x < sparse.width; ++x) {
                std::memcpy(displaced.rgba.data() + (std::size_t(y) * sparse.width + x) * 4,
                            sparse.rgba.data() + (std::size_t(y) * sparse.width + x - 8) * 4, 4);
            }
        }
        require(!cluster_image_match(displaced, sparse), "sparse oracle accepted displaced particles");
        s.unmount();
        require(s.vm.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                "shared platform owner teardown");
        std::cout << "PASS shared cluster platform / 64 full-path mutation comparisons / oracle negatives\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
