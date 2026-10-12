#include "render/texture_cache.hpp"

#include "constants/render.hpp"

#include <limits>

namespace weft::render {
    namespace {
        namespace limits = weft::constants::render::texture_cache;
    } // namespace

    std::uint64_t AssetIdentities::identify(const std::shared_ptr<const Asset>& source) {
        if (const auto i = entries_.find(source.get()); i != entries_.end()) {
            if (auto previous = i->second.first.lock(); previous && previous == source) {
                return i->second.second;
            }
            entries_.erase(i); // Address reuse cannot alias an old version.
        }
        // Weak metadata retains no CPU bitmap and keeps all live identities stable.
        if (entries_.size() >= std::size_t(limits::identity_prune_threshold)) {
            std::erase_if(entries_, [](const auto& entry) {
                return entry.second.first.expired();
            });
        }
        if (entries_.size() >= std::size_t(limits::max_identities)) {
            throw std::runtime_error("live asset identity metadata bound");
        }
        if (next_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("asset identity generation bound");
        }
        const auto id = next_++;
        entries_.emplace(source.get(), std::pair{std::weak_ptr<const Asset>(source), id});
        return id;
    }

    bool atlas_candidate(const Asset& asset) {
        return asset.encoding == Encoding::Coverage && asset.width <= unsigned(limits::atlas_max_image) &&
               asset.height <= unsigned(limits::atlas_max_image);
    }

    std::vector<std::uint8_t> bordered(const Asset& a) {
        constexpr auto border = std::uint32_t(limits::atlas_border);
        const auto w = a.width + 2 * border;
        const auto h = a.height + 2 * border;
        std::vector<std::uint8_t> out(std::size_t(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto sy = std::min(std::max(y, border) - border, a.height - 1);
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto sx = std::min(std::max(x, border) - border, a.width - 1);
                out[std::size_t(y) * w + x] = a.bytes[std::size_t(sy) * a.width + sx];
            }
        }
        return out;
    }

    std::array<float, 4> region_uv(const AtlasPacker::Region& r) {
        constexpr auto border = float(limits::atlas_border);
        constexpr auto size = float(limits::atlas_page_size);
        return {(r.x + border) / size, (r.y + border) / size, (r.width - 2 * border) / size,
                (r.height - 2 * border) / size};
    }
} // namespace weft::render
