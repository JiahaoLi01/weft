#pragma once

#include "gallery_effects.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_scene_session.hpp"

#include <memory>

// How each gallery scene runs (probes/gallery_scene_manifest.hpp): the CSS_TESTS and D3_TESTS cards with the scene
// utilities, the cluster groups' cases with theirs, and the CRPG UI's nested tooltip page as a whole page.
namespace probes {
    // The page (tools/build_tooltip_page.mjs) with its own stylesheet, html as the scene's root so the tooltips it
    // portals to body show, body over the gallery's backdrop as its Chrome reference has it, and the UI shader
    // effects it names.
    inline GallerySceneOptions gallery_page_options() {
        return {"tooltip-utilities.css", "TOOLTIP_TESTS.js", "html",
                "document.body.setAttribute('style','margin:0;position:relative;overflow:hidden;"
                "background-color:#050403')",
                &gallery_effects()};
    }

    inline GallerySceneOptions gallery_scene_options(unsigned scene) {
        if (scene == unsigned(weft::probe_constants::gallery::tooltip_page_scene)) {
            return gallery_page_options();
        }
        return {scene < unsigned(weft::probe_constants::gallery::core_scene_count) ? "scene-utilities.css"
                                                                                   : "cluster-utilities.css"};
    }

    inline std::unique_ptr<GallerySceneSession> gallery_scene_session(unsigned scene) {
        const auto d = gallery_scene_descriptors[scene];
        return std::make_unique<GallerySceneSession>(d.group, d.index, d.width, d.height, gallery_scene_options(scene));
    }
} // namespace probes
