#include "layout/document.hpp"
#include "base/css_scalar.hpp"
#include "base/rect.hpp"
#include "constants/layout.hpp"
#include "dom/attributes.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <optional>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace weft {
    namespace {
        using Style = StyleValues;
        constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();

        using Rect = geometry::Rect;
        using geometry::intersect;

        [[noreturn]] void fail(const std::string& s) {
            throw_diagnostic(Diag::DocumentProfile, s);
        }

        std::string get(const Style& s, std::string_view k, std::string_view v = {}) {
            return std::string(s.text(k, v));
        }

        float number(const std::string& s) {
            const auto n = css::parse_number<float>(s, static_cast<float>(constants::layout::max_length_magnitude_px));
            if (!n) {
                fail("finite number: " + s);
            }
            return *n;
        }

        float px(const std::string& s) {
            if (s == "0") {
                return 0;
            }
            if (s.ends_with("px")) {
                return number(s.substr(0, s.size() - 2));
            }
            fail("px required: " + s);
        }

        float edge(const Style& s, const char* k) {
            return px(get(s, k, "0"));
        }

        // Key of the initial containing block's box; no element uses the "box-" or "flow-" key space for it.
        constexpr std::string_view initial_containing_block = "initial-containing-block";

        // A canvas element's bitmap side, which is also its intrinsic size.
        float canvas_dimension(const DomSnapshot& element, weft::dom::CanvasSide side) {
            const auto attribute = element.attributes.find(side == weft::dom::CanvasSide::Width ? "width" : "height");
            const auto value =
                weft::dom::canvas_dimension(attribute == element.attributes.end() ? nullptr : &attribute->second, side);
            if (!value) {
                fail("canvas backing size");
            }
            return float(*value);
        }

        Rect shifted(Rect r, float x, float y) {
            r.x -= x;
            r.y -= y;
            return r;
        }

        Rect united(Rect a, Rect b) {
            if (a.w == 0 && a.h == 0) {
                return b;
            }
            const float x = std::min(a.x, b.x);
            const float y = std::min(a.y, b.y);
            return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
        }

        struct Entry {
            std::size_t child{absent};
            std::size_t paragraph{absent};
        };

        struct Node {
            const DomSnapshot* dom{};
            const ComputedStyle* computed{};
            Style style;
            std::string id;
            std::string key;
            std::string image;
            // A canvas element: intrinsic size from its width/height attributes; painted from its raster.
            bool canvas{};
            float canvas_width{};
            float canvas_height{};
            std::string overflow_x;
            std::string overflow_y;
            std::size_t parent{absent};
            std::vector<std::size_t> children;
            std::vector<Entry> entries;
            Rect box{};
            std::vector<Rect> fragments;
            bool hidden{};
            bool inlined{};
            int z{};
            float scroll_w{};
            float scroll_h{};
            float scroll_x{};
            float scroll_y{};
        };

        struct Paragraph {
            std::size_t host;
            std::string key;
            std::shared_ptr<InlineInput> input;
            InlineLayout layout;
            Rect box;
        };

        struct Calc {
            std::size_t node;
            std::string key;
            float percent;
            float offset;
        };

        struct Builder {
            FontSet& fonts;
            std::vector<Node> nodes;
            std::vector<Paragraph> paragraphs;
            std::vector<Calc> calcs;
            std::map<std::string, std::size_t> ids;
            std::size_t inline_bytes{};

            std::size_t collect(const DomSnapshot& dom, const ComputedStyle& computed, std::size_t parent,
                                bool hidden = false) {
                static const std::set<std::string> tags{"div",  "p",  "section", "button", "i",      "b",
                                                        "span", "ul", "li",      "img",    "canvas", "br"};
                if (dom.type != constants::element_node_type || dom.namespace_uri != constants::xhtml_namespace_uri ||
                    !tags.contains(dom.local_name)) {
                    fail("HTML tag/namespace");
                }
                auto s = computed.values;
                auto display = get(s, "display", "inline");
                auto position = get(s, "position", "static");
                if (display == "inline" &&
                    (position == "absolute" || (parent != absent && get(nodes[parent].style, "display") == "flex"))) {
                    display = "block";
                }
                if (display != "block" && display != "flex" && display != "inline" && display != "none") {
                    fail("display " + display);
                }
                if (position != "static" && position != "relative" && position != "absolute") {
                    fail("position " + position);
                }
                const bool inlined = display == "inline";
                if (inlined && position != "static") {
                    fail("positioned inline containing blocks pending");
                }
                s["display"] = display;
                hidden |= display == "none";
                std::string ox = get(s, "overflow-x", "visible");
                std::string oy = get(s, "overflow-y", "visible");
                const auto valid_overflow = [](const std::string& v) {
                    return v == "visible" || v == "hidden" || v == "auto" || v == "scroll" || v == "clip";
                };
                if (!valid_overflow(ox) || !valid_overflow(oy)) {
                    fail("overflow keyword outside profile");
                }
                if (!std::isfinite(dom.scroll_left) || !std::isfinite(dom.scroll_top)) {
                    fail("finite scroll snapshot required");
                }
                auto special = [](const std::string& v) {
                    return v == "visible" || v == "clip";
                };
                if (!special(ox) || !special(oy)) {
                    if (ox == "visible") {
                        ox = "auto";
                    } else if (ox == "clip") {
                        ox = "hidden";
                    }
                    if (oy == "visible") {
                        oy = "auto";
                    } else if (oy == "clip") {
                        oy = "hidden";
                    }
                }
                if (ox == "clip" || oy == "clip") {
                    fail("overflow:clip formatting context pending");
                }
                if ((ox == "visible") != (oy == "visible")) {
                    fail("single-axis visible overflow pending");
                }
                // Flex items keep their margins (Yoga resolves them as CSS does); block flow has no margin collapsing
                // yet, so its margins must be zero.
                const bool flex_item = parent != absent && get(nodes[parent].style, "display") == "flex";
                for (const auto& [property, value] : s) {
                    const auto k = property_name(property);
                    const auto& v = value.text();
                    if (k.starts_with("margin") && flex_item) {
                        if (v != "auto") {
                            (void)px(v);
                        }
                    } else if (k.starts_with("margin") || k == "border" || k == "border-width" ||
                               (k.ends_with("-width") && k.starts_with("border-"))) {
                        if (px(v) != 0) {
                            fail("zero margin/border subset");
                        }
                    }
                    if (k.starts_with("padding") &&
                        (px(v) < 0 || px(v) > constants::layout::max_length_px || (inlined && px(v) != 0))) {
                        fail("padding bounds / inline decorations");
                    }
                    if (inlined && k.ends_with("-radius") && v != "0" && v != "0 0") {
                        fail("inline rounded decorations pending");
                    }
                    if (display == "block" &&
                        (k == "gap" || k == "row-gap" || k == "column-gap" || k == "flex-direction" ||
                         k == "flex-wrap" || k == "align-items" || k == "align-content" || k == "justify-content")) {
                        fail("flex container property on block");
                    }
                    if (parent != absent && get(nodes[parent].style, "display") == "block" &&
                        (k == "flex" || k == "flex-grow" || k == "flex-shrink" || k == "flex-basis" ||
                         k == "align-self")) {
                        fail("flex item outside flex parent");
                    }
                }
                if (get(s, "isolation", "auto") != "auto" && get(s, "isolation") != "isolate") {
                    fail("isolation");
                }
                if (get(s, "pointer-events", "auto") != "auto" && get(s, "pointer-events") != "none") {
                    fail("pointer-events");
                }
                if ((dom.local_name == "ul" || dom.local_name == "li") && get(s, "list-style") != "none") {
                    fail("list markers require reset");
                }
                // Chrome's UA sheet gives ul a block margin and an inline-start padding that this profile does not
                // model; the author must set them.
                if (dom.local_name == "ul") {
                    const auto& specified = computed.specified_properties;
                    for (const auto* k : {"margin-top", "margin-bottom", "padding-left"}) {
                        if (std::find(specified.begin(), specified.end(), k) == specified.end()) {
                            fail("ul needs explicit margin and padding");
                        }
                    }
                }
                int z{};
                if (get(s, "z-index", "auto") != "auto") {
                    const auto v = number(s.at("z-index"));
                    if (std::floor(v) != v || std::abs(v) > constants::layout::max_z_index_magnitude ||
                        position == "static" || parent == absent ||
                        get(nodes[parent].style, "isolation") != "isolate") {
                        fail("z-index requires direct isolated positioned parent");
                    }
                    z = int(v);
                }
                const auto index = nodes.size();
                auto id = dom.attributes.contains("id") ? dom.attributes.at("id") : "box-" + std::to_string(index);
                if (!ids.emplace(id, index).second) {
                    fail("duplicate evidence id");
                }
                Node n;
                n.dom = &dom;
                n.computed = &computed;
                n.style = s;
                n.id = id;
                n.key = "flow-" + std::to_string(index);
                n.parent = parent;
                n.hidden = hidden;
                n.inlined = inlined;
                n.z = z;
                n.overflow_x = ox;
                n.overflow_y = oy;
                if (dom.local_name == "img") {
                    if (inlined || !dom.attributes.contains("src") || !dom.children.empty() ||
                        get(s, "width", "auto") == "auto" || get(s, "height", "auto") == "auto") {
                        fail("blockified image needs explicit dimensions/source");
                    }
                    n.image = dom.attributes.at("src");
                }
                if (dom.local_name == "canvas") {
                    if (inlined || !dom.children.empty()) {
                        fail("blockified canvas without fallback content");
                    }
                    n.canvas = true;
                    n.canvas_width = canvas_dimension(dom, weft::dom::CanvasSide::Width);
                    n.canvas_height = canvas_dimension(dom, weft::dom::CanvasSide::Height);
                }
                if (!inlined && position != "absolute" && parent != absent && get(s, "height").ends_with('%') &&
                    get(nodes[parent].style, "height", "auto") == "auto") {
                    fail("flow percent height needs definite parent");
                }
                nodes.push_back(std::move(n));
                std::size_t child{};
                for (const auto& c : dom.children) {
                    if (c.type == constants::element_node_type) {
                        auto i = collect(c, computed.children[child++], index, hidden);
                        nodes[index].children.push_back(i);
                    } else if (c.type != constants::text_node_type) {
                        fail("child node type");
                    }
                }
                return index;
            }

            InlineRun run(const std::string& text, std::size_t node, const std::vector<std::string>& ancestors,
                          bool br = false) {
                const auto& s = nodes[node].style;
                const auto family = get(s, "font-family", constants::default_font_family);
                if ((family != constants::default_font_family && family != constants::latin_font_family) ||
                    get(s, "font-weight", constants::default_font_weight) != constants::default_font_weight ||
                    get(s, "font-style", "normal") != "normal") {
                    fail("fixed normal-weight font set");
                }
                InlineRun r;
                r.text = text;
                r.ancestors = ancestors;
                r.font_size = px(get(s, "font-size", constants::default_font_size));
                r.line_height = px(get(s, "line-height", constants::default_line_height));
                r.color = parse_css_color(get(s, "color", constants::default_color));
                r.white_space = get(s, "white-space", "normal");
                r.hard_break = br;
                for (const auto& id : ancestors) {
                    r.ancestor_wrap.push_back(get(nodes[ids.at(id)].style, "white-space", "normal") != "nowrap");
                }
                if ((inline_bytes += text.size()) > constants::layout::max_aggregate_text_bytes) {
                    fail("aggregate inline byte bound");
                }
                return r;
            }

            void inline_runs(std::size_t node, std::vector<std::string> ancestors, std::vector<InlineRun>& runs) {
                const auto& n = nodes[node];
                if (n.hidden) {
                    return;
                }
                if (!n.inlined) {
                    fail("block descendant inside inline pending");
                }
                ancestors.push_back(n.id);
                if (n.dom->local_name == "br") {
                    runs.push_back(run({}, node, ancestors, true));
                    return;
                }
                std::size_t child{};
                for (const auto& c : n.dom->children) {
                    if (c.type == constants::text_node_type) {
                        runs.push_back(run(c.data, node, ancestors));
                    } else {
                        inline_runs(n.children[child++], ancestors, runs);
                    }
                }
            }

            FlexNode tree(std::size_t index) {
                const auto& n = nodes[index];
                auto s = n.style;
                std::vector<PropertyId> paint_only;
                for (const auto& entry : s) {
                    const auto k = property_name(entry.first);
                    if (k == "color" || k == "background-color" || k == "font-family" || k == "font-weight" ||
                        k == "font-style" || k == "text-align" || k == "pointer-events" || k == "isolation" ||
                        k == "list-style" || k == "user-select" || k == "z-index" || k == "border" ||
                        k == "overflow-x" || k == "overflow-y" || k.ends_with("-radius")) {
                        paint_only.push_back(entry.first);
                    }
                }
                for (const auto property : paint_only) {
                    s.erase(property);
                }
                s["overflow"] = n.overflow_x == "visible" ? "visible" : "hidden";
                s["display"] = n.hidden ? "none" : get(n.style, "display");
                for (const auto* k : {"max-width", "max-height"}) {
                    if (get(s, k) == "none") {
                        s.erase(k);
                    }
                }
                if (n.canvas) {
                    // A replaced element: an auto dimension is intrinsic, or follows a specified px dimension through
                    // the intrinsic ratio. Two specified dimensions (px or %) go to the flex adapter as they are.
                    const auto width = get(s, "width", "auto");
                    const auto height = get(s, "height", "auto");
                    if (width == "auto" || height == "auto") {
                        if ((width != "auto" && !width.ends_with("px")) ||
                            (height != "auto" && !height.ends_with("px"))) {
                            fail("canvas intrinsic ratio needs a px dimension");
                        }
                        const float w = width == "auto" ? (height != "auto" && n.canvas_height > 0
                                                               ? px(height) * n.canvas_width / n.canvas_height
                                                               : n.canvas_width)
                                                        : px(width);
                        const float h = height == "auto" ? (width != "auto" && n.canvas_width > 0
                                                                ? w * n.canvas_height / n.canvas_width
                                                                : n.canvas_height)
                                                         : px(height);
                        s["width"] = std::to_string(w) + "px";
                        s["height"] = std::to_string(h) + "px";
                    }
                }
                std::vector<std::pair<PropertyId, std::string>> resolved;
                for (const auto& [property, value] : s) {
                    const std::string k(property_name(property));
                    const auto& v = value.text();
                    if (v.starts_with("calc(")) {
                        if (get(s, "position", "static") != "absolute" ||
                            (k != "left" && k != "top" && k != "right" && k != "bottom" && k != "width" &&
                             k != "height")) {
                            fail("calc only in absolute dimensions/insets");
                        }
                        std::istringstream parts(v.substr(5, v.size() - 6));
                        std::string a;
                        std::string op;
                        std::string b;
                        std::string extra;
                        parts >> a >> op >> b;
                        if (!v.ends_with(')') || parts >> extra || !a.ends_with('%') || (op != "+" && op != "-")) {
                            fail("percent +/- px calc");
                        }
                        calcs.push_back({index, k, number(a.substr(0, a.size() - 1)), px(b) * (op == "-" ? -1 : 1)});
                        resolved.emplace_back(property, a);
                    }
                }
                for (auto& [property, text] : resolved) {
                    s.set(property, std::move(text));
                }
                FlexNode out{n.key, s, {}, {}};
                if (n.hidden) {
                    return out;
                }
                auto pending = std::make_shared<InlineInput>();
                pending->font_size = px(get(n.style, "font-size", constants::default_font_size));
                pending->line_height = px(get(n.style, "line-height", constants::default_line_height));
                pending->text_align = get(n.style, "text-align", "left");
                pending->white_space = get(n.style, "white-space", "normal");
                // Inline-only blocks are measured leaves. Mixed block/inline sequences
                // become anonymous block boxes; they never change the DOM identity tree.
                const bool pure = std::all_of(n.children.begin(), n.children.end(),
                                              [&](auto c) {
                                                  return nodes[c].hidden || nodes[c].inlined;
                                              }) &&
                                  get(n.style, "display") != "flex";
                auto flush = [&] {
                    // White space that collapses away generates no anonymous box (CSS 2.1 section 9.2.2.1).
                    const bool blank = std::all_of(pending->runs.begin(), pending->runs.end(), [](const auto& r) {
                        return !r.hard_break && r.white_space != "pre-line" &&
                               r.text.find_first_not_of(" \t\r\n\f") == std::string::npos;
                    });
                    if (pending->runs.empty() || (!pure && blank)) {
                        pending->runs.clear();
                        return;
                    }
                    const auto p = paragraphs.size();
                    const auto key = pure ? n.key : "anonymous-" + std::to_string(p);
                    paragraphs.push_back({index, key, pending, {}, {}});
                    nodes[index].entries.push_back({absent, p});
                    if (pure) {
                        out.inline_content = pending;
                    } else {
                        // An anonymous block inherits its parent's inherited properties.
                        FlexNode a{key,
                                   {{"display", "block"},
                                    {"font-size", get(n.style, "font-size", constants::default_font_size)},
                                    {"line-height", get(n.style, "line-height", constants::default_line_height)},
                                    {"white-space", get(n.style, "white-space", "normal")}},
                                   {},
                                   {}};
                        a.inline_content = pending;
                        out.children.push_back(std::move(a));
                    }
                    pending = std::make_shared<InlineInput>(*pending);
                    pending->runs.clear();
                };
                std::size_t child{};
                for (const auto& c : n.dom->children) {
                    if (c.type == constants::text_node_type) {
                        pending->runs.push_back(run(c.data, index, {}));
                    } else {
                        auto i = n.children[child++];
                        if (nodes[i].hidden) {
                            continue;
                        }
                        if (nodes[i].inlined) {
                            inline_runs(i, {}, pending->runs);
                        } else {
                            flush();
                            out.children.push_back(tree(i));
                            nodes[index].entries.push_back({i, absent});
                        }
                    }
                }
                flush();
                return out;
            }

            void arrange(const std::map<std::string, FlexBox>& boxes) {
                for (auto& n : nodes) {
                    if (!n.hidden && !n.inlined) {
                        const auto& b = boxes.at(n.key);
                        n.box = {b.x, b.y, b.width, b.height};
                    }
                }
                for (auto& p : paragraphs) {
                    const auto& b = boxes.at(p.key);
                    const auto& n = nodes[p.host];
                    const bool direct = p.key == n.key;
                    const float pl = direct ? edge(n.style, "padding-left") : 0;
                    const float pr = direct ? edge(n.style, "padding-right") : 0;
                    const float pt = direct ? edge(n.style, "padding-top") : 0;
                    p.box = {b.x + pl, b.y + pt, std::max(0.f, b.width - pl - pr), b.height};
                    p.layout = layout_inline(*p.input, p.box.w, fonts);
                    for (const auto& f : p.layout.fragments) {
                        for (const auto& id : f.ancestors) {
                            auto& target = nodes[ids.at(id)];
                            auto m = fonts.inline_metrics(
                                0, px(get(target.style, "font-size", constants::default_font_size)));
                            Rect r{p.box.x + f.x, p.box.y + f.baseline - m.ascent, f.width, m.ascent + m.descent};
                            if (!target.fragments.empty() &&
                                std::abs(target.fragments.back().y - r.y) < constants::layout::fragment_merge_epsilon &&
                                std::abs(target.fragments.back().h - r.h) < constants::layout::fragment_merge_epsilon &&
                                std::abs(target.fragments.back().x + target.fragments.back().w - r.x) <
                                    constants::layout::fragment_merge_epsilon) {
                                target.fragments.back().w += r.w;
                            } else {
                                target.fragments.push_back(r);
                            }
                        }
                    }
                }
                for (auto& n : nodes) {
                    if (n.inlined && !n.hidden) {
                        for (auto r : n.fragments) {
                            n.box = united(n.box, r);
                        }
                    }
                }
                for (std::size_t i = nodes.size(); i-- > 0;) {
                    auto& n = nodes[i];
                    if (n.hidden || n.inlined) {
                        continue;
                    }
                    float right = n.box.x + n.box.w;
                    float bottom = n.box.y + n.box.h;
                    for (const auto& e : n.entries) {
                        if (e.paragraph != absent) {
                            const auto& p = paragraphs[e.paragraph];
                            for (const auto& f : p.layout.fragments) {
                                right = std::max(right, p.box.x + f.x + f.width + edge(n.style, "padding-right"));
                            }
                            bottom = std::max(bottom, p.box.y + p.layout.height + edge(n.style, "padding-bottom"));
                        } else {
                            const auto& c = nodes[e.child];
                            right = std::max(right, c.box.x + (c.overflow_x == "visible" ? c.scroll_w : c.box.w) +
                                                        edge(n.style, "padding-right"));
                            bottom = std::max(bottom, c.box.y + (c.overflow_y == "visible" ? c.scroll_h : c.box.h) +
                                                          edge(n.style, "padding-bottom"));
                        }
                    }
                    n.scroll_w = std::max(n.box.w, right - n.box.x);
                    n.scroll_h = std::max(n.box.h, bottom - n.box.y);
                    if (n.overflow_x != "visible") {
                        n.scroll_x = std::clamp(float(n.dom->scroll_left), 0.f, std::max(0.f, n.scroll_w - n.box.w));
                    }
                    if (n.overflow_y != "visible") {
                        n.scroll_y = std::clamp(float(n.dom->scroll_top), 0.f, std::max(0.f, n.scroll_h - n.box.h));
                    }
                }
            }

            RoundedClip rounded(const Node& n, Rect r) {
                RoundedClip c;
                c.rect = r.array();
                unsigned i{};
                for (const auto* k : {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
                                      "border-bottom-left-radius"}) {
                    std::istringstream in(get(n.style, k, "0"));
                    std::string x;
                    std::string y;
                    std::string extra;
                    in >> x;
                    if (!(in >> y)) {
                        y = x;
                    }
                    if (in >> extra) {
                        fail("corner radius needs one/two lengths");
                    }
                    auto resolve = [&](const std::string& v, float extent) {
                        auto n = v.ends_with('%')
                                     ? number(v.substr(0, v.size() - 1)) * extent / constants::percent_scale
                                     : px(v);
                        if (n < 0) {
                            fail("negative radius");
                        }
                        return n;
                    };
                    c.radius_x[i] = resolve(x, r.w);
                    c.radius_y[i] = resolve(y, r.h);
                    ++i;
                }
                return normalize_rounded(c);
            }

            // A hit area of `target`: its box, inside the overflow clip and the rounded clips.
            static void hit_area(Page& page, std::size_t target, const Rect& r, const Rect& clip,
                                 const std::vector<RoundedClip>& masks) {
                auto& areas = page.hit_areas;
                auto chain = areas.add_clip({}, HitShape::box(clip.x, clip.y, clip.w, clip.h), HitAreas::none);
                for (const auto& mask : masks) {
                    HitShape rounded;
                    rounded.kind = HitShape::Kind::Rounded;
                    rounded.rounded = mask;
                    chain = areas.add_clip({}, std::move(rounded), chain);
                }
                areas.areas.push_back({target, {}, {HitShape::box(r.x, r.y, r.w, r.h)}, chain});
            }

            void quad(PositionedPage& out, const Node& n, Rect r, Rect clip, const std::vector<RoundedClip>& masks,
                      const std::array<float, 4>& color, const std::string& image = {},
                      std::optional<NodeIdentity> canvas = {}) {
                auto b = intersect(r, clip);
                if (b.w <= 0 || b.h <= 0 || (!image.size() && color[3] == 0)) {
                    return;
                }
                PositionedPaint p;
                p.paint = {PaintKind::Rectangle, n.id, {}, b.x, b.y, b.w, b.h, 0, color, {}};
                p.image_source = image;
                p.uv = {(b.x - r.x) / r.w, (b.y - r.y) / r.h, b.w / r.w, b.h / r.h};
                p.clip = clip.array();
                p.rounded_clips = masks;
                p.canvas_identity = canvas;
                out.paints.push_back(std::move(p));
            }

            void paint(std::size_t index, float dx, float dy, Rect clip, std::vector<RoundedClip> masks,
                       DocumentLayout& out) {
                auto& n = nodes[index];
                if (n.hidden || n.inlined) {
                    return;
                }
                const auto r = shifted(n.box, dx, dy);
                auto corner = rounded(n, r);
                auto own = masks;
                const bool curved = std::any_of(corner.radius_x.begin(), corner.radius_x.end(),
                                                [](auto v) {
                                                    return v > 0;
                                                }) &&
                                    std::any_of(corner.radius_y.begin(), corner.radius_y.end(), [](auto v) {
                                        return v > 0;
                                    });
                if (curved) {
                    own.push_back(corner);
                }
                if (own.size() > constants::layout::max_rounded_clip_depth) {
                    fail("rounded clip depth bound");
                }
                hit_area(out.page.geometry, index, r, clip, own);
                quad(out.page, n, r, clip, own, parse_css_color(get(n.style, "background-color", "transparent")));
                if (!n.image.empty()) {
                    quad(out.page, n, r, clip, own, {1, 1, 1, 1}, n.image);
                }
                if (n.canvas) {
                    quad(out.page, n, r, clip, own, {1, 1, 1, 1}, {}, n.dom->identity);
                }
                if (n.overflow_x != "visible") {
                    clip = intersect(clip, r);
                    masks = own;
                }
                dx += n.scroll_x;
                dy += n.scroll_y;
                auto entries = n.entries;
                std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
                    return (a.child == absent ? 0 : nodes[a.child].z) < (b.child == absent ? 0 : nodes[b.child].z);
                });
                for (const auto& e : entries) {
                    if (e.child != absent) {
                        paint(e.child, dx, dy, clip, masks, out);
                        continue;
                    }
                    const auto& p = paragraphs[e.paragraph];
                    for (const auto& f : p.layout.fragments) {
                        for (const auto& id : f.ancestors) {
                            const auto& span = nodes[ids.at(id)];
                            auto m =
                                fonts.inline_metrics(0, px(get(span.style, "font-size", constants::default_font_size)));
                            Rect fr{p.box.x + f.x - dx, p.box.y + f.baseline - m.ascent - dy, f.width,
                                    m.ascent + m.descent};
                            hit_area(out.page.geometry, ids.at(id), fr, clip, masks);
                            quad(out.page, span, fr, clip, masks,
                                 parse_css_color(get(span.style, "background-color", "transparent")));
                        }
                        PositionedPaint item;
                        item.paint = {
                            PaintKind::Text, n.id,    f.text,  p.box.x + f.x - dx, p.box.y + f.baseline - dy, 0, 0,
                            f.font_size,     f.color, f.shaped};
                        item.clip = clip.array();
                        item.rounded_clips = masks;
                        out.page.paints.push_back(std::move(item));
                    }
                }
            }

            void geometry(DocumentLayout& out) {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    const auto& n = nodes[i];
                    float dx{};
                    float dy{};
                    for (auto parent = n.parent; parent != absent; parent = nodes[parent].parent) {
                        dx += nodes[parent].scroll_x;
                        dy += nodes[parent].scroll_y;
                    }
                    auto b = shifted(n.box, dx, dy);
                    if (n.hidden || (n.inlined && n.fragments.empty())) {
                        b = {};
                    }
                    ElementGeometry g;
                    g.identity = n.dom->identity;
                    g.rect = {b.x, b.y, b.w, b.h};
                    if (n.inlined) {
                        for (auto f : n.fragments) {
                            f = shifted(f, dx, dy);
                            g.client_rects.push_back({f.x, f.y, f.w, f.h});
                        }
                    } else if (!n.hidden) {
                        g.client_rects.push_back(g.rect);
                        g.client_width = n.box.w;
                        g.client_height = n.box.h;
                        g.scroll_width = n.scroll_w;
                        g.scroll_height = n.scroll_h;
                        g.scroll_left = n.scroll_x;
                        g.scroll_top = n.scroll_y;
                        g.scroll_x = n.overflow_x != "visible";
                        g.scroll_y = n.overflow_y != "visible";
                        g.wheel_x = n.overflow_x == "auto" || n.overflow_x == "scroll";
                        g.wheel_y = n.overflow_y == "auto" || n.overflow_y == "scroll";
                    }
                    out.elements.push_back(g);
                    out.page.geometry.boxes.push_back({n.id, b.x, b.y, b.w, b.h});
                    bool disabled{};
                    for (auto parent = i; parent != absent; parent = nodes[parent].parent) {
                        disabled |= nodes[parent].dom->local_name == "button" &&
                                    nodes[parent].dom->attributes.contains("disabled");
                    }
                    out.page.geometry.hits.push_back({n.dom->identity, n.parent, b.x, b.y, b.w, b.h,
                                                      !n.hidden && get(n.style, "pointer-events", "auto") == "auto",
                                                      disabled});
                }
            }
        };
    } // namespace

    DocumentLayout layout_document(const DomSnapshot& root, const ComputedStyle& style, float w, float h,
                                   FontSet& fonts) {
        if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 || w > constants::layout::max_viewport_size_px ||
            h > constants::layout::max_viewport_size_px) {
            fail("viewport bounds");
        }
        validate_style_pair(root, style);
        Builder b{fonts};
        b.collect(root, style, absent);
        if (b.nodes[0].inlined || get(b.nodes[0].style, "position", "static") == "absolute") {
            fail("root must be a block or flex box in flow");
        }
        // The initial containing block: the viewport-sized box that contains the root element and every absolute
        // box without a positioned ancestor.
        FlexNode tree{std::string(initial_containing_block),
                      {{"display", "block"},
                       {"position", "relative"},
                       {"width", std::to_string(w) + "px"},
                       {"height", std::to_string(h) + "px"}},
                      {},
                      {}};
        tree.children.push_back(b.tree(0));
        std::map<std::string, FlexNode*> inputs;
        auto visit = [&](auto&& self, FlexNode& n) -> void {
            inputs[n.id] = &n;
            for (auto& c : n.children) {
                self(self, c);
            }
        };
        visit(visit, tree);
        auto layout = [&] {
            std::map<std::string, FlexBox> boxes;
            for (const auto& box : run_flex(tree, fonts)) {
                for (float v : {box.x, box.y, box.width, box.height}) {
                    if (!std::isfinite(v) || std::abs(v) > constants::layout::max_length_magnitude_px) {
                        fail("box bounds");
                    }
                }
                boxes.emplace(box.id, box);
            }
            return boxes;
        };
        auto boxes = layout();
        for (unsigned pass = 0; !b.calcs.empty(); ++pass) {
            bool changed{};
            for (const auto& c : b.calcs) {
                auto parent = b.nodes[c.node].parent;
                while (parent != absent && get(b.nodes[parent].style, "position", "static") == "static") {
                    parent = b.nodes[parent].parent;
                }
                const bool vertical = c.key == "top" || c.key == "bottom" || c.key == "height";
                auto base = parent == absent ? (vertical ? h : w)
                                             : (vertical ? boxes.at(b.nodes[parent].key).height
                                                         : boxes.at(b.nodes[parent].key).width);
                auto v = base * c.percent / constants::percent_scale + c.offset;
                if ((c.key == "width" || c.key == "height") && v < 0) {
                    fail("negative calc size");
                }
                auto value = std::to_string(v) + "px";
                auto& input = *inputs.at(b.nodes[c.node].key);
                changed |= input.style[c.key] != value;
                input.style[c.key] = value;
            }
            if (!changed) {
                break;
            }
            if (pass >= constants::layout::max_calc_relayout_passes ||
                (pass + 1) * boxes.size() > constants::layout::max_calc_relayout_box_work) {
                fail("calc dependency bound");
            }
            boxes = layout();
        }
        b.arrange(boxes);
        DocumentLayout out;
        b.geometry(out);
        b.paint(0, 0, 0, {0, 0, w, h}, {}, out);
        return out;
    }

    DocumentLayout layout_document(const DomSnapshot& root, std::string_view css, float w, float h, FontSet& fonts) {
        return layout_document(root, compute_style(root, css, document_style_profile()), w, h, fonts);
    }
} // namespace weft
