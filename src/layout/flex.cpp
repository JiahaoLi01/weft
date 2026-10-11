#include "layout/flex.hpp"
#include "base/css_scalar.hpp"
#include "constants/layout.hpp"
#include <yoga/Yoga.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = StyleValues;
        using P = PropertyId;

        float number(std::string_view input) {
            return css::parse_number<float>(std::string(input),
                                            static_cast<float>(constants::layout::max_flex_number_magnitude))
                .value();
        }

        // A parsed number within the adapter's bound; nullopt sends the value through its text, which reports it.
        std::optional<float> bounded(const Dimension& d) {
            const auto f = static_cast<float>(d.value);
            if (!std::isfinite(f) || std::abs(d.value) > constants::layout::max_flex_number_magnitude) {
                return std::nullopt;
            }
            return f;
        }

        float number(const CssValue& v) {
            if (const auto* d = v.dimension(); d && d->unit == Unit::None) {
                if (const auto f = bounded(*d)) {
                    return *f;
                }
            }
            return number(v.text());
        }

        struct Length {
            float value{};
            bool percent{};
            bool automatic{};
        };

        Length length(std::string_view input) {
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
            throw std::invalid_argument("WEFT_UNSUPPORTED_UNIT: " + std::string(input));
        }

        // An absent value is auto. px and % dimensions are read from the parsed value; any other text takes the
        // text path above, which accepts or reports it.
        Length length(const CssValue* v) {
            if (!v) {
                return {0, false, true};
            }
            if (const auto* d = v->dimension(); d && (d->unit == Unit::Px || d->unit == Unit::Percent)) {
                if (const auto f = bounded(*d)) {
                    return {*f, d->unit == Unit::Percent};
                }
            }
            return length(v->text());
        }

        float px(const Style& style, PropertyId key, float fallback = 0) {
            const auto* v = style.find(key);
            if (!v) {
                return fallback;
            }
            const auto n = length(v);
            if (n.automatic || n.percent) {
                throw std::invalid_argument("WEFT_UNSUPPORTED_INTRINSIC: " + std::string(property_name(key)) +
                                            " requires px in probe intrinsic sizing");
            }
            return n.value;
        }

        bool row(const Style& style) {
            return style.text(P::Display) != "block" && style.text(P::FlexDirection, "row").starts_with("row");
        }

        YGAlign align(std::string_view v) {
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
            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: " + std::string(v));
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
                return px(s, P::PaddingLeft, px(s, P::Padding)) + px(s, P::PaddingRight, px(s, P::Padding)) +
                       px(s, P::BorderLeftWidth, px(s, P::BorderWidth)) +
                       px(s, P::BorderRightWidth, px(s, P::BorderWidth));
            }

            float vertical(const Style& s) const {
                return px(s, P::PaddingTop, px(s, P::Padding)) + px(s, P::PaddingBottom, px(s, P::Padding)) +
                       px(s, P::BorderTopWidth, px(s, P::BorderWidth)) +
                       px(s, P::BorderBottomWidth, px(s, P::BorderWidth));
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
                                    n.input->style.text(P::WhiteSpace, "normal") == "nowrap", font, fonts);
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
                const bool nowrap = n.input->style.text(P::WhiteSpace, "normal") == "nowrap";
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
                           px(n.input->style, P::PaddingTop, px(n.input->style, P::Padding)) +
                           px(n.input->style, P::BorderTopWidth, px(n.input->style, P::BorderWidth));
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return 0;
                }
            }

            // The properties the adapter maps, or reads for text.
            static constexpr std::array supported_properties{P::AlignContent,
                                                             P::AlignItems,
                                                             P::AlignSelf,
                                                             P::BorderBottomWidth,
                                                             P::BorderLeftWidth,
                                                             P::BorderRightWidth,
                                                             P::BorderTopWidth,
                                                             P::BorderWidth,
                                                             P::Bottom,
                                                             P::BoxSizing,
                                                             P::ColumnGap,
                                                             P::Display,
                                                             P::Flex,
                                                             P::FlexBasis,
                                                             P::FlexDirection,
                                                             P::FlexGrow,
                                                             P::FlexShrink,
                                                             P::FlexWrap,
                                                             P::FontSize,
                                                             P::Gap,
                                                             P::Height,
                                                             P::JustifyContent,
                                                             P::Left,
                                                             P::LineHeight,
                                                             P::Margin,
                                                             P::MarginBottom,
                                                             P::MarginLeft,
                                                             P::MarginRight,
                                                             P::MarginTop,
                                                             P::MaxHeight,
                                                             P::MaxWidth,
                                                             P::MinHeight,
                                                             P::MinWidth,
                                                             P::Overflow,
                                                             P::Padding,
                                                             P::PaddingBottom,
                                                             P::PaddingLeft,
                                                             P::PaddingRight,
                                                             P::PaddingTop,
                                                             P::Position,
                                                             P::Right,
                                                             P::RowGap,
                                                             P::Top,
                                                             P::WhiteSpace,
                                                             P::Width};

            void apply(Node& n) {
                static const auto supported = [] {
                    std::array<bool, property_count> out{};
                    for (const auto id : supported_properties) {
                        out[std::size_t(id)] = true;
                    }
                    return out;
                }();
                auto y = n.yoga;
                const auto& s = n.input->style;
                if (s.size() > supported_properties.size()) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: excessive styles");
                }
                if ((s.contains(P::Flex) &&
                     (s.contains(P::FlexGrow) || s.contains(P::FlexShrink) || s.contains(P::FlexBasis))) ||
                    (s.contains(P::Gap) && (s.contains(P::RowGap) || s.contains(P::ColumnGap)))) {
                    throw std::invalid_argument(
                        "WEFT_UNSUPPORTED_STYLE_CONFLICT: resolve declaration order before adapter");
                }
                for (const auto shorthand : {P::Padding, P::Margin, P::BorderWidth}) {
                    if (!s.contains(shorthand)) {
                        continue;
                    }
                    const std::string prefix =
                        shorthand == P::BorderWidth ? "border-" : std::string(property_name(shorthand)) + '-';
                    for (const auto& entry : s) {
                        if (entry.first != shorthand && property_name(entry.first).starts_with(prefix)) {
                            throw std::invalid_argument(
                                "WEFT_UNSUPPORTED_STYLE_CONFLICT: resolve edge declarations before adapter");
                        }
                    }
                }
                for (const auto& [id, v] : s) {
                    const auto key = property_name(id);
                    if (!supported[std::size_t(id)]) {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_PROPERTY: " + std::string(key));
                    }
                    if (v.text().size() > constants::layout::max_style_value_bytes) {
                        throw std::invalid_argument("WEFT_LAYOUT_LIMIT: excessive style value");
                    }
                    if (id == P::FlexGrow || id == P::FlexShrink) {
                        if (number(v) < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative flex factor");
                        }
                    } else if (key.ends_with("width") || key.ends_with("height") || key.starts_with("padding") ||
                               key.ends_with("gap") || id == P::FontSize || id == P::FlexBasis) {
                        const auto l = length(&v);
                        if (!l.automatic && l.value < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative dimension");
                        }
                    }
                }
                if (mode != FlexMode::RawYoga) {
                    YGNodeStyleSetPositionType(y, YGPositionTypeStatic);
                    YGNodeStyleSetBoxSizing(y, YGBoxSizingContentBox);
                }
                for (const auto& [id, value] : s) {
                    const auto key = property_name(id);
                    const auto& v = value.text();
                    const auto dim = [&](auto points, auto percent, void (*automatic)(YGNodeRef) = nullptr) {
                        auto l = length(&value);
                        if (l.automatic) {
                            if (automatic) {
                                automatic(y);
                            } else {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_AUTO: " + std::string(key));
                            }
                        } else if (l.percent) {
                            percent(y, l.value);
                        } else {
                            points(y, l.value);
                        }
                    };
                    if (id == P::Display) {
                        if (v != "flex" && v != "inline-flex" && v != "none" &&
                            !(mode == FlexMode::RestrictedBlockAdapter && v == "block")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: " + v);
                        }
                        YGNodeStyleSetDisplay(y, v == "none" ? YGDisplayNone : YGDisplayFlex);
                    } else if (id == P::Width) {
                        dim(YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto);
                    } else if (id == P::Height) {
                        dim(YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto);
                    } else if (id == P::MinWidth && v != "auto") {
                        dim(YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent);
                    } else if (id == P::MinHeight && v != "auto") {
                        dim(YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent);
                    } else if (id == P::MaxWidth) {
                        dim(YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent);
                    } else if (id == P::MaxHeight) {
                        dim(YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent);
                    } else if (id == P::FlexDirection) {
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
                    } else if (id == P::FlexWrap) {
                        if (v != "nowrap" && v != "wrap" && v != "wrap-reverse") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_WRAP: " + v);
                        }
                        YGNodeStyleSetFlexWrap(y, v == "nowrap" ? YGWrapNoWrap
                                                  : v == "wrap" ? YGWrapWrap
                                                                : YGWrapWrapReverse);
                    } else if (id == P::FlexGrow) {
                        YGNodeStyleSetFlexGrow(y, number(value));
                    } else if (id == P::FlexShrink) {
                        YGNodeStyleSetFlexShrink(y, number(value));
                    } else if (id == P::FlexBasis) {
                        dim(YGNodeStyleSetFlexBasis, YGNodeStyleSetFlexBasisPercent, YGNodeStyleSetFlexBasisAuto);
                    } else if (id == P::Flex) {
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
                    } else if (id == P::AlignItems) {
                        if (v == "auto" || v.starts_with("space-")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-items " + v);
                        }
                        YGNodeStyleSetAlignItems(y, align(v));
                    } else if (id == P::AlignContent) {
                        if (v == "auto" || v == "baseline") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-content " + v);
                        }
                        YGNodeStyleSetAlignContent(y, align(v));
                    } else if (id == P::AlignSelf) {
                        if (v.starts_with("space-")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: align-self " + v);
                        }
                        YGNodeStyleSetAlignSelf(y, align(v));
                    } else if (id == P::JustifyContent) {
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
                    } else if (id == P::Position) {
                        if (v == "absolute") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeAbsolute);
                        } else if (v == "relative") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeRelative);
                        } else if (v == "static") {
                            YGNodeStyleSetPositionType(y, YGPositionTypeStatic);
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_POSITION: " + v);
                        }
                    } else if (id == P::Overflow) {
                        if (v == "visible") {
                            YGNodeStyleSetOverflow(y, YGOverflowVisible);
                        } else if (v == "hidden") {
                            YGNodeStyleSetOverflow(y, YGOverflowHidden);
                        } else if (v == "scroll" || v == "auto") {
                            YGNodeStyleSetOverflow(y, YGOverflowScroll);
                        } else {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_OVERFLOW: " + v);
                        }
                    } else if (id == P::BoxSizing) {
                        if (v != "border-box" && v != "content-box") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_BOX_SIZING: " + v);
                        }
                        YGNodeStyleSetBoxSizing(y, v == "border-box" ? YGBoxSizingBorderBox : YGBoxSizingContentBox);
                    } else if (id == P::Gap || id == P::RowGap || id == P::ColumnGap) {
                        auto l = length(&value);
                        if (l.automatic) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_GAP");
                        }
                        const auto gutter = id == P::Gap ? YGGutterAll : id == P::RowGap ? YGGutterRow : YGGutterColumn;
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
                        if (key.find("left") != key.npos) {
                            edge = YGEdgeLeft;
                        } else if (key.find("right") != key.npos) {
                            edge = YGEdgeRight;
                        } else if (key.find("top") != key.npos) {
                            edge = YGEdgeTop;
                        } else if (key.find("bottom") != key.npos) {
                            edge = YGEdgeBottom;
                        }
                        auto l = length(&value);
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
                    } else if (id == P::FontSize || id == P::LineHeight || id == P::WhiteSpace ||
                               (v == "auto" && (id == P::MinWidth || id == P::MinHeight))) {
                        if (id == P::WhiteSpace && v != "normal" && v != "nowrap" &&
                            !(n.input->inline_content && v == "pre-line")) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_WHITE_SPACE");
                        }
                    } else {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_PROPERTY: " + std::string(key));
                    }
                }
                if (mode == FlexMode::RestrictedBlockAdapter && s.text(P::Display) == "block") {
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
                const auto display = input.style.text(P::Display);
                if ((!input.children.empty() || !parent) && display != "flex" && display != "inline-flex" &&
                    !(mode == FlexMode::RestrictedBlockAdapter && (display == "block" || display == "none"))) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: container must explicitly be flex");
                }
                auto n = std::make_unique<Node>();
                n->engine = this;
                n->input = &input;
                n->font_size = px(input.style, P::FontSize,
                                  parent ? parent->font_size : constants::layout::flex_default_font_size_px);
                n->line_height = px(input.style, P::LineHeight,
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
                    parent->input->style.text(P::Display) == "block") {
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
                    if (input.style.text(P::WhiteSpace, "normal") == "nowrap") {
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
                        if (child.style.text(P::Position) != "absolute" && child.style.text(P::Display) != "none") {
                            ++flow_count;
                            if (row(input.style) && input.style.text(P::FlexWrap, "nowrap") == "nowrap") {
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
                            px(input.style, row(input.style) ? P::ColumnGap : P::RowGap, px(input.style, P::Gap));
                        if (row(input.style)) {
                            n->max_width += (flow_count - 1) * gap;
                            if (input.style.text(P::FlexWrap, "nowrap") == "nowrap") {
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
                const bool content_box = input.style.text(P::BoxSizing, "content-box") == "content-box";
                const auto specified = length(input.style.find(P::Width));
                if (!specified.automatic && !specified.percent) {
                    const float border = specified.value + (content_box ? edges : 0);
                    n->min_width = std::min(n->min_width, border);
                    n->max_width = std::max(n->min_width, border);
                }
                const auto specified_height = length(input.style.find(P::Height));
                n->auto_min_height = n->intrinsic_height;
                if (!specified_height.automatic && !specified_height.percent) {
                    n->intrinsic_height = specified_height.value + (content_box ? vertical(input.style) : 0);
                }
                if (mode != FlexMode::RawYoga && parent && parent->input->style.text(P::Display) != "block" &&
                    input.style.text(P::Position) != "absolute" &&
                    input.style.text(P::Overflow, "visible") == "visible") {
                    const bool main_row = row(parent->input->style);
                    if (input.style.text(main_row ? P::MinWidth : P::MinHeight, "auto") == "auto") {
                        float minimum = main_row ? n->min_width : n->auto_min_height;
                        // A nowrap line has width-independent intrinsic height. Only
                        // wrapping text requires a definite cross-axis constraint.
                        if (!main_row && (!input.text.empty() || input.inline_content) &&
                            input.style.text(P::WhiteSpace, "normal") != "nowrap") {
                            float content_width;
                            if (!specified.automatic && !specified.percent) {
                                content_width =
                                    specified.value -
                                    (input.style.text(P::BoxSizing, "content-box") == "border-box" ? edges : 0);
                            } else {
                                const auto parent_width = length(parent->input->style.find(P::Width));
                                if (parent_width.automatic || parent_width.percent) {
                                    throw std::invalid_argument(
                                        "WEFT_UNSUPPORTED_INTRINSIC: auto min-height needs definite cross constraint in M0");
                                }
                                content_width = parent_width.value -
                                                (parent->input->style.text(P::BoxSizing, "content-box") == "border-box"
                                                     ? horizontal(parent->input->style)
                                                     : 0);
                                if (specified.percent) {
                                    content_width *= specified.value / constants::percent_scale;
                                }
                                content_width -= edges;
                            }
                            const auto max_width = length(input.style.find(P::MaxWidth));
                            if (!max_width.automatic && !max_width.percent) {
                                content_width = std::min(
                                    content_width,
                                    max_width.value -
                                        (input.style.text(P::BoxSizing, "content-box") == "border-box" ? edges : 0));
                            }
                            minimum = text(*n, std::max(0.0f, content_width)).height + vertical(input.style);
                        }
                        if (!main_row && !specified_height.automatic && !specified_height.percent) {
                            minimum = std::min(minimum, n->intrinsic_height);
                        }
                        const auto maximum = length(input.style.find(main_row ? P::MaxWidth : P::MaxHeight));
                        if (!maximum.automatic && !maximum.percent) {
                            const float max_border =
                                maximum.value + (content_box ? (main_row ? edges : vertical(input.style)) : 0);
                            minimum = std::min(minimum, max_border);
                        }
                        // Yoga accepts a content-box min size when configured content-box.
                        if (content_box) {
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
                if (mode == FlexMode::RestrictedBlockAdapter && n.input->style.text(P::Display) == "none") {
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
