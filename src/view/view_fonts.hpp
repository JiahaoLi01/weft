#pragma once

#include "style/style.hpp"
#include "text/font_catalog.hpp"
#include "view/view_images.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The fonts a page defines (docs/compatibility.md, "@font-face"): its stylesheet's @font-face rules (CSS Fonts 4 §4),
// each face's file read by its path through the host's reader, resolved against the page's base as its images are.
namespace weft {
    // The faces of a page's @font-face rules, in order. Of a rule's sources the first that loads gives its face (CSS
    // Fonts 4 §4.3): a url() whose format() hints name a format weft reads (truetype, opentype and their -variations),
    // or one without hints whose file starts as a TrueType or OpenType font (a collection's first face); woff and woff2
    // are not read and fall to the next source, as a browser passes over a format it lacks, and so does local() (no
    // installed font is looked up by its full name). A rule none of whose sources loads is a diagnostic
    // ("WEFT_FONT_FACE: <family>: <each source's reason>") and defines no face; so is an unicode-range that does not
    // parse (the face then covers every code point).
    std::vector<FontFaceEntry> view_font_faces(const std::vector<FontFaceRule>& rules, std::string_view base,
                                               const ViewResourceReader& reader,
                                               const std::function<void(std::string)>& diagnose);

    // The catalog of a page that defines faces: the runtime's, without its faces of a family the page defines (a
    // page's family hides an installed one of the same name, ASCII case-insensitively), and the page's faces.
    FontCatalog view_page_catalog(const FontCatalog& runtime, std::vector<FontFaceEntry> page);
} // namespace weft
