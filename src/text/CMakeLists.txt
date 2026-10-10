# FreeType faces, HarfBuzz shaping and font fallback.
weft_add_library(text
    SOURCES font.cpp font_catalog.cpp
    PUBLIC weft::base
    PRIVATE freetype harfbuzz)
