#pragma once

#include "paint/display_list.hpp"
#include "render/frame_packet.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

// Display lists of axis-aligned boxes and images (a document page's) as quads for the quad renderers and the C ABI
// v1: each item cropped to its clip, and drawn through a coverage mask prepared on the CPU wherever its rounded clips
// cut it.
namespace weft::render {
    // Rounded-clip masks kept across packets. A bound that would be exceeded clears them.
    struct QuadMasks {
        struct Key {
            std::shared_ptr<const Asset> input;
            std::vector<float> coordinates;

            bool operator<(const Key& other) const;
        };

        std::map<Key, std::shared_ptr<const Asset>> masks;
        std::size_t bytes{};
        std::size_t hits{};
        std::size_t misses{};
    };

    // The list's boxes and images as quads in item order. Boxes must be plain rectangles and every item a translated
    // axis-aligned rectangle. The packet takes the list's assets in order of first use, then each mask as it is
    // prepared; persistent, when given, keeps masks across calls.
    FramePacket lower_to_quads(const paint::DisplayList& list, QuadMasks* persistent = nullptr);
} // namespace weft::render
