#include "view/view_text_field.hpp"

namespace weft {
    namespace {
        bool continuation(unsigned char b) {
            return (b & 0xC0) == 0x80;
        }

        // The code point that starts at i.
        char32_t decode(std::string_view text, std::uint32_t i) {
            const auto b = static_cast<unsigned char>(text[i]);
            const int n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
            char32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
            for (int k = 1; k < n && i + k < text.size(); ++k) {
                c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
            return c;
        }

        enum class Kind : std::uint8_t {
            Space,
            Word,
            Ideograph,
            Other
        };

        bool space(char32_t c) {
            return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == 0xA0 || c == 0x1680 ||
                   (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
                   c == 0x3000;
        }

        bool punctuation(char32_t c) {
            return (c >= 0x2010 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) || (c >= 0x3008 && c <= 0x3011) ||
                   (c >= 0x3014 && c <= 0x301F) || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
                   (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) || c == 0x00A1 || c == 0x00A7 ||
                   c == 0x00AB || c == 0x00B6 || c == 0x00B7 || c == 0x00BB || c == 0x00BF;
        }

        // CJK ideographs, kana and the marks between them, which no space separates from the words beside them.
        bool ideograph(char32_t c) {
            return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
                   (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3134F) || c == 0x3005 || c == 0x3006;
        }

        Kind kind(char32_t c) {
            if (space(c)) {
                return Kind::Space;
            }
            if (ideograph(c)) {
                return Kind::Ideograph;
            }
            if ((c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' ||
                (c >= 0x80 && !punctuation(c))) {
                return Kind::Word;
            }
            return Kind::Other;
        }

        Kind kind_at(std::string_view text, std::uint32_t i) {
            return kind(decode(text, i));
        }
    } // namespace

    std::uint32_t previous_character(std::string_view text, std::uint32_t offset) {
        if (offset == 0 || offset > text.size()) {
            return offset > text.size() ? static_cast<std::uint32_t>(text.size()) : 0;
        }
        auto i = offset - 1;
        while (i > 0 && continuation(static_cast<unsigned char>(text[i]))) {
            --i;
        }
        return i;
    }

    std::uint32_t next_character(std::string_view text, std::uint32_t offset) {
        if (offset >= text.size()) {
            return static_cast<std::uint32_t>(text.size());
        }
        auto i = offset + 1;
        while (i < text.size() && continuation(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        return i;
    }

    std::uint32_t previous_word(std::string_view text, std::uint32_t offset) {
        auto i = offset > text.size() ? static_cast<std::uint32_t>(text.size()) : offset;
        while (i > 0 && kind_at(text, previous_character(text, i)) == Kind::Space) {
            i = previous_character(text, i);
        }
        if (i == 0) {
            return 0;
        }
        const auto run = kind_at(text, previous_character(text, i));
        while (i > 0 && kind_at(text, previous_character(text, i)) == run) {
            i = previous_character(text, i);
        }
        return i;
    }

    std::uint32_t next_word(std::string_view text, std::uint32_t offset) {
        auto i = offset;
        if (i >= text.size()) {
            return static_cast<std::uint32_t>(text.size());
        }
        const auto run = kind_at(text, i);
        if (run != Kind::Space) {
            while (i < text.size() && kind_at(text, i) == run) {
                i = next_character(text, i);
            }
        }
        while (i < text.size() && kind_at(text, i) == Kind::Space) {
            i = next_character(text, i);
        }
        return i;
    }

    std::pair<std::uint32_t, std::uint32_t> word_at(std::string_view text, std::uint32_t offset) {
        if (text.empty()) {
            return {0, 0};
        }
        auto at = offset >= text.size() ? previous_character(text, static_cast<std::uint32_t>(text.size())) : offset;
        const auto run = kind_at(text, at);
        auto start = at;
        while (start > 0 && kind_at(text, previous_character(text, start)) == run) {
            start = previous_character(text, start);
        }
        auto end = at;
        while (end < text.size() && kind_at(text, end) == run) {
            end = next_character(text, end);
        }
        if (run == Kind::Word || run == Kind::Ideograph) {
            while (end < text.size() && kind_at(text, end) == Kind::Space && decode(text, end) != U'\n') {
                end = next_character(text, end);
            }
        }
        return {start, end};
    }

    std::pair<std::uint32_t, std::uint32_t> line_at(std::string_view text, std::uint32_t offset) {
        const auto at = offset > text.size() ? text.size() : std::size_t(offset);
        const auto before = at == 0 ? std::string_view::npos : text.rfind('\n', at - 1);
        const auto start = before == std::string_view::npos ? 0 : before + 1;
        const auto after = text.find('\n', at);
        const auto end = after == std::string_view::npos ? text.size() : after;
        return {static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end)};
    }
} // namespace weft
