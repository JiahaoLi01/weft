# Inline formatting (HarfBuzz + libunibreak), the Yoga flex adapter and the document formatter.
weft_add_library(layout
    SOURCES inline_layout.cpp flex.cpp document.cpp
    PUBLIC weft::text weft::style weft::render
    PRIVATE yogacore harfbuzz weft_unibreak)
