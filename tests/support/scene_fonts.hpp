#pragma once

#include "platform/win32/system_fonts.hpp"
#include "scene/scene_text.hpp"

// Font policy of the scene tests and probes on Windows: the installed fonts, with the generic families and the
// last-resort fallback order that Chrome 155 used on the machine that captured the scene references. The policy is
// not an assumption: tools/compare_scene_text.py checks every face choice against
// tests/fixtures/scene_text_reference.json.
namespace weft::testing {
    inline FontCatalog chrome_windows_font_catalog() {
        auto catalog = platform::win32::system_font_catalog();
        catalog.generic_families = {{"serif", "Noto Serif SC"},
                                    {"sans-serif", "Noto Sans SC"},
                                    {"monospace", "NSimSun"},
                                    {"system-ui", platform::win32::system_ui_family()}};
        catalog.fallback_families = {"Noto Sans SC", "Segoe UI Emoji", "Segoe UI Symbol"};
        return catalog;
    }

    // One scene text per owner thread: every scene, raster and probe on a thread shares its faces and caches.
    inline SceneText& thread_scene_text() {
        thread_local SceneText text(chrome_windows_font_catalog());
        return text;
    }
} // namespace weft::testing
