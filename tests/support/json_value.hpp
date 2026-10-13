#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A JSON reader for test fixtures: objects keep their member order, strings decode \u escapes (surrogate pairs too)
// to UTF-8, numbers are doubles. Malformed input throws.
namespace weft::testing {
    struct JsonValue {
        enum class Kind {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };

        Kind kind{Kind::Null};
        bool boolean{};
        double number{};
        std::string string;
        std::vector<JsonValue> array;
        std::vector<std::pair<std::string, JsonValue>> object;

        const JsonValue* find(std::string_view key) const {
            for (const auto& [k, v] : object) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }

        const JsonValue& operator[](std::string_view key) const {
            if (const auto* v = find(key)) {
                return *v;
            }
            throw std::runtime_error("JSON member missing: " + std::string(key));
        }

        const JsonValue& operator[](std::size_t i) const {
            return array.at(i);
        }

        std::size_t size() const {
            return kind == Kind::Array ? array.size() : object.size();
        }
    };

    class JsonReader {
    public:
        explicit JsonReader(std::string_view text) : text_(text) {
        }

        JsonValue document() {
            auto v = value();
            space();
            if (at_ != text_.size()) {
                fail("trailing input");
            }
            return v;
        }

    private:
        std::string_view text_;
        std::size_t at_{};

        [[noreturn]] void fail(const char* why) const {
            throw std::runtime_error(std::string("JSON ") + why + " at " + std::to_string(at_));
        }

        void space() {
            while (at_ < text_.size() &&
                   (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
                ++at_;
            }
        }

        char peek() {
            space();
            if (at_ >= text_.size()) {
                fail("unexpected end");
            }
            return text_[at_];
        }

        void expect(char c) {
            if (peek() != c) {
                fail("unexpected character");
            }
            ++at_;
        }

        static void utf8(std::string& out, std::uint32_t c) {
            if (c < 0x80) {
                out += char(c);
            } else if (c < 0x800) {
                out += char(0xc0 | (c >> 6));
                out += char(0x80 | (c & 0x3f));
            } else if (c < 0x10000) {
                out += char(0xe0 | (c >> 12));
                out += char(0x80 | ((c >> 6) & 0x3f));
                out += char(0x80 | (c & 0x3f));
            } else {
                out += char(0xf0 | (c >> 18));
                out += char(0x80 | ((c >> 12) & 0x3f));
                out += char(0x80 | ((c >> 6) & 0x3f));
                out += char(0x80 | (c & 0x3f));
            }
        }

        std::uint32_t hex4() {
            if (at_ + 4 > text_.size()) {
                fail("short escape");
            }
            std::uint32_t c{};
            for (int i = 0; i < 4; ++i) {
                const char h = text_[at_++];
                c = c * 16 + (h >= '0' && h <= '9'   ? std::uint32_t(h - '0')
                              : h >= 'a' && h <= 'f' ? std::uint32_t(h - 'a' + 10)
                              : h >= 'A' && h <= 'F' ? std::uint32_t(h - 'A' + 10)
                                                     : (fail("bad escape"), 0u));
            }
            return c;
        }

        std::string string() {
            expect('"');
            std::string out;
            while (true) {
                if (at_ >= text_.size()) {
                    fail("unterminated string");
                }
                const char c = text_[at_++];
                if (c == '"') {
                    return out;
                }
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (at_ >= text_.size()) {
                    fail("unterminated escape");
                }
                const char e = text_[at_++];
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
                        auto code = hex4();
                        if (code >= 0xd800 && code < 0xdc00 && at_ + 1 < text_.size() && text_[at_] == '\\' &&
                            text_[at_ + 1] == 'u') {
                            at_ += 2;
                            const auto low = hex4();
                            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                        }
                        utf8(out, code);
                        break;
                    }
                    default:
                        fail("bad escape");
                }
            }
        }

        JsonValue value() {
            JsonValue v;
            const char c = peek();
            if (c == '{') {
                v.kind = JsonValue::Kind::Object;
                ++at_;
                if (peek() == '}') {
                    ++at_;
                    return v;
                }
                while (true) {
                    auto key = string();
                    expect(':');
                    v.object.emplace_back(std::move(key), value());
                    if (peek() == ',') {
                        ++at_;
                        continue;
                    }
                    expect('}');
                    return v;
                }
            }
            if (c == '[') {
                v.kind = JsonValue::Kind::Array;
                ++at_;
                if (peek() == ']') {
                    ++at_;
                    return v;
                }
                while (true) {
                    v.array.push_back(value());
                    if (peek() == ',') {
                        ++at_;
                        continue;
                    }
                    expect(']');
                    return v;
                }
            }
            if (c == '"') {
                v.kind = JsonValue::Kind::String;
                v.string = string();
                return v;
            }
            if (text_.substr(at_, 4) == "true" || text_.substr(at_, 5) == "false") {
                v.kind = JsonValue::Kind::Bool;
                v.boolean = text_[at_] == 't';
                at_ += v.boolean ? 4 : 5;
                return v;
            }
            if (text_.substr(at_, 4) == "null") {
                at_ += 4;
                return v;
            }
            const auto start = at_;
            while (at_ < text_.size() &&
                   (std::string_view("+-.eE0123456789").find(text_[at_]) != std::string_view::npos)) {
                ++at_;
            }
            if (start == at_) {
                fail("unexpected character");
            }
            v.kind = JsonValue::Kind::Number;
            v.number = std::stod(std::string(text_.substr(start, at_ - start)));
            return v;
        }
    };

    inline JsonValue parse_json(std::string_view text) {
        return JsonReader(text).document();
    }
} // namespace weft::testing
