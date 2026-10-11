#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Shelf packing of small images into square atlas pages, so that consecutive images draw from one texture. A page only
// grows until every region on it is released; then it starts empty. Regions never move (docs/architecture.md, "FramePacket
// 与 GPU 生命周期"): a renderer keeps a page's texture while any of its regions may still be sampled.
namespace weft::render {
    class AtlasPacker {
    public:
        struct Region {
            std::uint32_t page{};
            std::uint32_t x{};
            std::uint32_t y{};
            std::uint32_t width{};
            std::uint32_t height{};
        };

        AtlasPacker(std::uint32_t page_size, std::uint32_t max_pages);

        // A width x height region on the first page with room, opening a page when none has one; nothing when the region
        // is empty or larger than a page, or every page is full.
        std::optional<Region> allocate(std::uint32_t width, std::uint32_t height);
        // Gives a region back; a page with no regions left starts empty.
        void release(const Region& region);

        // Pages opened so far: page indices are below this.
        std::uint32_t pages() const {
            return std::uint32_t(pages_.size());
        }

        std::uint32_t page_size() const {
            return page_size_;
        }

        // Regions in use on all pages.
        std::size_t live() const;

    private:
        struct Shelf {
            std::uint32_t y{};
            std::uint32_t height{};
            std::uint32_t used{};
        };

        struct Page {
            std::vector<Shelf> shelves;
            std::uint32_t bottom{};
            std::size_t live{};
        };

        std::optional<Region> place(std::uint32_t index, std::uint32_t width, std::uint32_t height);

        std::uint32_t page_size_;
        std::uint32_t max_pages_;
        std::vector<Page> pages_;
    };
} // namespace weft::render
