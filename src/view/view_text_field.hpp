#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

// Moving and selecting in a text field's value (UTF-8, dom/text_field.hpp) as Chrome on Windows does: by character (a
// code point), by word (Windows' word moves and double-click words, Blink's EditingBehavior), and by a textarea's line.
// A word is a run of letters and digits, or of CJK ideographs, kana and their marks (one word, where Chrome splits
// such a run by dictionary); a run of other non-space characters is one too, and spaces separate them.
namespace weft {
    // The character boundary before and after offset (offset itself at either end of text).
    std::uint32_t previous_character(std::string_view text, std::uint32_t offset);
    std::uint32_t next_character(std::string_view text, std::uint32_t offset);
    // Ctrl+Left: the start of the word before offset, past the spaces before it. Ctrl+Right on Windows: the start of the
    // next word, past the rest of the word at offset and the spaces after it (the end of text after the last word).
    std::uint32_t previous_word(std::string_view text, std::uint32_t offset);
    std::uint32_t next_word(std::string_view text, std::uint32_t offset);
    // What a double click at offset selects: the run at the character after it (before it at the end of text), a word
    // with the spaces that follow it (Windows' smart selection of trailing whitespace).
    std::pair<std::uint32_t, std::uint32_t> word_at(std::string_view text, std::uint32_t offset);
    // The line of a textarea's text at offset, between line feeds: its start, and its end before its line feed.
    std::pair<std::uint32_t, std::uint32_t> line_at(std::string_view text, std::uint32_t offset);
} // namespace weft
