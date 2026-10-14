#include "render/atlas_packer.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace weft::render {
    AtlasPacker::AtlasPacker(std::uint32_t page_size, std::uint32_t max_pages)
        : page_size_(page_size), max_pages_(max_pages) {
    }

    std::optional<AtlasPacker::Region> AtlasPacker::take(std::uint32_t index, Shelf& shelf, std::uint32_t width,
                                                         std::uint32_t height) {
        const auto span = std::find_if(shelf.free.begin(), shelf.free.end(), [&](const Span& s) {
            return s.width >= width;
        });
        if (span == shelf.free.end()) {
            return std::nullopt;
        }
        const Region region{index, span->x, shelf.y, width, height};
        span->x += width;
        span->width -= width;
        if (!span->width) {
            shelf.free.erase(span);
        }
        ++shelf.live;
        ++pages_[index].live;
        return region;
    }

    std::optional<AtlasPacker::Region> AtlasPacker::place(std::uint32_t index, std::uint32_t width,
                                                          std::uint32_t height) {
        auto& page = pages_[index];
        const auto room = [&](const Shelf& shelf) {
            return shelf.height >= height && std::any_of(shelf.free.begin(), shelf.free.end(), [&](const Span& s) {
                       return s.width >= width;
                   });
        };
        // The shortest shelf in use that is tall enough and has the width free, unless it is half as tall again: then
        // an empty shelf split to the height, or a new one, wastes less.
        Shelf* used{};
        Shelf* empty{};
        for (auto& shelf : page.shelves) {
            if (!room(shelf)) {
                continue;
            }
            auto& best = shelf.live ? used : empty;
            if (!best || shelf.height < best->height) {
                best = &shelf;
            }
        }
        if (used && used->height <= height + height / 2) {
            return take(index, *used, width, height);
        }
        if (empty) {
            // The empty shelf's top rows become a shelf of the height; the rest stays an empty shelf below it.
            const auto at = std::size_t(empty - page.shelves.data());
            if (empty->height > height) {
                Shelf rest{empty->y + height, empty->height - height, {{0, page_size_}}, 0};
                empty->height = height;
                page.shelves.insert(page.shelves.begin() + std::ptrdiff_t(at) + 1, std::move(rest));
            }
            return take(index, page.shelves[at], width, height);
        }
        if (page_size_ - page.bottom >= height) {
            page.shelves.push_back(Shelf{page.bottom, height, {{0, page_size_}}, 0});
            page.bottom += height;
            return take(index, page.shelves.back(), width, height);
        }
        if (used) {
            return take(index, *used, width, height);
        }
        return std::nullopt;
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
        auto& shelves = page.shelves;
        const auto found =
            std::lower_bound(shelves.begin(), shelves.end(), region.y, [](const Shelf& s, std::uint32_t y) {
                return s.y < y;
            });
        if (found == shelves.end() || found->y != region.y || !found->live || region.height > found->height ||
            region.x > page_size_ || region.width > page_size_ - region.x) {
            throw std::logic_error("atlas region released twice");
        }
        // The span goes back between its free neighbours, joining those it touches.
        auto& free = found->free;
        const auto next = std::lower_bound(free.begin(), free.end(), region.x, [](const Span& s, std::uint32_t x) {
            return s.x < x;
        });
        const auto end = region.x + region.width;
        if ((next != free.end() && next->x < end) ||
            (next != free.begin() && std::prev(next)->x + std::prev(next)->width > region.x)) {
            throw std::logic_error("atlas region released twice");
        }
        auto at = free.insert(next, Span{region.x, region.width});
        if (std::next(at) != free.end() && std::next(at)->x == end) {
            at->width += std::next(at)->width;
            free.erase(std::next(at));
        }
        if (at != free.begin() && std::prev(at)->x + std::prev(at)->width == at->x) {
            std::prev(at)->width += at->width;
            free.erase(at);
        }
        --found->live;
        if (!--page.live) {
            page.shelves.clear();
            page.bottom = 0;
            return;
        }
        if (found->live) {
            return;
        }
        // An emptied shelf joins the empty shelves beside it; empty shelves at the end give their rows back.
        auto i = std::size_t(found - shelves.begin());
        if (i + 1 < shelves.size() && !shelves[i + 1].live) {
            shelves[i].height += shelves[i + 1].height;
            shelves.erase(shelves.begin() + std::ptrdiff_t(i) + 1);
        }
        if (i > 0 && !shelves[i - 1].live) {
            shelves[i - 1].height += shelves[i].height;
            shelves.erase(shelves.begin() + std::ptrdiff_t(i));
            --i;
        }
        shelves[i].free = {{0, page_size_}};
        if (i + 1 == shelves.size()) {
            page.bottom = shelves[i].y;
            shelves.pop_back();
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
