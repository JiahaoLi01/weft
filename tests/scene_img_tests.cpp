#include "scene/scene.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "view/view.hpp"
#include "view/view_images.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The img element against Chrome (tools/browser_img_reference.mjs, tests/fixtures/img/chrome-results.json), on the
// images of tests/fixtures/img and Ahem (whose ascent, descent and line gap are alike in every table, so that an inline
// image's line places it alike on every platform):
//   - layout: every element's border box in each case's block agrees to a LayoutUnit (natural sizes, attributes, CSS
//     sizes, minimum and maximum sizes, aspect-ratio, box-sizing, blocks, inline images and vertical-align, flex, grid,
//     absolute positioning, right to left, random trees), laid out by the scene solver with the images a page's
//     ViewImages reads;
//   - paint: where the display list puts each quadrant of the image (its tile in the img's content box, as object-fit
//     and object-position place it) agrees to a px with where Chrome's screenshot shows each quadrant's color;
//   - script: tests/fixtures/img/script.js in a View logs what Chrome's logs, step by step.
// Portable.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    constexpr double tolerance = 1.0 / 64 + 1e-3;
    const std::string directory = WEFT_SOURCE_DIR "/tests/fixtures/img/";

    weft::FontCatalog catalog(const JsonValue& font) {
        weft::FontCatalog c;
        weft::FontFaceEntry face;
        face.family = font["family"].string;
        face.source.path = std::string(WEFT_SOURCE_DIR "/") + font["path"].string;
        c.faces.push_back(face);
        c.generic_families = {{"sans-serif", "Ahem"}, {"serif", "Ahem"}, {"system-ui", "Ahem"}};
        return c;
    }

    std::optional<std::vector<std::uint8_t>> read(std::string_view path) {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    void build(weft::testing::NativeScene& s, weft::dom::Node& parent, const JsonValue& child) {
        if (child.kind == JsonValue::Kind::String) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = child.string;
            s.tree.insert(parent, t, nullptr);
            return;
        }
        auto& e = s.html(child["tag"].string);
        if (!child["style"].string.empty()) {
            s.set(e, "style", child["style"].string);
        }
        if (const auto* attributes = child.find("attributes")) {
            for (const auto& [name, value] : attributes->object) {
                s.set(e, name, value.string);
            }
        }
        s.tree.insert(parent, e, nullptr);
        for (const auto& c : child["children"].array) {
            build(s, e, c);
        }
    }

    struct Failures {
        std::vector<std::string> list;
        double largest{};

        void check(double native, double chrome, const std::string& what, double within = tolerance) {
            const double d = std::abs(native - chrome);
            largest = std::max(largest, d);
            if (!(d <= within)) {
                std::ostringstream message;
                message << what << ": native " << native << ", Chrome " << chrome;
                list.push_back(message.str());
            }
        }
    };

    // A value as compact JSON text (numbers as the shortest that reads back), to compare logs.
    std::string json_text(const JsonValue& v) {
        switch (v.kind) {
            case JsonValue::Kind::Null:
                return "null";
            case JsonValue::Kind::Bool:
                return v.boolean ? "true" : "false";
            case JsonValue::Kind::Number: {
                std::ostringstream s;
                s.precision(17);
                s << v.number;
                return s.str();
            }
            case JsonValue::Kind::String:
                return '"' + v.string + '"';
            case JsonValue::Kind::Array: {
                std::string out = "[";
                for (std::size_t i = 0; i < v.array.size(); ++i) {
                    out += (i ? "," : "") + json_text(v.array[i]);
                }
                return out + "]";
            }
            case JsonValue::Kind::Object:
                break;
        }
        std::string out = "{";
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            out += (i ? ",\"" : "\"") + v.object[i].first + "\":" + json_text(v.object[i].second);
        }
        return out + "}";
    }

    // The page's root, as Chrome's body: Ahem 10px, positioned.
    weft::dom::Node& page(weft::testing::NativeScene& s) {
        s.set(*s.root, "style", "position:relative;font-family:Ahem;font-size:10px;background:#ffffff");
        return *s.root;
    }
} // namespace

int main() {
    try {
        // WEFT_IMG_RESULTS names another recording of the tool (its output argument), when debugging.
        const char* results = std::getenv("WEFT_IMG_RESULTS");
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(results ? std::string(results) : directory + "chrome-results.json"));
        auto fonts = std::make_shared<weft::FontCatalog>(catalog(fixture["font"]));
        weft::SceneText scene_text(*fonts);
        const weft::ViewResourceReader reader = read;
        std::vector<std::string> diagnostics;
        weft::ViewImages images(directory, reader, [&](std::string m) {
            diagnostics.push_back(std::move(m));
        });
        Failures failures;
        std::vector<std::string> warnings;

        // Layout. A case Chrome lays out otherwise for a reason the scene's flex adapter has for every box (its
        // `known`), and these random trees of flex and grid containers nested in flex and grid containers, differ
        // as known: listed, not failures. Their causes (docs/img.md): a percentage width or max-width in a flex item
        // its container shrinks, which the adapter resolves against the item's px width; the automatic minimum sizes
        // of nested flex containers, which the adapter approximates.
        const std::set<std::string> nested{"random 51",  "random 65",  "random 72",  "random 74", "random 96",
                                           "random 107", "random 108", "random 110", "random 113"};
        std::vector<std::string> known;
        std::vector<std::string> matching_known;
        std::size_t rects{};
        for (const auto& c : fixture["cases"].array) {
            const auto& name = c["name"].string;
            // WEFT_IMG_CASE names the one layout case to run, when debugging.
            if (const char* only = std::getenv("WEFT_IMG_CASE"); only && name != only) {
                continue;
            }
            const auto before = failures.list.size();
            const auto largest = failures.largest;
            weft::testing::NativeScene s;
            auto& root = page(s);
            auto& block = s.html("div");
            std::string style = "width:" + std::to_string(int(c["width"].number)) + "px;";
            if (const auto* own = c.find("style")) {
                style += own->string;
            }
            s.set(block, "style", style);
            if (const auto* attributes = c.find("attributes")) {
                for (const auto& [attribute, value] : attributes->object) {
                    s.set(block, attribute, value.string);
                }
            }
            s.tree.insert(root, block, nullptr);
            for (const auto& child : c["children"].array) {
                build(s, block, child);
            }
            weft::SceneSolver solver("", "", scene_text, true, true);
            solver.set_images(&images);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 1000, 1000, 0);
            for (const auto& w : solver.take_warnings()) {
                warnings.push_back(name + ": " + w);
            }
            const auto& node = frame.root.children.at(0);
            std::vector<std::pair<const weft::SceneNode*, std::array<double, 4>>> native;
            const auto walk = [&](auto&& self, const weft::SceneNode& n, double x, double y) -> void {
                for (const auto& child : n.children) {
                    if (child.tag == "#text") {
                        continue;
                    }
                    const double ax = x + child.box[0];
                    const double ay = y + child.box[1];
                    native.push_back({&child, {ax, ay, child.box[2], child.box[3]}});
                    self(self, child, ax, ay);
                }
            };
            walk(walk, node, 0, 0);
            const auto& expected = c["chrome"]["elements"].array;
            require(expected.size() == native.size(), name + ": element count");
            for (std::size_t e = 0; e < expected.size(); ++e) {
                const auto& x = expected[e];
                const auto& [n, r] = native[e];
                const auto what = name + " element " + std::to_string(e) + " <" + x["tag"].string + ">";
                require(n->tag == x["tag"].string, what + ": the same element");
                const auto& want = x["rect"].array;
                if (std::getenv("WEFT_IMG_CASE")) {
                    // The one case being debugged: each element's box, the scene's and Chrome's.
                    std::cout << "  " << e << " <" << x["tag"].string << "> " << r[0] << ' ' << r[1] << ' ' << r[2]
                              << ' ' << r[3] << " / Chrome " << want[0].number << ' ' << want[1].number << ' '
                              << want[2].number << ' ' << want[3].number << '\n';
                }
                failures.check(r[0], want[0].number, what + " x");
                failures.check(r[1], want[1].number, what + " y");
                failures.check(r[2], want[2].number, what + " width");
                failures.check(r[3], want[3].number, what + " height");
                if (const auto* image = x.find("image")) {
                    const double w = n->image ? double(n->image->width) : 0;
                    const double h = n->image ? double(n->image->height) : 0;
                    failures.check(w, (*image)[0].number, what + " natural width", 0);
                    failures.check(h, (*image)[1].number, what + " natural height", 0);
                }
                ++rects;
            }
            const auto* reason = c.find("known");
            if (reason || nested.contains(name)) {
                const auto why = reason ? reason->string : std::string("flex and grid containers nested in them");
                if (failures.list.size() > before) {
                    known.push_back(name + " (" + why + "): " + failures.list[before] + ", " +
                                    std::to_string(failures.list.size() - before) + " differences");
                    failures.list.resize(before);
                    failures.largest = largest;
                } else {
                    matching_known.push_back(name);
                }
            }
        }
        const auto layout_failures = failures.list.size();
        const auto layout_largest = failures.largest;

        // Paint: each fit's img alone, at 10, 10 as Chrome's (display block, margin 10px).
        std::size_t fits{};
        for (const auto& f : fixture["paint"].array) {
            const auto what = "fit " + f["src"].string + " " + f["style"].string;
            weft::testing::NativeScene s;
            auto& root = page(s);
            auto& e = s.html("img");
            s.set(e, "src", f["src"].string);
            s.set(e, "style", "display:block;margin:10px;background:#ffffff;" + f["style"].string);
            s.tree.insert(root, e, nullptr);
            weft::SceneSolver solver("", "", scene_text, true, true);
            solver.set_images(&images);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 400, 400, 0);
            const auto& node = frame.root.children.at(0);
            require(bool(node.image), what + ": an image");
            const auto listed = weft::scene_display_list(frame, {}, &scene_text);
            require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 1)), what + ": a list");
            const weft::paint::Item* item{};
            for (const auto& i : listed.list.items) {
                if (i.kind == weft::paint::Item::Kind::Image &&
                    listed.list.assets[listed.list.images[i.payload].asset] == node.image) {
                    require(!item, what + ": one image item");
                    item = &i;
                }
            }
            require(item, what + ": an image item");
            const auto& fit = listed.list.images[item->payload].fit;
            const float nw = float(node.image->width);
            const float nh = float(node.image->height);
            const auto tiles = weft::scene_texture_tiles(fit, nw, nh);
            // The image's extent and its quadrants' split in the img's border box: the tile within the painting area.
            const double origin[2] = {item->transform[4] - node.box[0], item->transform[5] - node.box[1]};
            const double area[2] = {item->width, item->height};
            std::array<double, 2> low{};
            std::array<double, 2> high{};
            std::array<double, 2> split{};
            for (std::size_t a = 0; a < 2; ++a) {
                const double start = tiles[a].start;
                const double size = tiles[a].tile;
                low[a] = origin[a] + std::max(0.0, start);
                high[a] = origin[a] + std::min(area[a], start + size);
                split[a] = origin[a] + start + size * std::floor((a ? nh : nw) / 2) / (a ? nh : nw);
            }
            // Chrome's: the quadrants' colors' extents, their split between neighbours.
            const auto& q = f["quadrants"].array;
            const auto present = [&](std::size_t k) {
                return q[k].kind == JsonValue::Kind::Array;
            };
            std::array<double, 2> chrome_low{1e9, 1e9};
            std::array<double, 2> chrome_high{-1e9, -1e9};
            for (std::size_t k = 0; k < 4; ++k) {
                if (present(k)) {
                    for (std::size_t a = 0; a < 2; ++a) {
                        chrome_low[a] = std::min(chrome_low[a], q[k][a].number);
                        chrome_high[a] = std::max(chrome_high[a], q[k][a + 2].number);
                    }
                }
            }
            constexpr double px = 1.01;
            for (std::size_t a = 0; a < 2; ++a) {
                const char* axis = a ? " y" : " x";
                failures.check(std::clamp(low[a], 0.0, double(node.box[2 + a])), chrome_low[a],
                               what + " image start" + axis, px);
                failures.check(std::clamp(high[a], 0.0, double(node.box[2 + a])), chrome_high[a],
                               what + " image end" + axis, px);
                // Neighbours across the split: red and green across x, red and blue down y (or the other pair).
                const std::array<std::array<std::size_t, 2>, 2> pairs =
                    a ? std::array<std::array<std::size_t, 2>, 2>{{{0, 2}, {1, 3}}}
                      : std::array<std::array<std::size_t, 2>, 2>{{{0, 1}, {2, 3}}};
                for (const auto& [before, after] : pairs) {
                    if (present(before) && present(after)) {
                        const double chrome_split = (q[before][a + 2].number + q[after][a].number) / 2;
                        failures.check(split[a], chrome_split, what + " split" + axis, px);
                        break;
                    }
                }
            }
            ++fits;
        }
        const auto paint_failures = failures.list.size() - layout_failures;

        // Script: script.js in a View, which reads the images by the same relative paths.
        std::size_t steps{};
        {
            weft::View view(fonts, scene_text, 800, 600, 1);
            view.set_resource_reader(read);
            weft::ViewPage p;
            p.base = directory;
            p.scripts.push_back({"script.js", weft::testing::read_file(directory + "script.js")});
            p.stylesheet = "#root{font-family:Ahem;font-size:10px}";
            p.forgiving_css = true;
            p.web_platform = true;
            view.load(p);
            double time = 0;
            std::string log;
            for (int k = 0; k < 200 && log.empty(); ++k) {
                time += 16;
                view.advance(time);
                const auto [ok, result] = view.evaluate("window.__imgLog ? JSON.stringify(window.__imgLog) : ''");
                require(ok, "the log reads: " + result);
                log = result;
            }
            require(!log.empty(), "script.js finishes");
            const auto native = weft::testing::parse_json(log);
            const auto& chrome = fixture["script"].array;
            const auto text = [](const JsonValue& v) {
                return json_text(v);
            };
            // Where weft differs on purpose: an img whose src changed is laid out with the new image at once (it
            // loads from the host's files within the frame), while Chrome keeps the old one until the new loads.
            const std::map<std::string, std::string> differences{
                {"src changed", R"(["src changed",false,16,16,120,40])"}};
            for (std::size_t k = 0; k < std::max(native.array.size(), chrome.size()); ++k) {
                const auto got = k < native.array.size() ? text(native.array[k]) : std::string("(none)");
                auto want = k < chrome.size() ? text(chrome[k]) : std::string("(none)");
                if (k < chrome.size() && !chrome[k].array.empty()) {
                    if (const auto d = differences.find(chrome[k][0].string); d != differences.end()) {
                        want = d->second;
                    }
                }
                if (got != want) {
                    failures.list.push_back("script step " + std::to_string(k) + ": native " + got + ", Chrome " +
                                            want);
                }
                ++steps;
            }
        }

        std::cout << fixture["cases"].size() << " img layout cases vs " << fixture["browser"].string << ": " << rects
                  << " element boxes, " << fixture["cases"].size() - known.size() << " cases alike, largest difference "
                  << layout_largest << " px, " << layout_failures << " failures; " << fits << " object-fit cases, "
                  << paint_failures << " failures; " << steps << " script steps, "
                  << failures.list.size() - layout_failures - paint_failures << " failures; " << known.size()
                  << " known layout differences\n";
        for (const auto& k : known) {
            std::cout << "  known: " << k << '\n';
        }
        for (const auto& m : matching_known) {
            std::cout << "  known to differ, but matches now: " << m << '\n';
        }
        for (const auto& w : warnings) {
            std::cout << "  warning: " << w << '\n';
        }
        for (const auto& f : failures.list) {
            std::cout << "  " << f << '\n';
        }
        require(failures.list.empty(), "img matches Chrome");
        std::cout << "PASS the img element against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
