#pragma once

#include "text/font_catalog.hpp"

#include <string>

// Windows font discovery for the portable font catalog. DirectWrite is used only to list installed faces (file,
// collection index, named-instance axis values); shaping, metrics and rasterization stay in HarfBuzz/FreeType.
namespace weft::platform::win32 {
    // The Windows UI font family (the message font of NONCLIENTMETRICS), e.g. "Segoe UI" or "Microsoft YaHei UI".
    std::string system_ui_family();

    // Every installed, non-simulated face of the system font collection under each of its localized family
    // names. Generic families default to Chrome's en-US choices on Windows (Times New Roman, Arial, Consolas),
    // system-ui to system_ui_family() (ui-* keywords stay unsupported, as in Chrome on Windows); fallback families are the UI family, Segoe UI Emoji and Segoe UI Symbol.
    // Hosts that follow another policy overwrite generic_families and fallback_families.
    FontCatalog system_font_catalog();
} // namespace weft::platform::win32
