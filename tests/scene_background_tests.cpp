#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_border_image.hpp"
#include "scene/scene_display_list.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

// A host texture's background layout (CSS Backgrounds 3 for an image whose natural size only the renderer knows,
// scene_background.hpp): background-origin, -clip, -size, -position, -repeat and -attachment parsed as CSS does, the
// background shorthand's one layer, the tiles laid out (sizes, positions, repeat, space and round), the diagnostics of
// what stays outside the profile, the display list's fit, and the native D3D11 pass (WARP) drawing the tiles as the
// reference raster lays them out on the CPU; the same layouts of a url() image (its translucent texels linear floats
// in the pass), which lays out as a host texture.
namespace {
    using weft::testing::require;
    using Fit = weft::paint::TextureFit;
    using Repeat = Fit::Repeat;

    bool about(float a, float b) {
        return std::abs(a - b) <= 1e-4f * std::max(1.f, std::abs(b));
    }

    template <class F>
    std::pair<weft::Diag, std::string> diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return {e.code(), e.what()};
        }
        return {weft::Diag{}, {}};
    }

    weft::StyleValues style(std::initializer_list<std::pair<weft::PropertyId, const char*>> values) {
        weft::StyleValues s;
        for (const auto& [id, v] : values) {
            s.set(id, v);
        }
        return s;
    }

    weft::BackgroundLayout layout(std::initializer_list<std::pair<weft::PropertyId, const char*>> values) {
        return weft::scene_background_layout(style(values));
    }

    bool rejects(std::initializer_list<std::pair<weft::PropertyId, const char*>> values) {
        try {
            (void)layout(values);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }

    // A fit in an area of x, y, w, h: the given sizes, position and repeats.
    Fit fit(std::array<float, 4> area, std::array<float, 2> size, std::array<float, 2> fraction = {},
            std::array<float, 2> offset = {}, std::array<Repeat, 2> repeat = {}, Fit::Scale scale = {}) {
        Fit f;
        f.area = area;
        f.size = size;
        f.fraction = fraction;
        f.offset = offset;
        f.repeat = repeat;
        f.scale = scale;
        return f;
    }

    void parses() {
        using P = weft::PropertyId;
        // The initial values: the padding box positions, the border box paints, natural size, top left, repeated.
        const auto initial = layout({});
        require(initial.origin == weft::BackgroundBox::Padding && initial.clip == weft::BackgroundBox::Border &&
                    initial.scale == Fit::Scale::Lengths && !initial.size[0] && !initial.size[1] &&
                    initial.position[0].share == 0 && initial.position[0].px == 0 &&
                    initial.repeat == std::array{Repeat::Repeat, Repeat::Repeat},
                "the initial layout");
        // background-size: one value sets the width, the height auto; cover and contain alone; percentages.
        const auto one = layout({{P::BackgroundSize, "50%"}});
        require(one.size[0] && about(one.size[0]->share, .5f) && !one.size[1], "a size of one value");
        const auto two = layout({{P::BackgroundSize, "auto 12px"}});
        require(!two.size[0] && two.size[1] && about(two.size[1]->px, 12), "auto and a length");
        require(layout({{P::BackgroundSize, "cover"}}).scale == Fit::Scale::Cover &&
                    layout({{P::BackgroundSize, "contain"}}).scale == Fit::Scale::Contain,
                "cover, contain");
        require(rejects({{P::BackgroundSize, "-4px"}}) && rejects({{P::BackgroundSize, "cover 10px"}}) &&
                    rejects({{P::BackgroundSize, "1px 2px 3px"}}),
                "sizes CSS does not take");
        // background-position: one value centers the other axis; keywords in either order; edges with offsets.
        const auto at = [&](const char* v) {
            return layout({{P::BackgroundPosition, v}}).position;
        };
        const auto is = [](const std::array<weft::BackgroundLength, 2>& p, float sx, float px, float sy, float py) {
            return about(p[0].share, sx) && about(p[0].px, px) && about(p[1].share, sy) && about(p[1].px, py);
        };
        require(is(at("center"), .5f, 0, .5f, 0) && is(at("top"), .5f, 0, 0, 0) && is(at("right"), 1, 0, .5f, 0) &&
                    is(at("10px"), 0, 10, .5f, 0),
                "one value");
        require(is(at("top left"), 0, 0, 0, 0) && is(at("left top"), 0, 0, 0, 0) &&
                    is(at("bottom center"), .5f, 0, 1, 0) && is(at("25% 75%"), .25f, 0, .75f, 0) &&
                    is(at("left 10px"), 0, 0, 0, 10) && is(at("calc(50% + 4px) 0"), .5f, 4, 0, 0),
                "two values");
        require(is(at("right 10px bottom 20%"), 1, -10, .8f, 0) && is(at("right 10px top"), 1, -10, 0, 0) &&
                    is(at("left bottom 4px"), 0, 0, 1, -4) && is(at("center top 6px"), .5f, 0, 0, 6),
                "edge offsets");
        require(rejects({{P::BackgroundPosition, "top 10px"}}) && rejects({{P::BackgroundPosition, "10px left"}}) &&
                    rejects({{P::BackgroundPosition, "left right"}}) &&
                    rejects({{P::BackgroundPosition, "center 4px top"}}) &&
                    rejects({{P::BackgroundPosition, "1px 2px 3px"}}) && rejects({{P::BackgroundPosition, "1em 2px"}}),
                "positions CSS does not take");
        // background-repeat: repeat-x and repeat-y, one value for both axes, two for each.
        require(layout({{P::BackgroundRepeat, "repeat-x"}}).repeat == std::array{Repeat::Repeat, Repeat::NoRepeat} &&
                    layout({{P::BackgroundRepeat, "repeat-y"}}).repeat ==
                        std::array{Repeat::NoRepeat, Repeat::Repeat} &&
                    layout({{P::BackgroundRepeat, "space"}}).repeat == std::array{Repeat::Space, Repeat::Space} &&
                    layout({{P::BackgroundRepeat, "round no-repeat"}}).repeat ==
                        std::array{Repeat::Round, Repeat::NoRepeat},
                "repeats");
        require(rejects({{P::BackgroundRepeat, "repeat-x repeat"}}) && rejects({{P::BackgroundRepeat, "mirror"}}),
                "repeats CSS does not take");
        // Boxes, and what stays outside the profile.
        const auto boxes = layout({{P::BackgroundOrigin, "content-box"}, {P::BackgroundClip, "padding-box"}});
        require(boxes.origin == weft::BackgroundBox::Content && boxes.clip == weft::BackgroundBox::Padding, "boxes");
        require(rejects({{P::BackgroundClip, "text"}}) && rejects({{P::BackgroundAttachment, "fixed"}}) &&
                    rejects({{P::BackgroundAttachment, "local"}}),
                "clip text, fixed and local attachments");
        // The boxes of an element of 100 x 60 with borders 2 4 6 8, padding 1 2 3 4 and corners of 10.
        const auto s = style({{P::BorderTopWidth, "2px"},
                              {P::BorderRightWidth, "4px"},
                              {P::BorderBottomWidth, "6px"},
                              {P::BorderLeftWidth, "8px"},
                              {P::PaddingTop, "1px"},
                              {P::PaddingRight, "2px"},
                              {P::PaddingBottom, "3px"},
                              {P::PaddingLeft, "4px"},
                              {P::BorderTopLeftRadius, "10px"},
                              {P::BorderTopRightRadius, "10px"},
                              {P::BorderBottomRightRadius, "10px"},
                              {P::BorderBottomLeftRadius, "10px"}});
        const auto border = weft::scene_background_box(s, weft::BackgroundBox::Border, 100, 60);
        const auto padding = weft::scene_background_box(s, weft::BackgroundBox::Padding, 100, 60);
        const auto content = weft::scene_background_box(s, weft::BackgroundBox::Content, 100, 60);
        require(border.rect == std::array<float, 4>{0, 0, 100, 60} && border.radius_x[0] == 10 &&
                    padding.rect == std::array<float, 4>{8, 2, 88, 52} && padding.radius_x[0] == 2 &&
                    padding.radius_y[0] == 8 && content.rect == std::array<float, 4>{12, 3, 82, 48} &&
                    content.radius_x == std::array<float, 4>{0, 4, 4, 0} &&
                    content.radius_y == std::array<float, 4>{0, 7, 1, 0},
                "border, padding and content boxes and their corners");
        bool percent{};
        try {
            (void)weft::scene_background_box(style({{P::PaddingTop, "10%"}}), weft::BackgroundBox::Content, 100, 60);
        } catch (const std::invalid_argument&) {
            percent = true;
        }
        require(percent, "a content box of a percentage padding is outside the profile");
        // The fit: the positioning area from the painting area's origin, sizes of its percentages.
        auto l = layout({{P::BackgroundSize, "50% 10px"}, {P::BackgroundPosition, "right 3px top 25%"}});
        l.origin = weft::BackgroundBox::Content;
        l.clip = weft::BackgroundBox::Padding;
        const auto f = weft::scene_texture_fit(l, padding, content);
        require(f.area == std::array<float, 4>{4, 1, 82, 48} && about(f.size[0], 41) && about(f.size[1], 10) &&
                    about(f.fraction[0], 1) && about(f.offset[0], -3) && about(f.fraction[1], .25f) &&
                    about(f.offset[1], 0),
                "the fit of a content-box origin over a padding-box clip");
    }

    void tiles() {
        // A texture of 16 x 8 in an area of 100 x 50 at 10, 20.
        const std::array<float, 4> area{10, 20, 100, 50};
        const auto t = [&](const Fit& f) {
            return weft::scene_texture_tiles(f, 16, 8);
        };
        const auto at = [](const weft::TextureTiles& a, float tile, float start, float period) {
            return about(a.tile, tile) && about(a.start, start) && about(a.period, period);
        };
        auto natural = t(fit(area, {-1, -1}));
        require(at(natural[0], 16, 10, 16) && at(natural[1], 8, 20, 8), "the natural size, repeated from the area");
        auto cover = t(fit(area, {-1, -1}, {.5f, .5f}, {}, {Repeat::NoRepeat, Repeat::NoRepeat}, Fit::Scale::Cover));
        require(at(cover[0], 100, 10, 0) && at(cover[1], 50, 20, 0), "cover");
        auto contain = t(
            fit({0, 0, 100, 80}, {-1, -1}, {.5f, .5f}, {}, {Repeat::NoRepeat, Repeat::NoRepeat}, Fit::Scale::Contain));
        require(at(contain[0], 100, 0, 0) && at(contain[1], 50, 15, 0), "contain, centered");
        auto width = t(fit(area, {50, -1}));
        require(at(width[0], 50, 10, 50) && at(width[1], 25, 20, 25), "a width and auto: the texture's ratio");
        auto height = t(fit(area, {-1, 4}));
        require(at(height[0], 8, 10, 8) && at(height[1], 4, 20, 4), "auto and a height");
        auto placed = t(fit(area, {-1, -1}, {1, 1}, {-2, 3}, {Repeat::NoRepeat, Repeat::NoRepeat}));
        require(at(placed[0], 16, 10 + 84 - 2, 0) && at(placed[1], 8, 20 + 42 + 3, 0), "placed at the far edges");
        // round: 100 / 30 rounds to 3 tiles of 33.3; the other axis keeps the ratio when its size is auto, and its
        // own size otherwise.
        auto round = t(fit(area, {30, 5}, {}, {}, {Repeat::Round, Repeat::Round}));
        require(at(round[0], 100.f / 3, 10, 100.f / 3) && at(round[1], 50.f / 10, 20, 50.f / 10), "round");
        auto ratio = t(fit(area, {-1, -1}, {}, {}, {Repeat::Round, Repeat::Repeat}));
        require(at(ratio[0], 100.f / 6, 10, 100.f / 6) && at(ratio[1], 8 * (100.f / 6) / 16, 20, 8 * (100.f / 6) / 16),
                "round on one axis, the other auto keeps the ratio");
        auto kept = t(fit(area, {-1, 8}, {}, {}, {Repeat::Round, Repeat::Repeat}));
        require(at(kept[1], 8, 20, 8), "round on one axis, the other's size kept");
        // space: whole tiles spread, the first and last at the edges; one placed by the position when two do not fit.
        auto spread = t(fit(area, {30, 30}, {.5f, .5f}, {}, {Repeat::Space, Repeat::Space}));
        require(at(spread[0], 30, 10, 35) && at(spread[1], 30, 20 + 10, 0), "space");
        // A tile of no size draws nothing.
        auto none = t(fit(area, {0, 10}));
        require(none[0].tile == 0 && !weft::scene_texture_point(none[0], 12).inside, "a tile of no size");
        // Points: inside a tile and where in it; between spaced tiles; before and after a single one.
        const auto p = weft::scene_texture_point(spread[0], 10 + 35 + 6);
        require(p.inside && about(p.u, 6.f / 30), "a point in the second spaced tile");
        require(!weft::scene_texture_point(spread[0], 10 + 31).inside, "a point between spaced tiles");
        require(!weft::scene_texture_point(placed[0], 10).inside && weft::scene_texture_point(placed[0], 93).inside &&
                    !weft::scene_texture_point(placed[0], 108.5f).inside,
                "around a single tile");
        const auto wrapped = weft::scene_texture_point(natural[0], 10 - 4);
        require(wrapped.inside && about(wrapped.u, 12.f / 16), "a repeated tile before the area's start");
    }

    // The solver: the background shorthand's one layer in the longhands, and diagnostics.
    void solver() {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:200px;height:100px");
        auto& e = s.html("div");
        s.set(e, "style",
              "position:absolute;left:10px;top:10px;width:80px;height:60px;border:2px solid #ffffff;"
              "background:#204060 -weft-texture(map) right 4px bottom / 50% auto repeat-x padding-box content-box");
        s.tree.insert(*s.root, e, nullptr);
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 100, 0);
        const auto& st = frame.root.children[0].style;
        using P = weft::PropertyId;
        require(st.text(P::BackgroundColor) == "#204060" && st.text(P::BackgroundImage) == "-weft-texture(map)" &&
                    st.text(P::BackgroundPosition) == "right 4px bottom" && st.text(P::BackgroundSize) == "50% auto" &&
                    st.text(P::BackgroundRepeat) == "repeat-x" && st.text(P::BackgroundOrigin) == "padding-box" &&
                    st.text(P::BackgroundClip) == "content-box" && st.text(P::BackgroundAttachment) == "scroll",
                "the shorthand's parts in the longhands");
        // One box sets both background-origin and background-clip.
        s.set(e, "style",
              "position:absolute;left:10px;top:10px;width:80px;height:60px;background:-weft-texture(map) content-box");
        const auto one_box = solver.frame(s.tree.snapshot(*s.root), 200, 100, 8);
        const auto& ob = one_box.root.children[0].style;
        require(ob.text(P::BackgroundOrigin) == "content-box" && ob.text(P::BackgroundClip) == "content-box",
                "one box for both");
        // A keyframe's shorthand sets the longhands too (its '/' with or without spaces): the size animates between its
        // keyframes.
        {
            weft::testing::NativeScene k;
            k.set(*k.root, "style", "position:relative;width:200px;height:100px");
            auto& shown = k.html("div");
            k.set(shown, "style",
                  "position:absolute;left:10px;top:10px;width:80px;height:60px;animation:grow 1s linear");
            k.tree.insert(*k.root, shown, nullptr);
            weft::SceneSolver animated("",
                                       "@keyframes grow{from{background:-weft-texture(map) center/10px 10px "
                                       "no-repeat}to{background:-weft-texture(map) center / 30px 30px no-repeat}}",
                                       weft::testing::thread_scene_text());
            (void)animated.frame(k.tree.snapshot(*k.root), 200, 100, 0);
            const auto at = animated.frame(k.tree.snapshot(*k.root), 200, 100, 500);
            const auto& ks = at.root.children[0].style;
            require(ks.text(P::BackgroundSize) == "20px 20px" && ks.text(P::BackgroundRepeat) == "no-repeat" &&
                        ks.text(P::BackgroundPosition) == "center" &&
                        ks.text(P::BackgroundImage) == "-weft-texture(map)",
                    "a keyframe's shorthand in the longhands, the size between its keyframes: " +
                        std::string(ks.text(P::BackgroundSize)));
        }
        // A shorthand without them resets them.
        s.set(e, "style",
              "position:absolute;left:10px;top:10px;width:80px;height:60px;background-size:10px;"
              "background:-weft-texture(map)");
        const auto reset = solver.frame(s.tree.snapshot(*s.root), 200, 100, 16);
        const auto& rs = reset.root.children[0].style;
        require(rs.text(P::BackgroundSize) == "auto" && rs.text(P::BackgroundPosition) == "0% 0%" &&
                    rs.text(P::BackgroundRepeat) == "repeat" && rs.text(P::BackgroundColor) == "transparent",
                "a shorthand resets the longhands it does not give");
        // What stays outside the profile is the scene's diagnostic.
        const auto refused = [&](const std::string& css, const std::string& message) {
            weft::testing::NativeScene d;
            d.set(*d.root, "style", "position:relative;width:200px;height:100px");
            auto& x = d.html("div");
            d.set(x, "style", "position:absolute;left:10px;top:10px;width:80px;height:60px;" + css);
            d.tree.insert(*d.root, x, nullptr);
            weft::SceneSolver one("", "", weft::testing::thread_scene_text());
            const auto [code, text] = diagnostic([&] {
                (void)one.frame(d.tree.snapshot(*d.root), 200, 100, 0);
            });
            return code != weft::Diag{} && text.find(message) != std::string::npos;
        };
        require(
            refused("background:-weft-texture(map) fixed", "background-attachment fixed") &&
                refused("background-image:-weft-texture(map);background-clip:text", "background-clip text") &&
                refused("background-image:-weft-texture(map);background-origin:content-box;padding:10%",
                        "content-box with padding") &&
                refused("background-image:-weft-texture(map);background-position:top 4px", "background-position") &&
                refused("background:linear-gradient(#ff0000, #0000ff) no-repeat", "background-repeat no-repeat") &&
                refused("background:linear-gradient(#ff0000, #0000ff) border-box", "background-origin border-box") &&
                refused("background:red, blue", "background shorthand outside one layer") &&
                refused("background:red blue", "background shorthand outside one layer"),
            "diagnostics");
        // A gradient keeps working with the initial values the shorthand sets.
        weft::testing::NativeScene g;
        g.set(*g.root, "style", "position:relative;width:200px;height:100px");
        auto& ge = g.html("div");
        g.set(
            ge, "style",
            "position:absolute;left:10px;top:10px;width:80px;height:60px;background:linear-gradient(#ff0000, #0000ff) "
            "repeat scroll padding-box border-box");
        g.tree.insert(*g.root, ge, nullptr);
        weft::SceneSolver gs("", "", weft::testing::thread_scene_text());
        const auto gf = gs.frame(g.tree.snapshot(*g.root), 200, 100, 0);
        const auto gl = weft::scene_display_list(gf, {}, &weft::testing::thread_scene_text());
        require(gl.fallback.empty() && gl.list.items.size() == 1 && gl.list.boxes[0].gradient.stops.size() == 2,
                "a gradient with the initial values: " + gl.fallback + " " + std::to_string(gl.list.items.size()) +
                    " items");
    }

    // A url() image of the scene: "t.png".
    struct OneImage final : weft::SceneImages {
        std::shared_ptr<const weft::paint::Asset> asset;

        std::shared_ptr<const weft::paint::Asset> image(std::string_view url) override {
            return url == "t.png" ? asset : nullptr;
        }
    };

    // The display list's fit and item, and the native pass (D3D11 WARP) against the reference raster: the same
    // texture (linear premultiplied RGBA8, some texels translucent) behind elements of each layout; or, with urls, a
    // url() image of as many texels (straight sRGB, the same columns translucent).
    void drawn(const std::filesystem::path& folder, bool urls) {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:240px;height:160px;background-color:#d8d0c0");
        const char* layouts[] = {
            // The natural size, repeated from the padding box, under a border.
            "left:4px;top:4px;width:70px;height:46px;border:3px solid #202020",
            // Stretched over the box, once.
            "left:80px;top:4px;width:60px;height:30px;background-size:100% 100%;background-repeat:no-repeat",
            // Cover and contain, centered.
            "left:146px;top:4px;width:40px;height:46px;background-size:cover;background-position:center;"
            "background-repeat:no-repeat",
            "left:192px;top:4px;width:44px;height:46px;background-size:contain;background-position:center;"
            "background-repeat:no-repeat;background-color:#305070",
            // A width with auto, at the far edges, repeated down only.
            "left:4px;top:56px;width:60px;height:50px;background-size:20px auto;"
            "background-position:right 5px bottom 4px;background-repeat:no-repeat repeat",
            // space across, round down; positioned in the border box, painted in the padding box, inside corners.
            "left:70px;top:56px;width:80px;height:50px;background-size:14px 9px;background-repeat:space round;"
            "background-origin:border-box;background-clip:padding-box;border:4px solid #a07040;border-radius:12px",
            // The content box, with padding.
            "left:156px;top:56px;width:80px;height:50px;padding:6px 10px;background-clip:content-box;"
            "background-origin:content-box;background-size:12px 12px;border:2px solid #000000",
            // repeat-x at a calc() position.
            "left:4px;top:112px;width:110px;height:44px;background-position:calc(50% + 3px) 25%;"
            "background-repeat:repeat-x;background-size:18px 12px",
            // A scaled, rotated element.
            "left:130px;top:118px;width:60px;height:30px;transform:rotate(15deg);background-size:30px 15px",
        };
        for (const auto* l : layouts) {
            auto& e = s.html("div");
            s.set(e, "style",
                  std::string(urls ? "position:absolute;background-image:url(t.png);"
                                   : "position:absolute;background-image:-weft-texture(t);") +
                      l);
            s.tree.insert(*s.root, e, nullptr);
        }
        // An image of 6 x 4 texels, each its own color, two columns translucent.
        weft::paint::Asset asset{6, 4, weft::paint::Encoding::SrgbStraight, {}, 0};
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 6; ++x) {
                const unsigned a = x >= 4 ? 128 : 255;
                for (const unsigned c : {40 * x + 30 * y, 200 - 30 * x, 60 + 45 * y, a}) {
                    asset.bytes.push_back(std::uint8_t(c));
                }
            }
        }
        OneImage images_of;
        images_of.asset = std::make_shared<const weft::paint::Asset>(std::move(asset));
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        solver.set_images(&images_of);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 240, 160, 0);
        // The display list: each element's item over its painting area with the fit of its layout.
        const auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text());
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 1)), "a well-formed list");
        std::vector<const weft::paint::Item*> images;
        for (const auto& item : listed.list.items) {
            if (item.kind == weft::paint::Item::Kind::Image) {
                images.push_back(&item);
            }
        }
        require(images.size() == std::size(layouts), "an image item for each element");
        require(std::all_of(images.begin(), images.end(),
                            [&](const weft::paint::Item* item) {
                                const auto& image = listed.list.images[item->payload];
                                return urls ? image.fitted && !image.texture && listed.list.assets.size() == 1 &&
                                                  listed.list.assets[image.asset] == images_of.asset
                                            : image.texture && !image.fitted;
                            }),
                urls ? "the image an asset of the list, fitted" : "the texture by name");
        const auto& first = listed.list.images[images[0]->payload].fit;
        require(images[0]->width == 70 && images[0]->height == 46 && first.area == std::array<float, 4>{3, 3, 64, 40},
                "the border box painted, the padding box positioning");
        const auto& sixth = *images[5];
        require(sixth.transform[4] == 74 && sixth.width == 72 &&
                    listed.list.images[sixth.payload].fit.area == std::array<float, 4>{-4, -4, 80, 50} &&
                    sixth.clips_count == 1,
                "the padding box painted inside its corners, the border box positioning");

        // A texture of 6 x 4 texels, each its own color, two columns translucent.
        weft::SceneRasterTexture texture{6, 4, {}};
        std::vector<std::uint8_t> bytes;
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 6; ++x) {
                const std::uint8_t a = x >= 4 ? 128 : 255;
                const std::array<std::uint8_t, 4> rgba{std::uint8_t((40 * x + 30 * y) * a / 255),
                                                       std::uint8_t((200 - 30 * x) * a / 255),
                                                       std::uint8_t((60 + 45 * y) * a / 255), a};
                for (const auto c : rgba) {
                    bytes.push_back(c);
                    texture.rgba.push_back(float(c) / 255);
                }
            }
        }
        for (const float scale : {1.f, 2.f}) {
            const auto w = unsigned(240 * scale);
            const auto h = unsigned(160 * scale);
            weft::testing::D3D11Scene d3d11{w, h};
            using Microsoft::WRL::ComPtr;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 6;
            desc.Height = 4;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{bytes.data(), 6 * 4, 0};
            ComPtr<ID3D11Texture2D> gpu_texture;
            ComPtr<ID3D11ShaderResourceView> view;
            require(SUCCEEDED(d3d11.gpu.device->CreateTexture2D(&desc, &data, &gpu_texture)) &&
                        SUCCEEDED(d3d11.gpu.device->CreateShaderResourceView(gpu_texture.Get(), nullptr, &view)),
                    "the host texture");
            const std::vector<weft::SceneTexture> bound{
                {"t", std::uint64_t(reinterpret_cast<std::uintptr_t>(view.Get())), false}};
            using Textures = std::span<const weft::SceneTexture>;
            const auto native = d3d11.draw(frame, scale, nullptr, {}, urls ? Textures{} : Textures(bound));
            d3d11.gpu.check_debug_messages();
            require(d3d11.scene.fallbacks == 0, "the native pass drew the frame");
            weft::SceneRaster reference(weft::testing::thread_scene_text());
            if (!urls) {
                reference.bind_textures({{"t", texture}});
            }
            const auto raster = reference.raster(frame, scale);
            require(raster.width == w && raster.height == h, "the reference raster's size");
            // Inside each axis-aligned painting area (2 device px in from its edges, its borders (at most 4 px here)
            // and its corners' radii, whose antialiasing and border joins the two draw apart), the native pass's
            // texels are the reference raster's.
            std::size_t apart{};
            std::size_t compared{};
            int worst{};
            const auto at_scale = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), scale).list;
            for (const auto& item : at_scale.items) {
                if (item.kind != weft::paint::Item::Kind::Image || item.transform[1] != 0 || item.transform[2] != 0) {
                    continue;
                }
                float radius{};
                for (std::uint32_t c = 0; c < item.clips_count; ++c) {
                    const auto& clip = at_scale.clips[item.clips_begin + c];
                    radius = std::max({radius, *std::max_element(clip.radius_x.begin(), clip.radius_x.end()),
                                       *std::max_element(clip.radius_y.begin(), clip.radius_y.end())});
                }
                const int inset = 2 + int(std::ceil((radius + 4) * scale));
                const int x0 = int(std::lround(item.transform[4] * scale)) + inset;
                const int y0 = int(std::lround(item.transform[5] * scale)) + inset;
                const int x1 = int(std::lround((item.transform[4] + item.width * item.transform[0]) * scale)) - inset;
                const int y1 = int(std::lround((item.transform[5] + item.height * item.transform[3]) * scale)) - inset;
                for (int y = std::max(y0, 0); y < std::min(y1, int(h)); ++y) {
                    for (int x = std::max(x0, 0); x < std::min(x1, int(w)); ++x) {
                        const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                        const auto* r = raster.rgba.data() + (std::size_t(y) * w + x) * 4;
                        int d{};
                        for (unsigned c = 0; c < 3; ++c) {
                            d = std::max(d, std::abs(int(n[2 - c]) - int(r[c])));
                        }
                        if (d > 2 && apart < 8) {
                            std::printf("  apart at %d, %d: native %d %d %d, raster %d %d %d\n", x, y, n[2], n[1], n[0],
                                        r[0], r[1], r[2]);
                        }
                        apart += d > 2;
                        worst = std::max(worst, d);
                        ++compared;
                    }
                }
            }
            std::vector<std::uint8_t> trio(std::size_t(w) * 2 * h * 4);
            for (unsigned y = 0; y < h; ++y) {
                for (unsigned x = 0; x < w; ++x) {
                    const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                    const auto* r = raster.rgba.data() + (std::size_t(y) * w + x) * 4;
                    auto* left = trio.data() + (std::size_t(y) * w * 2 + x) * 4;
                    left[0] = n[2];
                    left[1] = n[1];
                    left[2] = n[0];
                    left[3] = 255;
                    auto* right = trio.data() + (std::size_t(y) * w * 2 + w + x) * 4;
                    std::copy(r, r + 3, right);
                    right[3] = 255;
                }
            }
            weft::testing::write_png(folder / ((urls ? "images-" : "textures-") + std::to_string(int(scale)) + ".png"),
                                     w * 2, h, trio);
            std::printf("scale %.0f: %zu of %zu pixels inside the painting areas more than 2 levels apart, worst %d\n",
                        scale, apart, compared, worst);
            require(compared > w * h / 4 && apart == 0, "the native pass lays the texture out as the reference raster");
        }
    }

    // border-image's pieces (scene_border_image.hpp) of a 9 x 9 image sliced at 3 px over a 100 x 60 border box.
    void border_image_geometry() {
        weft::StyleValues s;
        for (const auto id : {weft::PropertyId::BorderTopWidth, weft::PropertyId::BorderRightWidth,
                              weft::PropertyId::BorderBottomWidth, weft::PropertyId::BorderLeftWidth}) {
            s[id] = "10px";
        }
        s[weft::PropertyId::BorderImageSlice] = "3 fill";
        using Box = std::array<float, 4>;
        auto pieces = weft::scene_border_image(s, 100, 60, 9, 9);
        require(pieces.size() == 9 && pieces[0].source == Box{0, 0, 3, 3} && pieces[0].target == Box{0, 0, 10, 10} &&
                    pieces[1].source == Box{3, 0, 3, 3} && pieces[1].target == Box{10, 0, 80, 10} &&
                    pieces[4].source == Box{3, 3, 3, 3} && pieces[4].target == Box{10, 10, 80, 40} &&
                    pieces[8].target == Box{90, 50, 10, 10},
                "fill: nine pieces, corners as wide as the borders, edges and the middle stretched");
        s[weft::PropertyId::BorderImageSlice] = "3";
        require(weft::scene_border_image(s, 100, 60, 9, 9).size() == 8, "without fill no middle");
        s[weft::PropertyId::BorderImageSlice] = "33.3333% 1";
        pieces = weft::scene_border_image(s, 100, 60, 9, 9);
        require(std::abs(pieces[0].source[3] - 3) < 1e-3f && pieces[0].source[2] == 1, "percentages of the image");
        s[weft::PropertyId::BorderImageSlice] = "3";
        s[weft::PropertyId::BorderImageWidth] = "2 auto";
        pieces = weft::scene_border_image(s, 100, 60, 9, 9);
        require(pieces[0].target == Box{0, 0, 3, 20}, "numbers are border widths, auto the slice's");
        s[weft::PropertyId::BorderImageWidth] = "40px";
        pieces = weft::scene_border_image(s, 100, 60, 9, 9);
        // 40 + 40 of a height of 60: every width scaled by 0.75, the corners meet and the side pieces have no height.
        require(pieces[0].target == Box{0, 0, 30, 30} && pieces.size() == 6,
                "opposite widths past the area are scaled down together");
        s[weft::PropertyId::BorderImageWidth] = "1";
        s[weft::PropertyId::BorderImageOutset] = "5px 1";
        pieces = weft::scene_border_image(s, 100, 60, 9, 9);
        // Without fill the eight pieces run top row, the sides, bottom row: the bottom edge is the seventh.
        require(pieces[0].target == Box{-10, -5, 10, 10} && pieces[6].target == Box{0, 55, 100, 10},
                "outsets grow the area: px, and numbers of the border widths");
        for (const auto& [id, value] :
             std::vector<std::pair<weft::PropertyId, std::string>>{{weft::PropertyId::BorderImageSlice, "3px"},
                                                                   {weft::PropertyId::BorderImageSlice, "1 2 3 4 5"},
                                                                   {weft::PropertyId::BorderImageWidth, "1em"},
                                                                   {weft::PropertyId::BorderImageOutset, "10%"}}) {
            auto bad = s;
            bad[id] = value;
            bool refused{};
            try {
                (void)weft::scene_border_image(bad, 100, 60, 9, 9);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused, "outside the profile: " + value);
        }
    }

    // border-image drawn: the display list's pieces (fitted images of slices) and the native pass against the reference
    // raster inside each piece, of a 9 x 9 url() image whose texels are each their own color, some translucent.
    void border_images_drawn(const std::filesystem::path& folder) {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:240px;height:160px;background-color:#d8d0c0");
        const char* layouts[] = {
            "left:10px;top:10px;width:100px;height:60px;border:10px solid #000000;border-image-slice:3 fill",
            "left:130px;top:14px;width:90px;height:70px;border-width:12px 8px;border-style:solid;"
            "border-image-slice:33.3333% 3 3 3;border-image-width:1 2;border-image-outset:4px",
            "left:20px;top:100px;width:200px;height:44px;border:6px solid;border-image-slice:3 fill;"
            "border-image-width:auto;background-color:#305070",
        };
        for (const auto* l : layouts) {
            auto& e = s.html("div");
            s.set(e, "style", std::string("position:absolute;border-image-source:url(t.png);") + l);
            s.tree.insert(*s.root, e, nullptr);
        }
        weft::paint::Asset asset{9, 9, weft::paint::Encoding::SrgbStraight, {}, 0};
        for (unsigned y = 0; y < 9; ++y) {
            for (unsigned x = 0; x < 9; ++x) {
                const unsigned a = (x + y) % 4 == 0 ? 128 : 255;
                for (const unsigned c : {25 * x + 10, 220 - 20 * y, 40 + 10 * (x + y), a}) {
                    asset.bytes.push_back(std::uint8_t(c));
                }
            }
        }
        OneImage images_of;
        images_of.asset = std::make_shared<const weft::paint::Asset>(std::move(asset));
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        solver.set_images(&images_of);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 240, 160, 0);
        const auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text());
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 1)), "a well-formed list");
        std::size_t pieces{};
        for (const auto& item : listed.list.items) {
            if (item.kind == weft::paint::Item::Kind::Image) {
                const auto& image = listed.list.images[item.payload];
                require(image.fitted && listed.list.assets[image.asset] == images_of.asset, "a piece of the image");
                ++pieces;
            }
            if (item.kind == weft::paint::Item::Kind::Box) {
                const auto& box = listed.list.boxes[item.payload];
                require(box.widths == std::array<float, 4>{}, "the border image instead of the border styles");
            }
        }
        require(pieces == 9 + 8 + 9, "nine pieces with fill, eight without");
        for (const float scale : {1.f, 2.f}) {
            const auto w = unsigned(240 * scale);
            const auto h = unsigned(160 * scale);
            weft::testing::D3D11Scene d3d11{w, h};
            const auto native = d3d11.draw(frame, scale);
            d3d11.gpu.check_debug_messages();
            require(d3d11.scene.fallbacks == 0, "the native pass drew the frame");
            weft::SceneRaster reference(weft::testing::thread_scene_text());
            const auto raster = reference.raster(frame, scale);
            std::size_t apart{};
            std::size_t compared{};
            int worst{};
            const auto at_scale = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), scale).list;
            for (const auto& item : at_scale.items) {
                if (item.kind != weft::paint::Item::Kind::Image) {
                    continue;
                }
                // Inside each piece, a device px in from its edges (where the next piece's texels meet its own).
                const int x0 = int(std::lround(item.transform[4] * scale)) + 1;
                const int y0 = int(std::lround(item.transform[5] * scale)) + 1;
                const int x1 = int(std::lround((item.transform[4] + item.width) * scale)) - 1;
                const int y1 = int(std::lround((item.transform[5] + item.height) * scale)) - 1;
                for (int y = std::max(y0, 0); y < std::min(y1, int(h)); ++y) {
                    for (int x = std::max(x0, 0); x < std::min(x1, int(w)); ++x) {
                        const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                        const auto* r = raster.rgba.data() + (std::size_t(y) * w + x) * 4;
                        int d{};
                        for (unsigned c = 0; c < 3; ++c) {
                            d = std::max(d, std::abs(int(n[2 - c]) - int(r[c])));
                        }
                        if (d > 2 && apart < 8) {
                            std::printf("  apart at %d, %d: native %d %d %d, raster %d %d %d\n", x, y, n[2], n[1], n[0],
                                        r[0], r[1], r[2]);
                        }
                        apart += d > 2;
                        worst = std::max(worst, d);
                        ++compared;
                    }
                }
            }
            std::vector<std::uint8_t> pair(std::size_t(w) * 2 * h * 4);
            for (unsigned y = 0; y < h; ++y) {
                for (unsigned x = 0; x < w; ++x) {
                    const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                    const auto* r = raster.rgba.data() + (std::size_t(y) * w + x) * 4;
                    auto* left = pair.data() + (std::size_t(y) * w * 2 + x) * 4;
                    left[0] = n[2];
                    left[1] = n[1];
                    left[2] = n[0];
                    left[3] = 255;
                    auto* right = pair.data() + (std::size_t(y) * w * 2 + w + x) * 4;
                    std::copy(r, r + 3, right);
                    right[3] = 255;
                }
            }
            weft::testing::write_png(folder / ("border-images-" + std::to_string(int(scale)) + ".png"), w * 2, h, pair);
            std::printf("border images at scale %.0f: %zu of %zu pixels inside the pieces more than 2 levels apart, "
                        "worst %d\n",
                        scale, apart, compared, worst);
            require(compared > w * h / 8 && apart == 0, "the native pass draws the pieces as the reference raster");
        }
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_scene_background_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        parses();
        tiles();
        solver();
        drawn(folder, false);
        drawn(folder, true);
        border_image_geometry();
        border_images_drawn(folder);
        std::cout << "PASS scene background layouts of host textures and url() images, border images\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
