#include "bridge/bridge_value.hpp"

#include "constants/bridge.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace weft::bridge {
    namespace {
        [[noreturn]] void malformed(const std::string& why) {
            throw std::invalid_argument("bridge value: " + why);
        }

        // Appends a code point as UTF-8.
        void append_utf8(std::string& out, std::uint32_t code) {
            if (code < 0x80) {
                out.push_back(char(code));
            } else if (code < 0x800) {
                out.push_back(char(0xc0 | (code >> 6)));
                out.push_back(char(0x80 | (code & 0x3f)));
            } else if (code < 0x10000) {
                out.push_back(char(0xe0 | (code >> 12)));
                out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(char(0x80 | (code & 0x3f)));
            } else {
                out.push_back(char(0xf0 | (code >> 18)));
                out.push_back(char(0x80 | ((code >> 12) & 0x3f)));
                out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(char(0x80 | (code & 0x3f)));
            }
        }

        // JSON text (RFC 8259) written as one value.
        class JsonParser {
        public:
            JsonParser(std::string_view text, ValueWriter& out) : text_(text), out_(out) {
            }

            void parse() {
                space();
                value(1);
                space();
                if (at_ != text_.size()) {
                    malformed("JSON text past the value");
                }
            }

        private:
            std::string_view text_;
            ValueWriter& out_;
            std::size_t at_{};
            std::string buffer_;

            bool end() const {
                return at_ == text_.size();
            }

            char peek() const {
                return end() ? '\0' : text_[at_];
            }

            void expect(char c) {
                if (peek() != c) {
                    malformed(std::string("JSON expected '") + c + "'");
                }
                ++at_;
            }

            void space() {
                while (!end() &&
                       (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
                    ++at_;
                }
            }

            void literal(std::string_view word) {
                if (text_.substr(at_, word.size()) != word) {
                    malformed("JSON literal");
                }
                at_ += word.size();
            }

            void value(unsigned depth) {
                switch (peek()) {
                    case '{':
                        object(depth);
                        break;
                    case '[':
                        array(depth);
                        break;
                    case '"':
                        out_.string(string());
                        break;
                    case 't':
                        literal("true");
                        out_.boolean(true);
                        break;
                    case 'f':
                        literal("false");
                        out_.boolean(false);
                        break;
                    case 'n':
                        literal("null");
                        out_.nil();
                        break;
                    default:
                        number();
                        break;
                }
            }

            void object(unsigned depth) {
                if (depth > constants::bridge::max_value_depth) {
                    malformed("JSON nested past the depth bound");
                }
                ++at_;
                const auto header = out_.open_map();
                std::uint32_t count{};
                space();
                if (peek() == '}') {
                    ++at_;
                    out_.close(header, count);
                    return;
                }
                for (;;) {
                    space();
                    if (peek() != '"') {
                        malformed("JSON object key");
                    }
                    out_.string(string());
                    space();
                    expect(':');
                    space();
                    value(depth + 1);
                    ++count;
                    space();
                    if (peek() == ',') {
                        ++at_;
                        continue;
                    }
                    expect('}');
                    break;
                }
                out_.close(header, count);
            }

            void array(unsigned depth) {
                if (depth > constants::bridge::max_value_depth) {
                    malformed("JSON nested past the depth bound");
                }
                ++at_;
                const auto header = out_.open_array();
                std::uint32_t count{};
                space();
                if (peek() == ']') {
                    ++at_;
                    out_.close(header, count);
                    return;
                }
                for (;;) {
                    space();
                    value(depth + 1);
                    ++count;
                    space();
                    if (peek() == ',') {
                        ++at_;
                        continue;
                    }
                    expect(']');
                    break;
                }
                out_.close(header, count);
            }

            std::uint32_t hex4() {
                if (text_.size() - at_ < 4) {
                    malformed("JSON \\u escape");
                }
                std::uint32_t code{};
                for (unsigned k = 0; k < 4; ++k) {
                    const char c = text_[at_++];
                    code <<= 4;
                    if (c >= '0' && c <= '9') {
                        code |= std::uint32_t(c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        code |= std::uint32_t(c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        code |= std::uint32_t(c - 'A' + 10);
                    } else {
                        malformed("JSON \\u escape");
                    }
                }
                return code;
            }

            // A string's text: escapes resolved, a surrogate pair as one code point; control characters are refused.
            const std::string& string() {
                ++at_;
                buffer_.clear();
                for (;;) {
                    if (end()) {
                        malformed("JSON string unterminated");
                    }
                    const char c = text_[at_++];
                    if (c == '"') {
                        return buffer_;
                    }
                    if (static_cast<unsigned char>(c) < 0x20) {
                        malformed("JSON control character in a string");
                    }
                    if (c != '\\') {
                        buffer_.push_back(c);
                        continue;
                    }
                    if (end()) {
                        malformed("JSON escape");
                    }
                    switch (text_[at_++]) {
                        case '"':
                            buffer_.push_back('"');
                            break;
                        case '\\':
                            buffer_.push_back('\\');
                            break;
                        case '/':
                            buffer_.push_back('/');
                            break;
                        case 'b':
                            buffer_.push_back('\b');
                            break;
                        case 'f':
                            buffer_.push_back('\f');
                            break;
                        case 'n':
                            buffer_.push_back('\n');
                            break;
                        case 'r':
                            buffer_.push_back('\r');
                            break;
                        case 't':
                            buffer_.push_back('\t');
                            break;
                        case 'u': {
                            auto code = hex4();
                            if (code >= 0xdc00 && code <= 0xdfff) {
                                malformed("JSON lone low surrogate");
                            }
                            if (code >= 0xd800 && code <= 0xdbff) {
                                if (text_.substr(at_, 2) != "\\u") {
                                    malformed("JSON lone high surrogate");
                                }
                                at_ += 2;
                                const auto low = hex4();
                                if (low < 0xdc00 || low > 0xdfff) {
                                    malformed("JSON lone high surrogate");
                                }
                                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                            }
                            append_utf8(buffer_, code);
                            break;
                        }
                        default:
                            malformed("JSON escape");
                    }
                }
            }

            // A number: an integer within int64 or uint64 stays one, others are finite doubles.
            void number() {
                const auto start = at_;
                const auto digits = [&] {
                    const auto first = at_;
                    while (!end() && text_[at_] >= '0' && text_[at_] <= '9') {
                        ++at_;
                    }
                    return at_ - first;
                };
                if (peek() == '-') {
                    ++at_;
                }
                const auto whole = at_;
                if (!digits() || (text_[whole] == '0' && at_ - whole > 1)) {
                    malformed("JSON number");
                }
                bool integral = true;
                if (peek() == '.') {
                    ++at_;
                    integral = false;
                    if (!digits()) {
                        malformed("JSON number");
                    }
                }
                if (peek() == 'e' || peek() == 'E') {
                    ++at_;
                    integral = false;
                    if (peek() == '+' || peek() == '-') {
                        ++at_;
                    }
                    if (!digits()) {
                        malformed("JSON number");
                    }
                }
                const auto* first = text_.data() + start;
                const auto* last = text_.data() + at_;
                if (integral) {
                    if (text_[start] == '-') {
                        // -0 stays the float it is in JS.
                        std::int64_t value{};
                        if (const auto r = std::from_chars(first, last, value);
                            r.ec == std::errc{} && r.ptr == last && value) {
                            out_.integer(value);
                            return;
                        }
                    } else {
                        std::uint64_t value{};
                        if (const auto r = std::from_chars(first, last, value); r.ec == std::errc{} && r.ptr == last) {
                            out_.unsigned_integer(value);
                            return;
                        }
                    }
                }
                double value{};
                const auto r = std::from_chars(first, last, value);
                if (r.ec != std::errc{} || r.ptr != last || !std::isfinite(value)) {
                    malformed("JSON number past the double range");
                }
                out_.number(value);
            }
        };

        void json_string(std::string& out, std::string_view text) {
            out.push_back('"');
            for (const char c : text) {
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
                        if (static_cast<unsigned char>(c) < 0x20) {
                            static constexpr char hex[] = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back(hex[(c >> 4) & 0xf]);
                            out.push_back(hex[c & 0xf]);
                        } else {
                            out.push_back(c);
                        }
                }
            }
            out.push_back('"');
        }

        template <class T>
        void json_number(std::string& out, T value) {
            char digits[32];
            const auto r = std::to_chars(digits, digits + sizeof digits, value);
            out.append(digits, r.ptr);
        }

        // depth counts the arrays and maps the item is in, itself included.
        void json_value(ValueReader& reader, std::string& out, unsigned depth) {
            const auto item = reader.next();
            using Type = ValueReader::Type;
            if ((item.type == Type::Array || item.type == Type::Map) && depth > constants::bridge::max_value_depth) {
                malformed("nested past the depth bound");
            }
            switch (item.type) {
                case Type::Nil:
                    out += "null";
                    break;
                case Type::Bool:
                    out += item.boolean ? "true" : "false";
                    break;
                case Type::Int:
                    json_number(out, item.integer);
                    break;
                case Type::Unsigned:
                    json_number(out, item.unsigned_integer);
                    break;
                case Type::Float:
                    json_number(out, item.number);
                    break;
                case Type::String:
                    json_string(out, item.text);
                    break;
                case Type::Bytes:
                    out.push_back('[');
                    for (std::size_t i = 0; i < item.bytes.size(); ++i) {
                        if (i) {
                            out.push_back(',');
                        }
                        json_number(out, unsigned(item.bytes[i]));
                    }
                    out.push_back(']');
                    break;
                case Type::Array:
                    out.push_back('[');
                    for (std::uint32_t i = 0; i < item.count; ++i) {
                        if (i) {
                            out.push_back(',');
                        }
                        json_value(reader, out, depth + 1);
                    }
                    out.push_back(']');
                    break;
                case Type::Map:
                    out.push_back('{');
                    for (std::uint32_t i = 0; i < item.count; ++i) {
                        if (i) {
                            out.push_back(',');
                        }
                        const auto key = reader.next();
                        if (key.type != Type::String) {
                            malformed("a map key that is not a string");
                        }
                        json_string(out, key.text);
                        out.push_back(':');
                        json_value(reader, out, depth + 1);
                    }
                    out.push_back('}');
                    break;
            }
        }
    } // namespace

    bool utf8(std::string_view text) {
        const auto* s = reinterpret_cast<const unsigned char*>(text.data());
        const std::size_t n = text.size();
        for (std::size_t i = 0; i < n;) {
            const unsigned c = s[i];
            if (c < 0x80) {
                ++i;
                continue;
            }
            std::size_t length{};
            std::uint32_t code{};
            std::uint32_t least{};
            if ((c & 0xe0) == 0xc0) {
                length = 2;
                code = c & 0x1f;
                least = 0x80;
            } else if ((c & 0xf0) == 0xe0) {
                length = 3;
                code = c & 0x0f;
                least = 0x800;
            } else if ((c & 0xf8) == 0xf0) {
                length = 4;
                code = c & 0x07;
                least = 0x10000;
            } else {
                return false;
            }
            if (n - i < length) {
                return false;
            }
            for (std::size_t k = 1; k < length; ++k) {
                const unsigned d = s[i + k];
                if ((d & 0xc0) != 0x80) {
                    return false;
                }
                code = (code << 6) | (d & 0x3f);
            }
            if (code < least || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                return false;
            }
            i += length;
        }
        return true;
    }

    std::span<const std::uint8_t> ValueReader::take(std::size_t count) {
        if (bytes_.size() - at_ < count) {
            malformed("truncated");
        }
        const auto out = bytes_.subspan(at_, count);
        at_ += count;
        return out;
    }

    std::uint64_t ValueReader::big_endian(std::size_t count) {
        std::uint64_t value{};
        for (const auto b : take(count)) {
            value = (value << 8) | b;
        }
        return value;
    }

    ValueReader::Item ValueReader::next() {
        Item item;
        const auto c = take(1)[0];
        const auto string = [&](std::uint64_t length) {
            const auto b = take(std::size_t(length));
            item.type = Type::String;
            item.text = {reinterpret_cast<const char*>(b.data()), b.size()};
            if (!utf8(item.text)) {
                malformed("a string that is not UTF-8");
            }
        };
        // A container's count: every item takes a byte at least, so a count past the bytes left is a lie.
        const auto sized = [&](Type type, std::uint64_t count) {
            if (count * (type == Type::Map ? 2 : 1) > bytes_.size() - at_) {
                malformed("a count past the bytes");
            }
            item.type = type;
            item.count = std::uint32_t(count);
        };
        const auto signed_value = [&](std::int64_t value) {
            item.type = Type::Int;
            item.integer = value;
        };
        const auto unsigned_value = [&](std::uint64_t value) {
            if (value <= std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                signed_value(std::int64_t(value));
            } else {
                item.type = Type::Unsigned;
                item.unsigned_integer = value;
            }
        };
        if (c <= 0x7f) {
            unsigned_value(c);
        } else if (c <= 0x8f) {
            sized(Type::Map, c & 0x0f);
        } else if (c <= 0x9f) {
            sized(Type::Array, c & 0x0f);
        } else if (c <= 0xbf) {
            string(c & 0x1f);
        } else if (c >= 0xe0) {
            signed_value(std::int8_t(c));
        } else {
            switch (c) {
                case 0xc0:
                    item.type = Type::Nil;
                    break;
                case 0xc2:
                case 0xc3:
                    item.type = Type::Bool;
                    item.boolean = c == 0xc3;
                    break;
                case 0xc4:
                case 0xc5:
                case 0xc6: {
                    const auto length = big_endian(std::size_t(1) << (c - 0xc4));
                    item.type = Type::Bytes;
                    item.bytes = take(std::size_t(length));
                    break;
                }
                case 0xca:
                    item.type = Type::Float;
                    item.number = std::bit_cast<float>(std::uint32_t(big_endian(4)));
                    break;
                case 0xcb:
                    item.type = Type::Float;
                    item.number = std::bit_cast<double>(big_endian(8));
                    break;
                case 0xcc:
                    unsigned_value(big_endian(1));
                    break;
                case 0xcd:
                    unsigned_value(big_endian(2));
                    break;
                case 0xce:
                    unsigned_value(big_endian(4));
                    break;
                case 0xcf:
                    unsigned_value(big_endian(8));
                    break;
                case 0xd0:
                    signed_value(std::int8_t(big_endian(1)));
                    break;
                case 0xd1:
                    signed_value(std::int16_t(big_endian(2)));
                    break;
                case 0xd2:
                    signed_value(std::int32_t(big_endian(4)));
                    break;
                case 0xd3:
                    signed_value(std::int64_t(big_endian(8)));
                    break;
                case 0xd9:
                    string(big_endian(1));
                    break;
                case 0xda:
                    string(big_endian(2));
                    break;
                case 0xdb:
                    string(big_endian(4));
                    break;
                case 0xdc:
                    sized(Type::Array, big_endian(2));
                    break;
                case 0xdd:
                    sized(Type::Array, big_endian(4));
                    break;
                case 0xde:
                    sized(Type::Map, big_endian(2));
                    break;
                case 0xdf:
                    sized(Type::Map, big_endian(4));
                    break;
                default:
                    malformed("a type outside the subset");
            }
        }
        if (item.type == Type::Float && !std::isfinite(item.number)) {
            malformed("a float that is not finite");
        }
        return item;
    }

    std::span<const std::uint8_t> ValueReader::skip() {
        const auto start = at_;
        std::uint64_t left = 1;
        while (left) {
            const auto item = next();
            --left;
            if (item.type == Type::Array) {
                left += item.count;
            } else if (item.type == Type::Map) {
                left += 2 * std::uint64_t(item.count);
            }
        }
        return bytes_.subspan(start, at_ - start);
    }

    void ValueWriter::big_endian(std::uint64_t value, std::size_t count) {
        for (std::size_t i = count; i-- > 0;) {
            data_.push_back(std::uint8_t(value >> (8 * i)));
        }
    }

    void ValueWriter::header(std::uint8_t fix, std::uint8_t fix_bound, std::uint8_t b8, std::uint8_t b16,
                             std::uint8_t b32, std::size_t count) {
        if (count <= fix_bound) {
            data_.push_back(std::uint8_t(fix | count));
        } else if (b8 && count <= 0xff) {
            data_.push_back(b8);
            big_endian(count, 1);
        } else if (count <= 0xffff) {
            data_.push_back(b16);
            big_endian(count, 2);
        } else if (count <= 0xffffffffu) {
            data_.push_back(b32);
            big_endian(count, 4);
        } else {
            malformed("a length past 32 bits");
        }
    }

    void ValueWriter::nil() {
        data_.push_back(0xc0);
    }

    void ValueWriter::boolean(bool value) {
        data_.push_back(value ? 0xc3 : 0xc2);
    }

    void ValueWriter::integer(std::int64_t value) {
        if (value >= 0) {
            unsigned_integer(std::uint64_t(value));
        } else if (value >= -32) {
            data_.push_back(std::uint8_t(std::int8_t(value)));
        } else if (value >= std::numeric_limits<std::int8_t>::min()) {
            data_.push_back(0xd0);
            big_endian(std::uint64_t(value), 1);
        } else if (value >= std::numeric_limits<std::int16_t>::min()) {
            data_.push_back(0xd1);
            big_endian(std::uint64_t(value), 2);
        } else if (value >= std::numeric_limits<std::int32_t>::min()) {
            data_.push_back(0xd2);
            big_endian(std::uint64_t(value), 4);
        } else {
            data_.push_back(0xd3);
            big_endian(std::uint64_t(value), 8);
        }
    }

    void ValueWriter::unsigned_integer(std::uint64_t value) {
        if (value <= 0x7f) {
            data_.push_back(std::uint8_t(value));
        } else if (value <= 0xff) {
            data_.push_back(0xcc);
            big_endian(value, 1);
        } else if (value <= 0xffff) {
            data_.push_back(0xcd);
            big_endian(value, 2);
        } else if (value <= 0xffffffffu) {
            data_.push_back(0xce);
            big_endian(value, 4);
        } else {
            data_.push_back(0xcf);
            big_endian(value, 8);
        }
    }

    void ValueWriter::number(double value) {
        if (!std::isfinite(value)) {
            malformed("a number that is not finite");
        }
        data_.push_back(0xcb);
        big_endian(std::bit_cast<std::uint64_t>(value), 8);
    }

    void ValueWriter::string(std::string_view text) {
        if (!utf8(text)) {
            malformed("a string that is not UTF-8");
        }
        header(0xa0, 31, 0xd9, 0xda, 0xdb, text.size());
        data_.insert(data_.end(), text.begin(), text.end());
    }

    void ValueWriter::bytes(std::span<const std::uint8_t> value) {
        const auto n = value.size();
        if (n <= 0xff) {
            data_.push_back(0xc4);
            big_endian(n, 1);
        } else if (n <= 0xffff) {
            data_.push_back(0xc5);
            big_endian(n, 2);
        } else if (n <= 0xffffffffu) {
            data_.push_back(0xc6);
            big_endian(n, 4);
        } else {
            malformed("a length past 32 bits");
        }
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void ValueWriter::array(std::uint32_t count) {
        header(0x90, 15, 0, 0xdc, 0xdd, count);
    }

    void ValueWriter::map(std::uint32_t count) {
        header(0x80, 15, 0, 0xde, 0xdf, count);
    }

    std::size_t ValueWriter::open_array() {
        const auto at = data_.size();
        data_.push_back(0xdd);
        big_endian(0, constants::bridge::container_header_bytes - 1);
        return at;
    }

    std::size_t ValueWriter::open_map() {
        const auto at = data_.size();
        data_.push_back(0xdf);
        big_endian(0, constants::bridge::container_header_bytes - 1);
        return at;
    }

    void ValueWriter::close(std::size_t header, std::uint32_t count) {
        for (std::size_t i = 0; i < constants::bridge::container_header_bytes - 1; ++i) {
            data_[header + 1 + i] = std::uint8_t(count >> (8 * (constants::bridge::container_header_bytes - 2 - i)));
        }
    }

    bool valid_value(std::span<const std::uint8_t> bytes, std::string* why) {
        try {
            if (bytes.size() > constants::bridge::max_value_bytes) {
                malformed("past the size bound");
            }
            ValueReader reader(bytes);

            // Items left in each open container, the value itself first; a map's keys at its even places from the end.
            struct Open {
                std::uint64_t left{};
                bool map{};
            };

            std::vector<Open> open{{1, false}};
            while (!open.empty()) {
                if (!open.back().left) {
                    open.pop_back();
                    continue;
                }
                const bool key = open.back().map && open.back().left % 2 == 0;
                --open.back().left;
                const auto item = reader.next();
                if (key && item.type != ValueReader::Type::String) {
                    malformed("a map key that is not a string");
                }
                if (item.type == ValueReader::Type::Array || item.type == ValueReader::Type::Map) {
                    if (open.size() > constants::bridge::max_value_depth) {
                        malformed("nested past the depth bound");
                    }
                    const bool map = item.type == ValueReader::Type::Map;
                    open.push_back({map ? 2 * std::uint64_t(item.count) : item.count, map});
                }
            }
            if (!reader.done()) {
                malformed("bytes past the value");
            }
            return true;
        } catch (const std::invalid_argument& e) {
            if (why) {
                *why = e.what();
            }
            return false;
        }
    }

    std::vector<std::uint8_t> value_from_json(std::string_view json) {
        ValueWriter out;
        JsonParser(json, out).parse();
        if (out.data().size() > constants::bridge::max_value_bytes) {
            malformed("past the size bound");
        }
        return out.take();
    }

    std::string value_to_json(std::span<const std::uint8_t> value) {
        ValueReader reader(value);
        std::string out;
        json_value(reader, out, 1);
        if (!reader.done()) {
            malformed("bytes past the value");
        }
        return out;
    }
} // namespace weft::bridge
