#pragma once

#include "paint/display_list.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace weft {
    // A scene frame as a display list, or why it has none.
    struct SceneDisplayList {
        paint::DisplayList list;
        // Empty unless the frame needs what the list cannot express yet (SVG, shadows, rounded or transformed clips,
        // projective transforms, unsupported values): the list is then empty, and the whole frame is drawn another
        // way, so no partly accepted list changes stacking.
        std::string fallback;
    };

    // The frame's boxes, text and opacity groups in paint order. layers are the nodes drawn as cached layer images.
    SceneDisplayList scene_display_list(const SceneFrame& frame, const std::vector<NodeIdentity>& layers = {});
} // namespace weft
