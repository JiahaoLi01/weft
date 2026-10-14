#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Shelf packing of small images into square atlas pages, so that consecutive images draw from one texture. A shelf is a
// row as tall as the image that opened it; a released region's span is free again for any image its shelf is tall
// enough for, an emptied shelf joins the empty shelves beside it (and a later, shorter image splits it), and empty
// shelves at the end of a page give their rows back. A page with no regions left starts empty. Regions never move
// (docs/architecture.md, "FramePacket 与 GPU 生命周期"): a renderer keeps a page's texture while any of its regions may
// still be sampled, and gives a region back only once nothing samples it.
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
        // Gives a region back. Throws std::logic_error for a region that is not placed (released twice).
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
        // Free texels [x, x + width) of a shelf's row.
        struct Span {
            std::uint32_t x{};
            std::uint32_t width{};
        };

        struct Shelf {
            std::uint32_t y{};
            std::uint32_t height{};
            // Left to right, none touching another.
            std::vector<Span> free;
            std::size_t live{};
        };

        struct Page {
            // Top to bottom, each starting where the one above ends; the last ends at bottom.
            std::vector<Shelf> shelves;
            std::uint32_t bottom{};
            std::size_t live{};
        };

        std::optional<Region> place(std::uint32_t index, std::uint32_t width, std::uint32_t height);
        // Takes width texels from the first free span of shelf (of page index) wide enough; nothing without one.
        std::optional<Region> take(std::uint32_t index, Shelf& shelf, std::uint32_t width, std::uint32_t height);

        std::uint32_t page_size_;
        std::uint32_t max_pages_;
        std::vector<Page> pages_;
    };
} // namespace weft::render
