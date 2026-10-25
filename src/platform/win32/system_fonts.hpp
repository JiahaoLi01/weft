#pragma once

#include "text/font_catalog.hpp"

#include <string>

// Windows font discovery for the portable font catalog. DirectWrite is used only to list installed faces (file,
// collection index, named-instance axis values); shaping, metrics and rasterization stay in HarfBuzz/FreeType.
namespace weft::platform::win32 {
    // The Windows UI font family (the message font of NONCLIENTMETRICS), e.g. "Segoe UI" or "Microsoft YaHei UI".
    std::string system_ui_family();

    // How system_font_catalog lists the installed faces. Both ways list the same entries in the same order.
    enum class SystemFontListing {
        // Font sets where the system has them, else font faces.
        Automatic,
        // From each family's font set, whose references, properties and axis values come from the system font
        // cache (Windows 10 1903 and later): some 5 ms for 1,400 fonts.
        FontSets,
        // From a font face made for each font, which opens and maps its file: some 0.3 ms a face, half a second for
        // 1,400 fonts. The only way before Windows 10 1903.
        FontFaces,
    };

    // Every installed, non-simulated face of the system font collection under each of its localized family
    // names. Generic families default to Chrome's en-US choices on Windows (Times New Roman, Arial, Consolas),
    // system-ui to system_ui_family() (ui-* keywords stay unsupported, as in Chrome on Windows); fallback families are the UI family, Segoe UI Emoji and Segoe UI Symbol.
    // Hosts that follow another policy overwrite generic_families and fallback_families.
    // Throws std::runtime_error("WEFT_SYSTEM_FONTS: ...") when DirectWrite fails, or for FontSets on a system without
    // them.
    FontCatalog system_font_catalog(SystemFontListing listing = SystemFontListing::Automatic);
} // namespace weft::platform::win32
