#include "paint/display_list.hpp"
#include "paint/particles.hpp"
#include "render/atlas_packer.hpp"
#include "render/quad_list.hpp"
#include "render/rounded_clip.hpp"
#include "render/scene_pass.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <memory>
#include <stdexcept>
#include <string>

// The portable paint IR: structural checks of display lists, the portable plan of a scene pass, quad packets as
// display lists, external textures, appended lists, UI shader effects and UI particles.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Kind = paint::Item::Kind;

    paint::Item item(Kind kind, std::uint32_t payload = 0) {
        paint::Item i;
        i.kind = kind;
        i.payload = payload;
        return i;
    }

    // A box, a text raster inside a group inside another group.
    paint::DisplayList nested() {
        paint::DisplayList list;
        list.boxes.emplace_back();
        list.rasters.push_back({std::make_shared<weft::SceneNode>()});
        list.items = {item(Kind::Box),      item(Kind::BeginGroup), item(Kind::BeginGroup), item(Kind::Text),
                      item(Kind::EndGroup), item(Kind::Layer),      item(Kind::EndGroup)};
        return list;
    }

    bool rejected(const paint::DisplayList& list, std::size_t depth, const std::string& reason) {
        const auto status = paint::check(list, depth);
        return !status && status.error().code == weft::Diag::PaintList &&
               status.error().message.find(reason) != std::string::npos;
    }

    void checks() {
        const auto good = nested();
        require(bool(paint::check(good, 2)) && paint::group_depth(good) == 2, "well-formed list");
        require(bool(paint::check(paint::DisplayList{}, 0)), "empty list");
        require(rejected(good, 1, "group depth bound at item 2"), "group depth bound");
        auto list = good;
        list.items[0].payload = 1;
        require(rejected(list, 2, "box payload out of range at item 0"), "box payload range");
        list = good;
        list.items[3].payload = 1;
        require(rejected(list, 2, "raster payload out of range or without source at item 3"), "raster payload range");
        list = good;
        list.rasters[0].source.reset();
        require(rejected(list, 2, "without source at item 3"), "raster without source");
        list = good;
        list.items.erase(list.items.begin() + 1);
        require(rejected(list, 2, "group end without begin at item 5"), "unmatched group end");
        list = good;
        list.items.pop_back();
        require(rejected(list, 2, "unclosed group"), "unclosed group");
        list = good;
        list.items[0].kind = Kind(99);
        require(rejected(list, 2, "unknown item kind at item 0"), "unknown kind");
        list = good;
        list.shadows.push_back({{0, 0, 0, .5f}, 1, 2, 4, 0, {}});
        list.items.push_back(item(Kind::Shadow, 0));
        require(bool(paint::check(list, 2)), "shadow item");
        list.items.back().payload = 1;
        require(rejected(list, 2, "shadow payload out of range at item 7"), "shadow payload range");
        list.items.back().payload = 0;
        list.shadows[0].blur = -1;
        require(rejected(list, 2, "shadow value at item 7"), "negative shadow blur");
        list.shadows[0].blur = 4;
        list.shadows[0].x = std::numeric_limits<float>::quiet_NaN();
        require(rejected(list, 2, "shadow value at item 7"), "shadow offset not finite");
        // Paths: verbs that start with a move and use exactly their points, finite points, a sane stroke.
        using Verb = paint::Path::Verb;
        paint::Path line;
        line.verbs = {Verb::Move, Verb::Line, Verb::Cubic, Verb::Close};
        line.points = {{0, 0}, {1, 1}, {2, 0}, {3, 1}, {4, 0}};
        const auto with = [&](paint::Path path, std::optional<paint::Stroke> stroke = std::nullopt) {
            auto l = good;
            l.paths.push_back({std::make_shared<const paint::Path>(std::move(path)), {1, 1, 1, 1}, stroke});
            l.items.push_back(item(Kind::Path, 0));
            return l;
        };
        require(bool(paint::check(with(line), 2)) && bool(paint::check(with(line, paint::Stroke{}), 2)), "path item");
        auto broken = line;
        broken.verbs[0] = Verb::Line;
        require(rejected(with(broken), 2, "path value at item 7"), "a path starts with a move");
        broken = line;
        broken.points.pop_back();
        require(rejected(with(broken), 2, "path value at item 7"), "path points short of its verbs");
        broken = line;
        broken.points[2][1] = std::numeric_limits<float>::infinity();
        require(rejected(with(broken), 2, "path value at item 7"), "path point not finite");
        paint::Stroke wide;
        wide.width = -1;
        require(rejected(with(line, wide), 2, "path value at item 7"), "negative stroke width");
        wide.width = 1;
        wide.dashes = {2, -1};
        require(rejected(with(line, wide), 2, "path value at item 7"), "negative dash");
        wide.dashes.clear();
        wide.miter_limit = .5f;
        require(rejected(with(line, wide), 2, "path value at item 7"), "miter limit below one");
        auto missing = with(line);
        missing.paths[0].path.reset();
        require(rejected(missing, 2, "path value at item 7"), "path without a path");
        missing.items.back().payload = 1;
        require(rejected(missing, 2, "path payload out of range at item 7"), "path payload range");
    }

    // A shadow image is the box's shape spread, offset and blurred, shown only outside the box, over a rectangle
    // padded by three standard deviations and two device pixels; the scene pass draws it as an image there.
    void shadows() {
        namespace render = weft::render;
        const paint::Shadow hard{{1, 0, 0, 1}, 10, 0, 0, 0, {}};
        const auto g = render::shadow_geometry(hard, 20, 10, 2);
        require(g.rect == std::array<float, 4>{9, -1, 22, 12} && g.width == 44 && g.height == 24,
                "hard shadow geometry");
        const auto image = render::shadow_image(hard, 20, 10, 2).value();
        require(image.geometry.rect == g.rect && image.rgba.size() == std::size_t(44) * 24 * 4, "hard shadow size");
        // The pixel whose center is at (x, y) in item px.
        const auto at = [](const render::ShadowImage& i, float x, float y) {
            const auto col = unsigned((x - i.geometry.rect[0]) * i.geometry.width / i.geometry.rect[2]);
            const auto row = unsigned((y - i.geometry.rect[1]) * i.geometry.height / i.geometry.rect[3]);
            return i.rgba.data() + (std::size_t(row) * i.geometry.width + col) * 4;
        };
        require(at(image, 15.25f, 5.25f)[3] == 0, "a shadow under its box");
        const auto* red = at(image, 25.25f, 5.25f);
        require(red[0] == 255 && red[1] == 0 && red[2] == 0 && red[3] == 255, "a hard shadow beside its box");
        require(at(image, 30.75f, 5.25f)[3] == 0, "past a hard shadow");
        // Blurred and spread: symmetric about the box, fading with distance, wider for a larger spread.
        const paint::Shadow soft{{0, 0, 0, 1}, 0, 0, 8, 2, {4, 4, 4, 4}};
        const auto blurred = render::shadow_image(soft, 20, 10, 1).value();
        const auto alpha = [&](const render::ShadowImage& i, float x, float y) {
            return int(at(i, x, y)[3]);
        };
        require(blurred.geometry.rect == std::array<float, 4>{-16, -16, 52, 42}, "blurred shadow geometry");
        require(alpha(blurred, 10.5f, 5.5f) == 0 &&
                    std::abs(alpha(blurred, -2.5f, 5.5f) - alpha(blurred, 22.5f, 5.5f)) <= 1 &&
                    alpha(blurred, -2.5f, 5.5f) > alpha(blurred, -6.5f, 5.5f) &&
                    alpha(blurred, -6.5f, 5.5f) > alpha(blurred, -10.5f, 5.5f) && alpha(blurred, -2.5f, 5.5f) > 64,
                "blurred shadow falls off symmetrically");
        auto wider = soft;
        wider.spread = 6;
        require(alpha(render::shadow_image(wider, 20, 10, 1).value(), -6.5f, 5.5f) > alpha(blurred, -6.5f, 5.5f),
                "spread widens a shadow");
        auto huge = soft;
        huge.blur = 64;
        const auto refused = render::shadow_image(huge, 4000, 4000, 2);
        require(!refused && refused.error().code == weft::Diag::PaintList, "shadow bitmap bound");
        // Planned as an image with its own colors over the shadow's rectangle, under the item's transform.
        paint::DisplayList list;
        list.shadows.push_back(hard);
        auto shadow = item(Kind::Shadow, 0);
        shadow.transform = {1, 0, 0, 1, 100, 50};
        shadow.width = 20;
        shadow.height = 10;
        shadow.clip = {0, 0, 200, 100};
        shadow.opacity = .5f;
        list.items = {shadow};
        require(bool(paint::check(list, 1)), "shadow list is well formed");
        const auto pass = render::plan_scene_pass(list, 2, 400, 200).value();
        const auto& r = pass.records[0];
        require(pass.batches.size() == 1 && pass.batches[0].step == render::ScenePass::Step::Image &&
                    r.size == std::array<float, 4>{22, 12, 0, 0} && r.extra[1] == .5f &&
                    r.extra[2] == render::scene_limits::flag_enabled &&
                    r.extra[3] == render::scene_limits::flag_disabled,
                "shadow image record");
        require(r.inv_x == std::array<float, 4>{1, 0, -109, 0} && r.inv_y == std::array<float, 4>{0, 1, -49, 0},
                "a shadow record maps its rectangle");
        // A path item draws its coverage over device pixels: the mask vector::mask_bounds gives within the clip and
        // the target, placed texel for pixel and tinted with the path's color.
        paint::DisplayList drawn;
        auto square = std::make_shared<paint::Path>();
        square->verbs = {paint::Path::Verb::Move, paint::Path::Verb::Line, paint::Path::Verb::Line,
                         paint::Path::Verb::Line, paint::Path::Verb::Close};
        square->points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        drawn.paths.push_back({square, {1, 0, 0, .5f}, std::nullopt});
        auto path_item = item(Kind::Path, 0);
        path_item.transform = {1, 0, 0, 1, 5.25f, 3};
        path_item.clip = {0, 0, 200, 100};
        drawn.items = {path_item};
        const auto path_pass = render::plan_scene_pass(drawn, 2, 40, 30).value();
        const auto& p = path_pass.records[0];
        require(path_pass.masks.size() == 1 && path_pass.masks[0] == std::array<std::int32_t, 4>{9, 5, 32, 27},
                "path mask in device pixels");
        require(path_pass.batches.size() == 1 && path_pass.batches[0].step == render::ScenePass::Step::Image &&
                    p.size == std::array<float, 4>{11.5f, 11, 0, 0} && p.background == paint::Color{1, 0, 0, .5f} &&
                    p.extra[2] == render::scene_limits::flag_enabled &&
                    p.extra[3] == render::scene_limits::flag_enabled &&
                    p.inv_x == std::array<float, 4>{1, 0, -4.5f, 0} && p.inv_y == std::array<float, 4>{0, 1, -2.5f, 0},
                "path record");
        drawn.items[0].clip = {50, 50, 60, 60};
        const auto hidden = render::plan_scene_pass(drawn, 2, 40, 30).value();
        require(hidden.masks[0] == std::array<std::int32_t, 4>{} && hidden.records[0].size[0] == 0,
                "a path outside its clip has no mask");
    }

    // Runs of boxes share a draw, images and groups get their own steps, and an opaque first box that covers the
    // target becomes the clear color unless crop narrows it.
    // Shelf packing: regions never overlap, a full page opens the next, the page bound refuses, and a page whose regions
    // all return starts empty.
    void packer() {
        weft::render::AtlasPacker atlas(16, 2);
        require(!atlas.allocate(0, 4) && !atlas.allocate(17, 4), "empty and oversized regions");
        const auto a = atlas.allocate(10, 6).value();
        const auto b = atlas.allocate(6, 6).value();
        const auto c = atlas.allocate(8, 4).value();
        require(a.page == 0 && a.x == 0 && a.y == 0 && b.x == 10 && b.y == 0 && c.x == 0 && c.y == 6,
                "shelves fill left to right, then down");
        const auto d = atlas.allocate(16, 8).value();
        require(d.page == 1 && d.x == 0 && d.y == 0 && atlas.pages() == 2, "a full page opens the next");
        require(atlas.allocate(16, 16) == std::nullopt, "page bound");
        for (const auto& r : {a, b, c}) {
            atlas.release(r);
        }
        const auto e = atlas.allocate(16, 16).value();
        require(e.page == 0 && e.x == 0 && e.y == 0 && atlas.live() == 2, "an emptied page starts over");
        atlas.release(d);
        bool twice{};
        try {
            atlas.release(d);
        } catch (const std::logic_error&) {
            twice = true;
        }
        require(twice, "a region released twice");
    }

    // Space comes back while a page holds other regions: a released span takes a region as short as its shelf, free
    // neighbours join, an emptied shelf takes a shorter region at its top and keeps the rest, empty shelves at the end
    // give their rows back, and a shelf more than half as tall again as a region is left for a new one. Random regions
    // never overlap or leave their page.
    void packer_reuse() {
        using Region = weft::render::AtlasPacker::Region;
        weft::render::AtlasPacker atlas(16, 1);
        const auto a = atlas.allocate(4, 4).value();
        const auto b = atlas.allocate(4, 4).value();
        const auto c = atlas.allocate(4, 4).value();
        const auto d = atlas.allocate(4, 4).value();
        const auto e = atlas.allocate(16, 6).value();
        const auto f = atlas.allocate(16, 2).value();
        require(a.x == 0 && d.x == 12 && d.y == 0 && e.y == 4 && f.y == 10 && !atlas.allocate(16, 5),
                "three shelves fill the page but its last rows");
        atlas.release(b);
        const auto g = atlas.allocate(4, 3).value();
        require(g.x == 4 && g.y == 0, "a released span takes a region as short as its shelf");
        atlas.release(g);
        atlas.release(c);
        const auto h = atlas.allocate(8, 4).value();
        require(h.x == 4 && h.y == 0, "free neighbours join");
        atlas.release(e);
        const auto i = atlas.allocate(16, 2).value();
        const auto j = atlas.allocate(16, 4).value();
        require(i.y == 4 && j.y == 6, "an emptied shelf takes a shorter region at its top, and the rest another");
        atlas.release(f);
        const auto k = atlas.allocate(16, 6).value();
        require(k.y == 10 && atlas.live() == 6, "an empty shelf at the end gives its rows back");
        bool twice{};
        try {
            atlas.release(h);
            atlas.release(h);
        } catch (const std::logic_error&) {
            twice = true;
        }
        require(twice && atlas.live() == 5, "a span released twice");
        // Free spans and empty shelves join their neighbours on either side, whichever empties first.
        weft::render::AtlasPacker spans(16, 1);
        const auto s0 = spans.allocate(4, 4).value();
        const auto s1 = spans.allocate(4, 4).value();
        const auto s2 = spans.allocate(4, 4).value();
        const auto s3 = spans.allocate(4, 4).value();
        spans.release(s2);
        spans.release(s1);
        const auto both = spans.allocate(8, 4).value();
        require(s0.x == 0 && s3.x == 12 && both.x == 4 && both.y == 0, "a released span joins the free span after it");
        weft::render::AtlasPacker rows(16, 1);
        const auto top = rows.allocate(16, 4).value();
        const auto upper = rows.allocate(16, 3).value();
        const auto lower = rows.allocate(16, 3).value();
        const auto bottom = rows.allocate(16, 6).value();
        require(top.y == 0 && upper.y == 4 && lower.y == 7 && bottom.y == 10 && !rows.allocate(1, 1),
                "four shelves fill the page");
        rows.release(lower);
        rows.release(upper);
        const auto joined = rows.allocate(16, 5);
        require(joined && joined->y == 4, "an emptied shelf joins the empty shelf below it");
        rows.release(*joined);
        const auto first = rows.allocate(16, 3).value();
        const auto second = rows.allocate(16, 3).value();
        require(first.y == 4 && second.y == 7, "the joined shelf split in two");
        rows.release(first);
        rows.release(second);
        const auto again = rows.allocate(16, 5);
        require(again && again->y == 4, "an emptied shelf joins the empty shelf above it");
        weft::render::AtlasPacker tall(16, 1);
        const auto x = tall.allocate(4, 8).value();
        const auto y = tall.allocate(4, 2).value();
        const auto z = tall.allocate(12, 6).value();
        require(x.y == 0 && y.y == 8 && z.y == 0 && z.x == 4,
                "a shelf more than half as tall again is left for a new one, one less tall takes the region");
        // Random regions on three pages of 64: none overlaps another or leaves its page; all of them back, the pages
        // start empty.
        constexpr std::uint32_t side = 64;
        weft::render::AtlasPacker random(side, 3);
        std::vector<Region> live;
        std::uint32_t seed = 0x12345u;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        std::size_t placed{};
        for (unsigned step = 0; step < 20000; ++step) {
            if (!live.empty() && next() % 2) {
                const auto at = next() % live.size();
                random.release(live[at]);
                live.erase(live.begin() + std::ptrdiff_t(at));
            } else if (const auto r = random.allocate(1 + next() % 24, 1 + next() % 16)) {
                live.push_back(*r);
                ++placed;
            }
            require(random.live() == live.size(), "random live count");
            if (step % 100) {
                continue;
            }
            std::vector<std::uint8_t> used(std::size_t(3) * side * side);
            for (const auto& r : live) {
                require(r.page < 3 && r.x + r.width <= side && r.y + r.height <= side, "random region on its page");
                for (std::uint32_t v = r.y; v < r.y + r.height; ++v) {
                    for (std::uint32_t u = r.x; u < r.x + r.width; ++u) {
                        auto& cell = used[(std::size_t(r.page) * side + v) * side + u];
                        require(!cell, "random regions overlap");
                        cell = 1;
                    }
                }
            }
        }
        require(placed > 5000, "random regions placed");
        for (const auto& r : live) {
            random.release(r);
        }
        for (std::uint32_t page = 0; page < 3; ++page) {
            const auto whole = random.allocate(side, side);
            require(whole && whole->page == page, "pages start empty once every region is back");
        }
    }

    // With texture names, consecutive boxes and images that sample one texture are one draw; another texture or a
    // group ends the run. Without them every image is its own step.
    void batching() {
        paint::DisplayList list;
        list.assets.push_back(std::make_shared<paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {255}}));
        list.assets.push_back(std::make_shared<paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {255}}));
        list.assets.push_back(std::make_shared<paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {255}}));
        list.boxes.emplace_back();
        list.images.resize(3);
        list.images[1].asset = 1;
        list.images[2].asset = 2;
        list.items = {item(Kind::Box),      item(Kind::Image, 0),   item(Kind::Box),      item(Kind::Image, 1),
                      item(Kind::Image, 2), item(Kind::BeginGroup), item(Kind::Image, 0), item(Kind::EndGroup)};
        for (auto& i : list.items) {
            i.width = i.height = 10;
            i.clip = {0, 0, 10, 10};
        }
        // Assets 0 and 1 share a texture (an atlas page); asset 2 has its own.
        const std::vector<std::uint64_t> textures{7, 7, 9};
        const auto merged =
            weft::render::plan_scene_pass(list, 1, 100, 100, std::nullopt, weft::render::Space::Linear, &textures)
                .value();
        using Step = weft::render::ScenePass::Step;
        const auto& b = merged.batches;
        require(b.size() == 5 && b[0].step == Step::Image && b[0].first == 0 && b[0].count == 4 && b[0].source == 0 &&
                    b[1].step == Step::Image && b[1].first == 4 && b[1].count == 1 && b[1].source == 2 &&
                    b[2].step == Step::BeginGroup && b[3].step == Step::Image && b[3].source == 0 &&
                    b[4].step == Step::EndGroup,
                "runs on one texture");
        const auto single = weft::render::plan_scene_pass(list, 1, 100, 100).value();
        require(single.batches.size() == 8 && single.batches[1].step == Step::Image && single.batches[1].count == 1 &&
                    single.batches[3].source == 1,
                "one step per image without texture names");
    }

    // Instance bounds: an encoded box keeps its antialiasing outset (2 device px), a linear pass's plain box covers
    // pixel centers and its bounds reach half a pixel past it; both stop at the outset past the clip, and a box clipped
    // away covers nothing.
    void bounds() {
        paint::DisplayList list;
        list.boxes.emplace_back().background = {1, 0, 0, 1};
        auto box = item(Kind::Box);
        box.transform = {1, 0, 0, 1, 10.5f, 20.25f};
        box.width = 30;
        box.height = 40;
        box.clip = {0, 0, 100, 50};
        list.items = {box, box};
        list.items[1].clip = {200, 200, 300, 300};
        const auto encoded = weft::render::plan_scene_pass(list, 2, 400, 400).value();
        require(encoded.records[0].bounds == std::array<float, 4>{9.5f, 19.25f, 32, 31.75f}, "encoded box bounds");
        require(encoded.records[1].bounds[2] == 0 && encoded.records[1].bounds[3] == 0, "clipped box bounds");
        const auto linear =
            weft::render::plan_scene_pass(list, 2, 400, 400, std::nullopt, weft::render::Space::Linear).value();
        require(linear.records[0].bounds == std::array<float, 4>{10.25f, 20, 30.5f, 30.25f}, "linear box bounds");
        list.boxes[0].radii = {4, 4, 4, 4};
        const auto rounded =
            weft::render::plan_scene_pass(list, 2, 400, 400, std::nullopt, weft::render::Space::Linear).value();
        require(rounded.records[0].bounds == encoded.records[0].bounds, "rounded linear box keeps its outset");
    }

    void scene_pass() {
        using Step = weft::render::ScenePass::Step;
        paint::DisplayList list;
        paint::Box background;
        background.background = {.25f, .5f, .75f, 1};
        list.boxes.push_back(background);
        paint::Box shape;
        shape.gradient.kind = paint::GradientKind::Radial;
        shape.gradient.angle = 30;
        shape.gradient.tile = {10, 20, 1, 2};
        shape.gradient.stops = {{0, {1, 0, 0, 1}}, {.5f, {0, 1, 0, 1}}, {1, {0, 0, 1, 1}}};
        list.boxes.push_back(shape);
        list.rasters.push_back({std::make_shared<weft::SceneNode>(), true, {1, 1, 0, 1}});
        auto box = [](std::uint32_t payload, float w, float h) {
            auto i = item(Kind::Box, payload);
            i.width = w;
            i.height = h;
            i.clip = {0, 0, w, h};
            return i;
        };
        list.items = {box(0, 50, 40), box(1, 10, 20),       item(Kind::Text), item(Kind::BeginGroup),
                      box(1, 4, 4),   item(Kind::EndGroup), item(Kind::Layer)};
        list.items[5].opacity = .5f;
        require(bool(paint::check(list, 1)), "planned list is well formed");
        const auto pass = weft::render::plan_scene_pass(list, 2, 100, 80).value();
        const std::vector<std::pair<Step, std::uint32_t>> expected{{Step::Boxes, 1},      {Step::Image, 2},
                                                                   {Step::BeginGroup, 3}, {Step::Boxes, 4},
                                                                   {Step::EndGroup, 5},   {Step::Image, 6}};
        require(pass.records.size() == list.items.size() && pass.batches.size() == expected.size() &&
                    pass.group_depth == 1 && pass.clear == background.background,
                "scene pass shape");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require(pass.batches[i].step == expected[i].first && pass.batches[i].first == expected[i].second,
                    "scene pass step " + std::to_string(i));
        }
        require(pass.batches[0].count == 1 && pass.batches[3].count == 1, "box runs");
        const auto& g = pass.records[1];
        require(g.size == std::array<float, 4>{10, 20, 2, 3} && g.tile == shape.gradient.tile && g.extra[0] == 30 &&
                    g.offsets[0] == std::array<float, 4>{0, .5f, 1, 0} && g.stops[2] == shape.gradient.stops[2].color,
                "gradient record");
        require(pass.records[2].background == list.rasters[0].tint && pass.records[2].extra[3] == 1 &&
                    pass.records[2].extra[2] == 1,
                "tinted mask record");
        require(pass.records[5].size == std::array<float, 4>{50, 40, 0, 0} && pass.records[5].extra[1] == .5f,
                "group composite covers the target");
        // A crop inside the target keeps the first box as a drawn, clipped item.
        const auto cropped = weft::render::plan_scene_pass(list, 2, 100, 80, paint::ClipRect{5, 5, 45, 35}).value();
        require(cropped.clear == paint::Color{} && cropped.batches[0].step == Step::Boxes &&
                    cropped.batches[0].first == 0 && cropped.batches[0].count == 2 &&
                    cropped.records[0].clip == paint::ClipRect{5, 5, 45, 35},
                "crop keeps the first box");
        // Not covering the target at this scale: no clear.
        require(weft::render::plan_scene_pass(list, 1, 100, 80).value().clear == paint::Color{}, "partial first box");
        // Images: a coverage mask and signed distances, tinted, in either space.
        auto images = list;
        images.assets.push_back(std::make_shared<paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {255}}));
        images.assets.push_back(
            std::make_shared<paint::Asset>(paint::Asset{1, 1, paint::Encoding::SignedDistance, {128}, 6}));
        paint::Image mask;
        mask.uv = {.25f, .5f, .5f, .25f};
        mask.color = {.5f, 1, 0, .5f};
        images.images.push_back(mask);
        paint::Image distance;
        distance.asset = 1;
        distance.stroke = 2;
        distance.outline = {0, 0, 1, 1};
        images.images.push_back(distance);
        images.items[1] = item(Kind::Image);
        images.items[4] = item(Kind::Image, 1);
        images.items[4].width = images.items[4].height = 4;
        require(bool(paint::check(images, 1)), "image list is well formed");
        const auto encoded = weft::render::plan_scene_pass(images, 2, 100, 80).value();
        const auto& m = encoded.records[1];
        require(encoded.batches[0].step == Step::Image && encoded.batches[0].first == 1 && m.extra[2] == 1 &&
                    m.mode[0] == 1 && m.uv == mask.uv && m.background == mask.color,
                "coverage image record");
        const auto& d = encoded.records[4];
        // One texel over 4 CSS px at 2 device px per CSS px: an eighth of a texel per device pixel.
        require(d.mode == std::array<float, 4>{2, 6, 2, .125f} && d.outline == distance.outline,
                "distance image record");
        const auto linear =
            weft::render::plan_scene_pass(images, 2, 100, 80, std::nullopt, weft::render::Space::Linear).value();
        require(linear.space == weft::render::Space::Linear &&
                    linear.records[1].background ==
                        paint::Color{weft::render::srgb_to_linear(.5f) * .5f, .5f, 0, .5f} &&
                    linear.clear == paint::Color{weft::render::srgb_to_linear(.25f), weft::render::srgb_to_linear(.5f),
                                                 weft::render::srgb_to_linear(.75f), 1},
                "linear colors");
        // Rounded clips are normalized and referenced by range; an invalid one refuses the plan.
        auto clipped = list;
        clipped.clips.push_back({{0, 0, 10, 10}, {8, 8, 8, 8}, {8, 8, 8, 8}});
        clipped.items[4].clips_count = 1;
        const auto planned = weft::render::plan_scene_pass(clipped, 2, 100, 80).value();
        require(planned.clips.size() == 1 && planned.clips[0].radius_x == std::array<float, 4>{5, 5, 5, 5} &&
                    planned.records[4].clips == std::array<float, 4>{0, 1, 0, 0},
                "rounded clip plan");
        clipped.clips[0].rect[2] = -1;
        const auto refused = weft::render::plan_scene_pass(clipped, 2, 100, 80);
        require(!refused && refused.error().code == weft::Diag::PaintList, "invalid rounded clip");
        auto chained = clipped;
        chained.clips.assign(weft::render::scene_limits::max_rounded_clips + 1, {{0, 0, 10, 10}, {}, {}});
        chained.items[4].clips_count = std::uint32_t(chained.clips.size());
        const auto chain = weft::render::plan_scene_pass(chained, 2, 100, 80);
        require(!chain && chain.error().code == weft::Diag::PaintList &&
                    chain.error().message.find("rounded clip chain bound") != std::string::npos,
                "rounded clip chain bound");
        // A clip in its own space records the rows of its map from target px and how many target px its unit spans;
        // an identity map records the identity and 1. A singular map refuses the plan and the list.
        auto turned = clipped;
        turned.clips[0] = {{0, 0, 10, 10}, {}, {}, {0, -.5f, .5f, 0, 0, 10}};
        const auto local = weft::render::plan_scene_pass(turned, 2, 100, 80).value();
        require(local.clips[0].inv_x == std::array<float, 4>{0, .5f, 0, 2} &&
                    local.clips[0].inv_y == std::array<float, 4>{-.5f, 0, 10, 0} &&
                    planned.clips[0].inv_x == std::array<float, 4>{1, 0, 0, 1} &&
                    planned.clips[0].inv_y == std::array<float, 4>{0, 1, 0, 0},
                "clip map record");
        turned.clips[0].to_local = {1, 2, 2, 4, 0, 0};
        const auto singular = weft::render::plan_scene_pass(turned, 2, 100, 80);
        require(!singular && singular.error().code == weft::Diag::PaintList, "singular clip map");
        require(rejected(turned, 1, "rounded clip map at item 4"), "singular clip map in the list");
        // A rotated clip contains what lies inside it in its own space: a quarter turn about the origin.
        const weft::RoundedClip quarter{{0, 0, 10, 4}, {}, {}, {0, 1, -1, 0, 0, 0}};
        require(weft::contains_rounded(quarter, 2, -5) && !weft::contains_rounded(quarter, 5, 2),
                "a rotated clip contains in its own space");
    }

    // A plane draws a nested list flat, then through its homography. The check takes a well-formed content list nested
    // at most max_plane_depth deep, a positive size and an invertible homography in front of the viewer; append
    // composes the homography with its transform; the scene pass records the inverse of the content image's map, its
    // depth row in outline, and bounds the image's projected corners.
    void planes() {
        // perspective(200px) rotateY(30deg) about the plane's origin, which lies at (10, 20) of the target.
        const float c = std::cos(std::numbers::pi_v<float> / 6);
        const float s = std::sin(std::numbers::pi_v<float> / 6);
        paint::Plane plane;
        plane.content = std::make_shared<const paint::DisplayList>(nested());
        plane.transform = {c, 0, 10, 0, 1, 20, s / 200, 0, 1};
        plane.width = 40;
        plane.height = 30;
        const auto with = [](const paint::Plane& p) {
            paint::DisplayList l;
            l.planes.push_back(p);
            auto i = item(Kind::Plane, 0);
            i.width = p.width;
            i.height = p.height;
            i.clip = {0, 0, 100, 80};
            l.items = {i};
            return l;
        };
        const auto map = [](const std::array<float, 9>& h, double x, double y) {
            const double w = h[6] * x + h[7] * y + h[8];
            return std::array<double, 2>{(h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w};
        };
        require(bool(paint::check(with(plane), 2)), "plane item");
        auto list = with(plane);
        list.items[0].payload = 1;
        require(rejected(list, 2, "plane payload out of range at item 0"), "plane payload range");
        auto broken = plane;
        broken.content.reset();
        require(rejected(with(broken), 2, "plane value at item 0"), "plane without content");
        broken = plane;
        broken.transform[4] = std::numeric_limits<float>::quiet_NaN();
        require(rejected(with(broken), 2, "plane value at item 0"), "plane homography not finite");
        broken = plane;
        broken.width = 0;
        require(rejected(with(broken), 2, "plane value at item 0"), "empty plane");
        broken = plane;
        broken.transform = {1, 2, 0, 2, 4, 0, 0, 0, 1};
        require(rejected(with(broken), 2, "plane behind the viewer or singular at item 0"), "singular plane");
        // Depth falls to zero at x = 20 of the 40 px plane.
        broken = plane;
        broken.transform = {1, 0, 0, 0, 1, 0, -1.f / 20, 0, 1};
        require(rejected(with(broken), 2, "plane behind the viewer or singular at item 0"), "plane behind the viewer");
        broken = plane;
        auto unclosed = nested();
        unclosed.items.pop_back();
        broken.content = std::make_shared<const paint::DisplayList>(unclosed);
        require(rejected(with(broken), 2, "unclosed group"), "plane content is checked");
        // Planes nest in plane contents two deep.
        auto inner = plane;
        inner.content = std::make_shared<const paint::DisplayList>(with(plane));
        require(bool(paint::check(with(inner), 2)), "plane in a plane");
        auto deepest = plane;
        deepest.content = std::make_shared<const paint::DisplayList>(with(inner));
        require(rejected(with(deepest), 2, "plane depth bound at item 0"), "plane depth bound");
        // Appended under a transform, the plane maps each of its points where the transform takes them.
        paint::DisplayList into;
        into.boxes.emplace_back();
        into.planes.push_back(plane);
        into.items = {item(Kind::Box), item(Kind::Plane, 0)};
        require(bool(paint::append(into, with(plane), {2, 0, 0, 3, 100, 200})) && bool(paint::check(into, 2)) &&
                    into.items[2].kind == Kind::Plane && into.items[2].payload == 1 &&
                    into.planes[1].content == plane.content,
                "appended plane");
        const auto before = map(plane.transform, 25, 10);
        const auto after = map(into.planes[1].transform, 25, 10);
        require(std::abs(after[0] - (2 * before[0] + 100)) < 1e-3 && std::abs(after[1] - (3 * before[1] + 200)) < 1e-3,
                "appended plane homography");
        // The scene pass: one image step; the content image is the plane with 2 px margins, 2 device px per CSS px.
        using Step = weft::render::ScenePass::Step;
        require(weft::render::plan_scene_pass(with(plane), 2, 100, 80).value().batches.size() == 1 &&
                    weft::render::plane_pixels(plane, 2) == std::array<std::uint32_t, 2>{88, 68},
                "plane image pixels");
        const auto pass = weft::render::plan_scene_pass(with(plane), 2, 100, 80).value();
        const auto& r = pass.records[0];
        require(pass.batches[0].step == Step::Image && pass.batches[0].first == 0 && r.mode[0] == 3 &&
                    r.extra[2] == 1 && r.extra[3] == 0 && r.size == std::array<float, 4>{44, 34, 0, 0},
                "plane record");
        // The shader's division takes a target point back to the image point that projects there: content point
        // (25, 10) lies at (27, 12) of the image.
        const auto target = map(plane.transform, 25, 10);
        const auto row = [&](const std::array<float, 4>& v) {
            return v[0] * target[0] + v[1] * target[1] + v[2];
        };
        const double depth = row(r.outline);
        require(depth > 0 && std::abs(row(r.inv_x) / depth - 27) < 1e-3 && std::abs(row(r.inv_y) / depth - 12) < 1e-3,
                "plane record inverts the image's map");
        // The bounds hold the image's projected corners, the margins included.
        for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{{{-2, -2}, {42, -2}, {-2, 32}, {42, 32}}}) {
            const auto p = map(plane.transform, x, y);
            require(p[0] >= r.bounds[0] && p[1] >= r.bounds[1] && p[0] <= r.bounds[0] + r.bounds[2] &&
                        p[1] <= r.bounds[1] + r.bounds[3],
                    "plane bounds hold the image corners");
        }
        // A plane in front of the viewer whose image margin is not: the list is well formed, the pass refuses it.
        broken = plane;
        broken.transform = {1, 0, 0, 0, 1, 0, -1.f / 40.5f, 0, 1};
        const auto edge = weft::render::plan_scene_pass(with(broken), 2, 100, 80);
        require(bool(paint::check(with(broken), 2)) && !edge && edge.error().code == weft::Diag::PaintList &&
                    edge.error().message.find("plane image behind the viewer") != std::string::npos,
                "plane image behind the viewer");
    }

    // Quads become items in order: boxes, tinted images and external textures; the group diagnostic is an isolated
    // group of red and blue, the clipped diagnostic a clip inset into its quad.
    void quads() {
        using weft::render::Draw;
        weft::render::FramePacket packet;
        packet.assets.push_back(
            std::make_shared<const paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {9}}));
        packet.draws.push_back({{1, 2, 3, 4}, {.5f, .25f, 0, 1}});
        packet.draws.push_back({{0, 0, 8, 8}, {1, 0, 0, .5f}, 0, false, false, .75f, {.25f, 0, .5f, 1}});
        packet.draws.push_back({{8, 8, 4, 4}, {1, 1, 1, 1}, 1});
        packet.draws.push_back({{20, 20, 40, 40}, {1, 1, 1, 1}, -1, true, false, .5f});
        packet.draws.push_back({{40, 40, 40, 40}, {0, 1, 0, 1}, -1, false, true});
        const auto list = weft::render::quad_list(packet, 1, 100, 80);
        require(bool(paint::check(list, 1)) && list.assets == packet.assets && list.external_textures == 1,
                "quad list is well formed");
        require(list.items.size() == 8 && list.items[0].kind == Kind::Box &&
                    list.items[0].transform == paint::Affine{1, 0, 0, 1, 1, 2} && list.items[0].width == 3 &&
                    list.items[0].height == 4 && list.items[0].clip == paint::ClipRect{0, 0, 100, 80} &&
                    list.boxes[list.items[0].payload].background == paint::Color{.5f, .25f, 0, 1},
                "solid quad box");
        const auto& tinted = list.images[list.items[1].payload];
        require(list.items[1].kind == Kind::Image && list.items[1].opacity == .75f && tinted.asset == 0 &&
                    tinted.color == paint::Color{1, 0, 0, .5f} && tinted.uv == std::array<float, 4>{.25f, 0, .5f, 1},
                "textured quad image");
        require(list.images[list.items[2].payload].asset == 1, "external texture after the assets");
        require(list.items[3].kind == Kind::BeginGroup && list.items[6].kind == Kind::EndGroup &&
                    list.items[6].opacity == .5f && list.items[4].opacity == 1 && list.items[5].opacity == 1 &&
                    list.boxes[list.items[4].payload].background == paint::Color{1, 0, 0, 1} &&
                    list.boxes[list.items[5].payload].background == paint::Color{0, 0, 1, 1},
                "group diagnostic");
        require(list.items[7].clip == paint::ClipRect{56, 56, 64, 64}, "clipped diagnostic");
    }

    // External textures are image assets past the leased ones, drawn with their own colors and no distance stroke.
    void external_textures() {
        paint::DisplayList list;
        list.assets.push_back(std::make_shared<const paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {9}}));
        list.images.resize(2);
        list.images[1].asset = 1;
        list.items = {item(Kind::Image, 0), item(Kind::Image, 1)};
        require(rejected(list, 1, "image payload or asset out of range"), "external texture beyond the count");
        list.external_textures = 1;
        require(bool(paint::check(list, 1)), "external texture within the count");
        list.images[1].stroke = 1;
        require(rejected(list, 1, "external texture with a distance stroke"), "external texture stroke");
        list.images[1].stroke = 0;
        list.images[1].uv = {0, .25f, 1, .5f};
        for (auto& i : list.items) {
            i.width = i.height = 10;
            i.clip = {0, 0, 10, 10};
        }
        auto planned = weft::render::plan_scene_pass(list, 1, 10, 10, std::nullopt, weft::render::Space::Linear);
        require(planned && planned->records[0].mode[0] == weft::render::scene_limits::image_mode_coverage &&
                    planned->records[1].mode == std::array<float, 4>{},
                "external texture is a color image");
        weft::render::flip_rows(planned->records[1]);
        require(planned->records[1].uv == std::array<float, 4>{0, .75f, 1, -.5f}, "bottom-up rows flip the uv");
    }

    // An appended list follows under its transform: payloads, assets, rounded clips and external textures renumber,
    // and the destination's external textures move past the new assets.
    void append() {
        paint::DisplayList into;
        into.external_textures = 1;
        into.images.emplace_back().asset = 0;
        into.items = {item(Kind::Image, 0)};
        paint::DisplayList from;
        from.assets.push_back(std::make_shared<const paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {9}}));
        from.external_textures = 1;
        from.boxes.emplace_back().background = {0, 1, 0, 1};
        from.images.resize(2);
        from.images[1].asset = 1;
        from.clips.push_back({{1, 2, 3, 4}, {1, 1, 1, 1}, {2, 2, 2, 2}});
        auto box = item(Kind::Box, 0);
        box.transform = {1, 0, 0, 1, 5, 6};
        box.clip = {0, 0, 10, 20};
        box.clips_count = 1;
        from.items = {box, item(Kind::Image, 0), item(Kind::Image, 1)};
        require(bool(paint::append(into, from, {2, 0, 0, 3, 100, 200})), "append");
        require(bool(paint::check(into, 1)) && into.assets == from.assets && into.external_textures == 2,
                "appended list is well formed");
        require(into.images[0].asset == 1 && into.images[1].asset == 0 && into.images[2].asset == 2,
                "assets, then each list's external textures");
        const auto& moved = into.items[1];
        require(moved.transform == paint::Affine{2, 0, 0, 3, 110, 218} &&
                    moved.clip == paint::ClipRect{100, 200, 120, 260} && moved.clips_begin == 0 &&
                    into.clips[0].rect == std::array<float, 4>{102, 206, 6, 12} &&
                    into.clips[0].radius_x == std::array<float, 4>{2, 2, 2, 2} &&
                    into.clips[0].radius_y == std::array<float, 4>{6, 6, 6, 6},
                "transform and clips map");
        require(into.items[2].payload == 1 && into.items[3].payload == 2 &&
                    into.boxes[into.items[1].payload].background == paint::Color{0, 1, 0, 1},
                "payloads renumber");
        const auto status = paint::append(into, from, {1, 1, 0, 1, 0, 0});
        require(!status && status.error().code == weft::Diag::PaintList, "skewed append");
        // A clip in its own space keeps its rectangle; its map composes with the transform's inverse, so a point
        // that moves with the transform lands where it did.
        paint::DisplayList turning;
        turning.boxes.emplace_back();
        turning.clips.push_back({{0, 0, 10, 4}, {}, {}, {0, 1, -1, 0, 3, 7}});
        auto inside = item(Kind::Box, 0);
        inside.clips_count = 1;
        turning.items = {inside};
        paint::DisplayList moved_into;
        require(bool(paint::append(moved_into, turning, {2, 0, 0, 3, 100, 200})), "append a clip in its own space");
        const auto& before = turning.clips[0].to_local;
        const auto& after = moved_into.clips[0].to_local;
        const float px = 4;
        const float py = -1;
        const float qx = 2 * px + 100;
        const float qy = 3 * py + 200;
        require(moved_into.clips[0].rect == turning.clips[0].rect &&
                    std::abs(qx * after[0] + qy * after[2] + after[4] - (px * before[0] + py * before[2] + before[4])) <
                        1e-4f &&
                    std::abs(qx * after[1] + qy * after[3] + after[5] - (px * before[1] + py * before[3] + before[5])) <
                        1e-4f,
                "appended clip map composes with the transform");
        // Shadows renumber after the destination's and keep their item-px values.
        paint::DisplayList shaded;
        shaded.shadows.push_back({{0, 0, 0, 1}, 1, 1, 0, 0, {}});
        shaded.items = {item(Kind::Shadow, 0)};
        paint::DisplayList more;
        more.shadows.push_back({{1, 0, 0, 1}, 3, 4, 5, 6, {1, 2, 3, 4}});
        more.items = {item(Kind::Shadow, 0)};
        require(bool(paint::append(shaded, more, {2, 0, 0, 2, 0, 0})) && bool(paint::check(shaded, 1)) &&
                    shaded.items[1].payload == 1 && shaded.shadows[1].blur == 5 && shaded.shadows[1].x == 3 &&
                    shaded.shadows[1].radii == std::array<float, 4>{1, 2, 3, 4},
                "appended shadows");
        // Paths renumber after the destination's and keep their points in item px.
        auto path = std::make_shared<paint::Path>();
        path->verbs = {paint::Path::Verb::Move, paint::Path::Verb::Line};
        path->points = {{0, 0}, {5, 5}};
        paint::DisplayList drawn;
        drawn.paths.push_back({path, {1, 0, 0, 1}, std::nullopt});
        drawn.items = {item(Kind::Path, 0)};
        paint::DisplayList stroked;
        stroked.paths.push_back({path, {0, 0, 1, 1}, paint::Stroke{}});
        stroked.items = {item(Kind::Path, 0)};
        require(bool(paint::append(drawn, stroked, {2, 0, 0, 2, 0, 0})) && bool(paint::check(drawn, 1)) &&
                    drawn.items[1].payload == 1 && drawn.paths[1].stroke && drawn.paths[1].path->points[1][0] == 5 &&
                    drawn.items[1].transform == paint::Affine{2, 0, 0, 2, 0, 0},
                "appended paths");
    }
} // namespace

// Effects: a leased program with at most its parameters and exactly its textures, coverage or sRGB assets, finite
// values and non-negative corners; appended lists renumber programs and textures; the plan carries the time, the
// parameters in the record fields effects leave unused and the corners, and draws a run of effects of one program
// and the same textures at once.
void effects() {
    auto program = std::make_shared<paint::EffectProgram>();
    program->name = "test";
    program->parameters = 3;
    program->textures = 1;
    const auto data = std::make_shared<const paint::Asset>(paint::Asset{2, 2, paint::Encoding::Coverage, {1, 2, 3, 4}});
    const auto distances =
        std::make_shared<const paint::Asset>(paint::Asset{2, 2, paint::Encoding::SignedDistance, {1, 2, 3, 4}, 4});
    const auto effect_item = [](std::uint32_t payload) {
        auto e = item(Kind::Effect, payload);
        e.width = 10;
        e.height = 8;
        e.clip = {0, 0, 100, 100};
        return e;
    };
    const auto make = [&] {
        paint::DisplayList list;
        list.assets = {data, distances};
        list.programs = {program};
        list.effects.push_back({0, 1.5f, {{1, 2, 3, 4}, {5, 6, 7, 8}}, {0}, {1, 2, 3, 4}});
        list.items.push_back(effect_item(0));
        return list;
    };
    require(bool(paint::check(make(), 4)), "a well-formed effect");
    const auto refused = [&](auto&& change, const char* what) {
        auto list = make();
        change(list);
        require(!paint::check(list, 4), what);
    };
    refused(
        [](paint::DisplayList& l) {
            l.items[0].payload = 1;
        },
        "effect payload out of range");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].program = 1;
        },
        "effect program out of range");
    refused(
        [](paint::DisplayList& l) {
            l.programs[0] = nullptr;
        },
        "effect without a program");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].parameters.resize(4);
        },
        "more parameters than the program reads");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].textures.clear();
        },
        "fewer textures than the program reads");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].textures = {2};
        },
        "effect texture out of range");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].textures = {1};
        },
        "signed distances as an effect texture");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].time = std::numeric_limits<float>::quiet_NaN();
        },
        "non-finite effect time");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].parameters[1][2] = std::numeric_limits<float>::infinity();
        },
        "non-finite effect parameter");
    refused(
        [](paint::DisplayList& l) {
            l.effects[0].radii[3] = -1;
        },
        "negative effect corner");

    // Appended: from's program follows into's, and its texture follows into's assets.
    auto into = make();
    require(bool(paint::append(into, make())), "append effects");
    require(into.programs.size() == 2 && into.effects.size() == 2 && into.items[1].payload == 1 &&
                into.effects[1].program == 1 && into.effects[1].textures == std::vector<std::uint32_t>{2} &&
                bool(paint::check(into, 4)),
            "appended effects renumbered");

    // The plan of every parameter: the fields effects leave unused, in the order the shader reads them.
    auto all = std::make_shared<paint::EffectProgram>();
    all->parameters = 16;
    paint::DisplayList list;
    list.assets = {data, std::make_shared<const paint::Asset>(paint::Asset{1, 1, paint::Encoding::Coverage, {9}})};
    list.programs = {all};
    paint::Effect every{0, 2.25f, {}, {}, {0, 1, 2, 3}};
    for (unsigned p = 0; p < 16; ++p) {
        every.parameters.push_back({float(p), 0, 0, 1});
    }
    list.effects = {every, every};
    auto other = std::make_shared<paint::EffectProgram>();
    other->textures = 1;
    list.programs.push_back(program);
    list.programs.push_back(other);
    list.effects.push_back({1, 0, {}, {0}, {}});
    list.effects.push_back({1, 0, {}, {1}, {}});
    list.effects.push_back({2, 0, {}, {1}, {}});
    list.boxes.emplace_back();
    list.items = {effect_item(0), effect_item(1), item(Kind::Box), effect_item(2), effect_item(3), effect_item(4)};
    list.items[2].width = 4;
    list.items[2].height = 4;
    list.items[2].clip = {0, 0, 100, 100};
    require(bool(paint::check(list, 4)), "a list of effects and a box");
    const auto pass = weft::render::plan_scene_pass(list, 1, 100, 100).value();
    const auto& r = pass.records[0];
    const std::array<const weft::render::SceneRecord::Vector*, 16> fields{
        &r.stops[0], &r.stops[1], &r.stops[2], &r.stops[3], &r.stops[4],   &r.stops[5],   &r.stops[6], &r.stops[7],
        &r.edges[0], &r.edges[1], &r.edges[2], &r.edges[3], &r.offsets[0], &r.offsets[1], &r.tile,     &r.background};
    for (unsigned p = 0; p < 16; ++p) {
        require((*fields[p])[0] == float(p), "effect parameter in its record field");
    }
    require(r.extra[0] == 2.25f && r.extra[2] == weft::render::scene_limits::flag_enabled &&
                r.radii == std::array<float, 4>{0, 1, 2, 3} && r.size[0] == 10 && r.size[1] == 8,
            "effect time, image geometry and corners in the record");
    using Step = weft::render::ScenePass::Step;
    const auto& b = pass.batches;
    require(b.size() == 5 && b[0].step == Step::Effect && b[0].first == 0 && b[0].count == 2 && b[0].source == 0 &&
                b[1].step == Step::Boxes && b[1].first == 2 && b[1].count == 1 && b[2].step == Step::Effect &&
                b[2].first == 3 && b[2].count == 1 && b[2].source == 2 && b[3].step == Step::Effect &&
                b[3].first == 4 && b[3].count == 1 && b[3].source == 3 && b[4].step == Step::Effect &&
                b[4].first == 5 && b[4].count == 1 && b[4].source == 4,
            "a run of effects of one program and the same textures is one draw; other textures or another program "
            "start another");
}

// UI particles: what the checks refuse of an emitter and an emission, append's renumbering, and the plan of an emission:
// one record holding it in particle_field order, and one draw of the particles it may have alive, none for none.
void particles() {
    const auto sprite = std::make_shared<const paint::Asset>(
        paint::Asset{4, 4, paint::Encoding::SrgbStraight, std::vector<std::uint8_t>(64, 255)});
    const auto distances =
        std::make_shared<const paint::Asset>(paint::Asset{2, 2, paint::Encoding::SignedDistance, {1, 2, 3, 4}, 4});
    const auto make = [&] {
        paint::DisplayList list;
        list.assets = {sprite, distances};
        paint::Particles p;
        p.emitter.shape = paint::ParticleEmitter::Shape::Circle;
        p.emitter.area = {10, 5, 4, 0};
        p.emitter.rate = 10;
        p.emitter.life = {1, 2};
        p.emitter.speed = {5, 10};
        p.emitter.direction = -90;
        p.emitter.spread = 40;
        p.emitter.gravity = {1, 30};
        p.emitter.drag = .5f;
        p.emitter.angle = {-10, 10};
        p.emitter.spin = {0, 90};
        p.emitter.size = {2, 4, 6, 1};
        p.emitter.scale = {.5f, 1.5f};
        p.emitter.color = {paint::Color{1, .5f, .25f, 1}, paint::Color{1, .4f, .2f, .8f},
                           paint::Color{.5f, .2f, .1f, .5f}, paint::Color{.2f, .1f, 0, 0}};
        p.emitter.softness = .5f;
        p.emission.elapsed = 3.25;
        p.emission.seed = 0x12345678u;
        list.particles.push_back(p);
        auto i = item(Kind::Particles);
        i.width = 20;
        i.height = 10;
        i.clip = {0, 0, 100, 100};
        i.transform = {2, 0, 0, 2, 5, 7};
        list.items.push_back(i);
        return list;
    };
    require(bool(paint::check(make(), 4)), "a well-formed emission");
    const auto refused = [&](auto&& change, const char* reason, const char* what) {
        auto list = make();
        change(list.particles[0]);
        require(rejected(list, 4, reason), what);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    refused(
        [](paint::Particles& p) {
            p.emitter.life = {0, 1};
        },
        "particle emitter range", "a lifetime of 0");
    refused(
        [](paint::Particles& p) {
            p.emitter.life = {2, 1};
        },
        "particle emitter range", "a life range backwards");
    refused(
        [](paint::Particles& p) {
            p.emitter.speed = {-1, 1};
        },
        "particle emitter range", "a negative speed");
    refused(
        [](paint::Particles& p) {
            p.emitter.spin = {5, 1};
        },
        "particle emitter range", "a spin range backwards");
    refused(
        [](paint::Particles& p) {
            p.emitter.drag = -1;
        },
        "particle emitter range", "a negative drag");
    refused(
        [](paint::Particles& p) {
            p.emitter.rate = -1;
        },
        "particle emitter range", "a negative rate");
    refused(
        [](paint::Particles& p) {
            p.emitter.softness = 1.5f;
        },
        "particle emitter range", "softness past 1");
    refused(
        [](paint::Particles& p) {
            p.emitter.size[2] = -1;
        },
        "particle emitter range", "a negative size");
    refused(
        [](paint::Particles& p) {
            p.emitter.area[2] = -1;
        },
        "particle emitter range", "a negative area");
    refused(
        [](paint::Particles& p) {
            p.emitter.frame_rate = -1;
        },
        "particle emitter range", "frames backwards");
    refused(
        [](paint::Particles& p) {
            p.emitter.color[1][3] = 2;
        },
        "particle emitter value", "alpha past 1");
    refused(
        [](paint::Particles& p) {
            p.emitter.color[2][0] = -1;
        },
        "particle emitter value", "a negative channel");
    refused(
        [&](paint::Particles& p) {
            p.emitter.gravity[0] = nan;
        },
        "particle emitter value", "a NaN");
    refused(
        [](paint::Particles& p) {
            p.emitter.sprite = 5;
        },
        "particle sprite", "a sprite out of range");
    refused(
        [](paint::Particles& p) {
            p.emitter.sprite = 1;
        },
        "particle sprite", "a distance field as a sprite");
    refused(
        [](paint::Particles& p) {
            p.emitter.sprite = 0;
            p.emitter.frames = {0, 1};
        },
        "particle sprite", "a sprite without frames");
    refused(
        [](paint::Particles& p) {
            p.emitter.sprite = 0;
            p.emitter.frames = {64, 32};
        },
        "particle sprite", "more frames than the bound");
    refused(
        [](paint::Particles& p) {
            p.emission.elapsed = -1;
        },
        "particle emission value", "a negative time");
    refused(
        [](paint::Particles& p) {
            p.emission.stopped = 4;
        },
        "particle emission value", "a stop after the time");
    refused(
        [&](paint::Particles& p) {
            p.emission.target = std::array<float, 2>{nan, 0};
        },
        "particle emission value", "a target at NaN");
    refused(
        [](paint::Particles& p) {
            p.emission.kind = paint::ParticleEmission::Kind::Burst;
            p.emission.count = 0;
        },
        "particle emission count", "an empty burst");
    refused(
        [](paint::Particles& p) {
            p.emission.kind = paint::ParticleEmission::Kind::Burst;
            p.emission.count = weft::constants::max_particles_per_emission + 1;
        },
        "particle emission count", "a burst past the bound");
    refused(
        [](paint::Particles& p) {
            p.emitter.rate = 40000;
        },
        "particle emission count", "a stream that may have more particles alive than the bound");
    {
        auto list = make();
        list.items[0].payload = 1;
        require(rejected(list, 4, "particles payload out of range"), "particles payload out of range");
    }

    // Appended: from's payloads follow into's, and its sprite follows into's assets.
    auto into = make();
    auto from = make();
    from.particles[0].emitter.sprite = 0;
    require(bool(paint::append(into, from)), "append particles");
    require(into.particles.size() == 2 && into.items[1].payload == 1 && into.particles[1].emitter.sprite == 2u &&
                !into.particles[0].emitter.sprite && bool(paint::check(into, 4)),
            "appended particles renumbered");

    // The plan: one record in particle_field order, one draw of the particles the stream may have alive.
    auto list = make();
    auto& p = list.particles[0];
    p.emitter.sprite = 0;
    p.emitter.frames = {2, 2};
    p.emitter.frame_rate = 6;
    p.emission.target = std::array<float, 2>{40, 50};
    require(bool(paint::check(list, 4)), "a sprite flying to a target");
    const auto timing = paint::particle_timing(p);
    // 33 emitted by 3.25 s at 10 per second, at most 21 of them alive (2 s lives): the newest is 50 ms old.
    require(timing.count == 21 && timing.newest == 32 && std::abs(timing.age - .05f) < 1e-6f &&
                std::abs(timing.interval - .1f) < 1e-7f,
            "a stream's timing");
    const auto pass = weft::render::plan_scene_pass(list, 2, 200, 200).value();
    using Step = weft::render::ScenePass::Step;
    require(pass.batches.size() == 1 && pass.batches[0].step == Step::Particles && pass.batches[0].first == 0 &&
                pass.batches[0].count == 21 && pass.batches[0].source == 0,
            "one draw of the particles alive");
    auto r = pass.records[0];
    using weft::render::particle_field;
    namespace f = weft::render;
    using V = weft::render::SceneRecord::Vector;
    require(particle_field(r, f::ParticleMapX) == V{2, 0, 5, 0} && particle_field(r, f::ParticleMapY) == V{0, 2, 7, 0},
            "the item's map into target px");
    require(particle_field(r, f::ParticleTiming) == V{timing.age, timing.interval, 32, 0} &&
                particle_field(r, f::ParticleEmission) ==
                    V{21, 0, float(0x12345678u % weft::constants::particle_index_period), 2},
            "timing, count, seed and shape");
    require(particle_field(r, f::ParticleArea) == V{10, 5, 4, 0} &&
                particle_field(r, f::ParticleLifeSpeed) == V{1, 2, 5, 10} &&
                particle_field(r, f::ParticleHeading) == V{-90, 40, .5f, .5f} &&
                particle_field(r, f::ParticleForces) == V{1, 30, 0, 90} &&
                particle_field(r, f::ParticleTurn) == V{-10, 10, .5f, 1.5f} &&
                particle_field(r, f::ParticleSize) == V{2, 4, 6, 1},
            "the emitter's ranges and sizes");
    for (unsigned k = 0; k < 4; ++k) {
        require(particle_field(r, f::ParticleField(f::ParticleColor + k)) == p.emitter.color[k],
                "the colors, straight and linear");
    }
    require(particle_field(r, f::ParticleSprite) == V{2, 2, 6, weft::render::scene_limits::particle_sprite_color} &&
                particle_field(r, f::ParticleTarget) == V{40, 50, weft::render::scene_limits::flag_enabled, 0} &&
                r.uv == V{0, 0, 1, 1},
            "the sprite, its frames and the target");
    // A burst draws its count; a stream that emits nothing draws nothing.
    p.emission.kind = paint::ParticleEmission::Kind::Burst;
    p.emission.count = 7;
    require(weft::render::plan_scene_pass(list, 2, 200, 200).value().batches.at(0).count == 7, "a burst's count");
    p.emission.kind = paint::ParticleEmission::Kind::Stream;
    p.emitter.rate = 0;
    require(weft::render::plan_scene_pass(list, 2, 200, 200).value().batches.empty(), "no particles, no draw");
}

// Backdrop filters: what the checks refuse, append's renumbering, and the plan of a backdrop: a record of the box's shape
// with its matrix after the blur and the device px of its region, a Backdrop step with the region, the matrix before the
// blur, the size it is downsampled to and the blur that remains there, none for a region outside the target.
void backdrops() {
    using Step = weft::render::ScenePass::Step;
    const auto make = [] {
        paint::DisplayList list;
        paint::Backdrop b;
        b.blur = 10;
        b.radii = {4, 4, 4, 4};
        b.post[0] = 2;
        list.backdrops.push_back(b);
        auto i = item(Kind::Backdrop);
        i.width = 40;
        i.height = 30;
        i.clip = {0, 0, 100, 100};
        i.transform = {1, 0, 0, 1, 10.5f, 20};
        list.items.push_back(i);
        return list;
    };
    require(bool(paint::check(make(), 4)), "a well-formed backdrop");
    const auto refused = [&](auto&& change, const char* reason, const char* what) {
        auto list = make();
        change(list);
        require(rejected(list, 4, reason), what);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    refused(
        [](paint::DisplayList& l) {
            l.items[0].payload = 1;
        },
        "backdrop payload out of range", "a backdrop out of range");
    refused(
        [](paint::DisplayList& l) {
            l.backdrops[0].blur = -1;
        },
        "backdrop value", "a negative blur");
    refused(
        [](paint::DisplayList& l) {
            l.backdrops[0].blur = weft::constants::max_backdrop_blur + 1;
        },
        "backdrop value", "a blur past the bound");
    refused(
        [&](paint::DisplayList& l) {
            l.backdrops[0].pre[3] = nan;
        },
        "backdrop value", "a matrix that is not finite");
    refused(
        [](paint::DisplayList& l) {
            l.backdrops[0].radii[1] = -1;
        },
        "backdrop value", "a negative radius");
    refused(
        [](paint::DisplayList& l) {
            l.backdrops[0].under = true;
        },
        "backdrop under no group", "beneath no group");
    {
        auto grouped = make();
        grouped.backdrops[0].under = true;
        grouped.items.insert(grouped.items.begin(), item(Kind::BeginGroup));
        grouped.items.push_back(item(Kind::EndGroup));
        require(bool(paint::check(grouped, 4)), "a backdrop beneath its group");
    }
    {
        auto into = make();
        require(bool(paint::append(into, make())) && into.backdrops.size() == 2 && into.items[1].payload == 1,
                "appended backdrops renumbered");
    }
    // The plan at scale 2: the box covers device px [21, 101) x [40, 100); a blur of 20 device px downsamples it to a fifth,
    // 16 x 12, where it is four.
    const auto list = make();
    const auto pass = weft::render::plan_scene_pass(list, 2, 200, 200).value();
    require(pass.batches.size() == 1 && pass.batches[0].step == Step::Backdrop && pass.batches[0].source == 0 &&
                pass.backdrops.size() == 1,
            "a backdrop's step");
    const auto& b = pass.backdrops[0];
    require(b.region == std::array<std::int32_t, 4>{21, 40, 101, 100} && b.size == std::array<unsigned, 2>{16, 12} &&
                b.sigma == std::array<float, 2>{4, 4} && b.pre == paint::identity_color_matrix && !b.under,
            "a backdrop's region and blur");
    const auto& r = pass.records[0];
    require(r.mode[0] == weft::constants::render::scene::image_mode_backdrop &&
                r.uv == weft::render::SceneRecord::Vector{21, 40, 80, 60} &&
                r.radii == weft::render::SceneRecord::Vector{4, 4, 4, 4} &&
                weft::render::effect_parameter(const_cast<weft::render::SceneRecord&>(r), 0)[0] == 2 &&
                r.extra[3] == weft::constants::render::scene::flag_enabled,
            "a backdrop's record");
    // A blur of exactly four device px blurs without a downsample; a scaled item's blur scales with it.
    {
        auto edge = make();
        edge.backdrops[0].blur = 2;
        const auto exact = weft::render::plan_scene_pass(edge, 2, 200, 200).value().backdrops.at(0);
        require(exact.size == std::array<unsigned, 2>{80, 60} && exact.sigma == std::array<float, 2>{4, 4},
                "a blur of four device px");
        edge.items[0].transform = {2, 0, 0, 2, 0, 0};
        const auto scaled = weft::render::plan_scene_pass(edge, 1, 200, 200).value().backdrops.at(0);
        require(scaled.size == std::array<unsigned, 2>{80, 60} && scaled.sigma == std::array<float, 2>{4, 4},
                "a blur scales with its item");
        // A blur so wide the region would scale below a texel keeps one, with the blur that remains there.
        edge.backdrops[0].blur = 256;
        const auto widest = weft::render::plan_scene_pass(edge, 1, 200, 200).value().backdrops.at(0);
        require(widest.size == std::array<unsigned, 2>{1, 1} &&
                    widest.sigma == std::array<float, 2>{512.f / 80, 512.f / 60},
                "a blur wider than the region");
    }
    // Without a matrix after the blur, the record says so.
    auto plain = make();
    plain.backdrops[0].post = paint::identity_color_matrix;
    require(weft::render::plan_scene_pass(plain, 2, 200, 200).value().records[0].extra[3] !=
                weft::constants::render::scene::flag_enabled,
            "no matrix after the blur");
    // A box on a fraction of a device pixel reads every pixel it touches: its region rounds out.
    {
        auto fractional = make();
        fractional.items[0].transform = {1, 0, 0, 1, 10.25f, 20.25f};
        const auto out = weft::render::plan_scene_pass(fractional, 2, 200, 200).value().backdrops.at(0);
        require(out.region == std::array<std::int32_t, 4>{20, 40, 101, 101}, "a backdrop's region rounds out");
        // Its 81 x 61 px downsample to whole texels, 16 x 12, and the blur there follows from each axis's scale.
        require(out.size == std::array<unsigned, 2>{16, 12} &&
                    out.sigma == std::array<float, 2>{20.f * 16 / 81, 20.f * 12 / 61},
                "a backdrop's downsample to whole texels");
    }
    // Outside the target, nothing to read or draw.
    auto away = make();
    away.items[0].transform = {1, 0, 0, 1, 500, 500};
    require(weft::render::plan_scene_pass(away, 2, 200, 200).value().batches.empty(), "a backdrop off the target");
    // The blur constants: weights over both sides sum to 1, three sigmas of taps.
    const auto k = weft::render::backdrop_blur_constants(2, true, 10, 20);
    float sum = k.weights[0][0];
    for (int i = 1; i <= int(k.extent[0]); ++i) {
        sum += 2 * k.weights[std::size_t(i) / 4][std::size_t(i) % 4];
    }
    require(k.extent[0] == 6 && std::abs(sum - 1) < 1e-5f && k.texel[2] == .1f && k.texel[3] == 0,
            "a Gaussian pass's constants");
    // An area downsample's: source texels per target texel, the axis and the source's texels on it.
    require(weft::render::backdrop_area_constants(81, 16, false).texel ==
                weft::render::SceneRecord::Vector{81.f / 16, 0, 81, 0},
            "an area downsample's constants");
}

int main() {
    try {
        checks();
        scene_pass();
        bounds();
        packer();
        packer_reuse();
        batching();
        quads();
        external_textures();
        append();
        shadows();
        planes();
        effects();
        particles();
        backdrops();
        std::cout << "PASS paint contracts: display list payloads, rasters, shadows, group nesting and kinds; scene "
                     "pass plans; quad lists; external textures; appended lists; shadow images; planes; effects; "
                     "particles; backdrops\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
