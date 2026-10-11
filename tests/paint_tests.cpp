#include "paint/display_list.hpp"
#include "render/quad_lowering.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// The portable paint IR: structural checks of display lists, the portable plan of a scene pass, and the quad
// lowering of page display lists.
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
    }

    // Runs of boxes share a draw, images and groups get their own steps, and an opaque first box that covers the
    // target becomes the clear color unless crop narrows it.
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
        // Images and rounded clips are outside the scene shader.
        auto images = list;
        images.assets.push_back(std::make_shared<paint::Asset>());
        images.images.emplace_back();
        images.items[1] = item(Kind::Image);
        const auto refused = weft::render::plan_scene_pass(images, 2, 100, 80);
        require(bool(paint::check(images, 1)) && !refused && refused.error().code == weft::Diag::PaintList &&
                    refused.error().message.ends_with("item 1"),
                "scene pass refuses images");
        auto clipped = list;
        clipped.clips.emplace_back();
        clipped.items[4].clips_count = 1;
        require(bool(paint::check(clipped, 1)) && !weft::render::plan_scene_pass(clipped, 2, 100, 80),
                "scene pass refuses rounded clips");
    }

    bool throws(const auto& f) {
        try {
            f();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }

    // Items crop to their clips, moving image uv; a rounded clip that cuts an item draws it through a mask, kept and
    // reused across packets; assets follow first use.
    void quads() {
        paint::DisplayList list;
        auto art = std::make_shared<paint::Asset>(
            paint::Asset{2, 2, paint::Encoding::SrgbStraight, std::vector<std::uint8_t>(16, 255)});
        list.assets = {art};
        paint::Image picture;
        picture.uv = {0, 0, 1, 1};
        list.images.push_back(picture);
        list.boxes.emplace_back().background = {1, 0, 0, 1};
        auto at = [](Kind kind, float x, float y, float w, float h, paint::ClipRect clip) {
            auto i = item(kind);
            i.transform = {1, 0, 0, 1, x, y};
            i.width = w;
            i.height = h;
            i.clip = clip;
            return i;
        };
        // An image cropped to its right half, a box inside a rounded clip, a box the rounded clip cuts.
        list.items = {at(Kind::Image, 0, 0, 20, 10, {10, 0, 40, 40}), at(Kind::Box, 12, 12, 4, 4, {0, 0, 40, 40}),
                      at(Kind::Box, 8, 8, 24, 24, {0, 0, 40, 40})};
        list.clips.push_back({{8, 8, 24, 24}, {8, 8, 8, 8}, {8, 8, 8, 8}});
        list.items[1].clips_count = list.items[2].clips_count = 1;
        weft::render::QuadMasks kept;
        const auto packet = weft::render::lower_to_quads(list, &kept);
        require(packet.draws.size() == 3 && packet.assets.size() == 2 && packet.assets[0] == art, "quad assets");
        const auto& image = packet.draws[0];
        require(image.rect.x == 10 && image.rect.w == 10 && image.uv == std::array<float, 4>{.5f, 0, .5f, 1},
                "cropped image uv");
        require(packet.draws[1].asset == -1 && packet.draws[1].srgb == paint::Color{1, 0, 0, 1}, "box inside its clip");
        const auto& cut = packet.draws[2];
        const auto& mask = *packet.assets[cut.asset];
        require(cut.asset == 1 && mask.encoding == paint::Encoding::Coverage && mask.width == 24 && mask.height == 24 &&
                    mask.bytes[0] < 64 && mask.bytes[12 * 24 + 12] == 255,
                "rounded mask");
        require(kept.misses == 1 && kept.hits == 0 && kept.masks.size() == 1, "mask kept");
        const auto again = weft::render::lower_to_quads(list, &kept);
        require(kept.hits == 1 && again.assets[again.draws[2].asset] == packet.assets[1], "kept mask reused");
        auto rotated = list;
        rotated.items[0].transform[1] = 1;
        require(throws([&] {
                    weft::render::lower_to_quads(rotated);
                }),
                "quads need translated items");
        auto rounded = list;
        rounded.boxes[0].radii[0] = 2;
        require(throws([&] {
                    weft::render::lower_to_quads(rounded);
                }),
                "quads need plain boxes");
    }
} // namespace

int main() {
    try {
        checks();
        scene_pass();
        quads();
        std::cout
            << "PASS paint contracts: display list payloads, rasters, group nesting and kinds; scene pass plans; quad lowering\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
