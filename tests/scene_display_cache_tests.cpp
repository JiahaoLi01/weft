#include "scene/scene_display_list.hpp"
#include "scene/scene_native.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

// What native scene passes keep between frames by content (step 12.1 of docs/refactoring-plan.md):
// - SVG elements' values (SceneDisplayCache): a display list built with the cache is the list built without it, frame
//   after frame, while random edits change elements' attributes, styles, text and identities; an unchanged element
//   is listed from its kept values and a changed or new one taken again; views keep their own; values that fail are
//   never kept.
// - Text images by source (scene_paint_source_key): two nodes' keys are equal exactly when scene_paint_source_equal
//   finds them equal but for their identities.
// - Path coverage by digest: a path drawn again, from the same object or another of the same points, reuses its
//   coverage; another point, dash, fill rule or device map draws it anew.
namespace {
    using weft::testing::require;

    // A display list as bytes: its fallback and every value of its items and of what they draw.
    struct Fingerprint {
        std::string out;

        void bytes(const void* data, std::size_t size) {
            out.append(static_cast<const char*>(data), size);
        }

        void number(float v) {
            const auto bits = std::bit_cast<std::uint32_t>(v);
            bytes(&bits, sizeof bits);
        }

        template <class T, std::size_t N>
        void numbers(const std::array<T, N>& values) {
            for (const auto& v : values) {
                if constexpr (std::is_floating_point_v<T>) {
                    number(v);
                } else {
                    numbers(v);
                }
            }
        }

        void count(std::size_t n) {
            const auto v = std::uint64_t(n);
            bytes(&v, sizeof v);
        }

        void path(const weft::paint::Path& p) {
            count(p.verbs.size());
            bytes(p.verbs.data(), p.verbs.size() * sizeof(p.verbs[0]));
            count(p.points.size());
            for (const auto& point : p.points) {
                numbers(point);
            }
            count(p.evenodd);
        }

        void list(const weft::SceneDisplayList& listed) {
            const auto& l = listed.list;
            out += listed.fallback;
            count(l.items.size());
            for (const auto& item : l.items) {
                count(std::size_t(item.kind));
                numbers(item.transform);
                numbers(item.clip);
                number(item.width);
                number(item.height);
                number(item.opacity);
                count(item.payload);
                count(item.clips_begin);
                count(item.clips_count);
            }
            count(l.paths.size());
            for (const auto& p : l.paths) {
                path(*p.path);
                numbers(p.color);
                count(p.stroke.has_value());
                if (p.stroke) {
                    number(p.stroke->width);
                    count(std::size_t(p.stroke->cap));
                    count(std::size_t(p.stroke->join));
                    number(p.stroke->miter_limit);
                    number(p.stroke->dash_offset);
                    count(p.stroke->dashes.size());
                    for (const auto d : p.stroke->dashes) {
                        number(d);
                    }
                }
            }
            count(l.rasters.size());
            for (const auto& r : l.rasters) {
                out += weft::scene_paint_source_key(*r.source);
                count(r.source->identity.slot);
                count(r.mask);
                numbers(r.tint);
                count(r.part);
                number(r.overhang);
            }
            count(l.boxes.size());
            for (const auto& b : l.boxes) {
                numbers(b.radii);
                numbers(b.widths);
                numbers(b.edges);
                numbers(b.background);
                count(std::size_t(b.gradient.kind));
            }
            count(l.clips.size());
            for (const auto& c : l.clips) {
                numbers(c.rect);
                numbers(c.radius_x);
                numbers(c.radius_y);
                numbers(c.to_local);
            }
        }
    };

    std::string fingerprint(const weft::SceneDisplayList& listed) {
        Fingerprint f;
        f.list(listed);
        return std::move(f.out);
    }

    weft::SceneNode element(std::uint32_t slot, std::string tag) {
        weft::SceneNode n;
        n.identity = {slot, 1, 1};
        n.tag = std::move(tag);
        n.space = weft::constants::svg_namespace_uri;
        return n;
    }

    // A page with an svg element over most of it, the given SVG elements inside.
    weft::SceneFrame page(std::vector<weft::SceneNode> elements) {
        weft::SceneFrame f;
        f.root.identity = {1, 1, 1};
        f.root.tag = "div";
        f.root.box = {0, 0, 320, 240};
        f.root.style["background-color"] = "#101820";
        auto svg = element(2, "svg");
        svg.box = {10, 10, 300, 220};
        svg.attributes["width"] = "300";
        svg.attributes["height"] = "220";
        svg.children = std::move(elements);
        f.root.children.push_back(std::move(svg));
        return f;
    }

    // Random SVG elements and edits of them.
    struct Random {
        std::mt19937 engine;

        explicit Random(unsigned seed) : engine(seed) {
        }

        int below(int n) {
            return std::uniform_int_distribution<int>(0, n - 1)(engine);
        }

        std::string number(int low, int high) {
            return std::to_string(low + below(high - low + 1));
        }

        std::string color() {
            static const char* colors[]{"#e63946",     "#2a9d8f", "rgba(40, 120, 220, 0.6)", "none", "currentColor",
                                        "transparent", "#ffffff"};
            return colors[below(int(std::size(colors)))];
        }

        std::string path() {
            std::string d = "M" + number(0, 200) + " " + number(0, 200);
            const int segments = 1 + below(5);
            for (int i = 0; i < segments; ++i) {
                switch (below(5)) {
                    case 0:
                        d += " L" + number(0, 200) + " " + number(0, 200);
                        break;
                    case 1:
                        d += " C" + number(0, 200) + " " + number(0, 200) + " " + number(0, 200) + " " +
                             number(0, 200) + " " + number(0, 200) + " " + number(0, 200);
                        break;
                    case 2:
                        d += " Q" + number(0, 200) + " " + number(0, 200) + " " + number(0, 200) + " " + number(0, 200);
                        break;
                    case 3:
                        d += " A" + number(5, 40) + " " + number(5, 40) + " " + number(0, 90) + " " + number(0, 1) +
                             " " + number(0, 1) + " " + number(0, 200) + " " + number(0, 200);
                        break;
                    default:
                        d += " Z";
                        break;
                }
            }
            return d;
        }

        // One of the attributes or styles an element of tag takes, set at random; rarely a value no element takes.
        void edit(weft::SceneNode& n) {
            const bool broken = below(40) == 0;
            switch (below(9)) {
                case 0:
                    n.style["fill"] = color();
                    break;
                case 1:
                    n.style["stroke"] = color();
                    break;
                case 2:
                    n.style["stroke-width"] = broken ? "wide" : number(0, 6);
                    break;
                case 3:
                    n.style["opacity"] = below(2) ? "1" : "0." + number(1, 9);
                    break;
                case 4:
                    n.attributes["transform"] = below(3) ? "translate(" + number(-20, 20) + " " + number(-20, 20) + ")"
                                                         : "rotate(" + number(0, 90) + ")";
                    break;
                case 5:
                    n.style["stroke-dasharray"] = below(2) ? "none" : number(1, 8) + " " + number(1, 8);
                    break;
                case 6:
                    n.style["fill-opacity"] = "0." + number(1, 9);
                    break;
                case 7:
                    shape(n, broken);
                    break;
                default:
                    if (n.tag == "text") {
                        n.text = "Label " + number(0, 9);
                    } else {
                        n.style["stroke-opacity"] = "0." + number(1, 9);
                    }
                    break;
            }
        }

        // The element's geometry attributes.
        void shape(weft::SceneNode& n, bool broken = false) {
            if (n.tag == "rect") {
                n.attributes["x"] = number(0, 150);
                n.attributes["y"] = number(0, 150);
                n.attributes["width"] = number(1, 80);
                n.attributes["height"] = number(1, 80);
                n.attributes["rx"] = number(0, 10);
            } else if (n.tag == "circle") {
                n.attributes["cx"] = number(0, 200);
                n.attributes["cy"] = number(0, 200);
                n.attributes["r"] = number(1, 40);
            } else if (n.tag == "line") {
                n.attributes["x1"] = number(0, 200);
                n.attributes["y1"] = number(0, 200);
                n.attributes["x2"] = number(0, 200);
                n.attributes["y2"] = number(0, 200);
            } else if (n.tag == "path") {
                n.attributes["d"] = broken ? "M 1 2 K 3" : path();
            } else if (n.tag == "polyline" || n.tag == "polygon") {
                n.attributes["points"] = number(0, 200) + "," + number(0, 200) + " " + number(0, 200) + "," +
                                         number(0, 200) + " " + number(0, 200) + "," + number(0, 200);
            } else if (n.tag == "text") {
                n.attributes["x"] = number(0, 200);
                n.attributes["y"] = number(10, 200);
                n.style["font-size"] = number(10, 24) + "px";
                n.style["text-anchor"] = below(2) ? "start" : "middle";
            }
        }

        weft::SceneNode element(std::uint32_t slot) {
            static const char* tags[]{"rect", "circle", "line", "path", "polyline", "polygon", "text", "g"};
            auto n = ::element(slot, tags[below(int(std::size(tags)))]);
            shape(n);
            n.style["fill"] = color();
            n.style["stroke"] = color();
            n.style["stroke-width"] = number(0, 4);
            if (n.tag == "text") {
                n.text = "Label " + number(0, 9);
            }
            if (n.tag == "g") {
                for (std::uint32_t k = 1; k <= 3; ++k) {
                    auto child = element(slot * 10 + k);
                    if (child.tag != "g") {
                        n.children.push_back(std::move(child));
                    }
                }
            }
            return n;
        }
    };

    // Every SVG element of a subtree, the element itself first.
    void elements(weft::SceneNode& n, std::vector<weft::SceneNode*>& out) {
        out.push_back(&n);
        for (auto& c : n.children.edit_all()) {
            elements(c, out);
        }
    }

    // The list of frame in view, with and without the cache, alike: its fingerprint, and its fallback.
    std::pair<std::string, std::string> both(weft::SceneDisplayCache& cache, std::uint64_t view,
                                             const weft::SceneFrame& frame, const std::string& what) {
        auto& text = weft::testing::thread_scene_text();
        cache.begin_pass(view);
        const auto listed = weft::scene_display_list(frame, {}, &text, 2, &cache);
        const auto with = fingerprint(listed);
        const auto without = fingerprint(weft::scene_display_list(frame, {}, &text, 2));
        require(with == without, what + ": the list with the cache is the list without it");
        return {with, listed.fallback};
    }

    void svg_values() {
        // Random scenes under random edits.
        std::size_t fallbacks{};
        for (unsigned seed = 1; seed <= 40; ++seed) {
            Random r(seed);
            std::vector<weft::SceneNode> scene;
            for (std::uint32_t i = 0; i < 12; ++i) {
                scene.push_back(r.element(100 + i));
            }
            auto frame = page(std::move(scene));
            weft::SceneDisplayCache cache;
            for (int step = 0; step < 30; ++step) {
                fallbacks += !both(cache, 0, frame, "seed " + std::to_string(seed)).second.empty();
                std::vector<weft::SceneNode*> all;
                for (auto& c : frame.root.children.edit(0).children.edit_all()) {
                    elements(c, all);
                }
                auto& n = *all[std::size_t(r.below(int(all.size())))];
                switch (r.below(6)) {
                    case 0:
                        // A new element in an old one's place.
                        n.identity.generation += 1;
                        break;
                    case 1:
                        // Unchanged.
                        break;
                    default:
                        r.edit(n);
                        break;
                }
            }
        }
        // An unchanged scene lists every element from the cache; an edit or a new identity takes one element anew.
        Random r(7);
        std::vector<weft::SceneNode> scene;
        for (std::uint32_t i = 0; i < 8; ++i) {
            auto n = element(200 + i, i % 2 ? "rect" : "circle");
            r.shape(n);
            n.style["fill"] = "#2a9d8f";
            scene.push_back(std::move(n));
        }
        auto frame = page(scene);
        weft::SceneDisplayCache cache;
        both(cache, 0, frame, "first");
        // The svg root is listed as a box, not by the cache: its eight elements are.
        require(cache.builds() == 8 && cache.reuses() == 0, "a first list takes every element's values");
        both(cache, 0, frame, "again");
        require(cache.builds() == 8 && cache.reuses() == 8, "an unchanged list keeps every element's values");
        frame.root.children.edit(0).children.edit(3).attributes["width"] = "33";
        both(cache, 0, frame, "edited");
        require(cache.builds() == 9 && cache.reuses() == 15, "an edited element is taken again");
        frame.root.children.edit(0).children.edit(5).identity.generation = 9;
        both(cache, 0, frame, "renewed");
        require(cache.builds() == 10 && cache.reuses() == 22, "a new element is taken anew");
        // Another view with the same identities keeps its own values: alternate views list from the cache.
        auto other = page(scene);
        for (auto& c : other.root.children.edit(0).children.edit_all()) {
            c.style["fill"] = "#e63946";
        }
        both(cache, 1, other, "other view");
        both(cache, 0, frame, "first view again");
        both(cache, 1, other, "other view again");
        require(cache.builds() == 18 && cache.reuses() == 38, "views keep their own values");
        // Values that fail are never kept: the frame falls back each time, with the same reason.
        frame.root.children.edit(0).children.edit(2).style["stroke"] = "#ffffff";
        frame.root.children.edit(0).children.edit(2).style["stroke-width"] = "wide";
        const auto failed = both(cache, 0, frame, "failing");
        const auto builds = cache.builds();
        require(!failed.second.empty() && both(cache, 0, frame, "failing again") == failed && cache.builds() == builds,
                "a failing element is never kept");
        // A small cache keeps what the current pass lists past its bound, and lets earlier passes' elements go.
        weft::SceneDisplayCache small(2, std::size_t(1) << 30);
        both(small, 0, page(scene), "small");
        both(small, 0, page(scene), "small again");
        require(small.builds() == 8 && small.reuses() == 8, "a pass over the bound keeps its elements");
        std::cout << "SVG values: 1200 random frames alike with and without the cache, " << fallbacks
                  << " of them falling back\n";
    }

    // Two nodes' keys against the equality they stand for, ignoring identity.
    void same(const weft::SceneNode& a, const weft::SceneNode& b, const std::string& what) {
        auto b_as_a = b;
        b_as_a.identity = a.identity;
        require((weft::scene_paint_source_key(a) == weft::scene_paint_source_key(b)) ==
                    weft::scene_paint_source_equal(a, b_as_a),
                what + ": the key as the equality");
    }

    void source_keys() {
        weft::SceneNode base;
        base.identity = {3, 1, 1};
        base.tag = "span";
        base.text = "Open: ";
        base.box = {0, 0, 40, 18};
        base.style["font-size"] = "12px";
        base.style["color"] = "#ffffff";
        base.attributes["class"] = "label";
        base.lines.push_back({"Open: ", {0, 0, 40, 18}, 14});
        base.paint.emplace();
        base.paint->values[weft::constants::scene::paint_slot_color] = weft::PaintValue{weft::PaintValue::Color};
        auto renamed = base;
        renamed.identity = {77, 4, 2};
        require(weft::scene_paint_source_key(base) == weft::scene_paint_source_key(renamed),
                "another node's same source has the same key");
        std::mt19937 engine(12);
        const auto edits = std::vector<std::function<void(weft::SceneNode&)>>{
            [](weft::SceneNode& n) {
                n.text += "x";
            },
            [](weft::SceneNode& n) {
                n.tag = "b";
            },
            [](weft::SceneNode& n) {
                n.space = weft::constants::svg_namespace_uri;
            },
            [](weft::SceneNode& n) {
                n.style["font-weight"] = "700";
            },
            [](weft::SceneNode& n) {
                n.style["color"] = "#fffffe";
            },
            [](weft::SceneNode& n) {
                n.attributes["title"] = "t";
            },
            [](weft::SceneNode& n) {
                n.attributes["class"] = "labels";
            },
            [](weft::SceneNode& n) {
                n.box[2] += 0.5f;
            },
            [](weft::SceneNode& n) {
                n.box[0] = -0.f;
            },
            [](weft::SceneNode& n) {
                n.lines[0].baseline = 15;
            },
            [](weft::SceneNode& n) {
                n.lines[0].text = "Open:";
            },
            [](weft::SceneNode& n) {
                n.fragments.push_back({{0, 0, 4, 4}, true, false});
            },
            [](weft::SceneNode& n) {
                n.paint->values[weft::constants::scene::paint_slot_color]->color[1] = 0.5;
            },
            [](weft::SceneNode& n) {
                n.paint->values[weft::constants::scene::paint_slot_opacity] = weft::PaintValue{};
            },
            [](weft::SceneNode& n) {
                n.paint.reset();
            },
            [](weft::SceneNode& n) {
                n.children.push_back(n);
            },
            [](weft::SceneNode& n) {
                n.box[1] = std::numeric_limits<float>::quiet_NaN();
            },
        };
        for (std::size_t i = 0; i < edits.size(); ++i) {
            auto edited = base;
            edits[i](edited);
            same(base, edited, "edit " + std::to_string(i));
            same(edited, edited, "edit " + std::to_string(i) + " against itself");
        }
        // Random pairs of edits.
        for (int k = 0; k < 400; ++k) {
            auto a = base;
            auto b = base;
            edits[std::size_t(engine() % edits.size())](a);
            edits[std::size_t(engine() % edits.size())](b);
            same(a, b, "random pair " + std::to_string(k));
        }
        // A node with an effect or particles shares its source with none.
        auto effected = base;
        effected.effect = std::make_shared<const weft::SceneEffect>();
        require(weft::scene_paint_source_key(effected) != weft::scene_paint_source_key(effected),
                "an effect's node has a key of its own");
        std::cout << "source keys: " << edits.size() << " edits and 400 random pairs as the equality\n";
    }

    // The path coverage of a frame's SVG elements, prepared twice and changed.
    void path_digests() {
        auto& text = weft::testing::thread_scene_text();
        weft::SceneNativeWork work;
        weft::ScenePreparer preparer(text, work, 4096);
        auto drawn = element(300, "path");
        drawn.attributes["d"] = "M10 10 L120 40 C140 80 60 120 20 90 Z";
        drawn.style["fill"] = "#2a9d8f";
        drawn.style["stroke"] = "#ffffff";
        drawn.style["stroke-width"] = "3";
        auto frame = page({drawn});
        const auto pass = [&] {
            require(bool(preparer.prepare(frame, 2, 640, 480, std::nullopt, 0, {})), "a native pass");
            return std::array<std::size_t, 2>{work.path_builds, work.path_reuses};
        };
        require(pass() == std::array<std::size_t, 2>{2, 0}, "a fill and a stroke drawn");
        require(pass() == std::array<std::size_t, 2>{2, 2}, "drawn again from the same path object");
        // Another element of the same path: another object, the same points.
        frame.root.children.edit(0).children.edit(0).identity.generation = 2;
        require(pass() == std::array<std::size_t, 2>{2, 4}, "another object of the same points");
        auto& n = frame.root.children.edit(0).children.edit(0);
        n.attributes["d"] = "M10 10 L120 41 C140 80 60 120 20 90 Z";
        require(pass() == std::array<std::size_t, 2>{4, 4}, "another point draws anew");
        n.style["stroke-dasharray"] = "4 2";
        require(pass() == std::array<std::size_t, 2>{5, 5}, "dashes draw the stroke anew");
        n.style["fill-rule"] = "evenodd";
        require(pass() == std::array<std::size_t, 2>{7, 5}, "another fill rule draws anew");
        // A map moved by whole device pixels (a unit at scale 2) covers what it covered, moved: the images stay. Moved
        // by part of a pixel, it draws anew.
        n.attributes["transform"] = "translate(1 0)";
        require(pass() == std::array<std::size_t, 2>{7, 7}, "a map moved by whole device pixels keeps its images");
        n.attributes["transform"] = "translate(1.3 0)";
        require(pass() == std::array<std::size_t, 2>{9, 7}, "another device map draws anew");
        n.style["stroke-width"] = "4";
        require(pass() == std::array<std::size_t, 2>{10, 8}, "another stroke width draws the stroke anew");
        // A narrower svg viewport clips what shows of the path, not its image: the same map, the same images (the
        // records' clips cut them).
        frame.root.children.edit(0).box[2] = 90;
        require(pass() == std::array<std::size_t, 2>{10, 10}, "a narrower viewport draws from the same images");
        // Past the root's edge too: an svg the target cuts keeps its path's image while it moves by whole device
        // pixels, though another part of the path shows.
        {
            weft::SceneNativeWork edge_work;
            weft::ScenePreparer edge(text, edge_work, 4096);
            auto across = element(400, "path");
            across.attributes["d"] = "M10 10 L120 40 L60 120 Z";
            across.style["fill"] = "#2a9d8f";
            auto cut = page({across});
            cut.root.children.edit(0).box = {250, 10, 300, 220};
            require(bool(edge.prepare(cut, 2, 640, 480, std::nullopt, 0, {})) && edge_work.path_builds == 1,
                    "a path the target cuts");
            cut.root.children.edit(0).box[0] = 249;
            require(bool(edge.prepare(cut, 2, 640, 480, std::nullopt, 0, {})) && edge_work.path_builds == 1 &&
                        edge_work.path_reuses == 1,
                    "moved by whole device pixels past the target's edge, it keeps its image");
        }
        // A stroked path past the viewport on every side: a map a fraction apart, or another width alone, draws it
        // anew.
        n.attributes["d"] = "M-60 -60 L400 -60 L400 400 L-60 400 Z M20 20 L60 90 L120 30";
        n.style["stroke-dasharray"] = "none";
        n.attributes.erase("transform");
        const auto covering = pass();
        n.attributes["transform"] = "translate(0.1 0.1)";
        require(pass() == std::array<std::size_t, 2>{covering[0] + 2, covering[1]},
                "a map a fraction apart draws anew");
        n.style["stroke-width"] = "5";
        require(pass() == std::array<std::size_t, 2>{covering[0] + 3, covering[1] + 1},
                "a width alone draws the stroke anew");
        // Paths of the same verbs and bounds and other points stay apart: drawn in turn, each is drawn once.
        n.attributes["d"] = "M10 10 L120 10 L120 120 L60 60 Z";
        const auto one = pass();
        n.attributes["d"] = "M10 10 L120 10 L120 120 L70 50 Z";
        const auto other = pass();
        n.attributes["d"] = "M10 10 L120 10 L120 120 L60 60 Z";
        const auto again = pass();
        require(other[0] == one[0] + 2 && again == std::array<std::size_t, 2>{other[0], other[1] + 2},
                "paths of the same verbs stay apart");
        std::cout << "path digests: reuse by object, by points and by maps moved by whole pixels, anew for points, "
                     "dashes, rules, maps, widths and masks\n";
    }
} // namespace

int main() {
    try {
        svg_values();
        source_keys();
        path_digests();
        std::cout << "PASS scene display caches: SVG values with and without the cache alike, source keys as the "
                     "equality, path coverage by digest\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
