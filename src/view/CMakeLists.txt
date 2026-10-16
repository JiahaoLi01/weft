# The public View (docs/engine-communication.md section 8): one page, its VM and its scene, for hosts.
weft_add_library(view
    SOURCES view.cpp view_images.cpp
    PUBLIC weft::script weft::scene weft::bridge
    PRIVATE weft_lodepng)
