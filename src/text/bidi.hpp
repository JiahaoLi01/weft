#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Text direction from SheenBidi (third_party/sheenbidi): the Unicode bidirectional algorithm (UAX #9) over UTF-8 and
// script runs (UAX #24) for shaping. No Unicode data of weft's own; the one rule written here is UAX #9 L2 over a
// line's run levels (bidi_visual_order), which Blink also applies to its line items rather than to characters.
namespace weft {
    // A paragraph's base direction: left-to-right, right-to-left, or its first strong character's (UAX #9 P2–P3,
    // left-to-right without one), as unicode-bidi: plaintext asks.
    enum class BidiBase : std::uint8_t {
        Ltr,
        Rtl,
        Auto,
    };

    struct BidiParagraphs {
        // The resolved embedding level of each byte (X1–I2; a character's bytes share its level), as ICU's
        // ubidi_setPara leaves them for Blink: whitespace, isolate controls, boundary neutrals and explicit
        // embeddings that end the text or precede a segment or paragraph separator at the paragraph's level (with
        // the separator), the other explicit embeddings and boundary neutrals at the level of the character after
        // them.
        std::vector<std::uint8_t> levels;
        // Each paragraph (P1: text up to and including a paragraph separator): its first byte and base level.
        std::vector<std::pair<std::size_t, std::uint8_t>> paragraphs;
    };

    // Whether text at a left-to-right base resolves to level 0 throughout: it has no right-to-left or Arabic letter,
    // Arabic number, nor explicit embedding, override or isolate. Then the algorithm can be skipped.
    bool bidi_left_to_right(std::string_view utf8);

    // The direction of text's first strong character (bidi class L, R or AL; true: right to left), as HTML's auto
    // directionality and UAX #9 P2 find it; none without one.
    std::optional<bool> bidi_first_strong_rtl(std::string_view utf8);

    // The levels of UTF-8 text, each paragraph at base.
    BidiParagraphs bidi_levels(std::string_view utf8, BidiBase base);

    // UAX #9 L2 over the levels of a line's runs in logical order: from the highest level to the lowest odd one, each
    // maximal sequence of runs at that level or higher reversed. The runs' indices in visual order, left to right.
    std::vector<std::size_t> bidi_visual_order(std::span<const std::uint8_t> levels);

    // A run of one script (UAX #24 with SheenBidi's handling of Common and Inherited characters, which take the script
    // around them and pair brackets): bytes [begin, end) and the script's ISO 15924 tag ('Arab', 'Latn', 'Zyyy' for
    // Common, ...), as HarfBuzz takes it (hb_script_from_iso15924_tag).
    struct ScriptRun {
        std::size_t begin{};
        std::size_t end{};
        std::uint32_t script{};
    };

    std::vector<ScriptRun> script_runs(std::string_view utf8);
} // namespace weft
