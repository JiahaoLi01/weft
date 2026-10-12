#pragma once

#include <cstdint>
#include <map>
#include <optional>

// The ranges of one block of device memory that resources share (vulkan::Device places small images in shared blocks
// this way): a new range takes the first free one that fits at its alignment, and a freed range joins its free
// neighbours. Not thread safe.
namespace weft::render::vulkan {
    class RangeHeap {
    public:
        explicit RangeHeap(std::uint64_t size);

        // The offset of a new range of size bytes at a multiple of alignment (a power of two): the first free range
        // that fits. None when no free range fits.
        std::optional<std::uint64_t> allocate(std::uint64_t size, std::uint64_t alignment);
        // Frees [offset, offset + size), a range allocate() returned. Throws when that overlaps a free range or leaves
        // the heap.
        void free(std::uint64_t offset, std::uint64_t size);

        std::uint64_t size() const noexcept {
            return size_;
        }

        // Bytes of the allocated ranges.
        std::uint64_t used() const noexcept {
            return used_;
        }

        // The free ranges' sizes by offset, ordered and apart: one range over the heap once every range is freed.
        const std::map<std::uint64_t, std::uint64_t>& free_ranges() const noexcept {
            return free_;
        }

    private:
        std::uint64_t size_;
        std::uint64_t used_{};
        std::map<std::uint64_t, std::uint64_t> free_;
    };
} // namespace weft::render::vulkan
