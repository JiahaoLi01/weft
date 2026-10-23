#pragma once

#include "constants/common.hpp"
#include "dom/selector.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// HTML's focus rules on an element's name and attributes (HTML §6.6.3 "focusable areas", with Blink's choices), shared
// by the script DOM and the View's scene. attribute(name) gives an attribute's value, or nullptr when it is absent; html
// says whether the element is an HTML one.
namespace weft::dom {
    inline std::string lower_ascii_copy(std::string_view text) {
        std::string out(text);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = char(c - 'A' + 'a');
            }
        }
        return out;
    }

    // An attribute value parsed by HTML's rules for parsing integers (tabindex); none for one that is no integer or
    // past a 32-bit int.
    inline std::optional<std::int32_t> html_integer(std::string_view text) {
        std::size_t i{};
        while (i < text.size() &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\f' || text[i] == '\r')) {
            ++i;
        }
        bool negative{};
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            negative = text[i++] == '-';
        }
        if (i == text.size() || text[i] < '0' || text[i] > '9') {
            return std::nullopt;
        }
        std::int64_t value{};
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            value = std::min<std::int64_t>(value * 10 + (text[i] - '0'), std::int64_t(1) << 32);
        }
        value = negative ? -value : value;
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return std::int32_t(value);
    }

    // An editable element: a text field, a textarea or an editing host. It shows its focus however it took it (Blink's
    // MayTriggerVirtualKeyboard).
    template <class Attribute>
    bool editable_element(std::string_view local, bool html, Attribute&& attribute) {
        if (html && local == "textarea") {
            return true;
        }
        if (html && local == "input") {
            const std::string* type = attribute("type");
            const auto t = type ? lower_ascii_copy(*type) : std::string();
            return t.empty() || t == "text" || t == "search" || t == "email" || t == "url" || t == "tel" ||
                   t == "password" || t == "number";
        }
        const std::string* editable = attribute("contenteditable");
        if (!editable) {
            return false;
        }
        const auto value = lower_ascii_copy(*editable);
        return value.empty() || value == "true" || value == "plaintext-only";
    }

    // An element that takes the focus without a tabindex: a link, a form control (not a hidden input), an iframe, an
    // editing host.
    template <class Attribute>
    bool focusable_by_default(std::string_view local, bool html, Attribute&& attribute) {
        if (html) {
            if ((local == "a" || local == "area") && attribute("href")) {
                return true;
            }
            if (local == "button" || local == "select" || local == "textarea" || local == "iframe") {
                return true;
            }
            if (local == "input") {
                const std::string* type = attribute("type");
                return !type || lower_ascii_copy(*type) != "hidden";
            }
        }
        return editable_element(local, html, attribute);
    }

    // A form control its disabled attribute disables.
    template <class Attribute>
    bool disabled_control(std::string_view local, bool html, Attribute&& attribute) {
        return html && detail::form_control(local) && attribute("disabled");
    }

    // With spatial navigation an element meant to respond to the user takes the focus too (Blink's
    // Element::SupportsSpatialNavigationFocus): one with a script listener for these events, or an SVG element with one
    // for its focus events; or one that sets cursor: pointer itself (the top of a tree of them, which the host's styles
    // tell).
    inline constexpr std::array<std::string_view, 6> spatial_navigation_events{"click",   "keydown",   "keypress",
                                                                               "keyup",   "mouseover", "mouseenter"};
    inline constexpr std::array<std::string_view, 4> svg_focus_events{"focus", "blur", "focusin", "focusout"};
} // namespace weft::dom
