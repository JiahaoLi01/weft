#include "layout/flex.hpp"
#include "base/css_scalar.hpp"
#include "constants/layout.hpp"
#include "layout/grid.hpp"
#include <yoga/Yoga.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
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

        // An alignment keyword as Yoga takes it: without the overflow keywords safe and unsafe, first baseline as
        // baseline, and the start and end of the writing mode (horizontal, left to right) as flex-start and flex-end.
        std::string_view yoga_alignment(std::string_view v) {
            for (const std::string_view prefix : {"safe ", "unsafe "}) {
                if (v.starts_with(prefix)) {
                    v.remove_prefix(prefix.size());
                }
            }
            if (v == "first baseline") {
                return "baseline";
            }
            if (v == "start" || v == "self-start" || v == "left") {
                return "flex-start";
            }
            if (v == "end" || v == "self-end" || v == "right") {
                return "flex-end";
            }
            return v;
        }

        // normal is the initial value: stretch for the align properties of flex boxes.
        YGAlign align(std::string_view v) {
            v = yoga_alignment(v);
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
                                  P::GridAutoColumns,
                                  P::GridAutoFlow,
                                  P::GridAutoRows,
                                  P::GridColumnEnd,
                                  P::GridColumnStart,
                                  P::GridRowEnd,
                                  P::GridRowStart,
                                  P::GridTemplateColumns,
                                  P::GridTemplateRows,
                                  P::Height,
                                  P::JustifyContent,
                                  P::JustifyItems,
                                  P::JustifySelf,
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
                                  P::Order,
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
            // CSS Grid (layout/grid.hpp): its items placed in tracks, each laid out by Yoga in a box of its grid area.
            Grid,
            None,
        };

        bool inline_level(std::string_view display) {
            return display == "inline" || display == "inline-block";
        }

        // A box that contains its absolutely positioned descendants: a positioned or transformed one (apply).
        bool contains_absolute(const Style& s) {
            const auto position = s.text(P::Position, "static");
            return position == "relative" || position == "absolute" || s.text(P::Transform, "none") != "none" ||
                   s.text(P::WillChange, "auto").find("transform") != std::string_view::npos;
        }

        struct Engine;
        struct Node;

        // A grid container's values and children, and the placement of its items.
        struct GridState {
            struct Child {
                // Its index among the grid's children in document order (Node::positions).
                std::size_t ordinal{};
                Node* node{};
                grid::Placement placement;
            };

            grid::TrackList columns;
            grid::TrackList rows;
            std::vector<grid::TrackSize> auto_columns;
            std::vector<grid::TrackSize> auto_rows;
            grid::Flow flow;
            // In-flow items in order-modified document order, their placement properties apart for grid::place, and
            // the absolutely positioned children.
            std::vector<Child> items;
            std::vector<grid::Placement> placements;
            std::vector<Child> absolute;
            // The last placement, for an explicit grid of these rows and columns.
            std::optional<std::array<std::size_t, 2>> placed_for;
            grid::Placed placed;
        };

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
            // Its order as a flex or grid item (CSS Display 3 §3.1); 0 out of flow.
            int order{};
            bool definite_height{};
            // Its border-box height where it is known before layout (known_heights).
            std::optional<float> known_height;
            // A grid's values, children and placement (prepare_grids).
            std::unique_ptr<GridState> grid;
            // A grid child's own Yoga root, a column flex box that holds it: as large as its grid area (or the grid's
            // padding box, for an absolutely positioned child), so that Yoga sizes, stretches and aligns it there.
            YGNodeRef area{};
            // Border-box origins of a grid's children in its border box, in child order.
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
                // Children before parents, so that no freed node is still attached; a grid child before its area.
                for (auto i = nodes.size(); i-- > 0;) {
                    if (nodes[i].yoga) {
                        YGNodeFree(nodes[i].yoga);
                    }
                    if (nodes[i].area) {
                        YGNodeFree(nodes[i].area);
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

            // A grid's children in order-modified document order (CSS Display 3 §3.1), stably by their order, each
            // with its index among the children in document order.
            std::vector<std::pair<std::size_t, Node*>> ordered_children(const Node& n) {
                std::vector<std::pair<std::size_t, Node*>> out;
                for (auto c = n.first_child; c != none; c = nodes[c].next_sibling) {
                    out.emplace_back(out.size(), &nodes[c]);
                }
                std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                    return a.second->order < b.second->order;
                });
                return out;
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
                    } else if ((display == "grid" || display == "inline-grid") && !n.box->replaced && !n.box->text) {
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
                        n.positions.resize(n.children);
                    }
                }
            }

            // Each grid's track lists and flow, its in-flow items in order-modified document order with their
            // placement properties, and its absolutely positioned children (orders are known once boxes are created).
            void prepare_grids() {
                for (auto& n : nodes) {
                    if (n.formatting != Formatting::Grid) {
                        continue;
                    }
                    const auto& s = *n.style;
                    auto state = std::make_unique<GridState>();
                    state->columns = grid::template_tracks("grid-template-columns", s.text(P::GridTemplateColumns));
                    state->rows = grid::template_tracks("grid-template-rows", s.text(P::GridTemplateRows));
                    state->auto_columns = s.contains(P::GridAutoColumns)
                                              ? grid::auto_tracks("grid-auto-columns", s.text(P::GridAutoColumns))
                                              : std::vector<grid::TrackSize>(1);
                    state->auto_rows = s.contains(P::GridAutoRows)
                                           ? grid::auto_tracks("grid-auto-rows", s.text(P::GridAutoRows))
                                           : std::vector<grid::TrackSize>(1);
                    if (s.contains(P::GridAutoFlow)) {
                        state->flow = grid::auto_flow(s.text(P::GridAutoFlow));
                    }
                    for (const auto& [k, c] : ordered_children(n)) {
                        if (c->formatting == Formatting::None) {
                            continue;
                        }
                        const auto& cs = *c->style;
                        GridState::Child child{k,
                                               c,
                                               {grid::line("grid-row-start", cs.text(P::GridRowStart)),
                                                grid::line("grid-row-end", cs.text(P::GridRowEnd)),
                                                grid::line("grid-column-start", cs.text(P::GridColumnStart)),
                                                grid::line("grid-column-end", cs.text(P::GridColumnEnd))}};
                        if (cs.text(P::Position) == "absolute") {
                            state->absolute.push_back(child);
                        } else {
                            state->placements.push_back(child.placement);
                            state->items.push_back(child);
                        }
                    }
                    n.grid = std::move(state);
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
                    // Track lists have the grid's own bound (layout/grid.hpp).
                    const bool tracks = id == P::GridTemplateColumns || id == P::GridTemplateRows ||
                                        id == P::GridAutoColumns || id == P::GridAutoRows;
                    if (!tracks && v.text().size() > constants::layout::max_style_value_bytes) {
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
                        if (!flex &&
                            (!css() || (v != "block" && !inline_level(v) && v != "grid" && v != "inline-grid"))) {
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
                        // stretch distributes nothing among flex items (CSS Box Alignment 3 §5.3).
                        const auto j_value = yoga_alignment(v);
                        YGJustify j;
                        if (j_value == "flex-start" || j_value == "normal" || j_value == "stretch") {
                            j = YGJustifyFlexStart;
                        } else if (j_value == "flex-end") {
                            j = YGJustifyFlexEnd;
                        } else if (j_value == "center") {
                            j = YGJustifyCenter;
                        } else if (j_value == "space-between") {
                            j = YGJustifySpaceBetween;
                        } else if (j_value == "space-around") {
                            j = YGJustifySpaceAround;
                        } else if (j_value == "space-evenly") {
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
                    } else if (id == P::Transform || id == P::GridTemplateColumns || id == P::GridTemplateRows ||
                               id == P::GridAutoColumns || id == P::GridAutoRows || id == P::GridAutoFlow ||
                               id == P::GridRowStart || id == P::GridRowEnd || id == P::GridColumnStart ||
                               id == P::GridColumnEnd || id == P::JustifyItems || id == P::JustifySelf ||
                               id == P::PlaceItems || id == P::Order) {
                        // Read by the containing block, grid and order rules.
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
                        // An absolutely positioned child is no flex item: its order is 0 (CSS Flexbox 1 §5.4).
                        if (const auto* order = n.style->find(P::Order);
                            order && n.style->text(P::Position) != "absolute" &&
                            (parent.formatting == Formatting::Flex || parent.formatting == Formatting::Grid)) {
                            const auto value = css::parse_integer(css::trim(order->text()));
                            if (!value) {
                                throw std::invalid_argument("WEFT_CSS_VALUE: order " + order->text());
                            }
                            n.order = *value;
                        }
                        if (parent.formatting == Formatting::Flex) {
                            // Flex items in order-modified document order (CSS Flexbox 1 §5.4): after the items
                            // before them of no greater order.
                            const auto order_at = [&](std::size_t k) {
                                return static_cast<const Node*>(YGNodeGetContext(YGNodeGetChild(parent.yoga, k)))
                                    ->order;
                            };
                            auto at = YGNodeGetChildCount(parent.yoga);
                            while (at > 0 && order_at(at - 1) > n.order) {
                                --at;
                            }
                            YGNodeInsertChild(parent.yoga, n.yoga, at);
                        } else if (parent.formatting != Formatting::Grid) {
                            YGNodeInsertChild(parent.yoga, n.yoga, YGNodeGetChildCount(parent.yoga));
                        } else if (n.formatting != Formatting::None) {
                            // A grid child is laid out in a box of its own (lay_out_item), not in the grid's node,
                            // which Yoga measures.
                            n.area = YGNodeNewWithConfig(config);
                            if (!n.area) {
                                throw std::bad_alloc();
                            }
                            YGNodeStyleSetFlexDirection(n.area, YGFlexDirectionColumn);
                            YGNodeInsertChild(n.area, n.yoga, 0);
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

            // What a child leaves unknown of its parent's intrinsic sizes, into `width` and `height` while they are
            // empty: nothing in an axis where its own size is px, which is all it contributes there; else what it does
            // not know of its own.
            static void pass_unknowns(const Node& c, std::string& width, std::string& height) {
                const auto fixed = [&](PropertyId key) {
                    const auto l = length(c.style->find(key));
                    return !l.automatic && !l.percent;
                };
                if (width.empty() && !fixed(P::Width)) {
                    width = c.unknown_width;
                }
                if (height.empty() && !fixed(P::Height)) {
                    height = c.unknown_height;
                }
            }

            // A px length a property states (a unitless 0 too), or nothing: absent, auto, a percentage or calc().
            static std::optional<float> stated_px(const CssValue* v) {
                const auto* d = v ? v->dimension() : nullptr;
                if (!d || !(d->unit == Unit::Px || (d->unit == Unit::None && d->value == 0))) {
                    return std::nullopt;
                }
                return bounded(*d);
            }

            // Each box's border-box height where it is known before layout, parents first, for the specified size
            // suggestion of a column flex item (CSS Flexbox 1 §4.5): the viewport's at the root; a px height; a
            // percentage of its containing block's known height (its parent's content box, or for an absolutely
            // positioned box the padding box of the nearest box that contains it); for an absolutely positioned box of
            // auto height with px top and bottom, that padding box less them and its margins. Percentages in edges,
            // and every other box, leave it unknown.
            void known_heights() {
                for (auto& n : nodes) {
                    const auto& s = *n.style;
                    std::string unknown;
                    const float edges = intrinsic_vertical(s, unknown);
                    const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                    const auto height = length(s.find(P::Height));
                    if (!n.index && options.viewport) {
                        n.known_height = (*options.viewport)[1];
                        continue;
                    }
                    if (!unknown.empty()) {
                        continue;
                    }
                    if (!height.automatic && !height.percent) {
                        n.known_height = height.value + (content_box ? edges : 0);
                        continue;
                    }
                    if (!n.index) {
                        continue;
                    }
                    const bool absolute = s.text(P::Position) == "absolute";
                    const Node* block = &nodes[n.box->parent];
                    while (absolute && block->index && !contains_absolute(*block->style)) {
                        block = &nodes[block->box->parent];
                    }
                    if (!block->known_height) {
                        continue;
                    }
                    const auto& b = *block->style;
                    std::string inner_unknown;
                    const auto border = [&](PropertyId side) {
                        return intrinsic_px(b, side, intrinsic_px(b, P::BorderWidth, 0, inner_unknown), inner_unknown);
                    };
                    const float inner = absolute ? border(P::BorderTopWidth) + border(P::BorderBottomWidth)
                                                 : intrinsic_vertical(b, inner_unknown);
                    if (!inner_unknown.empty()) {
                        continue;
                    }
                    const float basis = std::max(0.f, *block->known_height - inner);
                    if (height.percent) {
                        n.known_height =
                            basis * height.value / float(constants::percent_scale) + (content_box ? edges : 0);
                        continue;
                    }
                    // An auto margin is 0 here (CSS 2 §10.6.4).
                    const auto margin = [&](PropertyId side) -> std::optional<float> {
                        const auto* v = s.find(side) ? s.find(side) : s.find(P::Margin);
                        return !v || v->text() == "auto" ? std::optional<float>(0.f) : stated_px(v);
                    };
                    const auto top = stated_px(s.find(P::Top));
                    const auto bottom = stated_px(s.find(P::Bottom));
                    const auto margin_top = margin(P::MarginTop);
                    const auto margin_bottom = margin(P::MarginBottom);
                    if (absolute && top && bottom && margin_top && margin_bottom) {
                        n.known_height = std::max(0.f, basis - *top - *bottom - *margin_top - *margin_bottom);
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
                    if (w.percent) {
                        // A percentage width resolves against nothing in intrinsic sizes: the box contributes no
                        // min-content width and its natural width (its attribute's, or the default object width) as
                        // max-content (CSS Sizing 3 §5.2.2), which a grid's tracks read; flex keeps the diagnostic.
                        const auto natural = n.box->replaced_width.empty()
                                                 ? Length{constants::layout::default_object_width_px}
                                                 : presentation(n.box->replaced_width);
                        n.min_width = edges;
                        n.max_width =
                            (natural.automatic || natural.percent ? float(constants::layout::default_object_width_px)
                                                                  : natural.value) +
                            (content_box ? edges : 0);
                    }
                    n.intrinsic_height = h.value + (content_box ? edges_v : 0);
                } else if (n.formatting == Formatting::Grid) {
                    // The columns under min-content and max-content constraints (§11.1), from the items' intrinsic
                    // widths; the rows of items at their heights at their max-content widths.
                    auto& state = *n.grid;
                    const float column_gap =
                        intrinsic_px(s, P::ColumnGap, intrinsic_px(s, P::Gap, 0, unknown_w), unknown_w);
                    const float row_gap = intrinsic_px(s, P::RowGap, intrinsic_px(s, P::Gap, 0, unknown_h), unknown_h);
                    const auto definite = [&](PropertyId key, float box_edges) {
                        const auto l = length(s.find(key));
                        return l.automatic || l.percent ? std::numeric_limits<float>::quiet_NaN()
                                                        : std::max(0.f, l.value - (content_box ? 0 : box_edges));
                    };
                    const auto column_repeats =
                        repeat_count(state.columns, s, true, definite(P::Width, edges), column_gap);
                    const auto row_repeats = repeat_count(state.rows, s, false, definite(P::Height, edges_v), row_gap);
                    const auto explicit_columns = grid::expand(state.columns, column_repeats);
                    const auto explicit_rows = grid::expand(state.rows, row_repeats);
                    const auto& placed = placement(state, explicit_rows.size(), explicit_columns.size());
                    const float indefinite = std::numeric_limits<float>::quiet_NaN();
                    const auto columns = axis_tracks(state.columns, column_repeats, explicit_columns,
                                                     state.auto_columns, placed, true, indefinite);
                    auto rows =
                        axis_tracks(state.rows, row_repeats, explicit_rows, state.auto_rows, placed, false, indefinite);
                    std::vector<grid::Contribution> across;
                    std::vector<grid::Contribution> down;
                    for (std::size_t i = 0; i < state.items.size(); ++i) {
                        const auto& c = *state.items[i].node;
                        const auto& area = placed.areas[i];
                        across.push_back(column_contribution(c, area, columns, column_gap));
                        const float height = c.intrinsic_height + intrinsic_margins(*c.style, false);
                        down.push_back({area.row, area.row_span, height, height, height});
                        pass_unknowns(c, unknown_w, unknown_h);
                    }
                    auto narrowest = columns;
                    grid::size_tracks(narrowest, across, grid::Space::MinContent, 0, column_gap, false);
                    auto widest = columns;
                    grid::size_tracks(widest, across, grid::Space::MaxContent, 0, column_gap, false);
                    grid::size_tracks(rows, down, grid::Space::MaxContent, 0, row_gap, false);
                    n.min_width = grid::extent(narrowest, column_gap) + edges;
                    n.max_width = grid::extent(widest, column_gap) + edges;
                    n.intrinsic_height = grid::extent(rows, row_gap) + edges_v;
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
                        pass_unknowns(c, unknown_w, unknown_h);
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
                // The specified size suggestion (CSS Flexbox 1 §4.5), the most the content-based minimum can be: a px
                // size, or a percentage of a height known before layout.
                const auto& main_size = main_row ? specified : specified_height;
                std::optional<float> suggestion;
                if (!main_size.automatic && !main_size.percent) {
                    suggestion = main_row ? n.max_width : n.intrinsic_height;
                } else if (!main_row && main_size.percent && n.known_height) {
                    suggestion = *n.known_height;
                }
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
                    // Content of an unknown size needs no more than the specified size allows.
                    if (!suggestion) {
                        throw std::invalid_argument(unknown);
                    }
                    minimum = *suggestion;
                }
                if (suggestion) {
                    minimum = std::min(minimum, *suggestion);
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

            // Px margins of a box across (left and right) or down (top and bottom); percentages and auto resolve
            // against nothing in intrinsic sizes.
            static float intrinsic_margins(const Style& s, bool across) {
                float total{};
                for (const auto side :
                     across ? std::array{P::MarginLeft, P::MarginRight} : std::array{P::MarginTop, P::MarginBottom}) {
                    const auto* v = s.find(side);
                    if (!v) {
                        v = s.find(P::Margin);
                    }
                    if (v) {
                        const auto l = length(v);
                        total += l.automatic || l.percent ? 0 : l.value;
                    }
                }
                return total;
            }

            // A scroll container has no automatic minimum size in a grid (CSS Grid 1 §6.6).
            static bool scroll_container(const Style& s) {
                const auto overflow = s.text(P::Overflow, "visible");
                return overflow != "visible" && overflow != "clip";
            }

            // A grid gap in px (CSS Box Alignment 3 §8): its length, or its percentage of a definite content size; of
            // an indefinite one, nothing.
            static float gap(const Style& s, PropertyId axis, float basis) {
                const auto* v = s.find(axis);
                if (!v) {
                    v = s.find(P::Gap);
                }
                if (!v || v->text() == "normal") {
                    return 0;
                }
                const auto l = length(v);
                if (l.automatic) {
                    return 0;
                }
                if (l.percent) {
                    return std::isfinite(basis) ? basis * l.value / float(constants::percent_scale) : 0.f;
                }
                return l.value;
            }

            // How many times a track list's automatic repetition repeats (§7.2.3.2): as many as fit the definite
            // content size, else the px maximum (max-width or max-height), else as few as reach the px minimum; once
            // otherwise.
            static std::size_t repeat_count(const grid::TrackList& list, const Style& s, bool columns, float definite,
                                            float gap) {
                if (list.repeat == grid::TrackList::Repeat::None) {
                    return 0;
                }
                if (std::isfinite(definite)) {
                    return grid::repetitions(list, definite, definite, gap, false);
                }
                std::string ignored;
                const float edges = columns ? intrinsic_horizontal(s, ignored) : intrinsic_vertical(s, ignored);
                const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
                const auto content = [&](const Length& l) {
                    return std::max(0.f, l.value - (border_box ? edges : 0));
                };
                const auto max = length(s.find(columns ? P::MaxWidth : P::MaxHeight));
                if (!max.automatic && !max.percent) {
                    return grid::repetitions(list, content(max), content(max), gap, false);
                }
                const auto min = length(s.find(columns ? P::MinWidth : P::MinHeight));
                if (!min.automatic && !min.percent) {
                    return grid::repetitions(list, content(min), content(min), gap, true);
                }
                return 1;
            }

            // The placement of a grid's items (§8.5), kept for the explicit grid's size.
            static const grid::Placed& placement(GridState& state, std::size_t rows, std::size_t columns) {
                const std::array<std::size_t, 2> key{rows, columns};
                if (state.placed_for != key) {
                    state.placed = grid::place(state.placements, rows, columns, state.flow);
                    state.placed_for = key;
                }
                return state.placed;
            }

            // The tracks of one axis of the implicit grid (§7.1): the explicit ones, auto-fit repetitions that no item
            // occupies collapsed, and implicit tracks of the auto track list, cycled (backwards before the explicit
            // grid, §7.6); percentages of `basis`.
            static std::vector<grid::Track> axis_tracks(const grid::TrackList& list, std::size_t repeats,
                                                        const std::vector<grid::TrackSize>& explicit_tracks,
                                                        const std::vector<grid::TrackSize>& automatic,
                                                        const grid::Placed& placed, bool columns, float basis) {
                const auto total = columns ? placed.columns : placed.rows;
                const auto before = columns ? placed.columns_before : placed.rows_before;
                std::vector<bool> occupied(total);
                for (const auto& a : placed.areas) {
                    const auto first = columns ? a.column : a.row;
                    const auto span = columns ? a.column_span : a.row_span;
                    for (auto t = first; t < first + span && t < total; ++t) {
                        occupied[t] = true;
                    }
                }
                const auto fit = before + list.repeat_at;
                const auto fit_end = fit + repeats * list.repeated.size();
                const auto count = automatic.size();
                std::vector<grid::Track> out(total);
                for (std::size_t i = 0; i < total; ++i) {
                    const auto& size = i < before ? automatic[count - 1 - (before - 1 - i) % count]
                                       : i - before < explicit_tracks.size()
                                           ? explicit_tracks[i - before]
                                           : automatic[(i - before - explicit_tracks.size()) % count];
                    out[i] = grid::resolve(size, basis);
                    out[i].collapsed =
                        list.repeat == grid::TrackList::Repeat::Fit && i >= fit && i < fit_end && !occupied[i];
                }
                return out;
            }

            // Whether a content distribution value stretches auto tracks (normal and stretch, §11.8).
            static bool stretches(std::string_view value) {
                const auto text = css::trim(value);
                return text.empty() || text == "normal" || text == "stretch";
            }

            static std::size_t visible(const std::vector<grid::Track>& tracks) {
                return std::size_t(std::count_if(tracks.begin(), tracks.end(), [](const grid::Track& t) {
                    return !t.collapsed;
                }));
            }

            // The start of each track from `origin`, and the end of the last: each track not collapsed that another
            // follows has the gutter and the distributed space after it (§10.1, §7.2.3.2).
            static std::vector<float> track_lines(const std::vector<grid::Track>& tracks, float gap,
                                                  grid::Distribution distribution, float origin) {
                std::size_t last = tracks.size();
                for (std::size_t t = 0; t < tracks.size(); ++t) {
                    last = tracks[t].collapsed ? last : t;
                }
                std::vector<float> lines(tracks.size() + 1);
                float at = origin + distribution.offset;
                for (std::size_t t = 0; t < tracks.size(); ++t) {
                    lines[t] = at;
                    at += tracks[t].base;
                    if (!tracks[t].collapsed && last != tracks.size() && t < last) {
                        at += gap + distribution.between;
                    }
                }
                lines[tracks.size()] = at;
                return lines;
            }

            // The size of the tracks [first, first + span) with what lies between them.
            static float span_size(const std::vector<float>& lines, const std::vector<grid::Track>& tracks,
                                   std::size_t first, std::size_t span) {
                const auto last = first + span - 1;
                return lines[last] + tracks[last].base - lines[first];
            }

            // A laid-out box's padding and border as Yoga resolved them: left, top, right, bottom.
            static std::array<float, 4> layout_edges(YGNodeConstRef y) {
                std::array<float, 4> out{};
                const std::array edges{YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom};
                for (std::size_t i = 0; i < edges.size(); ++i) {
                    out[i] = YGNodeLayoutGetPadding(y, edges[i]) + YGNodeLayoutGetBorder(y, edges[i]);
                }
                return out;
            }

            // An item's outer contributions to the columns it spans (§11.5) from its intrinsic widths: a px width is
            // all three; otherwise its content's, as intrinsic sizes take it (percentages in it resolve against
            // nothing), within px min-width and max-width. The minimum contribution is its automatic minimum width
            // (§6.6) when it spans an auto minimum and is no scroll container: its min-content width, capped by the
            // area when all its tracks have a fixed maximum; else its min-width, or nothing.
            grid::Contribution column_contribution(const Node& c, const grid::Area& area,
                                                   const std::vector<grid::Track>& columns, float gap) const {
                const auto& s = *c.style;
                std::string ignored;
                const float edges = intrinsic_horizontal(s, ignored);
                const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                const auto border = [&](const Length& l) {
                    return l.value + (content_box ? edges : 0);
                };
                const auto definite = [](const Length& l) {
                    return !l.automatic && !l.percent;
                };
                const auto width = length(s.find(P::Width));
                const auto min_width = length(s.find(P::MinWidth));
                const auto max_width = length(s.find(P::MaxWidth));
                const float margins = intrinsic_margins(s, true);
                float min_content = definite(width) ? border(width) : c.min_width;
                float max_content = definite(width) ? border(width) : c.max_width;
                if (definite(max_width)) {
                    min_content = std::min(min_content, border(max_width));
                    max_content = std::min(max_content, border(max_width));
                }
                if (definite(min_width)) {
                    min_content = std::max(min_content, border(min_width));
                    max_content = std::max(max_content, border(min_width));
                }
                max_content = std::max(max_content, min_content);
                float minimum = min_content;
                if (!definite(width)) {
                    if (definite(min_width)) {
                        minimum = border(min_width);
                    } else {
                        bool automatic_track{};
                        bool fixed_maximum = true;
                        float cap = gap * float(area.column_span - 1);
                        for (auto t = area.column; t < area.column + area.column_span; ++t) {
                            automatic_track |= columns[t].min.kind == grid::Breadth::Kind::Auto;
                            fixed_maximum &= columns[t].max.kind == grid::Breadth::Kind::Length;
                            cap += columns[t].max.value;
                        }
                        minimum = !automatic_track || scroll_container(s) ? 0
                                  : fixed_maximum ? std::max(0.f, std::min(min_content, cap - margins))
                                                  : min_content;
                    }
                }
                return {area.column, area.column_span, min_content + margins, max_content + margins, minimum + margins};
            }

            // An item's minimum contribution to the rows it spans (§6.6), laid out at its content height (`outer`,
            // margins included): that height, or with an auto or percentage height its px min-height, or nothing when
            // it is a scroll container or spans no auto minimum.
            static float row_minimum(const Node& c, const grid::Area& area, const std::vector<grid::Track>& rows,
                                     float outer) {
                const auto& s = *c.style;
                const auto height = length(s.find(P::Height));
                if (!height.automatic && !height.percent) {
                    return outer;
                }
                const auto y = c.yoga;
                const float margins = YGNodeLayoutGetMargin(y, YGEdgeTop) + YGNodeLayoutGetMargin(y, YGEdgeBottom);
                const auto min_height = length(s.find(P::MinHeight));
                if (!min_height.automatic && !min_height.percent) {
                    const float edges =
                        s.text(P::BoxSizing, "content-box") == "content-box"
                            ? YGNodeLayoutGetPadding(y, YGEdgeTop) + YGNodeLayoutGetPadding(y, YGEdgeBottom) +
                                  YGNodeLayoutGetBorder(y, YGEdgeTop) + YGNodeLayoutGetBorder(y, YGEdgeBottom)
                            : 0;
                    return std::min(outer, min_height.value + edges + margins);
                }
                bool automatic_track{};
                for (auto t = area.row; t < area.row + area.row_span; ++t) {
                    automatic_track |= rows[t].min.kind == grid::Breadth::Kind::Auto;
                }
                return automatic_track && !scroll_container(s) ? outer : margins;
            }

            // place-items as a strict page keeps it (a forgiving page's cascade expands it): align-items, then
            // justify-items; one value is both.
            static std::string place_items(const Style& s, bool down) {
                const auto value = css::trim(s.text(P::PlaceItems, ""));
                const auto words = css::split(value, ' ').parts;
                if (words.size() == 2 && words[0] != "safe" && words[0] != "unsafe" && words[0] != "first" &&
                    words[0] != "last" && words[0] != "legacy") {
                    return down ? words[0] : words[1];
                }
                return value;
            }

            // An item's alignment in its grid area, across (its justify-self, else the grid's justify-items) or down
            // (its align-self, else align-items, else place-items): normal stretches a box but places a replaced one
            // at the start; baseline alignment across is the start (horizontal text).
            static grid::Align item_alignment(const Node& c, const Style& g, bool down) {
                std::string chosen(c.style->text(down ? P::AlignSelf : P::JustifySelf, "auto"));
                auto value = grid::self_alignment(down ? "align-self" : "justify-self", chosen);
                if (value == grid::Align::Auto) {
                    chosen = std::string(g.text(down ? P::AlignItems : P::JustifyItems, ""));
                    if (chosen.empty()) {
                        chosen = place_items(g, down);
                    }
                    value = grid::self_alignment(down ? "align-items" : "justify-items", chosen);
                }
                if (value == grid::Align::Auto ||
                    (value == grid::Align::Stretch && c.box->replaced && css::trim(chosen) != "stretch")) {
                    value = c.box->replaced ? grid::Align::Start : grid::Align::Stretch;
                }
                return !down && value == grid::Align::Baseline ? grid::Align::Start : value;
            }

            // Lays out a grid item in its area box (Node::area), a column flex box `width` wide: across, stretched or
            // placed as it justifies itself; down, at its content height when `height` is indefinite (NaN), else in
            // that height, stretched (with an auto height and no auto margin) or placed as it aligns itself.
            static void lay_out_item(Node& c, float width, float height, grid::Align justify, grid::Align align) {
                const auto a = c.area;
                const auto y = c.yoga;
                const auto& s = *c.style;
                const bool definite = std::isfinite(height);
                width = std::max(0.f, width);
                YGNodeStyleSetWidth(a, width);
                if (definite) {
                    YGNodeStyleSetHeight(a, std::max(0.f, height));
                } else {
                    YGNodeStyleSetHeightAuto(a);
                }
                const auto automatic_margin = [&](PropertyId side) {
                    return s.text(side, s.text(P::Margin, "0")) == "auto";
                };
                const bool stretch = definite && align == grid::Align::Stretch && length(s.find(P::Height)).automatic &&
                                     !automatic_margin(P::MarginTop) && !automatic_margin(P::MarginBottom);
                YGNodeStyleSetFlexGrow(y, stretch ? 1.f : 0.f);
                YGNodeStyleSetFlexShrink(y, stretch ? 1.f : 0.f);
                YGNodeStyleSetFlexBasisAuto(y);
                YGNodeStyleSetJustifyContent(a, align == grid::Align::Center ? YGJustifyCenter
                                                : align == grid::Align::End  ? YGJustifyFlexEnd
                                                                             : YGJustifyFlexStart);
                YGNodeStyleSetAlignItems(a, YGAlignFlexStart);
                YGNodeStyleSetAlignSelf(y, justify == grid::Align::Stretch  ? YGAlignStretch
                                           : justify == grid::Align::Center ? YGAlignCenter
                                           : justify == grid::Align::End    ? YGAlignFlexEnd
                                                                            : YGAlignFlexStart);
                YGNodeCalculateLayout(a, width, definite ? std::max(0.f, height) : YGUndefined, YGDirectionLTR);
            }

            // The distance from a laid-out box's border-box top to its first baseline (CSS Box Alignment 3 §9): its
            // first line's for text, else that of its first in-flow child that has one; none for a replaced box, a
            // grid, or a box without such a child.
            std::optional<float> first_baseline(const Node& n) const {
                const auto y = n.yoga;
                if (n.box->text) {
                    const auto edges = layout_edges(y);
                    const float content = std::max(0.f, YGNodeLayoutGetWidth(y) - edges[0] - edges[2]);
                    return edges[1] + text.layout(n.index, content).baseline;
                }
                if (n.box->replaced || n.formatting == Formatting::Grid || n.formatting == Formatting::None) {
                    return std::nullopt;
                }
                for (std::size_t i = 0, count = YGNodeGetChildCount(y); i < count; ++i) {
                    const auto child = YGNodeGetChild(y, i);
                    const auto* c = static_cast<const Node*>(YGNodeGetContext(child));
                    if (!c || c->formatting == Formatting::None ||
                        YGNodeStyleGetPositionType(child) == YGPositionTypeAbsolute) {
                        continue;
                    }
                    if (const auto b = first_baseline(*c)) {
                        return YGNodeLayoutGetTop(child) + *b;
                    }
                }
                return std::nullopt;
            }

            // A grid's layout at a content width and height, each definite (Exactly) or a constraint (AtMost a
            // maximum, Undefined none), as Yoga measures it (§11.1): sizes its columns, lays its items out in them
            // to size its rows, and returns its content size. `final` places the children (Node::positions) at a
            // definite size; `edges` are the grid's padding and border, left, top, right, bottom.
            std::array<float, 2> lay_out_grid(Node& g, float width, YGMeasureMode wm, float height, YGMeasureMode hm,
                                              bool final, const std::array<float, 4>& edges) {
                auto& state = *g.grid;
                const auto& s = *g.style;
                const float indefinite = std::numeric_limits<float>::quiet_NaN();
                const bool definite_width = wm == YGMeasureModeExactly;
                const bool definite_height = hm == YGMeasureModeExactly;
                const float width_basis = definite_width ? width : indefinite;
                const float height_basis = definite_height ? height : indefinite;
                const float column_gap = gap(s, P::ColumnGap, width_basis);
                const float row_gap = gap(s, P::RowGap, height_basis);
                const auto column_repeats = repeat_count(state.columns, s, true, width_basis, column_gap);
                const auto row_repeats = repeat_count(state.rows, s, false, height_basis, row_gap);
                const auto explicit_columns = grid::expand(state.columns, column_repeats);
                const auto explicit_rows = grid::expand(state.rows, row_repeats);
                const auto& placed = placement(state, explicit_rows.size(), explicit_columns.size());
                // The columns: in the definite width, or at the max-content width, or shrunk to fit a maximum.
                auto columns = axis_tracks(state.columns, column_repeats, explicit_columns, state.auto_columns, placed,
                                           true, width_basis);
                std::vector<grid::Contribution> across;
                across.reserve(state.items.size());
                for (std::size_t i = 0; i < state.items.size(); ++i) {
                    across.push_back(column_contribution(*state.items[i].node, placed.areas[i], columns, column_gap));
                }
                const auto justify_content = s.text(P::JustifyContent, "normal");
                if (definite_width) {
                    grid::size_tracks(columns, across, grid::Space::Definite, width, column_gap,
                                      stretches(justify_content));
                } else {
                    auto widest = columns;
                    grid::size_tracks(widest, across, grid::Space::MaxContent, 0, column_gap, false);
                    if (wm == YGMeasureModeAtMost && width < grid::extent(widest, column_gap)) {
                        auto narrowest = columns;
                        grid::size_tracks(narrowest, across, grid::Space::MinContent, 0, column_gap, false);
                        if (width <= grid::extent(narrowest, column_gap)) {
                            columns = std::move(narrowest);
                        } else {
                            grid::size_tracks(columns, across, grid::Space::Definite, width, column_gap,
                                              stretches(justify_content));
                        }
                    } else {
                        columns = std::move(widest);
                    }
                }
                const float content_width = definite_width ? width : grid::extent(columns, column_gap);
                const auto column_lines =
                    track_lines(columns, column_gap,
                                grid::distribute(justify_content, content_width - grid::extent(columns, column_gap),
                                                 visible(columns)),
                                edges[0]);
                // The rows: each item laid out across its columns at its content height (§11.5), one aligned to its
                // row's baseline shimmed down to it (§11.5 step 1).
                auto rows =
                    axis_tracks(state.rows, row_repeats, explicit_rows, state.auto_rows, placed, false, height_basis);
                const auto count = state.items.size();
                std::vector<grid::Contribution> down;
                down.reserve(count);
                std::vector<grid::Align> justify(count);
                std::vector<grid::Align> align(count);
                std::vector<float> baselines(count, indefinite);
                for (std::size_t i = 0; i < count; ++i) {
                    auto& c = *state.items[i].node;
                    const auto& area = placed.areas[i];
                    justify[i] = item_alignment(c, s, false);
                    align[i] = item_alignment(c, s, true);
                    lay_out_item(c, span_size(column_lines, columns, area.column, area.column_span), indefinite,
                                 justify[i], align[i]);
                    const float outer = YGNodeLayoutGetHeight(c.area);
                    down.push_back({area.row, area.row_span, outer, outer, row_minimum(c, area, rows, outer)});
                    if (align[i] == grid::Align::Baseline && area.row_span == 1) {
                        baselines[i] = YGNodeLayoutGetMargin(c.yoga, YGEdgeTop) +
                                       first_baseline(c).value_or(YGNodeLayoutGetHeight(c.yoga));
                    }
                }
                std::map<std::size_t, float> row_baselines;
                for (std::size_t i = 0; i < count; ++i) {
                    if (!std::isnan(baselines[i])) {
                        auto& b = row_baselines[placed.areas[i].row];
                        b = std::max(b, baselines[i]);
                    }
                }
                std::vector<float> shims(count);
                for (std::size_t i = 0; i < count; ++i) {
                    if (!std::isnan(baselines[i])) {
                        shims[i] = row_baselines[placed.areas[i].row] - baselines[i];
                        down[i].min_content += shims[i];
                        down[i].max_content += shims[i];
                        down[i].minimum += shims[i];
                    }
                }
                const auto align_content = s.text(P::AlignContent, "normal");
                if (definite_height) {
                    grid::size_tracks(rows, down, grid::Space::Definite, height, row_gap, stretches(align_content));
                } else {
                    grid::size_tracks(rows, down, grid::Space::MaxContent, 0, row_gap, false);
                }
                const float content_height = definite_height ? height : grid::extent(rows, row_gap);
                if (final) {
                    const auto row_lines = track_lines(
                        rows, row_gap,
                        grid::distribute(align_content, content_height - grid::extent(rows, row_gap), visible(rows)),
                        edges[1]);
                    for (std::size_t i = 0; i < count; ++i) {
                        auto& c = *state.items[i].node;
                        const auto& area = placed.areas[i];
                        lay_out_item(c, span_size(column_lines, columns, area.column, area.column_span),
                                     span_size(row_lines, rows, area.row, area.row_span), justify[i], align[i]);
                        g.positions[state.items[i].ordinal] = {column_lines[area.column] + YGNodeLayoutGetLeft(c.yoga),
                                                               row_lines[area.row] + YGNodeLayoutGetTop(c.yoga) +
                                                                   shims[i]};
                    }
                    place_absolute(g, {column_lines, columns, placed.columns_before, explicit_columns.size()},
                                   {row_lines, rows, placed.rows_before, explicit_rows.size()});
                }
                return {content_width, content_height};
            }

            // One axis of a laid-out grid: its track lines and tracks, the implicit tracks before the explicit grid
            // and the explicit ones.
            struct GridAxis {
                const std::vector<float>& lines;
                const std::vector<grid::Track>& tracks;
                std::size_t before{};
                std::size_t explicit_tracks{};

                // Where a line number puts an edge of a containing block (§9.1): the start of its track, or the end of
                // the track before it for an end edge; `fallback` (a padding edge) for auto, a span or a line outside
                // the implicit grid.
                float edge(const grid::Line& line, bool end, float fallback) const {
                    if (line.kind != grid::Line::Kind::Number) {
                        return fallback;
                    }
                    const auto index =
                        (line.value > 0 ? line.value - 1 : int(explicit_tracks) + 1 + line.value) + int(before);
                    if (index < 0 || index > int(tracks.size())) {
                        return fallback;
                    }
                    const auto i = std::size_t(index);
                    return end && i > 0 ? lines[i - 1] + tracks[i - 1].base : lines[i];
                }
            };

            // Places a grid's absolutely positioned children (§9.1): each in its containing block, the grid's padding
            // box or, when the grid is their containing block, the area between the line numbers it names (auto at
            // the padding edges). Insets against a containing block outside the grid are outside the profile.
            void place_absolute(Node& g, const GridAxis& across, const GridAxis& down) {
                auto& state = *g.grid;
                if (state.absolute.empty()) {
                    return;
                }
                const auto y = g.yoga;
                const float left = YGNodeLayoutGetBorder(y, YGEdgeLeft);
                const float top = YGNodeLayoutGetBorder(y, YGEdgeTop);
                const float right = YGNodeLayoutGetWidth(y) - YGNodeLayoutGetBorder(y, YGEdgeRight);
                const float bottom = YGNodeLayoutGetHeight(y) - YGNodeLayoutGetBorder(y, YGEdgeBottom);
                const bool contains = contains_absolute(*g.style);
                for (const auto& child : state.absolute) {
                    auto& c = *child.node;
                    const auto& cs = *c.style;
                    if (!contains) {
                        for (const auto side : {P::Left, P::Right, P::Top, P::Bottom}) {
                            if (cs.text(side, "auto") != "auto") {
                                throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: an absolutely positioned grid "
                                                            "child with insets against a box outside the grid");
                            }
                        }
                    }
                    const auto& p = child.placement;
                    const float x0 = contains ? across.edge(p.column_start, false, left) : left;
                    const float x1 = contains ? across.edge(p.column_end, true, right) : right;
                    const float y0 = contains ? down.edge(p.row_start, false, top) : top;
                    const float y1 = contains ? down.edge(p.row_end, true, bottom) : bottom;
                    const auto a = c.area;
                    YGNodeStyleSetWidth(a, std::max(0.f, x1 - x0));
                    YGNodeStyleSetHeight(a, std::max(0.f, y1 - y0));
                    YGNodeStyleSetJustifyContent(a, YGJustifyFlexStart);
                    YGNodeStyleSetAlignItems(a, YGAlignFlexStart);
                    YGNodeCalculateLayout(a, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0), YGDirectionLTR);
                    g.positions[child.ordinal] = {x0 + YGNodeLayoutGetLeft(c.yoga), y0 + YGNodeLayoutGetTop(c.yoga)};
                }
            }

            static YGSize measure(YGNodeConstRef y, float width, YGMeasureMode wm, float height,
                                  YGMeasureMode hm) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    ++n.measure_calls;
                    const auto& text = n.engine->text;
                    // At most `width` wide, a text box fits its content (CSS Sizing 3 fit-content): no wider than its
                    // max-content width and no narrower than its min-content width, its widest unbreakable run, which
                    // overflows a narrower width (an absolutely positioned box in a containing block too narrow for
                    // it). Its lines are laid out at the width it takes.
                    const float natural_width = text.layout(n.index, unbounded).width;
                    if (wm == YGMeasureModeAtMost) {
                        if (const float minimum = std::min(text.min_content(n.index), natural_width); width < minimum) {
                            return {minimum,
                                    hm == YGMeasureModeExactly ? height : text.layout(n.index, minimum).height};
                        }
                    }
                    const auto measured = text.layout(n.index, wm == YGMeasureModeUndefined ? unbounded : width);
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

            static YGSize grid_measured(YGNodeConstRef y, float width, YGMeasureMode wm, float height,
                                        YGMeasureMode hm) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    const auto size = n.engine->lay_out_grid(n, width, wm, height, hm, false, layout_edges(y));
                    return {size[0], size[1]};
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
                prepare_grids();
                if (css()) {
                    known_heights();
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
                        // The grid at its final size places its children.
                        const auto edges = layout_edges(y);
                        (void)lay_out_grid(n, std::max(0.f, g.width - edges[0] - edges[2]), YGMeasureModeExactly,
                                           std::max(0.f, g.height - edges[1] - edges[3]), YGMeasureModeExactly, true,
                                           edges);
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
