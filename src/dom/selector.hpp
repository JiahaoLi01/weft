#pragma once

#include "base/diagnostic.hpp"
#include "base/result.hpp"
#include "dom/text_field.hpp"

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
        // The selectors of Selectors 4 a weft document can match, for pages that take CSS as browsers do
        // (StyleProfile::forgiving): the sibling combinators + and ~; :is(), :where() and :not(); the tree-structural
        // pseudo-classes (:first-child, :nth-child(An+B [of S]) and their -of-type and last forms, :only-child, :empty);
        // :focus, :focus-visible and :focus-within; :disabled, :enabled and :checked by attribute; :read-only,
        // :read-write and :placeholder-shown by text field (dom/text_field.hpp); :scope and :defined; the
        // pseudo-elements ::before and ::after (and their CSS 2 one-colon spellings), and a text field's ::placeholder
        // and ::selection. Pseudo-classes and -elements of states and parts a weft document does not have (:visited,
        // ::backdrop, vendor-prefixed ones…) are valid and match nothing, so a selector list that names one keeps its
        // other selectors, as in a browser.
        bool modern{};
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
        // [name=value i]: the value compares ASCII case-insensitively (the modern grammar).
        bool ignore_case{};
    };

    // The pseudo-element a selector's subject is (the modern grammar): none, ::before, ::after, a text field's
    // ::placeholder or ::selection, or one weft documents do not have (::backdrop…), which matches nothing.
    enum class PseudoElement : std::uint8_t {
        None,
        Before,
        After,
        Placeholder,
        Selection,
        Other
    };

    struct Selector;

    // :is(), :where() or :not() with its selector list (the modern grammar). :is() and :where() take a forgiving list:
    // an item that is not a selector of the grammar is left out.
    struct SelectorAlternatives {
        enum class Kind : std::uint8_t {
            Is,
            Where,
            Not
        };

        Kind kind{};
        std::vector<Selector> list;
    };

    // A tree-structural pseudo-class: the element is the An+B-th (counting from 1) of its parent's element children,
    // from the first or the last, of every type or of its own; `of` restricts the count to the children that match it
    // (:nth-child(An+B of S)).
    struct NthSelector {
        int a{};
        int b{};
        bool from_end{};
        bool of_type{};
        std::vector<Selector> of;
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
        // The modern grammar's pseudo-classes.
        bool focus{};
        bool focus_visible{};
        bool focus_within{};
        bool disabled{};
        bool enabled{};
        bool checked{};
        bool empty{};
        // :read-only, :read-write and :placeholder-shown.
        bool read_only{};
        bool read_write{};
        bool placeholder_shown{};
        // A pseudo-class or -element no element of a weft document has: the compound matches nothing.
        bool never{};
        std::vector<NthSelector> nth;
        std::vector<SelectorAlternatives> alternatives;
        // The pseudo-element of the selector's subject; only its last compound has one.
        PseudoElement pseudo{};
    };

    struct Selector {
        // Compounds left to right; relations[i] joins parts[i] and parts[i + 1]: ' ' descendant, '>' child, '+' next
        // sibling and '~' subsequent sibling (the modern grammar).
        std::vector<Compound> parts;
        std::vector<char> relations;
        // ids, classes/attributes/pseudo-classes, types.
        std::array<unsigned, 3> specificity{};

        // The pseudo-element of its subject: none, or the one its last compound names.
        PseudoElement pseudo() const {
            return parts.empty() ? PseudoElement::None : parts.back().pseudo;
        }
    };

    // One complex selector.
    Result<Selector> parse_selector(std::string_view text, const SelectorGrammar& grammar);
    // A comma-separated list of at most max_items selectors; commas inside quotes and parentheses do not separate.
    Result<std::vector<Selector>> parse_selector_list(std::string_view text, const SelectorGrammar& grammar,
                                                      std::size_t max_items);

    // Whether a class attribute lists name among its ASCII-whitespace-separated classes.
    bool has_class(std::string_view list, std::string_view name);
    bool attribute_matches(const AttributeSelector& selector, std::string_view value);
    // Whether a 1-based position matches An+B: some n >= 0 gives a*n + b == position.
    bool nth_matches(int a, int b, std::size_t position);

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

    // What the modern grammar reads besides: the element siblings before and after an element (nullopt at the ends,
    // or where the accessor cannot see them), whether it has children that are content (elements or text), and focus.
    template <class E>
    concept StructuralElement = SelectorElement<E> && requires(const E& e) {
        {
            e.previous()
        } -> std::same_as<std::optional<E>>;
        {
            e.next()
        } -> std::same_as<std::optional<E>>;
        {
            e.has_content()
        } -> std::convertible_to<bool>;
        {
            e.focused()
        } -> std::convertible_to<bool>;
        {
            e.focus_visible()
        } -> std::convertible_to<bool>;
        {
            e.focus_within()
        } -> std::convertible_to<bool>;
    };

    // What :read-write and :placeholder-shown read: the element's text field state (dom/text_field.hpp), null for
    // another element. An accessor without it has no text fields.
    template <class E>
    concept FieldElement = requires(const E& e) {
        {
            e.field()
        } -> std::convertible_to<const TextFieldState*>;
    };

    namespace detail {
        // The form controls :disabled, :enabled and :checked apply to.
        inline bool form_control(std::string_view name) {
            return name == "button" || name == "input" || name == "select" || name == "textarea" ||
                   name == "optgroup" || name == "option" || name == "fieldset";
        }

        template <SelectorElement E>
        bool matches_from(const Selector& s, std::size_t k, const E& e, bool state);

        template <SelectorElement E>
        bool list_matches(const std::vector<Selector>& list, const E& e, bool state) {
            for (const auto& s : list) {
                if (s.pseudo() == PseudoElement::None && matches_from(s, s.parts.size() - 1, e, state)) {
                    return true;
                }
            }
            return false;
        }

        // The element's 1-based position among its parent's element children (of its own type, when of_type), from
        // the first or the last, counting only those that match `of` when it is given.
        template <SelectorElement E>
        std::size_t position(const E& e, const NthSelector& n, bool state) {
            std::size_t count = 1;
            const auto counts = [&](const E& sibling) {
                if (n.of_type && (sibling.local_name() != e.local_name() || sibling.html() != e.html())) {
                    return false;
                }
                return n.of.empty() || list_matches(n.of, sibling, state);
            };
            if constexpr (StructuralElement<E>) {
                for (auto s = n.from_end ? e.next() : e.previous(); s; s = n.from_end ? s->next() : s->previous()) {
                    if (counts(*s)) {
                        ++count;
                    }
                }
            }
            return count;
        }
    } // namespace detail

    // A compound on one element; state = false ignores :hover, :active and the focus states.
    template <SelectorElement E>
    bool matches(const Compound& c, const E& e, bool state = true) {
        if (c.never) {
            return false;
        }
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
        if (c.disabled || c.enabled || c.checked) {
            const auto name = e.local_name();
            const bool control = html && detail::form_control(name);
            const bool disabled = control && e.attribute("disabled") != nullptr;
            if ((c.disabled && !disabled) || (c.enabled && (!control || disabled))) {
                return false;
            }
            if (c.checked && !(html && (name == "input" || name == "option") &&
                               e.attribute(name == "option" ? "selected" : "checked"))) {
                return false;
            }
        }
        if (c.read_only || c.read_write || c.placeholder_shown) {
            // A text field the user may edit is read-write (HTML's mutable text controls); every other element is
            // read-only. Its placeholder shows while its value (with any composition) is empty.
            const TextFieldState* field{};
            if constexpr (FieldElement<E>) {
                field = e.field();
            }
            const bool writable = field && field->editable;
            if ((c.read_only && writable) || (c.read_write && !writable) ||
                (c.placeholder_shown && !(field && field->value.empty() && !field->placeholder.empty()))) {
                return false;
            }
        }
        if constexpr (StructuralElement<E>) {
            if (state && ((c.focus && !e.focused()) || (c.focus_visible && !e.focus_visible()) ||
                          (c.focus_within && !e.focus_within()))) {
                return false;
            }
            if (c.empty && e.has_content()) {
                return false;
            }
        } else if (c.focus || c.focus_visible || c.focus_within || c.empty) {
            return false;
        }
        for (const auto& n : c.nth) {
            if (!nth_matches(n.a, n.b, detail::position(e, n, state)) ||
                (!n.of.empty() && !detail::list_matches(n.of, e, state))) {
                return false;
            }
        }
        for (const auto& alternative : c.alternatives) {
            const bool any = detail::list_matches(alternative.list, e, state);
            if (any == (alternative.kind == SelectorAlternatives::Kind::Not)) {
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
            const char relation = s.relations[k - 1];
            if (relation == '+' || relation == '~') {
                if constexpr (StructuralElement<E>) {
                    for (auto sibling = e.previous(); sibling; sibling = sibling->previous()) {
                        if (matches_from(s, k - 1, *sibling, state)) {
                            return true;
                        }
                        if (relation == '+') {
                            return false;
                        }
                    }
                }
                return false;
            }
            auto parent = e.parent();
            if (relation == '>') {
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

    // Whether the selector matches the element. A selector whose subject is a pseudo-element matches no element.
    template <SelectorElement E>
    bool matches(const Selector& s, const E& e, bool state = true) {
        return !s.parts.empty() && s.pseudo() == PseudoElement::None &&
               detail::matches_from(s, s.parts.size() - 1, e, state);
    }

    // Whether the selector's pseudo-element (none for an element selector) belongs to the element: the element is its
    // originating element. A text field's own pseudo-elements belong to text fields alone.
    template <SelectorElement E>
    bool matches_originating(const Selector& s, const E& e, bool state = true) {
        if (s.parts.empty() || s.pseudo() == PseudoElement::Other) {
            return false;
        }
        if (s.pseudo() == PseudoElement::Placeholder || s.pseudo() == PseudoElement::Selection) {
            if constexpr (FieldElement<E>) {
                if (!e.field()) {
                    return false;
                }
            } else {
                return false;
            }
        }
        return detail::matches_from(s, s.parts.size() - 1, e, state);
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
