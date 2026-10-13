# The portable paint IR: display lists, their structural checks and the particle model.
weft_add_library(paint
    SOURCES display_list.cpp particles.cpp
    PUBLIC weft::base)
