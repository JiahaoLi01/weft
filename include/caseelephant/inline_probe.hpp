#pragma once
#include "caseelephant/text_probe.hpp"
#include "caseelephant/constants/common.hpp"
#include <array>
#include <string>
#include <vector>

namespace caseelephant {
    // Owning, owner-thread inputs. Ancestors identify inline element fragments.
    struct ProbeInlineRun {
        std::string text;
        std::vector<std::string> ancestors;
        float font_size{constants::default_font_size_px};
        float line_height{constants::default_line_height_px};
        std::array<float, 4> color{0, 0, 0, 1};
        std::string white_space{"normal"};
        bool hard_break{};
        std::vector<bool> ancestor_wrap;
    };

    struct ProbeInlineInput {
        std::vector<ProbeInlineRun> runs;
        float font_size{constants::default_font_size_px};
        float line_height{constants::default_line_height_px};
        std::string text_align{"left"};
        std::string white_space{"normal"};
    };

    struct ProbeInlineFragment {
        std::string text;
        std::vector<std::string> ancestors;
        float x{};
        float y{};
        float width{};
        float height{};
        float baseline{};
        float font_size{};
        std::array<float, 4> color{};
        ProbeShapedText shaped;
    };

    struct ProbeInlineLayout {
        float width{};
        float height{};
        float baseline{};
        float min_content{};
        float max_content{};
        std::size_t lines{};
        std::vector<ProbeInlineFragment> fragments;
    };

    struct ProbeUnicodePoint {
        std::uint32_t value{};
        std::uint32_t begin{};
        std::uint32_t end{};
    };

    // HarfBuzz validates/decodes UTF-8; libunibreak supplies UAX 14/29 boundaries.
    std::vector<ProbeUnicodePoint> probe_unicode_points(std::string_view text);

    struct ProbeUnicodeBreaks {
        std::vector<std::size_t> line;
        std::vector<std::size_t> mandatory;
        std::vector<std::size_t> grapheme;
    };

    ProbeUnicodeBreaks probe_unicode_breaks(std::string_view text);
    ProbeInlineLayout layout_inline_probe(const ProbeInlineInput&, float available, TextProbeFontSet& fonts);
} // namespace caseelephant
