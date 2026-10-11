#pragma once

#include "dom/node.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace weft::dom {
    // Values kept beside the DOM for some of its nodes, indexed by node slot. A node's identity finds its value while
    // that occupancy of the slot lasts; identities of one view epoch only. The owner erases a node's value when it
    // releases the node, so a later occupant of the slot starts without one.
    template <class T>
    class NodeTable {
    public:
        // The node's value, or nullptr when it has none or the identity names an earlier occupant of the slot.
        T* find(const NodeIdentity& node) noexcept {
            if (node.slot >= items_.size() || !items_[node.slot] || items_[node.slot]->generation != node.generation) {
                return nullptr;
            }
            return &items_[node.slot]->value;
        }

        const T* find(const NodeIdentity& node) const noexcept {
            return const_cast<NodeTable*>(this)->find(node);
        }

        // Gives a node that has no value one.
        T& emplace(const NodeIdentity& node, T value) {
            if (node.slot >= items_.size()) {
                items_.resize(std::size_t(node.slot) + 1);
            }
            assert(!items_[node.slot]);
            items_[node.slot] = std::make_unique<Item>(Item{node.generation, std::move(value)});
            ++size_;
            return items_[node.slot]->value;
        }

        // Destroys the node's value, if it has one.
        void erase(const NodeIdentity& node) noexcept {
            if (find(node)) {
                items_[node.slot].reset();
                --size_;
            }
        }

        std::size_t size() const noexcept {
            return size_;
        }

    private:
        struct Item {
            std::uint32_t generation{};
            T value;
        };

        std::vector<std::unique_ptr<Item>> items_;
        std::size_t size_{};
    };
} // namespace weft::dom
