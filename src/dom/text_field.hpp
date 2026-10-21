#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Text fields: an HTML input of a text type and a textarea (HTML §4.10.5, §4.10.11). What one shows and where its caret,
// selection and composition are, as the script DOM keeps it and a snapshot carries it to style and scenes; offsets
// helpers between the DOM's UTF-16 and the value's UTF-8.
namespace weft {
    struct TextFieldState {
        // The value (UTF-8; a textarea's line breaks are \n) and the placeholder attribute (line breaks taken out).
        std::string value;
        std::string placeholder;
        // A textarea; an input of type password, whose characters show as bullets.
        bool multiline{};
        bool password{};
        // Neither readonly nor disabled: the user edits it.
        bool editable{true};
        // The selection in value, in UTF-8 bytes (start <= end; collapsed: the caret); backward when its focus (where
        // the caret moves) is at the start.
        std::uint32_t selection_start{};
        std::uint32_t selection_end{};
        bool backward{};
        // An input method's composition in value (composing, from start to end; it may be empty), and the clause it is
        // converting in it, drawn with a thick underline (empty: none).
        bool composing{};
        std::uint32_t composition_start{};
        std::uint32_t composition_end{};
        std::uint32_t clause_start{};
        std::uint32_t clause_end{};
        // The caret's blink is in its on phase.
        bool caret_on{true};
        // How far the field's text is scrolled (an input's to the left, a textarea's up) to show the caret.
        float scroll_x{};
        float scroll_y{};

        friend bool operator==(const TextFieldState&, const TextFieldState&) = default;
    };

    // Whether an element is a text field: an HTML textarea, or an HTML input whose type attribute (null: none) is a
    // text type or one weft takes as text (text, search, password, email, url, tel, number and an unknown type; not
    // the buttons, checkboxes, radios, ranges, colors, files, dates and hidden).
    bool text_field_element(std::string_view local, bool html, const std::string* type);
    // An input's type keyword, lower case, "text" for a missing or unknown one.
    std::string text_field_type(const std::string* type);

    // The UTF-16 length of UTF-8 text, and offsets between the two (an offset inside a character goes to its end).
    std::size_t utf16_length(std::string_view utf8);
    std::size_t utf16_offset(std::string_view utf8, std::size_t byte);
    std::size_t byte_offset(std::string_view utf8, std::size_t utf16);
} // namespace weft
