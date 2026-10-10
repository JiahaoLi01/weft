#include "style/values.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>

namespace weft {
    namespace {
        constexpr std::array<std::string_view, property_count> property_names{
#define WEFT_PROPERTY(id, name) name,
#include "style/properties.inc"
#undef WEFT_PROPERTY
        };

        constexpr std::size_t keyword_count = 0
#define WEFT_KEYWORD(id, text) +1
#include "style/keywords.inc"
#undef WEFT_KEYWORD
            ;

        constexpr std::array<std::string_view, keyword_count> keyword_names{
#define WEFT_KEYWORD(id, text) text,
#include "style/keywords.inc"
#undef WEFT_KEYWORD
        };

        template <std::size_t N>
        std::optional<std::size_t> lookup(const std::array<std::string_view, N>& names, std::string_view text) {
            const auto it = std::lower_bound(names.begin(), names.end(), text);
            if (it == names.end() || *it != text) {
                return {};
            }
            return std::size_t(it - names.begin());
        }

        // Whether text is entirely a floating literal of the strtod grammar: optional sign, then decimal digits with
        // an optional point and e exponent, or 0x hexadecimal digits with an optional point and p exponent. Exactly
        // these texts are consumed whole by parse_number (inf and nan are rejected there as non-finite), so other
        // texts are refused without the exception std::stod throws for them.
        bool float_syntax(std::string_view s) {
            std::size_t i{};
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                ++i;
            }
            const auto rest = s.substr(i);
            const bool hex = rest.starts_with("0x") || rest.starts_with("0X");
            if (hex) {
                i += std::string_view("0x").size();
            }
            const auto mantissa = [&](char c) {
                return hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0 : c >= '0' && c <= '9';
            };
            std::size_t digits{};
            for (; i < s.size() && mantissa(s[i]); ++i) {
                ++digits;
            }
            if (i < s.size() && s[i] == '.') {
                for (++i; i < s.size() && mantissa(s[i]); ++i) {
                    ++digits;
                }
            }
            if (!digits) {
                return false;
            }
            if (i < s.size() && (hex ? s[i] == 'p' || s[i] == 'P' : s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                    ++i;
                }
                std::size_t exponent{};
                for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
                    ++exponent;
                }
                if (!exponent) {
                    return false;
                }
            }
            return i == s.size();
        }

        // A number with one of the supported unit suffixes, parsed as the scalar helpers parse it (the whole digit
        // text must be a finite number). Surrounding whitespace is not a dimension.
        std::optional<Dimension> dimension_of(std::string_view s) {
            static constexpr std::pair<std::string_view, Unit> units[]{
                {"px", Unit::Px},     {"%", Unit::Percent}, {"em", Unit::Em}, {"deg", Unit::Deg}, {"rad", Unit::Rad},
                {"turn", Unit::Turn}, {"ms", Unit::Ms},     {"s", Unit::S},   {"fr", Unit::Fr}};
            if (s.empty() || std::isspace(static_cast<unsigned char>(s.front())) ||
                std::isspace(static_cast<unsigned char>(s.back()))) {
                return {};
            }
            auto unit = Unit::None;
            auto digits = s;
            for (const auto& [suffix, value] : units) {
                if (s.size() > suffix.size() && s.ends_with(suffix)) {
                    unit = value;
                    digits = s.substr(0, s.size() - suffix.size());
                    break;
                }
            }
            if (digits.empty() || std::isspace(static_cast<unsigned char>(digits.back())) || !float_syntax(digits)) {
                return {};
            }
            const auto n = css::parse_number<double>(std::string(digits));
            if (!n) {
                return {};
            }
            return Dimension{*n, unit};
        }

        // calc(<dimension> + <dimension>) or calc(<dimension> - <dimension>) as the cascade serializes it.
        std::optional<Calc> calc_of(std::string_view s) {
            if (!s.starts_with("calc(") || !s.ends_with(')')) {
                return {};
            }
            const auto inner = s.substr(5, s.size() - 6);
            const auto plus = inner.find(" + ");
            const auto minus = inner.find(" - ");
            if ((plus == inner.npos) == (minus == inner.npos)) {
                return {};
            }
            const bool subtract = minus != inner.npos;
            const auto at = subtract ? minus : plus;
            const auto a = dimension_of(inner.substr(0, at));
            const auto b = dimension_of(inner.substr(at + 3));
            if (!a || !b) {
                return {};
            }
            return Calc{*a, *b, subtract};
        }

        // The spellings parse_css_color accepts directly or after expand_color_shorthand.
        std::optional<Rgba> color_of(std::string_view s) {
            // Each accepted spelling starts with one of these; most raw values stop here without a copy.
            if (std::string_view("#bwtTrR").find(s.front()) == std::string_view::npos) {
                return {};
            }
            auto lower = std::string(s);
            for (auto& c : lower) {
                c = char(std::tolower(static_cast<unsigned char>(c)));
            }
            Rgba out;
            std::string input(s);
            if (s == "black" || s == "white") {
                out.form = s == "black" ? Rgba::Form::Black : Rgba::Form::White;
                input = css::expand_color_shorthand(input);
            } else if (s.size() == 4 && s.front() == '#') {
                out.form = Rgba::Form::ShortHex;
                input = css::expand_color_shorthand(input);
            } else if (s.size() == 7 && s.front() == '#') {
                out.form = Rgba::Form::Hex;
            } else if (lower == "transparent") {
                out.form = Rgba::Form::Transparent;
            } else if (lower.starts_with("rgba(")) {
                out.form = Rgba::Form::Rgba;
            } else if (lower.starts_with("rgb(")) {
                out.form = Rgba::Form::Rgb;
            } else {
                return {};
            }
            try {
                out.channels = parse_css_color(input);
            } catch (const std::exception&) {
                return {};
            }
            return out;
        }

        [[noreturn]] void unregistered(std::string_view name) {
            throw_diagnostic(Diag::StyleProfile, "unregistered property " + std::string(name));
        }
    } // namespace

    std::string_view property_name(PropertyId id) {
        return property_names.at(std::size_t(id));
    }

    std::optional<PropertyId> find_property(std::string_view name) {
        if (const auto i = lookup(property_names, name)) {
            return PropertyId(*i);
        }
        return {};
    }

    std::string_view keyword_name(Keyword k) {
        return keyword_names.at(std::size_t(k));
    }

    std::optional<Keyword> find_keyword(std::string_view text) {
        if (text == "currentColor") {
            return Keyword::Currentcolor;
        }
        if (const auto i = lookup(keyword_names, text)) {
            return Keyword(*i);
        }
        return {};
    }

    CssValue::CssValue(std::string text) : text_(std::move(text)) {
        const std::string_view s = text_;
        if (s.empty()) {
            return;
        }
        if (const auto keyword = find_keyword(s)) {
            payload_ = *keyword;
        } else if (const auto color = color_of(s)) {
            payload_ = *color;
        } else if (const auto sum = calc_of(s)) {
            payload_ = *sum;
        } else if (const auto number = dimension_of(s)) {
            payload_ = *number;
        }
    }

    std::optional<Keyword> CssValue::keyword() const {
        if (const auto* k = std::get_if<Keyword>(&payload_)) {
            return *k;
        }
        return {};
    }

    bool CssValue::is(Keyword k) const {
        const auto* value = std::get_if<Keyword>(&payload_);
        return value && *value == k;
    }

    const Dimension* CssValue::dimension() const {
        return std::get_if<Dimension>(&payload_);
    }

    const Calc* CssValue::calc() const {
        return std::get_if<Calc>(&payload_);
    }

    const Rgba* CssValue::color() const {
        return std::get_if<Rgba>(&payload_);
    }

    StyleValues::StyleValues(std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
        for (const auto& [name, text] : entries) {
            set(name, std::string(text));
        }
    }

    const std::string& StyleValues::at(PropertyId id) const {
        const auto* value = find(id);
        if (!value) {
            throw std::out_of_range("StyleValues::at " + std::string(property_name(id)));
        }
        return value->text();
    }

    const std::string& StyleValues::at(std::string_view name) const {
        const auto* value = find(name);
        if (!value) {
            throw std::out_of_range("StyleValues::at " + std::string(name));
        }
        return value->text();
    }

    StyleValues::Slot StyleValues::operator[](std::string_view name) {
        const auto id = find_property(name);
        if (!id) {
            unregistered(name);
        }
        return {*this, *id};
    }

    const CssValue* StyleValues::find(PropertyId id) const {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, PropertyId value) {
            return e.first < value;
        });
        return it != entries_.end() && it->first == id ? &it->second : nullptr;
    }

    const CssValue* StyleValues::find(std::string_view name) const {
        const auto id = find_property(name);
        return id ? find(*id) : nullptr;
    }

    std::string_view StyleValues::text(PropertyId id, std::string_view fallback) const {
        const auto* value = find(id);
        return value ? std::string_view(value->text()) : fallback;
    }

    std::string_view StyleValues::text(std::string_view name, std::string_view fallback) const {
        const auto* value = find(name);
        return value ? std::string_view(value->text()) : fallback;
    }

    void StyleValues::set(PropertyId id, CssValue value) {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, PropertyId v) {
            return e.first < v;
        });
        if (it != entries_.end() && it->first == id) {
            it->second = std::move(value);
        } else {
            entries_.insert(it, {id, std::move(value)});
        }
    }

    void StyleValues::set(std::string_view name, std::string text) {
        const auto id = find_property(name);
        if (!id) {
            unregistered(name);
        }
        set(*id, CssValue(std::move(text)));
    }

    bool StyleValues::erase(PropertyId id) {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, PropertyId v) {
            return e.first < v;
        });
        if (it == entries_.end() || it->first != id) {
            return false;
        }
        entries_.erase(it);
        return true;
    }

    bool StyleValues::erase(std::string_view name) {
        const auto id = find_property(name);
        return id && erase(*id);
    }

    std::map<std::string, std::string> string_map(const StyleValues& values) {
        std::map<std::string, std::string> out;
        for (const auto& [id, value] : values) {
            out.emplace_hint(out.end(), std::string(property_name(id)), value.text());
        }
        return out;
    }

    namespace css {
        std::string style_value(const StyleValues& style, std::string_view property, const char* fallback) {
            const auto* value = style.find(property);
            return value ? value->text() : std::string(fallback);
        }
    } // namespace css
} // namespace weft
