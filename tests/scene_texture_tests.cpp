#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_values.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

// Host textures in scenes (background-image: -weft-texture(name), a Weft extension): the names a page can give, the
// scene diagnostic for any other, and the display list: the background color under the texture, the texture as an
// image item naming it (each name once in the list's names), the borders over it, its rounded corners as a clip, its
// rect snapped to device pixels, and an element that shows one drawn in the pass, never as a cached layer image.
namespace {
    using weft::testing::require;
    using Kind = weft::paint::Item::Kind;

    // The diagnostic code and message of what throws, or none.
    template <class F>
    std::pair<weft::Diag, std::string> diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return {e.code(), e.what()};
        }
        return {weft::Diag{}, {}};
    }

    std::vector<Kind> kinds(const weft::paint::DisplayList& list) {
        std::vector<Kind> out;
        for (const auto& item : list.items) {
            out.push_back(item.kind);
        }
        return out;
    }
} // namespace

int main() {
    try {
        // Names: letters, digits, '-' and '_', at most 64 bytes; another value of the function has an empty name, any
        // other value none.
        using weft::scene_values::texture_name;
        require(texture_name("-weft-texture(minimap)") == "minimap" &&
                    texture_name("-weft-texture( Unit_7-portrait )") == "Unit_7-portrait" &&
                    texture_name("-weft-texture(" + std::string(64, 'a') + ")") == std::string(64, 'a'),
                "texture names");
        require(texture_name("-weft-texture()") == "" && texture_name("-weft-texture(a b)") == "" &&
                    texture_name("-weft-texture(a.b)") == "" && texture_name("-weft-texture(\"a\")") == "" &&
                    texture_name("-weft-texture(" + std::string(65, 'a') + ")") == "" &&
                    texture_name("-weft-texture(a") == "",
                "names a page cannot give");
        require(!texture_name("none") && !texture_name("linear-gradient(red, blue)") &&
                    !texture_name("-weft-effect(glow)"),
                "other backgrounds");

        // A scene: a card with a background color, borders and corners; a square that names the texture again
        // through the background shorthand; and one of another name.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:240px;height:120px");
        auto& card = s.html("div");
        s.set(card, "style",
              "position:absolute;left:10px;top:20px;width:100px;height:60px;background-color:#202020;border:2px solid "
              "#ffffff;border-radius:8px;background-image:-weft-texture(portrait)");
        s.tree.insert(*s.root, card, nullptr);
        auto& twin = s.html("div");
        s.set(twin, "style",
              "position:absolute;left:120px;top:20px;width:40px;height:40px;background:-weft-texture(portrait)");
        s.tree.insert(*s.root, twin, nullptr);
        auto& map = s.html("div");
        s.set(map, "style",
              "position:absolute;left:170px;top:20px;width:50px;height:50px;background-image:-weft-texture(minimap)");
        s.tree.insert(*s.root, map, nullptr);
        weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 240, 120, 0);
        auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text());
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 4)), "a well-formed list");
        const auto& list = listed.list;
        // The card's background color, texture and borders; the twin's and the map's textures.
        require(kinds(list) == std::vector<Kind>{Kind::Box, Kind::Image, Kind::Box, Kind::Image, Kind::Image},
                "background color, texture, borders");
        require(list.textures == std::vector<std::string>{"portrait", "minimap"} && list.assets.empty() &&
                    !list.external_textures,
                "each name once, no assets");
        const auto& under = list.boxes[list.items[0].payload];
        const auto& over = list.boxes[list.items[2].payload];
        const auto image = [&](std::size_t i) {
            return list.images[list.items[i].payload];
        };
        require(under.background[3] > 0 && under.widths == std::array<float, 4>{} && over.background[3] == 0 &&
                    over.widths == std::array<float, 4>{2, 2, 2, 2},
                "the color under the texture, the borders over it");
        require(image(1).texture == 1 && image(3).texture == 1 && image(4).texture == 2 && !image(1).asset &&
                    image(1).uv == std::array<float, 4>{0, 0, 1, 1},
                "the textures by name");
        const auto& card_item = list.items[1];
        const auto& card_node = frame.root.children[0];
        require(card_item.transform == weft::paint::Affine{1, 0, 0, 1, 10, 20} && card_item.width == card_node.box[2] &&
                    card_item.height == card_node.box[3],
                "the texture over the border box");
        require(card_item.clips_count == 1 &&
                    list.clips[card_item.clips_begin].radius_x == std::array<float, 4>{8, 8, 8, 8} &&
                    list.items[3].clips_count == 0 && list.items[4].clips_count == 0,
                "inside the corners, as a rounded clip");

        // A name a page cannot give is the scene profile's diagnostic.
        s.set(map, "style",
              "position:absolute;left:170px;top:20px;width:50px;height:50px;background-image:-weft-texture(a.b)");
        const auto refused = diagnostic([&] {
            solver.frame(s.tree.snapshot(*s.root), 240, 120, 16);
        });
        require(refused.first == weft::Diag::SceneProfile &&
                    refused.second.find("-weft-texture() takes one texture name") != std::string::npos,
                "a name a page cannot give");
        solver.reset();

        // As Chrome draws an image, a texture fills its box's rect snapped to device pixels, each edge to the nearest; a
        // rotated box's is not snapped.
        s.set(card, "style",
              "position:absolute;left:10.375px;top:20.625px;width:100.5px;height:60.25px;"
              "background-image:-weft-texture(portrait)");
        s.set(twin, "style",
              "position:absolute;left:120.5px;top:20.5px;width:40px;height:40px;transform:rotate(30deg);"
              "background-image:-weft-texture(portrait)");
        s.set(map, "style", "position:absolute;left:170px;top:20px;width:50px;height:50px");
        const auto fractional = solver.frame(s.tree.snapshot(*s.root), 240, 120, 32);
        const auto texture_items = [&](float scale) {
            std::vector<weft::paint::Item> items;
            for (const auto& item :
                 weft::scene_display_list(fractional, {}, &weft::testing::thread_scene_text(), scale).list.items) {
                if (item.kind == Kind::Image) {
                    items.push_back(item);
                }
            }
            require(items.size() == 2, "the card's and the twin's textures");
            return items;
        };
        const auto at1 = texture_items(1);
        const auto at2 = texture_items(2);
        require(at1[0].transform == weft::paint::Affine{1, 0, 0, 1, 10, 21} && at1[0].width == 101 &&
                    at1[0].height == 60,
                "a texture snapped to pixels");
        require(at2[0].transform == weft::paint::Affine{1, 0, 0, 1, 10.5f, 20.5f} && at2[0].width == 100.5f &&
                    at2[0].height == 60.5f,
                "a texture snapped to device pixels");
        require(at1[1].transform == at2[1].transform && at1[1].width == 40 && at1[1].height == 40 &&
                    at1[1].transform[1] != 0,
                "a rotated texture not snapped");

        // An explicit layer that shows a texture draws in the pass, never as a cached image: the host's texture
        // changes without the page.
        const auto& fractional_card = fractional.root.children[0];
        const auto layered =
            weft::scene_display_list(fractional, {fractional_card.identity}, &weft::testing::thread_scene_text());
        require(std::none_of(layered.list.items.begin(), layered.list.items.end(),
                             [](const weft::paint::Item& item) {
                                 return item.kind == Kind::Layer;
                             }) &&
                    layered.list.textures == std::vector<std::string>{"portrait"},
                "a layer with a texture draws in the pass");

        // A keyframe's background shorthand names a texture too: the solver expands what an animation samples.
        {
            weft::testing::NativeScene k;
            k.set(*k.root, "style", "position:relative;width:120px;height:80px");
            auto& shown = k.html("div");
            k.set(shown, "style", "position:absolute;left:10px;top:10px;width:40px;height:40px;animation:show 1s");
            k.tree.insert(*k.root, shown, nullptr);
            weft::SceneSolver animated(
                "", "@keyframes show{from{background:-weft-texture(minimap)}to{background:-weft-texture(minimap)}}",
                weft::testing::thread_scene_text());
            const auto at = animated.frame(k.tree.snapshot(*k.root), 120, 80, 500);
            const auto keyed = weft::scene_display_list(at, {}, &weft::testing::thread_scene_text());
            require(keyed.fallback.empty() && keyed.list.textures == std::vector<std::string>{"minimap"} &&
                        kinds(keyed.list) == std::vector<Kind>{Kind::Image},
                    "a texture a keyframe names");
        }
        std::cout << "PASS scene textures: names, diagnostics, display lists, corners, snapping, layers, keyframes\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "scene texture tests failed: " << e.what() << '\n';
        return 1;
    }
}
