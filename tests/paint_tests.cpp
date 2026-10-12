#include "paint/display_list.hpp"
#include "render/atlas_packer.hpp"
#include "render/quad_list.hpp"
#include "render/rounded_clip.hpp"
#include "render/scene_pass.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

// The portable paint IR: structural checks of display lists, the portable plan of a scene pass, quad packets as
// display lists, external textures and appended lists.
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

int main() {
    try {
        checks();
        scene_pass();
        bounds();
        packer();
        batching();
        quads();
        external_textures();
        append();
        shadows();
        std::cout << "PASS paint contracts: display list payloads, rasters, shadows, group nesting and kinds; scene "
                     "pass plans; quad lists; external textures; appended lists; shadow images\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
