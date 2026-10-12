#include "scene/scene_native.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <iostream>
#include <map>

// The backend-neutral preparation of native scene passes: the prepared tree's images, planes and layers, the scratch
// its targets take, its caches by content, and the fallbacks it counts.
namespace {
    using weft::testing::require;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style, std::string text = {}) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        n.text = std::move(text);
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    // The images and layers of a prepared tree by id: one object per id.
    bool collect(const weft::ScenePrepared& p, std::map<std::uint64_t, const void*>& ids) {
        const auto add = [&](std::uint64_t id, const void* object) {
            return ids.emplace(id, object).first->second == object;
        };
        bool unique = true;
        for (std::size_t i = 0; i < p.images.size(); ++i) {
            if (p.images[i]) {
                unique &= add(p.images[i]->id, p.images[i].get());
            }
            if (p.layers[i]) {
                unique &= add(p.layers[i]->id, p.layers[i].get()) && collect(*p.layers[i]->content, ids);
            }
            if (p.planes[i]) {
                unique &= collect(*p.planes[i], ids);
            }
        }
        return unique;
    }
} // namespace

int main() {
    try {
        auto& text = weft::testing::thread_scene_text();
        weft::SceneNativeWork work;
        weft::ScenePreparer preparer(text, work, 4096);
        weft::SceneFrame frame;
        frame.root = node(1, {0, 0, 200, 100}, {{"background-color", "#ffffff"}});
        auto label = node(2, {4, 4, 60, 20}, {{"font-size", "12px"}, {"color", "#202020"}}, "Hi");
        auto shaded = node(3, {4, 40, 40, 30},
                           {{"background-color", "#3060c0"}, {"box-shadow", "2px 3px 4px 0px rgba(0, 0, 0, 0.5)"}});
        // A plane whose content holds a group: the translucent box over a child.
        auto plane = node(
            4, {80, 10, 100, 60},
            {{"transform", "perspective(300px) rotateY(30deg)"}, {"opacity", "0.5"}, {"background-color", "#20a060"}});
        plane.children = {node(5, {10, 10, 30, 20}, {{"background-color", "#f0c030"}})};
        auto boxed = node(6, {4, 76, 40, 20}, {{"background-color", "#c02060"}});
        boxed.children = {node(7, {4, 4, 20, 10}, {{"background-color", "#ffffff"}})};
        frame.root.children = {label, shaded, plane, boxed};
        const auto first = preparer.prepare(frame, 2, 400, 200, std::nullopt, 0, {});
        require(first && !work.fallbacks, "prepared");
        // One text image, one shadow image, one plane pass of the plane's image size with its content's group.
        std::size_t images{};
        std::size_t planes{};
        const weft::ScenePrepared* content{};
        for (std::size_t i = 0; i < first->images.size(); ++i) {
            images += bool(first->images[i]);
            if (first->planes[i]) {
                ++planes;
                content = first->planes[i].get();
            }
        }
        require(images == 2 && planes == 1 && content && work.text_builds == 1 && work.shadow_builds == 1,
                "prepared images and planes");
        weft::paint::Plane plane_value;
        plane_value.width = 100;
        plane_value.height = 60;
        const auto pixels = weft::render::plane_pixels(plane_value, 2);
        require(content->width == pixels[0] && content->height == pixels[1] && content->pass.group_depth == 1,
                "plane pass");
        // Scratch: the target's groups, and the plane's image and its group.
        const auto expected = std::size_t(400) * 200 * 4 * first->pass.group_depth +
                              std::size_t(pixels[0]) * pixels[1] * 4 * (content->pass.group_depth + 1);
        require(weft::scene_scratch_bytes(*first) == expected, "scratch of the prepared tree");
        // Unchanged content comes from the caches: the same images, by the same ids.
        const auto second = preparer.prepare(frame, 2, 400, 200, std::nullopt, 0, {});
        bool same = true;
        for (std::size_t i = 0; i < first->images.size(); ++i) {
            same &= first->images[i] == second->images[i];
        }
        require(same && work.text_builds == 1 && work.text_reuses == 1 && work.shadow_reuses == 1, "cache reuse");
        // A changed text builds a new image with a new id.
        frame.root.children[0].text = "Ho";
        const auto third = preparer.prepare(frame, 2, 400, 200, std::nullopt, 0, {});
        require(work.text_builds == 2 && third->images[1] && first->images[1] &&
                    third->images[1]->id != first->images[1]->id,
                "changed text");
        // An explicit layer: its content's pass, of the node's size with its margins.
        const auto layered = preparer.prepare(frame, 2, 400, 200, std::nullopt, 0, {boxed.identity});
        const weft::SceneLayerImage* layer{};
        for (const auto& l : layered->layers) {
            if (l) {
                layer = l.get();
            }
        }
        require(layer && layer->content && layer->width == 88 && layer->height == 48 && work.layer_builds == 1 &&
                    layer->content->width == 88,
                "layer content");
        (void)preparer.prepare(frame, 2, 400, 200, std::nullopt, 0, {boxed.identity});
        require(work.layer_reuses == 1, "layer reuse");
        // Images shared between passes keep their ids; distinct images and layers never share one.
        std::map<std::uint64_t, const void*> ids;
        bool unique = true;
        for (const auto* p : {first.get(), third.get(), layered.get()}) {
            unique &= collect(*p, ids);
        }
        require(unique && ids.size() == 4, "unique ids");
        // A frame a native pass cannot draw is null, its reason counted.
        auto behind = frame;
        behind.root.children[2].style["transform"] = "perspective(20px) rotateY(80deg)";
        require(!preparer.prepare(behind, 2, 400, 200, std::nullopt, 0, {}) && work.fallbacks == 1 &&
                    work.reasons.at("plane behind the viewer") == 1,
                "fallback reason");
        // A pass with more images than a cache's entry bound keeps them all for the next pass, which reuses every
        // one; a pass of other images then evicts the earlier pass's, oldest first, down to the bound.
        weft::SceneNativeWork bounded_work;
        weft::ScenePreparer bounded(text, bounded_work, 4096, {}, weft::SceneCacheBounds{4, std::size_t(1) << 30});
        const auto labels = [&](std::uint32_t first) {
            weft::SceneFrame f;
            f.root = node(first, {0, 0, 200, 200}, {{"background-color", "#ffffff"}});
            for (std::uint32_t i = 1; i <= 6; ++i) {
                f.root.children.push_back(node(first + i, {4, float(i) * 24, 80, 20},
                                               {{"font-size", "12px"}, {"color", "#202020"}},
                                               "Label " + std::to_string(i)));
            }
            return f;
        };
        const auto many = labels(200);
        require(bool(bounded.prepare(many, 1, 200, 200, std::nullopt, 0, {})) &&
                    bool(bounded.prepare(many, 1, 200, 200, std::nullopt, 0, {})) && bounded_work.text_builds == 6 &&
                    bounded_work.text_reuses == 6 && !bounded_work.image_evictions,
                "a pass over the entry bound keeps its images");
        require(bool(bounded.prepare(labels(300), 1, 200, 200, std::nullopt, 0, {})) &&
                    bounded_work.text_builds == 12 && bounded_work.image_evictions == 6,
                "a later pass evicts the earlier pass's images");
        std::cout << "PASS scene native preparation: images, planes, layers, scratch, caches, cache bounds, ids, "
                     "fallbacks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
