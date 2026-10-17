#pragma once
#include "text/font.hpp"
#include "constants/common.hpp"
#include <array>
#include <string>
#include <vector>

namespace weft {
    // Owning, owner-thread inputs. Ancestors identify inline element fragments.
    struct InlineRun {
        std::string text;
        std::vector<std::string> ancestors;
        float font_size{constants::default_font_size_px};
        float line_height{constants::default_line_height_px};
        std::array<float, 4> color{0, 0, 0, 1};
        std::string white_space{"normal"};
        bool hard_break{};
        std::vector<bool> ancestor_wrap;
    };

    struct InlineInput {
        std::vector<InlineRun> runs;
        float font_size{constants::default_font_size_px};
        float line_height{constants::default_line_height_px};
        std::string text_align{"left"};
        std::string white_space{"normal"};
    };

    struct InlineFragment {
        std::string text;
        std::vector<std::string> ancestors;
        float x{};
        float y{};
        float width{};
        float height{};
        float baseline{};
        float font_size{};
        std::array<float, 4> color{};
        ShapedText shaped;
    };

    struct InlineLayout {
        float width{};
        float height{};
        float baseline{};
        float min_content{};
        float max_content{};
        std::size_t lines{};
        std::vector<InlineFragment> fragments;
    };

    struct UnicodePoint {
        std::uint32_t value{};
        std::uint32_t begin{};
        std::uint32_t end{};
    };

    // HarfBuzz validates/decodes UTF-8; libunibreak supplies UAX 14/29 boundaries.
    std::vector<UnicodePoint> probe_unicode_points(std::string_view text);

    struct UnicodeBreaks {
        std::vector<std::size_t> line;
        std::vector<std::size_t> mandatory;
        std::vector<std::size_t> grapheme;
    };

    UnicodeBreaks probe_unicode_breaks(std::string_view text);
    // The code points of text that word-break treats as typographic letter units (CSS Text 3 §5.2), marked at their
    // first byte: those of the UAX 14 classes AL, HL, NU, AI and ID, and the letters of SA, CJ and the Hangul classes.
    std::vector<bool> probe_letter_units(std::string_view text);
    InlineLayout layout_inline(const InlineInput&, float available, FontSet& fonts);
} // namespace weft
