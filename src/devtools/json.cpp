#include "devtools/json.hpp"

#include "base/css_scalar.hpp"
#include "bridge/bridge_value.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace weft::devtools {
    namespace {
        constexpr unsigned max_depth = 128;
        constexpr std::string_view replacement = "\xEF\xBF\xBD";

        struct Parser {
            std::string_view text;
            std::size_t p{};

            void space() {
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r')) {
                    ++p;
                }
            }

            bool literal(std::string_view word) {
                if (text.substr(p, word.size()) != word) {
                    return false;
                }
                p += word.size();
                return true;
            }

            static void utf8(std::string& out, std::uint32_t c) {
                if (c < 0x80) {
                    out += char(c);
                } else if (c < 0x800) {
                    out += char(0xC0 | c >> 6);
                    out += char(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    out += char(0xE0 | c >> 12);
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                } else {
                    out += char(0xF0 | c >> 18);
                    out += char(0x80 | (c >> 12 & 0x3F));
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                }
            }

            std::optional<std::uint32_t> hex4() {
                if (p + 4 > text.size()) {
                    return std::nullopt;
                }
                std::uint32_t v{};
                const auto r = std::from_chars(text.data() + p, text.data() + p + 4, v, 16);
                if (r.ptr != text.data() + p + 4) {
                    return std::nullopt;
                }
                p += 4;
                return v;
            }

            std::optional<std::string> string() {
                if (p >= text.size() || text[p] != '"') {
                    return std::nullopt;
                }
                ++p;
                std::string out;
                while (p < text.size()) {
                    const char c = text[p++];
                    if (c == '"') {
                        return out;
                    }
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    if (p >= text.size()) {
                        return std::nullopt;
                    }
                    const char e = text[p++];
                    switch (e) {
                        case '"':
                        case '\\':
                        case '/':
                            out += e;
                            break;
                        case 'b':
                            out += '\b';
                            break;
                        case 'f':
                            out += '\f';
                            break;
                        case 'n':
                            out += '\n';
                            break;
                        case 'r':
                            out += '\r';
                            break;
                        case 't':
                            out += '\t';
                            break;
                        case 'u': {
                            auto c1 = hex4();
                            if (!c1) {
                                return std::nullopt;
                            }
                            std::uint32_t code = *c1;
                            if (code >= 0xD800 && code < 0xDC00 && literal("\\u")) {
                                const auto c2 = hex4();
                                if (!c2) {
                                    return std::nullopt;
                                }
                                code = 0x10000 + ((code - 0xD800) << 10) + (*c2 - 0xDC00);
                            }
                            utf8(out, code);
                            break;
                        }
                        default:
                            return std::nullopt;
                    }
                }
                return std::nullopt;
            }

            std::optional<Json> value(unsigned depth) {
                if (depth > max_depth) {
                    return std::nullopt;
                }
                space();
                if (p >= text.size()) {
                    return std::nullopt;
                }
                const char c = text[p];
                if (c == '{') {
                    ++p;
                    Json::Object out;
                    space();
                    if (p < text.size() && text[p] == '}') {
                        ++p;
                        return Json(std::move(out));
                    }
                    for (;;) {
                        space();
                        auto key = string();
                        space();
                        if (!key || p >= text.size() || text[p++] != ':') {
                            return std::nullopt;
                        }
                        auto item = value(depth + 1);
                        space();
                        if (!item || p >= text.size()) {
                            return std::nullopt;
                        }
                        out.emplace_back(std::move(*key), std::move(*item));
                        const char next = text[p++];
                        if (next == '}') {
                            return Json(std::move(out));
                        }
                        if (next != ',') {
                            return std::nullopt;
                        }
                    }
                }
                if (c == '[') {
                    ++p;
                    Json::Array out;
                    space();
                    if (p < text.size() && text[p] == ']') {
                        ++p;
                        return Json(std::move(out));
                    }
                    for (;;) {
                        auto item = value(depth + 1);
                        space();
                        if (!item || p >= text.size()) {
                            return std::nullopt;
                        }
                        out.push_back(std::move(*item));
                        const char next = text[p++];
                        if (next == ']') {
                            return Json(std::move(out));
                        }
                        if (next != ',') {
                            return std::nullopt;
                        }
                    }
                }
                if (c == '"') {
                    auto s = string();
                    return s ? std::optional<Json>(Json(std::move(*s))) : std::nullopt;
                }
                if (literal("true")) {
                    return Json(true);
                }
                if (literal("false")) {
                    return Json(false);
                }
                if (literal("null")) {
                    return Json();
                }
                const auto start = p;
                while (p < text.size() && (std::string_view("+-.eE").find(text[p]) != std::string_view::npos ||
                                           (text[p] >= '0' && text[p] <= '9'))) {
                    ++p;
                }
                const auto number = css::parse_number<double>(std::string(text.substr(start, p - start)));
                return p > start && number ? std::optional<Json>(Json(*number)) : std::nullopt;
            }
        };

        void write_string(std::string& out, const std::string& text) {
            out += '"';
            std::size_t p{};
            while (p < text.size()) {
                const auto c = static_cast<unsigned char>(text[p]);
                if (c >= 0x80) {
                    // The next code point as UTF-8, or one replacement for the byte.
                    const std::size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                    if (length > 1 && bridge::utf8(std::string_view(text).substr(p, length))) {
                        out.append(text, p, length);
                        p += length;
                    } else {
                        out += replacement;
                        ++p;
                    }
                    continue;
                }
                ++p;
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (c < 0x20) {
                            std::array<char, 8> code{};
                            std::snprintf(code.data(), code.size(), "\\u%04x", unsigned(c));
                            out += code.data();
                        } else {
                            out += char(c);
                        }
                }
            }
            out += '"';
        }
    } // namespace

    std::optional<Json> Json::parse(std::string_view text) {
        Parser parser{text};
        auto out = parser.value(0);
        parser.space();
        return out && parser.p == text.size() ? out : std::nullopt;
    }

    void Json::write(std::string& out) const {
        if (std::holds_alternative<std::monostate>(value_)) {
            out += "null";
        } else if (const auto* b = std::get_if<bool>(&value_)) {
            out += *b ? "true" : "false";
        } else if (const auto* n = std::get_if<double>(&value_)) {
            if (!std::isfinite(*n)) {
                out += "null";
                return;
            }
            std::array<char, 40> digits{};
            const auto r = std::to_chars(digits.data(), digits.data() + digits.size(), *n);
            out.append(digits.data(), r.ptr);
        } else if (const auto* s = std::get_if<std::string>(&value_)) {
            write_string(out, *s);
        } else if (const auto* a = std::get_if<Array>(&value_)) {
            out += '[';
            bool first = true;
            for (const auto& item : *a) {
                if (!first) {
                    out += ',';
                }
                first = false;
                item.write(out);
            }
            out += ']';
        } else if (const auto* o = std::get_if<Object>(&value_)) {
            out += '{';
            bool first = true;
            for (const auto& [key, item] : *o) {
                if (!first) {
                    out += ',';
                }
                first = false;
                write_string(out, key);
                out += ':';
                item.write(out);
            }
            out += '}';
        }
    }

    std::string Json::dump() const {
        std::string out;
        write(out);
        return out;
    }

    const Json* Json::find(std::string_view key) const {
        if (const auto* members = object()) {
            for (const auto& [name, item] : *members) {
                if (name == key) {
                    return &item;
                }
            }
        }
        return nullptr;
    }

    std::string Json::string_at(std::string_view key, std::string_view fallback) const {
        const auto* item = find(key);
        return item && item->string() ? *item->string() : std::string(fallback);
    }

    double Json::number_at(std::string_view key, double fallback) const {
        const auto* item = find(key);
        return item && item->number() ? *item->number() : fallback;
    }

    bool Json::bool_at(std::string_view key, bool fallback) const {
        const auto* item = find(key);
        const auto* flag = item ? std::get_if<bool>(&item->value_) : nullptr;
        return flag ? *flag : fallback;
    }
} // namespace weft::devtools
