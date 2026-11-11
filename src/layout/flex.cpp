#include "layout/flex.hpp"
#include "base/css_scalar.hpp"
#include "constants/layout.hpp"
#include "layout/grid.hpp"
#include <yoga/Yoga.h>
#include <algorithm>
#include <array>
#include <charconv>
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

        // A computed length in px as Blink takes it into layout: whole LayoutUnits, truncated toward zero
        // (LayoutUnit(float)). An em length computes to a px value between them.
        float layout_unit(float px) {
            return std::trunc(px * constants::layout_units_per_px) / constants::layout_units_per_px;
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

        // An intrinsic sizing keyword of a width (CSS Sizing 3 §3.2), or one of Chrome's -webkit- aliases of them.
        enum class Sizing : std::uint8_t {
            None,
            MinContent,
            MaxContent,
            FitContent,
        };

        Sizing sizing_keyword(std::string_view v) {
            // The trimmed text is a string of its own: a view of it must not outlive it.
            const std::string trimmed = css::trim(v);
            v = trimmed;
            if (v.starts_with("-webkit-")) {
                v.remove_prefix(std::string_view("-webkit-").size());
            }
            return v == "min-content"   ? Sizing::MinContent
                   : v == "max-content" ? Sizing::MaxContent
                   : v == "fit-content" ? Sizing::FitContent
                                        : Sizing::None;
        }

        Sizing sizing_keyword(const CssValue* v) {
            return v ? sizing_keyword(v->text()) : Sizing::None;
        }

        // A length, or auto for auto, none (max-width and max-height) and the sizing keywords, which take a box's
        // intrinsic sizes once they are known (Engine::resolve_sizing).
        Length length(std::string_view input) {
            if (input.empty() || input == "auto" || input == "none" || sizing_keyword(input) != Sizing::None) {
                return {0, false, true};
            }
            const auto [digits, unit] = css::split_unit(input);
            if (unit == css::Unit::Px) {
                return {layout_unit(number(digits))};
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
                    return {d->unit == Unit::Percent ? *f : layout_unit(*f), d->unit == Unit::Percent};
                }
            }
            return length(v->text());
        }

        // An svg width or height attribute: a length whose unitless numbers are px, and whose em are the element's
        // font size (style: its computed values).
        Length presentation(std::string_view input, const Style& style) {
            const auto text = css::trim(input);
            if (text.empty() || text == "auto") {
                return {0, false, true};
            }
            if (text.ends_with('%')) {
                return {number(std::string_view(text).substr(0, text.size() - 1)), true};
            }
            if (text.ends_with("em") && !text.ends_with("rem")) {
                const auto* size = style.find(P::FontSize);
                const auto l = size ? length(size) : Length{float(constants::default_font_size_px)};
                if (l.automatic || l.percent) {
                    throw std::invalid_argument("WEFT_CSS_VALUE: font-size of an svg sized in em");
                }
                return {number(std::string_view(text).substr(0, text.size() - 2)) * l.value};
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

        // An img's sizes beyond its box's (LayoutBox::image): its preferred aspect ratio (CSS Sizing 4 §5.1) and its
        // minimum and maximum sizes, in content-box px. The ratio is its natural one, or aspect-ratio's (a <ratio> of
        // its own over the natural one, of the box box-sizing names; auto && <ratio> only without a natural ratio, of
        // the content box), with what the ratio's box adds to the content box: content width w goes with content
        // height (w + add[0]) / ratio - add[1].
        struct ImageSizing {
            float ratio{};
            std::array<float, 2> add{};
            std::array<float, 2> min{};
            std::array<float, 2> max{unbounded, unbounded};
            // Its natural size, 0 x 0 without an image.
            std::array<float, 2> natural{};

            float height_of(float w) const {
                return std::max(0.f, (w + add[0]) / ratio - add[1]);
            }

            float width_of(float h) const {
                return std::max(0.f, (h + add[1]) * ratio - add[0]);
            }

            float clamp(std::size_t axis, float v) const {
                return std::max(min[axis], std::min(v, std::max(min[axis], max[axis])));
            }

            // The used content size: each axis given (exact, after its minimum and maximum) or auto. One auto axis
            // follows the other through the ratio, else takes the natural size; with both auto, the natural width
            // and the height at the ratio, within the minimums and maximums as CSS 2 §10.4's table keeps the ratio.
            std::array<float, 2> size(std::optional<float> width, std::optional<float> height) const {
                if (width && height) {
                    return {clamp(0, *width), clamp(1, *height)};
                }
                if (width) {
                    const float w = clamp(0, *width);
                    return {w, clamp(1, ratio > 0 ? height_of(w) : natural[1])};
                }
                if (height) {
                    const float h = clamp(1, *height);
                    return {clamp(0, ratio > 0 ? width_of(h) : natural[0]), h};
                }
                if (ratio <= 0) {
                    return {clamp(0, natural[0]), clamp(1, natural[1])};
                }
                const float w = natural[0];
                const float h = height_of(w);
                const float max_w = std::max(min[0], max[0]);
                const float max_h = std::max(min[1], max[1]);
                const bool wide = w > max_w;
                const bool narrow = w < min[0];
                const bool tall = h > max_h;
                const bool low = h < min[1];
                if (wide && tall) {
                    return max_w / w <= max_h / h ? std::array{max_w, std::max(min[1], height_of(max_w))}
                                                  : std::array{std::max(min[0], width_of(max_h)), max_h};
                }
                if (narrow && low) {
                    return min[0] / w <= min[1] / h ? std::array{std::min(max_w, width_of(min[1])), min[1]}
                                                    : std::array{min[0], std::min(max_h, height_of(min[0]))};
                }
                if (narrow && tall) {
                    return {min[0], max_h};
                }
                if (wide && low) {
                    return {max_w, min[1]};
                }
                if (wide) {
                    return {max_w, std::max(height_of(max_w), min[1])};
                }
                if (narrow) {
                    return {min[0], std::min(height_of(min[0]), max_h)};
                }
                if (tall) {
                    return {std::max(width_of(max_h), min[0]), max_h};
                }
                if (low) {
                    return {std::min(width_of(min[1]), max_w), min[1]};
                }
                return {w, h};
            }
        };

        // An edge's px: a length, or a percentage of basis (none: 0).
        float edge_px(const Style& s, PropertyId key, PropertyId shorthand, std::optional<float> basis) {
            const auto* v = s.find(key);
            if (!v) {
                v = s.find(shorthand);
            }
            const auto l = length(v);
            if (l.automatic) {
                return 0;
            }
            return l.percent ? (basis ? *basis * l.value / constants::percent_scale : 0) : l.value;
        }

        // The padding and borders of a box across and down, its percentages of basis (none: 0).
        std::array<float, 2> image_edges(const Style& s, std::optional<float> basis) {
            return {edge_px(s, P::PaddingLeft, P::Padding, basis) + edge_px(s, P::PaddingRight, P::Padding, basis) +
                        edge_px(s, P::BorderLeftWidth, P::BorderWidth, basis) +
                        edge_px(s, P::BorderRightWidth, P::BorderWidth, basis),
                    edge_px(s, P::PaddingTop, P::Padding, basis) + edge_px(s, P::PaddingBottom, P::Padding, basis) +
                        edge_px(s, P::BorderTopWidth, P::BorderWidth, basis) +
                        edge_px(s, P::BorderBottomWidth, P::BorderWidth, basis)};
        }

        // An img's sizing: box its layout box, s its values; basis the width its percentages resolve against (none: a
        // percentage width is auto, a percentage minimum 0 and maximum none). Heights' percentages resolve against
        // nothing here.
        ImageSizing image_sizing(const LayoutBox& box, const Style& s, std::optional<float> basis) {
            ImageSizing out;
            out.natural = box.natural.value_or(std::array<float, 2>{});
            const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
            const auto edges = image_edges(s, basis);
            const auto words = css::split(css::trim(s.text(P::AspectRatio, "auto")), ' ').parts;
            bool automatic{};
            std::string ratio_text;
            for (const auto& w : words) {
                if (w == "auto") {
                    automatic = true;
                } else {
                    ratio_text += w;
                }
            }
            float specified{};
            if (!ratio_text.empty()) {
                const auto slash = ratio_text.find('/');
                const float a = number(std::string_view(ratio_text).substr(0, slash));
                const float b =
                    slash == std::string::npos ? 1.f : number(std::string_view(ratio_text).substr(slash + 1));
                if (a > 0 && b > 0) {
                    specified = a / b;
                }
            }
            if (specified > 0 && (!automatic || box.ratio <= 0)) {
                out.ratio = specified;
                if (!automatic && border_box) {
                    out.add = edges;
                }
            } else {
                out.ratio = box.ratio > 0 ? box.ratio : 0.f;
            }
            const auto bound = [&](PropertyId key, std::size_t axis, float fallback) {
                const auto l = length(s.find(key));
                if (l.automatic || (l.percent && (axis == 1 || !basis))) {
                    return fallback;
                }
                const float v = l.percent ? *basis * l.value / constants::percent_scale : l.value;
                return std::max(0.f, v - (border_box ? edges[axis] : 0));
            };
            out.min = {bound(P::MinWidth, 0, 0), bound(P::MinHeight, 1, 0)};
            out.max = {bound(P::MaxWidth, 0, unbounded), bound(P::MaxHeight, 1, unbounded)};
            return out;
        }

        // An img's specified content width or height: a length (of the box box-sizing names), a percentage of basis
        // (a width's; none: auto), or auto.
        std::optional<float> image_specified(const Style& s, std::size_t axis, std::optional<float> basis) {
            const auto l = length(s.find(axis ? P::Height : P::Width));
            if (l.automatic || (l.percent && (axis == 1 || !basis))) {
                return std::nullopt;
            }
            const float v = l.percent ? *basis * l.value / constants::percent_scale : l.value;
            const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
            return std::max(0.f, v - (border_box ? image_edges(s, basis)[axis] : 0));
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
            // Its values with the intrinsic sizing keywords of its widths resolved to px (resolve_sizing).
            std::unique_ptr<Style> resolved;
            // A grid child's own Yoga root, a column flex box that holds it: as large as its grid area (or the grid's
            // padding box, for an absolutely positioned child), so that Yoga sizes, stretches and aligns it there.
            YGNodeRef area{};
            // Border-box origins of a grid's children in its border box, in child order.
            std::vector<std::array<float, 2>> positions;
            // Intrinsic border-box sizes, and why one is unknown. min_width, max_width and intrinsic_height are what
            // the box contributes to its parent's intrinsic sizes (CSS Sizing 3 §5.1): its px size where it states one,
            // else its content's, within its px minimum and maximum. auto_min_width and auto_min_height are its
            // content's alone, the content size suggestion of its automatic minimum size as a flex item (CSS Flexbox 1
            // §4.5), which its own size caps there instead.
            float min_width{};
            float max_width{};
            float intrinsic_height{};
            float auto_min_width{};
            float auto_min_height{};
            std::string unknown_width;
            std::string unknown_height;
            std::size_t measure_calls{};
            // The width an img's percentages resolve against, where the adapter knows it before Yoga measures the img
            // (its grid area's, its parent's px content width); else measure_image takes it from what Yoga gives.
            std::optional<float> containing_width;
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
                        if (v != "auto" && sizing_keyword(v) == Sizing::None) {
                            dim(YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent);
                        }
                    } else if (id == P::MinHeight) {
                        if (v != "auto") {
                            dim(YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent);
                        }
                    } else if (id == P::MaxWidth) {
                        dim(YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent, [](YGNodeRef node) {
                            YGNodeStyleSetMaxWidth(node, YGUndefined);
                        });
                    } else if (id == P::MaxHeight) {
                        dim(YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent, [](YGNodeRef node) {
                            YGNodeStyleSetMaxHeight(node, YGUndefined);
                        });
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
                // direction (CSS Writing Modes 4 §2.1): Yoga lays the box's rows and the cross axis of its columns
                // out from that side; physical edges (margins, padding, insets) stay where they are, and an
                // absolutely positioned box over-constrained by left and right keeps its start inset, as CSS 2 §10.3.7
                // keeps right in a right-to-left containing block.
                YGNodeStyleSetDirection(y, s.text(P::Direction, "ltr") == "rtl" ? YGDirectionRTL : YGDirectionLTR);
                // An absolutely positioned box over-constrained across (left, width and right, neither margin auto):
                // CSS 2 §10.3.7 ignores right in a left-to-right containing block and left in a right-to-left one.
                // Yoga keeps the start inset of the box's parent's direction, which is another box when the containing
                // block is further up. An auto margin takes the space instead, between both insets (Yoga patch 0007).
                const auto automatic_margin = [&](PropertyId side) {
                    return s.text(side, s.text(P::Margin, "0")) == "auto";
                };
                if (s.text(P::Position, "static") == "absolute" && s.text(P::Left, "auto") != "auto" &&
                    s.text(P::Right, "auto") != "auto" && !length(s.find(P::Width)).automatic &&
                    !automatic_margin(P::MarginLeft) && !automatic_margin(P::MarginRight)) {
                    YGNodeStyleSetPositionAuto(y, containing_rtl(n) ? YGEdgeLeft : YGEdgeRight);
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

            // Whether an absolutely positioned box's containing block is right to left: its nearest ancestor that
            // contains absolute boxes (positioned or transformed), else the layout's root.
            bool containing_rtl(const Node& n) const {
                auto parent = n.box->parent;
                while (parent != LayoutBox::root) {
                    const auto& a = nodes[parent];
                    if (contains_absolute(*a.style) || a.box->parent == LayoutBox::root) {
                        return a.style->text(P::Direction, "ltr") == "rtl";
                    }
                    parent = a.box->parent;
                }
                return n.style->text(P::Direction, "ltr") == "rtl";
            }

            // An svg's width and height attributes stand in for absent properties. With an intrinsic ratio and an
            // auto or percentage height in a parent of indefinite height, the height follows the width.
            void replace(Node& n) {
                const auto& box = *n.box;
                const auto& s = *n.style;
                if (box.image) {
                    replace_image(n);
                    return;
                }
                for (const bool width : {true, false}) {
                    if (s.contains(width ? P::Width : P::Height)) {
                        continue;
                    }
                    const auto attribute = width ? box.replaced_width : box.replaced_height;
                    const auto l = attribute.empty() ? Length{width ? constants::layout::default_object_width_px
                                                                    : constants::layout::default_object_height_px}
                                                     : presentation(attribute, s);
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

            // An img: its content size as CSS sizes a replaced box (ImageSizing), from the constraints Yoga gives
            // (measure_image). The root of a run shrunk to fit (an inline img) keeps the size it has in its
            // containing block, whose width its percentages resolve against; an absolutely positioned one keeps its
            // own size between insets on both sides, instead of stretching. A block-level one is not stretched.
            void replace_image(Node& n) {
                const auto& s = *n.style;
                const bool absolute = s.text(P::Position, "static") == "absolute";
                const auto both = [&](PropertyId a, PropertyId b) {
                    return s.text(a, "auto") != "auto" && s.text(b, "auto") != "auto";
                };
                const bool root = !n.index && options.fit_width;
                if (root || (absolute && (both(P::Left, P::Right) || both(P::Top, P::Bottom)))) {
                    const auto basis = root ? options.fit_width : std::nullopt;
                    const auto size =
                        image_sizing(*n.box, s, basis).size(image_specified(s, 0, basis), image_specified(s, 1, basis));
                    const auto edges = image_edges(s, basis);
                    const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
                    if (root || both(P::Left, P::Right) || !length(s.find(P::Width)).automatic) {
                        YGNodeStyleSetWidth(n.yoga, size[0] + (border_box ? edges[0] : 0));
                    }
                    if (root || both(P::Top, P::Bottom) || !length(s.find(P::Height)).automatic) {
                        YGNodeStyleSetHeight(n.yoga, size[1] + (border_box ? edges[1] : 0));
                    }
                    if (root) {
                        return;
                    }
                }
                YGNodeSetMeasureFunc(n.yoga, measure_image);
                if (!n.index || absolute) {
                    return;
                }
                const auto& parent = nodes[n.box->parent];
                n.containing_width = definite_content_width(parent);
                if (parent.formatting == Formatting::Block) {
                    YGNodeStyleSetAlignSelf(n.yoga, YGAlignFlexStart);
                }
                // A flex item of a row, of auto basis and width: its flex base size is its width at its definite
                // height (CSS Flexbox 1 §9.2 3B), else its content width (§9.2 3E). Yoga's would follow the height
                // its line gives it in a container whose height is not definite.
                if (parent.formatting == Formatting::Flex && row_direction(*parent.style) && automatic_basis(s) &&
                    length(s.find(P::Width)).automatic) {
                    const float width = image_sizing(*n.box, s, std::nullopt).size(std::nullopt, row_item_height(n))[0];
                    const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
                    YGNodeStyleSetFlexBasis(n.yoga, width + (border_box ? image_edges(s, std::nullopt)[0] : 0));
                }
            }

            // A box's content width where it is known before layout: its px width less its padding and borders
            // (box-sizing border-box), or, for an in-flow block-level box of auto width in a block, its parent's less
            // its px margins, padding and borders; the root's from the viewport or the width it is fit to. None
            // otherwise (a percentage, a shrink-to-fit or flex-sized box).
            std::optional<float> definite_content_width(const Node& n) const {
                const auto& s = *n.style;
                const auto edges = [&] {
                    return image_edges(s, std::nullopt)[0];
                };
                if (const auto w = length(s.find(P::Width)); !w.automatic) {
                    if (w.percent) {
                        return std::nullopt;
                    }
                    return std::max(0.f, w.value - (s.text(P::BoxSizing, "content-box") == "border-box" ? edges() : 0));
                }
                if (!n.index) {
                    if (options.viewport) {
                        return std::max(0.f, (*options.viewport)[0] - edges());
                    }
                    return std::nullopt;
                }
                const auto& parent = nodes[n.box->parent];
                const auto display = s.text(P::Display, "block");
                if (parent.formatting != Formatting::Block || s.text(P::Position, "static") == "absolute" ||
                    (display != "block" && display != "flex" && display != "grid")) {
                    return std::nullopt;
                }
                const auto inner = definite_content_width(parent);
                if (!inner) {
                    return std::nullopt;
                }
                const auto margin = [&](PropertyId side) {
                    const auto l = length(s.find(side) ? s.find(side) : s.find(P::Margin));
                    return l.automatic || l.percent ? 0.f : l.value;
                };
                return std::max(0.f, *inner - margin(P::MarginLeft) - margin(P::MarginRight) - edges());
            }

            // An img flex item's definite content height in a row: its own (px), or the one stretching gives it in a
            // single-line container of definite (px) height (CSS Flexbox 1 §9.8); none otherwise.
            std::optional<float> row_item_height(const Node& n) const {
                const auto& s = *n.style;
                if (auto height = image_specified(s, 1, std::nullopt)) {
                    return height;
                }
                const auto& parent = nodes[n.box->parent];
                const auto& ps = *parent.style;
                const auto own = css::trim(s.text(P::AlignSelf, "auto"));
                const auto align = own == "auto" ? css::trim(ps.text(P::AlignItems, "normal")) : own;
                const auto automatic_margin = [&](PropertyId side) {
                    return s.text(side, s.text(P::Margin, "0")) == "auto";
                };
                const auto parent_height = length(ps.find(P::Height));
                if ((align != "stretch" && align != "normal") || !length(s.find(P::Height)).automatic ||
                    automatic_margin(P::MarginTop) || automatic_margin(P::MarginBottom) ||
                    ps.text(P::FlexWrap, "nowrap") != "nowrap" || parent_height.automatic || parent_height.percent) {
                    return std::nullopt;
                }
                const float inner =
                    parent_height.value -
                    (ps.text(P::BoxSizing, "content-box") == "border-box" ? image_edges(ps, std::nullopt)[1] : 0);
                const float margins = edge_px(s, P::MarginTop, P::Margin, std::nullopt) +
                                      edge_px(s, P::MarginBottom, P::Margin, std::nullopt);
                return image_sizing(*n.box, s, std::nullopt)
                    .clamp(1, std::max(0.f, inner - margins - image_edges(s, std::nullopt)[1]));
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
                            // An item with an auto margin across is not stretched (CSS Flexbox 1 §8.1): it keeps its
                            // fit-content size there, where the margins place it. Yoga would stretch it to size its
                            // basis.
                            const auto& s = *n.style;
                            const auto automatic = [&](PropertyId side) {
                                return s.text(side, s.text(P::Margin, "0")) == "auto";
                            };
                            if (row_direction(*parent.style) ? automatic(P::MarginTop) || automatic(P::MarginBottom)
                                                             : automatic(P::MarginLeft) || automatic(P::MarginRight)) {
                                const auto own = css::trim(s.text(P::AlignSelf, "auto"));
                                const auto aligned =
                                    own == "auto" ? css::trim(parent.style->text(P::AlignItems, "normal")) : own;
                                if (aligned == "stretch" || aligned == "normal") {
                                    YGNodeStyleSetAlignSelf(n.yoga, YGAlignFlexStart);
                                }
                            }
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
            // the max-content width, as automatic minimum sizes need them. A container's are its children's
            // contributions, each child's margin box (CSS Sizing 3 §5.1): a child of a px width contributes that width
            // whatever it holds, so the min-content width of a box around a block 140px wide is no less than 140px.
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
                } else if (n.box->image) {
                    // Its size with percentages unresolved: its min-content width is its padding and borders alone
                    // when a percentage sizes its width (a compressible replaced box, CSS Sizing 3 §5.2.2).
                    unknown_w.clear();
                    unknown_h.clear();
                    const auto edges_image = image_edges(s, std::nullopt);
                    const auto size =
                        image_sizing(*n.box, s, std::nullopt)
                            .size(image_specified(s, 0, std::nullopt), image_specified(s, 1, std::nullopt));
                    n.max_width = size[0] + edges_image[0];
                    n.intrinsic_height = size[1] + edges_image[1];
                    n.min_width = length(s.find(P::Width)).percent || length(s.find(P::MaxWidth)).percent
                                      ? edges_image[0]
                                      : n.max_width;
                } else if (n.box->replaced) {
                    const auto size = [&](bool width) {
                        const auto* v = s.find(width ? P::Width : P::Height);
                        const auto attribute = width ? n.box->replaced_width : n.box->replaced_height;
                        const auto l = v                   ? length(v)
                                       : attribute.empty() ? Length{width ? constants::layout::default_object_width_px
                                                                          : constants::layout::default_object_height_px}
                                                           : presentation(attribute, s);
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
                                                 : presentation(n.box->replaced_width, s);
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
                        // A child contributes its margin box (CSS Sizing 3 §5.1), negative margins too; auto and
                        // percentage margins are 0 here. Margins down do not collapse, as Yoga lays them out.
                        const float margins = intrinsic_margins(*c.style, true);
                        const float height = c.intrinsic_height + intrinsic_margins(*c.style, false);
                        if (row && nowrap) {
                            n.min_width += c.min_width + margins;
                        } else {
                            n.min_width = std::max(n.min_width, c.min_width + margins);
                        }
                        if (row) {
                            n.max_width += c.max_width + margins;
                            n.intrinsic_height = std::max(n.intrinsic_height, height);
                        } else {
                            n.max_width = std::max(n.max_width, c.max_width + margins);
                            n.intrinsic_height += height;
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
                    // Negative margins never make a content size negative.
                    n.min_width = std::max(0.f, n.min_width) + edges;
                    n.max_width = std::max(0.f, n.max_width) + edges;
                    n.intrinsic_height = std::max(0.f, n.intrinsic_height) + edges_v;
                }
                // So far these are the content's sizes with the box's padding and borders: its automatic minimum size
                // as a flex item starts from them (automatic_minimum). What the box contributes to its parent's
                // intrinsic sizes is its own px size where it states one, then within its px maximum and minimum (CSS
                // Sizing 3 §5.1; a percentage is auto there). A box-sizing: border-box size is no smaller than the
                // box's padding and borders.
                n.auto_min_width = n.min_width;
                n.auto_min_height = n.intrinsic_height;
                const auto stated = [&](PropertyId key, float box_edges) -> std::optional<float> {
                    const auto l = length(s.find(key));
                    if (l.automatic || l.percent) {
                        return std::nullopt;
                    }
                    return content_box ? l.value + box_edges : std::max(l.value, box_edges);
                };
                const auto width = stated(P::Width, edges);
                // min-content and max-content, the keywords of the box's widths, are its content's widths, within its
                // px width. Each of width, min-width and max-width is then the width its keyword resolved to, or its
                // px length.
                auto widths = resolve_sizing(n, width ? std::min(n.min_width, *width) : n.min_width,
                                             width ? *width : n.max_width);
                std::optional<float> height;
                std::optional<float> min_height;
                std::optional<float> max_height;
                // An img's sizes are its used size already, from its px sizes, minimums, maximums and ratio; a
                // percentage width or max-width leaves it no min-content width, whatever its width attribute says.
                if (!n.box->image) {
                    widths[0] = widths[0] ? widths[0] : width;
                    widths[1] = widths[1] ? widths[1] : stated(P::MinWidth, edges);
                    widths[2] = widths[2] ? widths[2] : stated(P::MaxWidth, edges);
                    height = stated(P::Height, edges_v);
                    min_height = stated(P::MinHeight, edges_v);
                    max_height = stated(P::MaxHeight, edges_v);
                }
                // The maximum, then the minimum: a minimum over a maximum wins (CSS 2 §10.4).
                const auto within = [](float size, const std::optional<float>& maximum,
                                       const std::optional<float>& minimum) {
                    return std::max(std::min(size, maximum.value_or(unbounded)), minimum.value_or(0.f));
                };
                n.min_width = within(widths[0].value_or(n.min_width), widths[2], widths[1]);
                n.max_width = within(widths[0].value_or(n.max_width), widths[2], widths[1]);
                n.intrinsic_height = within(height.value_or(n.intrinsic_height), max_height, min_height);
            }

            // The intrinsic sizing keywords of a box's widths, now that its intrinsic widths are known (content_min and
            // content_max, border box): min-content and max-content make the width (or min-width, max-width) that
            // border-box width, for Yoga and for every read after (the box's values, resolved); fit-content is the
            // shrink-to-fit of auto where a box shrinks to fit (the root of an atomic inline, an absolutely positioned
            // box), else a diagnostic. Returns the border-box width each of width, min-width and max-width resolved
            // to, where it is min-content or max-content.
            std::array<std::optional<float>, 3> resolve_sizing(Node& n, float content_min, float content_max) {
                const auto& s = *n.style;
                const std::array<std::pair<PropertyId, Sizing>, 3> keywords{
                    {{P::Width, sizing_keyword(s.find(P::Width))},
                     {P::MinWidth, sizing_keyword(s.find(P::MinWidth))},
                     {P::MaxWidth, sizing_keyword(s.find(P::MaxWidth))}}};
                std::array<std::optional<float>, 3> widths;
                if (std::all_of(keywords.begin(), keywords.end(), [](const auto& k) {
                        return k.second == Sizing::None;
                    })) {
                    return widths;
                }
                const bool content_box = s.text(P::BoxSizing, "content-box") == "content-box";
                auto resolved = std::make_unique<Style>(s);
                for (std::size_t k = 0; k < keywords.size(); ++k) {
                    const auto& [id, keyword] = keywords[k];
                    if (keyword == Sizing::None) {
                        continue;
                    }
                    if (keyword == Sizing::FitContent) {
                        if (n.index && s.text(P::Position) != "absolute") {
                            throw std::invalid_argument("WEFT_UNSUPPORTED_UNIT: fit-content " +
                                                        std::string(property_name(id)) + " of a box in flow");
                        }
                        resolved->erase(id);
                        continue;
                    }
                    const float border = keyword == Sizing::MinContent ? content_min : content_max;
                    const float value = std::max(0.f, content_box ? border - horizontal(s) : border);
                    char buffer[32];
                    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
                    resolved->set(id, std::string(buffer, written.ptr) + "px");
                    widths[k] = border;
                    if (id == P::Width) {
                        YGNodeStyleSetWidth(n.yoga, value);
                    } else if (id == P::MinWidth) {
                        YGNodeStyleSetMinWidth(n.yoga, value);
                    } else {
                        YGNodeStyleSetMaxWidth(n.yoga, value);
                    }
                }
                n.resolved = std::move(resolved);
                n.style = n.resolved.get();
                return widths;
            }

            // A row flex container's content width where the percentages of its items' widths resolve before layout:
            // known (definite_content_width) and not flexed itself, as a row's item is.
            std::optional<float> percentage_basis(const Node& container) const {
                if (container.index && nodes[container.box->parent].formatting == Formatting::Flex &&
                    row_direction(*nodes[container.box->parent].style)) {
                    return std::nullopt;
                }
                return definite_content_width(container);
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
            // definite specified or maximum size: a px one, or a percentage of a container size known before layout
            // (one known only in layout does not cap it). Wrapping text in a column takes its height at its width,
            // definite from the item or its parent. When layout gives the width, an automatic basis and height make
            // the basis that same content height, the minimum too: the item does not shrink, without a second layout.
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
                // An img in a row: its content size suggestion is its width without its own width property (natural,
                // or from its definite height through its ratio) within a px max-width; a px width (the specified
                // size suggestion) caps it (CSS Flexbox 1 §4.5). Percentages resolve against the container's px
                // width; of another, a percentage width or max-width caps it by a width known only in layout: then it
                // has none.
                if (n.box->image && main_row) {
                    const auto basis = percentage_basis(parent);
                    // The content size suggestion within px bounds, a px width capping it; then a percentage width or
                    // max-width of that width. Without it, a percentage width leaves no minimum (the img is that
                    // wide), a percentage max-width is taken as not capping (it would widen a narrower img).
                    auto sizing = image_sizing(*n.box, s, std::nullopt);
                    sizing.min[0] = 0;
                    float minimum = sizing.size(std::nullopt, row_item_height(n))[0];
                    if (const auto width = image_specified(s, 0, std::nullopt)) {
                        minimum = std::min(minimum, *width);
                    }
                    const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
                    const float edges = image_edges(s, basis)[0];
                    for (const auto key : {P::Width, P::MaxWidth}) {
                        if (const auto l = length(s.find(key)); l.percent) {
                            if (basis) {
                                minimum = std::min(minimum, std::max(0.f, l.value / constants::percent_scale * *basis -
                                                                              (border_box ? edges : 0)));
                            } else if (key == P::Width) {
                                minimum = 0;
                            }
                        }
                    }
                    YGNodeStyleSetMinWidth(n.yoga, minimum + (border_box ? edges : 0));
                    return;
                }
                // An img in a column of auto basis and height takes its content height as its basis, which it does not
                // shrink below (as a box does, below). With a height, its content size suggestion is its height
                // without its own height property (natural, or from its definite width through its ratio) within a
                // px max-height; the px height caps it.
                if (n.box->image && !(automatic_basis(s) && length(s.find(P::Height)).automatic)) {
                    const auto basis = definite_content_width(parent);
                    auto sizing = image_sizing(*n.box, s, basis);
                    sizing.min[1] = 0;
                    auto width = image_specified(s, 0, basis);
                    const auto own = css::trim(s.text(P::AlignSelf, "auto"));
                    const auto align = own == "auto" ? css::trim(parent.style->text(P::AlignItems, "normal")) : own;
                    const auto automatic_margin = [&](PropertyId side) {
                        return s.text(side, s.text(P::Margin, "0")) == "auto";
                    };
                    if (!width && basis && (align == "stretch" || align == "normal") &&
                        !automatic_margin(P::MarginLeft) && !automatic_margin(P::MarginRight)) {
                        const float margins =
                            edge_px(s, P::MarginLeft, P::Margin, basis) + edge_px(s, P::MarginRight, P::Margin, basis);
                        width = sizing.clamp(0, std::max(0.f, *basis - margins - image_edges(s, basis)[0]));
                    }
                    float minimum = sizing.size(width, std::nullopt)[1];
                    if (const auto height = image_specified(s, 1, basis)) {
                        minimum = std::min(minimum, *height);
                    }
                    const bool border_box = s.text(P::BoxSizing, "content-box") == "border-box";
                    YGNodeStyleSetMinHeight(n.yoga, minimum + (border_box ? image_edges(s, basis)[1] : 0));
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
                // The content size suggestion (CSS Flexbox 1 §4.5): the item's min-content size, its content's alone. A
                // block of a px size in it counts whatever it holds; the item's own size does not, it is the next
                // suggestion.
                float minimum = main_row ? n.auto_min_width : n.auto_min_height;
                // What a percentage of the item's main size or maximum is of, where it is known before layout: a row's
                // content width (percentage_basis), a column's content height (known_heights); and the border-box
                // size a percentage of it gives the item.
                std::optional<float> basis;
                if (main_row) {
                    basis = percentage_basis(parent);
                } else if (parent.known_height) {
                    std::string unknown_edges;
                    const float edges = intrinsic_vertical(*parent.style, unknown_edges);
                    if (unknown_edges.empty()) {
                        basis = std::max(0.f, *parent.known_height - edges);
                    }
                }
                const auto of_basis = [&](const Length& l) {
                    const float edges = image_edges(s, main_row ? basis : std::nullopt)[main_row ? 0 : 1];
                    const float size = l.value / constants::percent_scale * *basis;
                    return content_box ? size + edges : std::max(size, edges);
                };
                const auto maximum = length(s.find(main_row ? P::MaxWidth : P::MaxHeight));
                // A percentage max-width of a row whose width only layout gives cannot cap a minimum set before it.
                // An item whose content cannot narrow (its min-content width is its max-content width), of automatic
                // basis and width, needs no minimum there: its basis is its content's width and its minimum that width
                // within the maximum, its hypothetical size. It does not shrink, and Yoga resolves the maximum.
                if (main_row && maximum.percent && !basis && unknown.empty() && specified.automatic &&
                    automatic_basis(s) && n.auto_min_width >= n.max_width) {
                    YGNodeStyleSetFlexShrink(n.yoga, 0);
                    return;
                }
                // The specified size suggestion, the most the content-based minimum can be: a px size (what the item
                // contributes to its container's intrinsic size: with an automatic minimum, that size within its
                // maximum), a percentage of the row's width, or a percentage of a height known before layout.
                const auto& main_size = main_row ? specified : specified_height;
                std::optional<float> suggestion;
                if (!main_size.automatic && !main_size.percent) {
                    suggestion = main_row ? n.max_width : n.intrinsic_height;
                } else if (main_row && main_size.percent && basis) {
                    suggestion = of_basis(main_size);
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
                // The maximum caps it: a px size, or a percentage of a size known before layout.
                if (!maximum.automatic && !maximum.percent) {
                    const float max_border =
                        maximum.value + (content_box ? (main_row ? horizontal(s) : vertical(s)) : 0);
                    minimum = std::min(minimum, max_border);
                } else if (maximum.percent && basis) {
                    minimum = std::min(minimum, of_basis(maximum));
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
            // rtl: the grid's direction, from whose side the item justifies itself.
            static void lay_out_item(Node& c, float width, float height, grid::Align justify, grid::Align align,
                                     bool rtl) {
                const auto a = c.area;
                const auto y = c.yoga;
                const auto& s = *c.style;
                const bool definite = std::isfinite(height);
                width = std::max(0.f, width);
                c.containing_width = width;
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
                YGNodeCalculateLayout(a, width, definite ? std::max(0.f, height) : YGUndefined,
                                      rtl ? YGDirectionRTL : YGDirectionLTR);
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
                // direction: a right-to-left grid lays its columns out from the right (CSS Grid 1 §7.1 with CSS
                // Writing Modes 4): an x of the left-to-right layout, mirrored in the content box.
                const bool rtl = s.text(P::Direction, "ltr") == "rtl";
                const auto mirror = [left = edges[0], content_width](float x) {
                    return 2 * left + content_width - x;
                };
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
                                 justify[i], align[i], rtl);
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
                        const float span = span_size(column_lines, columns, area.column, area.column_span);
                        lay_out_item(c, span, span_size(row_lines, rows, area.row, area.row_span), justify[i], align[i],
                                     rtl);
                        // A right-to-left grid's columns run from its content box's right edge.
                        const float start = rtl ? mirror(column_lines[area.column] + span) : column_lines[area.column];
                        g.positions[state.items[i].ordinal] = {start + YGNodeLayoutGetLeft(c.yoga),
                                                               row_lines[area.row] + YGNodeLayoutGetTop(c.yoga) +
                                                                   shims[i]};
                    }
                    place_absolute(g, {column_lines, columns, placed.columns_before, explicit_columns.size()},
                                   {row_lines, rows, placed.rows_before, explicit_rows.size()}, rtl, edges[0],
                                   content_width);
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
            // rtl: a right-to-left grid, whose left-to-right track lines mirror in its content box (from content_left,
            // content_width wide).
            void place_absolute(Node& g, const GridAxis& across, const GridAxis& down, bool rtl, float content_left,
                                float content_width) {
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
                    float x0 = contains ? across.edge(p.column_start, false, left) : left;
                    float x1 = contains ? across.edge(p.column_end, true, right) : right;
                    if (contains && rtl) {
                        const auto mirror = [&](float x) {
                            return 2 * content_left + content_width - x;
                        };
                        // Right to left: the start line on the right, the end line on the left; auto at the padding
                        // edges on those sides.
                        const float none = std::numeric_limits<float>::quiet_NaN();
                        const float start = across.edge(p.column_start, false, none);
                        const float end = across.edge(p.column_end, true, none);
                        x0 = std::isnan(end) ? left : mirror(end);
                        x1 = std::isnan(start) ? right : mirror(start);
                    }
                    const float y0 = contains ? down.edge(p.row_start, false, top) : top;
                    const float y1 = contains ? down.edge(p.row_end, true, bottom) : bottom;
                    const auto a = c.area;
                    c.containing_width = std::max(0.f, x1 - x0);
                    YGNodeStyleSetWidth(a, std::max(0.f, x1 - x0));
                    YGNodeStyleSetHeight(a, std::max(0.f, y1 - y0));
                    YGNodeStyleSetJustifyContent(a, YGJustifyFlexStart);
                    YGNodeStyleSetAlignItems(a, YGAlignFlexStart);
                    YGNodeCalculateLayout(a, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0),
                                          rtl ? YGDirectionRTL : YGDirectionLTR);
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

            // An img's content size under Yoga's constraints: an exact width or height is its own (Yoga resolved
            // the property, or stretches it), the other follows (ImageSizing::size). An available width is no
            // constraint (a replaced box overflows it) but gives its containing block's width: the width its
            // percentage minimum and maximum resolve against.
            static YGSize measure_image(YGNodeConstRef y, float width, YGMeasureMode wm, float height,
                                        YGMeasureMode hm) noexcept {
                auto& n = *static_cast<Node*>(YGNodeGetContext(y));
                try {
                    ++n.measure_calls;
                    const auto& s = *n.style;
                    std::optional<float> basis = n.containing_width;
                    if (!basis && wm == YGMeasureModeAtMost && std::isfinite(width)) {
                        const float margins = edge_px(s, P::MarginLeft, P::Margin, std::nullopt) +
                                              edge_px(s, P::MarginRight, P::Margin, std::nullopt);
                        basis = width + margins + image_edges(s, std::nullopt)[0];
                    }
                    const auto size =
                        image_sizing(*n.box, s, basis)
                            .size(wm == YGMeasureModeExactly ? std::optional<float>(width) : std::nullopt,
                                  hm == YGMeasureModeExactly ? std::optional<float>(height) : std::nullopt);
                    return {size[0], size[1]};
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

            IntrinsicWidths root_intrinsic() {
                link();
                classify();
                create();
                prepare_grids();
                known_heights();
                for (auto i = nodes.size(); i-- > 0;) {
                    intrinsic(nodes[i]);
                }
                if (callback_error) {
                    std::rethrow_exception(callback_error);
                }
                if (!nodes[0].unknown_width.empty()) {
                    throw std::invalid_argument(nodes[0].unknown_width);
                }
                return {nodes[0].min_width, nodes[0].max_width};
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

    IntrinsicWidths layout_intrinsic_widths(std::span<const LayoutBox> boxes, const TextMeasurer& text) {
        Engine engine(boxes, text, {});
        return engine.root_intrinsic();
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
