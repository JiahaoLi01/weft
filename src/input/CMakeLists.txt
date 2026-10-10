# Mouse routing, hit testing and DOM event dispatch.
weft_add_library(input
    SOURCES mouse_router.cpp
    PUBLIC weft::layout weft::script weft::render)
