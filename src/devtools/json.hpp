#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The JSON of the developer tools' protocol (docs/devtools.md): a value, its text, and the text it comes from.
namespace weft::devtools {
    class Json {
    public:
        using Array = std::vector<Json>;
        using Object = std::vector<std::pair<std::string, Json>>;

        Json() = default;

        Json(bool value) : value_(value) {
        }

        template <class T>
            requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
        Json(T value) : value_(double(value)) {
        }

        Json(double value) : value_(value) {
        }

        Json(const char* value) : value_(std::string(value)) {
        }

        Json(std::string value) : value_(std::move(value)) {
        }

        Json(std::string_view value) : value_(std::string(value)) {
        }

        Json(Array value) : value_(std::move(value)) {
        }

        Json(Object value) : value_(std::move(value)) {
        }

        // None for text that is not one JSON value.
        static std::optional<Json> parse(std::string_view text);
        // Text with invalid UTF-8 replaced, as a page's strings may hold (a WebSocket text frame may not).
        std::string dump() const;

        // A member of an object; null when this is none or has no such member.
        const Json* find(std::string_view key) const;
        std::string string_at(std::string_view key, std::string_view fallback = {}) const;
        double number_at(std::string_view key, double fallback = 0) const;
        bool bool_at(std::string_view key, bool fallback = false) const;

        const std::string* string() const {
            return std::get_if<std::string>(&value_);
        }

        const double* number() const {
            return std::get_if<double>(&value_);
        }

        const Array* array() const {
            return std::get_if<Array>(&value_);
        }

        const Object* object() const {
            return std::get_if<Object>(&value_);
        }

    private:
        void write(std::string& out) const;

        std::variant<std::monostate, bool, double, std::string, Array, Object> value_;
    };
} // namespace weft::devtools
