#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace weft {
    // A Web Storage area (the web platform's localStorage or sessionStorage): string keys to string values, at most
    // constants::script::max_storage_bytes of keys and values (UTF-8), in key order. Its View keeps it across page
    // loads; revision counts its changes, so a host saves it when the revision moved. Owner thread.
    class WebStorage {
    public:
        using Items = std::map<std::string, std::string, std::less<>>;

        std::optional<std::string_view> get(std::string_view key) const;
        // Whether it fit: an item past the quota leaves the area as it was.
        bool set(std::string_view key, std::string_view value);
        void remove(std::string_view key);
        void clear();
        // The key at index in key order; none past the end.
        std::optional<std::string_view> key(std::size_t index) const;
        std::size_t size() const;
        // Replaces every item (a host restoring what it saved); std::length_error past the quota, and the area stays.
        void replace(Items items);
        const Items& items() const;
        std::uint64_t revision() const;

    private:
        Items items_;
        std::size_t bytes_{};
        std::uint64_t revision_{};
    };
} // namespace weft
