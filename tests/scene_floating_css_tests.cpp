#include "constants/common.hpp"
#include "paint/display_list.hpp"
#include "render/shadow_image.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_gradient.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_transform.hpp"
#include "scene/scene_values.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

// The CSS the CRPG floating UI needs of scene pages (13.4a), value by value: gradients of every radial shape, size and
// position and repeating ones, text-shadow, filter: drop-shadow(), inset box-shadow and its pixels, transform-origin
// keywords, translate3d(), em and rem lengths, calc() insets, text-overflow: ellipsis lines, the path shadows of svg
// icons and the lists that carry them (other filters are filtered images, tests/scene_filter_tests.cpp); and what each
// leaves outside the profile, diagnosed.
// tests/scene_paint_reference_tests.cpp holds the same features against Chrome's pixels.
namespace {
    using weft::testing::require;
    using Kind = weft::paint::Item::Kind;
    using weft::paint::GradientKind;

    bool throws(const std::function<void()>& f) {
        try {
            f();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

    bool about(float a, float b, float tolerance = 1e-4f) {
        return std::abs(a - b) <= tolerance;
    }

    weft::StyleValues style(std::initializer_list<std::pair<const char*, const char*>> values) {
        weft::StyleValues s;
        for (const auto& [k, v] : values) {
            s[k] = v;
        }
        return s;
    }

    void gradients() {
        constexpr float root2 = std::numbers::sqrt2_v<float>;
        const auto radial = [](const char* image, float w, float h) {
            return *weft::scene_gradient(style({{"background-image", image}}), w, h);
        };
        // The default shape is an ellipse through the farthest corner, of the farthest sides' ratio.
        for (const auto* image :
             {"radial-gradient(ellipse at center, #000 0%, #fff 72%)", "radial-gradient(#000 0%, #fff 72%)"}) {
            const auto g = radial(image, 200, 40);
            require(g.kind == GradientKind::Radial && !g.repeating && about(g.shape[0], 100) && about(g.shape[1], 20) &&
                        about(g.shape[2], 100 * root2) && about(g.shape[3], 20 * root2) && g.stops.size() == 2 &&
                        about(g.stops[1].offset, .72f),
                    std::string("an ellipse through the farthest corner: ") + image);
        }
        const auto circle = radial("radial-gradient(circle, #000, #fff)", 200, 40);
        require(about(circle.shape[2], std::hypot(100.f, 20.f)) && circle.shape[2] == circle.shape[3],
                "a circle through the farthest corner");
        const auto closest = radial("radial-gradient(circle closest-side at 30% 40%, #000, #fff)", 50, 40);
        require(about(closest.shape[0], 15) && about(closest.shape[1], 16) && about(closest.shape[2], 15) &&
                    about(closest.shape[3], 15),
                "a circle meeting the closest side, at a position");
        const auto farthest = radial("radial-gradient(ellipse farthest-side at left top, #000, #fff)", 50, 40);
        require(about(farthest.shape[0], 0) && about(farthest.shape[1], 0) && about(farthest.shape[2], 50) &&
                    about(farthest.shape[3], 40),
                "an ellipse meeting the farthest sides, at keywords");
        const auto corner = radial("radial-gradient(closest-corner circle at 10px 10px, #000, #fff)", 50, 40);
        require(about(corner.shape[2], std::hypot(10.f, 10.f)), "a circle through the closest corner");
        const auto oval = radial("radial-gradient(ellipse closest-corner at 10px 10px, #000, #fff)", 50, 40);
        require(about(oval.shape[2], 10 * root2) && about(oval.shape[3], 10 * root2),
                "an ellipse through the closest corner");
        const auto sized = radial("radial-gradient(30px 15px, #000 10px, #fff 30px)", 50, 40);
        require(about(sized.shape[2], 30) && about(sized.shape[3], 15) && about(sized.stops[0].offset, 10 / 30.f) &&
                    about(sized.stops[1].offset, 1),
                "an ellipse of lengths, its length stops along its horizontal radius");
        const auto right = radial("radial-gradient(circle 16px at right, #000 50%, #fff 51%)", 50, 40);
        require(about(right.shape[0], 50) && about(right.shape[1], 20) && about(right.shape[2], 16),
                "a circle of a length at one keyword");
        for (const auto* image :
             {"radial-gradient(circle 10px 20px, #000, #fff)", "radial-gradient(ellipse 10px, #000, #fff)",
              "radial-gradient(circle 50%, #000, #fff)", "radial-gradient(circle closest-side at 0 0, #000, #fff)",
              "radial-gradient(circle at top bottom, #000, #fff)", "radial-gradient(circle circle, #000, #fff)",
              "repeating-conic-gradient(#000, #fff)", "linear-gradient(to top left, #000, #fff)",
              "linear-gradient(#000 1px 2px 3px, #fff)"}) {
            require(throws([&] {
                        weft::scene_gradient(style({{"background-image", image}}), 50, 40);
                    }),
                    std::string("a gradient outside the profile: ") + image);
        }
        const auto stripes = *weft::scene_gradient(
            style({{"background-image", "repeating-linear-gradient(90deg, #f00 0 5px, #00f 5px 10px)"}}), 100, 30);
        require(stripes.repeating && stripes.stops.size() == 4 && about(stripes.stops[1].offset, .05f) &&
                    about(stripes.stops[3].offset, .1f),
                "repeating stops along the gradient line");
        // In the first stripe, the second, and the first again a span later.
        require(weft::scene_gradient_color(stripes, 2.5f, 15, 100, 30)[0] == 1 &&
                    weft::scene_gradient_color(stripes, 7.5f, 15, 100, 30)[2] == 1 &&
                    weft::scene_gradient_color(stripes, 12.5f, 15, 100, 30)[0] == 1 &&
                    weft::scene_gradient_color(stripes, 97.5f, 15, 100, 30)[2] == 1,
                "repeating stripes");
        const auto flat = *weft::scene_gradient(
            style({{"background-image", "repeating-linear-gradient(#f00 5px, #00f 5px)"}}), 100, 30);
        require(!flat.repeating && flat.stops.size() == 2 && flat.stops[0].color == flat.stops[1].color &&
                    flat.stops[0].color[2] == 1,
                "repeating stops that span nothing: the last stop's color");
        const auto rings = *weft::scene_gradient(
            style({{"background-image", "repeating-radial-gradient(circle 20px, #f00 0 5px, #00f 5px 10px)"}}), 40, 40);
        require(rings.repeating && weft::scene_gradient_color(rings, 20, 22.5f, 40, 40)[0] == 1 &&
                    weft::scene_gradient_color(rings, 20, 27.5f, 40, 40)[2] == 1 &&
                    weft::scene_gradient_color(rings, 20, 32.5f, 40, 40)[0] == 1,
                "repeating rings");
        require(about(radial("linear-gradient(to right, #000, #fff)", 10, 10).angle, 90) &&
                    about(radial("linear-gradient(to bottom, #000, #fff)", 10, 10).angle, 180),
                "a linear gradient towards a side");
        require(!weft::scene_gradient(style({{"background-image", "linear-gradient(#000, #fff)"}}), 0, 10) &&
                    !weft::scene_gradient(
                        style({{"background-image", "linear-gradient(#000, #fff)"}, {"background-size", "0px 10px"}}),
                        10, 10),
                "an empty tile paints nothing");
        const auto ellipse = radial("radial-gradient(#000, #fff)", 200, 40);
        require(weft::scene_gradient_color(ellipse, 100, 20, 200, 40)[0] == 0 &&
                    weft::scene_gradient_color(ellipse, 0, 0, 200, 40)[0] == 1 &&
                    about(weft::scene_gradient_color(ellipse, 100 + 50 * root2, 20, 200, 40)[0], .5f, 1e-3f) &&
                    about(weft::scene_gradient_color(ellipse, 100, 20 + 10 * root2, 200, 40)[0], .5f, 1e-3f),
                "an elliptical gradient line");
        // A pixel the box's edge crosses, its center on the edge or past it: the box's color at that edge, not the
        // next tile's (a half pixel at the right of the box drew the opaque start of a fade there, a dark line).
        const auto alpha = [](const weft::paint::Gradient& g, float x, float y, float w, float h) {
            return weft::scene_gradient_color(g, x, y, w, h)[3];
        };
        const auto fade = [&](const char* image, float w, float h, const char* size = "100% 100%") {
            return *weft::scene_gradient(style({{"background-image", image}, {"background-size", size}}), w, h);
        };
        const auto across = fade("linear-gradient(90deg, #000 0%, transparent 100%)", 100.5f, 30);
        require(about(alpha(across, 100.5f, 15, 100.5f, 30), 0, 1e-5f) &&
                    about(alpha(across, 100.8f, 15, 100.5f, 30), 0, 1e-5f) &&
                    about(alpha(across, -.3f, 15, 100.5f, 30), 1, 1e-5f) &&
                    about(alpha(across, 0, 15, 100.5f, 30), 1, 1e-5f) &&
                    about(alpha(across, 50.25f, 15, 100.5f, 30), .5f, 1e-5f),
                "a fade's ends at and past the box's edges");
        const auto down = fade("linear-gradient(180deg, #000 0%, transparent 100%)", 30, 40.5f);
        require(about(alpha(down, 15, 40.5f, 30, 40.5f), 0, 1e-5f) &&
                    about(alpha(down, 15, 40.9f, 30, 40.5f), 0, 1e-5f) &&
                    about(alpha(down, 15, -.2f, 30, 40.5f), 1, 1e-5f),
                "a vertical fade's ends at and past the box's edges");
        // Tiles inside the box still repeat: past the last whole tile, the box's sliver of the next one starts the fade
        // again; an edge on a tile's boundary ends the last tile.
        const auto tiled = fade("linear-gradient(90deg, #000 0%, transparent 100%)", 100.5f, 30, "25px 100%");
        require(about(alpha(tiled, 75, 15, 100.5f, 30), 1, 1e-5f) &&
                    about(alpha(tiled, 100.5f, 15, 100.5f, 30), .98f, 1e-5f) &&
                    about(alpha(tiled, 100.8f, 15, 100.5f, 30), .98f, 1e-5f),
                "the sliver of the next tile in the box");
        const auto whole = fade("linear-gradient(90deg, #000 0%, transparent 100%)", 100, 30, "25px 100%");
        require(about(alpha(whole, 100, 15, 100, 30), 0, 1e-5f) && about(alpha(whole, 100.3f, 15, 100, 30), 0, 1e-5f) &&
                    about(alpha(whole, 50, 15, 100, 30), 1, 1e-5f),
                "an edge on a tile's boundary ends the tile");
    }

    void shadows() {
        namespace values = weft::scene_values;
        const auto listed = values::text_shadows(
            style({{"color", "#00ff00"}, {"text-shadow", "0 1px 2px rgba(0,0,0,0.9), #f00 3px 4px, 5px 6px"}}));
        require(listed.size() == 3 && listed[0].color == weft::paint::Color{0, 1, 0, 1} && listed[0].x == 5 &&
                    listed[1].color == weft::paint::Color{1, 0, 0, 1} && listed[1].x == 3 && listed[1].y == 4 &&
                    listed[1].blur == 0 && about(listed[2].color[3], .9f) && listed[2].blur == 2,
                "text shadows bottom first, the color before or after, currentcolor without one");
        require(values::text_shadows(style({{"text-shadow", "1px 1px transparent"}})).empty() &&
                    values::text_shadows(style({{"text-shadow", "none"}})).empty(),
                "transparent text shadows and none draw nothing");
        for (const auto* value : {"1px", "1px 2px 3px 4px", "1px 2px -3px",
                                  "1px 1px,1px 1px,1px 1px,1px 1px,1px 1px,1px 1px,1px 1px,1px 1px,1px 1px"}) {
            require(throws([&] {
                        values::text_shadows(style({{"text-shadow", value}}));
                    }),
                    std::string("a text shadow outside the profile: ") + value);
        }
        const auto dropped = values::drop_shadow(style({{"filter", "drop-shadow(0 1px 0.5px rgba(0,0,0,0.95))"}}));
        require(dropped && dropped->x == 0 && dropped->y == 1 && dropped->sigma == .5f &&
                    about(dropped->color[3], .95f),
                "a drop shadow's offset and standard deviation");
        const auto hard = values::drop_shadow(style({{"color", "#0000ff"}, {"filter", "drop-shadow(2px 3px)"}}));
        require(hard && hard->sigma == 0 && hard->color == weft::paint::Color{0, 0, 1, 1},
                "a drop shadow of currentcolor without a blur");
        require(!values::drop_shadow(style({{"filter", "none"}})) &&
                    !values::drop_shadow(style({{"filter", "drop-shadow(1px 1px transparent)"}})),
                "filter: none and a transparent drop shadow");
        for (const auto* value : {"blur(2px)", "drop-shadow(1px 1px) drop-shadow(2px 2px)", "drop-shadow(1px)",
                                  "drop-shadow(1px 1px -2px)", "drop-shadow(1px 1px 1px 1px)", "brightness(2)"}) {
            require(throws([&] {
                        values::drop_shadow(style({{"filter", value}}));
                    }),
                    std::string("a filter outside the profile: ") + value);
        }
        const auto boxes =
            values::box_shadows(style({{"color", "#ffffff"},
                                       {"box-shadow", "inset 1px 2px 3px 4px #ff0000, #0000ff 5px 6px inset, 7px 8px"},
                                       {"border-top-width", "2px"},
                                       {"border-right-width", "2px"},
                                       {"border-bottom-width", "2px"},
                                       {"border-left-width", "2px"},
                                       {"border-top-left-radius", "6px"},
                                       {"border-top-right-radius", "6px"},
                                       {"border-bottom-right-radius", "6px"},
                                       {"border-bottom-left-radius", "6px"}}),
                                40, 20, {6, 6, 6, 6});
        require(boxes.size() == 3 && !boxes[0].shadow.inset &&
                    boxes[0].shadow.color == weft::paint::Color{1, 1, 1, 1} &&
                    boxes[0].rect == std::array<float, 4>{0, 0, 40, 20} && boxes[1].shadow.inset &&
                    boxes[1].shadow.color == weft::paint::Color{0, 0, 1, 1} && boxes[2].shadow.inset &&
                    boxes[2].shadow.spread == 4 && boxes[2].rect == std::array<float, 4>{2, 2, 36, 16} &&
                    boxes[2].shadow.radii == std::array<float, 4>{4, 4, 4, 4},
                "box shadows: inset first or last, the color before or after, of the padding box");
        require(throws([&] {
                    values::box_shadows(style({{"box-shadow", "inset 1px"}}), 40, 20, {});
                }),
                "an inset shadow of one length");
        // Inset pixels: inside the box, outside its hole (the box less the spread, offset).
        weft::paint::Shadow inset;
        inset.inset = true;
        inset.color = {0, 0, 0, 1};
        inset.x = 3;
        inset.spread = 2;
        const auto image = weft::render::shadow_image(inset, 20, 10, 1).value();
        const auto alpha = [&](float x, float y) {
            const auto& g = image.geometry;
            const auto col = unsigned((x - g.rect[0]) * 1);
            const auto row = unsigned((y - g.rect[1]) * 1);
            return image.bgra[(std::size_t(row) * g.width + col) * 4 + 3];
        };
        require(alpha(4.5f, 5.5f) == 255 && alpha(10.5f, 1.5f) == 255 && alpha(10.5f, 8.5f) == 255 &&
                    alpha(10.5f, 5.5f) == 0 && alpha(19.5f, 5.5f) == 0 && alpha(-.5f, 5.5f) == 0 &&
                    alpha(20.5f, 5.5f) == 0,
                "an inset shadow inside the box, outside its offset hole");
        weft::paint::Shadow round;
        round.inset = true;
        round.color = {0, 0, 0, 1};
        round.spread = 6;
        round.radii = {10, 10, 10, 10};
        const auto ring = weft::render::shadow_image(round, 40, 30, 1).value();
        const auto ring_alpha = [&](float x, float y) {
            const auto& g = ring.geometry;
            return ring.bgra[(std::size_t(y - g.rect[1]) * g.width + std::size_t(x - g.rect[0])) * 4 + 3];
        };
        // A pixel a pixel inside the hole's rounded corner (radius 10 - 6), outside a corner of radius 10.
        require(ring_alpha(7.5f, 8.5f) == 0 && ring_alpha(3.5f, 15.5f) == 255,
                "an inset shadow's hole takes the box's radii less the spread");
        inset.blur = 4;
        const auto soft = weft::render::shadow_image(inset, 20, 10, 2).value();
        require(soft.geometry.rect[0] < 0 && soft.geometry.rect[2] > 20 &&
                    soft.geometry.width == unsigned(std::ceil(20 * 2 + 2 * 2)),
                "an inset shadow's image covers the box and its antialiased edge");
    }

    void transforms() {
        const auto origin = [](const char* value) {
            return weft::scene_transform_origin(style({{"transform-origin", value}}), 40, 20);
        };
        require(
            origin("left top") == std::array<float, 2>{0, 0} && origin("bottom") == std::array<float, 2>{20, 20} &&
                origin("right center") == std::array<float, 2>{40, 10} &&
                origin("center left") == std::array<float, 2>{0, 10} &&
                origin("10px bottom") == std::array<float, 2>{10, 20} &&
                origin("top right") == std::array<float, 2>{40, 0} && origin("top") == std::array<float, 2>{20, 0} &&
                origin("50% 50% 0") == std::array<float, 2>{20, 10} && origin("25% 3px") == std::array<float, 2>{10, 3},
            "transform-origin keywords in either order, lengths and a zero depth");
        for (const auto* value : {"top bottom", "left right", "bottom 10px", "1px 2px 3px", "a b", "1px 2px 0 4px"}) {
            require(throws([&] {
                        origin(value);
                    }),
                    std::string("a transform-origin outside the profile: ") + value);
        }
        const auto moved = weft::parse_scene_transform("translate3d(1px, 2px, 0) translateZ(0px)").value();
        require(moved.size() == 2 && moved[0].kind == weft::PaintOp::Translate && moved[0].count == 2 &&
                    moved[0].args[0].absolute == 1 && moved[0].args[1].absolute == 2 &&
                    moved[1].kind == weft::PaintOp::Translate && moved[1].args[0].absolute == 0,
                "translate3d() and translateZ() without depth translate the plane");
        for (const auto* value : {"translate3d(1px, 2px, 3px)", "translateZ(1px)", "translate3d(1px, 2px)"}) {
            require(!weft::parse_scene_transform(value), std::string("a 3D transform outside the profile: ") + value);
        }
    }

    // A scene of a parent div and the nodes add makes inside it, laid out at 200 x 100.
    struct Page {
        weft::testing::NativeScene s;
        weft::dom::Node* parent{};

        explicit Page(const std::string& parent_style) {
            s.set(*s.root, "style", "position:relative;width:200px;height:100px;font-family:Arial;font-size:10px");
            parent = &s.html("div");
            s.set(*parent, "style", parent_style);
            s.tree.insert(*s.root, *parent, nullptr);
        }

        weft::dom::Node& add(weft::dom::Node& to, const std::string& tag, const std::string& css,
                             const std::string& text = {}) {
            auto& e = s.html(tag);
            s.set(e, "style", css);
            s.tree.insert(to, e, nullptr);
            if (!text.empty()) {
                auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
                t.data = text;
                s.tree.insert(e, t, nullptr);
            }
            return e;
        }

        weft::SceneFrame frame() {
            weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
            return solver.frame(s.tree.snapshot(*s.root), 200, 100, 0);
        }
    };

    void lengths() {
        {
            Page p("position:relative;width:100px;height:40px");
            p.add(*p.parent, "div",
                  "position:absolute;font-size:2em;width:3em;height:1rem;letter-spacing:.1em;left:calc(100% + "
                  "0.5em);top:calc(50% - 4px)");
            p.add(*p.parent, "div", "position:absolute;font-size:150%;width:2em;height:2px;top:0");
            p.add(*p.parent, "div",
                  "position:absolute;right:calc(25% + 2px);bottom:calc(0% - 8px);margin-right:3px;width:12px;height:"
                  "12px");
            const auto frame = p.frame();
            const auto& parent = frame.root.children.at(0);
            const auto& a = parent.children.at(0);
            const auto& b = parent.children.at(1);
            const auto& c = parent.children.at(2);
            require(a.style.text(weft::PropertyId::FontSize) == "20px" && a.box[2] == 60 && a.box[3] == 16 &&
                        a.style.text(weft::PropertyId::LetterSpacing) == "2px",
                    "em lengths of the parent's font size for font-size, of the element's own otherwise; rem of 16px");
            require(a.box[0] == 110 && a.box[1] == 16, "calc() insets of a percentage and a length");
            require(b.style.text(weft::PropertyId::FontSize) == "15px" && b.box[2] == 30,
                    "a percentage font size of the parent's");
            require(c.box[0] == 100 - 25 - 2 - 3 - 12 && c.box[1] == 40 + 8 - 12,
                    "a calc() inset beside a margin of its side");
        }
        for (const auto* css :
             {"position:relative;left:calc(50% + 2px)", "position:absolute;left:calc(50% + 2px);margin-left:auto",
              "position:absolute;left:calc(50% + 2px);margin-left:10%", "position:absolute;left:calc(50% * 2)"}) {
            Page p("position:relative;width:100px;height:40px");
            p.add(*p.parent, "div", std::string(css) + ";width:10px;height:10px");
            require(throws([&] {
                        p.frame();
                    }),
                    std::string("a calc() inset outside the profile: ") + css);
        }
    }

    void ellipsis() {
        auto& text = weft::testing::thread_scene_text();
        const auto cut = [](const std::string& css, const std::string& content) {
            Page p("position:absolute;left:0;top:0;" + css);
            auto& t = p.s.tree.create(weft::constants::text_node_type, "#text", "", p.s.document->identity);
            t.data = content;
            p.s.tree.insert(*p.parent, t, nullptr);
            return p.frame().root.children.at(0);
        };
        const std::string nowrap =
            "width:60px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:16px";
        const auto name = cut(nowrap, "Nameplate of a long name");
        require(name.lines.size() == 2 && name.lines[1].text == "\xE2\x80\xA6" &&
                    name.lines[0].text.size() < std::string("Nameplate of a long name").size() &&
                    std::string("Nameplate of a long name").starts_with(name.lines[0].text),
                "an overflowing line: the start of its text, then an ellipsis line");
        const auto font = weft::scene_font(name.style);
        const auto width = [&](const std::string& s) {
            return s.empty() ? 0.f : text.shape(s, font)->metrics.width;
        };
        const auto ellipsis = width("\xE2\x80\xA6");
        const auto& kept = name.lines[0].text;
        const auto next = std::string("Nameplate of a long name").substr(0, kept.size() + 1);
        require(width(kept) + ellipsis <= 60 + 1.f / 32 && width(next) + ellipsis > 60,
                "the most characters that fit before the ellipsis");
        require(about(name.lines[1].rect[0], name.lines[0].rect[0] + width(kept), .05f) &&
                    name.lines[1].rect[0] + ellipsis <= 60 + 1.f / 64,
                "the ellipsis right after them, inside the box");
        require(name.lines[0].rect[2] > 60, "the line keeps the box it was laid out in");
        require(cut(nowrap, "Short").lines.size() == 1, "a line that fits has no ellipsis");
        require(cut("width:60px;white-space:nowrap;overflow:hidden;font-size:16px", "Nameplate of a long name")
                        .lines.size() == 1,
                "text-overflow: clip cuts nothing");
        require(cut("width:60px;white-space:nowrap;text-overflow:ellipsis;font-size:16px", "Nameplate of a long name")
                        .lines.size() == 1,
                "a box that does not clip cuts nothing");
        require(cut("width:60px;display:flex;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:16px",
                    "Nameplate of a long name")
                        .lines.size() == 1,
                "the text of a flex container is never cut");
        const auto narrow =
            cut("width:6px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:16px", "Wide");
        require(narrow.lines.size() == 2 && narrow.lines[0].text == "W" && narrow.lines[1].text == "\xE2\x80\xA6",
                "the first character is clipped rather than ellipsed");
        {
            // Inline children: a span the cut passes hides its text but keeps its place; the ellipsis is the
            // block's.
            Page p("position:absolute;left:0;top:0;" + nowrap + ";color:#0000ff");
            auto& t = p.s.tree.create(weft::constants::text_node_type, "#text", "", p.s.document->identity);
            t.data = "Mixed ";
            p.s.tree.insert(*p.parent, t, nullptr);
            p.add(*p.parent, "span", "color:#ff0000", "colored spans");
            p.add(*p.parent, "span", "color:#00ff00", " and more");
            const auto frame = p.frame();
            const auto& block = frame.root.children.at(0);
            const auto& last = block.children.at(2);
            require(block.lines.size() == 1 && block.lines[0].text == "\xE2\x80\xA6" &&
                        block.style.text(weft::PropertyId::Color) == "#0000ff",
                    "the ellipsis is a line of the block, in its color");
            const auto& hidden = last.children.empty() ? last : last.children.at(0);
            require(!hidden.lines.empty() && hidden.lines[0].text.empty() && hidden.lines[0].rect[2] > 0,
                    "text past the ellipsis draws nothing but keeps its place");
        }
        require(throws([&] {
                    cut("width:60px;overflow:hidden;text-overflow:fade", "Text");
                }),
                "text-overflow other than clip and ellipsis");
    }

    // An svg icon with a drop shadow: a stroked path and a stroked circle (a box).
    weft::SceneFrame icon(const std::string& svg_css, const std::string& path_css = {}) {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:100px;height:100px;color:#ffffff");
        auto& svg = s.svg("svg");
        for (const auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{{"width", "24"},
                                                                                             {"height", "24"},
                                                                                             {"viewBox", "0 0 24 24"},
                                                                                             {"fill", "none"},
                                                                                             {"stroke", "#e9ddc1"},
                                                                                             {"stroke-width", "2"}}) {
            s.set(svg, k, v);
        }
        s.set(svg, "style", svg_css);
        s.tree.insert(*s.root, svg, nullptr);
        auto& path = s.svg("path");
        s.set(path, "d", "M12 6v16");
        if (!path_css.empty()) {
            s.set(path, "style", path_css);
        }
        s.tree.insert(svg, path, nullptr);
        auto& circle = s.svg("circle");
        s.set(circle, "cx", "12");
        s.set(circle, "cy", "4");
        s.set(circle, "r", "2");
        s.tree.insert(svg, circle, nullptr);
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        return solver.frame(s.tree.snapshot(*s.root), 100, 100, 0);
    }

    void path_shadows() {
        const auto frame = icon("position:absolute;left:10px;top:20px;filter:drop-shadow(1px 2px 1.5px #102030)");
        const auto plan = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), 2);
        const auto& items = plan.list.items;
        std::size_t at = items.size();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Kind::PathShadow) {
                at = i;
            }
        }
        require(plan.fallback.empty() && at < items.size(), "an svg element's drop shadow is listed");
        const auto& s = plan.list.path_shadows[items[at].payload];
        const float e = std::ceil(3 * 1.5f);
        require(s.first == at + 1 && s.count == items.size() - at - 1 && s.count == 2 &&
                    items[at + 1].kind == Kind::Path && items[at + 2].kind == Kind::Box && s.x == 1 && s.y == 2 &&
                    s.blur == 1.5f && s.content == std::array<float, 4>{e, e, 24, 24} &&
                    s.color == weft::paint::Color{16 / 255.f, 32 / 255.f, 48 / 255.f, 1},
                "under the paths and shapes it shadows, which follow it");
        require(items[at].width == 24 + 2 * e && items[at].height == 24 + 2 * e &&
                    about(items[at].transform[4], 10 + 1 - e) && about(items[at].transform[5], 20 + 2 - e),
                "its box: the element's, offset and grown by the blur");
        require(bool(weft::paint::check(plan.list, 8)), "a well formed list");
        // A drop shadow of an svg element that paints a box, and one of an HTML element, are filtered images
        // (tests/scene_filter_tests.cpp); a filter on svg content falls back, saying why.
        const auto listed = [](const weft::SceneFrame& f) {
            return weft::scene_display_list(f, {}, &weft::testing::thread_scene_text(), 2);
        };
        const auto boxed = listed(icon("position:absolute;background-color:#ff0000;filter:drop-shadow(1px 2px)"));
        require(boxed.fallback.empty() && boxed.list.filters.size() == 1 && boxed.list.path_shadows.empty(),
                "a drop shadow of an svg element that paints a box: a filtered image");
        require(listed(icon("position:absolute", "filter:drop-shadow(1px 2px)")).fallback.find("svg content") !=
                    std::string::npos,
                "a filter on svg content");
        {
            weft::testing::NativeScene page;
            page.set(*page.root, "style", "position:relative;width:100px;height:100px");
            auto& div = page.html("div");
            page.set(div, "style", "width:10px;height:10px;background-color:#ff0000;filter:drop-shadow(1px 2px)");
            page.tree.insert(*page.root, div, nullptr);
            weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
            const auto html = listed(solver.frame(page.tree.snapshot(*page.root), 100, 100, 0));
            require(html.fallback.empty() && html.list.filters.size() == 1 &&
                        html.list.filters[0].ops.at(0).kind == weft::paint::FilterOp::Kind::DropShadow,
                    "a filter on an HTML element: a filtered image");
        }
        // A list's path shadow covers the paths that follow it, no other items; values are finite.
        auto list = plan.list;
        list.path_shadows[items[at].payload].first = std::uint32_t(at);
        require(!weft::paint::check(list, 8), "a path shadow over itself");
        list = plan.list;
        list.path_shadows[items[at].payload].count = std::uint32_t(items.size());
        require(!weft::paint::check(list, 8), "a path shadow past the list");
        list = plan.list;
        list.path_shadows[items[at].payload].blur = -1;
        require(!weft::paint::check(list, 8), "a path shadow of a negative blur");
        list = plan.list;
        list.items[at + 1].kind = Kind::Text;
        list.items[at + 1].payload = 0;
        list.rasters.push_back({std::make_shared<weft::SceneNode>()});
        require(!weft::paint::check(list, 8), "a path shadow over text");
        list = plan.list;
        list.rasters.push_back({std::make_shared<weft::SceneNode>(), false, {}, 0, 0, 1});
        list.items.push_back(items.front());
        list.items.back().kind = Kind::Text;
        list.items.back().payload = std::uint32_t(list.rasters.size() - 1);
        require(!weft::paint::check(list, 8), "a blurred raster that is no mask");
    }

    void text_shadow_items() {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:200px;height:100px;font-family:Arial");
        auto& div = s.html("div");
        s.set(div, "style",
              "position:absolute;left:5px;top:6px;font-size:20px;color:#ffffff;text-shadow:0 1px 2px "
              "rgba(0,0,0,0.9), #ff0000 3px 4px");
        s.tree.insert(*s.root, div, nullptr);
        auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
        t.data = "Text";
        s.tree.insert(div, t, nullptr);
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        const auto plan = weft::scene_display_list(solver.frame(s.tree.snapshot(*s.root), 200, 100, 0), {},
                                                   &weft::testing::thread_scene_text(), 1);
        std::vector<std::size_t> texts;
        for (std::size_t i = 0; i < plan.list.items.size(); ++i) {
            if (plan.list.items[i].kind == Kind::Text) {
                texts.push_back(i);
            }
        }
        require(plan.fallback.empty() && texts.size() == 3, "two shadows under the line");
        const auto& items = plan.list.items;
        const auto& red = plan.list.rasters[items[texts[0]].payload];
        const auto& black = plan.list.rasters[items[texts[1]].payload];
        const auto& line = plan.list.rasters[items[texts[2]].payload];
        require(red.mask && red.blur == 0 && red.tint == weft::paint::Color{1, 0, 0, 1} && black.mask &&
                    black.blur == 1 && about(black.tint[3], .9f) && black.overhang == line.overhang + 3 &&
                    red.source == black.source && black.source == line.source,
                "the last listed lowest: masks of the line's coverage, tinted, a blur of half the radius");
        require(about(items[texts[0]].transform[4], items[texts[2]].transform[4] + 3) &&
                    about(items[texts[0]].transform[5], items[texts[2]].transform[5] + 4) &&
                    about(items[texts[1]].transform[5], items[texts[2]].transform[5] + 1),
                "each offset from the line");
        require(bool(weft::paint::check(plan.list, 8)), "a well formed list of text shadows");
    }
} // namespace

int main() {
    try {
        gradients();
        shadows();
        transforms();
        lengths();
        ellipsis();
        path_shadows();
        text_shadow_items();
        std::cout << "PASS scene floating CSS: gradients, shadows, transforms, lengths, ellipsis, path shadows\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
