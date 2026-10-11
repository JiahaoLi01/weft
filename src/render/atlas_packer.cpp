#include "render/atlas_packer.hpp"

#include <stdexcept>

namespace weft::render {
    AtlasPacker::AtlasPacker(std::uint32_t page_size, std::uint32_t max_pages)
        : page_size_(page_size), max_pages_(max_pages) {
    }

    std::optional<AtlasPacker::Region> AtlasPacker::place(std::uint32_t index, std::uint32_t width,
                                                          std::uint32_t height) {
        auto& page = pages_[index];
        // The lowest shelf that is tall enough and has the width left.
        Shelf* best{};
        for (auto& shelf : page.shelves) {
            if (shelf.height >= height && page_size_ - shelf.used >= width && (!best || shelf.height < best->height)) {
                best = &shelf;
            }
        }
        if (!best) {
            if (page_size_ - page.bottom < height) {
                return std::nullopt;
            }
            best = &page.shelves.emplace_back(Shelf{page.bottom, height, 0});
            page.bottom += height;
        }
        const Region region{index, best->used, best->y, width, height};
        best->used += width;
        ++page.live;
        return region;
    }

    std::optional<AtlasPacker::Region> AtlasPacker::allocate(std::uint32_t width, std::uint32_t height) {
        if (!width || !height || width > page_size_ || height > page_size_) {
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            if (auto region = place(i, width, height)) {
                return region;
            }
        }
        if (pages_.size() >= max_pages_) {
            return std::nullopt;
        }
        pages_.emplace_back();
        return place(std::uint32_t(pages_.size() - 1), width, height);
    }

    void AtlasPacker::release(const Region& region) {
        if (region.page >= pages_.size() || !pages_[region.page].live) {
            throw std::logic_error("atlas region released twice");
        }
        auto& page = pages_[region.page];
        if (!--page.live) {
            page.shelves.clear();
            page.bottom = 0;
        }
    }

    std::size_t AtlasPacker::live() const {
        std::size_t count{};
        for (const auto& page : pages_) {
            count += page.live;
        }
        return count;
    }
} // namespace weft::render
