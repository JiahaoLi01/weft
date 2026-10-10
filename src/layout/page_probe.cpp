#include "caseelephant/page_probe.hpp"
#include "caseelephant/base/css_scalar.hpp"
#include "caseelephant/constants/layout.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = std::map<std::string, std::string>;

        float px(const Style& s, const std::string& key, float fallback = 0) {
            if (!s.contains(key)) {
                return fallback;
            }
            const auto& v = s.at(key);
            if (v == "0") {
                return 0;
            }
            if (!v.ends_with("px")) {
                throw std::invalid_argument("WEFT_PAGE_UNIT: " + key);
            }
            const auto n = css::parse_number<float>(v.substr(0, v.size() - 2));
            if (!n || *n < 0 || *n > constants::layout::max_length_px) {
                throw std::invalid_argument("WEFT_PAGE_VALUE: " + key);
            }
            return *n;
        }

        std::array<float, 4> color(const std::string& v) {
            return parse_css_color(v);
        }

        struct Styled {
            std::string id;
            std::string text;
            Style style;
            bool hidden{};
            NodeIdentity identity;
            std::size_t parent{};
            bool disabled{};
        };

        struct Builder {
            std::vector<Styled> nodes;

            FlexNode convert(const DomSnapshot& node, const ComputedStyle& computed, bool parent_hidden = false,
                             unsigned depth = 0, std::size_t parent_index = std::numeric_limits<std::size_t>::max(),
                             bool parent_disabled = false) {
                if (depth > constants::max_document_depth || nodes.size() >= constants::max_document_nodes) {
                    throw std::invalid_argument("WEFT_PAGE_LIMIT: node/depth bound");
                }
                if (node.type != constants::element_node_type || node.namespace_uri != constants::xhtml_namespace_uri ||
                    (node.local_name != "div" && node.local_name != "button")) {
                    throw std::invalid_argument("WEFT_PAGE_PROFILE: only explicit flex div/button elements");
                }
                Style style = computed.values;
                if (!style.contains("font-size")) {
                    style["font-size"] = constants::default_font_size;
                }
                if (!style.contains("line-height")) {
                    style["line-height"] = constants::default_line_height;
                }
                if (!style.contains("color")) {
                    style["color"] = constants::default_color;
                }
                if (!style.contains("font-family")) {
                    style["font-family"] = constants::latin_font_family;
                }
                if (!style.contains("font-weight")) {
                    style["font-weight"] = constants::default_font_weight;
                }
                if (!style.contains("text-align")) {
                    style["text-align"] = "left";
                }
                if (!style.contains("pointer-events")) {
                    style["pointer-events"] = "auto";
                }
                if (style["pointer-events"] != "auto" && style["pointer-events"] != "none") {
                    throw std::invalid_argument("WEFT_PAGE_INPUT: pointer-events auto/none profile");
                }
                if (style.contains("position") && style["position"] != "static") {
                    throw std::invalid_argument("WEFT_PAGE_PAINT_ORDER: positioned stacking requires future adapter");
                }
                if (!style.contains("display") || (style["display"] != "flex" && style["display"] != "none")) {
                    throw std::invalid_argument("WEFT_PAGE_FORMAT: explicit display:flex required");
                }
                if (style["font-family"] != constants::latin_font_family ||
                    style["font-weight"] != constants::default_font_weight || style["text-align"] != "left") {
                    throw std::invalid_argument("WEFT_PAGE_FONT: fixed Roboto regular/left profile");
                }
                const auto id = node.attributes.contains("id") ? node.attributes.at("id")
                                                               : "node-" + std::to_string(node.identity.slot) + "-" +
                                                                     std::to_string(node.identity.generation);
                std::string text;
                bool elements{};
                for (const auto& child : node.children) {
                    if (child.type == constants::text_node_type) {
                        text += child.data;
                    } else if (child.type == constants::element_node_type) {
                        elements = true;
                    } else {
                        throw std::invalid_argument("WEFT_PAGE_PROFILE: unsupported child node type");
                    }
                }
                // CSS normal-space collapse for this single-line profile.
                std::istringstream words(text);
                std::string word;
                std::string collapsed;
                while (words >> word) {
                    if (!collapsed.empty()) {
                        collapsed += ' ';
                    }
                    collapsed += word;
                }
                if (elements && !collapsed.empty()) {
                    throw std::invalid_argument("WEFT_PAGE_INLINE: mixed text/element children unsupported");
                }
                if (!collapsed.empty() && (!style.contains("white-space") || style["white-space"] != "nowrap")) {
                    throw std::invalid_argument("WEFT_PAGE_TEXT: single-line nowrap required");
                }
                const bool hidden = parent_hidden || style["display"] == "none";
                (void)color(style["color"]);
                if (style.contains("background-color")) {
                    (void)color(style["background-color"]);
                }
                for (const auto* key : {"border-width", "border-top-width", "border-right-width", "border-bottom-width",
                                        "border-left-width"}) {
                    if (px(style, key) != 0) {
                        throw std::invalid_argument("WEFT_PAGE_PAINT: nonzero borders unsupported");
                    }
                }
                if (style.contains("overflow") && style["overflow"] != "visible") {
                    throw std::invalid_argument("WEFT_PAGE_PAINT: overflow clipping unsupported");
                }
                const auto index = nodes.size();
                const bool disabled =
                    parent_disabled || (node.local_name == "button" && node.attributes.contains("disabled"));
                nodes.push_back({id, collapsed, style, hidden, node.identity, parent_index, disabled});

                for (const auto* paint :
                     {"color", "background-color", "font-family", "font-weight", "text-align", "pointer-events"}) {
                    style.erase(paint);
                }
                FlexNode layout{id, std::move(style), collapsed, {}};
                std::size_t styled_child{};
                for (const auto& child : node.children) {
                    if (child.type == constants::element_node_type) {
                        layout.children.push_back(
                            convert(child, computed.children[styled_child++], hidden, depth + 1, index, disabled));
                    }
                }
                return layout;
            }
        };
    } // namespace

    Page build_page(const DomSnapshot& root, std::string_view stylesheet, Font& font) {
        const auto computed = compute_style(root, stylesheet, flex_page_style_profile());
        Builder builder;
        const auto input = builder.convert(root, computed);
        Page page;
        page.boxes = run_flex(input, font, FlexMode::CssAdapter);
        if (page.boxes.size() != builder.nodes.size()) {
            throw std::logic_error("WEFT_PAGE_INTERNAL: node/box mapping");
        }
        for (std::size_t i = 0; i < builder.nodes.size(); ++i) {
            const auto& n = builder.nodes[i];
            const auto& box = page.boxes[i];
            if (n.id != box.id) {
                throw std::logic_error("WEFT_PAGE_INTERNAL: order mapping");
            }
            page.hits.push_back({n.identity, n.parent, box.x, box.y, box.width, box.height,
                                 !n.hidden && n.style.at("pointer-events") == "auto", n.disabled});
            if (n.hidden) {
                continue;
            }
            if (n.style.contains("background-color")) {
                page.paints.push_back({PaintKind::Rectangle,
                                       n.id,
                                       {},
                                       box.x,
                                       box.y,
                                       box.width,
                                       box.height,
                                       0,
                                       color(n.style.at("background-color")),
                                       {}});
            }
            if (n.text.empty()) {
                continue;
            }
            const float size = px(n.style, "font-size", constants::default_font_size_px);
            const float left = px(n.style, "padding-left", px(n.style, "padding"));
            const float right = px(n.style, "padding-right", px(n.style, "padding"));
            const float top = px(n.style, "padding-top", px(n.style, "padding"));
            if (box.text_lines != 1 ||
                box.text_width > box.width - left - right + constants::layout::text_overflow_tolerance_px) {
                throw std::invalid_argument("WEFT_PAGE_TEXT: text overflow requires future clipping/inline adapter");
            }
            auto shaped = font.shape(n.text, size);
            if (shaped.missing_glyph) {
                throw std::invalid_argument("WEFT_PAGE_FONT: missing glyph");
            }
            page.paints.push_back({PaintKind::Text, n.id, n.text, box.x + left, box.y + top + box.baseline, 0, 0, size,
                                   color(n.style.at("color")), std::move(shaped)});
        }
        return page;
    }

    Page build_page(const DomSnapshot& root, Font& font) {
        return build_page(root, "", font);
    }

} // namespace weft
