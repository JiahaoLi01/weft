# Inline formatting (HarfBuzz + libunibreak), the Yoga flex adapter and the page profiles.
# page, positioned_page and auto_page retire in refactoring step 8.
weft_add_library(layout
    SOURCES inline_layout.cpp flex.cpp page.cpp positioned_page.cpp auto_page.cpp document.cpp
    PUBLIC weft::text weft::style weft::render
    PRIVATE yogacore harfbuzz weft_unibreak)
