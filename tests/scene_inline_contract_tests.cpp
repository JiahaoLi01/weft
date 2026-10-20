#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Scene inline formatting contexts beyond Chrome's geometry (scene_inline_browser_contract): a layout reused while
// the content stays and redone when a text changes; hits and client rects by fragment, so the empty part of an inline
// element's bounding box hits its parent; a fragmented box painted with its start edge on the first fragment and its
// end edge on the last; text painted a line at a time; ruby; text decorations; overflow-wrap and word-break; the
// profile's diagnostics; and the native D3D11 pass drawing the lines and their decorations where the reference raster
// does.
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
        // A paragraph of mixed content (text runs beside an element) whose opacity, transform, color and shadows
        // animate, as a block fading up into view does: each frame builds its runs again, and they are the runs they
        // were, laid out, so the layout is reused (none of these is laid out by), the runs keeping their lines and
        // taking the color. A run whose text changes is laid out again.
        weft::testing::NativeScene m;
        auto& revealing =
            element(m, *m.root, "div", "width:120px;font-family:system-ui;font-size:14px;animation:reveal 1s infinite");
        text(m, revealing, "Words before ");
        text(m, element(m, revealing, "b", ""), "bold");
        auto& tail = text(m, revealing, " and the words after them that wrap");
        weft::SceneSolver revealed("",
                                   "@keyframes reveal{from{opacity:0;transform:translateY(30px);color:#ff0000;"
                                   "box-shadow:0 0 0 #000000;text-shadow:0 0 0 #000000}to{opacity:1;"
                                   "transform:translateY(0);color:#0000ff;box-shadow:0 0 40px #ffffff;"
                                   "text-shadow:0 1px 2px #000000}}",
                                   scene_text);
        const auto shown_first = revealed.frame(m.tree.snapshot(*m.root), 300, 200, 0);
        const auto shown_next = revealed.frame(m.tree.snapshot(*m.root), 300, 200, 250);
        const auto& runs_first = shown_first.root.children[0].children;
        const auto& runs_next = shown_next.root.children[0].children;
        require(revealed.work().layout_reuses == 1 && runs_first.size() == 3 && runs_next.size() == 3 &&
                    runs_next[2].lines.size() > 1 && runs_next[2].lines == runs_first[2].lines &&
                    runs_next[2].style.text(weft::PropertyId::Color, "") !=
                        runs_first[2].style.text(weft::PropertyId::Color, ""),
                "an animated paragraph of mixed content reuses its layout, its runs keeping their lines");
        tail.data = " and other words";
        m.tree.changed(tail);
        const auto shown_changed = revealed.frame(m.tree.snapshot(*m.root), 300, 200, 500);
        require(revealed.work().layout_evaluations == 2 &&
                    shown_changed.root.children[0].children[2].lines != runs_next[2].lines,
                "a run of an animated paragraph whose text changes is laid out again");

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
            // An element's mixed content (a line of highlighted code: spans and the text between them) keeps its
            // spaces and newlines as the element's own text does.
            {
                weft::testing::NativeScene t;
                auto& b = element(t, *t.root, "div", "width:240px;font-family:system-ui;font-size:14px;white-space:pre");
                auto& keyword = element(t, b, "span", "");
                text(t, keyword, "const");
                text(t, b, " x  = 1;\nlet y = 2;");
                weft::SceneSolver mixing("", "", scene_text);
                const auto mixed = mixing.frame(t.tree.snapshot(*t.root), 300, 200, 0);
                const auto& run = mixed.root.children[0].children[1];
                std::vector<std::string> got;
                for (const auto& line : run.lines) {
                    got.push_back(line.text);
                }
                require(run.tag == "#text" && got == std::vector<std::string>{" x  = 1;", "let y = 2;"} &&
                            run.lines[1].rect[1] > run.lines[0].rect[1],
                        "pre keeps the spaces and newlines of mixed content");
            }
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
        // Ruby (CSS Ruby 1): a ruby is an atomic inline of columns, each its base text under its rt's text, centred
        // over each other and as wide as the wider; the bases sit on the line's baseline; an rt of display: none
        // annotates nothing.
        for (const bool hidden_rt : {false, true}) {
            weft::testing::NativeScene t;
            auto& line =
                element(t, *t.root, "div", "width:400px;font-family:system-ui;font-size:20px;line-height:30px");
            text(t, line, "Ab ");
            auto& ruby = t.html("ruby");
            t.tree.insert(line, ruby, nullptr);
            const char* columns[][2] = {{"Wide", "x"}, {"i", "wider annotation"}};
            std::vector<weft::dom::Node*> rts;
            for (const auto& [base, reading] : columns) {
                text(t, ruby, base);
                auto& rt = t.html("rt");
                t.set(rt, "style", std::string("font-size:10px;line-height:12px") + (hidden_rt ? ";display:none" : ""));
                text(t, rt, reading);
                t.tree.insert(ruby, rt, nullptr);
                rts.push_back(&rt);
            }
            text(t, line, " cd");
            weft::SceneSolver ruby_solver("", "", scene_text);
            const auto f = ruby_solver.frame(t.tree.snapshot(*t.root), 500, 200, 0);
            const auto* r = find(f.root, ruby.identity);
            require(r && r->children.size() == 4, "a ruby of two bases and two annotations");
            const auto& first_base = r->children[0];
            const auto& first_rt = r->children[1];
            const auto& second_base = r->children[2];
            const auto& second_rt = r->children[3];
            const auto centre = [](const weft::SceneNode& n) {
                return n.box[0] + n.box[2] / 2;
            };
            // The surrounding text's baseline, and the bases', in the line's coordinates.
            const auto& paragraph = f.root.children[0];
            const float line_baseline = paragraph.lines.empty()
                                            ? paragraph.children[0].box[1] + paragraph.children[0].lines[0].baseline
                                            : paragraph.lines[0].rect[1] + paragraph.lines[0].baseline;
            const float base_baseline = r->box[1] + first_base.box[1] + first_base.lines.at(0).baseline;
            require(std::abs(base_baseline - line_baseline) < .5f,
                    "the bases sit on the line's baseline: " + std::to_string(base_baseline) + " vs " +
                        std::to_string(line_baseline));
            if (hidden_rt) {
                require(first_rt.box[2] == 0 && second_base.box[0] == first_base.box[0] + first_base.box[2] &&
                            r->box[2] == first_base.box[2] + second_base.box[2],
                        "an rt of display: none annotates nothing: the bases side by side");
                continue;
            }
            require(first_rt.box[1] + first_rt.box[3] <= first_base.box[1] + .5f &&
                        std::abs(centre(first_rt) - centre(first_base)) < .5f,
                    "an annotation over its base, centred");
            require(second_rt.box[2] > second_base.box[2] && std::abs(centre(second_rt) - centre(second_base)) < .5f &&
                        std::abs(r->box[2] - (first_base.box[2] + second_rt.box[2])) < .5f,
                    "a wider annotation widens its column, the base centred under it");
        }
        // text-decoration (CSS Text Decoration 3): an underline below each line's baseline and an overline at its top,
        // under the text, and a line-through about the middle of lowercase letters, over it, as wide as the line, in
        // the decoration color and thickness. An element of no lines of its own draws its parent's, in the parent's
        // color; one of its own lines takes the initial values of the decoration properties it leaves out, not its
        // parent's. The native pass draws the bands where the reference raster does.
        {
            weft::testing::NativeScene t;
            t.set(*t.root, "style", "position:relative;width:400px;height:120px;background-color:#202020");
            const std::string font =
                "position:absolute;font-family:system-ui;font-size:28px;line-height:40px;color:#ffffff;";
            auto& under = element(t, *t.root, "div", font + "left:10px;top:10px;text-decoration:underline 4px");
            auto& under_text = text(t, under, "HHH ");
            auto& green = element(t, under, "span", "color:#00ff00");
            auto& green_text = text(t, green, "HH");
            auto& struck =
                element(t, *t.root, "div", font + "left:10px;top:60px;text-decoration:line-through #ff0000 4px");
            auto& struck_text = text(t, struck, "HHHH");
            auto& own = element(t, *t.root, "div", font + "left:240px;top:10px;text-decoration-color:#ff0000");
            auto& over = element(t, own, "span", "color:#00ff00;text-decoration-line:overline");
            auto& over_text = text(t, over, "HH");
            weft::SceneSolver decorated("", "", scene_text);
            const auto f = decorated.frame(t.tree.snapshot(*t.root), 400, 120, 0);

            // Each line's rect and baseline in the frame, by the node that shows it.
            struct Placed {
                weft::NodeIdentity owner;
                std::array<float, 4> rect;
                float baseline;
            };

            std::vector<Placed> placed;
            const auto collect = [&](auto&& self, const weft::SceneNode& n, float x, float y) -> void {
                const float ax = x + n.box[0];
                const float ay = y + n.box[1];
                for (const auto& line : n.lines) {
                    placed.push_back({n.identity,
                                      {ax + line.rect[0], ay + line.rect[1], line.rect[2], line.rect[3]},
                                      ay + line.rect[1] + line.baseline});
                }
                for (const auto& c : n.children) {
                    self(self, c, ax, ay);
                }
            };
            collect(collect, f.root, 0, 0);
            const auto line_of = [&](const weft::dom::Node& e, const weft::dom::Node& content) {
                for (const auto& p : placed) {
                    if (p.owner == e.identity || p.owner == content.identity) {
                        return p;
                    }
                }
                throw std::runtime_error("no line of [" + content.data + "]");
            };
            const auto decorated_list = weft::scene_display_list(f, {}, &scene_text);
            require(decorated_list.fallback.empty() && bool(weft::paint::check(decorated_list.list, 1)),
                    "a well-formed decorated list");

            // The bands: the boxes thinner than a line.
            struct Band {
                std::size_t index;
                std::array<float, 4> rect;
                weft::paint::Color color;
            };

            std::vector<Band> bands;
            for (std::size_t i = 0; i < decorated_list.list.items.size(); ++i) {
                const auto& item = decorated_list.list.items[i];
                if (item.kind == Kind::Box && item.height <= 4) {
                    bands.push_back({i,
                                     {item.transform[4], item.transform[5], item.width, item.height},
                                     decorated_list.list.boxes[item.payload].background});
                }
            }
            require(bands.size() == 4, "four decoration bands, " + std::to_string(bands.size()) + " drawn");
            const auto band_of = [&](const Placed& line) {
                for (const auto& b : bands) {
                    if (std::abs(b.rect[0] - line.rect[0]) < .01f && std::abs(b.rect[2] - line.rect[2]) < .01f &&
                        b.rect[1] >= line.rect[1] - .01f && b.rect[1] < line.rect[1] + line.rect[3]) {
                        return b;
                    }
                }
                throw std::runtime_error("no band on the line at " + std::to_string(line.rect[0]) + "," +
                                         std::to_string(line.rect[1]));
            };
            const weft::paint::Color white{1, 1, 1, 1};
            const weft::paint::Color red{1, 0, 0, 1};
            const weft::paint::Color lime{0, 1, 0, 1};
            const auto under_line = line_of(under, under_text);
            const auto u = band_of(under_line);
            require(std::abs(u.rect[1] - (under_line.baseline + 28 * .09f)) < .01f && u.rect[3] == 4 &&
                        u.color == white && decorated_list.list.items.at(u.index + 1).kind == Kind::Text,
                    "an underline below the baseline, of its thickness and the text's color, under the text");
            const auto g = band_of(line_of(green, green_text));
            require(std::abs(g.rect[1] - u.rect[1]) < .01f && g.rect[3] == 4 && g.color == white,
                    "a child's text under its parent's underline, in the parent's color");
            const auto struck_line = line_of(struck, struck_text);
            const auto s2 = band_of(struck_line);
            require(std::abs(s2.rect[1] + 2 - (struck_line.baseline - 28 * .3f)) < .01f && s2.color == red &&
                        decorated_list.list.items.at(s2.index - 1).kind == Kind::Text,
                    "a line-through about the middle of the letters, in the decoration color, over the text");
            const auto over_line = line_of(over, over_text);
            const auto o = band_of(over_line);
            require(std::abs(o.rect[1] - over_line.rect[1]) < .01f && o.rect[3] == 2 && o.color == lime,
                    "an element's own overline: its own color and the automatic thickness, not its parent's color");
            weft::SceneRaster painter(scene_text);
            const auto decorated_raster = painter.raster(f, 1);
            weft::testing::D3D11Scene decorated_d3d11(400, 120);
            const auto decorated_native = decorated_d3d11.draw(f);
            decorated_d3d11.gpu.check_debug_messages();
            for (const auto& b : {u, g, s2, o}) {
                const auto x = std::size_t(b.rect[0] + b.rect[2] / 2);
                const auto y = std::size_t(b.rect[1] + b.rect[3] / 2);
                const auto i = (y * 400 + x) * 4;
                const std::array<int, 3> want{int(b.color[0] * 255), int(b.color[1] * 255), int(b.color[2] * 255)};
                const std::array<int, 3> n{decorated_native[i + 2], decorated_native[i + 1], decorated_native[i]};
                const std::array<int, 3> r{decorated_raster.rgba[i], decorated_raster.rgba[i + 1],
                                           decorated_raster.rgba[i + 2]};
                require(n == want && r == want, "a band at " + std::to_string(x) + "," + std::to_string(y) +
                                                    ": native " + std::to_string(n[0]) + "," + std::to_string(n[1]) +
                                                    "," + std::to_string(n[2]) + ", raster " + std::to_string(r[0]) +
                                                    "," + std::to_string(r[1]) + "," + std::to_string(r[2]));
            }
        }
        // overflow-wrap and word-break (CSS Text 3 §5.5, §5.2): overflow-wrap: break-word and anywhere (and word-break:
        // break-word, and the legacy word-wrap) break a word that overflows the line it starts between its grapheme
        // clusters, each line taking the most that fit; a word that fits on the next line goes there first. break-all
        // breaks between letters wherever a line fills; keep-all keeps CJK letters together. anywhere narrows the
        // min-content width and break-word does not: a flex item shrinks past its long word with the one only.
        {
            const std::string word = "Supercalifragilisticexpialidocious";
            weft::StyleValues font;
            font.set(weft::PropertyId::FontFamily, "system-ui");
            font.set(weft::PropertyId::FontSize, "14px");

            struct Laid {
                std::vector<std::string> texts;
                float widest{};
            };

            const auto lay = [&](const std::string& style, const std::string& content, int width = 80) {
                weft::testing::NativeScene t;
                auto& b =
                    element(t, *t.root, "div",
                            "width:" + std::to_string(width) + "px;font-family:system-ui;font-size:14px;" + style);
                text(t, b, content);
                weft::SceneSolver wrapping("", "", scene_text);
                Laid out;
                for (const auto& line : wrapping.frame(t.tree.snapshot(*t.root), 300, 300, 0).root.children[0].lines) {
                    out.texts.push_back(line.text);
                    out.widest = std::max(out.widest, line.rect[2]);
                }
                return out;
            };
            const auto joined = [](const Laid& l) {
                std::string out;
                for (const auto& t : l.texts) {
                    out += t;
                }
                return out;
            };
            const auto listed_lines = [](const Laid& l) {
                std::string out;
                for (const auto& t : l.texts) {
                    out += " [" + t + "]";
                }
                return out;
            };
            const auto normal = lay("", word);
            require(normal.texts.size() == 1 && normal.widest > 80,
                    "a long word overflows its line without overflow-wrap:" + listed_lines(normal));
            for (const std::string style :
                 {"overflow-wrap:break-word", "overflow-wrap:anywhere", "word-break:break-word", "word-wrap:break-word",
                  "white-space:pre-wrap;word-break:break-word"}) {
                const auto broken = lay(style, word);
                require(broken.texts.size() > 2 && broken.widest <= 80 && joined(broken) == word,
                        style + " breaks the word between its letters:" + listed_lines(broken));
                for (std::size_t k = 0; k + 1 < broken.texts.size(); ++k) {
                    const auto more = broken.texts[k] + broken.texts[k + 1].substr(0, 1);
                    require(scene_text.measure(more, font).width > 80,
                            style + ": the line [" + broken.texts[k] + "] takes the most letters that fit");
                }
            }
            require(lay("white-space:nowrap;overflow-wrap:anywhere", word).texts.size() == 1,
                    "overflow-wrap breaks no word of a text that does not wrap");
            const auto after_short = lay("overflow-wrap:break-word", "aa " + word);
            const auto break_all = lay("word-break:break-all", "aa " + word);
            require(after_short.texts.at(0) == "aa" && joined(after_short) == "aa" + word,
                    "break-word moves a long word to a line of its own first:" + listed_lines(after_short));
            require(break_all.texts.at(0).starts_with("aa S") && break_all.widest <= 80 &&
                        scene_text.measure(break_all.texts[0] + break_all.texts[1].substr(0, 1), font).width > 80,
                    "break-all fills the first line with the start of the long word:" + listed_lines(break_all));
            const std::string ideographs = "漢字漢字漢字漢字漢字漢字";
            require(lay("", ideographs).texts.size() > 1, "CJK text breaks between its ideographs");
            require(lay("word-break:keep-all", ideographs).texts.size() == 1, "keep-all keeps the ideographs together");
            const auto spaced = lay("word-break:keep-all", "漢字漢字 漢字漢字 漢字漢字");
            require(spaced.texts == std::vector<std::string>{"漢字漢字", "漢字漢字", "漢字漢字"},
                    "keep-all breaks at spaces only:" + listed_lines(spaced));
            std::string accented;
            for (int k = 0; k < 16; ++k) {
                accented += "e\xCC\x81";
            }
            const auto clusters = lay("overflow-wrap:anywhere", accented, 30);
            require(clusters.texts.size() > 1 && joined(clusters) == accented &&
                        std::none_of(clusters.texts.begin(), clusters.texts.end(),
                                     [](const std::string& t) {
                                         return t.starts_with("\xCC\x81");
                                     }),
                    "a grapheme cluster stays together:" + listed_lines(clusters));
            // A flex item of the long word in a row 80px wide: its automatic minimum width is its min-content width.
            const auto item = [&](const std::string& style) {
                weft::testing::NativeScene t;
                auto& row = element(t, *t.root, "div", "display:flex;width:80px");
                auto& i = element(t, row, "div", "font-family:system-ui;font-size:14px;" + style);
                text(t, i, word);
                weft::SceneSolver flexing("", "", scene_text);
                const auto f = flexing.frame(t.tree.snapshot(*t.root), 300, 300, 0);
                const auto& n = f.root.children[0].children[0];
                return std::pair{n.box[2], n.lines.size()};
            };
            const auto [plain_width, plain_lines] = item("");
            const auto [word_width, word_lines] = item("overflow-wrap:break-word");
            const auto [anywhere_width, anywhere_lines] = item("overflow-wrap:anywhere");
            const auto [legacy_width, legacy_lines] = item("word-break:break-word");
            require(plain_lines == 1 && plain_width > 80, "a flex item keeps its long word");
            require(word_lines == 1 && word_width == plain_width,
                    "break-word leaves the min-content width: the item keeps its word on one line, " +
                        std::to_string(word_width) + "px");
            require(
                anywhere_lines > 2 && anywhere_width == 80 && legacy_lines == anywhere_lines && legacy_width == 80,
                "anywhere (and word-break: break-word) narrows it: the item shrinks to its row and breaks the word, " +
                    std::to_string(anywhere_width) + "px in " + std::to_string(anywhere_lines) + " lines");
            // The native pass draws the broken lines where the reference raster does.
            weft::testing::NativeScene t;
            t.set(*t.root, "style", "position:relative;background-color:#101010");
            auto& b = element(t, *t.root, "div",
                              "width:80px;font-family:system-ui;font-size:14px;color:#e0e0e0;overflow-wrap:anywhere");
            text(t, b, "aa " + word);
            weft::SceneSolver drawing("", "", scene_text);
            const auto f = drawing.frame(t.tree.snapshot(*t.root), 120, 120, 0);
            weft::SceneRaster painter(scene_text);
            const auto broken_reference = painter.raster(f, 1);
            weft::testing::D3D11Scene broken_d3d11(120, 120);
            const auto broken_native = broken_d3d11.draw(f);
            broken_d3d11.gpu.check_debug_messages();
            const auto broken_ink = [&](bool from_native, const std::array<float, 4>& r) {
                std::array<double, 3> out{};
                for (int y = std::max(0, int(r[1]) - 2); y < std::min(120, int(r[1] + r[3]) + 2); ++y) {
                    for (int x = std::max(0, int(r[0])); x < std::min(120, int(r[0] + r[2]) + 1); ++x) {
                        const auto i = (std::size_t(y) * 120 + x) * 4;
                        const double v =
                            std::max(0, int(from_native ? broken_native[i + 1] : broken_reference.rgba[i + 1]) - 0x30);
                        out[0] += v;
                        out[1] += v * x;
                        out[2] += v * y;
                    }
                }
                return out;
            };
            const auto& paragraph = f.root.children[0];
            require(paragraph.lines.size() > 3, "the paragraph's lines");
            for (const auto& line : paragraph.lines) {
                const std::array<float, 4> r{paragraph.box[0] + line.rect[0], paragraph.box[1] + line.rect[1],
                                             line.rect[2], line.rect[3]};
                const auto x = broken_ink(false, r);
                const auto y = broken_ink(true, r);
                const double dx = std::abs(x[1] / x[0] - y[1] / y[0]);
                const double dy = std::abs(x[2] / x[0] - y[2] / y[0]);
                std::cout << "broken line [" << line.text << "]: centroid difference " << dx << ", " << dy << ", ink "
                          << y[0] / x[0] << '\n';
                require(x[0] > 0 && dx < 1 && dy < 1 && std::abs(y[0] / x[0] - 1) < .25,
                        "native pass draws the broken line [" + line.text + "] of the reference raster");
            }
        }
        refused("overflow-wrap:wobble", "", "overflow-wrap wobble outside scene profile");
        refused("word-break:auto-phrase", "", "word-break auto-phrase outside scene profile");
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
        std::cout
            << "PASS scene inline contract: reuse and relayout, fragment hits and rects, sliced boxes, lines, ruby, "
               "text decorations, overflow-wrap and word-break, "
               "native lines, diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
