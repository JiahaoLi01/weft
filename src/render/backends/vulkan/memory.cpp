#include "render/backends/vulkan/memory.hpp"

#include "render/backends/vulkan/vulkan.hpp"

#include <iterator>

namespace weft::render::vulkan {
    RangeHeap::RangeHeap(std::uint64_t size) : size_(size) {
        ensure(size > 0, "a heap of some bytes");
        free_.emplace(0, size);
    }

    std::optional<std::uint64_t> RangeHeap::allocate(std::uint64_t size, std::uint64_t alignment) {
        ensure(size > 0 && alignment > 0 && (alignment & (alignment - 1)) == 0,
               "a range of some bytes at a power-of-two alignment");
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            const auto start = it->first;
            const auto end = start + it->second;
            const auto aligned = (start + alignment - 1) & ~(alignment - 1);
            if (aligned >= end || end - aligned < size) {
                continue;
            }
            // The padding before the range and the rest after it stay free.
            free_.erase(it);
            if (aligned > start) {
                free_.emplace(start, aligned - start);
            }
            if (end - aligned > size) {
                free_.emplace(aligned + size, end - aligned - size);
            }
            used_ += size;
            return aligned;
        }
        return std::nullopt;
    }

    void RangeHeap::free(std::uint64_t offset, std::uint64_t size) {
        ensure(size > 0 && offset < size_ && size <= size_ - offset && size <= used_, "a range of the heap");
        const auto next = free_.lower_bound(offset);
        ensure(next == free_.end() || size <= next->first - offset, "a range that no free range overlaps");
        auto start = offset;
        auto length = size;
        if (next != free_.begin()) {
            const auto previous = std::prev(next);
            const auto previous_end = previous->first + previous->second;
            ensure(previous_end <= offset, "a range that no free range overlaps");
            if (previous_end == offset) {
                start = previous->first;
                length += previous->second;
                free_.erase(previous);
            }
        }
        if (next != free_.end() && next->first == offset + size) {
            length += next->second;
            free_.erase(next);
        }
        free_.emplace(start, length);
        used_ -= size;
    }
} // namespace weft::render::vulkan
