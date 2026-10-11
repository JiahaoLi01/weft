#pragma once

#include "base/diagnostic.hpp"
#include "base/result.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The one selector engine of the DOM queries and the style cascade. Selectors are compiled once and matched right to
// left through an element accessor, so the live DOM and owned snapshots share the grammar and its semantics.
namespace weft::dom {
    // What a selector may contain besides type, universal, class, id and attribute selectors, :root, and the
    // descendant and child combinators.
    struct SelectorGrammar {
        // Backslash escapes in identifiers; an escaped code point must be ASCII.
        bool escapes{};
        // :hover and :active.
        bool states{};
        // Diagnostics for text that is not a selector, and for a valid selector outside this grammar.
        Diag syntax{Diag::StyleProfile};
        Diag unsupported{Diag::StyleProfile};
    };

    struct AttributeSelector {
        enum class Operator : std::uint8_t {
            // [name]
            Exists,
            // [name=value]
            Equals,
            // [name~=value]: one of its whitespace-separated words.
            Includes,
            // [name|=value]: value, or value followed by a hyphen.
            DashMatch,
            // [name^=value], [name$=value], [name*=value]: a non-empty value starts, ends or occurs in it.
            Prefix,
            Suffix,
            Substring
        };

        // As written, and its ASCII lower case, which HTML elements compare exactly.
        std::string name;
        std::string lower_name;
        Operator op{};
        std::string value;
    };

    struct Compound {
        // Empty for none, "*" for any. As written, and its ASCII lower case, which HTML elements compare exactly.
        std::string tag;
        std::string lower_tag;
        std::vector<std::string> ids;
        std::vector<std::string> classes;
        std::vector<AttributeSelector> attributes;
        bool root{};
        bool hover{};
        bool active{};
    };

    struct Selector {
        // Compounds left to right; relations[i] joins parts[i] and parts[i + 1]: ' ' descendant, '>' child.
        std::vector<Compound> parts;
        std::vector<char> relations;
        // ids, classes/attributes/pseudo-classes, types.
        std::array<unsigned, 3> specificity{};
    };

    // One complex selector.
    Result<Selector> parse_selector(std::string_view text, const SelectorGrammar& grammar);
    // A comma-separated list of at most max_items selectors; commas inside quotes do not separate.
    Result<std::vector<Selector>> parse_selector_list(std::string_view text, const SelectorGrammar& grammar,
                                                      std::size_t max_items);

    // Whether a class attribute lists name among its ASCII-whitespace-separated classes.
    bool has_class(std::string_view list, std::string_view name);
    bool attribute_matches(const AttributeSelector& selector, std::string_view value);

    // What matching reads from an element. parent() is the parent element, nullopt above the topmost element.
    template <class E>
    concept SelectorElement = requires(const E& e, std::string_view name) {
        {
            e.local_name()
        } -> std::convertible_to<std::string_view>;
        {
            e.html()
        } -> std::convertible_to<bool>;
        {
            e.attribute(name)
        } -> std::convertible_to<const std::string*>;
        {
            e.hovered()
        } -> std::convertible_to<bool>;
        {
            e.active()
        } -> std::convertible_to<bool>;
        {
            e.document_root()
        } -> std::convertible_to<bool>;
        {
            e.parent()
        } -> std::same_as<std::optional<E>>;
    };

    // A compound on one element; state = false ignores :hover and :active.
    template <SelectorElement E>
    bool matches(const Compound& c, const E& e, bool state = true) {
        const bool html = e.html();
        if (c.root && !e.document_root()) {
            return false;
        }
        if (!c.tag.empty() && c.tag != "*" && std::string_view(html ? c.lower_tag : c.tag) != e.local_name()) {
            return false;
        }
        if (state && ((c.hover && !e.hovered()) || (c.active && !e.active()))) {
            return false;
        }
        if (!c.ids.empty()) {
            const std::string* id = e.attribute("id");
            for (const auto& wanted : c.ids) {
                if (!id || *id != wanted) {
                    return false;
                }
            }
        }
        if (!c.classes.empty()) {
            const std::string* list = e.attribute("class");
            for (const auto& wanted : c.classes) {
                if (!list || !has_class(*list, wanted)) {
                    return false;
                }
            }
        }
        for (const auto& a : c.attributes) {
            const std::string* value = e.attribute(html ? a.lower_name : a.name);
            if (!value || !attribute_matches(a, *value)) {
                return false;
            }
        }
        return true;
    }

    namespace detail {
        template <SelectorElement E>
        bool matches_from(const Selector& s, std::size_t k, const E& e, bool state) {
            if (!matches(s.parts[k], e, state)) {
                return false;
            }
            if (k == 0) {
                return true;
            }
            auto parent = e.parent();
            if (s.relations[k - 1] == '>') {
                return parent && matches_from(s, k - 1, *parent, state);
            }
            for (; parent; parent = parent->parent()) {
                if (matches_from(s, k - 1, *parent, state)) {
                    return true;
                }
            }
            return false;
        }
    } // namespace detail

    template <SelectorElement E>
    bool matches(const Selector& s, const E& e, bool state = true) {
        return !s.parts.empty() && detail::matches_from(s, s.parts.size() - 1, e, state);
    }

    template <SelectorElement E>
    bool matches(const std::vector<Selector>& list, const E& e, bool state = true) {
        for (const auto& s : list) {
            if (matches(s, e, state)) {
                return true;
            }
        }
        return false;
    }
} // namespace weft::dom
