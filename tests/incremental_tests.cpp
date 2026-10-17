#include "constants/common.hpp"
#include "dom/tree.hpp"
#include "paint/particles.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"
#include "scene/scene_particles.hpp"
#include "style/style.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <crtdbg.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The incremental pipeline against full computation (step 13.5 of docs/refactoring-plan.md): random changes of a DOM,
// and after each the cascade updated from the previous document, which visits only what changed, compared with a full
// cascade of the same document.
namespace {
    using weft::testing::require;
    using Id = std::array<std::uint64_t, 3>;

    Id id_of(weft::NodeIdentity n) {
        return {n.view_epoch, n.slot, n.generation};
    }

    // Seeded draws, as the other differential tests take them.
    struct Random {
        std::uint32_t seed;

        std::uint32_t next() {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            return seed >> 8;
        }

        std::size_t below(std::size_t n) {
            return next() % n;
        }
    };

    // A document of nested elements under the scene root, and the elements it may change.
    struct Page {
        weft::testing::NativeScene s;
        std::vector<weft::dom::Node*> elements;

        explicit Page(Random& r) {
            elements.push_back(s.root);
            for (int i = 0; i < 40; ++i) {
                add(*elements[r.below(elements.size())], i % 3 == 0 ? "span" : "div");
            }
        }

        weft::dom::Node& add(weft::dom::Node& parent, const char* tag) {
            auto& n = s.html(tag);
            s.tree.insert(parent, n, nullptr);
            elements.push_back(&n);
            return n;
        }

        // Whether a is b or one of b's ancestors.
        bool contains(const weft::dom::Node& a, const weft::dom::Node* b) const {
            for (; b; b = s.tree.parent_of(*b)) {
                if (b == &a) {
                    return true;
                }
            }
            return false;
        }

        // One random change: a class or attribute a selector reads, an inline style that sets inherited values or
        // custom properties, hover, a move, a removal or a new element.
        void change(Random& r) {
            auto& n = *elements[r.below(elements.size())];
            switch (r.below(9)) {
                case 0:
                    s.toggle_class(n, r.below(2) ? "on" : "hot");
                    break;
                case 1:
                    s.set(n, "data-tone", r.below(2) ? "gold" : "plain");
                    break;
                case 2: {
                    static const std::array<const char*, 5> styles{
                        "color:#225588", "--ink:#abcdef;color:var(--ink)", "width:14px",
                        "color:var(--ink,#998877);--ink:#102030", "font-size:20px;opacity:.5"};
                    s.set(n, "style", styles[r.below(styles.size())]);
                    break;
                }
                case 3:
                    n.hovered = !n.hovered;
                    s.tree.changed(n);
                    break;
                case 4:
                case 5: {
                    // A move under an element outside its subtree.
                    if (&n == s.root) {
                        break;
                    }
                    auto& to = *elements[r.below(elements.size())];
                    if (!contains(n, &to)) {
                        s.tree.insert(to, n, nullptr);
                    }
                    break;
                }
                case 6:
                    if (&n != s.root && n.parent.generation) {
                        s.tree.detach(n);
                    }
                    break;
                case 7:
                    if (s.tree.connected(n)) {
                        add(n, r.below(2) ? "span" : "div");
                    }
                    break;
                default:
                    s.set(n, "title", std::to_string(r.below(4)));
                    break;
            }
        }

        // One random change for a sheet whose rules read no parent value: mostly an inline style, in values children
        // inherit or not, against rules that set them too (some important), or gone; then a class, a move, a removal
        // or a new element.
        void change_inline(Random& r) {
            auto& n = *elements[r.below(elements.size())];
            switch (r.below(8)) {
                case 0:
                case 1:
                case 2: {
                    static const std::array<const char*, 14> styles{"transform:translate(3px,4px)",
                                                                    "opacity:.5",
                                                                    "opacity:.5 !important",
                                                                    "width:10px;transform:scale(2)",
                                                                    "background-color:#ff0000",
                                                                    "transform:none;opacity:1;width:20px",
                                                                    "opacity:.5;opacity:.25",
                                                                    "opacity:.4 !important;opacity:.6",
                                                                    "color:#123456",
                                                                    "width:inherit",
                                                                    "transform:var(--t,none)",
                                                                    "--t:translate(1px,1px)",
                                                                    "margin:4px",
                                                                    ""};
                    s.set(n, "style", styles[r.below(styles.size())]);
                    break;
                }
                case 3:
                    if (const auto it = std::find_if(n.attributes.begin(), n.attributes.end(),
                                                     [](const auto& a) {
                                                         return a.name == "style";
                                                     });
                        it != n.attributes.end()) {
                        n.attributes.erase(it);
                        s.tree.changed(n);
                    }
                    break;
                case 4:
                    s.toggle_class(n, r.below(3) == 0 ? "lock" : r.below(2) ? "box" : "wide");
                    break;
                case 5:
                    if (&n != s.root) {
                        auto& to = *elements[r.below(elements.size())];
                        if (!contains(n, &to)) {
                            s.tree.insert(to, n, nullptr);
                        }
                    }
                    break;
                case 6:
                    if (&n != s.root && n.parent.generation) {
                        s.tree.detach(n);
                    }
                    break;
                default:
                    if (s.tree.connected(n)) {
                        add(n, r.below(2) ? "span" : "div");
                    }
                    break;
            }
        }
    };

    // The nodes whose own values differ between two outputs, matched by identity within their parents (an output
    // the cascade reconstructs counts as changed).
    void changes(const weft::ComputedStyle& now, const weft::ComputedStyle* before, std::vector<Id>& out) {
        if (!before || before->identity != now.identity || before->local_name != now.local_name ||
            before->values != now.values || before->custom_properties != now.custom_properties ||
            before->invalid_at_computed_value != now.invalid_at_computed_value ||
            before->specified_properties != now.specified_properties) {
            out.push_back(id_of(now.identity));
        }
        for (const auto& c : now.children) {
            const weft::ComputedStyle* old{};
            if (before && before->identity == now.identity) {
                for (const auto& candidate : before->children) {
                    if (candidate.identity == c.identity) {
                        old = &candidate;
                    }
                }
            }
            changes(c, old, out);
        }
    }

    // Each node's stamp in out by identity.
    void stamps(const weft::ComputedStyle& n, std::map<Id, std::uint64_t>& out) {
        out[id_of(n.identity)] = n.stamp;
        for (const auto& c : n.children) {
            stamps(c, out);
        }
    }

    // Whether two outputs charged alike: every node's recorded bounds.
    bool same_charges(const weft::ComputedStyle& a, const weft::ComputedStyle& b) {
        if (a.own_work != b.own_work || a.own_bytes != b.own_bytes || a.subtree_nodes != b.subtree_nodes ||
            a.subtree_work != b.subtree_work || a.subtree_bytes != b.subtree_bytes ||
            a.children.size() != b.children.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!same_charges(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true;
    }

    // Random updates of a page against full cascades, steps of each seed: the values, the nodes reported changed, the
    // stamps, and the bounds kept subtrees charged.
    void cascade_runs(const std::string& sheet, void (Page::*change)(Random&),
                      std::initializer_list<std::uint32_t> seeds, int steps) {
        const auto profile = weft::scene_style_profile();
        const weft::CompiledStyle compiled(sheet, profile);
        for (std::uint32_t seed : seeds) {
            Random r{seed};
            Page page(r);
            weft::ComputedStyle output;
            weft::StyleMemo memo;
            std::optional<weft::DomSnapshot> previous;
            std::uint64_t stamp{};
            for (int step = 0; step < steps; ++step) {
                const auto at = " at step " + std::to_string(step) + " of seed " + std::to_string(seed);
                // Now and then several changes at once.
                for (std::size_t k = r.below(4) ? 1 : 1 + r.below(5); k > 0; --k) {
                    (page.*change)(r);
                }
                const auto dom = page.s.tree.snapshot(*page.s.root);
                const auto prior = output;
                std::map<Id, std::uint64_t> stamps_before;
                stamps(prior, stamps_before);
                std::vector<weft::NodeIdentity> changed;
                compiled.update(dom, previous ? &*previous : nullptr, output, memo, ++stamp, &changed);
                previous = dom;
                const auto expected = weft::compute_style(dom, sheet, profile);
                require(output == expected, "the updated cascade differs from a full one" + at);
                std::vector<Id> actual_changes;
                for (auto n : changed) {
                    actual_changes.push_back(id_of(n));
                }
                std::vector<Id> expected_changes;
                changes(output, step ? &prior : nullptr, expected_changes);
                std::sort(actual_changes.begin(), actual_changes.end());
                std::sort(expected_changes.begin(), expected_changes.end());
                require(actual_changes == expected_changes, "changed nodes are those whose values changed" + at);
                // A node takes the stamp exactly when its own values change; its subtree's latest stamp is its own or
                // a descendant's.
                std::map<Id, std::uint64_t> stamps_after;
                stamps(output, stamps_after);
                for (const auto& [n, s] : stamps_after) {
                    const bool now = std::binary_search(expected_changes.begin(), expected_changes.end(), n);
                    require(now ? s == stamp : stamps_before.contains(n) && stamps_before.at(n) == s,
                            "a stamp follows the node's own changes" + at);
                }
                const auto latest = [](auto&& self, const weft::ComputedStyle& n) -> std::uint64_t {
                    auto out = n.stamp;
                    for (const auto& c : n.children) {
                        out = std::max(out, self(self, c));
                    }
                    return out;
                };
                const auto children = [](auto&& self, const weft::ComputedStyle& n) -> std::uint64_t {
                    auto out = std::max(n.stamp, n.children_stamp);
                    for (const auto& c : n.children) {
                        out = std::max(out, self(self, c));
                    }
                    return out;
                };
                require(output.subtree_stamp == children(children, output) &&
                            output.subtree_stamp >= latest(latest, output),
                        "a subtree's latest stamp" + at);
                // What was kept without a visit charged the bounds as a full update does.
                weft::ComputedStyle full;
                weft::StyleMemo fresh;
                compiled.update(dom, nullptr, full, fresh, stamp);
                require(same_charges(output, full), "kept subtrees charged the bounds as a full update" + at);
            }
        }
    }

    void cascade_differential() {
        const std::string sheet = ".on span{color:#ff0000}.on>div{font-size:24px}.hot{background-color:#336699}"
                                  "[data-tone=gold] div{--ink:#ffd700}div:hover>span{color:#00ff00}"
                                  "span{color:var(--ink,#123456)}.hot span{width:inherit}";
        cascade_runs(sheet, &Page::change, {0x51u, 0x9e3u, 0x1234u}, 400);
        // Rules that read no parent value: a change of values children do not inherit leaves them alone, and an
        // inline style alone is cascaded again against the rules kept for the element.
        cascade_runs(".box{transform:translate(1px,2px);opacity:.8}.lock{opacity:.3 !important}.wide{width:30px}"
                     ".wide>div{width:12px}div{background-color:#101010}span{color:#202020}.box span{color:#ff0000}",
                     &Page::change_inline, {0x61u, 0x7f1u, 0x2468u}, 400);
        const auto profile = weft::scene_style_profile();
        const weft::CompiledStyle compiled(sheet, profile);
        // Copies the DOM did not produce (revision 0) are never taken as unchanged.
        {
            Random r{0x77};
            Page page(r);
            auto before = page.s.tree.snapshot(*page.s.root);
            const auto clear = [](auto&& self, weft::DomSnapshot& n) -> void {
                n.revision = 0;
                for (auto& c : n.children) {
                    self(self, c);
                }
            };
            clear(clear, before);
            weft::ComputedStyle output;
            weft::StyleMemo memo;
            compiled.update(before, nullptr, output, memo, 1);
            auto after = before;
            auto* deep = &after;
            while (!deep->children.empty()) {
                deep = &deep->children.back();
            }
            deep->attributes["style"] = "color:#0a0b0c";
            compiled.update(after, &before, output, memo, 2);
            require(output == weft::compute_style(after, sheet, profile), "copies of revision 0 are visited");
        }
        // At the node bound, past it by one node outside the subtree that changed: the update fails as a full one
        // does, though it keeps the large subtree without a visit.
        weft::testing::NativeScene s;
        auto& large = s.html("div");
        s.tree.insert(*s.root, large, nullptr);
        auto& sibling = s.html("div");
        s.tree.insert(*s.root, sibling, nullptr);
        for (int i = 0; i < weft::constants::max_document_nodes - 4; ++i) {
            s.tree.insert(large, s.html("i"), nullptr);
        }
        weft::ComputedStyle output;
        weft::StyleMemo memo;
        const auto first = s.tree.snapshot(*s.root);
        compiled.update(first, nullptr, output, memo, 1);
        s.tree.insert(sibling, s.html("b"), nullptr);
        const auto at_bound = s.tree.snapshot(*s.root);
        compiled.update(at_bound, &first, output, memo, 2);
        require(output.subtree_nodes == std::size_t(weft::constants::max_document_nodes), "a document at the bound");
        s.tree.insert(sibling, s.html("b"), nullptr);
        const auto over = s.tree.snapshot(*s.root);
        bool refused{};
        try {
            compiled.update(over, &at_bound, output, memo, 3);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "a document over the node bound fails though most of it is kept");
        std::cout << "PASS incremental cascade: 2,400 random updates against full cascades\n";
    }

    // A scene page: positioned panels of nameplates (a name over a bar), text in lines, a flex row, a rounded box that
    // clips its content (a translucent group with shadows, shadowed text), an svg icon, an animation, a transition, and
    // the changes a game UI makes to them.
    struct ScenePage {
        weft::testing::NativeScene s;
        std::vector<weft::dom::Node*> elements;
        std::vector<weft::dom::Node*> texts;
        weft::dom::Node* clipper{};
        weft::dom::Node* icon{};
        // A box under a translucent group that gains and loses an anchoring, and the first child, a box over the
        // whole page (a pass folds it into its clear).
        weft::dom::Node* anchorable{};
        weft::dom::Node* backdrop{};
        // The panels, the flock's birds, the flex row, and a caption on one line.
        std::vector<weft::dom::Node*> panels;
        std::vector<weft::dom::Node*> birds;
        weft::dom::Node* row{};
        weft::dom::Node* caption{};

        explicit ScenePage(Random& r) {
            backdrop = &add(*s.root, "div",
                            "position:absolute;left:0;top:0;width:400px;height:320px;"
                            "background-color:#101418");
            for (int p = 0; p < 4; ++p) {
                auto& panel = add(*s.root, "div",
                                  "position:absolute;left:" + std::to_string(10 + p * 90) + "px;top:10px;width:80px");
                panels.push_back(&panel);
                for (int u = 0; u < 3; ++u) {
                    auto& unit = add(panel, "div", "position:relative;height:30px");
                    text(add(unit, "div", "font-size:12px;line-height:14px"), "Unit " + std::to_string(p * 3 + u));
                    auto& bar = add(unit, "div", "height:6px;background-color:#222222;border:1px solid #445566");
                    add(bar, "div", "width:50%;height:100%;background-color:#c0392b");
                }
            }
            auto& line = add(*s.root, "div", "position:absolute;left:10px;top:200px;width:160px;font-size:14px");
            text(line, "Some ");
            text(add(line, "span", "color:#ff0000"), "inline words");
            text(line, " that wrap in lines");
            row = &add(*s.root, "div", "position:absolute;left:200px;top:200px;display:flex;gap:4px");
            for (int i = 0; i < 4; ++i) {
                add(*row, "div", "width:20px;height:20px;background-color:#3366" + std::to_string(10 + i * 20));
            }
            add(*s.root, "div", "position:absolute;left:10px;top:260px;width:30px;height:30px")
                .attributes.push_back({"class", "class", "", "pulse"});
            s.tree.changed(*elements.back());
            // A rounded clip before the others: the clips of the box below do not start the list's.
            auto& first_clip = add(*s.root, "div",
                                   "position:absolute;left:330px;top:10px;width:60px;height:40px;overflow:hidden;"
                                   "border-radius:8px;background-color:#304050");
            add(first_clip, "div",
                "position:absolute;left:4px;top:4px;width:30px;height:20px;background-color:#80c0f0");
            // Three anchored siblings of one z-index (a run the pass orders by depth), after the anchored panels.
            auto& flock = add(*s.root, "div", "position:absolute;left:0;top:0;width:400px;height:320px");
            for (int k = 0; k < 3; ++k) {
                auto& bird =
                    add(flock, "div", "position:absolute;left:0;top:0;width:20px;height:10px;background-color:#e0e040");
                s.set(bird, "data-anchor", std::to_string(3 + k));
                birds.push_back(&bird);
                text(add(bird, "div", "font-size:8px"), "b" + std::to_string(k));
            }
            // Rounded clips: the content of a box that clips to its rounded padding box.
            clipper = &add(*s.root, "div",
                           "position:absolute;left:220px;top:60px;width:150px;height:110px;overflow:hidden;"
                           "border-radius:10px;background-color:#202830");
            auto& group = add(*clipper, "div",
                              "position:absolute;left:10px;top:10px;width:100px;height:60px;opacity:0.7;"
                              "background-color:#405060");
            anchorable = &add(group, "div",
                              "position:absolute;left:8px;top:8px;width:40px;height:20px;background-color:#d4a843;"
                              "box-shadow:2px 3px 4px 0px rgba(0,0,0,0.5);border-radius:4px");
            text(add(*clipper, "div",
                     "position:absolute;left:10px;top:80px;font-size:12px;color:#ffffff;"
                     "text-shadow:1px 1px 2px #000000"),
                 "Clipped");
            // A rounded clip inside the clipping box: its chain holds the box's.
            auto& inner = add(*clipper, "div",
                              "position:absolute;left:115px;top:10px;width:30px;height:30px;overflow:hidden;"
                              "border-radius:6px;background-color:#506070");
            add(inner, "div", "position:absolute;left:5px;top:5px;width:30px;height:30px;background-color:#f08080");
            // An svg icon: listed whole when anything in it changes.
            icon = &s.svg("svg");
            s.set(*icon, "width", "40");
            s.set(*icon, "height", "40");
            s.set(*icon, "viewBox", "0 0 40 40");
            s.set(*icon, "style", "position:absolute;left:300px;top:250px;filter:drop-shadow(2px 2px 2px #000000)");
            s.tree.insert(*s.root, *icon, nullptr);
            auto& path = s.svg("path");
            s.set(path, "d", "M2 2 L38 2 L20 38 Z");
            s.set(path, "fill", "#33aa55");
            s.tree.insert(*icon, path, nullptr);
            // A caption on one line: a large text takes a texture of its own where a small one lies on an atlas page.
            caption = &add(*s.root, "div",
                           "position:absolute;left:4px;top:300px;font-size:12px;color:#ffffff;white-space:nowrap;"
                           "text-shadow:1px 1px 2px #000000");
            text(*caption, "Caption");
            (void)r;
        }

        weft::dom::Node& add(weft::dom::Node& parent, const char* tag, const std::string& style) {
            auto& n = s.html(tag);
            s.set(n, "style", style);
            s.tree.insert(parent, n, nullptr);
            elements.push_back(&n);
            return n;
        }

        void text(weft::dom::Node& parent, const std::string& data) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = data;
            s.tree.insert(parent, t, nullptr);
            texts.push_back(&t);
        }

        bool contains(const weft::dom::Node& a, const weft::dom::Node* b) const {
            for (; b; b = s.tree.parent_of(*b)) {
                if (b == &a) {
                    return true;
                }
            }
            return false;
        }

        // An inline style with one property set as the change gives it.
        void style(weft::dom::Node& n, const std::string& property, const std::string& value) {
            const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const auto& a) {
                return a.name == "style";
            });
            // Declarations start the text or follow a ';'.
            std::string text = ";" + (it == n.attributes.end() ? std::string() : it->value);
            if (const auto at = text.find(";" + property + ":"); at != std::string::npos) {
                const auto end = text.find(';', at + 1);
                text.erase(at, end == std::string::npos ? std::string::npos : end - at);
            }
            // The property alone: nothing is left.
            if (text.empty() || text.back() != ';') {
                text += ';';
            }
            s.set(n, "style", text.substr(1) + property + ":" + value);
        }

        void change(Random& r) {
            auto& n = *elements[r.below(elements.size())];
            switch (r.below(19)) {
                case 18:
                    // The clipping box scrolled (script's scrollTop and scrollLeft): its children move by what its
                    // content allows.
                    clipper->scroll_top = double(r.below(40));
                    clipper->scroll_left = r.below(2) ? 0. : double(r.below(30));
                    s.tree.changed(*clipper);
                    break;
                case 17: {
                    // Another anchor for a bird: the run it lies in orders it by that anchor's depth.
                    const auto it =
                        std::find_if(birds[0]->attributes.begin(), birds[0]->attributes.end(), [](const auto& a) {
                            return a.name == "data-anchor";
                        });
                    s.set(*birds[0], "data-anchor", it != birds[0]->attributes.end() && it->value == "3" ? "7" : "3");
                    break;
                }
                case 15:
                    // An anchoring under a translucent group: the group composites over the root's box while it holds
                    // one.
                    if (const auto it = std::find_if(anchorable->attributes.begin(), anchorable->attributes.end(),
                                                     [](const auto& a) {
                                                         return a.name == "data-anchor";
                                                     });
                        it != anchorable->attributes.end()) {
                        anchorable->attributes.erase(it);
                        s.tree.changed(*anchorable);
                    } else {
                        s.set(*anchorable, "data-anchor", "6");
                    }
                    break;
                case 16:
                    style(*backdrop, "background-color", r.below(2) ? "#101418" : "#181410");
                    break;
                case 12:
                    style(n, "z-index", std::to_string(r.below(3)));
                    break;
                case 13: {
                    auto& path = s.tree.at(icon->children[0]);
                    s.set(path, "fill", r.below(2) ? "#33aa55" : "#aa3355");
                    if (r.below(3) == 0) {
                        s.set(path, "d", r.below(2) ? "M2 2 L38 2 L20 38 Z" : "M4 4 L36 8 L18 36 Z");
                    }
                    break;
                }
                case 14:
                    // No radius: no rounded clip, fewer clips for as many items.
                    style(*clipper, "border-radius", std::to_string(r.below(3) ? 4 + r.below(10) : 0) + "px");
                    break;
                case 0:
                case 1:
                    style(n, "transform",
                          "translate(" + std::to_string(r.below(9)) + "px," + std::to_string(r.below(5)) + "px)");
                    break;
                case 2:
                    style(n, "opacity", r.below(2) ? "0.5" : "1");
                    break;
                case 3:
                    style(n, "background-color", r.below(2) ? "#336699" : "#996633");
                    break;
                case 4:
                    style(n, "width", std::to_string(10 + r.below(60)) + "%");
                    break;
                case 5:
                    style(n, "color", r.below(2) ? "#eeeeee" : "#ffcc00");
                    break;
                case 6:
                    style(n, "display", r.below(3) ? "block" : "none");
                    break;
                case 7:
                    s.toggle_class(n, r.below(2) ? "pulse" : "fade");
                    break;
                case 8: {
                    auto& t = *texts[r.below(texts.size())];
                    static const std::array<const char*, 3> datas{
                        "Short", "A longer text that takes more of the line",
                        "A text of many words that runs far wider than half an atlas page could ever hold at all"};
                    t.data = datas[r.below(datas.size())];
                    s.tree.changed(t);
                    break;
                }
                case 9: {
                    auto& to = *elements[r.below(elements.size())];
                    if (&n != s.root && !contains(n, &to) && to.local != "span") {
                        s.tree.insert(to, n, nullptr);
                    }
                    break;
                }
                case 10:
                    if (&n != s.root && n.parent.generation && r.below(3) == 0) {
                        s.tree.detach(n);
                    } else if (&n != s.root && !n.parent.generation) {
                        s.tree.insert(*s.root, n, nullptr);
                    }
                    break;
                default:
                    style(n, "font-size", std::to_string(10 + r.below(8)) + "px");
                    break;
            }
        }
    };

    bool same_elements(const std::vector<weft::ElementGeometry>& a, const std::vector<weft::ElementGeometry>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].identity != b[i].identity || a[i].rect != b[i].rect || a[i].client_rects != b[i].client_rects) {
                return false;
            }
        }
        return true;
    }

    // The handles of an update's frame that the update before gave too, for nodes neither the DOM nor the time
    // changed: every top-level subtree whose DOM revision stayed and that holds no animation.
    void scene_differential() {
        // The pulse animates an inherited color too: descendants take it each frame though nothing of theirs changes.
        const std::string sheet = ".fade{transition-property:opacity;transition-duration:50ms;"
                                  "transition-timing-function:linear}.pulse{animation:pulse 120ms linear 2}";
        const std::string keyframes = "@keyframes pulse{from{opacity:.25;color:#ff0000}to{opacity:1;color:#00ff00}}";
        auto& text = weft::testing::thread_scene_text();
        std::size_t shared{};
        std::size_t compared{};
        std::size_t layouts{};
        std::size_t partial{};
        for (std::uint32_t seed : {0x2au, 0x5150u, 0xbeefu}) {
            Random r{seed};
            ScenePage page(r);
            weft::SceneSolver incremental(sheet, keyframes, text);
            weft::SceneSolver full(sheet, keyframes, text, false);
            weft::SceneFrame frame;
            std::optional<weft::DomSnapshot> before;
            for (int step = 0; step < 300; ++step) {
                const auto at = " at step " + std::to_string(step) + " of seed " + std::to_string(seed);
                for (std::size_t k = r.below(5) ? 1 : 1 + r.below(4); k > 0; --k) {
                    page.change(r);
                }
                const auto dom = page.s.tree.snapshot(*page.s.root);
                const auto last = frame;
                const double time = step * 16.0;
                incremental.update(dom, frame, 400, 320, time);
                const auto expected = full.frame(dom, 400, 320, time);
                require(frame.root == expected.root && same_elements(frame.elements, expected.elements),
                        "the incremental scene differs from a full build" + at);
                // A first frame computes every numeric paint value, which == leaves out; animations start with it.
                weft::SceneSolver first(sheet, keyframes, text);
                const auto fresh = first.frame(dom, 400, 320, time);
                require(weft::scene_paint_source_equal(fresh.root, frame.root) || frame.animated || fresh.animated,
                        "the incremental paint values differ from a first frame's" + at);
                // Untouched subtrees keep their storage.
                if (before && !frame.animated && !last.animated) {
                    const auto& children = std::as_const(frame.root.children);
                    for (std::size_t k = 0; k < children.size(); ++k) {
                        const auto& node = children[k];
                        const auto* now = &dom.children[0];
                        const weft::DomSnapshot* was{};
                        for (const auto& c : dom.children) {
                            if (c.identity == node.identity) {
                                now = &c;
                            }
                        }
                        for (const auto& c : before->children) {
                            if (c.identity == node.identity) {
                                was = &c;
                            }
                        }
                        const auto old =
                            std::find_if(last.root.children.begin(), last.root.children.end(), [&](const auto& c) {
                                return c.identity == node.identity;
                            });
                        if (!was || was->revision != now->revision || old == last.root.children.end() ||
                            last.root.box != frame.root.box) {
                            continue;
                        }
                        ++compared;
                        const auto index = std::size_t(old - last.root.children.begin());
                        if (children.handle(k) == last.root.children.handle(index)) {
                            ++shared;
                        }
                    }
                }
                before = dom;
            }
            layouts += incremental.work().layout_evaluations;
            partial += incremental.work().partial_layouts;
        }
        // Layout may move a sibling's box, which gives it new storage; most stay.
        require(compared > 100 && shared * 10 > compared * 8,
                "untouched subtrees keep their storage: " + std::to_string(shared) + " of " + std::to_string(compared));
        // Most layouts take the subtrees around what changed alone.
        require(partial * 2 > layouts,
                "partial layouts: " + std::to_string(partial) + " of " + std::to_string(layouts));
        std::cout << "PASS incremental scene: 900 random updates against full builds; " << shared << " of " << compared
                  << " untouched subtrees shared; " << partial << " of " << layouts << " layouts partial\n";
    }

    // Particles: a stream on a panel in a wrapper that hides and shows (its descendants' particles follow), streams
    // that start and stop, bursts on elements (some flying to a target), and moves; each update against a full build.
    void particles_differential() {
        weft::SceneParticles registry;
        weft::paint::ParticleEmitter embers;
        embers.shape = weft::paint::ParticleEmitter::Shape::Circle;
        embers.area = {.5f, .5f, .5f, 0};
        embers.rate = 20;
        embers.life = {.2f, .3f};
        embers.speed = {20, 40};
        embers.size = {4, 6, 6, 2};
        embers.color = {weft::paint::Color{1, .6f, .2f, 1}, weft::paint::Color{1, .4f, .1f, .8f},
                        weft::paint::Color{.6f, .1f, .1f, .5f}, weft::paint::Color{.2f, .1f, .1f, 0}};
        require(bool(registry.add("embers", embers)), "register particles");
        auto& text = weft::testing::thread_scene_text();
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:400px;height:300px");
        auto& wrap = s.html("div");
        s.set(wrap, "style", "position:absolute;left:0;top:0;width:400px;height:300px");
        s.tree.insert(*s.root, wrap, nullptr);
        auto& inner = s.html("div");
        s.set(inner, "style", "position:absolute;left:10px;top:10px;width:300px;height:200px");
        s.tree.insert(wrap, inner, nullptr);
        auto& panel = s.html("div");
        s.tree.insert(inner, panel, nullptr);
        auto& other = s.html("div");
        s.set(other, "style", "position:absolute;left:200px;top:120px;width:30px;height:20px");
        s.tree.insert(inner, other, nullptr);
        auto& target = s.html("div");
        s.set(target, "style", "position:absolute;left:300px;top:200px;width:20px;height:20px");
        s.tree.insert(*s.root, target, nullptr);
        weft::SceneSolver incremental("", "", text);
        weft::SceneSolver full("", "", text, false);
        incremental.set_particles(&registry);
        full.set_particles(&registry);
        weft::SceneFrame frame;
        Random r{0x5eed};
        bool stream = true;
        int left = 100;
        std::string panel_style;
        for (int step = 0; step < 240; ++step) {
            switch (r.below(6)) {
                case 0:
                    s.set(wrap, "style",
                          std::string("position:absolute;left:0;top:0;width:400px;height:300px;display:") +
                              (r.below(2) ? "block" : "none"));
                    break;
                case 1:
                    stream = !stream;
                    break;
                case 2: {
                    // A burst on the panel or the other element, in both solvers.
                    weft::SceneBurst burst;
                    burst.count = 1 + std::uint32_t(r.below(8));
                    burst.duration = .05f;
                    if (r.below(2)) {
                        burst.target = target.identity;
                    }
                    const auto& on = r.below(2) ? panel : other;
                    incremental.emit_particles(on.identity, "embers", burst);
                    full.emit_particles(on.identity, "embers", burst);
                    break;
                }
                case 3:
                    left = 60 + int(r.below(80));
                    break;
                case 4:
                    s.set(other, "style",
                          "position:absolute;left:200px;top:120px;width:30px;height:20px;"
                          "background-color:" +
                              std::string(r.below(2) ? "#ff0000" : "#0000ff"));
                    break;
                default:
                    break;
            }
            // The panel changes only when its values do: hidden and shown again unchanged, its stream starts again
            // though nothing of it changed.
            const auto style = "position:absolute;left:" + std::to_string(left) +
                               "px;top:100px;width:40px;height:20px;-weft-particles:" + (stream ? "embers" : "none");
            if (style != panel_style) {
                s.set(panel, "style", style);
                panel_style = style;
            }
            const auto dom = s.tree.snapshot(*s.root);
            const double time = 1000 + step * 16.0;
            incremental.update(dom, frame, 400, 300, time);
            const auto expected = full.frame(dom, 400, 300, time);
            require(frame.root == expected.root && frame.animated == expected.animated,
                    "the incremental scene with particles differs from a full build at step " + std::to_string(step));
        }
        std::cout << "PASS incremental particles: 240 updates against full builds\n";
    }

    // An element's text beside its children: a span that turns block, absolute or hidden changes how the element's text
    // nodes show (an inline formatting context or not), though nothing of the element changes.
    void inline_differential() {
        auto& text = weft::testing::thread_scene_text();
        weft::testing::NativeScene s;
        auto& line = s.html("div");
        s.set(line, "style", "position:absolute;left:10px;top:10px;width:200px;font-size:14px");
        s.tree.insert(*s.root, line, nullptr);
        const auto words = [&](const std::string& data) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = data;
            s.tree.insert(line, t, nullptr);
        };
        words("Some ");
        auto& span = s.html("span");
        s.tree.insert(line, span, nullptr);
        auto& inner = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
        inner.data = "inline words";
        s.tree.insert(span, inner, nullptr);
        words("   ");
        weft::SceneSolver incremental("", "", text);
        weft::SceneSolver full("", "", text, false);
        weft::SceneFrame frame;
        static const std::array<const char*, 5> styles{"display:inline", "display:block", "position:absolute",
                                                       "display:none", "display:inline-block"};
        Random r{0x1e1e};
        for (int step = 0; step < 60; ++step) {
            const std::string style = styles[r.below(styles.size())];
            s.set(span, "style", style);
            const auto dom = s.tree.snapshot(*s.root);
            incremental.update(dom, frame, 400, 300, step * 16.0);
            const auto expected = full.frame(dom, 400, 300, step * 16.0);
            require(frame.root == expected.root, "the incremental scene differs from a full build as the span takes " +
                                                     style + " at step " + std::to_string(step));
        }
        std::cout << "PASS incremental inline context: 60 updates against full builds\n";
    }

    // Layouts of the subtrees around what changed (absolute boxes with an inset on each axis, laid out apart in their
    // containing block) against full layouts: insets on either side, sizes in px, percentages or auto, containing
    // blocks with padding and borders in px or percentages, static boxes between, text that wraps, and changes to
    // all of them, to positions and to the tree.
    void layout_differential() {
        auto& text = weft::testing::thread_scene_text();
        std::size_t layouts{};
        std::size_t partial{};
        for (std::uint32_t seed : {0x31u, 0x5a5u, 0x7777u}) {
            Random r{seed};
            weft::testing::NativeScene s;
            std::vector<weft::dom::Node*> boxes;
            std::vector<weft::dom::Node*> texts;
            // Some on one axis alone: the box takes its static position on the other.
            static const std::array<const char*, 9> insets{"left:6px;top:4px",
                                                           "right:6px;top:4px",
                                                           "left:6px;bottom:4px",
                                                           "right:6px;bottom:4px",
                                                           "left:4px;right:10px;top:8px",
                                                           "left:5%;top:10%",
                                                           "left:3px;top:2px;bottom:5px",
                                                           "left:6px",
                                                           "top:4px"};
            static const std::array<const char*, 5> widths{"width:40px", "width:30%", "", "width:auto", "width:120px"};
            static const std::array<const char*, 4> heights{"height:20px", "", "height:25%", "height:auto"};
            static const std::array<const char*, 4> edges{"padding:4px", "padding:5%", "",
                                                          "padding:2px;border:3px solid #000000"};
            static const std::array<const char*, 4> words{"Short", "Two words", "A few more words that wrap",
                                                          "Words that run on for a while and wrap in narrow boxes"};
            const auto style_of = [&](bool container) {
                std::string out = std::string("position:") +
                                  (container && r.below(3) == 0 ? "relative"
                                   : r.below(5)                 ? "absolute"
                                                                : "static") +
                                  ";";
                out += std::string(insets[r.below(insets.size())]) + ";";
                out += std::string(widths[r.below(widths.size())]) + ";";
                out += std::string(heights[r.below(heights.size())]) + ";";
                if (container) {
                    out += std::string(edges[r.below(edges.size())]) + ";";
                    // Elements are border-box at first; some are content-box.
                    if (r.below(3) == 0) {
                        out += r.below(2) ? "box-sizing:content-box;max-height:90px;" : "box-sizing:border-box;";
                    }
                }
                out += "font-size:" + std::to_string(10 + r.below(6)) + "px;background-color:#334455";
                return out;
            };
            const auto add = [&](weft::dom::Node& parent, bool container) -> weft::dom::Node& {
                auto& n = s.html("div");
                s.set(n, "style", style_of(container));
                s.tree.insert(parent, n, nullptr);
                boxes.push_back(&n);
                if (!container || r.below(3) == 0) {
                    auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
                    t.data = words[r.below(words.size())];
                    s.tree.insert(n, t, nullptr);
                    texts.push_back(&t);
                }
                return n;
            };
            for (int c = 0; c < 4; ++c) {
                auto& container = add(*s.root, true);
                for (int k = 0; k < 3; ++k) {
                    // Some children hold boxes of their own, absolute ones among them.
                    auto& child = add(container, r.below(2) == 0);
                    if (!child.children.empty()) {
                        continue;
                    }
                    if (r.below(2) == 0) {
                        add(child, false);
                    }
                }
            }
            weft::SceneSolver incremental("", "", text);
            weft::SceneSolver full("", "", text, false);
            weft::SceneFrame frame;
            for (int step = 0; step < 300; ++step) {
                const auto at = " at step " + std::to_string(step) + " of seed " + std::to_string(seed);
                for (std::size_t k = r.below(4) ? 1 : 2; k > 0; --k) {
                    auto& n = *boxes[r.below(boxes.size())];
                    switch (r.below(7)) {
                        case 0:
                        case 1:
                            s.set(n, "style", style_of(!n.children.empty()));
                            break;
                        case 2: {
                            auto& t = *texts[r.below(texts.size())];
                            t.data = words[r.below(words.size())];
                            s.tree.changed(t);
                            break;
                        }
                        case 3: {
                            // Shown or hidden.
                            const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const auto& a) {
                                return a.name == "style";
                            });
                            const std::string style = it == n.attributes.end() ? std::string() : it->value;
                            s.set(n, "style",
                                  style.ends_with(";display:none") ? style.substr(0, style.size() - 13)
                                                                   : style + ";display:none");
                            break;
                        }
                        case 4: {
                            // Moved under another box, outside its own subtree.
                            auto& to = *boxes[r.below(boxes.size())];
                            bool inside{};
                            for (const weft::dom::Node* b = &to; b; b = s.tree.parent_of(*b)) {
                                inside = inside || b == &n;
                            }
                            if (!inside && s.tree.connected(n)) {
                                s.tree.insert(to, n, nullptr);
                            }
                            break;
                        }
                        default: {
                            // Another width, alone.
                            const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const auto& a) {
                                return a.name == "style";
                            });
                            const std::string style = it == n.attributes.end() ? std::string() : it->value;
                            s.set(n, "style", style + ";width:" + std::to_string(20 + r.below(80)) + "px");
                            break;
                        }
                    }
                }
                const auto dom = s.tree.snapshot(*s.root);
                incremental.update(dom, frame, 400, 300, step * 16.0);
                const auto expected = full.frame(dom, 400, 300, step * 16.0);
                require(frame.root == expected.root && same_elements(frame.elements, expected.elements),
                        "an incremental layout differs from a full one" + at);
            }
            layouts += incremental.work().layout_evaluations;
            partial += incremental.work().partial_layouts;
        }
        require(partial * 4 > layouts,
                "partial layouts: " + std::to_string(partial) + " of " + std::to_string(layouts));
        // One box laid out apart in a containing block of each kind, its width changed: the part is the box alone.
        static const std::array<const char*, 5> blocks{
            "position:relative;left:10px;top:10px;width:200px;height:100px;padding:10px;border:3px solid #000000;"
            "box-sizing:content-box",
            "position:absolute;left:10px;top:10px;width:200px;height:100px;padding:6px 12px;box-sizing:border-box",
            "position:relative;left:10px;top:10px;width:50%;min-width:220px;max-height:80px;height:120px;padding:4px;"
            "box-sizing:content-box",
            "position:absolute;left:0;right:30px;top:0;bottom:40px;padding:8px;border:1px solid #000000",
            // Padding of a percentage of its own parent: the part is the containing block.
            "position:absolute;left:10px;top:10px;width:200px;height:100px;padding:5%"};
        static const std::array<const char*, 5> placed{"position:absolute;right:5px;top:5px;width:50px;height:20px",
                                                       "position:absolute;left:5px;bottom:5px;width:30%;height:20%",
                                                       "position:absolute;left:4px;right:4px;top:4px;bottom:4px",
                                                       "position:absolute;right:0;bottom:0;font-size:12px",
                                                       "position:absolute;left:10%;top:8%;width:40px;height:10px"};
        for (const auto* block : blocks) {
            for (const auto* box : placed) {
                weft::testing::NativeScene s;
                auto& cb = s.html("div");
                s.set(cb, "style", block);
                s.tree.insert(*s.root, cb, nullptr);
                auto& sibling = s.html("div");
                s.set(sibling, "style", "height:30px;background-color:#202020");
                s.tree.insert(cb, sibling, nullptr);
                auto& b = s.html("div");
                s.set(b, "style", box);
                s.tree.insert(cb, b, nullptr);
                auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
                t.data = "A label in the box";
                s.tree.insert(b, t, nullptr);
                weft::SceneSolver incremental("", "", text);
                weft::SceneSolver full("", "", text, false);
                weft::SceneFrame frame;
                incremental.update(s.tree.snapshot(*s.root), frame, 400, 300, 0);
                s.set(b, "style", std::string(box) + ";min-width:60px");
                const auto dom = s.tree.snapshot(*s.root);
                incremental.update(dom, frame, 400, 300, 16);
                const auto expected = full.frame(dom, 400, 300, 16);
                require(incremental.work().partial_layouts == 1, std::string("a part alone in ") + block);
                require(frame.root == expected.root && same_elements(frame.elements, expected.elements),
                        std::string("a part laid out alone differs from a full layout: ") + box + " in " + block);
            }
        }
        // A containing block wider than its parent, its padding a percentage of that parent: in a box of its own size
        // the padding would be wider, more than its height, so the part is the containing block.
        {
            weft::testing::NativeScene s;
            auto& parent = s.html("div");
            s.set(parent, "style", "position:absolute;left:10px;top:10px;width:100px;height:200px");
            s.tree.insert(*s.root, parent, nullptr);
            auto& cb = s.html("div");
            s.set(cb, "style", "position:absolute;left:0;top:0;width:300px;height:6px;padding:2%");
            s.tree.insert(parent, cb, nullptr);
            auto& b = s.html("div");
            s.set(b, "style", "position:absolute;left:5%;top:10%;width:40px;height:50%");
            s.tree.insert(cb, b, nullptr);
            weft::SceneSolver incremental("", "", text);
            weft::SceneSolver full("", "", text, false);
            weft::SceneFrame frame;
            incremental.update(s.tree.snapshot(*s.root), frame, 400, 300, 0);
            s.set(b, "style", "position:absolute;left:5%;top:10%;width:44px;height:50%");
            const auto dom = s.tree.snapshot(*s.root);
            incremental.update(dom, frame, 400, 300, 16);
            const auto expected = full.frame(dom, 400, 300, 16);
            require(incremental.work().partial_layouts == 1 && frame.root == expected.root,
                    "a part in a containing block whose padding is a percentage of its parent");
        }
        std::cout << "PASS incremental layout: 900 random updates against full layouts; " << partial << " of "
                  << layouts << " layouts partial; 26 parts alone in containing blocks of six kinds\n";
    }

    // Whether two passes draw alike, byte for byte: records, masks, rounded clips, batches, images, anchors and runs.
    std::string pass_difference(const weft::ScenePrepared& a, const weft::ScenePrepared& b) {
        if (a.pass.records.size() != b.pass.records.size() ||
            std::memcmp(a.pass.records.data(), b.pass.records.data(),
                        a.pass.records.size() * sizeof(weft::render::SceneRecord))) {
            for (std::size_t i = 0; i < std::min(a.pass.records.size(), b.pass.records.size()); ++i) {
                if (std::memcmp(&a.pass.records[i], &b.pass.records[i], sizeof(weft::render::SceneRecord))) {
                    return "record " + std::to_string(i) + " of " + std::to_string(a.pass.records.size());
                }
            }
            return "records";
        }
        if (a.pass.masks != b.pass.masks) {
            return "masks";
        }
        if (a.pass.clear != b.pass.clear) {
            return "clear";
        }
        if (a.pass.clips.size() != b.pass.clips.size() ||
            std::memcmp(a.pass.clips.data(), b.pass.clips.data(),
                        a.pass.clips.size() * sizeof(weft::render::SceneClip))) {
            return "rounded clips";
        }
        if (a.pass.batches.size() != b.pass.batches.size()) {
            return "batch count";
        }
        for (std::size_t i = 0; i < a.pass.batches.size(); ++i) {
            const auto& x = a.pass.batches[i];
            const auto& y = b.pass.batches[i];
            if (x.step != y.step || x.first != y.first || x.count != y.count || x.source != y.source) {
                return "batch " + std::to_string(i);
            }
        }
        if (a.images != b.images || a.pages != b.pages) {
            return "images";
        }
        if (a.anchors.size() != b.anchors.size()) {
            return "anchors";
        }
        for (std::size_t k = 0; k < a.anchors.size(); ++k) {
            const auto& x = a.anchors[k];
            const auto& y = b.anchors[k];
            if (x.element != y.element || !(x.anchoring == y.anchoring) || x.box != y.box || x.own != y.own ||
                x.first != y.first || x.end != y.end) {
                return "anchor " + std::to_string(k);
            }
        }
        if (a.runs.size() != b.runs.size()) {
            return "runs";
        }
        for (std::size_t r = 0; r < a.runs.size(); ++r) {
            const auto& x = a.runs[r];
            const auto& y = b.runs[r];
            if (x.reorderable != y.reorderable || x.members.size() != y.members.size()) {
                return "run " + std::to_string(r);
            }
            for (std::size_t m = 0; m < x.members.size(); ++m) {
                const auto& u = x.members[m];
                const auto& v = y.members[m];
                if (u.first != v.first || u.end != v.end || u.anchor != v.anchor || u.child != v.child) {
                    return "run " + std::to_string(r) + " member " + std::to_string(m);
                }
            }
        }
        return {};
    }

    // A WARP target's pixels as a renderer draws a frame into it with a slot (encoded premultiplied BGRA rows).
    std::vector<std::uint8_t> draw_warp(weft::render::d3d11::Renderer& gpu, weft::render::d3d11::SceneRenderer& scene,
                                        weft::render::d3d11::SceneRenderer::Slot& slot, const weft::SceneFrame& frame,
                                        const weft::SceneAnchorTable* anchors, unsigned width, unsigned height) {
        using Microsoft::WRL::ComPtr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> rtv;
        require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &target)) &&
                    SUCCEEDED(gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)),
                "WARP target");
        require(scene.paint(slot, frame, rtv.Get(), 1, width, height, std::nullopt, 0, {}, anchors), "a WARP pass");
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &staging)), "WARP readback");
        gpu.immediate->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(gpu.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "WARP map");
        std::vector<std::uint8_t> out(std::size_t(width) * height * 4);
        for (unsigned y = 0; y < height; ++y) {
            std::memcpy(out.data() + std::size_t(y) * width * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, width * 4);
        }
        gpu.immediate->Unmap(staging.Get(), 0);
        return out;
    }

    // A renderer that patches its kept pass, drawing with two slots in turn (so a slot holds the records of the version
    // before last and writes what two patches changed), against one that prepares every frame anew: the same pixels.
    void warp_differential() {
        const std::string sheet = ".pulse{animation:pulse 120ms linear 2}";
        const std::string keyframes = "@keyframes pulse{from{opacity:.25}to{opacity:1}}";
        auto& text = weft::testing::thread_scene_text();
        constexpr unsigned width = 400;
        constexpr unsigned height = 320;
        weft::render::d3d11::Renderer gpu(true, width, height);
        weft::render::d3d11::SceneRenderer patching(gpu, text);
        weft::render::d3d11::SceneRenderer fresh(gpu, text);
        std::array<weft::render::d3d11::SceneRenderer::Slot, 2> slots;
        weft::render::d3d11::SceneRenderer::Slot fresh_slot;
        weft::SceneAnchorChannel channel;
        Random r{0x777};
        ScenePage page(r);
        page.s.set(*page.panels[1], "data-anchor", "1");
        weft::SceneSolver solver(sheet, keyframes, text);
        weft::SceneFrame frame;
        for (int step = 0; step < 120; ++step) {
            for (std::size_t k = r.below(4) ? 1 : 2; k > 0; --k) {
                page.change(r);
            }
            solver.update(page.s.tree.snapshot(*page.s.root), frame, width, height, step * 16.0);
            // The flock's depths turn every few frames: the kept pass draws its run in another order over patches.
            const auto turn = float(step / 5 % 3);
            (void)channel.write(std::vector<weft::SceneAnchor>{{1, float(30 + step % 9), 40},
                                                               {3, 60, 200, 1, turn},
                                                               {4, 66, 203, 1, float((step / 5 + 1) % 3)},
                                                               {5, 72, 206, 1, float((step / 5 + 2) % 3)},
                                                               {6, 150, 150},
                                                               {7, 80, 210, 1, 4}});
            const auto* anchors = &channel.take(weft::SceneAnchorChannel::Reader::Render);
            const auto drawn = draw_warp(gpu, patching, slots[step % 2], frame, anchors, width, height);
            fresh.forget(0);
            const auto expected = draw_warp(gpu, fresh, fresh_slot, frame, anchors, width, height);
            require(drawn == expected, "a patched pass drew other pixels at step " + std::to_string(step));
        }
        require(patching.kept_patches > 60, "most frames patch: " + std::to_string(patching.kept_patches));
        require(patching.reordered_passes > 0, "kept passes drawn in the order of turned depths");
        // A change or two a frame, each patched: a slot writes only the records and rounded clips the patches since its
        // last draw changed, while the history reaches back to it (past the history's length).
        const auto patches = patching.kept_patches;
        const auto partial = patching.partial_uploads;
        {
            Random settled{0x779};
            ScenePage quiet(settled);
            // A box over the whole clipping box, whose corners its rounded clip cuts.
            quiet.add(*quiet.clipper, "div",
                      "position:absolute;left:0;top:0;width:150px;height:110px;background-color:#40a040");
            weft::SceneSolver quiet_solver(sheet, keyframes, text);
            weft::SceneFrame quiet_frame;
            // The page's animation ends before the frames drawn.
            quiet_solver.update(quiet.s.tree.snapshot(*quiet.s.root), quiet_frame, width, height, 0);
            (void)channel.write(std::vector<weft::SceneAnchor>{{3, 60, 200}, {4, 66, 203}, {5, 72, 206}});
            const auto* anchors = &channel.take(weft::SceneAnchorChannel::Reader::Render);
            auto& unit = quiet.s.tree.at(quiet.panels[2]->children[0]);
            for (int step = 0; step < 24; ++step) {
                quiet.style(unit, "color", step % 2 ? "#eeeeee" : "#ffcc00");
                // Now and then a rounded clip too: the slot a frame behind writes it again.
                if (step % 3 == 1) {
                    quiet.style(*quiet.clipper, "border-radius", step % 2 ? "6px" : "9px");
                }
                quiet_solver.update(quiet.s.tree.snapshot(*quiet.s.root), quiet_frame, width, height,
                                    400 + step * 16.0);
                const auto drawn = draw_warp(gpu, patching, slots[step % 2], quiet_frame, anchors, width, height);
                fresh.forget(0);
                const auto expected = draw_warp(gpu, fresh, fresh_slot, quiet_frame, anchors, width, height);
                require(drawn == expected, "a patched pass drew other pixels at quiet step " + std::to_string(step));
            }
        }
        const auto quiet_patches = patching.kept_patches - patches;
        const auto quiet_partial = patching.partial_uploads - partial;
        require(quiet_patches >= 22 && quiet_partial >= 20,
                "slots write only what the patches since changed: " + std::to_string(quiet_partial) + " of " +
                    std::to_string(quiet_patches) + " patched frames");
        std::cout << "PASS incremental WARP: 120 frames, " << patching.kept_patches - quiet_patches << " patched, "
                  << patching.patched_records << " records written again, " << patching.reordered_passes
                  << " drawn in another order; then " << quiet_patches << " patched frames, " << quiet_partial
                  << " written in part\n";
    }

    bool same_path(const weft::paint::PathPaint& a, const weft::paint::PathPaint& b) {
        const auto& x = *a.path;
        const auto& y = *b.path;
        const auto same_stroke = [](const weft::paint::Stroke& u, const weft::paint::Stroke& v) {
            return u.width == v.width && u.cap == v.cap && u.join == v.join && u.miter_limit == v.miter_limit &&
                   u.dashes == v.dashes && u.dash_offset == v.dash_offset;
        };
        return x.verbs == y.verbs && x.points == y.points && x.evenodd == y.evenodd && a.color == b.color &&
               bool(a.stroke) == bool(b.stroke) && (!a.stroke || same_stroke(*a.stroke, *b.stroke));
    }

    // Whether two lists hold the same values, part by part: items, rounded clips, payloads (paths by value), anchors
    // and runs.
    std::string list_difference(const weft::SceneDisplayList& a, const weft::SceneDisplayList& b) {
        const auto& x = a.list;
        const auto& y = b.list;
        if (x.items.size() != y.items.size()) {
            return "item count";
        }
        for (std::size_t i = 0; i < x.items.size(); ++i) {
            const auto& u = x.items[i];
            const auto& v = y.items[i];
            if (u.kind != v.kind || u.transform != v.transform || u.clip != v.clip || u.width != v.width ||
                u.height != v.height || u.opacity != v.opacity || u.payload != v.payload ||
                u.clips_begin != v.clips_begin || u.clips_count != v.clips_count || u.anchor != v.anchor) {
                const auto describe = [](const weft::paint::Item& it) {
                    std::string out = "kind " + std::to_string(int(it.kind)) + " payload " +
                                      std::to_string(it.payload) + " clips " + std::to_string(it.clips_begin) + "+" +
                                      std::to_string(it.clips_count) + " anchor " + std::to_string(it.anchor) +
                                      " opacity " + std::to_string(it.opacity) + " size " + std::to_string(it.width) +
                                      "x" + std::to_string(it.height) + " clip";
                    for (const auto c : it.clip) {
                        out += " " + std::to_string(c);
                    }
                    out += " transform";
                    for (const auto c : it.transform) {
                        out += " " + std::to_string(c);
                    }
                    return out;
                };
                return "item " + std::to_string(i) + ": " + describe(u) + " / " + describe(v);
            }
        }
        if (x.clips.size() != y.clips.size()) {
            return "clip count";
        }
        for (std::size_t i = 0; i < x.clips.size(); ++i) {
            const auto& u = x.clips[i];
            const auto& v = y.clips[i];
            if (u.rect != v.rect || u.radius_x != v.radius_x || u.radius_y != v.radius_y || u.to_local != v.to_local) {
                return "clip " + std::to_string(i);
            }
        }
        if (x.boxes.size() != y.boxes.size()) {
            return "box count";
        }
        for (std::size_t i = 0; i < x.boxes.size(); ++i) {
            const auto& u = x.boxes[i];
            const auto& v = y.boxes[i];
            const auto& g = u.gradient;
            const auto& h = v.gradient;
            bool stops = g.stops.size() == h.stops.size();
            for (std::size_t k = 0; stops && k < g.stops.size(); ++k) {
                stops = g.stops[k].offset == h.stops[k].offset && g.stops[k].color == h.stops[k].color;
            }
            if (u.radii != v.radii || u.widths != v.widths || u.edges != v.edges || u.background != v.background ||
                g.kind != h.kind || g.angle != h.angle || g.tile != h.tile || g.shape != h.shape ||
                g.repeating != h.repeating || !stops) {
                return "box " + std::to_string(i);
            }
        }
        if (x.rasters.size() != y.rasters.size()) {
            return "raster count";
        }
        for (std::size_t i = 0; i < x.rasters.size(); ++i) {
            const auto& u = x.rasters[i];
            const auto& v = y.rasters[i];
            if (!(*u.source == *v.source) || u.mask != v.mask || u.tint != v.tint || u.part != v.part ||
                u.overhang != v.overhang || u.blur != v.blur) {
                return "raster " + std::to_string(i);
            }
        }
        if (x.shadows.size() != y.shadows.size()) {
            return "shadow count";
        }
        for (std::size_t i = 0; i < x.shadows.size(); ++i) {
            const auto& u = x.shadows[i];
            const auto& v = y.shadows[i];
            if (u.color != v.color || u.x != v.x || u.y != v.y || u.blur != v.blur || u.spread != v.spread ||
                u.radii != v.radii || u.inset != v.inset) {
                return "shadow " + std::to_string(i);
            }
        }
        if (x.paths.size() != y.paths.size()) {
            return "path count";
        }
        for (std::size_t i = 0; i < x.paths.size(); ++i) {
            if (!same_path(x.paths[i], y.paths[i])) {
                return "path " + std::to_string(i);
            }
        }
        if (x.path_shadows.size() != y.path_shadows.size()) {
            return "path shadow count";
        }
        for (std::size_t i = 0; i < x.path_shadows.size(); ++i) {
            const auto& u = x.path_shadows[i];
            const auto& v = y.path_shadows[i];
            if (u.first != v.first || u.count != v.count || u.color != v.color || u.x != v.x || u.y != v.y ||
                u.blur != v.blur || u.content != v.content) {
                return "path shadow " + std::to_string(i);
            }
        }
        if (a.anchors.size() != b.anchors.size()) {
            return "anchor count";
        }
        for (std::size_t k = 0; k < a.anchors.size(); ++k) {
            const auto& u = a.anchors[k];
            const auto& v = b.anchors[k];
            if (u.element != v.element || !(u.anchoring == v.anchoring) || u.box != v.box || u.own != v.own ||
                u.first != v.first || u.end != v.end) {
                return "anchor " + std::to_string(k);
            }
        }
        if (a.runs.size() != b.runs.size()) {
            return "run count";
        }
        for (std::size_t r = 0; r < a.runs.size(); ++r) {
            if (a.runs[r].members.size() != b.runs[r].members.size()) {
                return "run " + std::to_string(r);
            }
            for (std::size_t m = 0; m < a.runs[r].members.size(); ++m) {
                const auto& u = a.runs[r].members[m];
                const auto& v = b.runs[r].members[m];
                if (u.first != v.first || u.end != v.end || u.anchor != v.anchor || u.child != v.child) {
                    return "run " + std::to_string(r) + " member " + std::to_string(m);
                }
            }
        }
        return {};
    }

    // Whether the spans a preparer keeps are a fresh list's: the same elements (the root aside, a node of each frame)
    // with the same contexts and ranges.
    std::string spans_difference(const weft::SceneListSpans& kept, const weft::SceneListSpans& fresh,
                                 const weft::SceneNode& fresh_root) {
        if (kept.size() != fresh.size()) {
            return "span count " + std::to_string(kept.size()) + " for " + std::to_string(fresh.size());
        }
        for (const auto& [node, span] : fresh) {
            if (node == &fresh_root) {
                continue;
            }
            const auto it = kept.find(node);
            if (it == kept.end()) {
                return "no span of a node";
            }
            const auto& u = it->second;
            if (u.parent != span.parent || u.clip != span.clip || u.chain_begin != span.chain_begin ||
                u.chain_count != span.chain_count || u.groups != span.groups || u.begin != span.begin ||
                u.end != span.end) {
                return "the span of a " + node->tag;
            }
        }
        return {};
    }

    // An effect that reads the time draws the frame's time: a kept pass of a frame that holds one is never patched,
    // though nothing of its tree changed.
    void effect_refusal() {
        auto& text = weft::testing::thread_scene_text();
        weft::SceneNativeWork work;
        weft::ScenePreparer preparer(text, work, 4096);
        auto program = std::make_shared<weft::paint::EffectProgram>();
        program->name = "clock";
        program->animated = true;
        weft::SceneFrame frame;
        frame.root.identity = {1, 1, 1};
        frame.root.tag = "div";
        frame.root.box = {0, 0, 200, 100};
        weft::SceneNode glow;
        glow.identity = {2, 1, 1};
        glow.tag = "div";
        glow.box = {10, 10, 60, 40};
        glow.effect = std::make_shared<const weft::SceneEffect>(weft::SceneEffect{program, {}, {}});
        frame.root.children.push_back(std::move(glow));
        frame.revision = 1;
        auto kept = preparer.prepare(frame, 1, 200, 100, std::nullopt, 0, {}, nullptr, true);
        require(kept && kept->keepable, "a kept pass with an effect");
        frame.time_ms = 16;
        frame.revision = 2;
        require(!preparer.patch(*kept, frame, 0, nullptr) &&
                    work.patch_refusals.contains("another root, size or effects"),
                "a pass with an effect that reads the time is prepared anew");
        std::cout << "PASS incremental effects: a pass with an effect is not patched" << '\n';
    }

    // The kept pass of a view patched frame by frame (ScenePreparer::patch) against a pass prepared anew for each
    // frame: the same records, masks, clips, batches, images, anchors and runs. Some panels follow anchors. The
    // flock's depths stay: a list holds a run in the order of the depths it was listed with, and the kept pass orders
    // it again each frame (the WARP test turns them).
    void pass_differential() {
        const std::string sheet = ".fade{transition-property:opacity;transition-duration:50ms;"
                                  "transition-timing-function:linear}.pulse{animation:pulse 120ms linear 2}"
                                  ".pulse div{color:#00ff00}";
        const std::string keyframes = "@keyframes pulse{from{opacity:.25;color:#ff0000}to{opacity:1;color:#00ff00}}";
        auto& text = weft::testing::thread_scene_text();
        weft::SceneNativeWork work;
        // Caches that hold every image, so that both passes sample the same ones.
        weft::SceneCacheBounds bounds;
        bounds.entries = 1u << 20;
        bounds.bytes = std::size_t(1) << 32;
        weft::ScenePreparer preparer(text, work, 4096, {}, bounds);
        weft::SceneAnchorChannel channel;
        // Brings a view's kept pass to frame, by a patch where it can or prepared anew, and compares it, its list and
        // its spans with fresh ones (prepared for view + 1): whether it patched.
        const auto step_view = [&](const weft::SceneFrame& frame, std::shared_ptr<weft::ScenePrepared>& kept,
                                   unsigned view, const weft::SceneAnchorTable* anchors, const std::string& at) {
            const bool done = kept && preparer.patch(*kept, frame, view, anchors);
            if (!done) {
                kept = preparer.prepare(frame, 2, 800, 640, std::nullopt, view, {}, anchors, true);
                require(kept && preparer.listed(view), "a kept pass" + at);
            }
            const auto fresh = preparer.prepare(frame, 2, 800, 640, std::nullopt, view + 1, {}, anchors, true);
            require(bool(fresh), "a fresh pass" + at);
            // The list and spans the patch keeps are a fresh list's.
            weft::SceneListSpans fresh_spans;
            const auto list =
                weft::scene_display_list(frame, {}, &text, 2, nullptr, {anchors, nullptr, true}, {&fresh_spans});
            const auto kept_list = list_difference(*preparer.listed(view), list);
            require(kept_list.empty(), "the kept list differs from a fresh one: " + kept_list + at);
            const auto difference = pass_difference(*kept, *fresh);
            require(difference.empty(), "the patched pass differs from a fresh one: " + difference + at);
            const auto kept_spans = spans_difference(*preparer.listed_spans(view), fresh_spans, frame.root);
            require(kept_spans.empty(), "the kept spans differ from a fresh list's: " + kept_spans + at);
            return done;
        };
        std::size_t patched{};
        std::size_t prepared_again{};
        for (std::uint32_t seed : {0x13u, 0x4d2u}) {
            Random r{seed};
            ScenePage page(r);
            // Two panels follow anchors 1 and 2.
            for (unsigned k = 0; k < 2; ++k) {
                page.s.set(*page.panels[k], "data-anchor", std::to_string(k + 1));
            }
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            for (int step = 0; step < 200; ++step) {
                const auto at = " at step " + std::to_string(step) + " of seed " + std::to_string(seed);
                for (std::size_t k = r.below(5) ? 1 : 1 + r.below(3); k > 0; --k) {
                    page.change(r);
                }
                const double time = step * 16.0;
                solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, time);
                (void)channel.write(std::vector<weft::SceneAnchor>{{1, float(20 + step % 7), 30},
                                                                   {2, 200, float(40 + step % 5)},
                                                                   {3, float(50 + step % 3), 300, 1, 0},
                                                                   {4, 70, 300, 1, 1},
                                                                   {5, 90, 300, 1, 2},
                                                                   {6, 150, 150},
                                                                   {7, 110, 300, 1, 5}});
                const auto* anchors = &channel.take(weft::SceneAnchorChannel::Reader::Render);
                if (step_view(frame, kept, 0, anchors, at)) {
                    ++patched;
                } else {
                    ++prepared_again;
                }
            }
        }
        require(patched > 200, "most frames patch the kept pass: " + std::to_string(patched) + " of " +
                                   std::to_string(patched + prepared_again));
        // Frames of a change or two from a pass kept for the frame before, each to its own view: whether they patch it,
        // and the reason a refusal gives.
        const auto refusals = [&](const char* reason) {
            return work.patch_refusals[reason];
        };
        // The refusals so far, for a failure's message.
        const auto refused = [&] {
            std::string out = " (refusals:";
            for (const auto& [reason, count] : work.patch_refusals) {
                out += " " + std::to_string(count) + " " + reason + ";";
            }
            return out + ")";
        };
        // An svg whose content alone changed is listed again whole, in place.
        {
            Random r{0x51};
            ScenePage page(r);
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 0);
            (void)step_view(frame, kept, 2, nullptr, " (svg)");
            page.s.set(page.s.tree.at(page.icon->children[0]), "fill", "#aa3355");
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 16);
            require(step_view(frame, kept, 2, nullptr, " (svg)"), "an svg whose path changed is patched" + refused());
        }
        // A part whose opacity group moved from one child to another holds as many items, of other kinds.
        {
            weft::testing::NativeScene s;
            const auto add = [&](weft::dom::Node& parent, const std::string& style) -> weft::dom::Node& {
                auto& n = s.html("div");
                s.set(n, "style", style);
                s.tree.insert(parent, n, nullptr);
                return n;
            };
            const std::string box = "position:absolute;top:0;width:40px;height:40px;background-color:#404040;";
            // Boxes besides the part, so that it is less than half the list.
            for (int i = 0; i < 8; ++i) {
                add(*s.root, "position:absolute;left:" + std::to_string(i * 24) +
                                 "px;top:100px;width:20px;height:20px;background-color:#303030");
            }
            auto& p = add(*s.root, "position:absolute;left:30px;top:10px;width:100px;height:60px;"
                                   "background-color:#202020");
            auto& a = add(p, box + "left:0;opacity:0.5");
            add(a, "width:20px;height:20px;background-color:#808080");
            auto& b = add(p, box + "left:50px");
            add(b, "width:20px;height:20px;background-color:#808080");
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            solver.update(s.tree.snapshot(*s.root), frame, 400, 320, 0);
            (void)step_view(frame, kept, 4, nullptr, " (groups)");
            s.set(p, "style",
                  "position:absolute;left:30px;top:10px;width:100px;height:60px;background-color:#202020;"
                  "transform:translate(1px,0px)");
            s.set(a, "style", box + "left:0");
            s.set(b, "style", box + "left:50px;opacity:0.5");
            solver.update(s.tree.snapshot(*s.root), frame, 400, 320, 16);
            const auto before = refusals("a part of other kinds");
            require(!step_view(frame, kept, 4, nullptr, " (groups)"),
                    "a part whose group moved to another child is prepared anew");
            require(refusals("a part of other kinds") == before + 1, "a part of other kinds" + refused());
        }
        // A text grown too tall for an atlas page takes a texture of its own, in another batch.
        {
            Random r{0x52};
            ScenePage page(r);
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 0);
            (void)step_view(frame, kept, 6, nullptr, " (batches)");
            page.style(*page.caption, "font-size", "80px");
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 16);
            const auto before = refusals("an image in another batch");
            require(!step_view(frame, kept, 6, nullptr, " (batches)"),
                    "a text that leaves its atlas page is prepared anew");
            require(refusals("an image in another batch") == before + 1, "an image in another batch" + refused());
        }
        // A translucent element inside an anchored one, listed again: its group composites over the root's box, as
        // the whole list's does.
        {
            Random r{0x53};
            ScenePage page(r);
            page.s.set(*page.panels[0], "data-anchor", "1");
            auto& unit = page.s.tree.at(page.panels[0]->children[0]);
            page.style(unit, "opacity", "0.5");
            weft::SceneAnchorChannel own;
            (void)own.write(std::vector<weft::SceneAnchor>{{1, 40, 50}});
            const auto* anchors = &own.take(weft::SceneAnchorChannel::Reader::Render);
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 0);
            (void)step_view(frame, kept, 8, anchors, " (anchored group)");
            page.style(unit, "transform", "translate(2px,1px)");
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 16);
            require(step_view(frame, kept, 8, anchors, " (anchored group)"),
                    "a translucent element inside an anchored one is patched" + refused());
        }
        // A frame that changes most of the list: listing it a part at a time costs more than listing it whole.
        {
            Random r{0x54};
            ScenePage page(r);
            weft::SceneSolver solver(sheet, keyframes, text);
            weft::SceneFrame frame;
            std::shared_ptr<weft::ScenePrepared> kept;
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 0);
            (void)step_view(frame, kept, 10, nullptr, " (most)");
            for (auto* panel : page.panels) {
                page.style(*panel, "transform", "translate(1px,1px)");
            }
            page.style(*page.clipper, "transform", "translate(1px,1px)");
            solver.update(page.s.tree.snapshot(*page.s.root), frame, 400, 320, 16);
            const auto before = refusals("most of the list changed");
            require(!step_view(frame, kept, 10, nullptr, " (most)"),
                    "a frame that changes most of the list is prepared anew");
            require(refusals("most of the list changed") == before + 1, "most of the list changed" + refused());
        }
        std::cout << "PASS incremental pass: " << patched << " patched passes and " << prepared_again
                  << " prepared anew, each as a fresh pass\n";
    }
} // namespace

int main(int argc, char** argv) {
#ifdef _DEBUG
    // A debug library's assertion reports on stderr and fails, rather than waiting on a dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    try {
        // The D3D11 renderer's reference raster (color glyph images) needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        // A section's name runs that section alone.
        const std::pair<const char*, void (*)()> sections[]{
            {"cascade", cascade_differential}, {"scene", scene_differential},   {"particles", particles_differential},
            {"inline", inline_differential},   {"layout", layout_differential}, {"effects", effect_refusal},
            {"pass", pass_differential},       {"warp", warp_differential}};
        for (const auto& [name, section] : sections) {
            if (argc < 2 || std::string_view(argv[1]) == name) {
                section();
                std::cout.flush();
            }
        }
        std::cout << "PASS incremental contract\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL incremental contract: " << e.what() << '\n';
        return 1;
    }
}
