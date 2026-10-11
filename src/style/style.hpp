#pragma once
#include "script/script_dom.hpp"
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
        bool single_value_inset{};
        bool four_value_edges{}; // Expand margin/padding/border-width before cascade.
        bool flow_shorthands{};  // Corner radii and overflow axes, before cascade.
        bool scene_syntax{};     // Explicit scene profile: escaped selectors, SVG, raw shorthands.
    };

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
        DomSnapshot input_key(const DomSnapshot&) const;
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
