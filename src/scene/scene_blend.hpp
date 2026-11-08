#pragma once

#include "paint/display_list.hpp"
#include "scene/scene.hpp"

// CSS blending of a scene element (Compositing and Blending 1): its mix-blend-mode, and the stacking contexts that
// isolate it. An element that blends composites its group over what its nearest ancestor stacking context painted
// before it (that ancestor's isolated group), or over what the pass's target holds outside every one.
namespace weft {
    // The element's mix-blend-mode (Normal for normal or none set).
    paint::BlendMode scene_blend_mode(const StyleValues& style);

    // Whether the element (a child of parent, none for the root) establishes a stacking context as Chrome takes one:
    // fixed or sticky position; a z-index other than auto on a positioned element or a flex or grid item; opacity
    // below 1; a transform, translate, rotate or scale, a filter, a backdrop-filter, a clip-path or a mask; a blend
    // mode; isolation: isolate; will-change naming a property that would.
    bool scene_stacking_context(const SceneNode& element, const SceneNode* parent);

    // Whether a stacking context's group must be isolated for blending: a descendant it holds directly (not inside a
    // nested stacking context) blends, or a stacking context among them blends.
    bool scene_isolates_blending(const SceneNode& element);
} // namespace weft
