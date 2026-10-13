#include "constants/common.hpp"
#include "scene/scene.hpp"
#include "scene/scene_text.hpp"
#include "text/case_mapping.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Scene inline formatting contexts against Chrome: the cases of tools/browser_scene_inline_reference.mjs
// (tests/fixtures/scene_inline_reference.json), each a block of a given width laid out by the scene solver with this
// machine's fonts. For every case: the block's height; every element's client rects (an inline element's fragments,
// with their padding and borders); and for every text node, the text on each line, where it starts and ends, and the
// top of its content area (the baseline less the font's ascent). Horizontal positions agree to a LayoutUnit (both sides
// round each item's advances up to 1/64 px), vertical ones exactly.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    constexpr double horizontal_tolerance = 1.0 / 64 + 1e-3;
    constexpr double vertical_tolerance = 1e-3;

    void build(weft::testing::NativeScene& s, weft::dom::Node& parent, const JsonValue& child) {
        if (child.kind == JsonValue::Kind::String) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = child.string;
            s.tree.insert(parent, t, nullptr);
            return;
        }
        const auto& tag = child["tag"].string;
        auto& e = tag == "svg" ? s.svg("svg") : s.html(tag);
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
        double x{};
        double y{};

        void check(double native, double chrome, double tolerance, bool horizontal, const std::string& what) {
            const double d = std::abs(native - chrome);
            (horizontal ? x : y) = std::max(horizontal ? x : y, d);
            if (!(d <= tolerance)) {
                std::ostringstream message;
                message << what << ": native " << native << ", Chrome " << chrome;
                list.push_back(message.str());
            }
        }
    };

    // The text of a line without the spaces (and no-break spaces, as JavaScript's trim) it starts and ends with.
    std::string trimmed(std::string s) {
        constexpr std::string_view nbsp = "\xC2\xA0";
        while (!s.empty() && (s.front() == ' ' || s.starts_with(nbsp))) {
            s.erase(0, s.front() == ' ' ? 1 : nbsp.size());
        }
        while (!s.empty() && (s.back() == ' ' || s.ends_with(nbsp))) {
            s.erase(s.size() - (s.back() == ' ' ? 1 : nbsp.size()));
        }
        return s;
    }

    struct Text {
        const weft::SceneNode* owner{};
        std::array<double, 2> origin{};
    };

    // The scene nodes that hold the case's text nodes, in tree order: an element whose only child is a text node holds
    // that text itself; other text becomes #text children.
    void texts(const JsonValue& json, const weft::SceneNode& node, std::array<double, 2> origin,
               std::vector<Text>& out) {
        const auto& children = json["children"].array;
        if (children.size() == 1 && children[0].kind == JsonValue::Kind::String) {
            out.push_back({&node, origin});
            return;
        }
        std::size_t k{};
        for (const auto& c : children) {
            while (k < node.children.size() && c.kind == JsonValue::Kind::String && node.children[k].tag != "#text") {
                ++k;
            }
            require(k < node.children.size(), "scene child for each case child");
            const auto& child = node.children[k++];
            const std::array<double, 2> at{origin[0] + child.box[0], origin[1] + child.box[1]};
            if (c.kind == JsonValue::Kind::String) {
                out.push_back({&child, at});
            } else if (c["tag"].string != "svg") {
                texts(c, child, at, out);
            }
        }
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/scene_inline_reference.json"));
        auto& scene_text = weft::testing::thread_scene_text();
        Failures failures;
        std::size_t rects{};
        std::size_t lines{};
        for (const auto& c : fixture["cases"].array) {
            const auto& name = c["name"].string;
            const auto& chrome = c["chrome"];
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative");
            auto& block = s.html("div");
            s.set(block, "style", "width:" + std::to_string(int(c["width"].number)) + "px;" + c["style"].string);
            s.tree.insert(*s.root, block, nullptr);
            for (const auto& child : c["children"].array) {
                build(s, block, child);
            }
            weft::SceneSolver solver("", "", scene_text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 1000, 1000, 0);
            const auto& node = frame.root.children.at(0);
            failures.check(node.box[3], chrome["height"].number, vertical_tolerance, false, name + " height");

            // Elements in tree order, positioned in the block.
            std::vector<std::vector<std::array<double, 4>>> native;
            std::vector<std::string> tags;
            const auto walk = [&](auto&& self, const weft::SceneNode& n, double x, double y) -> void {
                for (const auto& child : n.children) {
                    if (child.tag == "#text") {
                        continue;
                    }
                    const double ax = x + child.box[0];
                    const double ay = y + child.box[1];
                    std::vector<std::array<double, 4>> r;
                    if (child.fragments.empty()) {
                        r.push_back({ax, ay, child.box[2], child.box[3]});
                    } else {
                        for (const auto& f : child.fragments) {
                            r.push_back({ax + f.rect[0], ay + f.rect[1], f.rect[2], f.rect[3]});
                        }
                    }
                    native.push_back(std::move(r));
                    tags.push_back(child.tag);
                    if (child.space != weft::constants::svg_namespace_uri) {
                        self(self, child, ax, ay);
                    }
                }
            };
            walk(walk, node, 0, 0);
            const auto& elements = chrome["elements"].array;
            require(elements.size() == native.size(), name + ": element count");
            for (std::size_t e = 0; e < elements.size(); ++e) {
                const auto& expected = elements[e]["rects"].array;
                const auto what = name + " element " + std::to_string(e) + " <" + tags[e] + ">";
                if (tags[e] == "br") {
                    continue;
                }
                if (expected.size() != native[e].size()) {
                    failures.list.push_back(what + ": " + std::to_string(native[e].size()) + " rects, Chrome " +
                                            std::to_string(expected.size()));
                    continue;
                }
                for (std::size_t r = 0; r < expected.size(); ++r) {
                    const auto& x = expected[r];
                    const auto& n = native[e][r];
                    const auto at = what + " rect " + std::to_string(r);
                    failures.check(n[0], x[0].number, horizontal_tolerance, true, at + " x");
                    failures.check(n[2], x[2].number, horizontal_tolerance, true, at + " width");
                    failures.check(n[1], x[1].number, vertical_tolerance, false, at + " y");
                    failures.check(n[3], x[3].number, vertical_tolerance, false, at + " height");
                    ++rects;
                }
            }

            std::vector<Text> owners;
            texts(c, node, {0, 0}, owners);
            const auto& chrome_texts = chrome["texts"].array;
            require(owners.size() == chrome_texts.size(), name + ": text count");
            for (std::size_t t = 0; t < owners.size(); ++t) {
                const auto& owner = *owners[t].owner;
                const auto& expected = chrome_texts[t].array;
                const auto what = name + " text " + std::to_string(t);
                std::vector<const weft::SceneLine*> visible;
                for (const auto& line : owner.lines) {
                    if (!trimmed(line.text).empty()) {
                        visible.push_back(&line);
                    }
                }
                if (visible.size() != expected.size()) {
                    std::string got;
                    for (const auto* line : visible) {
                        got += "[" + line->text + "]";
                    }
                    std::string want;
                    for (const auto& line : expected) {
                        want += "[" + line["text"].string + "]";
                    }
                    failures.list.push_back(what + ": lines " + got + ", Chrome " + want);
                    continue;
                }
                const auto ascent = scene_text.strut(weft::scene_font(owner.style)).ascent;
                for (std::size_t l = 0; l < expected.size(); ++l) {
                    const auto& line = *visible[l];
                    const auto& x = expected[l];
                    const auto at = what + " line " + std::to_string(l);
                    // Chrome's lines are the DOM's characters; the scene's are as text-transform shows them.
                    auto chrome_text = x["text"].string;
                    const auto transform = owner.style.text(weft::PropertyId::TextTransform, "none");
                    if (transform == "uppercase" || transform == "lowercase") {
                        chrome_text = weft::case_mapped(chrome_text, transform == "uppercase" ? weft::TextCase::Upper
                                                                                              : weft::TextCase::Lower);
                    }
                    if (trimmed(line.text) != chrome_text) {
                        failures.list.push_back(at + ": [" + line.text + "], Chrome [" + chrome_text + "]");
                        continue;
                    }
                    // Chrome's end is the last visible character's; a line's text may end in a space where another
                    // inline box follows on the line.
                    const double left = owners[t].origin[0] + line.rect[0];
                    auto visible_text = line.text;
                    while (!visible_text.empty() && visible_text.back() == ' ') {
                        visible_text.pop_back();
                    }
                    const double width = scene_text.shape(visible_text, weft::scene_font(owner.style))->metrics.width;
                    failures.check(left, x["left"].number, horizontal_tolerance, true, at + " start");
                    failures.check(left + width, x["right"].number, horizontal_tolerance, true, at + " end");
                    failures.check(owners[t].origin[1] + line.rect[1] + line.baseline - ascent, x["top"].number,
                                   vertical_tolerance, false, at + " top");
                    ++lines;
                }
            }
        }
        std::cout << fixture["cases"].size() << " inline cases vs " << fixture["browser"].string << ": " << rects
                  << " element rects, " << lines << " text lines; largest difference x " << failures.x << " px, y "
                  << failures.y << " px; " << failures.list.size() << " failures\n";
        for (const auto& f : failures.list) {
            std::cout << "  " << f << '\n';
        }
        require(failures.list.empty(), "scene inline layout matches Chrome");
        std::cout << "PASS scene inline formatting contexts against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
