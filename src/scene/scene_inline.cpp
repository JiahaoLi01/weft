#include "scene/scene_inline.hpp"

#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "constants/text.hpp"
#include "layout/inline_layout.hpp"
#include "scene/scene.hpp"
#include "scene/scene_values.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace weft {
    namespace {
        using constants::text::layout_units_per_px;
        // LayoutUnits: 1/64 px.
        using Units = std::int64_t;
        constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
        // U+FFFC OBJECT REPLACEMENT CHARACTER stands for an atomic inline in the paragraph text, as in Blink: UAX 14
        // allows breaks on both sides of it.
        constexpr std::string_view object_replacement = "\xEF\xBF\xBC";

        [[noreturn]] void fail(const std::string& s) {
            throw_diagnostic(Diag::SceneProfile, s);
        }

        // A length to LayoutUnits, truncated as Blink converts a computed length.
        Units to_units(float px) {
            return Units(px * layout_units_per_px);
        }

        // Summed advances to LayoutUnits, rounded up as a shape result's snapped width.
        Units snapped(float px) {
            return Units(std::ceil(px * layout_units_per_px));
        }

        // A laid-out size to LayoutUnits: layout gives LayoutUnit-exact floats.
        Units exact(float px) {
            return Units(std::lround(px * layout_units_per_px));
        }

        float pixels(Units u) {
            return float(u) / layout_units_per_px;
        }

        // A length in px that parses back to the same float.
        std::string px_text(float v) {
            char buffer[32];
            const auto written = std::to_chars(buffer, buffer + sizeof(buffer), v);
            return std::string(buffer, written.ptr) + "px";
        }

        std::string_view display(const SceneNode& n) {
            return n.style.text(PropertyId::Display, "inline");
        }

        bool out_of_flow(const SceneNode& n) {
            const auto p = n.style.text(PropertyId::Position, "static");
            return p == "absolute" || p == "fixed";
        }

        bool inline_level(const SceneNode& n) {
            if (n.tag == "#text") {
                return true;
            }
            const auto d = display(n);
            return d == "inline" || d == "inline-block" || d == "inline-flex" || d == "inline-grid" ||
                   d == "inline-table";
        }

        bool collapsible(char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        }

        // An edge length of a box: px, em of its font size, or a percentage of the containing block's width; in whole
        // LayoutUnits, truncated, as layout takes it.
        float edge(const SceneStyle& s, PropertyId id, float percent_base) {
            const auto v = s.text(id, "0");
            if (v == "auto") {
                return 0;
            }
            const auto size = scene_values::len(s.text(PropertyId::FontSize, constants::default_font_size),
                                                constants::default_font_size_px);
            const auto trimmed = css::trim(v);
            const float px =
                trimmed.ends_with('%') ? scene_values::len(trimmed, percent_base) : scene_values::len(trimmed, size);
            return std::trunc(px * layout_units_per_px) / layout_units_per_px;
        }

        // word-break (CSS Text 3 §5.2) of a text: its legacy break-word is normal, with overflow-wrap: anywhere.
        enum class WordBreak : std::uint8_t {
            Normal,
            BreakAll,
            KeepAll,
        };

        // An opportunity overflow-wrap gives inside a word (CSS Text 3 §5.5), taken only where the word overflows the
        // line it starts: break-word's leave the min-content width as it is, anywhere's narrow it.
        enum class Emergency : std::uint8_t {
            None,
            BreakWord,
            Anywhere,
        };

        // vertical-align (CSS 2.1 §10.8.1) of an inline-level box: a keyword, a length that raises it (px), or a
        // percentage of its own line-height that raises it.
        struct Align {
            enum class Kind : std::uint8_t {
                Baseline,
                Sub,
                Super,
                TextTop,
                TextBottom,
                Middle,
                Top,
                Bottom,
                Length,
                Percent,
            };

            Kind kind{};
            float value{};
        };

        Align vertical_align(const SceneStyle& s) {
            using K = Align::Kind;
            const auto v = css::trim(s.text(PropertyId::VerticalAlign, "baseline"));
            static constexpr std::array<std::pair<std::string_view, K>, 8> keywords{{{"baseline", K::Baseline},
                                                                                     {"sub", K::Sub},
                                                                                     {"super", K::Super},
                                                                                     {"text-top", K::TextTop},
                                                                                     {"text-bottom", K::TextBottom},
                                                                                     {"middle", K::Middle},
                                                                                     {"top", K::Top},
                                                                                     {"bottom", K::Bottom}}};
            for (const auto& [name, kind] : keywords) {
                if (v == name) {
                    return {kind, 0};
                }
            }
            const auto [digits, unit] = css::split_unit(v, {.px = true, .percent = true, .em = true});
            const auto n =
                css::parse_number<float>(std::string(digits), float(constants::scene::max_numeric_magnitude));
            if (!n || (unit == css::Unit::None && *n != 0)) {
                fail("vertical-align " + std::string(v) + " outside scene profile");
            }
            if (unit == css::Unit::Percent) {
                return {K::Percent, *n};
            }
            const float size = scene_values::len(s.text(PropertyId::FontSize, constants::default_font_size),
                                                 constants::default_font_size_px);
            return {K::Length, unit == css::Unit::Em ? *n * size : *n};
        }

        // A box's extent about its baseline (Blink's FontHeight): how far it reaches above and below it; empty until
        // something is united into it.
        struct Extent {
            float ascent = -std::numeric_limits<float>::infinity();
            float descent = -std::numeric_limits<float>::infinity();

            void unite(const Extent& e) {
                ascent = std::max(ascent, e.ascent);
                descent = std::max(descent, e.descent);
            }

            // Its baseline moved down by shift.
            void move(float shift) {
                ascent -= shift;
                descent += shift;
            }

            bool empty() const {
                return !std::isfinite(ascent) || !std::isfinite(descent);
            }
        };

        // A computed font-size in px.
        float computed_font_size(const SceneStyle& s) {
            return scene_values::len(s.text(PropertyId::FontSize, constants::default_font_size),
                                     constants::default_font_size_px);
        }

        // Px from LayoutUnits rounded as Blink rounds a float into one (LayoutUnit::FromFloatRound).
        float rounded_units(float px) {
            return std::round(px * layout_units_per_px) / layout_units_per_px;
        }

        // The inline-direction edges and the vertical padding and borders of an inline element.
        struct Box {
            const SceneNode* node{};
            // The inline box around this one; none at the root.
            std::size_t parent{};
            std::size_t depth{};
            SceneFont font;
            // Its computed font-size (vertical-align: sub and super of its children).
            float font_size{};
            SceneStrut strut;
            Align align;
            float margin_start{};
            float margin_end{};
            // Border and padding.
            float inner_start{};
            float inner_end{};
            float top{};
            float bottom{};
            // Its direction is rtl: its start edge is its right one.
            bool rtl{};
            // It has a box fragment of its own, as Blink's LayoutInline::ComputeInitialShouldCreateBoxFragment and
            // BoxInfo::ShouldCreateBoxFragmentForChild decide (margins, borders, padding, a background, a shadow, an
            // outline or relative position; a child box with margins, another vertical-align or other font metrics):
            // only then does it hold a placeholder on its lines, which bidi reordering moves. Blink culls the others.
            bool fragment{};
        };

        struct Item {
            enum class Kind : std::uint8_t {
                Text,
                Open,
                Close,
                Atomic,
                Break,
                // An explicit bidi formatting character an inline box's unicode-bidi inserts around it (Blink's
                // kBidiControl items): no width, no content; at a soft wrap opportunity it stays on the line before,
                // as a close tag does.
                Control,
            };

            Kind kind{};
            const SceneNode* node{};
            // Text: the collapsed text, its font, its shaped run and the advance before each byte.
            std::string text;
            SceneFont font;
            std::shared_ptr<const SceneTextRun> run;
            std::vector<float> prefix;
            // Text: the bytes a glyph cluster starts at, and its end: where text-overflow may cut it.
            std::vector<bool> starts;
            // Text: its owner's word-break, and the opportunities its owner's overflow-wrap gives inside its words.
            WordBreak word_break{};
            Emergency emergency{};
            // The item's first byte in the paragraph text.
            std::size_t begin{};
            // Open and Close: the box; text and atomic: the innermost inline box around it, none at the root.
            std::size_t box{none};
            // Atomic: its vertical-align.
            Align align;
            // Text: shaped together with other items (its run is its part of theirs).
            bool joint{};
            // Text, atomic and control: the bidi level of its characters (a text item is split where it changes; a
            // control has the level SheenBidi gives it: an isolate's initiator and terminator the level around the
            // isolate, the other controls their preceding character's).
            std::uint8_t level{};
            // Control: the character (UTF-8) and whether it pops.
            std::string_view control;
            bool exit{};
        };

        // An unbreakable part of the content: a text item's bytes [begin, end), or one other item.
        struct Chunk {
            std::size_t item{};
            std::size_t begin{};
            std::size_t end{};
            // A soft or forced wrap opportunity lies before this unit.
            bool break_before{};
            // Or one that overflow-wrap gives (no soft one): the unit starts a grapheme cluster inside a word.
            Emergency emergency{};
        };

    } // namespace

    bool scene_inline_level(const SceneNode& n) {
        return inline_level(n);
    }

    bool scene_mixed_children(const SceneNode& n) {
        if (n.tag == "#text" || n.space == constants::svg_namespace_uri || !n.text.empty() || scene_img(n)) {
            return false;
        }
        const auto d = display(n);
        if (d != "block" && d != "inline-block" && d != "list-item" && d != "flow-root") {
            return false;
        }
        bool inline_child{};
        bool block_child{};
        for (const auto& c : n.children) {
            if (display(c) == "none" || out_of_flow(c)) {
                continue;
            }
            (inline_level(c) ? inline_child : block_child) = true;
        }
        return inline_child && block_child;
    }

    bool scene_img(const SceneNode& n) {
        return n.tag == "img" && (n.space.empty() || n.space == constants::xhtml_namespace_uri);
    }

    bool scene_atomic_inline(const SceneNode& n) {
        if (n.space == constants::svg_namespace_uri) {
            return true;
        }
        const auto d = display(n);
        // An img is a replaced box: an inline one is atomic.
        if (scene_img(n) && d == "inline") {
            return true;
        }
        return d == "inline-block" || d == "inline-flex" || d == "inline-grid" || d == "inline-table" || scene_ruby(n);
    }

    bool scene_ruby(const SceneNode& n) {
        if (n.tag != "ruby" || (!n.space.empty() && n.space != constants::xhtml_namespace_uri)) {
            return false;
        }
        const auto d = display(n);
        return d == "inline" || d == "ruby";
    }

    bool scene_inline_children(const SceneNode& n) {
        if (n.tag == "#text" || n.space == constants::svg_namespace_uri || !n.text.empty()) {
            return false;
        }
        const auto d = display(n);
        if (d == "flex" || d == "inline-flex" || d == "grid" || d == "inline-grid" || d == "none") {
            return false;
        }
        bool any{};
        for (const auto& c : n.children) {
            if (display(c) == "none" || out_of_flow(c)) {
                continue;
            }
            if (!inline_level(c)) {
                return false;
            }
            any = true;
        }
        return any;
    }

    std::vector<SceneNode> scene_fragment_boxes(const SceneNode& n) {
        std::vector<SceneNode> out;
        for (const auto& f : n.fragments) {
            SceneNode box;
            box.identity = n.identity;
            box.tag = n.tag;
            box.space = n.space;
            box.attributes = n.attributes;
            box.style = n.style;
            box.paint = n.paint;
            box.box = {n.box[0] + f.rect[0], n.box[1] + f.rect[1], f.rect[2], f.rect[3]};
            // Transforms do not apply to inline boxes.
            box.style.erase(PropertyId::Transform);
            if (!f.first) {
                box.style[PropertyId::BorderLeftWidth] = "0";
                box.style[PropertyId::BorderTopLeftRadius] = "0";
                box.style[PropertyId::BorderBottomLeftRadius] = "0";
            }
            if (!f.last) {
                box.style[PropertyId::BorderRightWidth] = "0";
                box.style[PropertyId::BorderTopRightRadius] = "0";
                box.style[PropertyId::BorderBottomRightRadius] = "0";
            }
            out.push_back(std::move(box));
        }
        return out;
    }

    SceneNode scene_without_box(const SceneNode& n) {
        SceneNode out = n;
        out.fragments.clear();
        out.effect.reset();
        // The fragments' boxes filter what lies behind them.
        out.style.erase(PropertyId::BackdropFilter);
        out.style.erase(PropertyId::Transform);
        out.style[PropertyId::BackgroundColor] = "transparent";
        out.style[PropertyId::BackgroundImage] = "none";
        out.style[PropertyId::BoxShadow] = "none";
        out.border_image.reset();
        for (const auto id : {PropertyId::BorderTopWidth, PropertyId::BorderRightWidth, PropertyId::BorderBottomWidth,
                              PropertyId::BorderLeftWidth}) {
            out.style[id] = "0";
        }
        if (out.paint) {
            // Only the typed color and opacity: the others would transform or paint the box again.
            for (unsigned i = 0; i < unsigned(constants::scene::paint_property_count); ++i) {
                if (i != unsigned(constants::scene::paint_slot_color) &&
                    i != unsigned(constants::scene::paint_slot_opacity)) {
                    out.paint->values[i].reset();
                }
            }
        }
        return out;
    }

    bool scene_shows_overflow(const SceneNode& n) {
        if (n.children.empty() && n.text.empty() && n.lines.empty()) {
            return false;
        }
        const auto overflow = n.style.text(PropertyId::Overflow, "visible");
        return (overflow == "visible" || overflow.empty()) && n.tag != "svg" &&
               n.style.text(PropertyId::Transform, "none").find("perspective(") == std::string_view::npos;
    }

    SceneNode scene_text_line(const SceneNode& n, std::size_t i) {
        const auto& line = n.lines.at(i);
        SceneNode out;
        out.identity = n.identity;
        out.tag = "#text";
        out.space = n.space;
        out.text = line.text;
        // The node's box when its only line lies inside it; otherwise the line box grown to whole px of the node's
        // coordinates. The text sits at its offset inside: glyphs keep the subpixel positions they have in the node,
        // and the image sits on the node's pixel grid.
        const auto& r = line.rect;
        const bool whole =
            n.lines.size() == 1 && r[0] >= 0 && r[1] >= 0 && r[0] + r[2] <= n.box[2] && r[1] + r[3] <= n.box[3];
        const float left = whole ? 0 : std::floor(r[0]);
        const float top = whole ? 0 : std::floor(r[1]);
        out.box = whole ? std::array<float, 4>{0, 0, n.box[2], n.box[3]}
                        : std::array<float, 4>{left, top, std::ceil(r[0] + r[2]) - left, std::ceil(r[1] + r[3]) - top};
        // Every value scene_font reads, so the line shapes as layout shaped it, and its color and pointer-events.
        for (const auto id : {PropertyId::Color, PropertyId::FontFamily, PropertyId::FontSize, PropertyId::FontWeight,
                              PropertyId::FontStyle, PropertyId::FontVariantNumeric, PropertyId::LetterSpacing,
                              PropertyId::LineHeight, PropertyId::PointerEvents}) {
            if (const auto* v = n.style.find(id)) {
                out.style.set(id, *v);
            }
        }
        // The line's direction and context, as layout shaped its run, and its glyphs when layout's are its own.
        out.text_context = {line.context_before, line.context_after};
        out.text_run = line.run;
        out.style[PropertyId::Direction] = line.rtl ? "rtl" : "ltr";
        out.style[PropertyId::UnicodeBidi] = line.bidi ? "bidi-override" : "normal";
        out.style[PropertyId::Display] = "inline";
        out.style[PropertyId::PaddingLeft] = px_text(line.rect[0] - left);
        out.style[PropertyId::PaddingTop] = px_text(line.rect[1] - top);
        if (n.paint && n.paint->values[constants::scene::paint_slot_color]) {
            out.paint.emplace();
            out.paint->values[constants::scene::paint_slot_color] = n.paint->values[constants::scene::paint_slot_color];
        }
        return out;
    }

    struct SceneInlineContent::Impl {
        SceneText& text;
        const SceneNode& root;
        SceneFont root_font;
        SceneStrut root_strut;
        std::string paragraph;
        std::vector<Item> items;
        std::vector<Box> boxes;
        std::vector<Chunk> chunks;
        bool breakable{};
        // A chunk starts at an opportunity overflow-wrap gives.
        bool emergencies{};
        // A collapsible space was the last character collected: the next one collapses. True at the start and after
        // a forced break, where a line drops its leading spaces.
        bool space_before{true};
        // text-overflow: ellipsis on a root that clips: what an overflowing line ends with, U+2026 or three full stops
        // when its primary font has no glyph for it (as Blink chooses), shaped in the root's font; empty otherwise.
        std::string ellipsis;
        std::shared_ptr<const SceneTextRun> ellipsis_run;
        // The paragraph text needs the bidirectional algorithm: a right-to-left or plaintext root, right-to-left text
        // or an inline box's bidi control. Then paragraph_levels holds each byte's level and each paragraph's base
        // level.
        bool bidi{};
        BidiParagraphs paragraph_levels;
        // Lines are placed in visual order (bidi_reorder): bidi, or an inline box whose direction is rtl (its start
        // edge on its right).
        bool reorder{};

        // The root's children it holds: [first_child, last_child).
        std::size_t first_child{};
        std::size_t last_child{};

        Impl(SceneText& t, const SceneNode& r, std::size_t f, std::size_t l)
            : text(t), root(r), first_child(f), last_child(l) {
            root_font = scene_font(root.style);
            root_strut = text.strut(root_font);
            const auto align = root.style.text(PropertyId::TextAlign, "start");
            if (align != "left" && align != "start" && align != "right" && align != "end" && align != "center") {
                fail("text-align " + std::string(align) + " outside scene profile");
            }
            const auto d = display(root);
            if (root.style.text(PropertyId::TextOverflow, "clip") == "ellipsis" &&
                root.style.text(PropertyId::Overflow, "visible") != "visible" &&
                (root.text.empty() || (d != "flex" && d != "inline-flex" && d != "grid" && d != "inline-grid"))) {
                ellipsis = text.primary_covers(root_font, constants::text::horizontal_ellipsis)
                               ? std::string(constants::text::horizontal_ellipsis_utf8)
                               : std::string(constants::text::ellipsis_fallback);
                ellipsis_run = text.shape(ellipsis, root_font);
            }
            // A root that overrides its text's direction starts it with the override, as Blink's EnterBlock does
            // (and ends it with the pop); its other unicode-bidi values act through its paragraphs' base direction.
            const bool rtl = root_font.base == BidiBase::Rtl;
            if (root_font.bidi_override) {
                add_control(rtl ? right_to_left_override : left_to_right_override, false, none);
                bidi_context.push_back(
                    {rtl ? right_to_left_override : left_to_right_override, pop_directional_formatting});
            }
            collect(root, none);
            if (root_font.bidi_override) {
                add_control(pop_directional_formatting, true, none);
                bidi_context.pop_back();
            }
            if (paragraph.size() > std::size_t(constants::max_text_bytes)) {
                fail("scene paragraph byte bound");
            }
            shape_items();
            split();
        }

        // The explicit bidi formatting characters (U+202A-U+202E, U+2066-U+2069), as UTF-8.
        static constexpr std::string_view left_to_right_embedding = "\xE2\x80\xAA";
        static constexpr std::string_view right_to_left_embedding = "\xE2\x80\xAB";
        static constexpr std::string_view pop_directional_formatting = "\xE2\x80\xAC";
        static constexpr std::string_view left_to_right_override = "\xE2\x80\xAD";
        static constexpr std::string_view right_to_left_override = "\xE2\x80\xAE";
        static constexpr std::string_view left_to_right_isolate = "\xE2\x81\xA6";
        static constexpr std::string_view right_to_left_isolate = "\xE2\x81\xA7";
        static constexpr std::string_view first_strong_isolate = "\xE2\x81\xA8";
        static constexpr std::string_view pop_directional_isolate = "\xE2\x81\xA9";

        // The bidi controls entered and not yet popped at the end of the paragraph so far, outermost first, each with
        // the character that pops it.
        std::vector<std::pair<std::string_view, std::string_view>> bidi_context;

        // A forced break (<br>, a preserved newline): the bidi contexts open across it pop before it and enter again
        // after it, as Blink's AppendForcedBreak does (CSS Writing Modes 4 §2.4.3), since it ends a paragraph of the
        // bidirectional algorithm.
        void add_break(Item item) {
            const auto box = item.box;
            for (auto k = bidi_context.size(); k-- > 0;) {
                add_control(bidi_context[k].second, true, box);
            }
            item.begin = paragraph.size();
            paragraph += '\n';
            space_before = true;
            items.push_back(std::move(item));
            for (const auto& [enter, pop] : bidi_context) {
                add_control(enter, false, box);
            }
        }

        void add_control(std::string_view character, bool exit, std::size_t box) {
            Item item;
            item.kind = Item::Kind::Control;
            item.control = character;
            item.exit = exit;
            item.box = box;
            item.begin = paragraph.size();
            paragraph += character;
            items.push_back(std::move(item));
        }

        // The controls an inline element's unicode-bidi inserts before its open tag (CSS Writing Modes 4 §2.4.2, as
        // Blink's EnterInline does), each with the one that pops it after its close tag (in reverse order there).
        static std::vector<std::pair<std::string_view, std::string_view>> bidi_controls(const SceneStyle& s) {
            const bool rtl = s.text(PropertyId::Direction, "ltr") == "rtl";
            const auto value = s.text(PropertyId::UnicodeBidi, "normal");
            if (value == "embed") {
                return {{rtl ? right_to_left_embedding : left_to_right_embedding, pop_directional_formatting}};
            }
            if (value == "bidi-override") {
                return {{rtl ? right_to_left_override : left_to_right_override, pop_directional_formatting}};
            }
            if (value == "isolate") {
                return {{rtl ? right_to_left_isolate : left_to_right_isolate, pop_directional_isolate}};
            }
            if (value == "plaintext") {
                return {{first_strong_isolate, pop_directional_isolate}};
            }
            if (value == "isolate-override") {
                return {{first_strong_isolate, pop_directional_isolate},
                        {rtl ? right_to_left_override : left_to_right_override, pop_directional_formatting}};
            }
            return {};
        }

        // The bidi levels of the paragraph (when it needs them), the text items split where their level changes,
        // and each shaped: in one direction, its level's, when the paragraph is bidi; otherwise left to right as the
        // text runs (no reordering).
        void shape_items() {
            for (auto& b : boxes) {
                const auto& s = b.node->style;
                const auto nonzero = [&](PropertyId id) {
                    const auto v = css::trim(s.text(id, "0"));
                    return !v.empty() && v != "0" && v != "0px" && v != "auto";
                };
                const auto bordered = [&](PropertyId width, PropertyId style) {
                    const auto v = s.text(style, "solid");
                    return v != "none" && v != "hidden" && nonzero(width);
                };
                const auto background = css::trim(s.text(PropertyId::BackgroundColor, "transparent"));
                b.fragment = nonzero(PropertyId::MarginLeft) || nonzero(PropertyId::MarginRight) ||
                             nonzero(PropertyId::MarginTop) || nonzero(PropertyId::MarginBottom) ||
                             nonzero(PropertyId::PaddingLeft) || nonzero(PropertyId::PaddingRight) ||
                             nonzero(PropertyId::PaddingTop) || nonzero(PropertyId::PaddingBottom) ||
                             bordered(PropertyId::BorderLeftWidth, PropertyId::BorderLeftStyle) ||
                             bordered(PropertyId::BorderRightWidth, PropertyId::BorderRightStyle) ||
                             bordered(PropertyId::BorderTopWidth, PropertyId::BorderTopStyle) ||
                             bordered(PropertyId::BorderBottomWidth, PropertyId::BorderBottomStyle) ||
                             (background != "transparent" && background != "rgba(0, 0, 0, 0)" && !background.empty()) ||
                             css::trim(s.text(PropertyId::BackgroundImage, "none")) != "none" ||
                             css::trim(s.text(PropertyId::BoxShadow, "none")) != "none" ||
                             css::trim(s.text(PropertyId::OutlineStyle, "none")) != "none" ||
                             s.text(PropertyId::Position, "static") != "static";
            }
            // A parent box for a child with margins, another vertical-align or other font metrics.
            for (const auto& b : boxes) {
                if (b.parent == none) {
                    continue;
                }
                auto& parent = boxes[b.parent];
                const bool margins = [&] {
                    const auto& s = b.node->style;
                    for (const auto id : {PropertyId::MarginLeft, PropertyId::MarginRight, PropertyId::MarginTop,
                                          PropertyId::MarginBottom}) {
                        const auto v = css::trim(s.text(id, "0"));
                        if (!v.empty() && v != "0" && v != "0px" && v != "auto") {
                            return true;
                        }
                    }
                    return false;
                }();
                parent.fragment = parent.fragment || margins || b.align.kind != Align::Kind::Baseline ||
                                  b.strut.ascent != parent.strut.ascent || b.strut.descent != parent.strut.descent;
            }
            bidi = root_font.base != BidiBase::Ltr || !bidi_left_to_right(paragraph);
            reorder = bidi || std::any_of(boxes.begin(), boxes.end(), [](const Box& b) {
                          return b.rtl;
                      });
            if (bidi) {
                paragraph_levels = bidi_levels(paragraph, root_font.base);
                const auto& levels = paragraph_levels.levels;
                std::vector<Item> split_items;
                split_items.reserve(items.size());
                for (auto& item : items) {
                    if (item.kind == Item::Kind::Atomic || item.kind == Item::Kind::Control) {
                        item.level = levels[item.begin];
                    }
                    if (item.kind != Item::Kind::Text) {
                        split_items.push_back(std::move(item));
                        continue;
                    }
                    std::size_t start = 0;
                    for (std::size_t k = 1; k <= item.text.size(); ++k) {
                        if (k < item.text.size() && levels[item.begin + k] == levels[item.begin + start]) {
                            continue;
                        }
                        Item part = item;
                        part.text = item.text.substr(start, k - start);
                        part.begin = item.begin + start;
                        part.level = levels[item.begin + start];
                        split_items.push_back(std::move(part));
                        start = k;
                    }
                }
                items = std::move(split_items);
            }
            // Runs of text items shaped together, as Blink's InlineNode::ShapeText does: consecutive items of one font
            // and one direction, with nothing between them but inline box edges that have no margin, border or
            // padding on that side and a baseline vertical-align (CSS Text 3 §7.3, ShouldBreakShapingBeforeBox).
            for (std::size_t i = 0; i < items.size();) {
                if (items[i].kind != Item::Kind::Text) {
                    ++i;
                    continue;
                }
                std::vector<std::size_t> run{i};
                auto j = i + 1;
                for (; j < items.size(); ++j) {
                    const auto& item = items[j];
                    if (item.kind == Item::Kind::Text) {
                        if (!same_font(item.font, items[i].font) || item.level != items[i].level) {
                            break;
                        }
                        run.push_back(j);
                    } else if ((item.kind != Item::Kind::Open && item.kind != Item::Kind::Close) ||
                               breaks_shaping(item)) {
                        break;
                    }
                }
                shape(run);
                i = j;
            }
        }

        // Whether two fonts shape alike (all but the line height, which shaping does not read).
        static bool same_font(const SceneFont& a, const SceneFont& b) {
            return a.families == b.families && a.size == b.size && a.weight == b.weight && a.italic == b.italic &&
                   a.numeric == b.numeric && a.letter_spacing == b.letter_spacing;
        }

        // Whether an inline box's start (an open tag) or end (a close tag) breaks shaping: it has a margin, border or
        // padding on that side (its start is its right when its direction is rtl), or a vertical-align other than
        // baseline.
        bool breaks_shaping(const Item& item) const {
            const auto& b = boxes[item.box];
            const auto& s = b.node->style;
            if (b.align.kind != Align::Kind::Baseline) {
                return true;
            }
            const bool left = (item.kind == Item::Kind::Open) != b.rtl;
            const auto nonzero = [&](PropertyId id) {
                const auto v = css::trim(s.text(id, "0"));
                return !v.empty() && v != "0" && v != "0px";
            };
            const auto bordered = [&](PropertyId width, PropertyId style) {
                const auto v = s.text(style, "solid");
                return v != "none" && v != "hidden" && nonzero(width);
            };
            return left ? nonzero(PropertyId::MarginLeft) || nonzero(PropertyId::PaddingLeft) ||
                              bordered(PropertyId::BorderLeftWidth, PropertyId::BorderLeftStyle)
                        : nonzero(PropertyId::MarginRight) || nonzero(PropertyId::PaddingRight) ||
                              bordered(PropertyId::BorderRightWidth, PropertyId::BorderRightStyle);
        }

        // The glyphs of a run's bytes [begin, end), from the pen where the first stands, their clusters from begin.
        static std::shared_ptr<const SceneTextRun> part(const SceneTextRun& run, std::size_t begin, std::size_t end,
                                                        const SceneTextMetrics& metrics) {
            auto out = std::make_shared<SceneTextRun>();
            out->size = run.size;
            out->metrics = metrics;
            out->text_id = run.text_id;
            out->faces = run.faces;
            float pen{};
            float origin = std::numeric_limits<float>::quiet_NaN();
            for (const auto& g : run.glyphs) {
                if (g.utf8_cluster >= begin && g.utf8_cluster < end) {
                    if (std::isnan(origin)) {
                        origin = pen;
                    }
                    auto glyph = g;
                    glyph.x -= origin;
                    glyph.utf8_cluster -= std::uint32_t(begin);
                    out->glyphs.push_back(glyph);
                }
                pen += g.advance;
            }
            return out;
        }

        // The paragraph's few characters before byte `at` and from byte `at` on: shaping's context (Blink shapes
        // with its paragraph's text around).
        ShapeContext context_at(std::size_t begin, std::size_t end) const {
            std::size_t from = begin;
            for (int n = 0; n < constants::text::shape_context_characters && from > 0; ++n) {
                do {
                    --from;
                } while (from > 0 && (static_cast<unsigned char>(paragraph[from]) & 0xC0) == 0x80);
            }
            std::size_t to = end;
            for (int n = 0; n < constants::text::shape_context_characters && to < paragraph.size(); ++n) {
                do {
                    ++to;
                } while (to < paragraph.size() && (static_cast<unsigned char>(paragraph[to]) & 0xC0) == 0x80);
            }
            return {std::string_view(paragraph).substr(from, begin - from),
                    std::string_view(paragraph).substr(end, to - end)};
        }

        // Shapes a run of text items together, in one direction (its level's, overridden; left to right in a
        // paragraph without bidi), in the paragraph's context, and gives each item its glyphs: those of its
        // characters (a ligature belongs to the item of its first character, as in Blink), from the pen where its
        // first one stands; then the advance before each byte and its cluster starts.
        void shape(const std::vector<std::size_t>& run) {
            auto& first = items[run.front()];
            auto& last = items[run.back()];
            auto font = first.font;
            font.base = bidi && first.level % 2 ? BidiBase::Rtl : BidiBase::Ltr;
            font.bidi_override = bidi;
            const auto begin = first.begin;
            const auto end = last.begin + last.text.size();
            const auto shaped =
                text.shape(std::string_view(paragraph).substr(begin, end - begin), font, context_at(begin, end));
            // Each glyph's pen, in the run's visual order.
            std::vector<float> pens;
            pens.reserve(shaped->glyphs.size());
            float pen{};
            for (const auto& g : shaped->glyphs) {
                pens.push_back(pen);
                pen += g.advance;
            }
            for (const auto k : run) {
                auto& item = items[k];
                item.font.base = font.base;
                item.font.bidi_override = font.bidi_override;
                item.joint = run.size() > 1;
                if (run.size() == 1) {
                    item.run = shaped;
                } else {
                    const auto from = item.begin - begin;
                    const auto to = from + item.text.size();
                    item.run =
                        part(*shaped, from, to, text.slice(*shaped, font, std::uint32_t(from), std::uint32_t(to)));
                }
                std::vector<float> at(item.text.size() + 1);
                for (const auto& g : item.run->glyphs) {
                    at[std::min<std::size_t>(g.utf8_cluster, item.text.size())] += g.advance;
                }
                item.prefix.assign(item.text.size() + 1, 0);
                for (std::size_t b = 0; b < item.text.size(); ++b) {
                    item.prefix[b + 1] = item.prefix[b] + at[b];
                }
                item.starts.assign(item.text.size() + 1, false);
                for (const auto& g : item.run->glyphs) {
                    item.starts[std::min<std::size_t>(g.utf8_cluster, item.text.size())] = true;
                }
                item.starts.back() = true;
            }
        }

        // How white-space treats a text (CSS Text 3 §3): whether its spaces and tabs collapse, and whether a newline
        // (a segment break) is a forced break; wraps() says whether its lines wrap.
        struct WhiteSpace {
            bool collapse_spaces{};
            bool keep_newlines{};
        };

        static WhiteSpace white_space(const SceneStyle& s) {
            const auto ws = s.text(PropertyId::WhiteSpace, "normal");
            if (ws == "normal" || ws == "nowrap") {
                return {true, false};
            }
            if (ws == "pre" || ws == "pre-wrap" || ws == "break-spaces") {
                return {false, true};
            }
            if (ws == "pre-line") {
                return {true, true};
            }
            fail("white-space " + std::string(ws) + " outside scene profile");
        }

        static bool wraps(const SceneStyle& s) {
            const auto ws = s.text(PropertyId::WhiteSpace, "normal");
            white_space(s);
            return ws == "normal" || ws == "pre-wrap" || ws == "pre-line" || ws == "break-spaces";
        }

        static WordBreak word_break(const SceneStyle& s) {
            const auto v = s.text(PropertyId::WordBreak, "normal");
            if (v == "normal" || v == "break-word") {
                return WordBreak::Normal;
            }
            if (v == "break-all") {
                return WordBreak::BreakAll;
            }
            if (v == "keep-all") {
                return WordBreak::KeepAll;
            }
            fail("word-break " + std::string(v) + " outside scene profile");
        }

        // The opportunities a text's overflow-wrap gives (word-break: break-word gives anywhere's); none where its lines
        // do not wrap.
        static Emergency emergency(const SceneStyle& s) {
            const auto v = s.text(PropertyId::OverflowWrap, "normal");
            if (v != "normal" && v != "break-word" && v != "anywhere") {
                fail("overflow-wrap " + std::string(v) + " outside scene profile");
            }
            if (!wraps(s)) {
                return Emergency::None;
            }
            if (v == "anywhere" || s.text(PropertyId::WordBreak, "normal") == "break-word") {
                return Emergency::Anywhere;
            }
            return v == "break-word" ? Emergency::BreakWord : Emergency::None;
        }

        // A text node's text: its runs between preserved newlines as text items, each such newline a forced break,
        // and each of the element's DOM text nodes (SceneNode::text_breaks) a run of its own, as Chrome lays them out.
        void add_text(const SceneNode& owner, std::size_t box) {
            const auto mode = white_space(owner.style);
            std::string run;
            const auto flush = [&] {
                if (!run.empty()) {
                    add_text_run(owner, box, std::move(run));
                    run.clear();
                }
            };
            auto next_break = owner.text_breaks.begin();
            for (std::size_t at = 0; at < owner.text.size(); ++at) {
                const char c = owner.text[at];
                if (next_break != owner.text_breaks.end() && *next_break == at) {
                    flush();
                    ++next_break;
                }
                if (mode.keep_newlines && (c == '\n' || c == '\r')) {
                    if (c == '\r') {
                        continue;
                    }
                    flush();
                    // A preserved newline breaks the line as <br> does; the spaces before it collapse away when they
                    // collapse, and hang otherwise.
                    Item item;
                    item.kind = Item::Kind::Break;
                    item.node = &owner;
                    item.box = box;
                    add_break(std::move(item));
                    continue;
                }
                if (mode.collapse_spaces && collapsible(c)) {
                    if (!space_before) {
                        run += ' ';
                        space_before = true;
                    }
                } else if (!mode.collapse_spaces && (c == '\t' || c == '\f' || c == '\r' || c == '\n')) {
                    // A preserved tab advances as spaces (the scene does not align to tab stops).
                    run += c == '\t' ? std::string(4, ' ') : std::string(" ");
                    space_before = false;
                } else {
                    run += c;
                    space_before = false;
                }
            }
            flush();
        }

        void add_text_run(const SceneNode& owner, std::size_t box, std::string content) {
            Item item;
            item.kind = Item::Kind::Text;
            item.node = &owner;
            item.box = box;
            item.text = std::move(content);
            if (item.text.empty()) {
                return;
            }
            // Shaped once the paragraph's levels are known (shape_items).
            item.font = scene_font(owner.style);
            item.word_break = word_break(owner.style);
            item.emergency = emergency(owner.style);
            item.begin = paragraph.size();
            paragraph += item.text;
            items.push_back(std::move(item));
        }

        void collect(const SceneNode& n, std::size_t box) {
            if (!n.text.empty()) {
                add_text(n, box);
            }
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                const auto& c = n.children[k];
                if (display(c) == "none" || (&n == &root && (k < first_child || k >= last_child))) {
                    continue;
                }
                if (out_of_flow(c)) {
                    if (&n != &root) {
                        fail("positioned element inside an inline box outside scene profile");
                    }
                    continue;
                }
                if (c.tag == "#text") {
                    add_text(c, box);
                    continue;
                }
                Item item;
                item.node = &c;
                item.box = box;
                item.begin = paragraph.size();
                item.align = vertical_align(c.style);
                if (c.tag == "br") {
                    item.kind = Item::Kind::Break;
                    add_break(std::move(item));
                } else if (scene_atomic_inline(c)) {
                    item.kind = Item::Kind::Atomic;
                    paragraph += object_replacement;
                    space_before = false;
                    items.push_back(std::move(item));
                } else if (display(c) == "inline") {
                    Box b;
                    b.node = &c;
                    b.parent = box;
                    b.depth = box == none ? 1 : boxes[box].depth + 1;
                    b.font = scene_font(c.style);
                    b.rtl = c.style.text(PropertyId::Direction, "ltr") == "rtl";
                    const auto controls = bidi_controls(c.style);
                    for (const auto& [enter, pop] : controls) {
                        add_control(enter, false, box);
                        bidi_context.push_back({enter, pop});
                    }
                    item.begin = paragraph.size();
                    b.font_size = computed_font_size(c.style);
                    b.strut = text.strut(b.font);
                    b.align = item.align;
                    wraps(c.style);
                    const auto index = boxes.size();
                    boxes.push_back(b);
                    item.kind = Item::Kind::Open;
                    item.box = index;
                    items.push_back(item);
                    collect(c, index);
                    Item close;
                    close.kind = Item::Kind::Close;
                    close.node = &c;
                    close.box = index;
                    close.begin = paragraph.size();
                    items.push_back(close);
                    for (auto k = controls.size(); k-- > 0;) {
                        add_control(controls[k].second, true, box);
                        bidi_context.pop_back();
                    }
                } else {
                    fail("block-level box inside an inline formatting context outside scene profile");
                }
            }
        }

        // Whether soft wraps are allowed at a boundary inside box b (none: the root): its white-space.
        bool wrapping(std::size_t b) const {
            return wraps(b == none ? root.style : boxes[b].node->style);
        }

        // The nearest box that contains both a and b; none for the root.
        std::size_t common(std::size_t a, std::size_t b) const {
            const auto depth = [&](std::size_t x) {
                return x == none ? std::size_t(0) : boxes[x].depth;
            };
            while (depth(a) > depth(b)) {
                a = boxes[a].parent;
            }
            while (depth(b) > depth(a)) {
                b = boxes[b].parent;
            }
            while (a != b) {
                a = boxes[a].parent;
                b = boxes[b].parent;
            }
            return a;
        }

        // word-break (CSS Text 3 §5.2) over the UAX 14 opportunities between grapheme clusters, a letter unit by its
        // first character (probe_letter_units): break-all treats letter units as ideographs, so that a break may fall
        // between two of them where either side's text is break-all; keep-all suppresses the opportunities between two
        // letter units of keep-all texts.
        void tailor(std::vector<bool>& allowed, const std::vector<bool>& cluster,
                    const std::vector<std::size_t>& owner) const {
            if (std::none_of(items.begin(), items.end(), [](const Item& item) {
                    return item.kind == Item::Kind::Text && item.word_break != WordBreak::Normal;
                })) {
                return;
            }
            const auto mode = [&](std::size_t byte) {
                const auto& item = items[owner[byte]];
                return item.kind == Item::Kind::Text ? item.word_break : WordBreak::Normal;
            };
            const auto letters = probe_letter_units(paragraph);
            // The first byte of the cluster before p.
            std::size_t base = 0;
            for (std::size_t p = 1; p < paragraph.size(); ++p) {
                if (!cluster[p]) {
                    continue;
                }
                if (letters[base] && letters[p]) {
                    const auto a = mode(p - 1);
                    const auto b = mode(p);
                    if (a == WordBreak::BreakAll || b == WordBreak::BreakAll) {
                        allowed[p] = true;
                    } else if (a == WordBreak::KeepAll && b == WordBreak::KeepAll) {
                        allowed[p] = false;
                    }
                }
                base = p;
            }
        }

        // The chunks and their wrap opportunities: UAX 14 over the whole paragraph as word-break tailors it (and after
        // every run of spaces and around atomic inlines, as Blink breaks), a soft
        // opportunity where the nearest common ancestor of the characters on either side allows wrapping (CSS Text 3,
        // 5.1), a forced one after <br>. An opportunity at an inline box's edge lies after the end edges there and
        // before the start edges. A text whose overflow-wrap breaks words splits them into its grapheme clusters too,
        // each after the first starting at an emergency opportunity.
        void split() {
            std::vector<std::size_t> owner(paragraph.size(), none);
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = items[i];
                const auto size = item.kind == Item::Kind::Text      ? item.text.size()
                                  : item.kind == Item::Kind::Atomic  ? object_replacement.size()
                                  : item.kind == Item::Kind::Break   ? std::size_t(1)
                                  : item.kind == Item::Kind::Control ? item.control.size()
                                                                     : std::size_t(0);
                std::fill(owner.begin() + std::ptrdiff_t(item.begin), owner.begin() + std::ptrdiff_t(item.begin + size),
                          i);
            }
            std::vector<bool> opportunity(paragraph.size() + 1);
            // The bytes a grapheme cluster starts at.
            std::vector<bool> cluster(paragraph.size() + 1);
            if (!paragraph.empty()) {
                const auto breaks = probe_unicode_breaks(paragraph);
                std::vector<bool> allowed(paragraph.size() + 1);
                for (const auto p : breaks.line) {
                    allowed[p] = true;
                }
                cluster[0] = true;
                for (const auto p : breaks.grapheme) {
                    cluster[p] = true;
                }
                tailor(allowed, cluster, owner);
                // After a run of spaces a line may always break, as Blink's line break iterator has it
                // (BreakSpaceType::kAfterSpaceRun), also where UAX 14 keeps the next character (LB14-LB17: "( x",
                // "\" (").
                for (std::size_t p = 1; p < paragraph.size(); ++p) {
                    if (paragraph[p - 1] == ' ' && paragraph[p] != ' ' && cluster[p]) {
                        allowed[p] = true;
                    }
                }
                // An atomic inline has an opportunity after it, and one before it after text that ends where it
                // starts, whatever the characters beside it (Blink's LineBreaker::CanBreakAfterAtomicInline and
                // CanBreakAfter: also before "?", a quote or U+00A0; not after a bidi control).
                for (const auto& item : items) {
                    if (item.kind != Item::Kind::Atomic) {
                        continue;
                    }
                    if (item.begin > 0 && items[owner[item.begin - 1]].kind == Item::Kind::Text) {
                        allowed[item.begin] = true;
                    }
                    if (const auto end = item.begin + object_replacement.size();
                        end < paragraph.size() && cluster[end]) {
                        allowed[end] = true;
                    }
                }
                for (std::size_t p = 1; p < paragraph.size(); ++p) {
                    if (!allowed[p]) {
                        continue;
                    }
                    const auto& before = items[owner[p - 1]];
                    const auto& after = items[owner[p]];
                    opportunity[p] = before.kind == Item::Kind::Break || wrapping(common(before.box, after.box));
                }
            }
            std::size_t placed = std::numeric_limits<std::size_t>::max();
            // An opportunity at a unit that does not take it (an end edge, a bidi control), for the next one that does:
            // a control is a character of its own, so the unit after it starts at another byte.
            std::size_t carried = std::numeric_limits<std::size_t>::max();
            const auto add = [&](std::size_t item, std::size_t begin, std::size_t end, std::size_t at, bool closing,
                                 Emergency emergency) {
                Chunk u{item, begin, end};
                const bool here = at != placed && opportunity[at];
                // The first unit at an opportunity that is not an end edge (nor a control) takes it.
                if (closing) {
                    if (here) {
                        carried = at;
                    }
                } else if (here || carried != std::numeric_limits<std::size_t>::max()) {
                    u.break_before = true;
                    placed = here ? at : carried;
                    carried = std::numeric_limits<std::size_t>::max();
                    breakable |= !chunks.empty() && items[chunks.back().item].kind != Item::Kind::Break;
                } else if (emergency != Emergency::None) {
                    u.emergency = emergency;
                    emergencies = true;
                    breakable = true;
                }
                chunks.push_back(u);
            };
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = items[i];
                switch (item.kind) {
                    case Item::Kind::Text: {
                        std::size_t start = 0;
                        auto at_start = Emergency::None;
                        for (std::size_t k = 1; k < item.text.size(); ++k) {
                            const auto p = item.begin + k;
                            // overflow-wrap breaks a word between grapheme clusters, not inside a glyph cluster (a
                            // ligature) nor after a space, where a soft opportunity lies.
                            const bool emergency = item.emergency != Emergency::None && cluster[p] && item.starts[k] &&
                                                   item.text[k - 1] != ' ';
                            if (opportunity[p] || emergency) {
                                add(i, start, k, item.begin + start, false, at_start);
                                at_start = opportunity[p] ? Emergency::None : item.emergency;
                                start = k;
                            }
                        }
                        add(i, start, item.text.size(), item.begin + start, false, at_start);
                        break;
                    }
                    case Item::Kind::Close:
                        add(i, 0, 0, item.begin, true, Emergency::None);
                        break;
                    case Item::Kind::Control:
                        // A control never takes an opportunity: one that pops stays with the content before it, as a
                        // close tag does, and so does one that enters (Chrome's line breaker keeps it on the line
                        // that breaks before the open tag after it, LineBreakBidiControlEnter), at its level.
                        add(i, 0, 0, item.begin, true, Emergency::None);
                        break;
                    default:
                        add(i, 0, 0, item.begin, false, Emergency::None);
                        break;
                }
            }
        }

        // The box edges at an available width, for percentages.
        void resolve_edges(float available) {
            const float base = std::isfinite(available) ? available : 0;
            for (auto& b : boxes) {
                const auto& s = b.node->style;
                // A side whose border style is none or hidden has no border width.
                const auto border = [&](PropertyId width, PropertyId style) {
                    const auto v = s.text(style, "solid");
                    return v == "none" || v == "hidden" ? 0.f : edge(s, width, base);
                };
                const float left_margin = edge(s, PropertyId::MarginLeft, base);
                const float right_margin = edge(s, PropertyId::MarginRight, base);
                const float left = border(PropertyId::BorderLeftWidth, PropertyId::BorderLeftStyle) +
                                   edge(s, PropertyId::PaddingLeft, base);
                const float right = border(PropertyId::BorderRightWidth, PropertyId::BorderRightStyle) +
                                    edge(s, PropertyId::PaddingRight, base);
                // A right-to-left box starts on its right.
                b.margin_start = b.rtl ? right_margin : left_margin;
                b.margin_end = b.rtl ? left_margin : right_margin;
                b.inner_start = b.rtl ? right : left;
                b.inner_end = b.rtl ? left : right;
                b.top = border(PropertyId::BorderTopWidth, PropertyId::BorderTopStyle) +
                        edge(s, PropertyId::PaddingTop, base);
                b.bottom = border(PropertyId::BorderBottomWidth, PropertyId::BorderBottomStyle) +
                           edge(s, PropertyId::PaddingBottom, base);
            }
        }

        bool content(const Chunk& u) const {
            const auto& item = items[u.item];
            switch (item.kind) {
                case Item::Kind::Open:
                    return boxes[item.box].margin_start + boxes[item.box].inner_start != 0;
                case Item::Kind::Close:
                    return boxes[item.box].margin_end + boxes[item.box].inner_end != 0;
                case Item::Kind::Control:
                    return false;
                default:
                    return true;
            }
        }

        // The end of a line's text: the last text unit of chunks [first, last) and its end without trailing spaces;
        // spaces-only chunks before it drop out too. none when the line has no text.
        struct Trim {
            std::size_t unit{none};
            std::size_t end{};
        };

        Trim trim(std::size_t first, std::size_t last) const {
            for (auto u = last; u-- > first;) {
                const auto& unit = chunks[u];
                const auto& item = items[unit.item];
                // Spaces before a forced break end the line too.
                if (item.kind == Item::Kind::Open || item.kind == Item::Kind::Close || item.kind == Item::Kind::Break ||
                    item.kind == Item::Kind::Control) {
                    continue;
                }
                if (item.kind != Item::Kind::Text) {
                    return {};
                }
                auto end = unit.end;
                while (end > unit.begin && item.text[end - 1] == ' ') {
                    --end;
                }
                if (end > unit.begin) {
                    return {u, end};
                }
                // A unit of spaces only: the trimmed end lies before it.
                return {u, unit.begin};
            }
            return {};
        }

        // A line's width as its chunks are added in order: whole LayoutUnits, and the raw advances of the text fragment
        // being added (an item's chunks side by side are one fragment).
        struct Advance {
            Units total{};
            float raw{};
            std::size_t fragment{none};
        };

        // Adds chunk u to a line's width, a text chunk's bytes up to stop.
        void advance(Advance& a, std::size_t u, std::size_t stop, const std::vector<SceneAtomicExtent>& extents) const {
            const auto& unit = chunks[u];
            const auto& item = items[unit.item];
            if (item.kind == Item::Kind::Text) {
                if (unit.item != a.fragment) {
                    a.total += snapped(a.raw);
                    a.raw = 0;
                    a.fragment = unit.item;
                }
                if (stop > unit.begin) {
                    a.raw += item.prefix[stop] - item.prefix[unit.begin];
                }
                return;
            }
            a.total += snapped(a.raw);
            a.raw = 0;
            a.fragment = none;
            if (item.kind == Item::Kind::Open) {
                a.total += to_units(boxes[item.box].margin_start) + to_units(boxes[item.box].inner_start);
            } else if (item.kind == Item::Kind::Close) {
                a.total += to_units(boxes[item.box].inner_end) + to_units(boxes[item.box].margin_end);
            } else if (item.kind == Item::Kind::Atomic) {
                const auto& e = extents[unit.item];
                a.total += to_units(e.margin[3]) + exact(e.width) + to_units(e.margin[1]);
            }
        }

        static Units measured(const Advance& a) {
            return a.total + snapped(a.raw);
        }

        // The width of chunks [first, last) on one line, without the line's trailing spaces when `hang`, nor the end
        // edges after them when the line may break there (content follows): Blink's line breaker takes the close tags
        // that follow a line's trailing spaces at a break with them, so an inline box's end padding and border may
        // overflow the line its spaces end. At the paragraph's end they count.
        Units width(std::size_t first, std::size_t last, bool hang,
                    const std::vector<SceneAtomicExtent>& extents) const {
            const auto end = hang ? trim(first, last) : Trim{};
            const bool spaces = last < chunks.size() && end.unit != none && end.end < chunks[end.unit].end;
            Advance a;
            for (auto u = first; u < last; ++u) {
                if (spaces && u > end.unit && items[chunks[u].item].kind == Item::Kind::Close) {
                    continue;
                }
                advance(a, u, u == end.unit ? end.end : chunks[u].end, extents);
            }
            return measured(a);
        }

        // overflow-wrap (CSS Text 3 §5.5): the word of chunks [i, j) starts the line that begins at chunk `start` (no
        // content before it there) and overflows it, so the line ends at the word's last emergency opportunity that
        // fits, or at its first when none does (a grapheme cluster wider than the line overflows it alone), and the
        // lines after take the rest alike. For the min-content width (`intrinsic`) only anywhere's opportunities count.
        // Returns the first chunk of the line the word ends on.
        std::size_t rescue(std::size_t start, std::size_t i, std::size_t j, Units limit,
                           const std::vector<SceneAtomicExtent>& extents, bool intrinsic,
                           std::vector<std::pair<std::size_t, std::size_t>>& lines) const {
            if (width(start, j, true, extents) <= limit) {
                return start;
            }
            const auto usable = [&](std::size_t u) {
                return chunks[u].emergency == Emergency::Anywhere ||
                       (chunks[u].emergency == Emergency::BreakWord && !intrinsic);
            };
            // The line so far, without hanging spaces: none lie before an emergency opportunity.
            Advance a;
            for (auto u = start; u < i; ++u) {
                advance(a, u, chunks[u].end, extents);
            }
            // The last opportunity on the line that fits.
            std::size_t fit = none;
            for (auto v = i; v < j; ++v) {
                if (v > start && usable(v)) {
                    if (measured(a) <= limit) {
                        fit = v;
                    } else {
                        const auto end = fit == none ? v : fit;
                        lines.emplace_back(start, end);
                        start = end;
                        fit = none;
                        a = {};
                        for (auto u = end; u < v; ++u) {
                            advance(a, u, chunks[u].end, extents);
                        }
                        // No opportunity lies between the line's start and v, the next line's first.
                        if (end < v) {
                            if (measured(a) <= limit) {
                                fit = v;
                            } else {
                                lines.emplace_back(start, v);
                                start = v;
                                a = {};
                            }
                        }
                    }
                }
                advance(a, v, chunks[v].end, extents);
            }
            // The rest, its trailing spaces hanging, still past the limit: the line ends at its last opportunity.
            if (fit != none && width(start, j, true, extents) > limit) {
                lines.emplace_back(start, fit);
                start = fit;
            }
            return start;
        }

        // A piece and its extents on a line: above and below the baseline (with margins), and its rect's top above the
        // baseline and height; a text piece's item and bytes [begin, end) of its text.
        struct Placed {
            SceneInlinePiece piece;
            Units x{};
            Units w{};
            float above{};
            float below{};
            float top{};
            float height{};
            // How far its baseline lies below the line's (vertical-align).
            float shift{};
            std::size_t item{none};
            std::size_t begin{};
            std::size_t end{};
        };

        // A box's fragment on a line: its box, its ends (LayoutUnits from the line's left), and whether the box starts
        // and ends on the line (first, last); after place_visually, whether it has the box's left and right edges.
        struct Fragment {
            std::size_t box{};
            Units start{};
            Units end{};
            bool first{};
            bool last{};
        };

        // A line's leaf in logical order: a placed piece, the placeholder of a box fragment where the box opens on
        // the line (Blink's box placeholder), or a bidi control (control: its item; Blink keeps them on the line as
        // children of no size at their levels); and the innermost fragment it lies in (none: the root's).
        struct Leaf {
            std::size_t piece{none};
            std::size_t fragment{none};
            std::size_t parent{none};
            std::size_t control{none};
        };

        // The base direction of the line that starts at chunk `first`: its paragraph's.
        bool line_base_rtl(std::size_t first) const {
            if (!bidi || paragraph_levels.paragraphs.empty()) {
                return false;
            }
            auto at = items[chunks[first].item].begin + chunks[first].begin;
            for (auto u = first; u < chunks.size(); ++u) {
                const auto kind = items[chunks[u].item].kind;
                if (kind == Item::Kind::Text || kind == Item::Kind::Atomic) {
                    at = items[chunks[u].item].begin + chunks[u].begin;
                    break;
                }
            }
            std::uint8_t level = paragraph_levels.paragraphs.front().second;
            for (const auto& [start, base] : paragraph_levels.paragraphs) {
                if (start > at) {
                    break;
                }
                level = base;
            }
            return level % 2;
        }

        // Places a line in visual order, as Blink's LogicalLineBuilder::BidiReorder and InlineLayoutStateStack do: its
        // leaves reordered by their levels (UAX #9 L2; a box placeholder takes the level of the leaf after it, at the
        // line's end the line's base level), each box fragment split into the ranges of its content that stay
        // together, the box's line-left edge (its start edge, its end edge when its direction is rtl) on its leftmost
        // range and its line-right edge on its rightmost, and each leaf placed left to right after the edges before it.
        // Rewrites the pieces' x and the fragments (one per range, with physical edges); returns the line's width.
        Units place_visually(bool line_rtl, const std::vector<SceneAtomicExtent>& extents, std::vector<Placed>& placed,
                             const std::vector<Leaf>& sequence, std::vector<Fragment>& fragments,
                             const std::vector<std::size_t>& fragment_parent,
                             const std::vector<std::size_t>& fragment_opened,
                             std::vector<float>& fragment_shift) const {
            constexpr std::uint8_t opaque = 0xff;
            const std::uint8_t base = line_rtl ? 1 : 0;
            std::vector<std::uint8_t> leaf_levels(sequence.size(), opaque);
            for (std::size_t i = 0; i < sequence.size(); ++i) {
                if (sequence[i].piece != none) {
                    leaf_levels[i] = bidi ? items[placed[sequence[i].piece].item].level : 0;
                } else if (sequence[i].control != none) {
                    leaf_levels[i] = items[sequence[i].control].level;
                }
            }
            auto after = base;
            for (auto i = leaf_levels.size(); i-- > 0;) {
                if (leaf_levels[i] == opaque) {
                    leaf_levels[i] = after;
                } else {
                    after = leaf_levels[i];
                }
            }
            const auto visual = bidi_visual_order(leaf_levels);
            const auto inside = [&](const Leaf& leaf, std::size_t f) {
                for (auto g = leaf.parent; g != none; g = fragment_parent[g]) {
                    if (g == f) {
                        return true;
                    }
                }
                return false;
            };

            struct Range {
                std::size_t fragment{};
                std::size_t first{};
                std::size_t last{};
                bool left{};
                bool right{};
                Units start{};
                Units end{};
            };

            std::vector<Range> ranges;
            std::vector<std::size_t> empty;
            for (std::size_t f = 0; f < fragments.size(); ++f) {
                const auto from = ranges.size();
                for (std::size_t at = 0; at < visual.size(); ++at) {
                    if (!inside(sequence[visual[at]], f)) {
                        continue;
                    }
                    if (ranges.size() > from && ranges.back().last + 1 == at) {
                        ranges.back().last = at;
                    } else {
                        ranges.push_back({f, at, at});
                    }
                }
                if (ranges.size() > from) {
                    const bool rtl = boxes[fragments[f].box].rtl;
                    ranges[from].left = rtl ? fragments[f].last : fragments[f].first;
                    ranges.back().right = rtl ? fragments[f].first : fragments[f].last;
                } else {
                    // A culled box with nothing on the line: no width, before the leaf that follows where it opens.
                    empty.push_back(f);
                }
            }
            // At each leaf, the ranges that start there outermost first, and those that end there innermost first.
            std::vector<std::vector<std::size_t>> starting(visual.size());
            std::vector<std::vector<std::size_t>> ending(visual.size());
            for (std::size_t r = 0; r < ranges.size(); ++r) {
                starting[ranges[r].first].push_back(r);
                ending[ranges[r].last].push_back(r);
            }
            const auto depth = [&](std::size_t r) {
                return boxes[fragments[ranges[r].fragment].box].depth;
            };
            Units x{};
            for (std::size_t at = 0; at < visual.size(); ++at) {
                std::stable_sort(starting[at].begin(), starting[at].end(), [&](std::size_t a, std::size_t b) {
                    return depth(a) < depth(b);
                });
                for (const auto r : starting[at]) {
                    auto& range = ranges[r];
                    const auto& b = boxes[fragments[range.fragment].box];
                    if (range.left) {
                        x += to_units(b.rtl ? b.margin_end : b.margin_start);
                    }
                    range.start = x;
                    if (range.left) {
                        x += to_units(b.rtl ? b.inner_end : b.inner_start);
                    }
                }
                const auto& leaf = sequence[visual[at]];
                if (leaf.piece != none) {
                    auto& p = placed[leaf.piece];
                    if (p.piece.kind == SceneInlinePiece::Kind::Atomic) {
                        const auto& e = extents[p.item];
                        x += to_units(e.margin[3]);
                        p.x = x;
                        x += p.w + to_units(e.margin[1]);
                    } else {
                        p.x = x;
                        x += p.w;
                    }
                }
                std::stable_sort(ending[at].begin(), ending[at].end(), [&](std::size_t a, std::size_t b) {
                    return depth(a) > depth(b);
                });
                for (const auto r : ending[at]) {
                    auto& range = ranges[r];
                    const auto& b = boxes[fragments[range.fragment].box];
                    if (range.right) {
                        x += to_units(b.rtl ? b.inner_start : b.inner_end);
                    }
                    range.end = x;
                    if (range.right) {
                        x += to_units(b.rtl ? b.margin_start : b.margin_end);
                    }
                }
            }
            std::vector<Fragment> split;
            std::vector<float> shift;
            for (const auto& range : ranges) {
                split.push_back({fragments[range.fragment].box, range.start, range.end, range.left, range.right});
                shift.push_back(fragment_shift[range.fragment]);
            }
            // The left of each leaf, by its index in the sequence.
            std::vector<Units> leaf_x(sequence.size() + 1, x);
            {
                Units at{};
                for (std::size_t v = 0; v < visual.size(); ++v) {
                    const auto& leaf = sequence[visual[v]];
                    if (leaf.piece != none) {
                        at = placed[leaf.piece].x;
                    }
                    leaf_x[visual[v]] = at;
                }
            }
            for (const auto f : empty) {
                const auto at = leaf_x[std::min(fragment_opened[f], sequence.size())];
                split.push_back({fragments[f].box, at, at, false, false});
                shift.push_back(fragment_shift[f]);
            }
            fragments = std::move(split);
            fragment_shift = std::move(shift);
            return x;
        }

        // text-overflow: ellipsis on a line of pieces wider than limit (line_width wide), as Blink's LineTruncator
        // truncates it (CSS Overflow 3, "text-overflow"): in visual order from the line's end (its left when it is
        // right to left), each piece that starts past the content box is hidden; the last one that reaches into it
        // keeps the characters that fit before the ellipsis (from its logical start when it runs the line's direction,
        // else from its end), or all of it when it fits, and the ellipsis follows on the line's end side. The first
        // piece is never hidden: when not even its first character fits, it keeps that character and is clipped.
        // Hidden text draws nothing but keeps its rect, so geometry stays as laid out; on a right-to-left line a
        // truncated piece's rect shrinks to what it keeps, at its right. The ellipsis piece, or nothing when no piece
        // can take it.
        std::optional<Placed> truncate(std::vector<Placed>& placed, Units limit, bool line_rtl,
                                       Units line_width) const {
            float advance{};
            for (const auto& g : ellipsis_run->glyphs) {
                advance += g.advance;
            }
            const Units width = snapped(advance);
            const auto hide = [&](Placed& p) {
                if (p.piece.kind != SceneInlinePiece::Kind::Text) {
                    fail("text-overflow: ellipsis hiding an atomic inline outside scene profile");
                }
                p.piece.text.clear();
                p.piece.run.reset();
            };
            // The pieces from the line's end to its start, in visual order: from the right on a left-to-right line,
            // from the left on a right-to-left one.
            std::vector<std::size_t> order(placed.size());
            for (std::size_t k = 0; k < order.size(); ++k) {
                order[k] = k;
            }
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return line_rtl ? placed[a].x < placed[b].x : placed[a].x > placed[b].x;
            });
            for (std::size_t i = 0; i < order.size(); ++i) {
                auto& p = placed[order[i]];
                const bool first = i + 1 == order.size();
                // Its distance from the line's start.
                const Units offset = line_rtl ? line_width - (p.x + p.w) : p.x;
                Units space = limit - offset;
                if (space <= 0) {
                    if (!first) {
                        hide(p);
                    }
                    continue;
                }
                space -= width;
                // The edge the ellipsis follows: the piece's end side.
                Units end = line_rtl ? p.x : p.x + p.w;
                if (space < p.w) {
                    if ((space <= 0 && !first) || (p.piece.kind != SceneInlinePiece::Kind::Text && !first)) {
                        hide(p);
                        continue;
                    }
                    if (p.piece.kind == SceneInlinePiece::Kind::Text) {
                        // The most characters whose advances fit, at a cluster boundary: from its logical start when
                        // it runs the line's way, else from its logical end (its part at the line's start either way).
                        const auto& item = items[p.item];
                        const bool from_start = (p.piece.bidi && p.piece.rtl) == line_rtl;
                        const float room = pixels(space);
                        const auto kept = [&](std::size_t a, std::size_t b) {
                            return item.prefix[b] - item.prefix[a];
                        };
                        std::size_t fit = from_start ? p.begin : p.end;
                        for (auto b = p.begin + 1; b < p.end; ++b) {
                            if (item.starts[b] && (from_start ? kept(p.begin, b) : kept(b, p.end)) <= room &&
                                (from_start || fit == p.end)) {
                                fit = b;
                            }
                        }
                        if (from_start && item.starts[p.end] && kept(p.begin, p.end) <= room) {
                            fit = p.end;
                        }
                        const bool none_fits = from_start ? fit == p.begin : fit == p.end;
                        const bool all_fit = from_start ? fit == p.end : fit == p.begin;
                        if (none_fits || all_fit) {
                            if (!first) {
                                hide(p);
                                continue;
                            }
                            // The first character is clipped rather than ellipsed; a piece that fits by its advances
                            // but not by its snapped width loses its last.
                            if (from_start) {
                                auto cut = none_fits ? p.begin + 1 : p.end - 1;
                                while (cut > p.begin + 1 && cut < p.end && !item.starts[cut]) {
                                    none_fits ? ++cut : --cut;
                                }
                                fit = std::clamp(cut, p.begin + 1, p.end);
                            } else {
                                auto cut = none_fits ? p.end - 1 : p.begin + 1;
                                while (cut > p.begin && cut < p.end - 1 && !item.starts[cut]) {
                                    none_fits ? --cut : ++cut;
                                }
                                fit = std::clamp(cut, p.begin, p.end - 1);
                            }
                        }
                        const auto a = from_start ? p.begin : fit;
                        const auto b = from_start ? fit : p.end;
                        p.piece.text = item.text.substr(a, b - a);
                        if (p.piece.run) {
                            p.piece.run = part(*item.run, a, b,
                                               text.slice(*item.run, item.font, std::uint32_t(a), std::uint32_t(b)));
                        }
                        const Units kept_width = snapped(kept(a, b));
                        if (line_rtl) {
                            // What is kept stays at the piece's right, the line's start.
                            p.x += p.w - kept_width;
                            p.w = kept_width;
                            end = p.x;
                        } else {
                            end = p.x + kept_width;
                        }
                    }
                }
                Placed e;
                e.piece.kind = SceneInlinePiece::Kind::Text;
                e.piece.node = &root;
                e.piece.text = ellipsis;
                e.piece.bidi = bidi;
                e.piece.rtl = bidi && line_rtl;
                e.x = line_rtl ? end - width : end;
                e.w = width;
                e.top = ellipsis_run->metrics.baseline;
                e.height = ellipsis_run->metrics.height;
                return e;
            }
            return std::nullopt;
        }

        // A box aligned top or bottom (with its subtree) that waits for the extent of its nearest ancestor aligned so,
        // or of the line box: its pieces and fragments [first, end), its extent about its baseline, and its own such
        // descendants.
        struct Pending {
            Align::Kind kind{};
            Extent extent;
            std::size_t first_piece{};
            std::size_t end_piece{};
            std::size_t first_fragment{};
            std::size_t end_fragment{};
            std::vector<Pending> pending;
        };

        // An inline-level box while a line is placed, as Blink's InlineLayoutStateStack keeps it: the line's root
        // (box none, node null), an inline box, or an atomic inline (box none). Its extent about its own baseline holds
        // its strut and the content aligned with it so far; its pieces and fragments are those from first_piece and
        // first_fragment on when it closes.
        struct Level {
            std::size_t box{none};
            const SceneNode* node{};
            Align align;
            Extent extent;
            std::size_t first_piece{};
            std::size_t first_fragment{};
            std::vector<Pending> pending;
        };

        // The font and strut of a level's box: the root's for the root.
        const SceneFont& level_font(const Level& l) const {
            return l.box == none ? root_font : boxes[l.box].font;
        }

        const SceneStrut& level_strut(const Level& l) const {
            return l.box == none ? root_strut : boxes[l.box].strut;
        }

        // How far a box's baseline lies below its parent's (Blink's ApplyBaselineShift), in LayoutUnits as Blink takes
        // them; none for top and bottom, which wait for the extent of a box around. own: the box's extent, its
        // content's included; line_height: its computed line-height (percentages).
        std::optional<float> baseline_shift(const Align& a, const Extent& own, float line_height, const Level& parent) {
            using K = Align::Kind;
            const auto& font = level_font(parent);
            const auto& strut = level_strut(parent);
            const float computed = parent.box == none ? computed_font_size(root.style) : boxes[parent.box].font_size;
            const auto size = Units(std::lround(computed * layout_units_per_px));
            const auto one = Units(layout_units_per_px);
            switch (a.kind) {
                case K::Baseline:
                    return 0.f;
                case K::Sub:
                    return pixels(size / constants::scene::subscript_font_size_divisor + one);
                case K::Super:
                    return -pixels(size / constants::scene::superscript_font_size_divisor + one);
                case K::Length:
                    return -pixels(to_units(a.value));
                case K::Percent:
                    return -pixels(to_units(line_height * a.value / float(constants::percent_scale)));
                case K::Middle:
                    return pixels((exact(own.ascent) - exact(own.descent)) / 2) -
                           rounded_units(text.x_height(font) / 2);
                case K::TextTop:
                    return own.ascent - strut.ascent;
                case K::TextBottom:
                    return strut.descent - own.descent;
                case K::Top:
                case K::Bottom:
                    return std::nullopt;
            }
            return 0.f;
        }

        // A box's computed line-height in px: its line-height, or its primary font's normal line spacing.
        float line_height_of(const SceneNode& node) {
            const auto font = scene_font(node.style);
            if (font.line_height) {
                return *font.line_height;
            }
            const auto strut = text.strut(font);
            return strut.above + strut.below;
        }

        static void move(std::vector<Placed>& placed, std::vector<float>& fragment_shift, std::size_t first_piece,
                         std::size_t end_piece, std::size_t first_fragment, std::size_t end_fragment, float shift) {
            for (auto k = first_piece; k < end_piece; ++k) {
                placed[k].shift += shift;
            }
            for (auto k = first_fragment; k < end_fragment; ++k) {
                fragment_shift[k] += shift;
            }
        }

        // Aligns a box that waited for the extent of the box around (its top with that one's top, or its bottom with
        // its bottom), after its own waiting descendants, and unites it into that extent.
        static void align_pending(Pending& p, Extent& around, std::vector<Placed>& placed,
                                  std::vector<float>& fragment_shift) {
            for (auto& d : p.pending) {
                align_pending(d, p.extent, placed, fragment_shift);
            }
            const float shift =
                p.kind == Align::Kind::Top ? p.extent.ascent - around.ascent : around.descent - p.extent.descent;
            move(placed, fragment_shift, p.first_piece, p.end_piece, p.first_fragment, p.end_fragment, shift);
            p.extent.move(shift);
            around.unite(p.extent);
        }

        // intrinsic: the lines that give the min-content width, which overflow-wrap: break-word does not narrow.
        SceneInlineLayout layout(float available, SceneAtomicLayout atomic, bool intrinsic) {
            if (std::isnan(available) || available < 0) {
                fail("inline available width");
            }
            resolve_edges(available);
            std::vector<SceneAtomicExtent> extents(items.size());
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].kind == Item::Kind::Atomic) {
                    extents[i] = atomic(*items[i].node, available);
                }
            }
            const bool bounded = std::isfinite(available);
            // A laid-out content width (the box less its borders and padding, in floats): to the nearest LayoutUnit,
            // or float error takes a unit off the max-content width a shrink-to-fit box was sized to and its last
            // word wraps (Blink keeps the content box in LayoutUnits throughout).
            const Units limit = bounded ? exact(available) : std::numeric_limits<Units>::max();
            // Greedy filling: lines as ranges of chunks.
            std::vector<std::pair<std::size_t, std::size_t>> lines;
            std::size_t start = 0;
            bool filled{};
            for (std::size_t i = 0; i < chunks.size();) {
                auto j = i + 1;
                while (j < chunks.size() && !chunks[j].break_before &&
                       items[chunks[j - 1].item].kind != Item::Kind::Break) {
                    ++j;
                }
                if (filled && width(start, j, true, extents) > limit) {
                    lines.emplace_back(start, i);
                    start = i;
                    filled = false;
                }
                if (!filled && emergencies && bounded) {
                    start = rescue(start, i, j, limit, extents, intrinsic, lines);
                }
                const bool forced = items[chunks[j - 1].item].kind == Item::Kind::Break;
                // The end edges and bidi pops right after a forced break end its line, as Blink's line breaker keeps
                // them (its trailing items).
                while (forced && j < chunks.size() &&
                       (items[chunks[j].item].kind == Item::Kind::Close ||
                        (items[chunks[j].item].kind == Item::Kind::Control && items[chunks[j].item].exit))) {
                    ++j;
                }
                for (auto u = i; u < j; ++u) {
                    filled |= content(chunks[u]);
                }
                i = j;
                if (forced) {
                    lines.emplace_back(start, j);
                    start = j;
                    filled = false;
                }
            }
            if (start < chunks.size()) {
                lines.emplace_back(start, chunks.size());
            }
            SceneInlineLayout out;
            const auto align = root.style.text(PropertyId::TextAlign, "start");
            // The boxes open at the start of the current line.
            std::vector<std::size_t> open;
            float top{};
            for (const auto& [first, last] : lines) {
                bool any = std::any_of(chunks.begin() + std::ptrdiff_t(first), chunks.begin() + std::ptrdiff_t(last),
                                       [&](const Chunk& u) {
                                           return content(u);
                                       });
                if (!any) {
                    // Only collapsed spaces and empty inline boxes: no line box. Boxes still open and close.
                    for (auto u = first; u < last; ++u) {
                        const auto& item = items[chunks[u].item];
                        if (item.kind == Item::Kind::Open) {
                            open.push_back(item.box);
                        } else if (item.kind == Item::Kind::Close && !open.empty()) {
                            open.pop_back();
                        }
                    }
                    continue;
                }
                const auto end = trim(first, last);
                // The line's base direction: its paragraph's (each paragraph's own with unicode-bidi: plaintext).
                const bool line_rtl = line_base_rtl(first);

                std::vector<Fragment> fragments;
                std::vector<Placed> placed;
                // Each fragment's baseline offset from the line's.
                std::vector<float> fragment_shift;
                // For reordering: the fragment each fragment lies in (none: the root's), and the line's leaves and box
                // placeholders in logical order.
                std::vector<std::size_t> fragment_parent;
                // Where in the sequence each fragment opens (a culled box's has no placeholder there).
                std::vector<std::size_t> fragment_opened;
                std::vector<Leaf> sequence;
                // The fragments of the boxes open at this point of the line, outermost first.
                std::vector<std::size_t> stack;
                const auto open_fragment = [&](std::size_t b, Units at, bool starts) {
                    const auto parent = stack.empty() ? none : stack.back();
                    if (boxes[b].fragment) {
                        sequence.push_back({none, fragments.size(), fragments.size()});
                    }
                    fragment_opened.push_back(sequence.size());
                    fragment_parent.push_back(parent);
                    stack.push_back(fragments.size());
                    fragments.push_back({b, at, at, starts, false});
                    fragment_shift.push_back(0);
                };
                // The boxes open on the line, the root first; each starts with its strut.
                const auto strut_extent = [](const SceneStrut& strut) {
                    Extent e;
                    e.ascent = strut.above;
                    e.descent = strut.below;
                    return e;
                };
                std::vector<Level> levels;
                levels.push_back({none, nullptr, {}, strut_extent(root_strut), 0, 0, {}});
                // Fragments of the boxes that continue from the line before start at the line's start.
                for (const auto b : open) {
                    levels.push_back({b,
                                      boxes[b].node,
                                      boxes[b].align,
                                      strut_extent(boxes[b].strut),
                                      placed.size(),
                                      fragments.size(),
                                      {}});
                    open_fragment(b, 0, false);
                }
                // Closes the innermost level: shifts its baseline from its parent's as its vertical-align says and
                // unites its extent into its parent's, or leaves it to wait (top, bottom) for its nearest ancestor
                // aligned so, or for the line box.
                const auto close_level = [&] {
                    Level child = std::move(levels.back());
                    levels.pop_back();
                    auto& parent = levels.back();
                    const float line_height =
                        child.align.kind == Align::Kind::Percent && child.node ? line_height_of(*child.node) : 0.f;
                    if (const auto shift = baseline_shift(child.align, child.extent, line_height, parent)) {
                        for (auto& d : child.pending) {
                            align_pending(d, child.extent, placed, fragment_shift);
                        }
                        move(placed, fragment_shift, child.first_piece, placed.size(), child.first_fragment,
                             fragments.size(), *shift);
                        child.extent.move(*shift);
                        parent.extent.unite(child.extent);
                        return;
                    }
                    auto at = levels.size() - 1;
                    while (at > 0 && levels[at].align.kind != Align::Kind::Top &&
                           levels[at].align.kind != Align::Kind::Bottom) {
                        --at;
                    }
                    levels[at].pending.push_back({child.align.kind, child.extent, child.first_piece, placed.size(),
                                                  child.first_fragment, fragments.size(), std::move(child.pending)});
                };
                Units x{};
                // The text fragment being collected: its item, first byte and raw advances.
                std::size_t fragment_item = none;
                std::size_t fragment_begin{};
                std::size_t fragment_end{};
                float raw{};
                Units fragment_x{};
                const auto flush = [&] {
                    if (fragment_item == none) {
                        return;
                    }
                    const auto& item = items[fragment_item];
                    const Units w = snapped(raw);
                    if (fragment_end > fragment_begin) {
                        Placed p;
                        p.piece.kind = SceneInlinePiece::Kind::Text;
                        p.piece.node = item.node;
                        p.piece.text = item.text.substr(fragment_begin, fragment_end - fragment_begin);
                        const auto m = text.slice(*item.run, item.font, std::uint32_t(fragment_begin),
                                                  std::uint32_t(fragment_end));
                        p.x = fragment_x;
                        p.w = w;
                        p.item = fragment_item;
                        p.begin = fragment_begin;
                        p.end = fragment_end;
                        p.piece.bidi = bidi;
                        p.piece.rtl = bidi && item.level % 2;
                        const auto context = context_at(item.begin + fragment_begin, item.begin + fragment_end);
                        p.piece.context_before = context.before;
                        p.piece.context_after = context.after;
                        if (item.joint) {
                            p.piece.run = part(*item.run, fragment_begin, fragment_end, m);
                        }
                        sequence.push_back({placed.size(), none, stack.empty() ? none : stack.back()});
                        p.above = p.top = m.baseline;
                        p.below = m.height - m.baseline;
                        p.height = m.height;
                        Extent e;
                        e.ascent = p.above;
                        e.descent = p.below;
                        levels.back().extent.unite(e);
                        placed.push_back(std::move(p));
                    }
                    x = fragment_x + w;
                    fragment_item = none;
                    raw = 0;
                };
                // The end of the last content inside each box on this line.
                const auto reach = [&](Units at) {
                    for (const auto f : stack) {
                        fragments[f].end = std::max(fragments[f].end, at);
                    }
                };
                for (auto u = first; u < last; ++u) {
                    const auto& unit = chunks[u];
                    const auto& item = items[unit.item];
                    if (item.kind == Item::Kind::Text) {
                        const auto stop = u == end.unit                        ? end.end
                                          : (end.unit != none && u > end.unit) ? unit.begin
                                                                               : unit.end;
                        if (unit.item != fragment_item) {
                            flush();
                            fragment_item = unit.item;
                            fragment_begin = unit.begin;
                            fragment_x = x;
                        }
                        fragment_end = std::max(fragment_begin, stop);
                        if (stop > unit.begin) {
                            raw += item.prefix[stop] - item.prefix[unit.begin];
                        }
                        reach(fragment_x + snapped(raw));
                        continue;
                    }
                    flush();
                    if (item.kind == Item::Kind::Open) {
                        const auto& b = boxes[item.box];
                        x += to_units(b.margin_start);
                        levels.push_back(
                            {item.box, b.node, b.align, strut_extent(b.strut), placed.size(), fragments.size(), {}});
                        open_fragment(item.box, x, true);
                        x += to_units(b.inner_start);
                        reach(x);
                        open.push_back(item.box);
                    } else if (item.kind == Item::Kind::Close) {
                        const auto& b = boxes[item.box];
                        x += to_units(b.inner_end);
                        reach(x);
                        if (!stack.empty()) {
                            // The box ends where its end edge lies, which the negative margin of an atomic inline
                            // in it may have taken back past content.
                            fragments[stack.back()].end = x;
                            fragments[stack.back()].last = true;
                            stack.pop_back();
                        }
                        if (!open.empty()) {
                            open.pop_back();
                        }
                        if (levels.size() > 1) {
                            close_level();
                        }
                        x += to_units(b.margin_end);
                    } else if (item.kind == Item::Kind::Control) {
                        sequence.push_back({none, none, stack.empty() ? none : stack.back(), unit.item});
                    } else if (item.kind == Item::Kind::Atomic) {
                        const auto& e = extents[unit.item];
                        x += to_units(e.margin[3]);
                        Placed p;
                        p.piece.kind = SceneInlinePiece::Kind::Atomic;
                        p.piece.node = item.node;
                        p.item = unit.item;
                        sequence.push_back({placed.size(), none, stack.empty() ? none : stack.back()});
                        p.x = x;
                        p.w = exact(e.width);
                        p.above = e.margin[0] + e.baseline;
                        p.below = e.margin[0] + e.height + e.margin[2] - p.above;
                        p.top = e.baseline;
                        p.height = e.height;
                        Extent margin_box;
                        margin_box.ascent = p.above;
                        margin_box.descent = p.below;
                        x += p.w;
                        reach(x);
                        x += to_units(e.margin[1]);
                        levels.push_back(
                            {none, item.node, item.align, margin_box, placed.size(), fragments.size(), {}});
                        placed.push_back(std::move(p));
                        close_level();
                    }
                }
                flush();
                // The boxes that continue on the next line close here; then the boxes aligned top or bottom take their
                // places in the line box.
                while (levels.size() > 1) {
                    close_level();
                }
                auto& line = levels.front();
                for (auto& d : line.pending) {
                    align_pending(d, line.extent, placed, fragment_shift);
                }
                const float above = line.extent.ascent;
                const float below = line.extent.descent;
                // In visual order: each leaf at its place left to right, each box split where reordering separates
                // its content, its edges at its leftmost and rightmost pieces.
                if (reorder) {
                    x = place_visually(line_rtl, extents, placed, sequence, fragments, fragment_parent, fragment_opened,
                                       fragment_shift);
                }
                // The ellipsis sits on the line's baseline without changing its height, and the line keeps its width.
                if (!ellipsis.empty() && bounded && x > limit) {
                    if (auto e = truncate(placed, limit, line_rtl, x)) {
                        placed.push_back(std::move(*e));
                    }
                }
                // text-align: start and end by the line's direction; a line that overflows is start-aligned.
                Units offset{};
                if (bounded) {
                    const bool right =
                        align == "right" || (align == "start" && line_rtl) || (align == "end" && !line_rtl);
                    if (x >= limit) {
                        offset = line_rtl ? limit - x : 0;
                    } else if (align == "center") {
                        offset = (limit - x) / 2;
                    } else if (right) {
                        offset = limit - x;
                    }
                }
                const float baseline = top + above;
                for (auto& p : placed) {
                    p.piece.rect = {pixels(offset + p.x), baseline + p.shift - p.top, pixels(p.w), p.height};
                    p.piece.baseline = p.top;
                    out.pieces.push_back(std::move(p.piece));
                }
                for (std::size_t k = 0; k < fragments.size(); ++k) {
                    const auto& f = fragments[k];
                    const auto& b = boxes[f.box];
                    SceneInlinePiece piece;
                    piece.kind = SceneInlinePiece::Kind::Box;
                    piece.node = b.node;
                    piece.first = f.first;
                    piece.last = f.last;
                    piece.rect = {pixels(offset + f.start), baseline + fragment_shift[k] - b.strut.ascent - b.top,
                                  pixels(f.end - f.start), b.strut.ascent + b.strut.descent + b.top + b.bottom};
                    out.pieces.push_back(std::move(piece));
                }
                if (!out.lines) {
                    out.baseline = baseline;
                }
                out.last_baseline = baseline;
                out.width = std::max(out.width, pixels(x));
                top += above + below;
                ++out.lines;
            }
            out.height = top;
            return out;
        }
    };

    SceneInlineContent::SceneInlineContent(SceneText& text, const SceneNode& root, std::size_t first, std::size_t last)
        : impl_(std::make_unique<Impl>(text, root, first, last)) {
    }

    SceneInlineContent::~SceneInlineContent() = default;

    SceneInlineLayout SceneInlineContent::layout(float available, SceneAtomicLayout atomic) {
        return impl_->layout(available, atomic, false);
    }

    float SceneInlineContent::min_content(SceneAtomicLayout atomic) {
        return impl_->layout(0, atomic, true).width;
    }

    bool SceneInlineContent::breakable() const {
        return impl_->breakable;
    }
} // namespace weft
