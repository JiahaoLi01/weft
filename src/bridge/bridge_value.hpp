#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The values of the engine bridge (docs/engine-communication.md, 6.1): a subset of MessagePack, written and read
// without a third-party library. A value is nil, a boolean, an integer (int64, or uint64 above it), a finite float (32
// or 64 bit), a UTF-8 string, bytes, an array of values, or a map of string keys to values, at most
// constants::bridge::max_value_depth deep and constants::bridge::max_value_bytes long. Ext types, keys that are not
// strings, strings that are not UTF-8, floats that are not finite and bytes past the value are refused. JSON text
// converts to such a value.
namespace weft::bridge {
    // Reads one value's items in order: a scalar, or an array's or map's count, then its items (a map's keys and values
    // alternating). Throws std::invalid_argument for bytes that are not a value of the subset.
    class ValueReader {
    public:
        enum class Type : std::uint8_t {
            Nil,
            Bool,
            // An integer within int64 (integer); Unsigned only above it (unsigned_integer).
            Int,
            Unsigned,
            Float,
            String,
            Bytes,
            Array,
            Map
        };

        struct Item {
            Type type{};
            bool boolean{};
            std::int64_t integer{};
            std::uint64_t unsigned_integer{};
            double number{};
            std::string_view text;
            std::span<const std::uint8_t> bytes;
            // Items an array holds, or entries (key and value pairs) a map holds.
            std::uint32_t count{};
        };

        explicit ValueReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        }

        Item next();
        // The bytes of the whole value at the read position, which the reader moves past.
        std::span<const std::uint8_t> skip();

        std::size_t offset() const {
            return at_;
        }

        bool done() const {
            return at_ == bytes_.size();
        }

    private:
        std::span<const std::uint8_t> bytes_;
        std::size_t at_{};

        std::span<const std::uint8_t> take(std::size_t count);
        std::uint64_t big_endian(std::size_t count);
    };

    // Appends one value's items: a scalar, or an array's or map's count and then its items. The smallest form of each.
    class ValueWriter {
    public:
        void nil();
        void boolean(bool value);
        void integer(std::int64_t value);
        void unsigned_integer(std::uint64_t value);
        // Throws std::invalid_argument for a number that is not finite.
        void number(double value);
        // Throws std::invalid_argument for text that is not UTF-8.
        void string(std::string_view text);
        void bytes(std::span<const std::uint8_t> value);
        void array(std::uint32_t count);
        void map(std::uint32_t count);
        // A container whose count is known once its items are written: its header's place, to patch with the count.
        std::size_t open_array();
        std::size_t open_map();
        void close(std::size_t header, std::uint32_t count);

        std::vector<std::uint8_t>& data() {
            return data_;
        }

        std::vector<std::uint8_t> take() {
            return std::move(data_);
        }

    private:
        std::vector<std::uint8_t> data_;

        void header(std::uint8_t fix, std::uint8_t fix_bound, std::uint8_t b8, std::uint8_t b16, std::uint8_t b32,
                    std::size_t count);
        void big_endian(std::uint64_t value, std::size_t count);
    };

    // Whether text is well-formed UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF.
    bool utf8(std::string_view text);

    // Whether bytes hold exactly one value of the subset within the bounds; the reason when not.
    bool valid_value(std::span<const std::uint8_t> bytes, std::string* why = nullptr);

    // A value from JSON text (RFC 8259): integers that int64 or uint64 hold stay integers, other numbers are floats.
    // Throws std::invalid_argument for text that is not JSON, or past the bounds.
    std::vector<std::uint8_t> value_from_json(std::string_view json);

    // A value as JSON text (bytes as an array of numbers), for diagnostics and tests. Throws std::invalid_argument for
    // bytes that are not a value.
    std::string value_to_json(std::span<const std::uint8_t> value);
} // namespace weft::bridge
