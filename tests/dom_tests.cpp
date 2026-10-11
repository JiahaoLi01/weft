#include "constants/common.hpp"
#include "constants/dom.hpp"
#include "dom/attributes.hpp"
#include "dom/node_table.hpp"
#include "dom/tree.hpp"
#include "support/check.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// src/dom besides the selector engine: the native tree, per-node side tables and HTML attribute values. The tree runs
// here without a script engine.
namespace {
    using weft::testing::require;
    namespace dom = weft::dom;
    using Ids = std::vector<weft::NodeIdentity>;

    constexpr int element_type = weft::constants::element_node_type;
    constexpr int text_type = weft::constants::text_node_type;
    constexpr int document_type = weft::constants::document_node_type;

    // A document with html > body, as the script DOM bootstraps it.
    struct Page {
        dom::Tree tree{1};
        dom::Node* document{};
        dom::Node* html{};
        dom::Node* body{};

        Page() {
            document = &tree.create(document_type, "#document", "", {});
            html = &element("html");
            body = &element("body");
            tree.insert(*document, *html, nullptr);
            tree.insert(*html, *body, nullptr);
        }

        dom::Node& element(std::string local) {
            return tree.create(element_type, std::move(local), weft::constants::xhtml_namespace_uri,
                               document->identity);
        }

        dom::Node& text(std::string data) {
            auto& n = tree.create(text_type, "#text", "", document->identity);
            n.data = std::move(data);
            return n;
        }
    };

    std::vector<dom::Selector> selectors(std::string_view text) {
        const dom::SelectorGrammar grammar{true, true, weft::Diag::DomSelector, weft::Diag::DomUnsupportedSelector};
        return dom::parse_selector_list(text, grammar, weft::constants::dom::max_selector_list_items).value();
    }

    std::optional<std::uint32_t> width(const char* text) {
        const std::string attribute(text);
        const auto value = dom::canvas_dimension(&attribute, dom::CanvasSide::Width);
        return value ? std::optional(*value) : std::nullopt;
    }

    void tree_edges() {
        Page p;
        auto& a = p.element("div");
        auto& b = p.element("span");
        auto& c = p.element("p");
        require(p.tree.insert(*p.body, a, nullptr).moved, "append");
        p.tree.insert(*p.body, b, nullptr);
        p.tree.insert(*p.body, c, &a);
        require(p.body->children == Ids{c.identity, a.identity, b.identity} && a.parent == p.body->identity,
                "insert order");
        // A move within the parent reports the parent as the former one.
        const auto reorder = p.tree.insert(*p.body, b, &c);
        require(reorder.moved && reorder.former_parent == p.body->identity &&
                    p.body->children == Ids{b.identity, c.identity, a.identity},
                "reorder");
        const auto revision = p.tree.revision();
        require(!p.tree.insert(*p.body, a, &a).moved && p.tree.revision() == revision, "insert before itself");
        const auto move = p.tree.insert(a, b, nullptr);
        require(move.former_parent == p.body->identity && b.parent == a.identity &&
                    p.body->children == Ids{c.identity, a.identity},
                "move to another parent");
        require(p.tree.detach(b) == a.identity && !b.parent.generation && a.children.empty() &&
                    !p.tree.detach(b).generation,
                "detach");
        require(p.tree.connected(a) && !p.tree.connected(b) && p.tree.connected(*p.document), "connected");
        p.tree.insert(a, p.text("ab"), nullptr);
        p.tree.insert(c, p.text("c"), nullptr);
        require(p.tree.text(*p.body) == "cab" && p.tree.text(*p.document) == "cab", "textContent");
    }

    void tree_refusals() {
        Page p;
        auto& a = p.element("div");
        auto& b = p.element("span");
        auto& words = p.text("words");
        p.tree.insert(*p.body, a, nullptr);
        p.tree.insert(a, words, nullptr);
        auto& other = p.tree.create(document_type, "#document", "", {});
        auto& foreign = p.tree.create(element_type, "div", weft::constants::xhtml_namespace_uri, other.identity);
        // Each refusal throws before changing anything.
        const auto refused = [&](auto&& insert, std::string_view code) {
            const auto revision = p.tree.revision();
            const auto children = p.body->children;
            try {
                insert();
            } catch (const std::runtime_error& e) {
                return std::string_view(e.what()).starts_with(code) && p.tree.revision() == revision &&
                       p.body->children == children && !b.parent.generation;
            }
            return false;
        };
        require(refused(
                    [&] {
                        p.tree.insert(words, b, nullptr);
                    },
                    "WEFT_HIERARCHY"),
                "text parent");
        require(refused(
                    [&] {
                        p.tree.insert(*p.body, *p.document, nullptr);
                    },
                    "WEFT_HIERARCHY"),
                "Document child");
        require(refused(
                    [&] {
                        p.tree.insert(a, *p.body, nullptr);
                    },
                    "WEFT_HIERARCHY"),
                "cycle");
        require(refused(
                    [&] {
                        p.tree.insert(*p.body, foreign, nullptr);
                    },
                    "WEFT_UNSUPPORTED_DOM"),
                "cross-document");
        require(refused(
                    [&] {
                        p.tree.insert(*p.body, b, &words);
                    },
                    "WEFT_NOT_FOUND"),
                "reference child elsewhere");
        require(p.tree.document() == p.document->identity, "the first Document is the tree's");
        // A node below another Document is not connected to the tree's.
        p.tree.insert(other, foreign, nullptr);
        require(!p.tree.connected(foreign) && p.tree.connected(a), "connected to the tree's Document only");
        // Document, html and body take three levels; a chain below body may fill the rest, one more is refused.
        dom::Node* bottom = &a;
        for (int depth = 4; depth < weft::constants::dom::max_depth; ++depth) {
            auto& next = p.element("div");
            p.tree.insert(*bottom, next, nullptr);
            bottom = &next;
        }
        require(refused(
                    [&] {
                        p.tree.insert(*bottom, b, nullptr);
                    },
                    "WEFT_DOM_DEPTH_LIMIT"),
                "depth bound");
        // A detached subtree too tall for its new place is refused as a whole: one level deeper than before.
        p.tree.detach(a);
        auto& deeper = p.element("section");
        p.tree.insert(*p.body, deeper, nullptr);
        require(refused(
                    [&] {
                        p.tree.insert(deeper, a, nullptr);
                    },
                    "WEFT_DOM_DEPTH_LIMIT") &&
                    !a.parent.generation,
                "tall subtree");
        p.tree.insert(*p.body, a, nullptr);
        require(a.parent == p.body->identity, "the same subtree fits at its former depth");
    }

    void tree_journal() {
        Page p;
        const auto start = p.tree.revision();
        auto& a = p.element("div");
        p.tree.insert(*p.body, a, nullptr);
        require(p.tree.revision() == start + 1 && p.body->subtree_revision == p.tree.revision() &&
                    p.document->subtree_revision == p.tree.revision() && a.subtree_revision == p.tree.revision(),
                "subtree revisions up to the root");
        const auto changes = p.tree.changes_since(start);
        require(!changes.full_required && changes.nodes == Ids{a.identity} && changes.revision == p.tree.revision(),
                "changes since");
        require(p.tree.changes_since(0).full_required, "revision 0 needs a full snapshot");
        bool future{};
        try {
            (void)p.tree.changes_since(p.tree.revision() + 1);
        } catch (const std::invalid_argument&) {
            future = true;
        }
        require(future, "future revision");
        const auto mutations = p.tree.mutations();
        for (int i = 0; i <= weft::constants::dom::change_journal_capacity; ++i) {
            p.tree.changed(a);
        }
        require(p.tree.changes_since(start).full_required &&
                    p.tree.mutations() == mutations + weft::constants::dom::change_journal_capacity + 1,
                "journal overflow");
        // A released node leaves the journal's answers.
        const auto before = p.tree.revision();
        p.tree.changed(a);
        p.tree.detach(a);
        const auto released = a.identity;
        p.tree.release(released);
        const auto later = p.tree.changes_since(before);
        require(std::find(later.nodes.begin(), later.nodes.end(), released) == later.nodes.end() &&
                    std::find(later.nodes.begin(), later.nodes.end(), p.body->identity) != later.nodes.end(),
                "released node in the journal");
    }

    void tree_copies() {
        Page p;
        auto& a = p.element("div");
        p.tree.insert(*p.body, a, nullptr);
        a.attributes.push_back({"id", "id", "", "main"});
        p.tree.insert(a, p.text("a\"b\n"), nullptr);
        const auto copied = p.tree.snapshot_nodes();
        const auto snapshot = p.tree.snapshot(*p.body);
        require(snapshot.identity == p.body->identity && snapshot.children.size() == 1 &&
                    snapshot.children[0].attributes.at("id") == "main" &&
                    snapshot.children[0].children[0].data == "a\"b\n" && p.tree.snapshot_nodes() == copied + 3,
                "snapshot");
        require(p.tree.dump(a) == R"({"nodeType":1,"localName":"div","namespaceURI":"http://www.w3.org/1999/xhtml",)"
                                  R"("textContent":"a\"b\u000a","attributes":{"id":"main"},"children":[{"nodeType":3,)"
                                  R"("localName":"#text","namespaceURI":"","textContent":"a\"b\u000a","attributes":{},)"
                                  R"("children":[]}]})",
                "dump");
        a.attributes.push_back({"xlink:href", "href", "http://www.w3.org/1999/xlink", "#a"});
        bool refused{};
        try {
            (void)p.tree.snapshot(*p.body);
        } catch (const std::runtime_error& e) {
            refused = std::string_view(e.what()).starts_with("WEFT_PAGE_PROFILE");
        }
        require(refused, "namespaced attributes are outside the snapshot profile");
    }

    void tree_queries() {
        Page p;
        auto& list = p.element("ul");
        auto& item = p.element("li");
        auto& link = p.element("a");
        p.tree.insert(*p.body, list, nullptr);
        p.tree.insert(list, item, nullptr);
        p.tree.insert(item, link, nullptr);
        std::vector<dom::Node*> found;
        p.tree.query(*p.document, selectors("ul > li, a"), found, false);
        require(found == std::vector<dom::Node*>{&item, &link}, "query in tree order");
        found.clear();
        // Ancestors continue past the query root.
        p.tree.query(item, selectors("body a"), found, true);
        require(found == std::vector<dom::Node*>{&link} && p.tree.matches(*p.html, selectors(":root")) &&
                    !p.tree.matches(*p.document, selectors("*")),
                "query scope");
    }

    // A refreshed copy equals a fresh snapshot after random mutations, reports exactly whether its content changed,
    // carries the nodes' revisions, and costs nothing when nothing changed. The mutations include changes recorded
    // without a content difference.
    void tree_refresh_differential() {
        Page p;
        std::vector<dom::Node*> pool;
        for (int i = 0; i < 24; ++i) {
            pool.push_back(i % 4 == 3 ? &p.text("t" + std::to_string(i)) : &p.element(i % 2 ? "div" : "span"));
        }
        std::uint32_t seed = 0x95a;
        const auto next = [&] {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            return seed >> 8;
        };
        const auto revisions_match = [&](auto&& self, const weft::DomSnapshot& copy) -> bool {
            if (copy.revision != p.tree.at(copy.identity).subtree_revision) {
                return false;
            }
            for (const auto& child : copy.children) {
                if (!self(self, child)) {
                    return false;
                }
            }
            return true;
        };
        auto copy = p.tree.snapshot(*p.body);
        // Copies kept by update_copy: one from tree snapshots, one from values whose revisions were cleared.
        auto mirror = copy;
        auto plain_mirror = copy;
        const auto clear_revisions = [](auto&& self, weft::DomSnapshot& value) -> void {
            value.revision = 0;
            for (auto& child : value.children) {
                self(self, child);
            }
        };
        constexpr int steps = 6000;
        for (int step = 0; step < steps; ++step) {
            auto& n = *pool[next() % pool.size()];
            switch (next() % 8) {
                case 0: {
                    dom::Node* parent = p.body;
                    if (const auto pick = next() % (pool.size() + 1);
                        pick < pool.size() && pool[pick]->type == element_type) {
                        parent = pool[pick];
                    }
                    dom::Node* before = parent->children.empty() || next() % 3 == 0
                                            ? nullptr
                                            : &p.tree.at(parent->children[next() % parent->children.size()]);
                    try {
                        p.tree.insert(*parent, n, before);
                    } catch (const std::runtime_error&) {
                        // Cycles and other refusals leave the tree as it was.
                    }
                    break;
                }
                case 1:
                    p.tree.detach(n);
                    break;
                case 2:
                    if (n.type == text_type) {
                        n.data = "t" + std::to_string(next() % 5);
                        p.tree.changed(n);
                    }
                    break;
                case 3:
                    if (n.type == element_type) {
                        const std::string name = next() % 2 ? "id" : "data-x";
                        const auto value = std::to_string(next() % 3);
                        auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const dom::Attribute& a) {
                            return a.name == name;
                        });
                        if (it == n.attributes.end()) {
                            n.attributes.push_back({name, name, "", value});
                        } else {
                            it->value = value;
                        }
                        p.tree.changed(n);
                    }
                    break;
                case 4:
                    if (!n.attributes.empty()) {
                        n.attributes.erase(n.attributes.begin() + std::ptrdiff_t(next() % n.attributes.size()));
                        p.tree.changed(n);
                    }
                    break;
                case 5: {
                    bool& flag = next() % 2 ? n.hovered : n.active;
                    flag = !flag;
                    p.tree.changed(n);
                    break;
                }
                case 6:
                    n.scroll_top = double(next() % 3);
                    p.tree.changed(n);
                    break;
                default:
                    // A recorded change with no content difference.
                    p.tree.changed(n);
                    break;
            }
            if (next() % 3) {
                continue;
            }
            const auto before = copy;
            const bool changed = p.tree.refresh(*p.body, copy);
            const auto fresh = p.tree.snapshot(*p.body);
            const auto at = " at step " + std::to_string(step);
            require(copy == fresh, "refresh differs from a fresh snapshot" + at);
            require(changed == (before != fresh), "refresh misreported a change" + at);
            // The revision shortcut never disagrees with full content equality, for equal or older copies.
            require(dom::same_content(copy, fresh) && dom::same_content(before, fresh) == (before == fresh) &&
                        dom::same_content(fresh, before) == (before == fresh),
                    "same_content disagrees with content equality" + at);
            require(revisions_match(revisions_match, copy), "refreshed revisions" + at);
            const auto mirrored_before = mirror;
            require(dom::update_copy(mirror, fresh) == (mirrored_before != fresh) && mirror == fresh &&
                        revisions_match(revisions_match, mirror),
                    "update_copy from a snapshot" + at);
            auto plain = fresh;
            clear_revisions(clear_revisions, plain);
            const auto plain_before = plain_mirror;
            require(dom::update_copy(plain_mirror, plain) == (plain_before != plain) && plain_mirror == plain,
                    "update_copy from a value without revisions" + at);
            require(dom::same_content(plain, fresh) &&
                        dom::same_content(plain_before, fresh) == (plain_before == fresh),
                    "same_content without revisions" + at);
            require(dom::same_content(plain_before, plain) == (plain_before == plain),
                    "same_content between values without revisions" + at);
            const auto visited = p.tree.snapshot_nodes();
            require(!p.tree.refresh(*p.body, copy) && p.tree.snapshot_nodes() == visited,
                    "refresh of an unchanged subtree did work" + at);
        }
        // A copy of another node is replaced.
        auto other = p.tree.snapshot(*p.html);
        require(p.tree.refresh(*p.body, other) && other == p.tree.snapshot(*p.body), "copy of another node");
    }

    // Snapshot equality is content equality: every field but the revision counts.
    void snapshot_equality() {
        Page p;
        auto& a = p.element("div");
        p.tree.insert(*p.body, a, nullptr);
        const auto base = p.tree.snapshot(*p.body);
        auto copy = base;
        copy.revision += 7;
        require(copy == base, "revision is not content");
        // Two new nodes share the initial revision; only identity tells their copies apart.
        auto& first = p.element("div");
        auto& second = p.element("div");
        const auto one = p.tree.snapshot(first);
        const auto two = p.tree.snapshot(second);
        require(one.revision == two.revision && !dom::same_content(one, two) && one != two,
                "copies of two nodes at one revision");
        const std::vector<void (*)(weft::DomSnapshot&)> edits{
            [](weft::DomSnapshot& s) {
                ++s.identity.generation;
            },
            [](weft::DomSnapshot& s) {
                ++s.type;
            },
            [](weft::DomSnapshot& s) {
                s.local_name += "x";
            },
            [](weft::DomSnapshot& s) {
                s.namespace_uri += "x";
            },
            [](weft::DomSnapshot& s) {
                s.data += "x";
            },
            [](weft::DomSnapshot& s) {
                s.attributes["x"] = "1";
            },
            [](weft::DomSnapshot& s) {
                s.children.clear();
            },
            [](weft::DomSnapshot& s) {
                s.scroll_left += 1;
            },
            [](weft::DomSnapshot& s) {
                s.scroll_top += 1;
            },
            [](weft::DomSnapshot& s) {
                s.hovered = !s.hovered;
            },
            [](weft::DomSnapshot& s) {
                s.active = !s.active;
            },
            [](weft::DomSnapshot& s) {
                s.children[0].data += "x";
            },
        };
        for (std::size_t i = 0; i < edits.size(); ++i) {
            auto edited = base;
            edits[i](edited);
            require(edited != base, "snapshot equality misses content field " + std::to_string(i));
        }
    }

    void tree_lifetimes() {
        Page p;
        auto& a = p.element("div");
        const auto id = a.identity;
        const auto taken = p.tree.release(id);
        require(taken.get() == &a && !p.tree.find(id) && p.tree.size() == 3 && !p.tree.release(id), "release");
        auto& b = p.element("div");
        require(b.identity.slot == id.slot && b.identity.generation == id.generation + 1, "slot reuse");
        // A new epoch: identities of the old one never resolve, even where slot and generation would.
        const Ids all{p.body->identity, p.html->identity, p.document->identity, b.identity};
        for (const auto& node : all) {
            p.tree.release(node);
        }
        p.tree.reset(2);
        auto& document = p.tree.create(document_type, "#document", "", {});
        require(document.identity.view_epoch == 2 && !p.tree.find(all[2]) && p.tree.document() == document.identity &&
                    p.tree.revision() == 1,
                "epoch reset");
    }

    void node_tables() {
        // A node's value is found by its slot and generation only; erasing destroys it.
        auto shared = std::make_shared<int>(7);
        dom::NodeTable<std::shared_ptr<int>> table;
        const weft::NodeIdentity node{3, 1, 1};
        table.emplace(node, shared);
        require(table.find(node) && **table.find(node) == 7 && table.size() == 1 && shared.use_count() == 2, "emplace");
        require(!table.find({3, 2, 1}) && !table.find({4, 1, 1}) && !table.find({100000, 1, 1}),
                "a later occupant or another slot found the value");
        table.erase({3, 2, 1});
        require(table.size() == 1 && shared.use_count() == 2, "a stale identity erased the value");
        table.erase(node);
        require(!table.find(node) && table.size() == 0 && shared.use_count() == 1, "erase kept the value");
        // The slot's next occupant starts without a value and may get its own; slots are independent.
        require(!table.find({3, 2, 1}), "the next occupant inherited a value");
        table.emplace({3, 2, 1}, std::make_shared<int>(8));
        table.emplace({0, 5, 1}, std::make_shared<int>(9));
        require(table.size() == 2 && **table.find({3, 2, 1}) == 8 && **table.find({0, 5, 1}) == 9 && !table.find(node),
                "independent slots");
    }

    void canvas_sizes() {
        // Canvas sides: HTML's rules for parsing non-negative integers, or the 300 by 150 default.
        require(*dom::canvas_dimension(nullptr, dom::CanvasSide::Width) == 300 &&
                    *dom::canvas_dimension(nullptr, dom::CanvasSide::Height) == 150,
                "absent attribute");
        const std::string invalid = "x";
        require(*dom::canvas_dimension(&invalid, dom::CanvasSide::Height) == 150, "invalid height");

        struct Case {
            const char* text;
            std::uint32_t expected;
        };

        for (const auto& [text, expected] :
             {Case{"", 300}, Case{" ", 300}, Case{"12", 12}, Case{" \t\n\f\r12", 12}, Case{"+5", 5}, Case{"7px", 7},
              Case{"7.9", 7}, Case{"1e3", 1}, Case{"0002048", 2048}, Case{"0", 0}, Case{"-0", 0}, Case{"-00", 0},
              Case{"-1", 300}, Case{"-99999", 300}, Case{"abc", 300}, Case{"\v12", 300}, Case{"+-1", 300},
              Case{"- 1", 300}}) {
            require(width(text) == expected, std::string("canvas width of '") + text + "'");
        }
        // Above the profile bound, however long the digit run.
        for (const char* text : {"2049", "99999999999999999999999"}) {
            const std::string attribute(text);
            const auto value = dom::canvas_dimension(&attribute, dom::CanvasSide::Width);
            require(!value && value.error().code == weft::Diag::CanvasSizeLimit, std::string("accepted ") + text);
        }
    }
} // namespace

int main() {
    try {
        tree_edges();
        tree_refusals();
        tree_journal();
        tree_copies();
        tree_queries();
        tree_lifetimes();
        tree_refresh_differential();
        snapshot_equality();
        node_tables();
        canvas_sizes();
        std::cout << "PASS DOM: native tree edges, refusals, journal, copies, queries and lifetimes without a script "
                     "engine; incremental snapshots and copies against full ones; node side tables; canvas sizes\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
