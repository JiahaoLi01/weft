# Text direction (SheenBidi): the bidirectional algorithm and script runs; style reads it for dir=auto.
weft_add_library(bidi SOURCES bidi.cpp PUBLIC weft::base PRIVATE weft_sheenbidi)
# FreeType faces, HarfBuzz shaping, font fallback and case mapping.
weft_add_library(text
    SOURCES case_mapping.cpp color_canvas.cpp font.cpp font_catalog.cpp mapped_file.cpp unicode_properties.cpp
    PUBLIC weft::base weft::bidi
    PRIVATE freetype harfbuzz weft_unicode_case)
