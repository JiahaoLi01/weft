# Vector paths as coverage: fills and strokes rasterized with FreeType's antialiasing rasterizer and stroker.
weft_add_library(vector
    SOURCES path_raster.cpp
    PUBLIC weft::paint
    PRIVATE freetype)
