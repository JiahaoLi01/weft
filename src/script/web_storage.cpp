#include "script/web_storage.hpp"

#include "constants/script.hpp"

#include <iterator>
#include <stdexcept>

namespace weft {
    std::optional<std::string_view> WebStorage::get(std::string_view key) const {
        const auto found = items_.find(key);
        if (found == items_.end()) {
            return std::nullopt;
        }
        return std::string_view(found->second);
    }

    bool WebStorage::set(std::string_view key, std::string_view value) {
        const auto found = items_.find(key);
        const std::size_t before = found == items_.end() ? 0 : key.size() + found->second.size();
        const std::size_t bytes = bytes_ - before + key.size() + value.size();
        if (bytes > std::size_t(constants::script::max_storage_bytes)) {
            return false;
        }
        if (found != items_.end() && found->second == value) {
            return true;
        }
        if (found == items_.end()) {
            items_.emplace(std::string(key), std::string(value));
        } else {
            found->second = value;
        }
        bytes_ = bytes;
        ++revision_;
        return true;
    }

    void WebStorage::remove(std::string_view key) {
        const auto found = items_.find(key);
        if (found == items_.end()) {
            return;
        }
        bytes_ -= found->first.size() + found->second.size();
        items_.erase(found);
        ++revision_;
    }

    void WebStorage::clear() {
        if (items_.empty()) {
            return;
        }
        items_.clear();
        bytes_ = 0;
        ++revision_;
    }

    std::optional<std::string_view> WebStorage::key(std::size_t index) const {
        if (index >= items_.size()) {
            return std::nullopt;
        }
        return std::string_view(std::next(items_.begin(), std::ptrdiff_t(index))->first);
    }

    std::size_t WebStorage::size() const {
        return items_.size();
    }

    void WebStorage::replace(Items items) {
        std::size_t bytes{};
        for (const auto& [key, value] : items) {
            bytes += key.size() + value.size();
        }
        if (bytes > std::size_t(constants::script::max_storage_bytes)) {
            throw std::length_error("WEFT_STORAGE_QUOTA: the items exceed the storage quota");
        }
        items_ = std::move(items);
        bytes_ = bytes;
        ++revision_;
    }

    const WebStorage::Items& WebStorage::items() const {
        return items_;
    }

    std::uint64_t WebStorage::revision() const {
        return revision_;
    }
} // namespace weft
