#include "caseelephant/flex_probe.hpp"
#include "caseelephant/base/css_scalar.hpp"
#include "caseelephant/constants/layout.hpp"
#include <yoga/Yoga.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = std::map<std::string, std::string>;

        std::string value(const Style& s, const std::string& key, const char* fallback = "") {
            return css::style_value(s, key, fallback);
        }

        float number(std::string_view input) {
            return css::parse_number<float>(std::string(input),
                                            static_cast<float>(constants::layout::max_flex_number_magnitude))
                .value();
        }

        struct Length {
            float value{};
            bool percent{};
            bool automatic{};
        };

        Length length(const std::string& input) {
            if (input.empty() || input == "auto") {
                return {0, false, true};
            }
            const auto [digits, unit] = css::split_unit(input);
            if (unit == css::Unit::Px) {
                return {number(digits)};
            }
            if (unit == css::Unit::Percent) {
                return {number(digits), true};
            }
            if (input == "0") {
                return {};
            }
            throw std::invalid_argument("WEFT_UNSUPPORTED_UNIT: " + input);
        }

        float px(const Style& style, const std::string& key, float fallback = 0) {
            if (!style.contains(key)) {
                return fallback;
            }
            const auto n = length(style.at(key));
            if (n.automatic || n.percent) {
                throw std::invalid_argument("WEFT_UNSUPPORTED_INTRINSIC: " + key +
                                            " requires px in probe intrinsic sizing");
            }
            return n.value;
        }

        bool row(const Style& style) {
            return value(style, "display") != "block" && value(style, "flex-direction", "row").starts_with("row");
        }

        YGAlign align(const std::string& v) {
            if (v == "auto") {
                return YGAlignAuto;
            }
            if (v == "stretch") {
                return YGAlignStretch;
            }
            if (v == "flex-start") {
                return YGAlignFlexStart;
            }
            if (v == "flex-end") {
                return YGAlignFlexEnd;
            }
            if (v == "center") {
                return YGAlignCenter;
            }
            if (v == "baseline") {
                return YGAlignBaseline;
            }
            if (v == "space-between") {
                return YGAlignSpaceBetween;
            }
            if (v == "space-around") {
                return YGAlignSpaceAround;
            }
            if (v == "space-evenly") {
                return YGAlignSpaceEvenly;
            }
            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: " + v);
        }

        struct TextLayout {
            float width{};
            float height{};
            float baseline{};
            std::size_t lines{};
        };
        struct Engine;

        struct Node {
            Engine* engine{};
            const FlexNode* input{};
            YGNodeRef yoga{};
            std::vector<std::unique_ptr<Node>> children;
            float font_size{constants::layout::flex_default_font_size_px};
            float line_height{constants::layout::flex_default_line_height_px};
            float min_width{};
            float max_width{};
            float intrinsic_height{};
            float auto_min_height{};
            std::size_t measure_calls{};

            ~Node() {
                if (yoga) {
                    YGNodeFree(yoga);
                }
            }
        };

        struct Engine {
            Font& font;
            FontSet* fonts{};
            FlexMode mode;
            YGConfigRef config{YGConfigNew()};
            std::exception_ptr callback_error;
            std::size_t node_count{};
            std::size_t text_bytes{};
            std::set<std::string> identities;

            Engine(Font& f, FlexMode m, FontSet* fs = nullptr) : font(f), fonts(fs), mode(m) {
                if (!config) {
                    throw std::bad_alloc();
                }
                YGConfigSetUseWebDefaults(config, true);
                YGConfigSetPointScaleFactor(config, 0);
                YGConfigSetErrata(config, YGErrataNone);
            }

            ~Engine() {
                YGConfigFree(config);
            }

            float horizontal(const Style& s) const {
                return px(s, "padding-left", px(s, "padding")) + px(s, "padding-right", px(s, "padding")) +
                       px(s, "border-left-width", px(s, "border-width")) +
                       px(s, "border-right-width", px(s, "border-width"));
            }

            float vertical(const Style& s) const {
                return px(s, "padding-top", px(s, "padding")) + px(s, "padding-bottom", px(s, "padding")) +
                       px(s, "border-top-width", px(s, "border-width")) +
                       px(s, "border-bottom-width", px(s, "border-width"));
            }

            TextLayout text(const Node& n, float available = std::numeric_limits<float>::infinity()) const {
                if (n.input->inline_content) {
                    if (!fonts) {
                        throw std::invalid_argument("WEFT_INLINE_PROFILE: font set required");
                    }
                    auto result = layout_inline(*n.input->inline_content, available, *fonts);
                    return {result.width, result.height, result.baseline, result.lines};
                }
                if (n.input->text.empty()) {
                    return {};
                }
                if (mode == FlexMode::RestrictedBlockAdapter || fonts) {
                    const auto lines =
                        layout_text(n.input->text, n.font_size, available,
                                    value(n.input->style, "white-space", "normal") == "nowrap", font, fonts);
                    float widest{};
                    for (const auto& line : lines) {
                        widest = std::max(widest, line.shaped.advance);
                    }
                    const auto m = font.metrics(n.font_size);
                    return {widest, lines.size() * n.line_height, (n.line_height - m.ascent - m.descent) / 2 + m.ascent,
                            lines.size()};
                }
                for (unsigned char c : n.input->text) {
                    if ((c < constants::layout::codepoint_space && c != ' ') ||
                        c >= constants::layout::codepoint_delete) {
                        throw std::invalid_argument(
                            "WEFT_UNSUPPORTED_TEXT_BREAK: M0 layout fixtures require printable ASCII");
                    }
                }
                const bool nowrap = value(n.input->style, "white-space", "normal") == "nowrap";
                const float space = font.shape(" ", n.font_size).advance;
                std::istringstream words(n.input->text);
                std::string word;
                float line{};
                float widest{};
                std::size_t lines = 0;
                while (words >> word) {
                    const float width = font.shape(word, n.font_size).advance;
                    if (!lines) {
                        lines = 1;
                    }
                    if (!nowrap && line > 0 && line + space + width > available + constants::layout::line_fit_epsilon) {
                        widest = std::max(widest, line);
                        ++lines;
                        line = 0;
                    }
                    if (line > 0) {
                        line += space;
                    }
                    line += width;
                }
                const auto metrics = font.metrics(n.font_size);
                const float baseline = (n.line_height - metrics.ascent - metrics.descent) / 2 + metrics.ascent;
                return {std::max(widest, line), lines * n.line_height, baseline, lines};
            }

            static YGSize measure(YGNodeConstRef y, float width, YGMeasureMode wm, float height,
                                  YGMeasureMode hm) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    ++n.measure_calls;
                    const auto measured = n.engine->text(
                        n, wm == YGMeasureModeUndefined ? std::numeric_limits<float>::infinity() : width);
                    const float natural_width = n.engine->text(n).width;
                    return {wm == YGMeasureModeExactly  ? width
                            : wm == YGMeasureModeAtMost ? std::min(width, natural_width)
                                                        : measured.width,
                            hm == YGMeasureModeExactly  ? height
                            : hm == YGMeasureModeAtMost ? std::min(height, measured.height)
                                                        : measured.height};
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return {};
                }
            }

            static float baseline(YGNodeConstRef y, float, float) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    return n.engine->text(n).baseline +
                           px(n.input->style, "padding-top", px(n.input->style, "padding")) +
                           px(n.input->style, "border-top-width", px(n.input->style, "border-width"));
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return 0;
                }
            }

            void apply(Node& n) {
                auto y = n.yoga;
                const auto& s = n.input->style;
                static const std::set<std::string> supported{"display",
                                                             "width",
                                                             "height",
                                                             "min-width",
                                                             "min-height",
                                                             "max-width",
                                                             "max-height",
                                                             "flex",
                                                             "flex-grow",
                                                             "flex-shrink",
                                                             "flex-basis",
                                                             "flex-direction",
                                                             "flex-wrap",
                                                             "align-items",
                                                             "align-content",
                                                             "align-self",
                                                             "justify-content",
                                                             "position",
                                                             "overflow",
                                                             "box-sizing",
                                                             "gap",
                                                             "row-gap",
                                                             "column-gap",
                                                             "padding",
                                                             "padding-top",
                                                             "padding-right",
                                                             "padding-bottom",
                                                             "padding-left",
                                                             "margin",
                                                             "margin-top",
                                                             "margin-right",
                                                             "margin-bottom",
                                                             "margin-left",
                                                             "border-width",
                                                             "border-top-width",
                                                             "border-right-width",
                                                             "border-bottom-width",
                                                             "border-left-width",
                                                             "top",
                                                             "right",
                                                             "bottom",
                                                             "left",
                                                             "font-size",
                                                             "line-height",
                                                             "white-space"};
                if (s.size() > supported.size()) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: excessive styles");
                }
                if ((s.contains("flex") &&
                     (s.contains("flex-grow") || s.contains("flex-shrink") || s.contains("flex-basis"))) ||
                    (s.contains("gap") && (s.contains("row-gap") || s.contains("column-gap")))) {
                    throw std::invalid_argument(
                        "WEFT_UNSUPPORTED_STYLE_CONFLICT: resolve declaration order before adapter");
                }
                for (const auto* shorthand : {"padding", "margin", "border-width"}) {
                    if (!s.contains(shorthand)) {
                        continue;
                    }
                    const std::string prefix =
                        std::string(shorthand) == "border-width" ? "border-" : std::string(shorthand) + '-';
                    for (const auto& [key, ignored] : s) {
                        (void)ignored;
                        if (key != shorthand && key.starts_with(prefix)) {
                            throw std::invalid_argument(
                                "WEFT_UNSUPPORTED_STYLE_CONFLICT: resolve edge declarations before adapter");
                        }
                    }
                }
                for (const auto& [key, v] : s) {
                    if (!supported.contains(key)) {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_PROPERTY: " + key);
                    }
                    if (v.size() > constants::layout::max_style_value_bytes) {
                        throw std::invalid_argument("WEFT_LAYOUT_LIMIT: excessive style value");
                    }
                    if (key == "flex-grow" || key == "flex-shrink") {
                        if (number(v) < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative flex factor");
                        }
                    } else if (key.ends_with("width") || key.ends_with("height") || key.starts_with("padding") ||
                               key.ends_with("gap") || key == "font-size" || key == "flex-basis") {
                        const auto l = length(v);
                        if (!l.automatic && l.value < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative dimension");
                        }
                    }
                }
                if (mode != FlexMode::RawYoga) {
                    YGNodeStyleSetPositionType(y, YGPositionTypeStatic);
                    YGNodeStyleSetBoxSizing(y, YGBoxSizingContentBox);
                }
                for (const auto& [key, v] : s) {
                    const auto dim = [&](auto points, auto percent, void (*automatic)(YGNodeRef) = nullptr) {
                        auto l = length(v);
                        if (l.automatic) {
                            if (automatic) {
                                automatic(y);
                            } else {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_AUTO: " + key);
                            }
                        } else if (l.percent) {
                            percent(y, l.value);
                        } else {
                            points(y, l.value);
                        }
                    };
                    if (key == "display") {
                        if (v != "flex" && v != "inline-flex" && v != "none" &&
                            !(mode == FlexMode::RestrictedBlockAdapter && v == "block")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: " + v);
                        }
                        YGNodeStyleSetDisplay(y, v == "none" ? YGDisplayNone : YGDisplayFlex);
                    } else if (key == "width") {
                        dim(YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto);
                    } else if (key == "height") {
                        dim(YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto);
                    } else if (key == "min-width" && v != "auto") {
                        dim(YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent);
                    } else if (key == "min-height" && v != "auto") {
                        dim(YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent);
                    } else if (key == "max-width") {
                        dim(YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent);
                    } else if (key == "max-height") {
                        dim(YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent);
                    } else if (key == "flex-direction") {
                        if (v == "row") {
                            YGNodeStyleSetFlexDirection(y, YGFlexDirectionRow);
                        } else if (v == "column") {
                            YGNodeStyleSetFlexDirection(y, YGFlexDirectionColumn);
                        } else if (v == "row-reverse") {
                            YGNodeStyleSetFlexDirection(y, YGFlexDirectionRowReverse);
                        } else if (v == "column-reverse") {
                            YGNodeStyleSetFlexDirection(y, YGFlexDirectionColumnReverse);
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_DIRECTION: " + v);
                        }
                    } else if (key == "flex-wrap") {
                        if (v != "nowrap" && v != "wrap" && v != "wrap-reverse") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_WRAP: " + v);
                        }
                        YGNodeStyleSetFlexWrap(y, v == "nowrap" ? YGWrapNoWrap
                                                  : v == "wrap" ? YGWrapWrap
                                                                : YGWrapWrapReverse);
                    } else if (key == "flex-grow") {
                        YGNodeStyleSetFlexGrow(y, number(v));
                    } else if (key == "flex-shrink") {
                        YGNodeStyleSetFlexShrink(y, number(v));
                    } else if (key == "flex-basis") {
                        dim(YGNodeStyleSetFlexBasis, YGNodeStyleSetFlexBasisPercent, YGNodeStyleSetFlexBasisAuto);
                    } else if (key == "flex") {
                        float grow{};
                        float shrink{1};
                        std::string basis;
                        if (v == "none") {
                            grow = 0;
                            shrink = 0;
                            basis = "auto";
                        } else if (v == "auto") {
                            grow = 1;
                            basis = "auto";
                        } else {
                            std::istringstream tokens(v);
                            std::string a;
                            std::string b;
                            std::string c;
                            std::string extra;
                            tokens >> a;
                            grow = number(a);
                            if (tokens >> b) {
                                if (!(tokens >> c) || (tokens >> extra)) {
                                    throw std::invalid_argument(
                                        "WEFT_UNSUPPORTED_FLEX: use 1 or explicit grow shrink basis");
                                }
                                shrink = number(b);
                                basis = c;
                            } else {
                                basis = "0%";
                            }
                        }
                        YGNodeStyleSetFlexGrow(y, grow);
                        YGNodeStyleSetFlexShrink(y, shrink);
                        const auto l = length(basis);
                        if (grow < 0 || shrink < 0 || (!l.automatic && l.value < 0)) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative flex shorthand");
                        }
                        if (l.automatic) {
                            YGNodeStyleSetFlexBasisAuto(y);
                        } else if (l.percent) {
                            YGNodeStyleSetFlexBasisPercent(y, l.value);
                        } else {
                            YGNodeStyleSetFlexBasis(y, l.value);
                        }
                    } else if (key == "align-items") {
                        if (v == "auto" || v.starts_with("space-")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-items " + v);
                        }
                        YGNodeStyleSetAlignItems(y, align(v));
                    } else if (key == "align-content") {
                        if (v == "auto" || v == "baseline") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-content " + v);
                        }
                        YGNodeStyleSetAlignContent(y, align(v));
                    } else if (key == "align-self") {
                        if (v.starts_with("space-")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-self " + v);
                        }
                        YGNodeStyleSetAlignSelf(y, align(v));
                    } else if (key == "justify-content") {
                        YGJustify j;
                        if (v == "flex-start") {
                            j = YGJustifyFlexStart;
                        } else if (v == "flex-end") {
                            j = YGJustifyFlexEnd;
                        } else if (v == "center") {
                            j = YGJustifyCenter;
                        } else if (v == "space-between") {
                            j = YGJustifySpaceBetween;
                        } else if (v == "space-around") {
                            j = YGJustifySpaceAround;
                        } else if (v == "space-evenly") {
                            j = YGJustifySpaceEvenly;
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_JUSTIFY: " + v);
                        }
                        YGNodeStyleSetJustifyContent(y, j);
                    } else if (key == "position") {
                        if (v == "absolute") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeAbsolute);
                        } else if (v == "relative") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeRelative);
                        } else if (v == "static") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeStatic);
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_POSITION: " + v);
                        }
                    } else if (key == "overflow") {
                        if (v == "visible") {
                            YGNodeStyleSetOverflow(y, YGOverflowVisible);
                        } else if (v == "hidden") {
                            YGNodeStyleSetOverflow(y, YGOverflowHidden);
                        } else if (v == "scroll" || v == "auto") {
                            YGNodeStyleSetOverflow(y, YGOverflowScroll);
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_OVERFLOW: " + v);
                        }
                    } else if (key == "box-sizing") {
                        if (v != "border-box" && v != "content-box") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_BOX_SIZING: " + v);
                        }
                        YGNodeStyleSetBoxSizing(y, v == "border-box" ? YGBoxSizingBorderBox : YGBoxSizingContentBox);
                    } else if (key == "gap" || key == "row-gap" || key == "column-gap") {
                        auto l = length(v);
                        if (l.automatic) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_GAP");
                        }
                        const auto gutter = key == "gap"       ? YGGutterAll
                                            : key == "row-gap" ? YGGutterRow
                                                               : YGGutterColumn;
                        if (l.percent) {
                            YGNodeStyleSetGapPercent(y, gutter, l.value);
                        } else {
                            YGNodeStyleSetGap(y, gutter, l.value);
                        }
                    } else if (key == "padding" || key == "margin" || key == "border-width" ||
                               key.starts_with("padding-") || key.starts_with("margin-") ||
                               key.starts_with("border-") || key == "top" || key == "left" || key == "right" ||
                               key == "bottom") {
                        YGEdge edge = YGEdgeAll;
                        if (key.find("left") != std::string::npos || key == "left") {
                            edge = YGEdgeLeft;
                        } else if (key.find("right") != std::string::npos || key == "right") {
                            edge = YGEdgeRight;
                        } else if (key.find("top") != std::string::npos || key == "top") {
                            edge = YGEdgeTop;
                        } else if (key.find("bottom") != std::string::npos || key == "bottom") {
                            edge = YGEdgeBottom;
                        }
                        auto l = length(v);
                        if (key.starts_with("margin")) {
                            if (l.automatic) {
                                YGNodeStyleSetMarginAuto(y, edge);
                            } else if (l.percent) {
                                YGNodeStyleSetMarginPercent(y, edge, l.value);
                            } else {
                                YGNodeStyleSetMargin(y, edge, l.value);
                            }
                        } else if (key.starts_with("padding")) {
                            if (l.automatic) {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_PADDING");
                            }
                            if (l.percent) {
                                YGNodeStyleSetPaddingPercent(y, edge, l.value);
                            } else {
                                YGNodeStyleSetPadding(y, edge, l.value);
                            }
                        } else if (key.starts_with("border")) {
                            if (l.automatic || l.percent) {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_BORDER");
                            }
                            YGNodeStyleSetBorder(y, edge, l.value);
                        } else {
                            if (l.automatic) {
                                YGNodeStyleSetPositionAuto(y, edge);
                            } else if (l.percent) {
                                YGNodeStyleSetPositionPercent(y, edge, l.value);
                            } else {
                                YGNodeStyleSetPosition(y, edge, l.value);
                            }
                        }
                    } else if (key == "font-size" || key == "line-height" || key == "white-space" ||
                               (v == "auto" && (key == "min-width" || key == "min-height"))) {
                        if (key == "white-space" && v != "normal" && v != "nowrap" &&
                            !(n.input->inline_content && v == "pre-line")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_WHITE_SPACE");
                        }
                    } else {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_PROPERTY: " + key);
                    }
                }
                if (mode == FlexMode::RestrictedBlockAdapter && value(s, "display") == "block") {
                    YGNodeStyleSetFlexDirection(y, YGFlexDirectionColumn);
                    YGNodeStyleSetAlignItems(y, YGAlignStretch);
                    YGNodeStyleSetJustifyContent(y, YGJustifyFlexStart);
                }
            }

            std::unique_ptr<Node> build(const FlexNode& input, const Node* parent, std::size_t depth = 0) {
                if (++node_count > constants::max_document_nodes || depth > constants::max_document_depth) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe tree bounds");
                }
                if (input.text.size() > constants::max_text_bytes) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe text exceeds 64 KiB");
                }
                if ((mode == FlexMode::RestrictedBlockAdapter || fonts) &&
                    (text_bytes += input.text.size()) > constants::layout::max_aggregate_text_bytes) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: aggregate text bytes");
                }
                if (input.id.empty() || !identities.insert(input.id).second) {
                    throw std::invalid_argument("WEFT_LAYOUT_ID: empty or duplicate");
                }
                if (input.inline_content) {
                    for (const auto& r : input.inline_content->runs) {
                        if ((text_bytes += r.text.size()) > constants::layout::max_aggregate_text_bytes) {
                            throw std::invalid_argument("WEFT_LAYOUT_LIMIT: aggregate inline bytes");
                        }
                    }
                }
                if (!input.children.empty() && (!input.text.empty() || input.inline_content)) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: mixed text/flex children");
                }
                if ((!input.children.empty() || !parent) && value(input.style, "display") != "flex" &&
                    value(input.style, "display") != "inline-flex" &&
                    !(mode == FlexMode::RestrictedBlockAdapter &&
                      (value(input.style, "display") == "block" || value(input.style, "display") == "none"))) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: container must explicitly be flex");
                }
                auto n = std::make_unique<Node>();
                n->engine = this;
                n->input = &input;
                n->font_size = px(input.style, "font-size",
                                  parent ? parent->font_size : constants::layout::flex_default_font_size_px);
                n->line_height = px(input.style, "line-height",
                                    parent ? parent->line_height : constants::layout::flex_default_line_height_px);
                n->yoga = YGNodeNewWithConfig(config);
                if (!n->yoga) {
                    throw std::bad_alloc();
                }
                YGNodeSetContext(n->yoga, n.get());
                apply(*n);
                // In the explicitly bounded zero-margin block subset, column packing
                // is equivalent only when normal-flow children never flex/shrink.
                if (mode == FlexMode::RestrictedBlockAdapter && parent &&
                    value(parent->input->style, "display") == "block") {
                    YGNodeStyleSetFlexGrow(n->yoga, 0);
                    YGNodeStyleSetFlexShrink(n->yoga, 0);
                    YGNodeStyleSetFlexBasisAuto(n->yoga);
                    YGNodeStyleSetAlignSelf(n->yoga, YGAlignStretch);
                }
                const float edges = horizontal(input.style);
                if (!input.text.empty() || input.inline_content) {
                    const auto natural = text(*n);
                    n->max_width = natural.width + edges;
                    n->intrinsic_height = natural.height + vertical(input.style);
                    std::istringstream tokens(input.text);
                    std::string word;
                    while (tokens >> word) {
                        n->min_width = std::max(n->min_width, fonts ? fonts->shape(word, n->font_size).shaped.advance
                                                                    : font.shape(word, n->font_size).advance);
                    }
                    if (value(input.style, "white-space", "normal") == "nowrap") {
                        n->min_width = natural.width;
                    }
                    if (input.inline_content) {
                        n->min_width =
                            layout_inline(*input.inline_content, std::numeric_limits<float>::infinity(), *fonts)
                                .min_content;
                    }
                    n->min_width += edges;
                    YGNodeSetMeasureFunc(n->yoga, measure);
                    YGNodeSetBaselineFunc(n->yoga, baseline);
                } else {
                    std::size_t flow_count{};
                    for (const auto& child : input.children) {
                        auto c = build(child, n.get(), depth + 1);
                        YGNodeInsertChild(n->yoga, c->yoga, static_cast<std::uint32_t>(n->children.size()));
                        if (value(child.style, "position") != "absolute" && value(child.style, "display") != "none") {
                            ++flow_count;
                            if (row(input.style) && value(input.style, "flex-wrap", "nowrap") == "nowrap") {
                                n->min_width += c->min_width;
                            } else {
                                n->min_width = std::max(n->min_width, c->min_width);
                            }
                            if (row(input.style)) {
                                n->max_width += c->max_width;
                                n->intrinsic_height = std::max(n->intrinsic_height, c->intrinsic_height);
                            } else {
                                n->max_width = std::max(n->max_width, c->max_width);
                                n->intrinsic_height += c->intrinsic_height;
                            }
                        }
                        n->children.push_back(std::move(c));
                    }
                    if (flow_count > 1) {
                        const float gap =
                            px(input.style, row(input.style) ? "column-gap" : "row-gap", px(input.style, "gap"));
                        if (row(input.style)) {
                            n->max_width += (flow_count - 1) * gap;
                            if (value(input.style, "flex-wrap", "nowrap") == "nowrap") {
                                n->min_width += (flow_count - 1) * gap;
                            }
                        } else {
                            n->intrinsic_height += (flow_count - 1) * gap;
                        }
                    }
                    n->min_width += edges;
                    n->max_width += edges;
                    n->intrinsic_height += vertical(input.style);
                }
                const auto specified = length(value(input.style, "width"));
                if (!specified.automatic && !specified.percent) {
                    const float border = specified.value +
                                         (value(input.style, "box-sizing", "content-box") == "content-box" ? edges : 0);
                    n->min_width = std::min(n->min_width, border);
                    n->max_width = std::max(n->min_width, border);
                }
                const auto specified_height = length(value(input.style, "height"));
                n->auto_min_height = n->intrinsic_height;
                if (!specified_height.automatic && !specified_height.percent) {
                    n->intrinsic_height =
                        specified_height.value +
                        (value(input.style, "box-sizing", "content-box") == "content-box" ? vertical(input.style) : 0);
                }
                if (mode != FlexMode::RawYoga && parent && value(parent->input->style, "display") != "block" &&
                    value(input.style, "position") != "absolute" &&
                    value(input.style, "overflow", "visible") == "visible") {
                    const bool main_row = row(parent->input->style);
                    const auto key = main_row ? "min-width" : "min-height";
                    if (value(input.style, key, "auto") == "auto") {
                        float minimum = main_row ? n->min_width : n->auto_min_height;
                        // A nowrap line has width-independent intrinsic height. Only
                        // wrapping text requires a definite cross-axis constraint.
                        if (!main_row && (!input.text.empty() || input.inline_content) &&
                            value(input.style, "white-space", "normal") != "nowrap") {
                            float content_width;
                            if (!specified.automatic && !specified.percent) {
                                content_width =
                                    specified.value -
                                    (value(input.style, "box-sizing", "content-box") == "border-box" ? edges : 0);
                            } else {
                                const auto parent_width = length(value(parent->input->style, "width"));
                                if (parent_width.automatic || parent_width.percent) {
                                    throw std::invalid_argument(
                                        "WEFT_UNSUPPORTED_INTRINSIC: auto min-height needs definite cross constraint in M0");
                                }
                                content_width = parent_width.value - (value(parent->input->style, "box-sizing",
                                                                            "content-box") == "border-box"
                                                                          ? horizontal(parent->input->style)
                                                                          : 0);
                                if (specified.percent) {
                                    content_width *= specified.value / constants::percent_scale;
                                }
                                content_width -= edges;
                            }
                            const auto max_width = length(value(input.style, "max-width"));
                            if (!max_width.automatic && !max_width.percent) {
                                content_width = std::min(
                                    content_width,
                                    max_width.value -
                                        (value(input.style, "box-sizing", "content-box") == "border-box" ? edges : 0));
                            }
                            minimum = text(*n, std::max(0.0f, content_width)).height + vertical(input.style);
                        }
                        if (!main_row && !specified_height.automatic && !specified_height.percent) {
                            minimum = std::min(minimum, n->intrinsic_height);
                        }
                        const auto maximum = length(value(input.style, main_row ? "max-width" : "max-height"));
                        if (!maximum.automatic && !maximum.percent) {
                            const float max_border =
                                maximum.value + (value(input.style, "box-sizing", "content-box") == "content-box"
                                                     ? (main_row ? edges : vertical(input.style))
                                                     : 0);
                            minimum = std::min(minimum, max_border);
                        }
                        // Yoga accepts a content-box min size when configured content-box.
                        if (value(input.style, "box-sizing", "content-box") == "content-box") {
                            minimum -= main_row ? edges : vertical(input.style);
                        }
                        minimum = std::max(0.0f, minimum);
                        if (main_row) {
                            YGNodeStyleSetMinWidth(n->yoga, minimum);
                        } else {
                            YGNodeStyleSetMinHeight(n->yoga, minimum);
                        }
                    }
                }
                return n;
            }

            void collect(Node& n, float x, float y, std::vector<FlexBox>& output) {
                if (mode == FlexMode::RestrictedBlockAdapter && value(n.input->style, "display") == "none") {
                    output.push_back({n.input->id});
                    for (auto& c : n.children) {
                        collect(*c, 0, 0, output);
                    }
                    return;
                }
                x += YGNodeLayoutGetLeft(n.yoga);
                y += YGNodeLayoutGetTop(n.yoga);
                const float width = YGNodeLayoutGetWidth(n.yoga);
                const float height = YGNodeLayoutGetHeight(n.yoga);
                const float content_width = std::max(0.0f, width - YGNodeLayoutGetPadding(n.yoga, YGEdgeLeft) -
                                                               YGNodeLayoutGetPadding(n.yoga, YGEdgeRight) -
                                                               YGNodeLayoutGetBorder(n.yoga, YGEdgeLeft) -
                                                               YGNodeLayoutGetBorder(n.yoga, YGEdgeRight));
                // Final text is independently laid out even when Yoga skipped measure.
                const auto final = text(n, content_width);
                output.push_back({n.input->id, x, y, width, height, final.width, final.height, final.baseline,
                                  final.lines, n.measure_calls});
                for (auto& child : n.children) {
                    collect(*child, x, y, output);
                }
            }
        };
    } // namespace

    std::vector<FlexBox> run_flex(const FlexNode& root, Font& font, FlexMode mode) {
        Engine engine(font, mode);
        auto tree = engine.build(root, nullptr);
        YGNodeCalculateLayout(tree->yoga, YGUndefined, YGUndefined, YGDirectionLTR);
        if (engine.callback_error) {
            std::rethrow_exception(engine.callback_error);
        }
        std::vector<FlexBox> output;
        output.reserve(engine.node_count);
        engine.collect(*tree, 0, 0, output);
        return output;
    }

    std::vector<FlexBox> run_flex(const FlexNode& root, FontSet& fonts, FlexMode mode) {
        Engine engine(fonts.primary(), mode, &fonts);
        auto tree = engine.build(root, nullptr);
        YGNodeCalculateLayout(tree->yoga, YGUndefined, YGUndefined, YGDirectionLTR);
        if (engine.callback_error) {
            std::rethrow_exception(engine.callback_error);
        }
        std::vector<FlexBox> out;
        out.reserve(engine.node_count);
        engine.collect(*tree, 0, 0, out);
        return out;
    }

    std::vector<TextLine> layout_text(std::string_view input, float size, float available, bool nowrap, Font& font,
                                      FontSet* fonts) {
        if (input.size() > constants::max_text_bytes) {
            throw std::invalid_argument("WEFT_LAYOUT_LIMIT: text bytes");
        }
        if (!nowrap) {
            for (unsigned char c : input) {
                if (c >= constants::layout::codepoint_delete || (c < constants::layout::codepoint_space && c != ' ')) {
                    throw std::invalid_argument(
                        "WEFT_UNSUPPORTED_TEXT_BREAK: non-ASCII needs nowrap until Unicode line breaking");
                }
            }
        }
        std::size_t work{};
        auto shape = [&](const std::string& s) {
            if ((work += s.size()) > constants::layout::max_line_measure_work_bytes) {
                throw std::invalid_argument("WEFT_LAYOUT_LIMIT: line measurement work");
            }
            auto shaped = fonts ? fonts->shape(s, size).shaped : font.shape(s, size);
            if (shaped.missing_glyph) {
                throw std::invalid_argument("WEFT_LAYOUT_FONT: missing glyph");
            }
            return shaped;
        };
        std::vector<TextLine> out;
        if (input.empty()) {
            return out;
        }
        if (nowrap) {
            out.push_back({std::string(input), shape(std::string(input))});
            return out;
        }
        std::istringstream words{std::string(input)};
        std::string word;
        std::string line;
        while (words >> word) {
            auto candidate = line.empty() ? word : line + ' ' + word;
            auto shaped = shape(candidate);
            if (!line.empty() && shaped.advance > available + constants::layout::line_fit_epsilon) {
                out.push_back({line, shape(line)});
                line = word;
            } else {
                line = std::move(candidate);
            }
        }
        if (!line.empty()) {
            out.push_back({line, shape(line)});
        }
        return out;
    }
} // namespace weft
