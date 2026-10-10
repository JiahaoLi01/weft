#include "caseelephant/positioned_page_probe.hpp"
#include "caseelephant/base/css_scalar.hpp"
#include "caseelephant/base/rect.hpp"
#include "caseelephant/constants/layout.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = std::map<std::string, std::string>;

        using Box = geometry::Rect;
        using css::trim;
        using geometry::intersect;

        [[noreturn]] void fail(const std::string& text) {
            throw_diagnostic(Diag::PositionedProfile, text);
        }

        std::string value(const Style& s, const char* name, const char* fallback = "") {
            return css::style_value(s, name, fallback);
        }

        float number(std::string_view v) {
            const std::string text(v);
            const auto n =
                css::parse_number<float>(text, static_cast<float>(constants::layout::max_length_magnitude_px));
            if (!n) {
                fail("finite numeric value " + text);
            }
            return *n;
        }

        float length(std::string v, float base) {
            v = trim(v);
            if (v == "0") {
                return 0;
            }
            if (v.starts_with("calc(") && v.ends_with(')')) {
                // Bounded percent +/- px expression; CSS whitespace around +/- required.
                v = trim(v.substr(5, v.size() - 6));
                auto p = v.find(" + ");
                bool minus = false;
                if (p == std::string::npos) {
                    p = v.find(" - ");
                    minus = true;
                }
                if (p == std::string::npos || !trim(v.substr(0, p)).ends_with('%') ||
                    !trim(v.substr(p + 3)).ends_with("px")) {
                    fail("calc expression");
                }
                return length(v.substr(0, p), base) + (minus ? -1 : 1) * length(v.substr(p + 3), base);
            }
            const auto [digits, unit] = css::split_unit(v);
            if (unit == css::Unit::Px) {
                return number(digits);
            }
            if (unit == css::Unit::Percent) {
                return base * number(digits) / constants::percent_scale;
            }
            fail("length " + v);
        }

        std::array<float, 4> color(const std::string& v) {
            return parse_css_color(v);
        }

        struct Node {
            const DomSnapshot* dom{};
            Style style;
            Box box;
            Box clip;
            std::string text;
            std::string id;
            std::vector<std::size_t> children;
            std::size_t parent{};
            bool hidden{};
            bool disabled{};
            bool hit{};
            int z{};
        };

        struct Builder {
            Font& font;
            FontSet* fonts{};
            std::vector<Node> nodes;
            std::vector<const DomSnapshot*> path;

            std::size_t layout(const DomSnapshot& dom, const ComputedStyle& computed, Box containing, Box flow,
                               Box clip, std::size_t parent, bool hidden = false, bool disabled = false) {
                if (path.size() >= constants::max_document_depth || nodes.size() >= constants::max_document_nodes) {
                    fail("DOM node/depth bound");
                }
                if (dom.type != constants::element_node_type || dom.namespace_uri != constants::xhtml_namespace_uri ||
                    (dom.local_name != "div" && dom.local_name != "i" && dom.local_name != "img" &&
                     dom.local_name != "canvas" && dom.local_name != "button" && dom.local_name != "span" &&
                     dom.local_name != "b" && dom.local_name != "ul" && dom.local_name != "li")) {
                    fail("HTML element profile");
                }
                path.push_back(&dom);
                Style s = computed.values;
                for (const auto* k : {"margin", "padding", "border"}) {
                    if (s.contains(k) && s[k] != "0" && s[k] != "0px") {
                        fail("nonzero box edge " + std::string(k));
                    }
                }
                if (value(s, "box-sizing", "content-box") != "content-box" && value(s, "box-sizing") != "border-box") {
                    fail("box sizing");
                }
                if (value(s, "max-width", "none") != "none" ||
                    (value(s, "user-select", "auto") != "auto" && value(s, "user-select") != "none")) {
                    fail("image decoration");
                }
                const auto position = value(s, "position", "static");
                const auto display = value(s, "display", "block");
                if (position != "static" && position != "relative" && position != "absolute") {
                    fail("position " + position);
                }
                if (display != "block" && display != "none") {
                    fail("display " + display);
                }
                if (hidden || display == "none") {
                    // No box is generated anywhere under a display:none ancestor.
                    // Keep zero-valued entries for DOM/browser geometry comparisons.
                    const auto index = nodes.size();
                    nodes.push_back(
                        {&dom,
                         s,
                         {},
                         clip,
                         {},
                         dom.attributes.contains("id") ? dom.attributes.at("id") : "box-" + std::to_string(index),
                         {},
                         parent,
                         true,
                         disabled,
                         false,
                         0});
                    std::size_t styled_child{};
                    for (const auto& child : dom.children) {
                        if (child.type == constants::element_node_type) {
                            const auto c =
                                layout(child, computed.children[styled_child++], {}, {}, clip, index, true, disabled);
                            nodes[index].children.push_back(c);
                        }
                    }
                    path.pop_back();
                    return index;
                }
                if (dom.local_name != "div" && position != "absolute" && !s.contains("display")) {
                    fail("inline/replaced normal flow requires explicit block context");
                }
                if (value(s, "overflow", "visible") != "visible" && value(s, "overflow") != "hidden") {
                    fail("overflow profile");
                }
                if (value(s, "isolation", "auto") != "auto" && value(s, "isolation") != "isolate") {
                    fail("isolation");
                }
                if (value(s, "pointer-events", "auto") != "auto" && value(s, "pointer-events") != "none") {
                    fail("pointer events");
                }
                if (s.contains("font-style") && s.at("font-style") != "normal" && s.at("font-style") != "italic") {
                    fail("font style");
                }
                if (s.contains("list-style") && s.at("list-style") != "none") {
                    fail("list markers");
                }
                if (dom.local_name == "li" && value(s, "list-style") != "none") {
                    fail("list markers require explicit none");
                }
                if (dom.local_name == "ul" && (!s.contains("margin") || !s.contains("padding"))) {
                    fail("list UA box edges require explicit zero reset");
                }
                int z{};
                if (value(s, "z-index", "auto") != "auto") {
                    const auto v = value(s, "z-index");
                    const auto n = number(v);
                    if (n != std::floor(n) || std::abs(n) > constants::layout::max_z_index_magnitude) {
                        fail("z-index integer");
                    }
                    z = int(n);
                    if (position == "static" || parent == std::numeric_limits<std::size_t>::max() ||
                        value(nodes[parent].style, "isolation", "auto") != "isolate") {
                        fail("non-auto z-index requires direct isolated positioned parent");
                    }
                }
                const bool absolute = position == "absolute";
                const Box base = absolute ? containing : flow;
                Box box{base.x, base.y};
                auto specified = [&](const char* k) {
                    return s.contains(k) && s.at(k) != "auto";
                };
                const float left = specified("left") ? length(s.at("left"), base.w) : 0;
                const float top = specified("top") ? length(s.at("top"), base.h) : 0;
                const float right = specified("right") ? length(s.at("right"), base.w) : 0;
                const float bottom = specified("bottom") ? length(s.at("bottom"), base.h) : 0;
                box.w = specified("width") ? std::max(0.f, length(s.at("width"), base.w))
                                           : base.w - (absolute ? left + right : 0);
                if (specified("height")) {
                    box.h = std::max(0.f, length(s.at("height"), base.h));
                } else if (absolute && specified("top") && specified("bottom")) {
                    box.h = std::max(0.f, base.h - top - bottom);
                } else if (dom.local_name != "canvas") {
                    fail("definite height required");
                }
                if (dom.local_name == "canvas") {
                    if (!dom.children.empty()) {
                        fail("canvas fallback content outside profile");
                    }
                    auto dimension = [&](const char* name, float fallback) {
                        if (!dom.attributes.contains(name)) {
                            return fallback;
                        }
                        const auto& text = dom.attributes.at(name);
                        std::size_t p = text.find_first_not_of(" \t\r\n\f");
                        if (p == text.npos) {
                            return fallback;
                        }
                        if (text[p] == '+') {
                            ++p;
                        }
                        if (p == text.size() || !std::isdigit(static_cast<unsigned char>(text[p]))) {
                            return fallback;
                        }
                        float n = 0;
                        while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) {
                            n = n * 10 + (text[p++] - '0');
                            if (n > constants::max_canvas_dimension) {
                                fail("canvas backing size");
                            }
                        }
                        return n;
                    };
                    const float iw = dimension("width", constants::default_canvas_width);
                    const float ih = dimension("height", constants::default_canvas_height);
                    if (!specified("width")) {
                        box.w = specified("height") && ih > 0 ? box.h * iw / ih : iw;
                    }
                    if (!specified("height")) {
                        box.h = specified("width") && iw > 0 ? box.w * ih / iw : ih;
                    }
                }
                if (dom.local_name != "canvas" && !specified("width") && absolute &&
                    (!specified("left") || !specified("right"))) {
                    fail("absolute auto width requires both insets");
                }
                if (box.w < 0 || box.w > constants::layout::max_length_magnitude_px ||
                    box.h > constants::layout::max_length_magnitude_px || !std::isfinite(box.w) ||
                    !std::isfinite(box.h)) {
                    fail("box bound");
                }
                if (absolute) {
                    box.x += specified("left") ? left : specified("right") ? base.w - right - box.w : 0;
                    box.y += specified("top") ? top : specified("bottom") ? base.h - bottom - box.h : 0;
                } else if (position == "relative") {
                    box.x += specified("left") ? left : -right;
                    box.y += specified("top") ? top : -bottom;
                }
                std::string text;
                for (const auto& child : dom.children) {
                    if (child.type == constants::text_node_type) {
                        text += child.data;
                    } else if (child.type != constants::element_node_type) {
                        fail("child node type");
                    }
                }
                std::istringstream words(text);
                std::string word;
                text.clear();
                while (words >> word) {
                    if (!text.empty()) {
                        text += ' ';
                    }
                    text += word;
                }
                if (!text.empty() && std::any_of(dom.children.begin(), dom.children.end(), [](const auto& c) {
                        return c.type == constants::element_node_type;
                    })) {
                    fail("mixed inline content");
                }
                hidden = hidden || display == "none";
                disabled = disabled || (dom.local_name == "button" && dom.attributes.contains("disabled"));
                const auto index = nodes.size();
                nodes.push_back(
                    {&dom,
                     s,
                     box,
                     clip,
                     text,
                     dom.attributes.contains("id") ? dom.attributes.at("id") : "box-" + std::to_string(index),
                     {},
                     parent,
                     hidden,
                     disabled,
                     !hidden && value(s, "pointer-events", "auto") == "auto",
                     z});
                Box child_clip = clip;
                if (value(s, "overflow", "visible") == "hidden") {
                    child_clip = intersect(clip, box);
                }
                const Box child_containing = position == "static" ? containing : box;
                float cursor{};
                std::size_t styled_child{};
                for (const auto& child : dom.children) {
                    if (child.type == constants::element_node_type) {
                        // Definite block flow only; positioned children do not advance flow.
                        const auto c =
                            layout(child, computed.children[styled_child++], child_containing,
                                   {box.x, box.y + cursor, box.w, box.h}, child_clip, index, hidden, disabled);
                        nodes[index].children.push_back(c);
                        if (!nodes[c].hidden && value(nodes[c].style, "position", "static") != "absolute") {
                            cursor += nodes[c].box.h;
                        }
                    }
                }
                path.pop_back();
                return index;
            }

            void paint(std::size_t index, PositionedPage& output) {
                const auto& n = nodes[index];
                if (n.hidden) {
                    return;
                }
                output.geometry.hits.push_back(
                    {n.dom->identity, n.parent, n.box.x, n.box.y, n.box.w, n.box.h, n.hit, n.disabled});
                auto quad = [&](std::array<float, 4> c, const std::string& image,
                                const NodeIdentity* canvas = nullptr) {
                    const auto b = intersect(n.box, n.clip);
                    if (b.w <= 0 || b.h <= 0) {
                        return;
                    }
                    PositionedPaint p;
                    p.paint = {PaintKind::Rectangle, n.id, {}, b.x, b.y, b.w, b.h, 0, c, {}};
                    p.image_source = image;
                    if (canvas) {
                        p.canvas_identity = *canvas;
                    }
                    p.uv = {(b.x - n.box.x) / n.box.w, (b.y - n.box.y) / n.box.h, b.w / n.box.w, b.h / n.box.h};
                    p.clip = {n.clip.x, n.clip.y, n.clip.w, n.clip.h};
                    output.paints.push_back(std::move(p));
                };
                if (n.style.contains("background-color")) {
                    quad(color(n.style.at("background-color")), {});
                }
                if (n.dom->local_name == "img") {
                    if (!n.dom->attributes.contains("src") || !n.dom->children.empty()) {
                        fail("image source/children");
                    }
                    quad({1, 1, 1, 1}, n.dom->attributes.at("src"));
                }
                if (n.dom->local_name == "canvas") {
                    quad({1, 1, 1, 1}, {}, &n.dom->identity);
                }
                if (!n.text.empty()) {
                    if (value(n.style, "font-style", "normal") != "normal") {
                        fail("italic text profile");
                    }
                    const auto family = value(n.style, "font-family", constants::latin_font_family);
                    if ((family != constants::latin_font_family &&
                         !(fonts && family == constants::default_font_family)) ||
                        value(n.style, "font-weight", constants::default_font_weight) !=
                            constants::default_font_weight ||
                        value(n.style, "white-space", "normal") != "nowrap" ||
                        value(n.style, "text-align", "left") != "left") {
                        fail("fixed single-line font family/weight/alignment profile");
                    }
                    const float size = length(value(n.style, "font-size", constants::default_font_size), 0);
                    const float line = length(value(n.style, "line-height", constants::default_line_height), 0);
                    if (size <= 0 || size > constants::layout::max_font_size_px || line <= 0 ||
                        line > constants::layout::max_length_px) {
                        fail("font size/line height bound");
                    }
                    // This explicit-height single-line profile uses the primary face's
                    // strut. Fallback changes glyphs, not the baseline of the element.
                    // A general inline line-box builder must account for all runs later.
                    const auto m = font.metrics(size);
                    const float baseline = (line - m.ascent - m.descent) / 2 + m.ascent;
                    ShapedText shaped;
                    if (fonts && family == constants::default_font_family) {
                        auto fallback = fonts->shape(n.text, size);
                        shaped = std::move(fallback.shaped);
                    } else {
                        shaped = font.shape(n.text, size);
                    }
                    if (shaped.missing_glyph) {
                        fail("missing font glyph");
                    }
                    PositionedPaint p;
                    p.paint = {PaintKind::Text,
                               n.id,
                               n.text,
                               n.box.x,
                               n.box.y + baseline,
                               0,
                               0,
                               size,
                               color(value(n.style, "color", "#ffffff")),
                               std::move(shaped)};
                    p.clip = {n.clip.x, n.clip.y, n.clip.w, n.clip.h};
                    output.paints.push_back(std::move(p));
                }
                auto children = n.children;
                std::stable_sort(children.begin(), children.end(), [&](auto a, auto b) {
                    return nodes[a].z < nodes[b].z;
                });
                for (const auto child : children) {
                    paint(child, output);
                }
            }
        };
    } // namespace

    namespace {
        PositionedPage build(const DomSnapshot& root, const ComputedStyle& computed, float w, float h, Font& font,
                             FontSet* fonts) {
            if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 ||
                w > constants::layout::max_viewport_size_px || h > constants::layout::max_viewport_size_px) {
                fail("viewport bound");
            }
            validate_style_pair(root, computed);
            Builder builder{font, fonts, {}, {}};
            const Box viewport{0, 0, w, h};
            builder.layout(root, computed, viewport, viewport, viewport, std::numeric_limits<std::size_t>::max());
            PositionedPage output;
            for (const auto& node : builder.nodes) {
                output.geometry.boxes.push_back({node.id, node.box.x, node.box.y, node.box.w, node.box.h});
                ElementGeometry g;
                g.identity = node.dom->identity;
                g.rect = {node.box.x, node.box.y, node.box.w, node.box.h};
                if (!node.hidden) {
                    g.client_rects.push_back(g.rect);
                }
                g.client_width = g.scroll_width = node.box.w;
                g.client_height = g.scroll_height = node.box.h;
                output.elements.push_back(std::move(g));
            }
            builder.paint(0, output);
            // Geometry/hit identities are layout-indexed; paint can reorder. Hit testing
            // for this new formatting context is not exposed until its separate contract.
            output.geometry.hits.clear();
            return output;
        }
    } // namespace

    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view css, float w, float h, Font& font) {
        try {
            return build(root, compute_style(root, css, positioned_style_profile()), w, h, font, nullptr);
        } catch (const DiagnosticError& e) {
            if (e.code() == Diag::StyleProfile) {
                fail(e.what());
            }
            throw;
        }
    }

    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view css, float w, float h,
                                         FontSet& fonts) {
        try {
            return build(root, compute_style(root, css, positioned_style_profile()), w, h, fonts.primary(), &fonts);
        } catch (const DiagnosticError& e) {
            if (e.code() == Diag::StyleProfile) {
                fail(e.what());
            }
            throw;
        }
    }

    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float w, float h,
                                         Font& font) {
        return build(root, style, w, h, font, nullptr);
    }

    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float w, float h,
                                         FontSet& fonts) {
        return build(root, style, w, h, fonts.primary(), &fonts);
    }

} // namespace weft
