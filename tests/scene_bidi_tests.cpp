#include "constants/common.hpp"
#include "scene/scene.hpp"
#include "scene/scene_inline.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// Bidirectional text and complex scripts in scene inline formatting contexts against Chrome: the cases of
// tools/browser_bidi_reference.mjs (tests/fixtures/scene_bidi_reference.json), each a block of a given width of
// Arabic, Hebrew, Thai and Devanagari text with Latin, inline boxes and atomic inlines under direction, unicode-bidi,
// dir, bdi and bdo, laid out by the scene solver with the faces Chrome drew them in (the repository's HarfBuzz test
// fonts). For every case: every element's client rects (an inline element's fragments, split where reordering
// separates its content, with its padding and borders on the sides Chrome puts them; a box Chrome culls, one without
// borders, padding or margins, reports its text's fragments, so its extent on each line is compared); and every text
// node's fragments, a rect per line and per run of one bidi level: where each starts and how wide it is (its glyphs'
// advances: the shaping). Horizontal positions agree to a LayoutUnit. Vertically each rect is compared by the line it
// lies on: the scene's line metrics are Chrome's on Windows (the faces' OS/2 Win or typo metrics, DirectWrite's), the
// fixture is Chrome's on macOS (hhea metrics), so heights differ by the faces' metrics, not by bidi. Portable: no
// system font.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    constexpr double horizontal_tolerance = 1.0 / 64 + 1e-3;
    // A character's rect: Chrome snaps its left and right to LayoutUnits, each a LayoutUnit apart at most.
    constexpr double character_tolerance = 1.0 / 64 + 1e-3;

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

    struct Text {
        const weft::SceneNode* owner{};
        std::array<double, 2> origin{};
        // The DOM's text.
        std::string raw;
    };

    // The scene nodes that hold the case's text nodes, in tree order: an element whose only child is a text node holds
    // that text itself; other text becomes #text children.
    void texts(const JsonValue& json, const weft::SceneNode& node, std::array<double, 2> origin,
               std::vector<Text>& out) {
        const auto& children = json["children"].array;
        if (children.size() == 1 && children[0].kind == JsonValue::Kind::String) {
            out.push_back({&node, origin, children[0].string});
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
                out.push_back({&child, at, c.string});
            } else {
                texts(c, child, at, out);
            }
        }
    }

    // Each character that is a cluster of its own in a line of owner's text, as drawing shapes the line
    // (scene_text_line: one run of its direction): where its glyphs start and how far they advance, against Chrome's
    // rect of that character ([UTF-16 offset, left, top, width, height]). Its visual order within the run and the
    // run's direction show here.
    // Where needle lies in raw text from `from` on, a space of it matching a run of spaces there (white-space:
    // normal collapses them): each of needle's bytes at its byte of raw; empty when it does not.
    std::vector<std::size_t> find_collapsed(const std::string& raw, const std::string& needle, std::size_t from) {
        for (auto start = from; start < raw.size(); ++start) {
            std::vector<std::size_t> at;
            auto r = start;
            bool ok = true;
            for (std::size_t j = 0; ok && j < needle.size(); ++j) {
                if (r >= raw.size()) {
                    ok = false;
                } else if (needle[j] == ' ' && raw[r] == ' ') {
                    at.push_back(r);
                    while (r < raw.size() && raw[r] == ' ') {
                        ++r;
                    }
                } else if (needle[j] == raw[r]) {
                    at.push_back(r++);
                } else {
                    ok = false;
                }
            }
            if (ok) {
                at.push_back(r);
                return at;
            }
        }
        return {};
    }

    void compare_characters(weft::SceneText& text, const weft::SceneNode& owner, const std::string& raw,
                            std::array<double, 2> origin, const JsonValue& chars, const std::string& what,
                            Failures& failures, std::size_t& count) {
        // The UTF-16 offset of each byte of the DOM's text.
        std::vector<std::size_t> utf16(raw.size() + 1);
        std::size_t units{};
        for (std::size_t b = 0; b < raw.size();) {
            const auto lead = static_cast<unsigned char>(raw[b]);
            const std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            for (std::size_t k = 0; k < length && b + k < raw.size(); ++k) {
                utf16[b + k] = units;
            }
            units += length == 4 ? 2 : 1;
            b += length;
        }
        utf16[raw.size()] = units;
        std::size_t cursor{};
        for (std::size_t i = 0; i < owner.lines.size(); ++i) {
            const auto& line = owner.lines[i];
            const auto map = line.text.empty() ? std::vector<std::size_t>{} : find_collapsed(raw, line.text, cursor);
            if (map.empty()) {
                continue;
            }
            cursor = map.back();
            const auto node = weft::scene_text_line(owner, i);
            const auto run = node.text_run ? node.text_run
                                           : text.shape(line.text, weft::scene_font(node.style),
                                                        weft::ShapeContext{node.text_context[0], node.text_context[1]});
            std::vector<std::uint32_t> starts;
            for (const auto& g : run->glyphs) {
                starts.push_back(g.utf8_cluster);
            }
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
            for (std::size_t k = 0; k < starts.size(); ++k) {
                const auto begin = starts[k];
                const auto end = k + 1 < starts.size() ? starts[k + 1] : std::uint32_t(line.text.size());
                // A cluster of one code point.
                const auto lead = static_cast<unsigned char>(line.text[begin]);
                const std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                if (end - begin != length) {
                    continue;
                }
                // Its glyphs' pen positions (the advances before them in the run's visual order; a glyph's x adds its
                // offset), and their advances.
                double left = std::numeric_limits<double>::infinity();
                double advance{};
                double pen{};
                for (const auto& g : run->glyphs) {
                    if (g.utf8_cluster == begin) {
                        left = std::min(left, pen);
                        advance += g.advance;
                    }
                    pen += g.advance;
                }
                const auto offset = utf16[map[begin]];
                for (const auto& c : chars.array) {
                    if (std::size_t(c[0].number) != offset) {
                        continue;
                    }
                    const auto where = what + " character " + std::to_string(offset) + " '" +
                                       line.text.substr(begin, end - begin) + "'" + (line.rtl ? " rtl" : "");
                    failures.check(origin[0] + line.rect[0] + left, c[1].number, character_tolerance, true,
                                   where + " left");
                    failures.check(advance, c[3].number, 2 * character_tolerance, true, where + " width");
                    ++count;
                }
            }
        }
    }

    // The ink of a node's lines as drawing shapes them (scene_text_line), across: from the leftmost to the rightmost
    // column a glyph covers, in the case's block; lines whose text is `only` (or all but it when `except`).
    std::optional<std::array<double, 2>> ink(weft::SceneText& text, const weft::SceneNode& owner,
                                             std::array<double, 2> origin, const std::string& only, bool except) {
        std::optional<std::array<double, 2>> out;
        for (std::size_t i = 0; i < owner.lines.size(); ++i) {
            const auto& line = owner.lines[i];
            if (line.text.empty() || (line.text == only) == except) {
                continue;
            }
            const auto node = weft::scene_text_line(owner, i);
            const auto run = node.text_run ? node.text_run
                                           : text.shape(line.text, weft::scene_font(node.style),
                                                        weft::ShapeContext{node.text_context[0], node.text_context[1]});
            for (const auto& g : run->glyphs) {
                const auto bitmap = text.face(g.face).rasterize(g.glyph_id, run->size);
                if (!bitmap.width) {
                    continue;
                }
                const double lo = origin[0] + line.rect[0] + g.x + bitmap.left;
                const double hi = lo + bitmap.width;
                out = out ? std::array<double, 2>{std::min((*out)[0], lo), std::max((*out)[1], hi)}
                          : std::array<double, 2>{lo, hi};
            }
        }
        return out;
    }

    // A rect, its top replaced by the index of the line it lies on.
    using Rect = std::array<double, 4>;

    // The tops of a case's lines: the distinct tops of its text fragments and inline boxes' fragments (each line's
    // text and boxes share the primary font's content area: every case's text is one size), also of a line with no
    // text.
    std::vector<double> line_tops(const std::vector<std::vector<Rect>>& texts,
                                  const std::vector<std::vector<Rect>>& boxes) {
        std::vector<double> tops;
        for (const auto* rects : {&texts, &boxes}) {
            for (const auto& t : *rects) {
                for (const auto& r : t) {
                    // A rect of no width (a box's end alone, a break) makes no line.
                    if (r[2] > 0) {
                        tops.push_back(r[1]);
                    }
                }
            }
        }
        std::sort(tops.begin(), tops.end());
        std::vector<double> lines;
        for (const auto top : tops) {
            if (lines.empty() || top - lines.back() > 4) {
                lines.push_back(top);
            }
        }
        return lines;
    }

    // Each rect's top as the index of the nearest line, its height dropped; sorted by line, then left to right.
    void by_line(std::vector<Rect>& rects, const std::vector<double>& lines) {
        for (auto& r : rects) {
            const double middle = r[1] + r[3] / 2;
            std::size_t best{};
            for (std::size_t l = 0; l < lines.size(); ++l) {
                if (std::abs(lines[l] + 12 - middle) < std::abs(lines[best] + 12 - middle)) {
                    best = l;
                }
            }
            r[1] = double(best);
            r[3] = 0;
        }
        std::sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) {
            return a[1] != b[1] ? a[1] < b[1] : a[0] < b[0];
        });
    }

    // A culled box's rects as one extent per line.
    std::vector<Rect> per_line(const std::vector<Rect>& rects) {
        std::vector<Rect> out;
        for (const auto& r : rects) {
            if (!out.empty() && out.back()[1] == r[1]) {
                const double right = std::max(out.back()[0] + out.back()[2], r[0] + r[2]);
                out.back()[0] = std::min(out.back()[0], r[0]);
                out.back()[2] = right - out.back()[0];
            } else {
                out.push_back(r);
            }
        }
        return out;
    }

    std::string list(const std::vector<Rect>& rects) {
        std::ostringstream s;
        for (const auto& r : rects) {
            s << "[" << r[0] << " " << r[1] << " " << r[2] << " " << r[3] << "]";
        }
        return s.str();
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/scene_bidi_reference.json"));
        weft::SceneText scene_text(catalog(fixture["fonts"]));
        Failures failures;
        std::size_t rects{};
        std::size_t fragments{};
        std::size_t characters{};
        std::size_t inks{};
        for (const auto& c : fixture["cases"].array) {
            const auto& name = c["name"].string;
            const auto& chrome = c["chrome"];
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative");
            auto& block = s.html("div");
            s.set(block, "style", "width:" + std::to_string(int(c["width"].number)) + "px;" + c["style"].string);
            if (const auto* attributes = c.find("attributes")) {
                for (const auto& [attribute, value] : attributes->object) {
                    s.set(block, attribute, value.string);
                }
            }
            s.tree.insert(*s.root, block, nullptr);
            for (const auto& child : c["children"].array) {
                build(s, block, child);
            }
            weft::SceneSolver solver("", "", scene_text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), 1000, 1000, 0);
            const auto& node = frame.root.children.at(0);

            // text-overflow: ellipsis: where the ellipsis's ink and the kept text's ink lie (Chrome's from a screenshot: the
            // block's color is the ellipsis's only).
            if (const auto* inked = chrome.find("ink")) {
                std::vector<Text> owners;
                texts(c, node, {0, 0}, owners);
                const std::string ellipsis = "\xE2\x80\xA6";
                const auto mark = ink(scene_text, node, {0, 0}, ellipsis, false);
                std::optional<std::array<double, 2>> kept;
                for (const auto& o : owners) {
                    if (const auto r = ink(scene_text, *o.owner, o.origin, ellipsis, true)) {
                        kept = kept
                                   ? std::array<double, 2>{std::min((*kept)[0], (*r)[0]), std::max((*kept)[1], (*r)[1])}
                                   : *r;
                    }
                }
                require(mark && kept, name + ": an ellipsis and text drawn");
                // Chrome's ranges are the columns of fully dark or red pixels; a glyph's coverage reaches further by
                // its antialiased edges.
                constexpr double ink_tolerance = 2;
                const auto near = [&](double native, double chrome, const std::string& what) {
                    if (std::abs(native - chrome) > ink_tolerance) {
                        std::ostringstream m;
                        m << name << " " << what << ": native " << native << ", Chrome " << chrome;
                        failures.list.push_back(m.str());
                    }
                };
                near((*mark)[0], (*inked)["ellipsis"][0].number, "ellipsis ink left");
                near((*mark)[1], (*inked)["ellipsis"][1].number, "ellipsis ink right");
                near(std::max(0.0, (*kept)[0]), (*inked)["text"][0].number, "text ink left");
                near(std::min(double(c["width"].number), (*kept)[1]), (*inked)["text"][1].number, "text ink right");
                ++inks;
                continue;
            }

            // Text fragments: a line's text of one level, its left and width; the top of its content area (the
            // primary font's ascent above the baseline, as Chrome's rect) places it on its line.
            std::vector<Text> owners;
            texts(c, node, {0, 0}, owners);
            const auto& chrome_texts = chrome["texts"].array;
            require(owners.size() == chrome_texts.size(), name + ": text count");
            std::vector<std::vector<Rect>> native_texts;
            std::vector<std::vector<Rect>> expected_texts;
            std::vector<std::string> line_texts;
            for (std::size_t t = 0; t < owners.size(); ++t) {
                const auto& owner = *owners[t].owner;
                const auto strut = scene_text.strut(weft::scene_font(owner.style));
                std::vector<Rect> expected;
                for (const auto& r : chrome_texts[t]["fragments"].array) {
                    expected.push_back({r[0].number, r[1].number, r[2].number, r[3].number});
                }
                std::vector<Rect> got;
                std::string seen;
                compare_characters(scene_text, owner, owners[t].raw, owners[t].origin, chrome_texts[t]["chars"],
                                   name + " text " + std::to_string(t), failures, characters);
                for (const auto& line : owner.lines) {
                    seen += "[" + line.text + (line.rtl ? " rtl" : "") + "]";
                    if (line.text.empty() || line.rect[2] <= 0) {
                        continue;
                    }
                    got.push_back({owners[t].origin[0] + line.rect[0],
                                   owners[t].origin[1] + line.rect[1] + line.baseline - strut.ascent, line.rect[2],
                                   strut.ascent + strut.descent});
                }
                native_texts.push_back(std::move(got));
                expected_texts.push_back(std::move(expected));
                line_texts.push_back(seen);
            }
            if (const char* debug = std::getenv("WEFT_BIDI_DEBUG"); debug && name == debug) {
                for (std::size_t t = 0; t < owners.size(); ++t) {
                    std::cout << "text " << t << ":";
                    for (const auto& line : owners[t].owner->lines) {
                        std::cout << " [" << line.text << "|" << (line.rtl ? "rtl " : "")
                                  << line.rect[0] + owners[t].origin[0] << "," << line.rect[1] + owners[t].origin[1]
                                  << " w" << line.rect[2] << "]";
                    }
                    std::cout << "\n";
                }
            }
            // Elements in tree order, positioned in the block.
            std::vector<std::vector<Rect>> native;
            std::vector<const weft::SceneNode*> nodes;
            const auto walk = [&](auto&& self, const weft::SceneNode& n, double x, double y) -> void {
                for (const auto& child : n.children) {
                    if (child.tag == "#text") {
                        continue;
                    }
                    const double ax = x + child.box[0];
                    const double ay = y + child.box[1];
                    std::vector<Rect> r;
                    if (child.fragments.empty()) {
                        r.push_back({ax, ay, child.box[2], child.box[3]});
                    } else {
                        for (const auto& f : child.fragments) {
                            r.push_back({ax + f.rect[0], ay + f.rect[1], f.rect[2], f.rect[3]});
                        }
                    }
                    native.push_back(std::move(r));
                    nodes.push_back(&child);
                    self(self, child, ax, ay);
                }
            };
            walk(walk, node, 0, 0);
            const auto& elements = chrome["elements"].array;
            require(elements.size() == native.size(), name + ": element count");
            // The inline boxes' rects (not atomic inlines', which stand on the baseline at their own height).
            std::vector<std::vector<Rect>> native_boxes;
            std::vector<std::vector<Rect>> chrome_boxes;
            for (std::size_t e = 0; e < elements.size(); ++e) {
                if (nodes[e]->style.text(weft::PropertyId::Display, "inline") != "inline" || nodes[e]->tag == "br") {
                    continue;
                }
                native_boxes.push_back(native[e]);
                std::vector<Rect> expected;
                for (const auto& r : elements[e]["rects"].array) {
                    expected.push_back({r[0].number, r[1].number, r[2].number, r[3].number});
                }
                chrome_boxes.push_back(std::move(expected));
            }
            const auto native_lines = line_tops(native_texts, native_boxes);
            const auto chrome_lines = line_tops(expected_texts, chrome_boxes);
            if (native_lines.size() != chrome_lines.size()) {
                failures.list.push_back(name + ": " + std::to_string(native_lines.size()) + " lines, Chrome " +
                                        std::to_string(chrome_lines.size()));
                continue;
            }
            for (std::size_t t = 0; t < owners.size(); ++t) {
                auto& got = native_texts[t];
                auto& expected = expected_texts[t];
                by_line(got, native_lines);
                by_line(expected, chrome_lines);
                const auto what = name + " text " + std::to_string(t);
                if (expected.size() != got.size()) {
                    failures.list.push_back(what + ": fragments " + list(got) + " " + line_texts[t] + ", Chrome " +
                                            list(expected));
                    continue;
                }
                for (std::size_t r = 0; r < expected.size(); ++r) {
                    const auto at = what + " fragment " + std::to_string(r);
                    failures.check(got[r][0], expected[r][0], horizontal_tolerance, true, at + " x");
                    failures.check(got[r][2], expected[r][2], horizontal_tolerance, true, at + " width");
                    failures.check(got[r][1], expected[r][1], 0, false, at + " line");
                    ++fragments;
                }
            }

            for (std::size_t e = 0; e < elements.size(); ++e) {
                const auto& tag = nodes[e]->tag;
                if (tag == "br") {
                    continue;
                }
                std::vector<Rect> expected;
                for (const auto& r : elements[e]["rects"].array) {
                    expected.push_back({r[0].number, r[1].number, r[2].number, r[3].number});
                }
                auto got = native[e];
                by_line(expected, chrome_lines);
                by_line(got, native_lines);
                // Chrome culls an inline box without borders, padding or margins: its client rects are its text's.
                const auto& style = nodes[e]->style;
                const bool atomic = style.text(weft::PropertyId::Display, "inline") != "inline";
                bool decorated{};
                for (const auto id : {weft::PropertyId::PaddingLeft, weft::PropertyId::PaddingRight,
                                      weft::PropertyId::BorderLeftWidth, weft::PropertyId::BorderRightWidth,
                                      weft::PropertyId::MarginLeft, weft::PropertyId::MarginRight}) {
                    const auto v = style.text(id, "0");
                    decorated = decorated || (v != "0" && v != "0px");
                }
                if (!atomic && !decorated) {
                    // A culled box has a rect only where it has content: the scene's fragment of no width on a line
                    // where only its end lies is no rect of Chrome's.
                    const auto empty = [](const Rect& r) {
                        return r[2] <= 0;
                    };
                    expected.erase(std::remove_if(expected.begin(), expected.end(), empty), expected.end());
                    got.erase(std::remove_if(got.begin(), got.end(), empty), got.end());
                    expected = per_line(expected);
                    got = per_line(got);
                }
                const auto what = name + " element " + std::to_string(e) + " <" + tag + ">";
                if (expected.size() != got.size()) {
                    failures.list.push_back(what + ": rects " + list(got) + ", Chrome " + list(expected));
                    continue;
                }
                for (std::size_t r = 0; r < expected.size(); ++r) {
                    const auto at = what + " rect " + std::to_string(r);
                    failures.check(got[r][0], expected[r][0], horizontal_tolerance, true, at + " x");
                    failures.check(got[r][2], expected[r][2], horizontal_tolerance, true, at + " width");
                    if (!atomic) {
                        failures.check(got[r][1], expected[r][1], 0, false, at + " line");
                    }
                    ++rects;
                }
            }
        }
        std::cout << fixture["cases"].size() << " bidi cases vs " << fixture["browser"].string << ": " << rects
                  << " element rects, " << fragments << " text fragments, " << characters << " characters, " << inks
                  << " ellipses; largest difference x " << failures.x << " px, y " << failures.y << " px; "
                  << failures.list.size() << " failures\n";
        for (const auto& f : failures.list) {
            std::cout << "  " << f << '\n';
        }
        require(failures.list.empty(), "scene bidi layout matches Chrome");
        std::cout << "PASS scene bidirectional text and complex scripts against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
