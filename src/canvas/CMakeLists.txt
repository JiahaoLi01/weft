# Canvas 2D context model shared by the script bindings and the reference raster.
weft_add_library(canvas SOURCES canvas.cpp PUBLIC weft::base)

if(WIN32)
    # Direct2D/WIC reference raster for Canvas 2D.
    weft_add_library(canvas_d2d
        SOURCES canvas_d2d.cpp
        PUBLIC weft::canvas
        PRIVATE d2d1 windowscodecs ole32)
endif()
