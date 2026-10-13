#include "text/case_mapping.hpp"

#include <libunicode.h>

#include <vector>

namespace weft {
    namespace {
        // A decoded unit: a code point, or with this bit a byte that is not UTF-8, kept as it is.
        constexpr std::uint32_t raw_byte = 0x80000000u;
        constexpr char32_t capital_sigma = 0x3a3;
        constexpr char32_t final_small_sigma = 0x3c2;

        std::vector<std::uint32_t> decode(std::string_view s) {
            std::vector<std::uint32_t> out;
            out.reserve(s.size());
            for (std::size_t i = 0; i < s.size();) {
                const auto lead = static_cast<unsigned char>(s[i]);
                const std::size_t length = lead < 0x80                   ? 1
                                           : lead >= 0xc2 && lead < 0xe0 ? 2
                                           : lead >= 0xe0 && lead < 0xf0 ? 3
                                           : lead >= 0xf0 && lead < 0xf5 ? 4
                                                                         : 0;
                std::uint32_t c = length == 1   ? lead
                                  : length == 2 ? lead & 0x1f
                                  : length == 3 ? lead & 0x0f
                                                : lead & 0x07;
                bool valid = length > 0 && i + length <= s.size();
                for (std::size_t k = 1; valid && k < length; ++k) {
                    const auto next = static_cast<unsigned char>(s[i + k]);
                    valid = (next & 0xc0) == 0x80;
                    c = c << 6 | (next & 0x3f);
                }
                // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
                valid = valid && !(length == 3 && (c < 0x800 || (c >= 0xd800 && c < 0xe000))) &&
                        !(length == 4 && (c < 0x10000 || c > 0x10ffff));
                if (valid) {
                    out.push_back(c);
                    i += length;
                } else {
                    out.push_back(raw_byte | lead);
                    ++i;
                }
            }
            return out;
        }

        void encode(std::string& out, std::uint32_t c) {
            if (c & raw_byte) {
                out += char(c & 0xff);
            } else if (c < 0x80) {
                out += char(c);
            } else if (c < 0x800) {
                out += char(0xc0 | c >> 6);
                out += char(0x80 | (c & 0x3f));
            } else if (c < 0x10000) {
                out += char(0xe0 | c >> 12);
                out += char(0x80 | (c >> 6 & 0x3f));
                out += char(0x80 | (c & 0x3f));
            } else {
                out += char(0xf0 | c >> 18);
                out += char(0x80 | (c >> 12 & 0x3f));
                out += char(0x80 | (c >> 6 & 0x3f));
                out += char(0x80 | (c & 0x3f));
            }
        }

        // Unicode's Final_Sigma: a cased letter before the sigma and none after it, case-ignorable characters
        // between.
        bool final_sigma(const std::vector<std::uint32_t>& units, std::size_t at) {
            bool cased_before{};
            for (std::size_t k = at; k-- > 0;) {
                if (units[k] & raw_byte) {
                    break;
                }
                if (!lre_is_case_ignorable(units[k])) {
                    cased_before = lre_is_cased(units[k]);
                    break;
                }
            }
            if (!cased_before) {
                return false;
            }
            for (std::size_t k = at + 1; k < units.size(); ++k) {
                if (units[k] & raw_byte) {
                    break;
                }
                if (!lre_is_case_ignorable(units[k])) {
                    return !lre_is_cased(units[k]);
                }
            }
            return true;
        }
    } // namespace

    std::string case_mapped(std::string_view utf8, TextCase to) {
        const auto units = decode(utf8);
        std::string out;
        out.reserve(utf8.size());
        for (std::size_t i = 0; i < units.size(); ++i) {
            const auto c = units[i];
            if (c & raw_byte) {
                encode(out, c);
                continue;
            }
            if (to == TextCase::Lower && c == capital_sigma && final_sigma(units, i)) {
                encode(out, final_small_sigma);
                continue;
            }
            std::uint32_t mapped[LRE_CC_RES_LEN_MAX];
            const int count = lre_case_conv(mapped, c, to == TextCase::Upper ? 0 : 1);
            for (int k = 0; k < count; ++k) {
                encode(out, mapped[k]);
            }
        }
        return out;
    }
} // namespace weft
