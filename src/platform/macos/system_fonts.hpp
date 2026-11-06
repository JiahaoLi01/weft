#pragma once

#include "text/font_catalog.hpp"

// macOS font discovery for the portable font catalog, which iOS uses as it is (the same CoreText calls; its generic
// families and fallbacks are macOS Chrome's, not checked against a browser on iOS). CoreText is used only to list the
// faces (file, weight, style, variation axes) and the system's fallback order; FreeType finds a face's index inside a
// collection (.ttc) by its PostScript name and reads the family names CSS also matches. Shaping, metrics and
// rasterization stay in HarfBuzz/FreeType.
namespace weft::platform::macos {
    // The family name system-ui maps to: CoreText's hidden UI family (".AppleSystemUIFont", San Francisco).
    inline constexpr const char* system_ui_family = ".AppleSystemUIFont";

    // Every face CoreText makes available (and the generic families' targets, which its collection leaves out), under
    // its family name and the family names of its name table, at the CSS weight nearest its CoreText weight trait (the
    // NSFontWeight values); the system UI font at the nine weights, upright and italic, under system_ui_family,
    // "-apple-system" and "BlinkMacSystemFont" (Chrome's aliases); and the hidden families of the UI font's cascade
    // list that have a readable file, regular and bold. Generic families follow Chrome on macOS: serif "Times",
    // sans-serif "Helvetica", monospace "Courier", system-ui system_ui_family; the fallback families are the UI family
    // then its cascade list in CoreText's order, a hidden CJK family as the installed family it is the UI cut of
    // (PingFang SC, Hiragino Sans, ...). A face FreeType cannot read, whose collection index cannot be found, or whose
    // file is larger than constants::text::max_font_file_bytes is left out. Listed once per process (later installs
    // are not seen). Hosts that follow another policy overwrite generic_families and fallback_families.
    FontCatalog system_font_catalog();
} // namespace weft::platform::macos
