#include "layout/flex.hpp"
#include "base/css_scalar.hpp"
#include "constants/layout.hpp"
#include <yoga/Yoga.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = StyleValues;
        using P = PropertyId;
        constexpr float unbounded = std::numeric_limits<float>::infinity();
        constexpr std::size_t none = LayoutBox::root;

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

        // calc() of a percentage and a px length, either first, added or subtracted ("calc(100% + 1.6px)"): the
        // percentage and the length, signed. WEFT_UNSUPPORTED_UNIT for any other calc().
        std::array<float, 2> percent_and_length(std::string_view input) {
            const auto unsupported = [&] {
                return std::invalid_argument("WEFT_UNSUPPORTED_UNIT: " + std::string(input));
            };
            if (!input.starts_with("calc(") || !input.ends_with(')')) {
                throw unsupported();
            }
            const auto body = css::trim(input.substr(5, input.size() - 6));
            // CSS requires whitespace around the operator.
            auto at = body.find(" + ");
            float sign = 1;
            if (at == body.npos) {
                at = body.find(" - ");
                sign = -1;
            }
            if (at == body.npos) {
                throw unsupported();
            }
            const auto a = length(css::trim(body.substr(0, at)));
            const auto b = length(css::trim(body.substr(at + 3)));
            if (a.automatic || b.automatic || a.percent == b.percent) {
                throw unsupported();
            }
            return a.percent ? std::array{a.value, sign * b.value} : std::array{sign * b.value, a.value};
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

        // An svg width or height attribute: a length whose unitless numbers are px.
        Length presentation(std::string_view input) {
            const auto text = css::trim(input);
            if (text.empty() || text == "auto") {
                return {0, false, true};
            }
            if (text.ends_with('%')) {
                return {number(std::string_view(text).substr(0, text.size() - 1)), true};
            }
            return {number(text.ends_with("px") ? std::string_view(text).substr(0, text.size() - 2) : text)};
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

        // px() for intrinsic sizes, which resolve no percentage or auto: those make the size unknown. `unknown`
        // keeps the first such diagnostic, raised only if an automatic minimum needs the size.
        float intrinsic_px(const Style& style, PropertyId key, float fallback, std::string& unknown) {
            const auto* v = style.find(key);
            if (!v) {
                return fallback;
            }
            const auto n = length(v);
            if (n.automatic || n.percent) {
                if (unknown.empty()) {
                    unknown = "WEFT_UNSUPPORTED_INTRINSIC: " + std::string(property_name(key)) +
                              " requires px in probe intrinsic sizing";
                }
                return 0;
            }
            return n.value;
        }

        float horizontal(const Style& s) {
            return px(s, P::PaddingLeft, px(s, P::Padding)) + px(s, P::PaddingRight, px(s, P::Padding)) +
                   px(s, P::BorderLeftWidth, px(s, P::BorderWidth)) + px(s, P::BorderRightWidth, px(s, P::BorderWidth));
        }

        float vertical(const Style& s) {
            return px(s, P::PaddingTop, px(s, P::Padding)) + px(s, P::PaddingBottom, px(s, P::Padding)) +
                   px(s, P::BorderTopWidth, px(s, P::BorderWidth)) + px(s, P::BorderBottomWidth, px(s, P::BorderWidth));
        }

        float intrinsic_horizontal(const Style& s, std::string& unknown) {
            const auto edge = [&](PropertyId key, PropertyId shorthand) {
                return intrinsic_px(s, key, intrinsic_px(s, shorthand, 0, unknown), unknown);
            };
            return edge(P::PaddingLeft, P::Padding) + edge(P::PaddingRight, P::Padding) +
                   edge(P::BorderLeftWidth, P::BorderWidth) + edge(P::BorderRightWidth, P::BorderWidth);
        }

        float intrinsic_vertical(const Style& s, std::string& unknown) {
            const auto edge = [&](PropertyId key, PropertyId shorthand) {
                return intrinsic_px(s, key, intrinsic_px(s, shorthand, 0, unknown), unknown);
            };
            return edge(P::PaddingTop, P::Padding) + edge(P::PaddingBottom, P::Padding) +
                   edge(P::BorderTopWidth, P::BorderWidth) + edge(P::BorderBottomWidth, P::BorderWidth);
        }

        bool row_direction(const Style& style) {
            return style.text(P::FlexDirection, "row").starts_with("row");
        }

        // normal is the initial value: stretch for the align properties of flex boxes.
        YGAlign align(std::string_view v) {
            if (v == "auto") {
                return YGAlignAuto;
            }
            if (v == "stretch" || v == "normal") {
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

        // What each registered property is to the box mapping.
        enum class Role : std::uint8_t {
            // Paint, stacking and interaction: no effect on boxes.
            Ignored,
            // Mapped to Yoga or read by the grid and containing block rules.
            Box,
            // Read by the formatter's text system.
            Text,
            // Affects boxes but has no mapping: a diagnostic.
            Unsupported,
        };

        constexpr auto roles = [] {
            std::array<Role, property_count> out{};
            for (const auto id : {P::AlignContent,
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
                                  P::Gap,
                                  P::GridTemplateColumns,
                                  P::Height,
                                  P::JustifyContent,
                                  P::JustifyItems,
                                  P::Left,
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
                                  P::PlaceItems,
                                  P::Position,
                                  P::Right,
                                  P::RowGap,
                                  P::Top,
                                  P::Transform,
                                  P::Width}) {
                out[std::size_t(id)] = Role::Box;
            }
            for (const auto id : {P::FontFamily, P::FontSize, P::FontStyle, P::FontWeight, P::LetterSpacing,
                                  P::LineHeight, P::TextAlign, P::WhiteSpace}) {
                out[std::size_t(id)] = Role::Text;
            }
            for (const auto id : {P::Border, P::ListStyle, P::OverflowX, P::OverflowY}) {
                out[std::size_t(id)] = Role::Unsupported;
            }
            return out;
        }();

        // How a box lays out its children.
        enum class Formatting : std::uint8_t {
            Flex,
            // block, and inline-level boxes with box children: a column whose children do not flex.
            Block,
            // A block whose children are all inline-level, approximated as one top-aligned line without breaks.
            InlineRow,
            // Equal minmax(0, 1fr) columns, rows sized to content.
            Grid,
            None,
        };

        bool inline_level(std::string_view display) {
            return display == "inline" || display == "inline-block";
        }

        struct Engine;

        struct Node {
            Engine* engine{};
            std::size_t index{};
            const LayoutBox* box{};
            const Style* style{};
            YGNodeRef yoga{};
            Formatting formatting{Formatting::Block};
            std::size_t depth{};
            std::size_t first_child{none};
            std::size_t next_sibling{none};
            std::size_t last_child{none};
            std::size_t children{};
            bool definite_height{};
            unsigned columns{};
            // Grid cell origins of the children, in child order.
            std::vector<std::array<float, 2>> positions;
            // Intrinsic border-box sizes for automatic minimum sizes, and why one is unknown.
            float min_width{};
            float max_width{};
            float intrinsic_height{};
            float auto_min_height{};
            std::string unknown_width;
            std::string unknown_height;
            std::size_t measure_calls{};
        };

        struct Engine {
            std::span<const LayoutBox> boxes;
            const TextMeasurer& text;
            LayoutOptions options;
            YGConfigRef config{YGConfigNew()};
            std::vector<Node> nodes;
            std::exception_ptr callback_error;

            Engine(std::span<const LayoutBox> b, const TextMeasurer& t, const LayoutOptions& o)
                : boxes(b), text(t), options(o) {
                if (!config) {
                    throw std::bad_alloc();
                }
                YGConfigSetUseWebDefaults(config, true);
                YGConfigSetPointScaleFactor(config, 0);
                YGConfigSetErrata(config, YGErrataNone);
            }

            Engine(const Engine&) = delete;
            Engine& operator=(const Engine&) = delete;

            ~Engine() {
                // Children before parents, so that no freed node is still attached.
                for (auto i = nodes.size(); i-- > 0;) {
                    if (nodes[i].yoga) {
                        YGNodeFree(nodes[i].yoga);
                    }
                }
                YGConfigFree(config);
            }

            bool css() const {
                return options.mode == FlexMode::CssAdapter;
            }

            template <class F>
            void each_child(const Node& n, F&& f) {
                for (auto c = n.first_child; c != none; c = nodes[c].next_sibling) {
                    f(nodes[c]);
                }
            }

            void link() {
                if (boxes.empty() || boxes[0].parent != none) {
                    throw std::invalid_argument("WEFT_LAYOUT_TREE: the first box is the only root");
                }
                if (boxes.size() > std::size_t(constants::max_document_nodes)) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe tree bounds");
                }
                nodes.resize(boxes.size());
                for (std::size_t i = 0; i < boxes.size(); ++i) {
                    auto& n = nodes[i];
                    n.engine = this;
                    n.index = i;
                    n.box = &boxes[i];
                    n.style = boxes[i].style;
                    if (!n.style) {
                        throw std::invalid_argument("WEFT_LAYOUT_TREE: box without style");
                    }
                    if (!i) {
                        continue;
                    }
                    const auto p = boxes[i].parent;
                    if (p >= i) {
                        throw std::invalid_argument("WEFT_LAYOUT_TREE: boxes out of tree order");
                    }
                    auto& parent = nodes[p];
                    n.depth = parent.depth + 1;
                    if (n.depth > std::size_t(constants::max_document_depth)) {
                        throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe tree bounds");
                    }
                    (parent.last_child == none ? parent.first_child : nodes[parent.last_child].next_sibling) = i;
                    parent.last_child = i;
                    ++parent.children;
                }
            }

            void classify() {
                for (auto& n : nodes) {
                    const auto display = n.style->text(P::Display);
                    if (n.box->text && n.children) {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: mixed text/flex children");
                    }
                    if (!css()) {
                        if ((n.children || !n.index) && display != "flex" && display != "inline-flex") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: container must explicitly be flex");
                        }
                        n.formatting = display == "none" ? Formatting::None : Formatting::Flex;
                        continue;
                    }
                    if (display == "none") {
                        n.formatting = Formatting::None;
                    } else if (display == "flex" || display == "inline-flex") {
                        n.formatting = Formatting::Flex;
                    } else if (display == "grid" && !n.box->replaced && !n.box->text) {
                        n.formatting = Formatting::Grid;
                    } else {
                        n.formatting = Formatting::Block;
                    }
                }
                for (auto& n : nodes) {
                    if (n.formatting == Formatting::Block && n.children) {
                        bool all_inline = true;
                        each_child(n, [&](const Node& c) {
                            all_inline &= inline_level(c.style->text(P::Display, "inline"));
                        });
                        if (all_inline) {
                            n.formatting = Formatting::InlineRow;
                        }
                    }
                    if (n.formatting == Formatting::Grid) {
                        const auto columns = n.style->text(P::GridTemplateColumns);
                        if (const auto repeat = css::grid_repeat(columns)) {
                            n.columns = *repeat;
                        } else {
                            const auto tracks = css::split(columns, ' ').parts;
                            if (tracks.empty() || tracks.size() > std::size_t(constants::layout::max_grid_columns) ||
                                !std::all_of(tracks.begin(), tracks.end(), [](const auto& t) {
                                    return t == "1fr";
                                })) {
                                throw std::invalid_argument(
                                    "WEFT_UNSUPPORTED_FORMAT: grid supports 1-8 equal minmax(0,1fr) tracks");
                            }
                            n.columns = unsigned(tracks.size());
                        }
                        each_child(n, [&](const Node& c) {
                            if (c.style->text(P::Position) == "absolute") {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: absolute grid child");
                            }
                        });
                        n.positions.resize(n.children);
                    }
                }
            }

            void validate(const Style& s) const {
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
                        if (entry.first != shorthand && roles[std::size_t(entry.first)] == Role::Box &&
                            property_name(entry.first).starts_with(prefix)) {
                            throw std::invalid_argument(
                                "WEFT_UNSUPPORTED_STYLE_CONFLICT: resolve edge declarations before adapter");
                        }
                    }
                }
                for (const auto& [id, v] : s) {
                    const auto role = roles[std::size_t(id)];
                    const auto key = property_name(id);
                    if (role == Role::Unsupported) {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_PROPERTY: " + std::string(key));
                    }
                    // A transform only decides whether the box is a containing block.
                    if (role != Role::Box || id == P::Transform) {
                        continue;
                    }
                    if (v.text().size() > constants::layout::max_style_value_bytes) {
                        throw std::invalid_argument("WEFT_LAYOUT_LIMIT: excessive style value");
                    }
                    if (id == P::FlexGrow || id == P::FlexShrink) {
                        if (number(v) < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative flex factor");
                        }
                    } else if (key.ends_with("width") || key.ends_with("height") || key.starts_with("padding") ||
                               key.ends_with("gap") || id == P::FlexBasis) {
                        const auto l = length(&v);
                        if (!l.automatic && l.value < 0) {
                            throw std::invalid_argument("WEFT_CSS_VALUE: negative dimension");
                        }
                    }
                }
            }

            void apply(Node& n) {
                auto y = n.yoga;
                const auto& s = *n.style;
                validate(s);
                if (css()) {
                    YGNodeStyleSetPositionType(y, YGPositionTypeStatic);
                    YGNodeStyleSetBoxSizing(y, YGBoxSizingContentBox);
                }
                // The lengths of calc() insets, added to the margins on their sides once the margins are read.
                std::vector<std::pair<YGEdge, float>> shifts;
                for (const auto& [id, value] : s) {
                    if (roles[std::size_t(id)] != Role::Box) {
                        continue;
                    }
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
                        const bool flex = v == "flex" || v == "inline-flex" || v == "none";
                        if (!flex && (!css() || (v != "block" && !inline_level(v) && v != "grid"))) {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: " + v);
                        }
                        YGNodeStyleSetDisplay(y, v == "none" ? YGDisplayNone : YGDisplayFlex);
                    } else if (id == P::Width) {
                        dim(YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto);
                    } else if (id == P::Height) {
                        dim(YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto);
                    } else if (id == P::MinWidth) {
                        if (v != "auto") {
                            dim(YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent);
                        }
                    } else if (id == P::MinHeight) {
                        if (v != "auto") {
                            dim(YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent);
                        }
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
                        if (v == "flex-start" || v == "normal") {
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
                        } else if (v == "hidden" || v == "clip") {
                            // clip as hidden: it clips and its box does not grow to its content.
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
                    } else if (id == P::Transform || id == P::GridTemplateColumns || id == P::JustifyItems ||
                               id == P::PlaceItems) {
                        // Read by the containing block and grid rules.
                    } else {
                        // Padding, margin, border widths and insets.
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
                        if (!key.starts_with("margin") && !key.starts_with("padding") && !key.starts_with("border") &&
                            v.starts_with("calc(")) {
                            // An inset of a percentage and a length: the percentage as the inset, the length on the
                            // margin of its side, which moves an absolute box's border box as the length would.
                            const auto [percent, px] = percent_and_length(v);
                            YGNodeStyleSetPositionPercent(y, edge, percent);
                            shifts.emplace_back(edge, px);
                            continue;
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
                        } else if (l.automatic) {
                            YGNodeStyleSetPositionAuto(y, edge);
                        } else if (l.percent) {
                            YGNodeStyleSetPositionPercent(y, edge, l.value);
                        } else {
                            YGNodeStyleSetPosition(y, edge, l.value);
                        }
                    }
                }
                if (!shifts.empty()) {
                    // Elsewhere a margin would move other boxes too.
                    const auto position = s.text(P::Position, "static");
                    if (position != "absolute" && position != "fixed") {
                        throw std::invalid_argument("WEFT_UNSUPPORTED_UNIT: calc() inset of a box not absolutely "
                                                    "positioned");
                    }
                    for (const auto& [edge, shift] : shifts) {
                        const auto side = edge == YGEdgeLeft    ? P::MarginLeft
                                          : edge == YGEdgeRight ? P::MarginRight
                                          : edge == YGEdgeTop   ? P::MarginTop
                                                                : P::MarginBottom;
                        const auto* margin = s.find(side);
                        if (!margin) {
                            margin = s.find(P::Margin);
                        }
                        float base{};
                        if (margin) {
                            const auto m = length(margin);
                            if (m.automatic || m.percent) {
                                throw std::invalid_argument(
                                    "WEFT_UNSUPPORTED_UNIT: calc() inset beside an auto or percentage margin");
                            }
                            base = m.value;
                        }
                        YGNodeStyleSetMargin(y, edge, base + shift);
                    }
                }
                if (!css()) {
                    return;
                }
                if (n.formatting == Formatting::Block || n.formatting == Formatting::InlineRow) {
                    const bool line = n.formatting == Formatting::InlineRow;
                    YGNodeStyleSetFlexDirection(y, line ? YGFlexDirectionRow : YGFlexDirectionColumn);
                    YGNodeStyleSetAlignItems(y, line ? YGAlignFlexStart : YGAlignStretch);
                    YGNodeStyleSetJustifyContent(y, YGJustifyFlexStart);
                }
                // A transformed box, or one about to be, contains its absolute descendants.
                if (s.text(P::Transform, "none") != "none" ||
                    s.text(P::WillChange, "auto").find("transform") != std::string_view::npos) {
                    YGNodeSetAlwaysFormsContainingBlock(y, true);
                }
            }

            // An svg's width and height attributes stand in for absent properties. With an intrinsic ratio and an
            // auto or percentage height in a parent of indefinite height, the height follows the width.
            void replace(Node& n) {
                const auto& box = *n.box;
                const auto& s = *n.style;
                for (const bool width : {true, false}) {
                    if (s.contains(width ? P::Width : P::Height)) {
                        continue;
                    }
                    const auto attribute = width ? box.replaced_width : box.replaced_height;
                    const auto l = attribute.empty() ? Length{width ? constants::layout::default_object_width_px
                                                                    : constants::layout::default_object_height_px}
                                                     : presentation(attribute);
                    if (l.automatic) {
                        continue;
                    }
                    if (l.percent) {
                        (width ? YGNodeStyleSetWidthPercent : YGNodeStyleSetHeightPercent)(n.yoga, l.value);
                    } else {
                        (width ? YGNodeStyleSetWidth : YGNodeStyleSetHeight)(n.yoga, l.value);
                    }
                }
                if (box.ratio == 0 || !n.index || nodes[box.parent].definite_height) {
                    return;
                }
                const auto height = s.contains(P::Height) ? s.text(P::Height) : box.replaced_height;
                if (!height.empty() && height != "auto" && !height.ends_with('%')) {
                    return;
                }
                if (!std::isfinite(box.ratio) || box.ratio <= 0) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: SVG intrinsic ratio");
                }
                YGNodeStyleSetHeightAuto(n.yoga);
                YGNodeStyleSetAspectRatio(n.yoga, box.ratio);
            }

            void create() {
                for (auto& n : nodes) {
                    n.yoga = YGNodeNewWithConfig(config);
                    if (!n.yoga) {
                        throw std::bad_alloc();
                    }
                    YGNodeSetContext(n.yoga, &n);
                    apply(n);
                    const auto height = n.style->text(P::Height);
                    n.definite_height = !n.index || (!height.empty() && height != "auto" &&
                                                     (!height.ends_with('%') || nodes[n.box->parent].definite_height));
                    if (n.index) {
                        const auto& parent = nodes[n.box->parent];
                        if (parent.formatting != Formatting::Grid) {
                            YGNodeInsertChild(parent.yoga, n.yoga, YGNodeGetChildCount(parent.yoga));
                        }
                        // Block and inline layout do not flex their children.
                        if (css() &&
                            (parent.formatting == Formatting::Block || parent.formatting == Formatting::InlineRow)) {
                            YGNodeStyleSetFlexGrow(n.yoga, 0);
                            YGNodeStyleSetFlexShrink(n.yoga, 0);
                            YGNodeStyleSetFlexBasisAuto(n.yoga);
                            YGNodeStyleSetAlignSelf(n.yoga, parent.formatting == Formatting::Block ? YGAlignStretch
                                                                                                   : YGAlignAuto);
                        }
                    }
                    if (n.box->replaced) {
                        replace(n);
                    }
                    if (n.box->text) {
                        YGNodeSetMeasureFunc(n.yoga, measure);
                        YGNodeSetBaselineFunc(n.yoga, baseline);
                    } else if (n.formatting == Formatting::Grid) {
                        YGNodeSetMeasureFunc(n.yoga, grid_measured);
                    }
                }
            }

            // Border-box intrinsic sizes, children first: max-content width, min-content width and the height at
            // the max-content width, as automatic minimum sizes need them.
            void intrinsic(Node& n) {
                const auto& s = *n.style;
                auto& unknown_w = n.unknown_width;
                auto& unknown_h = n.unknown_height;
                const float edges = intrinsic_horizontal(s, unknown_w);
                const float edges_v = intrinsic_vertical(s, unknown_h);
                const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                if (n.box->text) {
                    const auto natural = text.layout(n.index, unbounded);
                    n.max_width = natural.width + edges;
                    n.intrinsic_height = natural.height + edges_v;
                    n.min_width = text.min_content(n.index) + edges;
                } else if (n.box->replaced) {
                    const auto size = [&](bool width) {
                        const auto* v = s.find(width ? P::Width : P::Height);
                        const auto attribute = width ? n.box->replaced_width : n.box->replaced_height;
                        const auto l = v                   ? length(v)
                                       : attribute.empty() ? Length{width ? constants::layout::default_object_width_px
                                                                          : constants::layout::default_object_height_px}
                                                           : presentation(attribute);
                        return l;
                    };
                    const auto w = size(true);
                    const auto h = size(false);
                    if (w.automatic || w.percent) {
                        unknown_w = "WEFT_UNSUPPORTED_INTRINSIC: replaced box without a px width";
                    }
                    if (h.automatic || h.percent || YGNodeStyleGetAspectRatio(n.yoga) > 0) {
                        unknown_h = "WEFT_UNSUPPORTED_INTRINSIC: replaced box without a px height";
                    }
                    n.min_width = n.max_width = w.value + (content_box ? edges : 0);
                    n.intrinsic_height = h.value + (content_box ? edges_v : 0);
                } else if (n.formatting == Formatting::Grid) {
                    const float column_gap =
                        intrinsic_px(s, P::ColumnGap, intrinsic_px(s, P::Gap, 0, unknown_w), unknown_w);
                    const float row_gap = intrinsic_px(s, P::RowGap, intrinsic_px(s, P::Gap, 0, unknown_h), unknown_h);
                    std::vector<float> rows;
                    float widest{};
                    std::size_t placed{};
                    each_child(n, [&](const Node& c) {
                        if (c.formatting == Formatting::None) {
                            return;
                        }
                        const auto row = placed++ / n.columns;
                        rows.resize(row + 1);
                        rows[row] = std::max(rows[row], c.intrinsic_height);
                        widest = std::max(widest, c.max_width);
                        if (unknown_w.empty()) {
                            unknown_w = c.unknown_width;
                        }
                        if (unknown_h.empty()) {
                            unknown_h = c.unknown_height;
                        }
                    });
                    const float gaps = column_gap * float(n.columns - 1);
                    n.min_width = gaps + edges;
                    n.max_width = widest * float(n.columns) + gaps + edges;
                    for (const auto r : rows) {
                        n.intrinsic_height += r;
                    }
                    if (!rows.empty()) {
                        n.intrinsic_height += row_gap * float(rows.size() - 1);
                    }
                    n.intrinsic_height += edges_v;
                } else {
                    const bool line = n.formatting == Formatting::InlineRow;
                    const bool row = line || ((n.formatting == Formatting::Flex || n.formatting == Formatting::None) &&
                                              row_direction(s));
                    const bool nowrap = line || s.text(P::FlexWrap, "nowrap") == "nowrap";
                    std::size_t flow_count{};
                    each_child(n, [&](const Node& c) {
                        if (c.style->text(P::Position) == "absolute" || c.style->text(P::Display) == "none") {
                            return;
                        }
                        ++flow_count;
                        if (row && nowrap) {
                            n.min_width += c.min_width;
                        } else {
                            n.min_width = std::max(n.min_width, c.min_width);
                        }
                        if (row) {
                            n.max_width += c.max_width;
                            n.intrinsic_height = std::max(n.intrinsic_height, c.intrinsic_height);
                        } else {
                            n.max_width = std::max(n.max_width, c.max_width);
                            n.intrinsic_height += c.intrinsic_height;
                        }
                        if (unknown_w.empty()) {
                            unknown_w = c.unknown_width;
                        }
                        if (unknown_h.empty()) {
                            unknown_h = c.unknown_height;
                        }
                    });
                    if (flow_count > 1) {
                        auto& unknown = row ? unknown_w : unknown_h;
                        const float gap = intrinsic_px(s, row ? P::ColumnGap : P::RowGap,
                                                       intrinsic_px(s, P::Gap, 0, unknown), unknown);
                        if (row) {
                            n.max_width += float(flow_count - 1) * gap;
                            if (nowrap) {
                                n.min_width += float(flow_count - 1) * gap;
                            }
                        } else {
                            n.intrinsic_height += float(flow_count - 1) * gap;
                        }
                    }
                    n.min_width += edges;
                    n.max_width += edges;
                    n.intrinsic_height += edges_v;
                }
                const auto specified = length(s.find(P::Width));
                if (!specified.automatic && !specified.percent) {
                    const float border = specified.value + (content_box ? edges : 0);
                    n.min_width = std::min(n.min_width, border);
                    n.max_width = std::max(n.min_width, border);
                }
                const auto specified_height = length(s.find(P::Height));
                n.auto_min_height = n.intrinsic_height;
                if (!specified_height.automatic && !specified_height.percent) {
                    n.intrinsic_height = specified_height.value + (content_box ? edges_v : 0);
                }
            }

            // Whether a flex item's basis is auto: no flex-basis or flex-basis: auto, and no flex shorthand other
            // than none and auto.
            static bool automatic_basis(const Style& s) {
                if (const auto* flex = s.find(P::Flex)) {
                    const auto& v = flex->text();
                    return v == "none" || v == "auto" || v.ends_with(" auto");
                }
                return length(s.find(P::FlexBasis)).automatic;
            }

            // CSS automatic minimum size of a flex item in its parent's main axis: its content size, capped by a
            // definite specified or maximum size. Wrapping text in a column takes its height at its width, definite
            // from the item or its parent. When layout gives the width, an automatic basis and height make the basis
            // that same content height, the minimum too: the item does not shrink, without a second layout.
            void automatic_minimum(Node& n) {
                const auto& s = *n.style;
                const auto& parent = nodes[n.box->parent];
                if (parent.formatting != Formatting::Flex || s.text(P::Position) == "absolute" ||
                    s.text(P::Overflow, "visible") != "visible") {
                    return;
                }
                const bool main_row = row_direction(*parent.style);
                if (s.text(main_row ? P::MinWidth : P::MinHeight, "auto") != "auto") {
                    return;
                }
                const auto& unknown = main_row ? n.unknown_width : n.unknown_height;
                const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                const auto specified = length(s.find(P::Width));
                const auto specified_height = length(s.find(P::Height));
                // In a column, an automatic basis and height make the basis the content height at the item's width,
                // and that is the automatic minimum too (a box's min-content height is its height at its width): the
                // item does not shrink. The height at the max-content width would undercount a wrapping row, whose
                // lines at the item's width are more than at its max-content width.
                if (!main_row && automatic_basis(s) && specified_height.automatic) {
                    YGNodeStyleSetFlexShrink(n.yoga, 0);
                    return;
                }
                float minimum = main_row ? n.min_width : n.auto_min_height;
                // A nowrap line has width-independent intrinsic height. Only wrapping text requires a definite
                // cross-axis constraint.
                if (!main_row && n.box->text && text.wraps(n.index)) {
                    const float edges = horizontal(s);
                    float content_width;
                    if (!specified.automatic && !specified.percent) {
                        content_width = specified.value - (content_box ? 0 : edges);
                    } else {
                        const auto parent_width = length(parent.style->find(P::Width));
                        if (parent_width.automatic || parent_width.percent) {
                            if (automatic_basis(s) && specified_height.automatic) {
                                YGNodeStyleSetFlexShrink(n.yoga, 0);
                                return;
                            }
                            throw std::invalid_argument(
                                "WEFT_UNSUPPORTED_INTRINSIC: auto min-height needs definite cross constraint in M0");
                        }
                        content_width =
                            parent_width.value - (parent.style->text(P::BoxSizing, "content-box") == "border-box"
                                                      ? horizontal(*parent.style)
                                                      : 0);
                        if (specified.percent) {
                            content_width *= specified.value / constants::percent_scale;
                        }
                        content_width -= edges;
                    }
                    const auto max_width = length(s.find(P::MaxWidth));
                    if (!max_width.automatic && !max_width.percent) {
                        content_width = std::min(content_width, max_width.value - (content_box ? 0 : edges));
                    }
                    minimum = text.layout(n.index, std::max(0.0f, content_width)).height + vertical(s);
                } else if (!unknown.empty()) {
                    throw std::invalid_argument(unknown);
                }
                if (!main_row && !specified_height.automatic && !specified_height.percent) {
                    minimum = std::min(minimum, n.intrinsic_height);
                }
                const auto maximum = length(s.find(main_row ? P::MaxWidth : P::MaxHeight));
                if (!maximum.automatic && !maximum.percent) {
                    const float max_border =
                        maximum.value + (content_box ? (main_row ? horizontal(s) : vertical(s)) : 0);
                    minimum = std::min(minimum, max_border);
                }
                // Yoga accepts a content-box min size when configured content-box.
                if (content_box) {
                    minimum -= main_row ? horizontal(s) : vertical(s);
                }
                minimum = std::max(0.0f, minimum);
                if (main_row) {
                    YGNodeStyleSetMinWidth(n.yoga, minimum);
                } else {
                    YGNodeStyleSetMinHeight(n.yoga, minimum);
                }
            }

            // Places a grid's children in rows of equal columns at the given border-box width (and final height, when
            // known), and returns the border-box height.
            float grid(Node& g, float width, float height = -1) {
                const auto& s = *g.style;
                const float left =
                    px(s, P::PaddingLeft, px(s, P::Padding)) + px(s, P::BorderLeftWidth, px(s, P::BorderWidth));
                const float right =
                    px(s, P::PaddingRight, px(s, P::Padding)) + px(s, P::BorderRightWidth, px(s, P::BorderWidth));
                const float top =
                    px(s, P::PaddingTop, px(s, P::Padding)) + px(s, P::BorderTopWidth, px(s, P::BorderWidth));
                const float bottom =
                    px(s, P::PaddingBottom, px(s, P::Padding)) + px(s, P::BorderBottomWidth, px(s, P::BorderWidth));
                const float column_gap = px(s, P::ColumnGap, px(s, P::Gap));
                const float row_gap = px(s, P::RowGap, px(s, P::Gap));
                if (!std::isfinite(width) || width < left + right) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: grid needs a definite width");
                }
                const float cell =
                    std::max(0.f, (width - left - right - column_gap * float(g.columns - 1)) / float(g.columns));
                std::vector<Node*> placed;
                each_child(g, [&](Node& c) {
                    if (c.formatting != Formatting::None) {
                        placed.push_back(&c);
                    }
                });
                const auto rows = (placed.size() + g.columns - 1) / g.columns;
                std::vector<float> heights(rows);
                for (std::size_t i = 0; i < placed.size(); ++i) {
                    auto& c = *placed[i];
                    if (length(c.style->find(P::Width)).automatic) {
                        YGNodeStyleSetWidth(c.yoga, cell);
                    }
                    YGNodeCalculateLayout(c.yoga, cell, YGUndefined, YGDirectionLTR);
                    heights[i / g.columns] = std::max(heights[i / g.columns], YGNodeLayoutGetHeight(c.yoga));
                }
                float content{};
                for (const auto h : heights) {
                    content += h;
                }
                if (rows) {
                    content += row_gap * float(rows - 1);
                }
                const auto align_content = s.text(P::AlignContent);
                float y = top;
                if (height >= 0 && align_content == "center") {
                    y += std::max(0.f, (height - top - bottom - content) / 2);
                }
                if (height >= 0 && rows &&
                    (align_content.empty() || align_content == "normal" || align_content == "stretch")) {
                    const float extra = std::max(0.f, (height - top - bottom - content) / float(rows));
                    for (auto& h : heights) {
                        h += extra;
                    }
                    content += extra * float(rows);
                }
                const bool center_x = s.text(P::PlaceItems) == "center" || s.text(P::JustifyItems) == "center";
                const bool center_y = s.text(P::PlaceItems) == "center" || s.text(P::AlignItems) == "center";
                std::vector<float> row_tops(rows);
                for (std::size_t row = 0; row < rows; ++row) {
                    row_tops[row] = y;
                    y += heights[row] + row_gap;
                }
                std::fill(g.positions.begin(), g.positions.end(), std::array<float, 2>{});
                std::size_t cell_index{};
                std::size_t ordinal{};
                each_child(g, [&](Node& c) {
                    const auto k = ordinal++;
                    if (c.formatting == Formatting::None) {
                        return;
                    }
                    const auto row = cell_index / g.columns;
                    const auto column = cell_index % g.columns;
                    ++cell_index;
                    float x = left + float(column) * (cell + column_gap);
                    float dy{};
                    if (center_x) {
                        x += (cell - YGNodeLayoutGetWidth(c.yoga)) / 2;
                    }
                    if (center_y) {
                        dy = (heights[row] - YGNodeLayoutGetHeight(c.yoga)) / 2;
                    }
                    g.positions[k] = {x, row_tops[row] + dy};
                });
                return content + top + bottom;
            }

            static YGSize measure(YGNodeConstRef y, float width, YGMeasureMode wm, float height,
                                  YGMeasureMode hm) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    ++n.measure_calls;
                    const auto& text = n.engine->text;
                    const auto measured = text.layout(n.index, wm == YGMeasureModeUndefined ? unbounded : width);
                    const float natural_width = text.layout(n.index, unbounded).width;
                    // A text box is as tall as its lines; it overflows a smaller available height.
                    return {wm == YGMeasureModeExactly  ? width
                            : wm == YGMeasureModeAtMost ? std::min(width, natural_width)
                                                        : measured.width,
                            hm == YGMeasureModeExactly ? height : measured.height};
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return {};
                }
            }

            static float baseline(YGNodeConstRef y, float, float) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    const auto& s = *n.style;
                    return n.engine->text.layout(n.index, unbounded).baseline +
                           px(s, P::PaddingTop, px(s, P::Padding)) + px(s, P::BorderTopWidth, px(s, P::BorderWidth));
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return 0;
                }
            }

            static YGSize grid_measured(YGNodeConstRef y, float width, YGMeasureMode, float, YGMeasureMode) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    return {width, n.engine->grid(n, width + horizontal(*n.style)) - vertical(*n.style)};
                } catch (...) {
                    n.engine->callback_error = std::current_exception();
                    return {};
                }
            }

            // Lays out from the root: at the viewport's size, at a width shrunk to fit, or at the root's own size.
            void calculate(YGNodeRef root) {
                if (options.viewport) {
                    const auto [w, h] = *options.viewport;
                    YGNodeStyleSetWidth(root, w);
                    YGNodeStyleSetHeight(root, h);
                    YGNodeCalculateLayout(root, w, h, YGDirectionLTR);
                } else if (options.fit_width && css()) {
                    // The available width is the containing block's: percentages resolve against it. A box of auto
                    // width shrinks to fit it, less its margins; a replaced box keeps its own size.
                    const auto& n = nodes[0];
                    const float available = *options.fit_width;
                    if (std::isnan(available) || available < 0) {
                        throw std::invalid_argument("WEFT_LAYOUT_TREE: shrink-to-fit width");
                    }
                    if (!n.box->replaced && length(n.style->find(P::Width)).automatic) {
                        if (!n.unknown_width.empty()) {
                            throw std::invalid_argument(n.unknown_width);
                        }
                        const auto& s = *n.style;
                        const float margins =
                            px(s, P::MarginLeft, px(s, P::Margin)) + px(s, P::MarginRight, px(s, P::Margin));
                        const float width =
                            std::max(n.min_width, std::min(std::max(0.f, available - margins), n.max_width));
                        const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                        YGNodeStyleSetWidth(root, content_box ? width - horizontal(s) : width);
                    }
                    YGNodeCalculateLayout(root, available, YGUndefined, YGDirectionLTR);
                } else {
                    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);
                }
            }

            std::vector<BoxGeometry> run() {
                link();
                classify();
                create();
                if (css()) {
                    for (auto i = nodes.size(); i-- > 0;) {
                        intrinsic(nodes[i]);
                    }
                    for (std::size_t i = 1; i < nodes.size(); ++i) {
                        automatic_minimum(nodes[i]);
                    }
                }
                const auto root = nodes[0].yoga;
                calculate(root);
                if (callback_error) {
                    std::rethrow_exception(callback_error);
                }
                std::vector<BoxGeometry> out(nodes.size());
                // Origin of each box's children: a hidden box places its subtree at the layout origin.
                std::vector<std::array<float, 2>> origin(nodes.size());
                // Each box's position among its parent's children, and each parent's count so far.
                std::vector<std::size_t> ordinal(nodes.size());
                std::vector<std::size_t> counted(nodes.size());
                for (auto& n : nodes) {
                    auto& g = out[n.index];
                    const auto p = n.box->parent;
                    if (p != none) {
                        ordinal[n.index] = counted[p]++;
                    }
                    if (css() && n.formatting == Formatting::None) {
                        continue;
                    }
                    const auto y = n.yoga;
                    const bool in_grid = p != none && nodes[p].formatting == Formatting::Grid;
                    g.left = in_grid ? nodes[p].positions[ordinal[n.index]][0] : YGNodeLayoutGetLeft(y);
                    g.top = in_grid ? nodes[p].positions[ordinal[n.index]][1] : YGNodeLayoutGetTop(y);
                    g.width = YGNodeLayoutGetWidth(y);
                    g.height = YGNodeLayoutGetHeight(y);
                    g.x = (p == none ? 0 : origin[p][0]) + g.left;
                    g.y = (p == none ? 0 : origin[p][1]) + g.top;
                    origin[n.index] = {g.x, g.y};
                    g.measure_calls = n.measure_calls;
                    if (n.formatting == Formatting::Grid) {
                        grid(n, g.width, g.height);
                        if (callback_error) {
                            std::rethrow_exception(callback_error);
                        }
                    }
                    if (options.final_text && n.box->text) {
                        // Final text is independently laid out even when Yoga skipped measure.
                        const float content_width = std::max(0.0f, g.width - YGNodeLayoutGetPadding(y, YGEdgeLeft) -
                                                                       YGNodeLayoutGetPadding(y, YGEdgeRight) -
                                                                       YGNodeLayoutGetBorder(y, YGEdgeLeft) -
                                                                       YGNodeLayoutGetBorder(y, YGEdgeRight));
                        g.text = text.layout(n.index, content_width);
                    }
                }
                return out;
            }
        };
    } // namespace

    std::vector<BoxGeometry> layout_boxes(std::span<const LayoutBox> boxes, const TextMeasurer& text,
                                          const LayoutOptions& options) {
        Engine engine(boxes, text, options);
        return engine.run();
    }

    namespace {
        // The document text system for FlexNode trees: plain text shaped and broken at spaces, or an inline
        // formatting context.
        struct DocumentText {
            Font& font;
            FontSet* fonts{};
            std::vector<const FlexNode*> nodes;
            std::vector<float> font_size;
            std::vector<float> line_height;

            bool nowrap(std::size_t i) const {
                return nodes[i]->style.text(P::WhiteSpace, "normal") == "nowrap";
            }

            TextMetrics layout(std::size_t i, float available) const {
                const auto& n = *nodes[i];
                if (n.inline_content) {
                    if (!fonts) {
                        throw std::invalid_argument("WEFT_INLINE_PROFILE: font set required");
                    }
                    auto result = layout_inline(*n.inline_content, available, *fonts);
                    return {result.width, result.height, result.baseline, result.lines};
                }
                if (n.text.empty()) {
                    return {};
                }
                const auto lines = layout_text(n.text, font_size[i], available, nowrap(i), font, fonts);
                float widest{};
                for (const auto& line : lines) {
                    widest = std::max(widest, line.shaped.advance);
                }
                const auto m = font.metrics(font_size[i]);
                return {widest, float(lines.size()) * line_height[i],
                        (line_height[i] - m.ascent - m.descent) / 2 + m.ascent, lines.size()};
            }

            float min_content(std::size_t i) const {
                const auto& n = *nodes[i];
                if (n.inline_content) {
                    if (!fonts) {
                        throw std::invalid_argument("WEFT_INLINE_PROFILE: font set required");
                    }
                    return layout_inline(*n.inline_content, unbounded, *fonts).min_content;
                }
                if (nowrap(i)) {
                    return layout(i, unbounded).width;
                }
                float widest{};
                std::istringstream tokens(n.text);
                std::string word;
                while (tokens >> word) {
                    widest = std::max(widest, fonts ? fonts->shape(word, font_size[i]).shaped.advance
                                                    : font.shape(word, font_size[i]).advance);
                }
                return widest;
            }

            bool wraps(std::size_t i) const {
                return !nowrap(i);
            }
        };

        // Flattens a FlexNode tree into layout boxes, checking the tree's identity, text and font bounds and
        // inheriting font size and line height.
        struct Flattener {
            DocumentText& text;
            std::vector<LayoutBox>& boxes;
            std::set<std::string> identities;
            std::size_t text_bytes{};

            void add(const FlexNode& input, std::size_t parent, std::size_t depth) {
                if (boxes.size() >= std::size_t(constants::max_document_nodes) ||
                    depth > std::size_t(constants::max_document_depth)) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe tree bounds");
                }
                if (input.text.size() > constants::max_text_bytes) {
                    throw std::invalid_argument("WEFT_LAYOUT_LIMIT: probe text exceeds 64 KiB");
                }
                if ((text_bytes += input.text.size()) > constants::layout::max_aggregate_text_bytes) {
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
                const auto& s = input.style;
                const auto whitespace = s.text(P::WhiteSpace, "normal");
                if (s.contains(P::WhiteSpace) && whitespace != "normal" && whitespace != "nowrap" &&
                    !(input.inline_content && whitespace == "pre-line")) {
                    throw std::invalid_argument("WEFT_UNSUPPORTED_WHITE_SPACE");
                }
                const auto index = boxes.size();
                const bool root = parent == LayoutBox::root;
                const float size =
                    px(s, P::FontSize, root ? constants::layout::flex_default_font_size_px : text.font_size[parent]);
                const float line = px(s, P::LineHeight,
                                      root ? constants::layout::flex_default_line_height_px : text.line_height[parent]);
                if (size < 0 || line < 0) {
                    throw std::invalid_argument("WEFT_CSS_VALUE: negative dimension");
                }
                boxes.push_back({&s, parent, !input.text.empty() || input.inline_content != nullptr});
                text.nodes.push_back(&input);
                text.font_size.push_back(size);
                text.line_height.push_back(line);
                for (const auto& child : input.children) {
                    add(child, index, depth + 1);
                }
            }
        };

        std::vector<FlexBox> flex(const FlexNode& root, Font& font, FontSet* fonts, FlexMode mode) {
            DocumentText text{font, fonts};
            std::vector<LayoutBox> boxes;
            Flattener{text, boxes}.add(root, LayoutBox::root, 0);
            const auto layout = [&](std::size_t box, float available) {
                return text.layout(box, available);
            };
            const auto min_content = [&](std::size_t box) {
                return text.min_content(box);
            };
            const auto wraps = [&](std::size_t box) {
                return text.wraps(box);
            };
            const auto geometry = layout_boxes(boxes, {layout, min_content, wraps}, {mode, std::nullopt, true});
            std::vector<FlexBox> out;
            out.reserve(geometry.size());
            for (std::size_t i = 0; i < geometry.size(); ++i) {
                const auto& g = geometry[i];
                out.push_back({text.nodes[i]->id, g.x, g.y, g.width, g.height, g.text.width, g.text.height,
                               g.text.baseline, g.text.lines, g.measure_calls});
            }
            return out;
        }
    } // namespace

    std::vector<FlexBox> run_flex(const FlexNode& root, Font& font, FlexMode mode) {
        return flex(root, font, nullptr, mode);
    }

    std::vector<FlexBox> run_flex(const FlexNode& root, FontSet& fonts, FlexMode mode) {
        return flex(root, fonts.primary(), &fonts, mode);
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
