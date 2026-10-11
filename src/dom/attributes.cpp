#include "dom/attributes.hpp"

#include "constants/common.hpp"
#include "constants/dom.hpp"

#include <algorithm>
#include <string_view>

namespace weft::dom {
    namespace {
        bool digit(char c) {
            return c >= '0' && c <= '9';
        }
    } // namespace

    Result<std::uint32_t> canvas_dimension(const std::string* attribute, CanvasSide side) {
        const auto fallback = std::uint32_t(side == CanvasSide::Width ? constants::dom::default_canvas_width
                                                                      : constants::dom::default_canvas_height);
        if (!attribute) {
            return fallback;
        }
        const std::string_view text = *attribute;
        // HTML's ASCII whitespace, an optional sign, then at least one digit; trailing text is ignored.
        auto p = text.find_first_not_of(" \t\n\f\r");
        if (p == text.npos) {
            return fallback;
        }
        const bool negative = text[p] == '-';
        if (negative || text[p] == '+') {
            ++p;
        }
        if (p == text.size() || !digit(text[p])) {
            return fallback;
        }
        // Saturates just above the profile bound, which keeps any digit run in range.
        const auto bound = std::uint64_t(constants::max_canvas_dimension);
        std::uint64_t value{};
        for (; p < text.size() && digit(text[p]); ++p) {
            value = std::min(value * 10 + std::uint64_t(text[p] - '0'), bound + 1);
        }
        // Minus zero is zero; any other negative value is not a non-negative integer.
        if (negative) {
            return value == 0 ? 0u : fallback;
        }
        if (value > bound) {
            return make_diagnostic(Diag::CanvasSizeLimit, "backing dimensions <=2048");
        }
        return std::uint32_t(value);
    }
} // namespace weft::dom
