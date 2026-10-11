#include "paint/display_list.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// The portable paint IR: structural checks of display lists.
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
} // namespace

int main() {
    try {
        checks();
        std::cout << "PASS paint contracts: display list payloads, rasters, group nesting and kinds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
