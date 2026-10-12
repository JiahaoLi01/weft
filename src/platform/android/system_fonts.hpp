#pragma once

#include "text/font_catalog.hpp"

// Android font discovery for the portable font catalog. The NDK's system font iterator (API level 29) lists the
// installed faces (file, collection index, weight, style, variation axes); FreeType reads their family names, which the
// iterator does not report. Shaping, metrics and rasterization stay in HarfBuzz/FreeType.
namespace weft::platform::android {
    // Every system face under each of its family names. Generic families follow Chrome on Android: serif "Noto Serif",
    // sans-serif and system-ui "Roboto", monospace "Droid Sans Mono". The fallback families are the other families in
    // the iterator's order, which lists the default faces before the locale fallbacks (CJK, emoji). A face whose file
    // FreeType cannot read is left out. Hosts that follow another policy overwrite generic_families and
    // fallback_families.
    FontCatalog system_font_catalog();
} // namespace weft::platform::android
