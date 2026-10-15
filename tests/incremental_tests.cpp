#include "constants/common.hpp"
#include "dom/tree.hpp"
#include "paint/particles.hpp"
#include "scene/scene.hpp"
#include "scene/scene_particles.hpp"
#include "style/style.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
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

    void cascade_differential() {
        const std::string sheet = ".on span{color:#ff0000}.on>div{font-size:24px}.hot{background-color:#336699}"
                                  "[data-tone=gold] div{--ink:#ffd700}div:hover>span{color:#00ff00}"
                                  "span{color:var(--ink,#123456)}.hot span{width:inherit}";
        const auto profile = weft::scene_style_profile();
        const weft::CompiledStyle compiled(sheet, profile);
        for (std::uint32_t seed : {0x51u, 0x9e3u, 0x1234u}) {
            Random r{seed};
            Page page(r);
            weft::ComputedStyle output;
            weft::StyleMemo memo;
            std::optional<weft::DomSnapshot> previous;
            std::uint64_t stamp{};
            for (int step = 0; step < 400; ++step) {
                const auto at = " at step " + std::to_string(step) + " of seed " + std::to_string(seed);
                // Now and then several changes at once.
                for (std::size_t k = r.below(4) ? 1 : 1 + r.below(5); k > 0; --k) {
                    page.change(r);
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
        auto& small = s.html("div");
        s.tree.insert(*s.root, small, nullptr);
        for (int i = 0; i < weft::constants::max_document_nodes - 4; ++i) {
            s.tree.insert(large, s.html("i"), nullptr);
        }
        weft::ComputedStyle output;
        weft::StyleMemo memo;
        const auto first = s.tree.snapshot(*s.root);
        compiled.update(first, nullptr, output, memo, 1);
        s.tree.insert(small, s.html("b"), nullptr);
        const auto at_bound = s.tree.snapshot(*s.root);
        compiled.update(at_bound, &first, output, memo, 2);
        require(output.subtree_nodes == std::size_t(weft::constants::max_document_nodes), "a document at the bound");
        s.tree.insert(small, s.html("b"), nullptr);
        const auto over = s.tree.snapshot(*s.root);
        bool refused{};
        try {
            compiled.update(over, &at_bound, output, memo, 3);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "a document over the node bound fails though most of it is kept");
        std::cout << "PASS incremental cascade: 1,200 random updates against full cascades\n";
    }

    // A scene page: positioned panels of nameplates (a name over a bar), text in lines, a flex row, an animation, a
    // transition, and the changes a game UI makes to them.
    struct ScenePage {
        weft::testing::NativeScene s;
        std::vector<weft::dom::Node*> elements;
        std::vector<weft::dom::Node*> texts;

        explicit ScenePage(Random& r) {
            for (int p = 0; p < 4; ++p) {
                auto& panel = add(*s.root, "div",
                                  "position:absolute;left:" + std::to_string(10 + p * 90) + "px;top:10px;width:80px");
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
            auto& row = add(*s.root, "div", "position:absolute;left:200px;top:200px;display:flex;gap:4px");
            for (int i = 0; i < 4; ++i) {
                add(row, "div", "width:20px;height:20px;background-color:#3366" + std::to_string(10 + i * 20));
            }
            add(*s.root, "div", "position:absolute;left:10px;top:260px;width:30px;height:30px")
                .attributes.push_back({"class", "class", "", "pulse"});
            s.tree.changed(*elements.back());
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
            if (text.back() != ';') {
                text += ';';
            }
            s.set(n, "style", text.substr(1) + property + ":" + value);
        }

        void change(Random& r) {
            auto& n = *elements[r.below(elements.size())];
            switch (r.below(12)) {
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
                    t.data = r.below(2) ? "Short" : "A longer text that takes more of the line";
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
        }
        // Layout may move a sibling's box, which gives it new storage; most stay.
        require(compared > 100 && shared * 10 > compared * 8,
                "untouched subtrees keep their storage: " + std::to_string(shared) + " of " + std::to_string(compared));
        std::cout << "PASS incremental scene: 900 random updates against full builds; " << shared << " of " << compared
                  << " untouched subtrees shared\n";
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
} // namespace

int main() {
    try {
        cascade_differential();
        scene_differential();
        particles_differential();
        inline_differential();
        std::cout << "PASS incremental contract\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL incremental contract: " << e.what() << '\n';
        return 1;
    }
}
