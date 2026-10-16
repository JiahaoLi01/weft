#include "scene/scene_native.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

// The backend-neutral preparation of native scene passes: the prepared tree's images, planes and layers, the scratch
// its targets take, its caches by content, the fallbacks it counts, and its atlas pages: where small images lie, which
// runs of steps draw at once, and when a region comes back.
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

    // Small and large images among boxes: labels (text of one color) over boxes, a shadow, text in colors of its own, a
    // label in a translucent group, an SVG path, and text too tall for an atlas page; between two labels, a backdrop
    // over no pixel of the target, which draws nothing.
    weft::SceneFrame atlas_frame() {
        weft::SceneFrame f;
        f.root = node(600, {0, 0, 400, 300}, {{"background-color", "#ffffff"}});
        auto& c = f.root.children;
        c.push_back(node(601, {4, 4, 80, 20}, {{"background-color", "#e0e0e0"}}));
        c.push_back(node(602, {4, 4, 80, 20}, {{"font-size", "12px"}, {"color", "#202020"}}, "Open 1"));
        c.push_back(node(613, {500, 400, 20, 20}, {{"backdrop-filter", "blur(4px)"}}));
        c.push_back(node(603, {4, 30, 80, 20}, {{"background-color", "#d0d0d0"}}));
        c.push_back(node(604, {4, 30, 80, 20}, {{"font-size", "12px"}, {"color", "#2060c0"}}, "Close 2"));
        c.push_back(node(605, {100, 4, 40, 30},
                         {{"background-color", "#3060c0"}, {"box-shadow", "2px 3px 4px 0px rgba(0, 0, 0, 0.5)"}}));
        c.push_back(node(606, {100, 50, 80, 20}, {{"font-size", "12px"}, {"color", "#202020"}}, "\xe2\x98\x85 star"));
        auto group = node(607, {200, 4, 80, 40}, {{"opacity", "0.5"}, {"background-color", "#40a040"}});
        group.children = {node(608, {4, 4, 60, 20}, {{"font-size", "12px"}, {"color", "#ffffff"}}, "Inside")};
        c.push_back(group);
        auto svg = node(609, {4, 80, 100, 60}, {});
        svg.tag = "svg";
        svg.space = weft::constants::svg_namespace_uri;
        svg.attributes = {{"width", "100"}, {"height", "60"}};
        auto path = node(610, {0, 0, 100, 60}, {{"fill", "#c03030"}});
        path.tag = "path";
        path.space = weft::constants::svg_namespace_uri;
        path.attributes = {{"d", "M10 10 L50 15 L30 40 Z"}};
        svg.children = {path};
        c.push_back(svg);
        c.push_back(node(611, {4, 150, 300, 120}, {{"font-size", "80px"}, {"color", "#202020"}}, "Big"));
        c.push_back(node(612, {150, 80, 80, 20}, {{"background-color", "#a0a0a0"}}));
        return f;
    }

    // Steps that draw: all but a group's beginning, and an image step with no image.
    std::size_t draws(const weft::ScenePrepared& p) {
        using Step = weft::render::ScenePass::Step;
        return std::size_t(std::count_if(p.pass.batches.begin(), p.pass.batches.end(), [&](const auto& b) {
            return b.step != Step::BeginGroup &&
                   (b.step != Step::Image || p.images[b.first] || p.planes[b.first] || p.layers[b.first]);
        }));
    }

    // Where each image of a pass on an atlas page lies: page, x, y, width and height, with its format.
    std::vector<std::array<std::uint32_t, 6>> places(const weft::ScenePrepared& p) {
        std::vector<std::array<std::uint32_t, 6>> out;
        for (const auto& image : p.images) {
            if (image && image->atlas) {
                out.push_back({std::uint32_t(image->format), image->atlas->page, image->atlas->x, image->atlas->y,
                               image->width, image->height});
            }
        }
        return out;
    }

    // Two places' regions share a texel, their borders included.
    bool overlap(const std::array<std::uint32_t, 6>& a, const std::array<std::uint32_t, 6>& b) {
        const auto border = std::uint32_t(weft::constants::render::scene::atlas_border);
        return a[0] == b[0] && a[1] == b[1] && a[2] < b[2] + b[4] + 2 * border && b[2] < a[2] + a[4] + 2 * border &&
               a[3] < b[3] + b[5] + 2 * border && b[3] < a[3] + a[5] + 2 * border;
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
        frame.root.children.edit(0).text = "Ho";
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
        behind.root.children.edit(2).style["transform"] = "perspective(20px) rotateY(80deg)";
        require(!preparer.prepare(behind, 2, 400, 200, std::nullopt, 0, {}) && work.fallbacks == 1 &&
                    work.reasons.at("plane behind the viewer") == 1,
                "fallback reason");
        // A window's shadow whose pixels at the device scale pass the bitmap bound (900 x 640 at 1.5, a panel of a
        // game at 150%) is an image made at a lower scale, over the same rectangle: the frame still draws.
        {
            weft::SceneFrame window;
            window.root = node(20, {0, 0, 1280, 720}, {});
            window.root.children = {node(
                21, {190, 40, 900, 640},
                {{"background-color", "rgba(24, 17, 12, 0.9)"}, {"box-shadow", "0 4px 14px rgba(0, 0, 0, 0.55)"}})};
            const auto drawn = preparer.prepare(window, 1.5f, 1920, 1080, std::nullopt, 0, {});
            const weft::SceneImage* shadow{};
            for (const auto& image : drawn ? drawn->images : decltype(drawn->images){}) {
                shadow = image ? image.get() : shadow;
            }
            require(drawn && work.fallbacks == 1 && shadow &&
                        std::size_t(shadow->width) * shadow->height <=
                            std::size_t(weft::constants::scene::max_shadow_bitmap_pixels) &&
                        shadow->width < 1350,
                    "a large shadow at a high scale is made coarser, not a fallback");
        }
        // An effect's textures: an image of each asset in the format the effect samples (coverage as data, an opaque
        // sRGB image as sRGB, a translucent one as linear premultiplied floats), built once while the asset lives.
        {
            auto program = std::make_shared<weft::paint::EffectProgram>();
            program->textures = 3;
            const auto data = std::make_shared<const weft::paint::Asset>(
                weft::paint::Asset{2, 1, weft::paint::Encoding::Coverage, {10, 20}});
            const auto opaque = std::make_shared<const weft::paint::Asset>(
                weft::paint::Asset{1, 1, weft::paint::Encoding::SrgbStraight, {255, 128, 0, 255}});
            const auto translucent = std::make_shared<const weft::paint::Asset>(
                weft::paint::Asset{1, 1, weft::paint::Encoding::SrgbStraight, {255, 128, 0, 128}});
            weft::SceneFrame effected;
            effected.root = node(400, {0, 0, 40, 40}, {});
            effected.root.effect =
                std::make_shared<const weft::SceneEffect>(weft::SceneEffect{program, {data, opaque, translucent}, {}});
            weft::SceneNativeWork effect_work;
            weft::ScenePreparer textures(text, effect_work, 4096);
            const auto prepared = textures.prepare(effected, 1, 40, 40, std::nullopt, 0, {});
            require(prepared && prepared->effects.size() == 1 && prepared->effects[0].program == program,
                    "a prepared effect");
            const auto& effect_images = prepared->effects[0].textures;
            float red{};
            std::memcpy(&red, effect_images[2]->pixels.data(), sizeof(red));
            require(effect_images.size() == 3 && effect_images[0]->format == weft::SceneImage::Format::Data &&
                        effect_images[0]->pixels == std::vector<std::uint8_t>{10, 20} &&
                        effect_images[1]->format == weft::SceneImage::Format::Srgb &&
                        effect_images[2]->format == weft::SceneImage::Format::Linear &&
                        effect_images[2]->pixels.size() == 4 * sizeof(float) && std::abs(red - 128 / 255.f) < 1e-6f,
                    "effect texture formats");
            const auto again = textures.prepare(effected, 1, 40, 40, std::nullopt, 0, {});
            require(again && again->effects[0].textures[0] == effect_images[0] &&
                        effect_work.effect_texture_builds == 3 && effect_work.effect_texture_reuses == 3,
                    "effect textures built once");
        }
        const auto labels = [&](std::uint32_t first, const std::string& word) {
            weft::SceneFrame f;
            f.root = node(first, {0, 0, 200, 200}, {{"background-color", "#ffffff"}});
            for (std::uint32_t i = 1; i <= 6; ++i) {
                f.root.children.push_back(node(first + i, {4, float(i) * 24, 80, 20},
                                               {{"font-size", "12px"}, {"color", "#202020"}},
                                               word + " " + std::to_string(i)));
            }
            return f;
        };
        // Text images go by their source, not their node: other nodes with the same text reuse them, and a node whose
        // text changes gets another.
        {
            weft::SceneNativeWork keyed_work;
            weft::ScenePreparer keyed(text, keyed_work, 4096);
            const auto first_pass = keyed.prepare(labels(200, "Label"), 1, 200, 200, std::nullopt, 0, {});
            const auto renamed = keyed.prepare(labels(300, "Label"), 1, 200, 200, std::nullopt, 0, {});
            require(first_pass && renamed && keyed_work.text_builds == 6 && keyed_work.text_reuses == 6 &&
                        renamed->images == first_pass->images,
                    "other nodes with the same text reuse the images");
            auto changed = labels(200, "Label");
            changed.root.children.edit(2).text = "Label 30";
            const auto relabelled = keyed.prepare(changed, 1, 200, 200, std::nullopt, 0, {});
            require(relabelled && keyed_work.text_builds == 7 && keyed_work.text_reuses == 11 &&
                        relabelled->images[3] != first_pass->images[3],
                    "a node's changed text is another image");
            // The same text at another scale is another image.
            const auto doubled = keyed.prepare(labels(200, "Label"), 2, 400, 400, std::nullopt, 0, {});
            require(doubled && keyed_work.text_builds == 13 && doubled->images[1] != first_pass->images[1],
                    "the same text at another scale is another image");
        }
        // A pass with more images than a cache's entry bound keeps them all for the next pass, which reuses every
        // one; a pass of other images then evicts the earlier pass's, oldest first, down to the bound.
        weft::SceneNativeWork bounded_work;
        weft::ScenePreparer bounded(text, bounded_work, 4096, {}, weft::SceneCacheBounds{4, std::size_t(1) << 30});
        const auto many = labels(200, "Label");
        require(bool(bounded.prepare(many, 1, 200, 200, std::nullopt, 0, {})) &&
                    bool(bounded.prepare(many, 1, 200, 200, std::nullopt, 0, {})) && bounded_work.text_builds == 6 &&
                    bounded_work.text_reuses == 6 && !bounded_work.image_evictions,
                "a pass over the entry bound keeps its images");
        require(bool(bounded.prepare(labels(300, "Name"), 1, 200, 200, std::nullopt, 0, {})) &&
                    bounded_work.text_builds == 12 && bounded_work.image_evictions == 6,
                "a later pass evicts the earlier pass's images");
        // Small images lie on atlas pages of their format, coverage (paths, text of one color) or colors (shadows, text in
        // colors of its own); an image too large has a texture of its own. Their records sample their pages, and runs of
        // boxes and of images on one page next to each other are one Atlas step, never across a group. Without pages,
        // the same records but for their images' places, and more draws.
        {
            using Step = weft::render::ScenePass::Step;
            using Format = weft::SceneImage::Format;
            weft::SceneNativeWork atlas_work;
            weft::ScenePreparer atlas(text, atlas_work, 4096);
            const auto small = atlas_frame();
            const auto p = atlas.prepare(small, 2, 800, 600, std::nullopt, 0, {});
            require(p && !atlas_work.fallbacks, "an atlas pass");
            std::size_t coverage{};
            std::size_t colors{};
            std::size_t own{};
            for (std::size_t i = 0; i < p->images.size(); ++i) {
                const auto& image = p->images[i];
                if (!image) {
                    continue;
                }
                if (!image->atlas) {
                    ++own;
                    continue;
                }
                (image->format == Format::Data ? coverage : colors) += 1;
                const auto size = float(weft::scene_atlas_page_size(image->format));
                require(p->pass.records[i].uv == weft::render::SceneRecord::Vector{float(image->atlas->x) / size,
                                                                                   float(image->atlas->y) / size,
                                                                                   float(image->width) / size,
                                                                                   float(image->height) / size},
                        "an atlas image's record samples its page");
            }
            // Coverage keeps its antialiasing: a text mask is its white text's alpha, which edges cover in part, not a
            // color channel, which white text fills wherever it covers at all.
            for (const auto& image : p->images) {
                require(!image || image->format != Format::Data ||
                            std::any_of(image->pixels.begin(), image->pixels.end(),
                                        [](std::uint8_t v) {
                                            return v > 0 && v < 255;
                                        }),
                        "coverage keeps its antialiasing");
            }
            require(std::none_of(p->pass.batches.begin(), p->pass.batches.end(),
                                 [](const auto& b) {
                                     return b.step == Step::Backdrop;
                                 }) &&
                        std::any_of(p->pass.records.begin(), p->pass.records.end(),
                                    [](const auto& r) {
                                        return r.mode[0] == weft::render::scene_limits::image_mode_backdrop;
                                    }),
                    "a backdrop over no pixel has a record and no step");
            require(coverage == 4 && colors == 2 && own == 1 && atlas_work.atlas_placements == 6 &&
                        !atlas_work.atlas_refusals,
                    "labels, the label in the group and the path on a coverage page, the shadow and the colored text "
                    "on a color page, the tall text on its own");
            std::size_t runs{};
            for (const auto& b : p->pass.batches) {
                require(b.step != Step::Image || !p->images[b.first] || !p->images[b.first]->atlas,
                        "no atlas image draws alone");
                if (b.step != Step::Atlas) {
                    continue;
                }
                ++runs;
                const auto& page = p->pages.at(b.source);
                for (auto i = b.first; i < b.first + b.count; ++i) {
                    const auto& image = p->images[i];
                    require(!p->planes[i] && !p->layers[i] &&
                                p->pass.records[i].mode[0] != weft::render::scene_limits::image_mode_backdrop &&
                                (!image ||
                                 (image->atlas && image->format == page.format && image->atlas->page == page.page)),
                            "an Atlas step draws boxes and images on its page, and no item that has no step");
                }
            }
            weft::SceneNativeWork plain_work;
            weft::ScenePreparer plain(text, plain_work, 4096, {}, weft::SceneCacheBounds{.atlas_pages = 0});
            const auto q = plain.prepare(small, 2, 800, 600, std::nullopt, 0, {});
            require(q && q->pages.empty() && places(*q).empty() && !plain_work.atlas_placements &&
                        !plain_work.atlas_refusals,
                    "no atlas pages, no places");
            bool alike = q->pass.records.size() == p->pass.records.size();
            for (std::size_t i = 0; alike && i < q->pass.records.size(); ++i) {
                auto r = p->pass.records[i];
                r.uv = q->pass.records[i].uv;
                alike = std::memcmp(&r, &q->pass.records[i], sizeof r) == 0;
            }
            require(alike, "the same records but for where their images lie");
            require(runs >= 3 && draws(*p) + 5 <= draws(*q),
                    "runs of boxes and atlas images draw at once: " + std::to_string(draws(*p)) + " draws, " +
                        std::to_string(draws(*q)) + " without pages");
        }
        // A region comes back once its image is gone, and only then: a pass's new images never take the regions of an
        // earlier pass's images that something still holds, and take those of images nothing holds any more.
        {
            weft::SceneNativeWork reuse_work;
            weft::ScenePreparer reuse(text, reuse_work, 4096, {}, weft::SceneCacheBounds{4, std::size_t(1) << 30});
            auto held = reuse.prepare(labels(700, "Alpha"), 1, 200, 200, std::nullopt, 0, {});
            const auto alpha = places(*held);
            auto beta = reuse.prepare(labels(800, "Beta"), 1, 200, 200, std::nullopt, 0, {});
            bool apart = alpha.size() == 6 && places(*beta).size() == 6;
            for (const auto& a : alpha) {
                for (const auto& b : places(*beta)) {
                    apart &= !overlap(a, b);
                }
            }
            require(apart && reuse_work.image_evictions == 6, "evicted images still held keep their regions");
            held.reset();
            beta.reset();
            const auto gamma = reuse.prepare(labels(900, "Gamma"), 1, 200, 200, std::nullopt, 0, {});
            std::size_t taken{};
            for (const auto& g : places(*gamma)) {
                taken += std::count_if(alpha.begin(), alpha.end(), [&](const auto& a) {
                    return a[0] == g[0] && a[1] == g[1] && a[2] == g[2] && a[3] == g[3];
                });
            }
            require(taken == 6, "images nothing holds give their regions to new ones");
        }
        // A full atlas: images that find no page with room get textures of their own, and draw on their own.
        {
            weft::SceneFrame f;
            f.root = node(1000, {0, 0, 400, 300}, {{"background-color", "#ffffff"}});
            for (std::uint32_t i = 0; i < 40; ++i) {
                f.root.children.push_back(node(1001 + i, {4 + float(i % 4) * 90, 4 + float(i / 4) * 24, 80, 20},
                                               {{"font-size", "12px"}, {"color", "#202020"}},
                                               "\xe2\x98\x85 " + std::to_string(i)));
            }
            weft::SceneNativeWork full_work;
            weft::ScenePreparer full(text, full_work, 4096, {}, weft::SceneCacheBounds{.atlas_pages = 1});
            const auto p = full.prepare(f, 4, 1600, 1200, std::nullopt, 0, {});
            std::size_t alone{};
            for (const auto& b : p->pass.batches) {
                alone +=
                    b.step == weft::render::ScenePass::Step::Image && p->images[b.first] && !p->images[b.first]->atlas;
            }
            require(p && full_work.atlas_refusals > 0 && full_work.atlas_placements + full_work.atlas_refusals == 40 &&
                        alone == full_work.atlas_refusals,
                    "a full atlas: " + std::to_string(full_work.atlas_refusals) + " images of their own");
        }
        std::cout << "PASS scene native preparation: images, planes, layers, scratch, caches, cache bounds, ids, "
                     "fallbacks, atlas pages\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
