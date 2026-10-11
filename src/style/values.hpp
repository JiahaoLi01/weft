#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Typed computed style values. A value keeps its exact text, which getComputedStyle, diagnostics and string-keyed
// caches see, and a payload parsed once from that text for the grammars consumers read: keywords, numbers with units,
// calc() sums and colors. Any other text is a raw value. The payload is a function of the text, so equal texts are
// equal values.
namespace weft {
    enum class PropertyId : std::uint8_t {
#define WEFT_PROPERTY(id, name) id,
#include "style/properties.inc"
#undef WEFT_PROPERTY
    };

    inline constexpr std::size_t property_count = 0
#define WEFT_PROPERTY(id, name) +1
#include "style/properties.inc"
#undef WEFT_PROPERTY
        ;

    std::string_view property_name(PropertyId);
    // nullopt for a name outside the registry.
    std::optional<PropertyId> find_property(std::string_view name);

    enum class Keyword : std::uint8_t {
#define WEFT_KEYWORD(id, text) id,
#include "style/keywords.inc"
#undef WEFT_KEYWORD
    };

    std::string_view keyword_name(Keyword);
    // Case-sensitive, like the string comparisons it replaces; "currentColor" is also Currentcolor.
    std::optional<Keyword> find_keyword(std::string_view text);

    enum class Unit : std::uint8_t {
        None,
        Px,
        Percent,
        Em,
        Deg,
        Rad,
        Turn,
        S,
        Ms,
        Fr
    };

    struct Dimension {
        double value{};
        Unit unit{};
        friend bool operator==(const Dimension&, const Dimension&) = default;
    };

    // calc(a + b) or calc(a - b), the only calc() form the style profiles accept.
    struct Calc {
        Dimension a;
        Dimension b;
        bool subtract{};
        friend bool operator==(const Calc&, const Calc&) = default;
    };

    // A color as parse_css_color returns it (straight encoded sRGB, clamped). The form records which spelling it
    // came from: consumers that reject a spelling (short hex) keep doing so.
    struct Rgba {
        enum class Form : std::uint8_t {
            Hex,
            ShortHex,
            Rgb,
            Rgba,
            Transparent,
            Black,
            White
        };

        std::array<float, 4> channels{};
        Form form{};
        friend bool operator==(const Rgba&, const Rgba&) = default;
    };

    class CssValue {
    public:
        enum class Kind : std::uint8_t {
            Raw,
            Keyword,
            Dimension,
            Calc,
            Color
        };

        CssValue() = default;
        explicit CssValue(std::string text);

        const std::string& text() const {
            return text_;
        }

        Kind kind() const {
            return Kind(payload_.index());
        }

        std::optional<Keyword> keyword() const;
        bool is(Keyword) const;
        const Dimension* dimension() const;
        const Calc* calc() const;
        const Rgba* color() const;

        friend bool operator==(const CssValue& a, const CssValue& b) {
            return a.text_ == b.text_;
        }

    private:
        std::string text_;
        // Alternatives in Kind order.
        std::variant<std::monostate, Keyword, Dimension, Calc, Rgba> payload_;
    };

    // Property values of one element, in PropertyId (that is, name) order.
    class StyleValues {
    public:
        using Entry = std::pair<PropertyId, CssValue>;
        using const_iterator = std::vector<Entry>::const_iterator;

        // Write access by name, for code written against string maps: style["opacity"] = "0.5". Reading through a
        // slot gives the text, or an empty string when the property is absent, and never inserts.
        class Slot {
        public:
            Slot(StyleValues& values, PropertyId id) : values_(values), id_(id) {
            }

            Slot& operator=(std::string text) {
                values_.set(id_, std::move(text));
                return *this;
            }

            Slot& operator=(CssValue value) {
                values_.set(id_, std::move(value));
                return *this;
            }

            operator std::string() const {
                return std::string(values_.text(id_));
            }

            friend bool operator==(const Slot& slot, std::string_view text) {
                return slot.values_.text(slot.id_) == text;
            }

        private:
            StyleValues& values_;
            PropertyId id_;
        };

        StyleValues() = default;
        // Name and text pairs, like a string map literal: {{"display", "flex"}, {"gap", "4px"}}.
        StyleValues(std::initializer_list<std::pair<std::string_view, std::string_view>> entries);

        const CssValue* find(PropertyId) const;
        // nullptr also for a name outside the registry.
        const CssValue* find(std::string_view name) const;

        bool contains(PropertyId id) const {
            return find(id) != nullptr;
        }

        bool contains(std::string_view name) const {
            return find(name) != nullptr;
        }

        // The value's text, or fallback when the property is absent. Valid until this object changes.
        std::string_view text(PropertyId, std::string_view fallback = {}) const;
        std::string_view text(std::string_view name, std::string_view fallback = {}) const;
        // The text of a present property; std::out_of_range when absent, like std::map::at.
        const std::string& at(PropertyId) const;
        const std::string& at(std::string_view name) const;
        // A registered name; a name outside the registry is a diagnostic (Diag::StyleProfile).
        Slot operator[](std::string_view name);

        Slot operator[](PropertyId id) {
            return {*this, id};
        }

        void set(PropertyId, CssValue);

        void set(PropertyId id, std::string text) {
            set(id, CssValue(std::move(text)));
        }

        // A registered name; a name outside the registry is a diagnostic (Diag::StyleProfile).
        void set(std::string_view name, std::string text);
        bool erase(PropertyId);
        bool erase(std::string_view name);

        void clear() {
            entries_.clear();
        }

        std::size_t size() const {
            return entries_.size();
        }

        bool empty() const {
            return entries_.empty();
        }

        const_iterator begin() const {
            return entries_.begin();
        }

        const_iterator end() const {
            return entries_.end();
        }

        friend bool operator==(const StyleValues&, const StyleValues&) = default;

    private:
        std::vector<Entry> entries_;
    };

    namespace css {
        // The text of a property, or fallback, as the string-map overload in base/css_scalar.hpp.
        std::string style_value(const StyleValues& style, std::string_view property, const char* fallback = "");
    } // namespace css
} // namespace weft
