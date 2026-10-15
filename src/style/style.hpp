#pragma once
#include "dom/node.hpp"
#include "style/values.hpp"
#include <array>
#include <optional>
#include <memory>

namespace weft {
    // Bounded M0 author-style evaluator. All inputs/outputs are owned values. It
    // does not call JS, measure text, run layout, or retain DOM/native resources.
    enum class CssGrammar {
        Tokens,
        Length,
        Number,
        Color,
        Keyword,
        Radius
    };

    struct CssProperty {
        std::string initial;
        bool inherited{};
        CssGrammar grammar{CssGrammar::Tokens};
        std::vector<std::string> keywords;
    };

    struct StyleProfile {
        std::map<std::string, CssProperty> properties;
        std::string ua_stylesheet;
        // Takes the inset shorthand: one value for every side, or one to four (top, right, bottom, left) with
        // four_value_edges.
        bool single_value_inset{};
        bool four_value_edges{}; // Expand margin/padding/border-width before cascade.
        bool flow_shorthands{};  // Corner radii and overflow axes, before cascade.
        bool scene_syntax{};     // Explicit scene profile: escaped selectors, SVG, raw shorthands.
    };

    // The custom properties an element's children inherit, as the cascade resolved them (opaque).
    struct StyleEnvironment;

    struct ComputedStyle {
        NodeIdentity identity;
        std::string local_name;
        // Sparse values: absent properties use the formatting profile's defaults.
        // Explicit CSS-wide keywords and invalid computed declarations are resolved.
        StyleValues values;
        // nullopt is guaranteed-invalid; an empty string is a valid empty value.
        std::map<std::string, std::optional<std::string>> custom_properties;
        std::vector<std::string> invalid_at_computed_value;
        std::vector<ComputedStyle> children;           // Element children, DOM order.
        std::vector<std::string> specified_properties; // Cascade provenance for animated inheritance.

        // Bookkeeping of the cascade, not content: == ignores it. stamp is the update (CompiledStyle::update) that last
        // changed the node's own values (its identity, name, values, custom properties or lists), children_stamp the
        // one that last changed which element children it has or their order, and subtree_stamp the latest of either
        // in its subtree. What the node charged against the cascade's bounds, and what its subtree did,
        // which an update charges again for what it keeps without computing it: the node's own token work and output
        // bytes, and its subtree's nodes, token work and output bytes. environment holds the custom properties its
        // children inherit.
        std::uint64_t stamp{};
        std::uint64_t children_stamp{};
        std::uint64_t subtree_stamp{};
        std::size_t own_work{};
        std::size_t own_bytes{};
        std::size_t subtree_nodes{};
        std::size_t subtree_work{};
        std::size_t subtree_bytes{};
        std::shared_ptr<const StyleEnvironment> environment;

        friend bool operator==(const ComputedStyle& a, const ComputedStyle& b) {
            return a.identity == b.identity && a.local_name == b.local_name && a.values == b.values &&
                   a.custom_properties == b.custom_properties &&
                   a.invalid_at_computed_value == b.invalid_at_computed_value && a.children == b.children &&
                   a.specified_properties == b.specified_properties;
        }
    };

    ComputedStyle compute_style(const DomSnapshot& root, std::string_view author_stylesheet,
                                const StyleProfile& profile);

    // Immutable, bounded parsed stylesheet. Each evaluation has independent work
    // counters; results own all values and never retain the input DOM.
    // Optional owner-local bounded value/rule memo. Full evaluation remains
    // available; cached entries carry no DOM/VM references or GPU resources.
    class StyleMemo {
    public:
        StyleMemo();
        ~StyleMemo();
        void clear();
        StyleMemo(const StyleMemo&) = delete;
        StyleMemo& operator=(const StyleMemo&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class CompiledStyle;
    };

    // What a cascade reads from a document: its elements, the attributes some selector or presentation mapping reads,
    // and hover/active where a selector can see them. CompiledStyle::refresh_input keeps it current. Owner value.
    class StyleInput {
    public:
        struct Node {
            NodeIdentity identity;
            int type{};
            std::string local_name;
            std::string namespace_uri;
            AttributeMap attributes;
            bool hovered{};
            bool active{};
            std::vector<Node> children; // Element children, DOM order.
            // The subtree's nodes, which count against the document's node bound.
            std::uint32_t nodes{};
            // The DomSnapshot::revision this node was read from. Bookkeeping, not content: == ignores it.
            std::uint64_t revision{};

            friend bool operator==(const Node& a, const Node& b) {
                return a.identity == b.identity && a.type == b.type && a.local_name == b.local_name &&
                       a.namespace_uri == b.namespace_uri && a.attributes == b.attributes && a.hovered == b.hovered &&
                       a.active == b.active && a.children == b.children && a.nodes == b.nodes;
            }
        };

        // The document as last read; null before the first read and after a failed one.
        const Node* root() const {
            return root_ ? &*root_ : nullptr;
        }

    private:
        std::optional<Node> root_;
        std::shared_ptr<const int> binding_; // The CompiledStyle that read it.
        friend class CompiledStyle;
    };

    class CompiledStyle {
    public:
        CompiledStyle(std::string author_stylesheet, StyleProfile profile);
        ~CompiledStyle();
        CompiledStyle(const CompiledStyle&) = delete;
        CompiledStyle& operator=(const CompiledStyle&) = delete;
        ComputedStyle compute(const DomSnapshot&) const;
        ComputedStyle compute(const DomSnapshot&, StyleMemo&) const;
        // Owner-only in-place derived tree. Unchanged own values avoid copying;
        // quotas and child identity/order checks still apply. Discard on failure.
        // Optional changed_nodes reports changed or reconstructed own output values
        // by generation. Reparenting reconstructs values; sibling reorder alone
        // does not invalidate an unchanged own value.
        // Discard both output and changed_nodes on failure.
        void update(const DomSnapshot&, ComputedStyle&, StyleMemo&,
                    std::vector<NodeIdentity>* changed_nodes = nullptr) const;
        // The same, from `previous`, the document `output` was last updated from (whose update ended without an
        // exception), so the work follows what changed: a subtree whose revision did not move (non-zero, of one node,
        // DomSnapshot::revision) keeps its values without a visit unless its context changed (an ancestor's values,
        // or what a selector reads of an ancestor), and a node only an descendant of which changed keeps its own
        // values. Nodes whose own values change take `stamp` (ComputedStyle::stamp), which must exceed every earlier
        // update's. The bounds are charged as a full update charges them. Null previous: every node is visited.
        void update(const DomSnapshot& dom, const DomSnapshot* previous, ComputedStyle& output, StyleMemo& memo,
                    std::uint64_t stamp, std::vector<NodeIdentity>* changed_nodes = nullptr) const;
        // Makes input describe dom as this cascade reads it and returns whether that changed. A subtree whose
        // revision matches the one input last read is not visited. A document outside the node/depth bounds throws
        // and leaves input as it was; any other failure leaves it empty, so the next read reports a change.
        bool refresh_input(const DomSnapshot& dom, StyleInput& input) const;
        // For a host's full layout-cache key: erase only pseudo state which cannot
        // match a supported selector. Text, scroll, attributes and identities stay.
        void normalize_selector_state(DomSnapshot&) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // :root matches only an HTML document-element snapshot, never a scoped div.
    // Exact pairing checks are required before passing a computed tree to layout.
    void validate_style_pair(const DomSnapshot& dom, const ComputedStyle& style);
    // The document formatter's property registry and UA sheet.
    StyleProfile document_style_profile();
    // The shared color subset is #rrggbb, transparent, and legacy comma rgb/rgba
    // numbers. Components/alpha clamp as CSS requires. No color spaces/named colors.
    std::array<float, 4> parse_css_color(std::string_view value);
} // namespace weft
