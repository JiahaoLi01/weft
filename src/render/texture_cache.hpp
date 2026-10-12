#pragma once

#include "render/atlas_packer.hpp"
#include "render/frame_packet.hpp"
#include "render/resource_lifetime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

// The texture cache policy that every renderer shares; the textures themselves are each backend's. Stable identities
// of live shared assets, an LRU index of immutable textures by (asset id, version) bounded in entries and bytes, and
// the glyph atlas placement of small coverage images. Each cached texture holds one CPU lease of its resource
// (ResourceLifetime); eviction releases it, and only for textures that no frame or use holds, so a texture retires once
// the last use of it completed.
namespace weft::render {
    // Stable identities of live shared assets, the cache keys of display list frames: an asset keeps its identity while
    // it lives, a reused address never aliases an expired asset, and identities only grow, also across clear().
    class AssetIdentities {
    public:
        // Throws std::runtime_error past the bound of live identities.
        std::uint64_t identify(const std::shared_ptr<const Asset>& source);

        std::size_t size() const noexcept {
            return entries_.size();
        }

        void clear() noexcept {
            entries_.clear();
        }

    private:
        std::map<const Asset*, std::pair<std::weak_ptr<const Asset>, std::uint64_t>> entries_;
        std::uint64_t next_{1};
    };

    // Whether a renderer that caches puts the asset on the glyph atlas: coverage images of at most atlas_max_image
    // pixels a side.
    bool atlas_candidate(const Asset& asset);
    // The asset's image as an atlas region holds it, inside a border that repeats its nearest edge texels, so that
    // filtering at the image's edges reads what a clamped texture of its own would.
    std::vector<std::uint8_t> bordered(const Asset& asset);
    // The normalized part of its page that a region's image covers, without the border.
    std::array<float, 4> region_uv(const AtlasPacker::Region& region);

    // An LRU index of immutable textures. Native is the backend's texture and has a ResourceId id.
    template <class Native>
    class TextureCache {
    public:
        using Key = std::pair<std::uint64_t, std::uint64_t>;

        struct Entry {
            Native native;
            std::size_t bytes{};
            std::uint64_t touched{};
        };

        TextureCache(std::size_t resources, std::size_t byte_bound)
            : resource_limit(resources), byte_limit(byte_bound) {
        }

        // The entry of key, now the most recently used, or nullptr. A found entry counts as a hit.
        Entry* use(const Key& key) {
            const auto found = entries.find(key);
            if (found == entries.end()) {
                return nullptr;
            }
            found->second.touched = ++clock_;
            ++hits;
            return &found->second;
        }

        // Adds the texture just created for key, which holds the resource's creation lease, as the most recently used.
        Entry& add(const Key& key, Native native, std::size_t size) {
            auto& entry = entries.emplace(key, Entry{std::move(native), size, ++clock_}).first->second;
            bytes += size;
            peak_bytes = std::max(peak_bytes, bytes);
            return entry;
        }

        // Releases the leases of the least recently used textures that no frame or use holds (one CPU lease, no pins)
        // until one more texture and `required` more bytes fit, or of every texture with `all`. evicted(entry) runs
        // before each entry goes. Throws std::runtime_error when the cache cannot make room.
        template <class Evicted>
        void evict(ResourceLifetime& tracker, std::size_t required, bool all, Evicted&& evicted) {
            if (required > byte_limit) {
                throw std::runtime_error("GPU cache resource byte bound");
            }
            while (!entries.empty() && (all || entries.size() >= resource_limit || required > byte_limit - bytes)) {
                auto victim = entries.end();
                for (auto i = entries.begin(); i != entries.end(); ++i) {
                    ResourceInfo state;
                    if (tracker.InspectResource(i->second.native.id, state) != LifetimeResult::Ok) {
                        throw std::runtime_error("inspect cache eviction");
                    }
                    if (state.cpu_leases == 1 && !state.prepared_pins && !state.encoded_pins && !state.submitted_pins &&
                        (victim == entries.end() || i->second.touched < victim->second.touched)) {
                        victim = i;
                    }
                }
                if (victim == entries.end()) {
                    throw std::runtime_error("bounded cache cannot evict resources in current full packet");
                }
                if (tracker.ReleaseResource(victim->second.native.id) != LifetimeResult::Ok) {
                    throw std::runtime_error("release completed GPU cache lease");
                }
                evicted(victim->second);
                bytes -= victim->second.bytes;
                entries.erase(victim);
            }
        }

        std::map<Key, Entry> entries;
        // Bytes the textures hold, their peak, and hits of use().
        std::size_t bytes{};
        std::size_t peak_bytes{};
        std::size_t hits{};
        std::size_t resource_limit;
        std::size_t byte_limit;

    private:
        std::uint64_t clock_{};
    };
} // namespace weft::render
