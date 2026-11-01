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
        };

        struct Item {
            enum class Kind : std::uint8_t {
                Text,
                Open,
                Close,
                Atomic,
                Break,
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

    bool scene_atomic_inline(const SceneNode& n) {
        if (n.space == constants::svg_namespace_uri) {
            return true;
        }
        const auto d = display(n);
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

        Impl(SceneText& t, const SceneNode& r) : text(t), root(r) {
            root_font = scene_font(root.style);
            root_strut = text.strut(root_font);
            const auto align = root.style.text(PropertyId::TextAlign, "left");
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
            collect(root, none);
            if (paragraph.size() > std::size_t(constants::max_text_bytes)) {
                fail("scene paragraph byte bound");
            }
            split();
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
                    item.begin = paragraph.size();
                    paragraph += '\n';
                    space_before = true;
                    items.push_back(std::move(item));
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
            item.font = scene_font(owner.style);
            item.run = text.shape(item.text, item.font);
            std::vector<float> at(item.text.size() + 1);
            for (const auto& g : item.run->glyphs) {
                at[std::min<std::size_t>(g.utf8_cluster, item.text.size())] += g.advance;
            }
            item.prefix.assign(item.text.size() + 1, 0);
            for (std::size_t k = 0; k < item.text.size(); ++k) {
                item.prefix[k + 1] = item.prefix[k] + at[k];
            }
            item.starts.assign(item.text.size() + 1, false);
            for (const auto& g : item.run->glyphs) {
                item.starts[std::min<std::size_t>(g.utf8_cluster, item.text.size())] = true;
            }
            item.starts.back() = true;
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
            for (const auto& c : n.children) {
                if (display(c) == "none") {
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
                    paragraph += '\n';
                    space_before = true;
                    items.push_back(std::move(item));
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

        // The chunks and their wrap opportunities: UAX 14 over the whole paragraph as word-break tailors it, a soft
        // opportunity where the nearest common ancestor of the characters on either side allows wrapping (CSS Text 3,
        // 5.1), a forced one after <br>. An opportunity at an inline box's edge lies after the end edges there and
        // before the start edges. A text whose overflow-wrap breaks words splits them into its grapheme clusters too,
        // each after the first starting at an emergency opportunity.
        void split() {
            std::vector<std::size_t> owner(paragraph.size(), none);
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = items[i];
                const auto size = item.kind == Item::Kind::Text     ? item.text.size()
                                  : item.kind == Item::Kind::Atomic ? object_replacement.size()
                                  : item.kind == Item::Kind::Break  ? std::size_t(1)
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
            const auto add = [&](std::size_t item, std::size_t begin, std::size_t end, std::size_t at, bool closing,
                                 Emergency emergency) {
                Chunk u{item, begin, end};
                // The first unit at an opportunity that is not an end edge takes it.
                if (!closing && at != placed && opportunity[at]) {
                    u.break_before = true;
                    placed = at;
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
                b.margin_start = edge(s, PropertyId::MarginLeft, base);
                b.margin_end = edge(s, PropertyId::MarginRight, base);
                b.inner_start = border(PropertyId::BorderLeftWidth, PropertyId::BorderLeftStyle) +
                                edge(s, PropertyId::PaddingLeft, base);
                b.inner_end = border(PropertyId::BorderRightWidth, PropertyId::BorderRightStyle) +
                              edge(s, PropertyId::PaddingRight, base);
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
                if (item.kind == Item::Kind::Open || item.kind == Item::Kind::Close || item.kind == Item::Kind::Break) {
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

        // The width of chunks [first, last) on one line, without the line's trailing spaces when `hang`.
        Units width(std::size_t first, std::size_t last, bool hang,
                    const std::vector<SceneAtomicExtent>& extents) const {
            const auto end = hang ? trim(first, last) : Trim{};
            Advance a;
            for (auto u = first; u < last; ++u) {
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

        // text-overflow: ellipsis on a line of pieces wider than limit, as Blink's LineTruncator truncates it (CSS
        // Overflow 3, "text-overflow"): from the line's end, each piece that starts past the content box is hidden; the
        // last one that reaches into it keeps the characters that fit before the ellipsis, or all of it when it fits,
        // and the ellipsis follows. The first piece is never hidden: when not even its first character fits, it keeps
        // that character and is clipped. Hidden text draws nothing but keeps its rect, so geometry stays as laid out.
        // The ellipsis piece, or nothing when no piece can take it.
        std::optional<Placed> truncate(std::vector<Placed>& placed, Units limit) const {
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
            };
            for (auto k = placed.size(); k-- > 0;) {
                auto& p = placed[k];
                const bool first = k == 0;
                Units space = limit - p.x;
                if (space <= 0) {
                    if (!first) {
                        hide(p);
                    }
                    continue;
                }
                space -= width;
                Units end = p.x + p.w;
                if (space < p.w) {
                    if ((space <= 0 && !first) || (p.piece.kind != SceneInlinePiece::Kind::Text && !first)) {
                        hide(p);
                        continue;
                    }
                    if (p.piece.kind == SceneInlinePiece::Kind::Text) {
                        // The most characters whose advances fit, at a cluster boundary.
                        const auto& item = items[p.item];
                        const float room = pixels(space);
                        auto fit = p.begin;
                        for (auto b = p.begin + 1; b <= p.end; ++b) {
                            if (item.starts[b] && item.prefix[b] - item.prefix[p.begin] <= room) {
                                fit = b;
                            }
                        }
                        if (fit == p.begin || fit == p.end) {
                            if (!first) {
                                hide(p);
                                continue;
                            }
                            // The first character is clipped rather than ellipsed; a piece that fits by its advances
                            // but not by its snapped width loses its last.
                            auto cut = fit == p.begin ? p.begin + 1 : p.end - 1;
                            while (cut > p.begin + 1 && cut < p.end && !item.starts[cut]) {
                                fit == p.begin ? ++cut : --cut;
                            }
                            fit = std::clamp(cut, p.begin + 1, p.end);
                        }
                        p.piece.text = item.text.substr(p.begin, fit - p.begin);
                        end = p.x + snapped(item.prefix[fit] - item.prefix[p.begin]);
                    }
                }
                Placed e;
                e.piece.kind = SceneInlinePiece::Kind::Text;
                e.piece.node = &root;
                e.piece.text = ellipsis;
                e.x = end;
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
                for (auto u = i; u < j; ++u) {
                    filled |= content(chunks[u]);
                }
                i = j;
                if (items[chunks[j - 1].item].kind == Item::Kind::Break) {
                    lines.emplace_back(start, j);
                    start = j;
                    filled = false;
                }
            }
            if (start < chunks.size()) {
                lines.emplace_back(start, chunks.size());
            }
            SceneInlineLayout out;
            const auto align = root.style.text(PropertyId::TextAlign, "left");
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

                struct Fragment {
                    std::size_t box{};
                    Units start{};
                    Units end{};
                    bool first{};
                    bool last{};
                };

                std::vector<Fragment> fragments;
                std::vector<Placed> placed;
                // Each fragment's baseline offset from the line's.
                std::vector<float> fragment_shift;
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
                std::vector<std::size_t> stack;
                for (const auto b : open) {
                    levels.push_back({b,
                                      boxes[b].node,
                                      boxes[b].align,
                                      strut_extent(boxes[b].strut),
                                      placed.size(),
                                      fragments.size(),
                                      {}});
                    stack.push_back(fragments.size());
                    fragments.push_back({b, 0, 0, false, false});
                    fragment_shift.push_back(0);
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
                        stack.push_back(fragments.size());
                        fragments.push_back({item.box, x, x, true, false});
                        fragment_shift.push_back(0);
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
                    } else if (item.kind == Item::Kind::Atomic) {
                        const auto& e = extents[unit.item];
                        x += to_units(e.margin[3]);
                        Placed p;
                        p.piece.kind = SceneInlinePiece::Kind::Atomic;
                        p.piece.node = item.node;
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
                // The ellipsis sits on the line's baseline without changing its height, and the line keeps its width.
                if (!ellipsis.empty() && bounded && x > limit) {
                    if (auto e = truncate(placed, limit)) {
                        placed.push_back(std::move(*e));
                    }
                }
                Units offset{};
                if (bounded && x < limit) {
                    if (align == "center") {
                        offset = (limit - x) / 2;
                    } else if (align == "right" || align == "end") {
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

    SceneInlineContent::SceneInlineContent(SceneText& text, const SceneNode& root)
        : impl_(std::make_unique<Impl>(text, root)) {
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
