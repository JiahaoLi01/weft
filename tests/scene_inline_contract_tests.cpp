#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// Scene inline formatting contexts beyond Chrome's geometry (scene_inline_browser_contract): a layout reused while
// the content stays and redone when a text changes; hits and client rects by fragment, so the empty part of an inline
// element's bounding box hits its parent; a fragmented box painted with its start edge on the first fragment and its
// end edge on the last; text painted a line at a time; the profile's diagnostics; and the native D3D11 pass drawing
// the lines where the reference raster does.
namespace {
    using weft::testing::require;
    using Kind = weft::paint::Item::Kind;

    weft::dom::Node& text(weft::testing::NativeScene& s, weft::dom::Node& parent, std::string data) {
        auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
        t.data = std::move(data);
        s.tree.insert(parent, t, nullptr);
        return t;
    }

    weft::dom::Node& element(weft::testing::NativeScene& s, weft::dom::Node& parent, const std::string& tag,
                             const std::string& style) {
        auto& e = s.html(tag);
        if (!style.empty()) {
            s.set(e, "style", style);
        }
        s.tree.insert(parent, e, nullptr);
        return e;
    }

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

    const weft::SceneNode* find(const weft::SceneNode& n, weft::NodeIdentity id) {
        if (n.identity == id) {
            return &n;
        }
        for (const auto& c : n.children) {
            if (const auto* f = find(c, id)) {
                return f;
            }
        }
        return nullptr;
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& scene_text = weft::testing::thread_scene_text();

        // A paragraph whose bordered span starts on the first line and ends on the second.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;background-color:#101010");
        auto& block = element(s, *s.root, "div", "width:160px;font-family:system-ui;font-size:14px;color:#e0e0e0");
        text(s, block, "First words ");
        auto& span = element(s, block, "span",
                             "padding:0 4px;border:2px solid #d4a843;border-radius:3px;background-color:#303030");
        auto& inside = text(s, span, "bordered span words");
        text(s, block, " end.");
        weft::SceneSolver solver("", "", scene_text);
        weft::SceneFrame frame;
        solver.update(s.tree.snapshot(*s.root), frame, 300, 200, 0);
        const auto* laid = find(frame.root, span.identity);
        require(laid && laid->fragments.size() == 2 && laid->fragments[0].first && !laid->fragments[0].last &&
                    !laid->fragments[1].first && laid->fragments[1].last,
                "a span over two lines: two fragments, the first starting it and the last ending it");
        const auto& first = laid->fragments[0].rect;
        const auto& last = laid->fragments[1].rect;
        std::cout << "fragments: (" << first[0] << ", " << first[1] << ", " << first[2] << ", " << first[3] << ") ("
                  << last[0] << ", " << last[1] << ", " << last[2] << ", " << last[3] << ")\n";
        require(last[1] > first[1] && last[0] < first[0], "the second fragment starts the next line");

        // The same content again: the layout is reused, lines and fragments with it.
        const auto lines_before = frame.root.children[0].children;
        solver.update(s.tree.snapshot(*s.root), frame, 300, 200, 16);
        require(solver.work().layout_reuses == 1 && frame.root.children[0].children == lines_before,
                "an unchanged paragraph keeps its lines");
        // Fresh frames of the same content: the second reuses the layout, its lines and fragments coming from the
        // solver's reference frame.
        weft::SceneSolver fresh("", "", scene_text);
        const auto once = fresh.frame(s.tree.snapshot(*s.root), 300, 200, 0);
        const auto twice = fresh.frame(s.tree.snapshot(*s.root), 300, 200, 16);
        require(fresh.work().layout_reuses == 1 && twice.root == once.root,
                "a fresh frame of unchanged content keeps the lines and fragments");
        // The reference frame follows a frame laid out again, lines too.
        inside.data = "bordered span words again";
        s.tree.changed(inside);
        const auto relaid = fresh.frame(s.tree.snapshot(*s.root), 300, 200, 32);
        const auto kept = fresh.frame(s.tree.snapshot(*s.root), 300, 200, 48);
        require(relaid.root != once.root && kept.root == relaid.root, "the reference frame keeps a new layout's lines");
        inside.data = "bordered span words";
        s.tree.changed(inside);
        // A paragraph whose color animates: no subtree is static, so each fresh frame builds its nodes anew, yet the
        // layout is reused and the lines come with the boxes.
        weft::testing::NativeScene a;
        auto& pulsing =
            element(a, *a.root, "div", "width:120px;font-family:system-ui;font-size:14px;animation:pulse 1s infinite");
        text(a, pulsing, "Words that wrap over more than one line here");
        weft::SceneSolver animated("", "@keyframes pulse{from{color:#ff0000}to{color:#0000ff}}", scene_text);
        const auto first_frame = animated.frame(a.tree.snapshot(*a.root), 300, 200, 0);
        const auto next_frame = animated.frame(a.tree.snapshot(*a.root), 300, 200, 250);
        require(animated.work().layout_reuses == 1 && next_frame.root.children[0].lines.size() > 1 &&
                    next_frame.root.children[0].lines == first_frame.root.children[0].lines,
                "an animated paragraph reuses its layout with its lines");

        // Hits and client rects by fragment. The block's (and span's) origin in the frame.
        const auto& block_node = frame.root.children[0];
        const float bx = block_node.box[0] + laid->box[0];
        const float by = block_node.box[1] + laid->box[1];
        weft::SceneGeometry geometry(scene_text);
        const auto inside_first = geometry.hit_test(frame, bx + first[0] + first[2] / 2, by + first[1] + first[3] / 2);
        const auto inside_last = geometry.hit_test(frame, bx + last[0] + last[2] / 2, by + last[1] + last[3] / 2);
        // Right of the second fragment, inside the span's bounding box (the first line reaches further).
        const auto beside = geometry.hit_test(frame, bx + last[0] + last[2] + 4, by + last[1] + last[3] / 2);
        require(inside_first == span.identity && inside_last == span.identity && beside == block.identity,
                "an inline element hits by fragment; the rest of its bounding box hits its parent");
        const auto elements = geometry.geometry(frame);
        const auto entry = std::find_if(elements.begin(), elements.end(), [&](const weft::ElementGeometry& e) {
            return e.identity == span.identity;
        });
        require(entry != elements.end() && entry->client_rects.size() == 2 &&
                    std::abs(entry->client_rects[1].at(0) - (bx + last[0])) < 1e-3 &&
                    std::abs(entry->client_rects[0].at(1) - (by + first[1])) < 1e-3 && entry->client_width == 0,
                "client rects are the fragments; an inline element has no client box");

        // Painted a fragment at a time: the left border only on the first, the right only on the last, each with
        // its corners; then the text, a line at a time.
        const auto listed = weft::scene_display_list(frame, {}, &scene_text);
        require(listed.fallback.empty(), "a native display list");
        std::vector<weft::paint::Box> bordered;
        std::size_t texts{};
        std::vector<std::uint32_t> parts;
        for (const auto& item : listed.list.items) {
            if (item.kind == Kind::Box) {
                const auto& b = listed.list.boxes[item.payload];
                if (b.widths[0] > 0) {
                    bordered.push_back(b);
                }
            } else if (item.kind == Kind::Text) {
                ++texts;
                parts.push_back(listed.list.rasters[item.payload].part);
            }
        }
        require(bordered.size() == 2 && bordered[0].widths[3] == 2 && bordered[0].widths[1] == 0 &&
                    bordered[1].widths[3] == 0 && bordered[1].widths[1] == 2 && bordered[0].radii[0] > 0 &&
                    bordered[0].radii[1] == 0 && bordered[1].radii[0] == 0 && bordered[1].radii[1] > 0,
                "a fragmented box: start edge and corners first, end edge and corners last");
        require(texts == 4 && std::count(parts.begin(), parts.end(), 1u) == 1,
                "text a line at a time: the span's two lines, and one line of each text beside it");

        // A changed text lays out again.
        inside.data = "short";
        s.tree.changed(inside);
        solver.update(s.tree.snapshot(*s.root), frame, 300, 200, 32);
        const auto* changed = find(frame.root, span.identity);
        require(solver.work().layout_evaluations == 2 && changed && changed->fragments.size() == 1,
                "a changed text is laid out again");

        // The native pass draws the lines where the reference raster does.
        inside.data = "bordered span words";
        s.tree.changed(inside);
        solver.update(s.tree.snapshot(*s.root), frame, 300, 200, 48);
        weft::SceneRaster raster(scene_text);
        const auto reference = raster.raster(frame, 1);
        weft::testing::D3D11Scene d3d11(300, 200);
        const auto native = d3d11.draw(frame);
        // Glyph rasterizers differ at the edges (DirectWrite there, FreeType here): each line's ink, over the dark
        // background, has its centroid within a pixel and most of its amount (reference RGBA, native premultiplied
        // BGRA over an opaque fill). A line drawn elsewhere is many pixels off.
        const auto ink = [&](bool from_native, const std::array<float, 4>& r) {
            std::array<double, 3> out{};
            for (int y = std::max(0, int(r[1]) - 2); y < std::min(200, int(r[1] + r[3]) + 2); ++y) {
                for (int x = std::max(0, int(r[0])); x < std::min(300, int(r[0] + r[2]) + 1); ++x) {
                    const auto i = (std::size_t(y) * 300 + x) * 4;
                    const double v = std::max(0, int(from_native ? native[i + 1] : reference.rgba[i + 1]) - 0x30);
                    out[0] += v;
                    out[1] += v * x;
                    out[2] += v * y;
                }
            }
            return out;
        };
        std::size_t compared{};
        const auto lines_of = [&](auto&& self, const weft::SceneNode& n, float x, float y) -> void {
            const float ax = x + n.box[0];
            const float ay = y + n.box[1];
            for (const auto& line : n.lines) {
                const std::array<float, 4> r{ax + line.rect[0], ay + line.rect[1], line.rect[2], line.rect[3]};
                const auto a = ink(false, r);
                const auto b = ink(true, r);
                const double dx = std::abs(a[1] / a[0] - b[1] / b[0]);
                const double dy = std::abs(a[2] / a[0] - b[2] / b[0]);
                std::cout << "line [" << line.text << "]: centroid difference " << dx << ", " << dy << ", ink "
                          << b[0] / a[0] << '\n';
                require(a[0] > 0 && dx < 1 && dy < 1 && std::abs(b[0] / a[0] - 1) < .25,
                        "native pass draws the line [" + line.text + "] of the reference raster");
                ++compared;
            }
            for (const auto& c : n.children) {
                self(self, c, ax, ay);
            }
        };
        lines_of(lines_of, frame.root, 0, 0);
        require(compared == 4, "four lines compared");
        d3d11.gpu.check_debug_messages();

        // Outside the profile: diagnostics, not silently changed semantics.
        const auto refused = [&](const std::string& style, const std::string& child, const char* expected) {
            weft::testing::NativeScene t;
            auto& b = element(t, *t.root, "div", style);
            if (child.empty()) {
                text(t, b, "some text");
            } else {
                auto& c = element(t, b, "span", child);
                text(t, c, "inner");
                text(t, b, " after");
            }
            weft::SceneSolver refusing("", "", scene_text);
            const auto [code, message] = diagnostic([&] {
                refusing.frame(t.tree.snapshot(*t.root), 200, 100, 0);
            });
            require(code == weft::Diag::SceneProfile && message.find(expected) != std::string::npos,
                    std::string("diagnostic: ") + expected + " (got " + message + ")");
        };
        refused("white-space:balance", "", "white-space balance outside scene profile");
        // white-space (CSS Text 3 §3): pre keeps spaces and breaks only at newlines, pre-wrap also wraps, pre-line
        // collapses spaces but keeps newlines.
        {
            const auto laid_lines = [&](const std::string& style, const std::string& content) {
                weft::testing::NativeScene t;
                auto& b = element(t, *t.root, "div", "width:120px;font-family:system-ui;font-size:14px;" + style);
                text(t, b, content);
                weft::SceneSolver solver("", "", scene_text);
                return solver.frame(t.tree.snapshot(*t.root), 300, 200, 0).root.children[0].lines;
            };
            const auto lines = [&](const std::string& style, const std::string& content) {
                std::vector<std::string> out;
                for (const auto& line : laid_lines(style, content)) {
                    out.push_back(line.text);
                }
                return out;
            };
            // The distance between the first and the last text line.
            const auto apart = [&](const std::string& content) {
                const auto l = laid_lines("white-space:pre-wrap", content);
                return l.size() == 2 ? l[1].rect[1] - l[0].rect[1] : -1.f;
            };
            const std::string long_line = "one two three four five six seven eight";
            require(lines("white-space:pre", "a  b\nc") == std::vector<std::string>{"a  b", "c"},
                    "pre keeps spaces and breaks at a newline");
            require(lines("white-space:pre", long_line).size() == 1, "pre does not wrap");
            require(lines("white-space:pre-wrap", long_line).size() > 1, "pre-wrap wraps");
            // An empty line has a line box (it ends in a preserved newline) but no text.
            require(apart("x\ny") > 0 && apart("x\n\ny") > 1.5f * apart("x\ny"), "pre-wrap keeps an empty line");
            require(lines("white-space:pre-line", "a   b\nc") == std::vector<std::string>{"a b", "c"},
                    "pre-line collapses spaces and keeps newlines");
            require(lines("", "a  b\nc") == std::vector<std::string>{"a b c"}, "normal collapses both");
        }
        // A shrink-to-fit box (absolutely positioned, auto width) is its text's max-content width plus its padding and
        // borders, and the text fits on one line in it: its content width is the box less those edges in floats,
        // which float error must not take a LayoutUnit below the max-content width it was sized to.
        for (const std::string padding : {"0 1.3em", "0 20.8px", "0 13.3px", "0 7.7px", "0 .35em", "0 3.1px"}) {
            for (const std::string words : {"Wrap", "End turn", "one two three"}) {
                weft::testing::NativeScene t;
                auto& b = element(t, *t.root, "div",
                                  "position:absolute;top:10px;right:10px;font-family:system-ui;font-size:16px;"
                                  "border:1px solid #aa8800;padding:" +
                                      padding);
                text(t, b, words);
                weft::SceneSolver fitting("", "", scene_text);
                const auto laid_out = fitting.frame(t.tree.snapshot(*t.root), 400, 200, 0);
                require(laid_out.root.children[0].lines.size() == 1,
                        "one line of '" + words + "' in a shrink-to-fit box with padding " + padding);
            }
        }
        // A box of no area paints nothing of its own, but what overflows it shows, as in a browser: the absolutely
        // positioned child of a wrapper of no height paints, in the display list and the reference raster; a wrapper
        // that clips its overflow hides it.
        for (const bool clipped : {false, true}) {
            weft::testing::NativeScene t;
            auto& wrapper =
                element(t, *t.root, "div", clipped ? "position:relative;overflow:hidden" : "position:relative");
            element(t, wrapper, "div",
                    "position:absolute;left:10px;top:10px;width:20px;height:20px;background-color:#ff0000");
            weft::SceneSolver hollow("", "", scene_text);
            const auto f = hollow.frame(t.tree.snapshot(*t.root), 100, 100, 0);
            require(f.root.children[0].box[3] == 0, "a wrapper of no height");
            const auto hollow_list = weft::scene_display_list(f, {}, &scene_text);
            const auto boxes =
                std::count_if(hollow_list.list.items.begin(), hollow_list.list.items.end(), [](const auto& item) {
                    return item.kind == Kind::Box;
                });
            weft::SceneRaster painter(scene_text);
            const auto red = painter.raster(f, 1).rgba[(std::size_t(20) * 100 + 20) * 4];
            require(clipped ? boxes == 0 && red == 0 : boxes == 1 && red == 255,
                    std::string(clipped ? "a clipping" : "an unclipped") +
                        " wrapper of no height: " + std::to_string(boxes) + " boxes, red " + std::to_string(red));
        }
        refused("text-align:justify", "", "text-align justify outside scene profile");
        refused("", "vertical-align:middle", "vertical-align middle outside scene profile");
        {
            weft::testing::NativeScene t;
            auto& b = element(t, *t.root, "div", "");
            auto& outer = element(t, b, "span", "");
            auto& inner = element(t, outer, "div", "");
            text(t, inner, "block in inline");
            weft::SceneSolver refusing("", "", scene_text);
            const auto [code, message] = diagnostic([&] {
                refusing.frame(t.tree.snapshot(*t.root), 200, 100, 0);
            });
            require(code == weft::Diag::SceneProfile &&
                        message.find("block-level box inside an inline") != std::string::npos,
                    "diagnostic: a block inside an inline element");
        }
        std::cout << "PASS scene inline contract: reuse and relayout, fragment hits and rects, sliced boxes, lines, "
                     "native lines, diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
