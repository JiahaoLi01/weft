#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <utility>

// The image caches of a native scene pass (scene_native.hpp): values by key, the least recently used first out once
// the cache is over its entries or bytes, but never a value the current pass or the last one used. One pass may hold
// more than the bounds while it lasts; a pass that reuses what the last one built finds all of it, though it builds
// values of its own too (the last pass's values it has yet to reach stay).
//
// Owner thread of the preparer.
namespace weft {
    template <class Key, class Value>
    class SceneImageCache {
    public:
        SceneImageCache(std::size_t max_entries, std::size_t max_bytes)
            : max_entries_(max_entries), max_bytes_(max_bytes) {
        }

        // Starts a pass: what passes before the last one used may go.
        void begin_pass() {
            ++pass_;
        }

        // The value under key, now the most recently used; null when absent.
        Value* find(const Key& key) {
            const auto found = index_.find(key);
            if (found == index_.end()) {
                return nullptr;
            }
            order_.splice(order_.end(), order_, found->second);
            found->second->pass = pass_;
            return &found->second->value;
        }

        // Puts value of bytes under key, in place of what was there, as the most recently used, then evicts the least
        // recently used values of passes before the last while over a bound.
        Value& put(const Key& key, Value value, std::size_t bytes) {
            erase(key);
            order_.push_back(Node{key, std::move(value), bytes, pass_});
            index_.emplace(key, std::prev(order_.end()));
            bytes_ += bytes;
            while ((order_.size() > max_entries_ || bytes_ > max_bytes_) && order_.front().pass + 1 < pass_) {
                bytes_ -= order_.front().bytes;
                index_.erase(order_.front().key);
                order_.pop_front();
                ++evictions_;
            }
            return order_.back().value;
        }

        void erase(const Key& key) {
            if (const auto found = index_.find(key); found != index_.end()) {
                bytes_ -= found->second->bytes;
                order_.erase(found->second);
                index_.erase(found);
            }
        }

        std::size_t size() const {
            return order_.size();
        }

        std::size_t bytes() const {
            return bytes_;
        }

        // Values evicted for the bounds, not those replaced or erased.
        std::size_t evictions() const {
            return evictions_;
        }

    private:
        struct Node {
            Key key;
            Value value;
            std::size_t bytes{};
            // The last pass that used the value.
            std::uint64_t pass{};
        };

        std::size_t max_entries_{};
        std::size_t max_bytes_{};
        std::uint64_t pass_{};
        // The least recently used first.
        std::list<Node> order_;
        std::map<Key, typename std::list<Node>::iterator> index_;
        std::size_t bytes_{};
        std::size_t evictions_{};
    };
} // namespace weft
