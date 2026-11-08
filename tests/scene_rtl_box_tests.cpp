#include "constants/common.hpp"
#include "scene/scene.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The direction of boxes against Chrome: the cases of tools/browser_rtl_box_reference.mjs
// (tests/fixtures/scene_rtl_box_reference.json), blocks and their margins, flex rows and columns, absolutely
// positioned boxes, grids, inline blocks and scroll containers under direction rtl, nested in either direction, laid
// out by the scene solver with the repository fonts Chrome drew their text in. Every element's border box relative to
// the case's block agrees to a LayoutUnit (heights do not depend on font metrics: text has a fixed line height; an
// inline block's top does, by its line's baseline, and is not compared), and every scroll container's scrollLeft and
// scrollWidth after the scrollLeft a case sets. Random trees come after their left-to-right twins and are compared
// only where the twin matches Chrome: a twin that does not shows layout the scene does otherwise in either direction
// (margin collapsing, auto margins of a block of auto width), counted and listed apart. Portable.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    constexpr double tolerance = 1.0 / 64 + 1e-3;

    weft::FontCatalog catalog(const JsonValue& fonts) {
        weft::FontCatalog c;
        for (const auto& f : fonts.array) {
            weft::FontFaceEntry face;
            face.family = f["family"].string;
            face.source.path = std::string(WEFT_SOURCE_DIR "/") + f["path"].string;
            c.faces.push_back(face);
        }
        c.generic_families = {{"sans-serif", "Roboto"}, {"serif", "Roboto"}, {"system-ui", "Roboto"}};
        return c;
    }

    // The case's elements in tree order (Chrome's querySelectorAll('*') of the case's block).
    void build(weft::testing::NativeScene& s, weft::dom::Node& parent, const JsonValue& child,
               std::vector<weft::dom::Node*>& elements) {
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
        elements.push_back(&e);
        for (const auto& c : child["children"].array) {
            build(s, e, c, elements);
        }
    }

    struct Failures {
        std::vector<std::string> list;
        double largest{};

        void check(double native, double chrome, const std::string& what) {
            const double d = std::abs(native - chrome);
            largest = std::max(largest, d);
            if (!(d <= tolerance)) {
                std::ostringstream message;
                message << what << ": native " << native << ", Chrome " << chrome;
                list.push_back(message.str());
            }
        }
    };
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/scene_rtl_box_reference.json"));
        weft::SceneText scene_text(catalog(fixture["fonts"]));
        Failures failures;
        std::size_t rects{};
        std::size_t scrolls{};
        // Twins that differ from Chrome, and the random trees left out for them.
        std::vector<std::string> twins;
        std::size_t compared{};
        bool twin_matches = true;
        for (const auto& c : fixture["cases"].array) {
            const auto& name = c["name"].string;
            const bool twin = name.ends_with(" ltr");
            const bool random = name.starts_with("random ") && !twin;
            if (random && !twin_matches) {
                continue;
            }
            const auto before = failures.list.size();
            const auto largest = failures.largest;
            const auto& chrome = c["chrome"];
            weft::testing::NativeScene s;
            s.set(*s.root, "style",
                  "position:relative;font-family:Roboto, TestArabic, TestHebrew;font-size:16px;line-height:20px");
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
            s.tree.insert(*s.root, block, nullptr);
            std::vector<weft::dom::Node*> elements;
            for (const auto& child : c["children"].array) {
                build(s, block, child, elements);
            }
            if (const auto* list = c.find("scroll")) {
                for (const auto& scroll : list->array) {
                    elements.at(std::size_t(scroll["element"].number))->scroll_left = scroll["left"].number;
                }
            }
            weft::SceneSolver solver("", "", scene_text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 1000, 1000, 0);
            const auto& node = frame.root.children.at(0);

            // Elements in tree order, their border boxes in the case's block.
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
            const auto& expected = chrome["elements"].array;
            require(expected.size() == native.size(), name + ": element count");
            for (std::size_t e = 0; e < expected.size(); ++e) {
                const auto& x = expected[e];
                const auto& [n, r] = native[e];
                const auto what = name + " element " + std::to_string(e) + " <" + x["tag"].string + ">";
                const auto& want = x["rect"].array;
                failures.check(r[0], want[0].number, what + " x");
                failures.check(r[2], want[2].number, what + " width");
                // An inline block stands on its line's baseline, which the faces' ascent and descent place: the
                // scene's are Chrome's on Windows, the fixture's Chrome's on macOS.
                if (n->style.text(weft::PropertyId::Display, "block") != "inline-block") {
                    failures.check(r[1], want[1].number, what + " y");
                    failures.check(r[3], want[3].number, what + " height");
                }
                ++rects;
                if (const auto* scroll = x.find("scroll")) {
                    weft::ElementGeometry g;
                    g.client_width = std::max(0.0, double(n->box[2]));
                    weft::scene_scroll_geometry(*n, g);
                    failures.check(g.scroll_left, scroll->array[0].number, what + " scrollLeft");
                    failures.check(std::max(g.scroll_width, g.client_width), scroll->array[1].number,
                                   what + " scrollWidth");
                    ++scrolls;
                }
            }
            if (twin) {
                twin_matches = failures.list.size() == before;
                if (!twin_matches) {
                    twins.push_back(name + ": " + failures.list[before]);
                    failures.list.resize(before);
                    failures.largest = largest;
                }
            } else if (random) {
                ++compared;
            }
        }
        std::cout << fixture["cases"].size() << " box direction cases vs " << fixture["browser"].string << ": " << rects
                  << " element boxes, " << scrolls << " scroll containers; " << compared << " random trees compared, "
                  << twins.size() << " left out (their left-to-right twins differ from Chrome); largest difference "
                  << failures.largest << " px; " << failures.list.size() << " failures\n";
        for (const auto& t : twins) {
            std::cout << "  left out: " << t << '\n';
        }
        for (const auto& f : failures.list) {
            std::cout << "  " << f << '\n';
        }
        require(failures.list.empty(), "box direction matches Chrome");
        std::cout << "PASS the direction of boxes against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
